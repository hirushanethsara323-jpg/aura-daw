// ============================================================================
// AURA DAW - src/plugin/sandbox/SharedMemory.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The platform half of SharedMemory. Everything here is a blocking system call,
// which is why it is confined to this file and why the header's thread contract
// says control thread only.
//
// Errors come back as values with the platform's own numbers in the detail
// string. Nothing is logged here: the caller (SandboxSession) decides whether a
// failed mapping is worth the user's attention, and logging as well as returning
// would report one failure twice with two different wordings.
// ============================================================================

#include "aura/plugin/sandbox/SharedMemory.hpp"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace aura::plugin::sandbox {
namespace {

/// `aura-` is the namespace guard; see platformShmName().
constexpr const char* kNamePrefix = "aura-";

std::string describeErrno(int value) {
#if defined(_WIN32)
    return "error " + std::to_string(value);
#else
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : ("errno " + std::to_string(value));
#endif
}

#if defined(_WIN32)
/// Safe here rather than merely convenient: isUsableShmName() restricts a mapping
/// name to ASCII, so widening byte-by-byte cannot lose anything. (An executable
/// *path* is a different matter - a Windows user name can be non-ASCII - and
/// ProcessSupervisor uses MultiByteToWideChar for exactly that reason.)
std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::string describeLast(DWORD code) { return "Win32 error " + std::to_string(code); }
#endif

} // namespace

bool isUsableShmName(const std::string& name) noexcept {
    if (name.empty() || name.size() > kMaxShmNameLength)
        return false;
    if (name == "." || name == "..")
        return false;
    for (const char character : name) {
        const bool ok = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                        character == '-';
        if (!ok)
            return false;
    }
    return true;
}

std::string platformShmName(const std::string& name) {
    if (!isUsableShmName(name))
        return {};
#if defined(_WIN32)
    return std::string("Local\\") + kNamePrefix + name;
#else
    return std::string("/") + kNamePrefix + name;
#endif
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : data_(other.data_), size_(other.size_), name_(std::move(other.name_)),
      platformName_(std::move(other.platformName_)), owner_(other.owner_),
      descriptor_(other.descriptor_), handle_(other.handle_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.owner_ = false;
    other.descriptor_ = -1;
    other.handle_ = nullptr;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        size_ = other.size_;
        name_ = std::move(other.name_);
        platformName_ = std::move(other.platformName_);
        owner_ = other.owner_;
        descriptor_ = other.descriptor_;
        handle_ = other.handle_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.owner_ = false;
        other.descriptor_ = -1;
        other.handle_ = nullptr;
    }
    return *this;
}

SharedMemory::~SharedMemory() { release(); }

void SharedMemory::release() noexcept {
#if defined(_WIN32)
    if (data_ != nullptr)
        ::UnmapViewOfFile(data_);
    if (handle_ != nullptr)
        ::CloseHandle(handle_);
#else
    if (data_ != nullptr)
        ::munmap(data_, size_);
    // Only the creator unlinks. An opener that unlinked would remove the object
    // from the namespace while the host was still using it, and a helper that
    // crashed on the way out would take the session's buffers with it.
    if (owner_ && !platformName_.empty())
        ::shm_unlink(platformName_.c_str());
    if (descriptor_ >= 0)
        ::close(descriptor_);
#endif
    data_ = nullptr;
    size_ = 0;
    owner_ = false;
    descriptor_ = -1;
    handle_ = nullptr;
    name_.clear();
    platformName_.clear();
}

// ---------------------------------------------------------------------------
// create / open
// ---------------------------------------------------------------------------

AuraResult<SharedMemory> SharedMemory::create(const std::string& name, std::size_t bytes) {
    if (!isUsableShmName(name))
        return failureT<SharedMemory>(makeError(
            ErrorCode::InvalidArgument, "a shared-memory name must be 1-64 characters of "
                                        "[A-Za-z0-9._-] and must not be \".\" or \"..\"",
            "refused '" + name + "': a mapping name is a machine-wide namespace, so a "
            "separator or a traversal in it is not something to sanitise around"));
    if (bytes == 0)
        return failureT<SharedMemory>(
            makeError(ErrorCode::InvalidArgument, "a shared mapping cannot be zero bytes"));

    const std::string platform = platformShmName(name);

#if defined(_WIN32)
    const std::wstring wide = widen(platform);
    // The size is split because CreateFileMapping takes it as two DWORDs. An arena
    // is kilobytes, so the high word is always zero in practice; splitting it
    // anyway costs nothing and keeps the call correct if a future arena is not.
    const DWORD high = static_cast<DWORD>((static_cast<unsigned long long>(bytes) >> 32) & 0xFFFFFFFFull);
    const DWORD low = static_cast<DWORD>(static_cast<unsigned long long>(bytes) & 0xFFFFFFFFull);
    HANDLE section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low,
                                          wide.c_str());
    if (section == nullptr)
        return failureT<SharedMemory>(makeError(
            ErrorCode::IoError, "could not create the shared section",
            platform + ": " + describeLast(::GetLastError())));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        // Someone else owns this name. Reusing it would mean attaching to counters
        // and payload from a session that is not this one.
        ::CloseHandle(section);
        return failureT<SharedMemory>(makeError(
            ErrorCode::AlreadyExists, "a shared mapping of that name already exists",
            platform + ": left over from a previous session, or another process owns it"));
    }

    void* mapped = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (mapped == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(section);
        return failureT<SharedMemory>(makeError(ErrorCode::IoError,
                                                "could not map the shared section into this process",
                                                platform + ": " + describeLast(code)));
    }
    // The section handle is kept for the object's lifetime: on Windows the mapping
    // is destroyed when its last handle closes, so dropping it here would leave the
    // helper opening a name that no longer exists.
    return SharedMemory(mapped, bytes, name, platform, true, -1, section);
