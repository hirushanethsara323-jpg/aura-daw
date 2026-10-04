// ============================================================================
// AURA DAW - src/plugin/sandbox/SandboxSession.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Starting, owning and stopping one helper process. See SandboxSession.hpp for the
// split of responsibility between this and the helper - the short version is that
// this file may reset the rings because it is the only one that knows whether a peer
// is running, and the helper may not.
//
// Nothing here is real-time. Every method can block, allocate or issue a system
// call, and the audio path is expected to go through ring() and never through this
// object; publishTransport() is the one exception, and it is written to be callable
// from a callback even though the rest of the class is not.
// ============================================================================

#include "aura/plugin/sandbox/SandboxSession.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "plugin/ChildProcess.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace aura::plugin::sandbox {
namespace {

/// Interval between polls while waiting for a helper to report ready. Short enough
/// that a helper which is ready immediately costs one sleep, long enough that
/// waiting five seconds for one that never starts is not five thousand syscalls.
constexpr int kReadyPollMs = 2;

/// How long to let a helper leave on its own before the supervisor's signals run.
/// A helper that has been asked to stop is in the middle of finishing a block, which
/// at the largest supported buffer is under a millisecond.
constexpr double kGracefulExitSeconds = 0.25;

/// Where the helper's diagnostic output goes.
///
/// A file in the temporary directory rather than a pipe, for the reason the scanner
/// learned: a pipe needs the parent reading while the child writes, and a child that
/// writes more than the pipe buffer holds blocks forever - which is precisely the
/// failure a supervisor exists to survive. Named after the session so two sessions do
/// not write one file.
std::string helperLogPath(const std::string& sessionName) {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    const std::filesystem::path base = error ? std::filesystem::path(".") : directory;
    return (base / (sessionName + ".aura-helper.log")).string();
}

std::uint64_t currentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// naming
// ---------------------------------------------------------------------------

std::string SandboxSession::generateName() noexcept {
    // Unique per session, not per plug-in and not per run. The reason is Windows:
    // it has no shm_unlink, so a named section lives until its last handle closes,
    // and a previous helper that is still alive - which is exactly the situation a
    // restart happens in - would make a fixed name unusable. POSIX would tolerate a
    // fixed name, and tolerating it on one platform is how a bug ships.
    //
    // The counter covers two sessions started within one clock tick; the pid covers
    // two AURA processes on one machine; the clock covers a restarted one.
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();

    std::string name = "s" + std::to_string(currentProcessId()) + "-" +
                       std::to_string(serial) + "-" +
                       std::to_string(static_cast<std::uint64_t>(ticks) % 1000000000ull);
    if (name.size() > kMaxShmNameLength)
        name.resize(kMaxShmNameLength);
    return name;
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------

AuraResult<std::unique_ptr<SandboxSession>> SandboxSession::start(const SessionConfig& config,
                                                                SessionReport& report) {
    report = SessionReport{};
    // The block size, channel count and slot depth are range-checked by
    // computeLayout() below, which is the one place that knows what the arena can
    // hold. Checking them again here would be a second opinion that could drift from
    // the first, and would report a limit the layout does not actually have.

    // A private constructor means make_unique cannot be used, and `new` here is the
    // point: a session that fails halfway has to be destroyed, because its destructor
    // is what stops the helper and unlinks the name.
    std::unique_ptr<SandboxSession> session(new SandboxSession());
    session->config_ = config;
    session->supervisor_ = std::make_unique<ProcessSupervisor>();

    // ---------------------------------------------------------- the executable
    session->executablePath_ = config.helperExecutable;
    if (session->executablePath_.empty())
        session->executablePath_ = plugin::helperExecutablePath(kHelperExecutableName);
    if (session->executablePath_.empty())
        return failureT<std::unique_ptr<SandboxSession>>(makeError(
            ErrorCode::NotFound, "the plug-in helper executable could not be found",
            std::string("looked for ") + kHelperExecutableName +
                " next to the running executable and in ../tools"));

    // ------------------------------------------------------------- the mapping
    ArenaSpec spec{};
    spec.maxBlockSize = config.maxBlockSize;
    spec.maxChannels = config.maxChannels;
    spec.audioSlots = config.audioSlots;
    ArenaSpec normalised{};
    const auto layout = computeLayout(spec, normalised);
    if (layout.hasError()) {
        report.failure = "the arena could not be laid out: " + layout.error().toString();
        return failureT<std::unique_ptr<SandboxSession>>(layout.error());
    }
    const std::size_t bytes = arenaBytes(normalised);
    if (bytes == 0)
        return failureT<std::unique_ptr<SandboxSession>>(makeError(
            ErrorCode::InvalidArgument, "the sandbox arena could not be sized"));

    session->name_ = config.name.empty() ? generateName() : config.name;
    auto mapping = SharedMemory::create(session->name_, bytes);
    if (mapping.hasError()) {
        report.failure = "could not create the shared mapping: " +
                                   mapping.error().toString();
        return failureT<std::unique_ptr<SandboxSession>>(mapping.error());
    }
    session->mapping_ = std::move(mapping.value());

    auto arena = ArenaView::create(session->mapping_.data(), session->mapping_.size(), normalised);
    if (arena.hasError()) {
        report.failure =
            "could not lay out the arena: " + arena.error().toString();
        return failureT<std::unique_ptr<SandboxSession>>(arena.error());
    }
    session->arena_ = std::move(arena.value());

    // Published before the helper starts, so it prepares for this session's sample
    // rate instead of a default it guessed. A helper that prepared for the wrong rate
    // is not audibly wrong in a way anyone would diagnose quickly.
    // The session's own limits rather than the arena's budget: a helper that prepared
    // for the budget would be preparing for blocks the device will never send, and a
    // plug-in that read blockSize from here would report the wrong latency.
    TransportSnapshot initial{};
    initial.sampleRate = config.sampleRate;
    initial.tempoBpm = config.tempoBpm;
    initial.blockSize = config.maxBlockSize;
    initial.channelCount = config.maxChannels;
    session->publishTransport(initial);

    const Status attached = session->ring_.attach(session->arena_, SharedAudioRing::Side::Host);
    if (attached.hasError()) {
        report.failure = "could not attach to the arena: " + attached.error().toString();
        return failureT<std::unique_ptr<SandboxSession>>(attached.error());
    }

    // ------------------------------------------------------------- the helper
    HelperLaunch launch;
    launch.executable = session->executablePath_;
    launch.arguments = {helperArg::kArena,
                        session->name_,
                        helperArg::kBytes,
                        std::to_string(bytes),
                        helperArg::kProcessor,
                        config.processorId.empty() ? std::string(processorId::kGain)
                                                   : config.processorId,
                        helperArg::kIdleExitMs,
                        std::to_string(config.idleExitMs),
                        helperArg::kStartupTimeoutMs,
                        std::to_string(config.startupTimeoutMs)};
    if (config.maxBlocks != 0) {
        launch.arguments.push_back(helperArg::kMaxBlocks);
        launch.arguments.push_back(std::to_string(config.maxBlocks));
    }
    // The helper's stderr is redirected rather than piped, and kept: a process that
    // crashes cannot be asked what happened, so its last words are the only evidence
    // that will ever exist.
    launch.outputPath = helperLogPath(session->name_);

    const Status started = session->supervisor_->start(launch);
    if (started.hasError()) {
        report.failure = "could not start the helper: " + started.error().toString();
        return failureT<std::unique_ptr<SandboxSession>>(started.error());
    }

    // ----------------------------------------------------------- wait for ready
    ControlBlock* const control = session->arena_.control();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(
                                  config.readyTimeoutSeconds > 0.0 ? config.readyTimeoutSeconds
                                                                   : 5.0));
    for (;;) {
        if (control != nullptr &&
            (control->helperState.load(std::memory_order_acquire) & helperState::kReady) != 0u)
            return session;

        // A helper that has already left is not going to become ready, and waiting
        // out the timeout would turn "the processor id does not exist" into a five
        // second hang in the UI.
        if (session->supervisor_->poll() != HelperLiveness::Running) {
            report.liveness = session->supervisor_->liveness();
            report.exitCode = session->supervisor_->exitCode();
            report.helperOutput = session->supervisor_->takeOutput();
            report.failure =
                "the helper exited with code " + std::to_string(report.exitCode) +
                " before reporting ready";
            return failureT<std::unique_ptr<SandboxSession>>(makeError(
                ErrorCode::PluginLoadFailed, "the plug-in helper did not start",
                report.failure +
                    (report.helperOutput.empty()
                         ? std::string()
                         : ("\nhelper said: " + report.helperOutput))));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            report.failure = "the helper did not report ready within " +
                                       std::to_string(config.readyTimeoutSeconds) + "s";
            return failureT<std::unique_ptr<SandboxSession>>(makeError(
                ErrorCode::PluginLoadFailed, "the plug-in helper did not become ready",
                report.failure));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadyPollMs));
    }
}

