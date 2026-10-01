// ============================================================================
// AURA DAW - plugin/sandbox/SharedMemory.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "SharedMemory.hpp"

#include <atomic>
#include <cstdint> // std::uint64_t is used below and is pulled in transitively by some
#include <cstdio>  // libstdc++ versions - which is exactly why it is spelled out here.
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aura::plugin::sandbox {
namespace {

/// POSIX shared memory names are not filesystem paths: they start with a slash and
/// may contain no other one. Windows names must avoid backslashes. One sanitiser for
/// both keeps the two implementations from drifting.
std::string sanitiseName(std::string_view name) {
    std::string cleaned;
    cleaned.reserve(name.size() + 1);
    for (const char character : name) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '-' ||
                             character == '_' || character == '.';
        cleaned.push_back(allowed ? character : '_');
    }
    if (cleaned.empty())
        cleaned = "aura-region";
    return cleaned;
}

/// The name the operating system actually sees. The name the *caller* holds stays the
/// plain one it passed in, because that is what travels to the helper on its command
/// line and what shows up in a log; decorating it here is the only place that knows
/// about `Local\` and leading slashes, so the two can never drift apart.
std::string platformName(std::string_view name) {
#if defined(_WIN32)
    return "Local\\" + sanitiseName(name);
#else
    return "/" + sanitiseName(name);
#endif
}

} // namespace

std::string makeRegionName(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
#if defined(_WIN32)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return std::string(prefix) + "-" + std::to_string(pid) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

SharedRegion::~SharedRegion() { close(); }

SharedRegion::SharedRegion(SharedRegion&& other) noexcept {
    *this = std::move(other);
}

SharedRegion& SharedRegion::operator=(SharedRegion&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        name_ = std::move(other.name_);
        error_ = std::move(other.error_);
#if defined(_WIN32)
        mapping_ = other.mapping_;
#else
        fd_ = other.fd_;
#endif
        owner_ = other.owner_;
        other.data_ = nullptr;
        other.size_ = 0;
#if defined(_WIN32)
        other.mapping_ = nullptr;
#else
        other.fd_ = -1;
#endif
        other.owner_ = false;
    }
    return *this;
}

void SharedRegion::release() noexcept {
    data_ = nullptr;
    size_ = 0;
#if defined(_WIN32)
    mapping_ = nullptr;
#else
    fd_ = -1;
#endif
    owner_ = false;
}

void SharedRegion::close() noexcept {
    if (data_ == nullptr)
        return;
#if defined(_WIN32)
    UnmapViewOfFile(data_);
    if (mapping_ != nullptr)
        CloseHandle(static_cast<HANDLE>(mapping_));
#else
    munmap(data_, size_);
    if (fd_ >= 0)
        ::close(fd_);
#endif
    data_ = nullptr;
    size_ = 0;
#if defined(_WIN32)
    mapping_ = nullptr;
#else
    fd_ = -1;
#endif
}

SharedRegion SharedRegion::create(std::string_view name, std::size_t bytes) {
    SharedRegion region;
    region.size_ = bytes;
#if defined(_WIN32)
    // `Local\` scope: visible to this session only and needs no privileges. A
    // name in the global namespace needs SeCreateGlobalPrivilege and would let
    // another user's session attach to our audio. A stale mapping from a crashed
    // run releases itself when its last handle closes, so there is nothing to clean
    // up first - unlike POSIX, where the name outlives the processes.
    const std::string fullName = platformName(name);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(bytes >> 32),
                                        static_cast<DWORD>(bytes & 0xFFFFFFFFu),
                                        std::wstring(fullName.begin(), fullName.end()).c_str());
    if (mapping == nullptr) {
        region.error_ = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")";
        return region;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (view == nullptr) {
        region.error_ = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return region;
    }
    region.data_ = view;
    region.mapping_ = mapping;
    region.name_ = std::string(name);
#else
    const std::string fullName = platformName(name);
    // A crashed host leaves its name behind (POSIX shared memory is not reference
    // counted the way a file mapping is), and creating over it would hand the new
    // session a region that a zombie helper might still be reading. Unlinking first
    // is the documented way to start clean; a live region with our name would mean
    // another AURA with the same pid, which cannot happen.
    ::shm_unlink(fullName.c_str());
    const int fd = ::shm_open(fullName.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        region.error_ = std::string("shm_open failed: ") + std::strerror(errno);
        return region;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        region.error_ = std::string("ftruncate failed: ") + std::strerror(errno);
        ::close(fd);
        ::shm_unlink(fullName.c_str());
        return region;
    }
    void* view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        region.error_ = std::string("mmap failed: ") + std::strerror(errno);
        ::close(fd);
        ::shm_unlink(fullName.c_str());
        return region;
    }
    region.data_ = view;
    region.fd_ = fd;
    region.name_ = std::string(name);
    region.owner_ = true;
#endif
    return region;
}

SharedRegion SharedRegion::open(std::string_view name, std::size_t bytes, std::string& error) {
    SharedRegion region;
    region.size_ = bytes;
#if defined(_WIN32)
    const std::string fullName = platformName(name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                      std::wstring(fullName.begin(), fullName.end()).c_str());
    if (mapping == nullptr) {
        error = "the sandbox's shared memory is not there yet (OpenFileMapping: " +
                std::to_string(GetLastError()) + ")";
        return region;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (view == nullptr) {
        error = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return region;
    }
    region.data_ = view;
    region.mapping_ = mapping;
    region.name_ = std::string(name);
#else
    const std::string fullName = platformName(name);
    const int fd = ::shm_open(fullName.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        error = std::string("the sandbox's shared memory is not there yet (shm_open: ") +
                std::strerror(errno) + ")";
        return region;
    }
    void* view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        error = std::string("mmap failed: ") + std::strerror(errno);
        ::close(fd);
        return region;
    }
    region.data_ = view;
    region.fd_ = fd;
    region.name_ = std::string(name);
#endif
    return region;
}

void SharedRegion::unlink() noexcept {
    if (name_.empty())
        return;
    const std::string fullName = platformName(name_);
#if defined(_WIN32)
    // A file mapping has no name to remove: it lives exactly as long as the last
    // handle, which is why the host is the one that creates it and the helper only
    // ever attaches.
#else
    ::shm_unlink(fullName.c_str());
#endif
}

} // namespace aura::plugin::sandbox
