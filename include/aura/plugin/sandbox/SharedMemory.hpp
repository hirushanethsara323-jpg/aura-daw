// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/SharedMemory.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// A named block of memory two processes can map at the same time.
//
// SandboxArena decides what lives in the mapping and SharedAudioRing decides how
// it is crossed without waiting. This is the mapping itself: the only part of the
// sandbox that is an operating-system object, kept in one file so the layout and
// the rings stay testable on a plain heap buffer - which is exactly how the
// phase-1 tests exercise them - and so no platform ifdef leaks into either.
//
// Both sides reach the same memory BY NAME, not by an inherited handle. That is
// deliberate: a handle would have to survive CreateProcess/fork, would tie the
// helper's lifetime to a descriptor the host owns, and on Windows would need
// DuplicateHandle to mean anything in the child. A name needs none of that, and it
// is what makes "the host died, the helper noticed" a question about the arena's
// control word rather than about a broken pipe.
//
// Thread contract: control thread only. Creating, opening and releasing a mapping
// are blocking system calls. The audio thread only ever touches these bytes
// through an already-attached SharedAudioRing, which allocates nothing and takes
// no lock.
// ============================================================================
#pragma once

#include <cstddef>
#include <string>

#include "aura/core/Errors.hpp"

namespace aura::plugin::sandbox {

/// Longest accepted mapping name, in characters. The limit exists because the
/// name is embedded in a platform path (`/aura-<name>` on POSIX,
/// `Local\aura-<name>` on Windows) and both have their own ceilings; refusing a
/// long name with a clear error beats a truncated one that silently maps a
/// different object than the peer asked for.
inline constexpr std::size_t kMaxShmNameLength = 64;

/// True when `name` may be used as a mapping name.
///
/// The character set is deliberately narrow - letters, digits, `.`, `_`, `-` -
/// because a mapping name is a namespace shared with every other process on the
/// machine, and the name usually originates from a plug-in or session identifier,
/// which is data AURA does not control. A separator in it would let a caller reach
/// outside the AURA namespace (POSIX `shm_open` treats the whole string as a path
/// under `/dev/shm`), and `..` would let it walk. Refusing is cheaper than
/// sanitising: a rejected name is a programming error or an attack, and in neither
/// case is guessing what the caller meant the right answer.
[[nodiscard]] bool isUsableShmName(const std::string& name) noexcept;

/// The platform-specific name a mapping is registered under: `/aura-<name>` on
/// POSIX, `Local\aura-<name>` on Windows.
///
/// `Local\` and not `Global\` on Windows because a global object needs
/// SeCreateGlobalPrivilege - which an ordinary desktop DAW does not have, and
/// should not want. The helper runs in the user's session, so `Local\` reaches it.
/// The `aura-` prefix is a collision guard against other software on the machine
/// and makes the object identifiable in Process Explorer or `ls /dev/shm`.
///
/// Returns an empty string when `name` is not usable; callers check
/// isUsableShmName() rather than testing this for emptiness, so the reason is
/// reported and not inferred.
[[nodiscard]] std::string platformShmName(const std::string& name);

/// A shared mapping. Movable, not copyable - two owners of one mapping would
/// unmap it twice.
///
/// The creator unlinks the object when it lets go; an opener only unmaps. On POSIX
/// that asymmetry is literal - `shm_unlink` removes the *name* at once while an
/// opener keeps reading valid memory - and it is what stops the host's shutdown from
/// pulling the mapping out from under a helper that is still using it, and what stops
/// a helper that crashes from leaving a stale object behind.
///
/// **Windows has no unlink, and this class does not pretend otherwise.** A named
/// section object lives until its last handle closes, so a creator's `release()`
/// frees the name only when no peer still holds it; while a helper is alive the name
/// is taken and `create()` reports AlreadyExists. That is the correct answer - the
/// alternative is handing back a live peer's section, which is precisely what
/// `create()` exists to refuse - but it makes the two platforms differ in a way
/// callers have to design around: **a session's name must be unique to that
/// session.** A fixed name would collide on Windows whenever a previous helper is
/// still alive, which is exactly when a new session is most likely to be starting.
///
/// Found by CI's Windows jobs, not by reading the documentation: the POSIX rule was
/// written down here first, asserted by a test, and passed on Linux for a whole
/// increment before a Windows runner disagreed with it.
class SharedMemory {
public:
    SharedMemory() = default;
    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    ~SharedMemory();

    /// Creates a NEW mapping of `bytes`, or fails. An existing object of the same
    /// name is reported as AlreadyExists rather than reused: a mapping left over
    /// from a crashed session holds a header whose counters mean nothing to this
    /// one, and quietly attaching to it is how a session starts with audio from a
    /// dead process in its buffers.
    ///
    /// On Windows a *live* peer produces the same AlreadyExists, because the name is
    /// held until its last handle closes and there is no unlink to free it early.
    /// Making the name unique to the session is what keeps that case unreachable -
    /// see the class comment.
    ///
    /// Control thread only.
    [[nodiscard]] static AuraResult<SharedMemory> create(const std::string& name, std::size_t bytes);

    /// Opens a mapping the peer already created. NotFound when nothing of that
    /// name exists - which for a helper means the host went away between spawning
    /// it and now, and is a clean exit rather than an error to retry.
    ///
    /// Control thread only.
    [[nodiscard]] static AuraResult<SharedMemory> open(const std::string& name, std::size_t bytes);

    [[nodiscard]] bool isValid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] void* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /// The name as the caller gave it.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    /// The name as the platform sees it. Useful in a diagnostic, which is the only
    /// thing that should ever need it.
    [[nodiscard]] const std::string& platformName() const noexcept { return platformName_; }

    /// Unmaps (and, for a creator, unlinks) now rather than at destruction. After
    /// this the object is empty and every accessor reports that; touching `data()`
    /// of an invalid mapping is the caller's bug, not something this class can
    /// defend against, so it returns nullptr and says so in the contract above.
    void release() noexcept;

private:
    SharedMemory(void* data, std::size_t size, std::string name, std::string platformName,
                 bool owner, int descriptor, void* handle) noexcept
        : data_(data), size_(size), name_(std::move(name)),
          platformName_(std::move(platformName)), owner_(owner), descriptor_(descriptor),
          handle_(handle) {}

    void* data_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;
    std::string platformName_;
    bool owner_ = false;
    /// POSIX keeps no descriptor open after mapping, so this is -1 in practice; it
    /// exists because a failed create has to close the descriptor it got, and a
    /// single place to hold it keeps that path from leaking.
    int descriptor_ = -1;
    /// Windows keeps the section HANDLE for the object's lifetime: the mapping
    /// disappears when the last handle closes, so the creator must hold it until
    /// the helper has opened the object by name.
    void* handle_ = nullptr;
};

} // namespace aura::plugin::sandbox