// ---------------------------------------------------------------------------
// lifetime
// ---------------------------------------------------------------------------

SandboxSession::~SandboxSession() {
    // A session that is simply dropped still has to stop its helper: an orphan holds
    // the mapping open, and on Windows it holds the NAME open too.
    stop();
    if (supervisor_)
        report_.helperOutput = supervisor_->takeOutput();
}

void SandboxSession::stop(double graceSeconds) noexcept {
    if (stopped_ || !supervisor_)
        return;
    // Ask first. A helper that leaves on its own reports kOk, sets kExiting, and
    // publishes its final counters - all of which a SIGKILL would take away, and all
    // of which are what distinguishes "shut down" from "died" in a diagnostic.
    requestShutdown();
    // Assigned rather than discarded: waitForExit() is what reaps the helper and
    // records the code it chose, and a helper that left on its own reports kOk where
    // one that had to be signalled does not. That difference is the whole diagnostic.
    // waitForExit() reaps and records, so a helper that left on its own is already
    // accounted for; only one that ignored the request needs the supervisor's signals.
    if (supervisor_->waitForExit(kGracefulExitSeconds) == HelperLiveness::Running)
        supervisor_->stop(graceSeconds);
    stopped_ = true;
    report_.liveness = supervisor_->liveness();
    report_.exitCode = supervisor_->exitCode();
    // The creator unlinks on release, so the name is free again for the next session
    // on POSIX immediately and on Windows once the helper's handle is gone - which,
    // having just stopped it, is now.
    mapping_.release();
}

