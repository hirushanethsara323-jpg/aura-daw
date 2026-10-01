// ============================================================================
// AURA DAW - plugin/sandbox/SharedMemory.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The platform half of the sandbox transport: a named region of memory that a
// second process can find, map and unmap.
//
// Deliberately small. Windows uses CreateFileMappingW/MapViewOfFile and a
// `Local\` name (session-local, which needs no privileges and cannot collide with
// another user's session); POSIX uses shm_open + mmap, with the name length and
// character set the platform allows handled in one place. Everything above this -
// the layout, the ordering rules - is in SandboxArena.hpp and is identical on both.
//
// Creating and mapping are control-thread operations, never audio-thread ones.
// ============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace aura::plugin::sandbox {

/// A mapped region of shared memory. Move-only: the handle owns the mapping, and
/// unmapping is the last thing that happens to it.
class SharedRegion {
public:
    SharedRegion() = default;
    ~SharedRegion();

    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    SharedRegion(SharedRegion&& other) noexcept;
    SharedRegion& operator=(SharedRegion&& other) noexcept;

    /// Creates a new region of `bytes` under `name`, replacing any stale region with
    /// the same name. Only the host creates; the helper attaches.
    ///
    /// `name` is the plain, caller-facing name - the same string `name()` returns and
    /// the same one that goes to the helper. Platform decoration (`Local\` on Windows,
    /// a leading slash on POSIX) happens inside and is never seen by a caller.
    [[nodiscard]] static SharedRegion create(std::string_view name, std::size_t bytes);

    /// Maps an existing region. Fails (with the reason in `error`) when the host is
    /// not there yet - which the helper retries rather than treating as fatal.
    [[nodiscard]] static SharedRegion open(std::string_view name, std::size_t bytes,
                                          std::string& error);

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] void* data() noexcept { return data_; }
    [[nodiscard]] const void* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Hands ownership of the mapping to the caller. The helper does this so the
    /// region stays mapped for the life of the process even if the object that
    /// opened it goes away - a sandbox must not unmap memory a plug-in still points
    /// at because a wrapper went out of scope.
    void release() noexcept;

    /// Removes the region (host side, after every helper has detached).
    void unlink() noexcept;

    /// Last failure from create()/open(), for the caller to log with context.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    void close() noexcept;

    void* data_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;
    std::string error_;
#if defined(_WIN32)
    void* mapping_ = nullptr; ///< HANDLE
    bool owner_ = false;      ///< creator: the mapping goes away when the last handle closes
#else
    int fd_ = -1;
    bool owner_ = false;
#endif
};

/// A process-unique region name. Two sandboxes, or two AURA instances, or a scan
/// running while a session plays, must not find each other's memory; a name with the
/// pid and a counter in it is the cheapest way to guarantee that.
[[nodiscard]] std::string makeRegionName(std::string_view prefix);

} // namespace aura::plugin::sandbox
