// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/ProcessSupervisor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Holding a long-lived helper process: starting it, noticing when it stops, and
// stopping it when it will not.
//
// AURA already has plugin::runChildProcess(), and this is deliberately not that.
// runChildProcess() is spawn-wait-report with a wall-clock deadline, which is the
// right shape for scanning a bundle that should answer in milliseconds. A sandbox
// helper is the opposite shape: it is expected to run for the length of a session,
// it is watched by a counter rather than a clock (Decision 3 in
// docs/research/PLUGIN_SANDBOX_RESEARCH.md: "no progress for N blocks", because a
// block count means the same thing at 44.1 kHz and 192 kHz and a clock does not),
// and stopping it is a request first and a signal second. Two shapes, two
// mechanisms, one set of platform idioms - both redirect output to a file rather
// than a pipe, both put the POSIX child in its own process group, and both kill the
// group rather than the process.
//
// Thread contract: control thread only. Every method here can block or issue a
// system call. The audio thread never touches a supervisor: it talks to the arena,
// and the arena is the only thing that has to be real-time safe. That separation is
// the reason a hung helper costs the session a dry plug-in rather than a dropout.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::plugin::sandbox {

/// What to launch.
struct HelperLaunch {
    std::string executable;
    std::vector<std::string> arguments;
    /// When non-empty the helper's stdout and stderr are redirected to this file.
    ///
    /// A file and not a pipe, for the reason the scanner learned: a pipe needs the
    /// parent reading while the child writes, and a child that writes more than the
    /// pipe buffer holds blocks forever - which is precisely the failure this class
    /// exists to survive. The helper's last words are the only explanation of a
    /// crash that will ever exist, so the session always sets this.
    std::string outputPath;
};

/// What the supervisor last observed. Reported rather than inferred from an exit
/// code, because "the process is gone" and "we made it gone" are different facts
/// with different messages attached.
enum class HelperLiveness : std::uint8_t {
    NotStarted, ///< start() has not been called, or it failed
    Running,    ///< alive as of the last poll
    Exited,     ///< finished on its own; exitCode() is its own number
    Killed,     ///< we terminated it; exitCode() is the platform's report
};

[[nodiscard]] const char* toString(HelperLiveness liveness) noexcept;

class ProcessSupervisor {
public:
    ProcessSupervisor() = default;
    /// Stops and reaps. A supervisor that is destroyed while its helper runs would
    /// otherwise leave either a zombie (POSIX) or an orphan that keeps a shared
    /// mapping open (both), and an orphan holding the arena is a session that
    /// cannot create its next one.
    ~ProcessSupervisor();
    ProcessSupervisor(ProcessSupervisor&& other) noexcept;
    ProcessSupervisor& operator=(ProcessSupervisor&& other) noexcept;
    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    /// Starts the helper. Fails without side effects when it cannot be started, or
    /// when this supervisor is already responsible for one - starting twice would
    /// lose track of the first, and a helper nobody is watching is a helper nobody
    /// will kill.
    [[nodiscard]] Status start(const HelperLaunch& launch);

    /// Non-blocking: has it finished? Reaps it if so, which is what makes the
    /// liveness and the exit code available. Safe to call as often as the host's
    /// control loop likes; it is one waitpid(WNOHANG) or one zero-length wait.
    ///
    /// Always waits on the specific child, never on "any child": a process that
    /// supervises through two mechanisms at once (this and the scanner's) would
    /// otherwise steal the other's exit status.
    [[nodiscard]] HelperLiveness poll() noexcept;

    /// poll() until the helper exits or `timeoutSeconds` passes. Returns Running on
    /// timeout - the caller decides whether that is a hang worth killing.
    [[nodiscard]] HelperLiveness waitForExit(double timeoutSeconds) noexcept;

    /// Terminates the helper: a polite signal, `graceSeconds` to act on it, then an
    /// insistent one. Sets Killed.
    ///
    /// On POSIX "polite" is SIGTERM to the whole process group and "insistent" is
    /// SIGKILL to the group. On Windows there is no polite signal - TerminateProcess
    /// is the only verb - so the graceful path there is the arena's
    /// request::kShutdown bit, which SandboxSession sets before calling this. stop()
    /// is the enforcement that runs when the request was ignored, which is exactly
    /// when a helper is worth enforcing against.
    void stop(double graceSeconds = 0.5) noexcept;

    /// Terminates immediately with no grace period. For a helper the watchdog has
    /// already decided is hung: asking it nicely again would only delay the answer.
    void kill() noexcept;

    [[nodiscard]] HelperLiveness liveness() const noexcept { return liveness_; }
    /// Meaningful once poll() has seen it exit. -1 before that. On POSIX a helper
    /// that died on a signal reports 128+N, the shell convention, so a segfault is
    /// 139 and not "an exit code the plug-in chose".
    [[nodiscard]] int exitCode() const noexcept { return exitCode_; }
    [[nodiscard]] std::uint64_t processId() const noexcept { return processId_; }

    /// Reads and deletes the redirected output file. Only meaningful after the
    /// helper has exited: while it runs, the file is whatever has been flushed so
    /// far. Returns an empty string when there was no redirection or nothing was
    /// written, which is the normal case for a helper that shut down cleanly.
    [[nodiscard]] std::string takeOutput();

private:
    void detachHandles() noexcept;

    HelperLiveness liveness_ = HelperLiveness::NotStarted;
    int exitCode_ = -1;
    std::uint64_t processId_ = 0;
    std::string outputPath_;
    /// Whether we asked it to stop, which is what separates Killed from Exited when
    /// the platform cannot tell us why the process ended.
    bool weTerminated_ = false;
#if defined(_WIN32)
    void* processHandle_ = nullptr;
    void* threadHandle_ = nullptr;
#else
    int pid_ = -1;
#endif
};

} // namespace aura::plugin::sandbox