#else
    // O_EXCL so a leftover object from a crashed session is reported rather than
    // adopted, and O_CLOEXEC so the descriptor cannot leak into a child spawned
    // between here and the close below.
    const int descriptor =
        ::shm_open(platform.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        const int code = errno;
        if (code == EEXIST)
            return failureT<SharedMemory>(makeError(
                ErrorCode::AlreadyExists, "a shared mapping of that name already exists",
                platform + ": left over from a previous session, or another process owns it"));
        if (code == EACCES || code == EPERM)
            return failureT<SharedMemory>(makeError(
                ErrorCode::PermissionDenied, "not permitted to create a shared mapping",
                platform + ": " + describeErrno(code)));
        return failureT<SharedMemory>(makeError(ErrorCode::IoError,
                                                "could not create a shared mapping",
                                                platform + ": " + describeErrno(code)));
    }

    if (::ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
        const int code = errno;
        ::close(descriptor);
        ::shm_unlink(platform.c_str());
        return failureT<SharedMemory>(makeError(
            ErrorCode::IoError, "could not size the shared mapping",
            platform + ": " + std::to_string(bytes) + " bytes requested, " + describeErrno(code)));
    }

    void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapped == MAP_FAILED) {
        const int code = errno;
        ::close(descriptor);
        ::shm_unlink(platform.c_str());
        return failureT<SharedMemory>(makeError(
            ErrorCode::IoError, "could not map the shared object into this process",
            platform + ": " + describeErrno(code)));
    }
    // The mapping outlives the descriptor, so the descriptor is not needed any
    // more. Closing it here rather than holding it keeps the object's lifetime
    // rule in one place: the mapping (and the unlink below) own it.
    ::close(descriptor);

    // A new POSIX shared object is zero-filled, and a new Windows section is too.
    // ArenaView::create() memsets regardless, because relying on the OS for the
    // initial contents of a structure two processes interpret is relying on a
    // documentation footnote; the arena owns its own initialisation.
    return SharedMemory(mapped, bytes, name, platform, true, -1, nullptr);
#endif
}

AuraResult<SharedMemory> SharedMemory::open(const std::string& name, std::size_t bytes) {
    if (!isUsableShmName(name))
        return failureT<SharedMemory>(makeError(
            ErrorCode::InvalidArgument, "a shared-memory name must be 1-64 characters of "
                                        "[A-Za-z0-9._-] and must not be \".\" or \"..\"",
            "refused '" + name + "'"));
    if (bytes == 0)
        return failureT<SharedMemory>(
            makeError(ErrorCode::InvalidArgument, "a shared mapping cannot be zero bytes"));

    const std::string platform = platformShmName(name);

#if defined(_WIN32)
    const std::wstring wide = widen(platform);
    // Open rather than create: OPEN_EXISTING means a helper never brings a mapping
    // into existence that the host did not ask for.
    HANDLE section = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
    if (section == nullptr) {
        const DWORD code = ::GetLastError();
        return failureT<SharedMemory>(makeError(
            code == ERROR_FILE_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError,
            "could not open the shared section", platform + ": " + describeLast(code)));
    }
    void* mapped = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (mapped == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(section);
        return failureT<SharedMemory>(makeError(
            ErrorCode::IoError, "could not map the shared section into this process",
            platform + ": " + describeLast(code) + " (" + std::to_string(bytes) + " bytes)"));
    }
    return SharedMemory(mapped, bytes, name, platform, false, -1, section);
#else
    const int descriptor = ::shm_open(platform.c_str(), O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        const int code = errno;
        return failureT<SharedMemory>(makeError(
            code == ENOENT ? ErrorCode::NotFound
                           : (code == EACCES || code == EPERM ? ErrorCode::PermissionDenied
                                                              : ErrorCode::IoError),
            "could not open the shared object",
            platform + ": " + describeErrno(code) +
                " (for a helper this usually means the host went away before it started)"));
    }

    void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapped == MAP_FAILED) {
        const int code = errno;
        ::close(descriptor);
        return failureT<SharedMemory>(makeError(
            ErrorCode::IoError, "could not map the shared object into this process",
            platform + ": " + describeErrno(code) + " (" + std::to_string(bytes) + " bytes)"));
    }
    ::close(descriptor);
    // owner_ is false: an opener unmaps but never unlinks.
    return SharedMemory(mapped, bytes, name, platform, false, -1, nullptr);
#endif
}

} // namespace aura::plugin::sandbox
