// ============================================================================
// AURA DAW - src/plugin/ChildProcess.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Running a short-lived helper process with a wall-clock limit, and reporting
// what happened when it did not finish.
//
// This exists for exactly one reason: scanning plug-ins runs third-party code
// before the user has asked for anything, and that code is the most likely thing
// in a DAW to hang or crash. A scanner that cannot be killed is a DAW that cannot
// be started, so the host runs the risky half in a child process it is willing to
// terminate, and this is the mechanism. The same mechanism is useful for the
// crash-reporting helper later (M11).
//
// It is deliberately small: spawn, wait with a deadline, kill on expiry, collect
// the merged output. No pipes to drain (the child writes to a temporary file, so a
// child that blocks on a full pipe cannot deadlock the parent), no shell.
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace aura::plugin {

struct ChildProcessRequest {
    std::string executable;
    std::vector<std::string> arguments;
    /// Wall-clock budget. A plug-in that has not answered by then is treated the
    /// way the user would treat it: stopped, and remembered as unloadable.
    double timeoutSeconds = 10.0;
};

struct ChildProcessResult {
    bool started = false;      ///< the process was created at all
    bool timedOut = false;     ///< we had to kill it
    int exitCode = -1;         ///< process exit code, or 128+signal on POSIX
    double elapsedMs = 0.0;
    std::string capturedOutput; ///< merged stdout + stderr (UTF-8 as written)
    std::string error;          ///< human-readable reason when something went wrong

    /// True when the child died on its own instead of exiting or being killed.
    [[nodiscard]] bool crashed() const noexcept {
        return started && !timedOut && exitCode != 0;
    }
};

/// Runs the process and waits at most `timeoutSeconds`. Never throws; a failure to
/// start comes back in `error` because the caller is usually a background scanner
/// that has nowhere to propagate an exception to.
[[nodiscard]] ChildProcessResult runChildProcess(const ChildProcessRequest& request);

/// Path of a helper that ships next to the running executable (for example
/// `aura_scan_host`). Empty when the running executable's location cannot be
/// determined, in which case the caller falls back to in-process scanning.
[[nodiscard]] std::string helperExecutablePath(const std::string& name);

} // namespace aura::plugin
