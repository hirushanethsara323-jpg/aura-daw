// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/SandboxSession.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// One helper process, and everything the host has to do to have one.
//
// A session is: create a named mapping, lay an arena out in it, publish what the
// session is (rate, tempo, position) so the helper prepares for the right thing,
// start the helper, wait for it to say it is ready, and then own its life until it
// is stopped. That is too much bookkeeping for the class that implements
// PluginInstance to also hold, and the crash protocol (phase 4) and the policy
// plumbing (phase 5) both need the same object - so it is its own class.
//
// The split of responsibility is deliberate and worth stating, because getting it
// wrong is how a session ends up resetting shared counters out from under a live
// peer:
//
//   * The SESSION owns lifecycle and the shared mapping. It is the only thing that
//     may call SharedAudioRing::reset(), and it does so only when no helper is
//     running - at start, and after a crash. reset() zeroes both sides' counters
//     plus the control word, so calling it while a peer is exchanging is a race, and
//     clearing hostRequests would swallow a shutdown request that arrived with it.
//   * The HELPER owns its processor's state. Asking a plug-in to reset (a transport
//     seek, a stop) is a request bit, not a counter reset: the helper re-prepares its
//     processor and clears its own tails, and the transport keeps flowing. Dropping
//     in-flight blocks is not what a user means by "reset".
//
// Thread contract: every method here is control thread only, and says so. The audio
// path reaches the session through ring() and nothing else - SharedAudioRing's own
// push/pop are the RT-safe surface, and a session that had to be consulted per block
// would be a session that could fail per block.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "aura/core/Errors.hpp"
#include "aura/plugin/sandbox/HelperProtocol.hpp"
#include "aura/plugin/sandbox/ProcessSupervisor.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"
#include "aura/plugin/sandbox/SharedAudioRing.hpp"
#include "aura/plugin/sandbox/SharedMemory.hpp"

namespace aura::plugin::sandbox {

/// What a session is asked to be.
struct SessionConfig {
    /// The processor the helper runs. Empty means the reference processor, which is
    /// what makes a session possible in a build with no plug-in SDK at all.
    std::string processorId = processorId::kGain;

    /// Limits the transport is sized for. A block bigger than `maxBlockSize` or wider
    /// than `maxChannels` is refused by the ring rather than truncated, so these come
    /// from the device, not from a guess.
    std::uint32_t maxBlockSize = 512;
    std::uint32_t maxChannels = 2;
    std::uint32_t audioSlots = kDefaultAudioSlots;

    double sampleRate = 48000.0;
    double tempoBpm = 120.0;

    /// Shared-memory name. Empty means "generate one unique to this session", which
    /// is what almost every caller wants: Windows has no unlink, so a name a previous
    /// helper still holds is a name this session cannot have, and a fixed name would
    /// collide exactly when a restart is happening. See SharedMemory.hpp.
    std::string name;

    /// Helper executable. Empty means "find it next to the running executable", the
    /// same search the VST 3 scanner uses.
    std::string helperExecutable;

    /// How long to wait for the helper to report ready before declaring the session
    /// failed. A helper that cannot start should fail fast enough that the user's
    /// click feels like it answered.
    double readyTimeoutSeconds = 5.0;

    /// Passed through to the helper. See HelperProtocol.hpp for why there are two and
    /// why both are backstops rather than the watchdog.
    std::int64_t idleExitMs = kDefaultIdleExitMs;
    std::int64_t startupTimeoutMs = kDefaultStartupTimeoutMs;

    /// Stops the helper after N blocks. Tests only; 0 means no limit.
    std::uint64_t maxBlocks = 0;
};

/// What happened to a session, in the terms a user would want to read.
struct SessionReport {
    HelperLiveness liveness = HelperLiveness::NotStarted;
    int exitCode = -1;
    /// The helper's own last words - its stderr, which is the only explanation of a
    /// failure that will ever exist, because a process that crashed cannot be asked.
    std::string helperOutput;
    /// Why the session failed, when it failed for a reason the host knows.
    std::string failure;

