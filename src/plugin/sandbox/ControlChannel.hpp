// ============================================================================
// AURA DAW - plugin/sandbox/ControlChannel.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sandbox's second connection (M7 phase 2): a duplex byte channel between the
// host and one helper process.
//
// Why a second channel at all, when the arena already crosses the boundary? Because
// everything that is *not* audio has a different shape: variable-size (state blobs,
// parameter lists, error text), occasional, and allowed to block. Putting those in
// the shared arena would mean sizing them at prepare() time from a worst case nobody
// can bound; putting them on a channel that can block is honest, because none of it
// ever runs on the audio thread.
//
// The channel is deliberately dumb: length-prefixed frames of bytes, first one to
// arrive wins, no ordering logic. The *messages* that travel in the frames are JSON
// (core/Json.hpp), which gives the protocol three properties this project already
// relies on elsewhere: it is diffable when it goes wrong, the parser is already
// fuzzed (fuzz/), and a mismatch is a legible error instead of a misread structure.
//
// Frames are capped at kMaxFrameBytes so a hostile or broken peer cannot make the
// host allocate without bound. A frame that claims more than the cap is a protocol
// error; the connection is dropped, which the supervisor turns into "kill the helper
// and mark the plug-in faulty".
//
// Creating and using a channel is control-thread work. Nothing here is RT-safe and
// nothing here pretends to be.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "aura/core/Json.hpp"

namespace aura::plugin::sandbox {

/// The protocol version carried in every message envelope. Bumped when a message
/// shape changes; a peer with a different version is refused at the handshake, the
/// same way a mismatched arena version is refused in SandboxArena.hpp.
inline constexpr int kProtocolVersion = 1;

/// The largest frame either side will send or accept. 4 MiB is far above any state
/// blob a plug-in has any business producing and far below anything that would hurt
/// to allocate.
inline constexpr std::uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

/// What a receive attempt did. `Closed` is not an error: it is the peer exiting,
/// which is how the host learns a helper is gone.
enum class ChannelStatus {
    Frame,   ///< a frame arrived in `out`
    Timeout, ///< the deadline passed with nothing to read
    Closed,  ///< the peer closed its end
    Error    ///< the channel failed; `error` says how
};

/// One end of a duplex channel. Move-only: it owns its handles.
class ControlChannel {
public:
    ControlChannel() = default;
    ~ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;
    ControlChannel(ControlChannel&& other) noexcept;
    ControlChannel& operator=(ControlChannel&& other) noexcept;

    /// Host side: a channel plus the child's end, still unspawned. `childEnd()` is
    /// what the spawn function hands to the process, and `closeChildEnd()` must be
    /// called by the host once the child has started - otherwise the host keeps a
    /// writer to itself and a close by the helper never looks like a close.
    [[nodiscard]] static ControlChannel createPair(std::string& error);

    /// Helper side: adopts the end the host passed on the command line.
    /// `raw` is a POSIX file descriptor or a Windows handle value used for both
    /// directions (POSIX has one descriptor per side; Windows needs adoptHandles).
    [[nodiscard]] static ControlChannel adopt(std::uintptr_t raw, std::string& error);

    /// Helper side, the precise form: the read end and the write end. On POSIX a
    /// socket end is both directions in one descriptor, so `writeHandle` is ignored
    /// (and documented as such rather than silently required); on Windows the two
    /// anonymous pipes are genuinely separate handles.
    [[nodiscard]] static ControlChannel adoptHandles(std::uintptr_t readHandle,
                                                      std::uintptr_t writeHandle,
                                                      std::string& error);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uintptr_t handle() const noexcept; ///< fd on POSIX, HANDLE value on Windows
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// The *child's* end, for the spawn call to inherit (POSIX: the descriptor the
    /// helper is told about; Windows: the two pipe handles). Only meaningful on a
    /// channel from createPair(), before closeChildEnd().
    [[nodiscard]] std::uintptr_t childHandle() const noexcept;
    [[nodiscard]] std::uintptr_t childWriteHandle() const noexcept; ///< Windows only, 0 elsewhere
    /// Drops the host's copy of the child's end. After this the helper is the only
    /// holder, which is what makes "the helper exited" observable as a closed channel.
    void closeChildEnd() noexcept;

    /// Blocks until the whole frame is written. A peer that stops reading fills the
    /// pipe and this waits; the supervisor's watchdog is what bounds that, and it is
    /// the honest place for the bound - a control channel that silently dropped
    /// messages would turn a wedged helper into a corrupt one.
    bool sendFrame(std::string_view payload, std::string& error);

    /// Waits at most `timeoutSeconds` for a frame (negative or zero: forever).
    [[nodiscard]] ChannelStatus receiveFrame(std::string& payload, double timeoutSeconds,
                                             std::string& error);

    /// The JSON layer: an envelope is `{"protocol": 1, "kind": "...", ...}`. Sending
    /// rejects the message before it goes out; receiving rejects it after it arrives,
    /// with the reason, so a protocol mismatch reads as itself rather than as
    /// "the plug-in did something odd".
    bool sendMessage(const json::Value& message, std::string& error);
    [[nodiscard]] ChannelStatus receiveMessage(json::Value& message, double timeoutSeconds,
                                               std::string& error);

    /// Closes this end. Idempotent; the destructor does it too.
    void close() noexcept;

private:
#if defined(_WIN32)
    void* readHandle_ = nullptr;
    void* writeHandle_ = nullptr;
    void* childReadHandle_ = nullptr;  ///< host only: the child's ends, until spawn
    void* childWriteHandle_ = nullptr;
#else
    int fd_ = -1;
    int childFd_ = -1; ///< host only: the child's end, until spawn
#endif
    std::string error_;
};

/// Builds an envelope: `{"protocol": kProtocolVersion, "kind": kind}`.
[[nodiscard]] json::Value makeMessage(std::string_view kind);

/// The `kind` field of a message, or an empty string when it is not a valid envelope.
/// `error` is set when the message is malformed or from another protocol version.
[[nodiscard]] std::string messageKind(const json::Value& message, std::string& error);

} // namespace aura::plugin::sandbox
