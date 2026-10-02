// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/HelperProtocol.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The contract between two programs: the plug-in helper process and the host that
// starts it. Nothing else in the sandbox needs to know how the helper is invoked
// or what its exit codes mean, and the helper needs to know nothing about the
// engine - so the agreement lives here, in one header both sides include.
//
// This is a wire format in all but name. It changes only with a reason, and a
// change that the other side cannot detect is a bug: the helper checks the arena's
// layout version, and the host checks the helper's exit code, so neither side can
// silently disagree.
//
// Thread contract: this header declares constants only.
// ============================================================================
#pragma once

#include <cstdint>

namespace aura::plugin::sandbox {

/// The helper executable's file name, without a directory or an extension.
/// Resolved next to the running executable the same way `aura_scan_host` is - see
/// plugin::helperExecutablePath().
inline constexpr const char* kHelperExecutableName = "aura_plugin_host";

/// Exit codes the helper returns.
///
/// Distinct per failure mode because the host reports one of these to the user
/// alongside the plug-in's name, and "the helper exited with 3" has to be
/// distinguishable from "the helper exited with 5" for the message to be worth
/// reading. 1 is deliberately unused: it is what a C runtime or an uncaught
/// exception produces, so reserving it means an exit code of 1 tells the host the
/// helper died in a way it did not choose.
namespace helperExit {
inline constexpr int kOk = 0;              ///< shut down on request, cleanly
inline constexpr int kBadArguments = 2;    ///< argv did not parse; the host's bug
inline constexpr int kShmOpenFailed = 3;   ///< no mapping by that name (host gone?)
inline constexpr int kAttachFailed = 4;    ///< a mapping, but not one we understand
inline constexpr int kProcessorFailed = 5; ///< could not load or prepare the plug-in
inline constexpr int kHostGone = 6;        ///< idle past the deadline with no work
} // namespace helperExit

/// Command-line switches. Long options with a separate value, because the helper's
/// argv is built by AURA and read by AURA, and `--name value` is the form that is
/// obvious in a process listing - which is where a user looks when a plug-in hangs.
namespace helperArg {
inline constexpr const char* kArena = "--arena";         ///< shared-memory name
inline constexpr const char* kBytes = "--bytes";         ///< mapping size, decimal
inline constexpr const char* kProcessor = "--processor"; ///< processor id to run
inline constexpr const char* kMaxBlocks = "--max-blocks";///< stop after N blocks (tests)
inline constexpr const char* kIdleExitMs = "--idle-exit-ms"; ///< host-gone backstop, once live
inline constexpr const char* kStartupTimeoutMs = "--startup-timeout-ms"; ///< before the first block
inline constexpr const char* kVersion = "--version";     ///< print and exit 0
} // namespace helperArg

/// Default for `--idle-exit-ms`.
///
/// This is a BACKSTOP, not the watchdog. The real watchdog (Decision 3 in
/// docs/research/PLUGIN_SANDBOX_RESEARCH.md) lives in the host and counts blocks,
/// not milliseconds, because "no progress for N blocks" is meaningful at any sample
/// rate and a clock is not available on the audio path. This one exists so a helper
/// cannot outlive a host that died without asking it to stop: with no host there
/// are no blocks, so after this long the helper exits with kHostGone rather than
/// spinning on a mapping nobody will ever write to again.
inline constexpr std::int64_t kDefaultIdleExitMs = 5000;

/// Default for `--startup-timeout-ms`.
///
/// Two deadlines rather than one, because "no blocks yet" means different things
/// before and after a session has been live. Before the first block the host may
/// legitimately still be opening a device, so the helper waits longer; after it, the
/// host is running a device callback that pushes a block whether or not the transport
/// is playing, so silence means the host is gone. Getting this wrong in the other
/// direction is what leaves an orphan holding a shared mapping after a host crash,
/// and the next session's create() refusing it with AlreadyExists.
///
/// Both are backstops. The real mechanisms are the host's block-count watchdog
/// (Decision 3) and, on Windows, a job object with KILL_ON_JOB_CLOSE (phase 6) -
/// neither of which needs the helper to guess anything about time.
inline constexpr std::int64_t kDefaultStartupTimeoutMs = 10000;

/// Processor ids the helper understands without any third-party SDK.
///
/// A reference processor is not a demo: it is what makes the whole cross-process
/// path testable on a machine with no VST 3 bundle installed, which is every CI
/// runner and every contributor who has not opted into the SDK. The real adapters
/// (VST 3, then CLAP) plug in behind the same `SandboxProcessor` interface and the
/// same helper, which is the point of Decision 6 in the research doc.
namespace processorId {
/// Applies parameter 0 as a linear gain (normalised 0..1 mapped to 0..2, so 0.5 is
/// unity) and forwards every note it receives back out. Deterministic on purpose:
/// a test can predict every sample and every event it should get back.
inline constexpr const char* kGain = "aura.sandbox.gain";
} // namespace processorId

} // namespace aura::plugin::sandbox
