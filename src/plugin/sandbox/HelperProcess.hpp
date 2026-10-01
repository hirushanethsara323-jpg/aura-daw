// ============================================================================
// AURA DAW - plugin/sandbox/HelperProcess.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Starting, watching and stopping one sandbox helper process (M7 phase 2).
//
// This is deliberately not src/plugin/ChildProcess.hpp. That one exists to run a
// *short-lived* process with a deadline and collect its output: perfect for a
// scanner, wrong for a helper that must outlive a scan by hours and stay
// interactive the whole time. The differences that matter:
//
//   * a channel, not a file - the helper is talked to while it runs;
//   * no wall-clock deadline of its own - the supervisor decides, because the
//     budget is "as long as the session lasts", which ChildProcess cannot express;
//   * a stable identity for the longest-lived thing this program starts, because
//     the sandbox's whole purpose is that its death must not be the session's.
//
// What is deliberately NOT here yet, and is written down rather than implied:
// the Windows job object (so a plug-in that spawns its own children dies with the
// helper). It is on the M7 phase-2 list in docs/research/PLUGIN_SANDBOX_RESEARCH.md
// and lands with the crash/hang protocol, not before.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ControlChannel.hpp"

namespace aura::plugin::sandbox {

struct HelperLaunch {
    std::string executable;             ///< absolute path to aura_plugin_host
    std::vector<std::string> arguments; ///< the helper's own arguments, without the channel
};

/// A running helper. Move-only, like the channel it talks through.
class HelperProcess {
public:
    HelperProcess() = default;
    ~HelperProcess();
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;
    HelperProcess(HelperProcess&& other) noexcept;
    HelperProcess& operator=(HelperProcess&& other) noexcept;

    /// Spawns the helper with one end of `channel`, which must come from
    /// `ControlChannel::createPair()`. The host's copy of the child's end is dropped
    /// here, so a helper exit is observable as a closed channel. `channel` is left
    /// holding the host's end only.
    [[nodiscard]] static HelperProcess spawn(const HelperLaunch& launch, ControlChannel& channel,
                                             std::string& error);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

    /// Kills the helper now. Used when the watchdog fires, when the plug-in is
    /// removed, and during shutdown - in that order of likelihood.
    bool terminate(std::string& error);

    /// Waits up to `timeoutSeconds` for the helper to exit (negative: forever).
    /// Returns the exit code, or -1 when it is still running (`timedOut` set) or on
    /// error. On POSIX a signal death comes back as 128 + signal, the convention
    /// src/plugin/ChildProcess.cpp already uses, so "crashed" and "exited non-zero"
    /// stay distinguishable by the caller that knows the difference.
    [[nodiscard]] int waitForExit(double timeoutSeconds, bool& timedOut, std::string& error);

    /// True once the helper has been observed to die abnormally.
    [[nodiscard]] bool crashed() const noexcept { return crashed_; }

private:
    /// Drops whatever the object currently holds, killing a live helper. Shared by
    /// the destructor and the move-assignment so there is one place that knows what
    /// "let go of this helper" means.
    void release() noexcept;

#if defined(_WIN32)
    void* processHandle_ = nullptr; ///< HANDLE
#endif
    /// On POSIX this is the pid; on Windows the process id from PROCESS_INFORMATION.
    /// One member rather than one per platform: the two mean the same thing to every
    /// caller, and a preprocessor-split member list is how a struct quietly becomes
    /// two structs.
    std::uint32_t id_ = 0;
    bool exited_ = false;
    bool crashed_ = false;
    int exitCode_ = -1;
};

} // namespace aura::plugin::sandbox