SessionReport SandboxSession::report() {
    stop();
    if (supervisor_ && report_.helperOutput.empty())
        report_.helperOutput = supervisor_->takeOutput();
    return report_;
}

bool SandboxSession::isAlive() noexcept {
    if (stopped_ || !supervisor_)
        return false;
    return supervisor_->poll() == HelperLiveness::Running;
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------

void SandboxSession::requestReset() noexcept {
    ControlBlock* const control = arena_.control();
    if (control == nullptr)
        return;
    // A request, not a counter reset - see the class comment. The helper re-prepares
    // its processor and clears its own tails while the transport keeps flowing, which
    // is what a user means by resetting a plug-in: the reverb stops ringing, the
    // audio does not glitch.
    control->hostRequests.fetch_or(request::kReset, std::memory_order_acq_rel);
}

void SandboxSession::requestShutdown() noexcept {
    ControlBlock* const control = arena_.control();
    if (control == nullptr)
        return;
    control->hostRequests.fetch_or(request::kShutdown, std::memory_order_acq_rel);
}

void SandboxSession::publishTransport(const TransportSnapshot& snapshot) noexcept {
    publishTransportSnapshot(arena_.control(), snapshot);
}

std::uint64_t SandboxSession::helperBlocksProcessed() const noexcept {
    const Telemetry* const telemetry = arena_.telemetry();
    if (telemetry == nullptr)
        return 0;
    return telemetry->blocksProcessed.load(std::memory_order_relaxed);
}

std::int32_t SandboxSession::latencyFrames(std::uint32_t blockSize) const noexcept {
    return ring_.latencyFrames(blockSize);
}

} // namespace aura::plugin::sandbox