    [[nodiscard]] bool startedCleanly() const noexcept { return failure.empty(); }
};

class SandboxSession {
public:
    ~SandboxSession();
    // Neither copyable nor movable, because the ring it holds is neither: a ring
    // knows which SIDE of the transport it is, and a second object claiming the same
    // side would push and pop through one set of counters from two places. Sessions
    // are held by pointer - start() returns a unique_ptr - so nothing needs a move.
    SandboxSession(const SandboxSession&) = delete;
    SandboxSession& operator=(const SandboxSession&) = delete;
    SandboxSession(SandboxSession&&) = delete;
    SandboxSession& operator=(SandboxSession&&) = delete;

    /// Creates the mapping, lays out the arena, publishes the session and starts the
    /// helper, then waits for it to report ready.
    ///
    /// A failure here is complete: nothing is left running and no name is left
    /// behind, because a half-started session is indistinguishable from a hung one
    /// to everything above it.
    ///
    /// `report` is filled either way, and it is the reason the parameter exists: on
    /// failure there is no session object left to ask, and the helper's exit code and
    /// last words are the only explanation of a start that did not work. An error
    /// value carries a message, but the message is for a log and the report is for a
    /// caller that has to decide what to tell the user.
    [[nodiscard]] static AuraResult<std::unique_ptr<SandboxSession>> start(
        const SessionConfig& config, SessionReport& report);

    /// The transport. The only part of a session the audio thread touches.
    [[nodiscard]] SharedAudioRing& ring() noexcept { return ring_; }
    [[nodiscard]] const SharedAudioRing& ring() const noexcept { return ring_; }
    [[nodiscard]] ControlBlock* control() noexcept { return arena_.control(); }
    [[nodiscard]] Telemetry* telemetry() noexcept { return arena_.telemetry(); }

    [[nodiscard]] const ArenaSpec& spec() const noexcept { return arena_.spec(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::uint64_t processId() const noexcept { return supervisor_->processId(); }

    /// Blocks the helper has delivered since it started - the heartbeat the watchdog
    /// counts. Read from telemetry rather than tracked here, so a helper that stops
    /// incrementing it is noticed even if this object is never consulted again.
    [[nodiscard]] std::uint64_t helperBlocksProcessed() const noexcept;

    /// Asks the helper to reset its processor and re-publishes the transport on the
    /// next block. Does NOT touch the rings: see the class comment for why a plug-in
    /// reset is a request and not a counter reset.
    void requestReset() noexcept;
    /// Asks the helper to stop. It clears the bit and exits with kOk; `stop()` is what
    /// runs if it does not.
    void requestShutdown() noexcept;

    /// Asks, then enforces: the shutdown bit, a grace period for the helper to leave
    /// on its own terms, then the supervisor's signals. Idempotent.
    void stop(double graceSeconds = 0.5) noexcept;

    /// Non-blocking liveness, refreshed from the supervisor. An audio thread must not
    /// call this: it is a waitpid, and the audio path finds out about a dead helper by
    /// the rings going quiet, not by asking.
    [[nodiscard]] bool isAlive() noexcept;

    /// Stops and collects everything worth reporting. Takes the helper's output file,
    /// so a second call returns an empty one.
    [[nodiscard]] SessionReport report();

    /// Publishes what the session is, so the helper prepares for the right sample
    /// rate and a plug-in that asks for tempo gets the real one. Cheap enough to call
    /// whenever the transport moves; the seqlock exists for exactly this.
    void publishTransport(const TransportSnapshot& snapshot) noexcept;

    /// Latency in frames for a given block size: `(slots - 1) * blockSize`, which is
    /// what a deferred exchange costs and what the graph compensates for. Signed to
    /// match SharedAudioRing::latencyFrames(), which returns -1 when it cannot say.
    [[nodiscard]] std::int32_t latencyFrames(std::uint32_t blockSize) const noexcept;

private:
    SandboxSession() = default;

    /// Builds a name no other session can be using. Unique per session rather than
    /// per plug-in, for the Windows reason in SessionConfig::name.
    [[nodiscard]] static std::string generateName() noexcept;

    std::string name_;
    std::string executablePath_;
    SharedMemory mapping_;
    ArenaView arena_;
    SharedAudioRing ring_;
    std::unique_ptr<ProcessSupervisor> supervisor_;
    SessionConfig config_;
    SessionReport report_;
    bool stopped_ = false;
};

} // namespace aura::plugin::sandbox
