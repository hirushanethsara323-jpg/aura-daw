// ============================================================================
// AURA DAW - tools/scan_host/ScanHost.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `aura_scan_host` — the child process that loads a plug-in bundle and reports
// what is inside it. The host application runs this instead of loading untrusted
// code itself, so a bundle that hangs or crashes during scanning costs a process
// the host was willing to lose rather than the session the user is working in.
//
// Usage (documented in docs/PLUGIN_HOST.md, called by PluginScanner):
//   aura_scan_host --scan <bundle> --out <file>
//   aura_scan_host --version
//
// Test hooks (used by tests/plugin/ChildProcessTests.cpp to prove the supervisor
// really does kill a runaway child and really does report a crash):
//   AURA_SCAN_BEHAVIOUR=hang    block forever instead of scanning
//   AURA_SCAN_BEHAVIOUR=crash   abort() instead of scanning
// They are environment variables rather than flags so a real plug-in cannot ask
// its way into them: only whoever launched the scanner can set them.
//
// Exit codes: 0 scanned (the JSON says whether a bundle was usable), 1 the
// arguments were wrong, 2 this build cannot scan VST 3 (no SDK), 3 the bundle
// could not be read.
// ============================================================================

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "aura/core/Version.hpp"

#if AURA_ENABLE_VST3
#include "Vst3Host.hpp"
#endif

namespace {

std::string behaviourOverride() {
    if (const char* value = std::getenv("AURA_SCAN_BEHAVIOUR"))
        return value;
    return {};
}

int fail(int code, const std::string& message) {
    std::cerr << "aura_scan_host: " << message << "\n";
    return code;
}

} // namespace

int main(int argc, char** argv) {
    std::string bundle;
    std::string output;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--scan" && i + 1 < argc)
            bundle = argv[++i];
        else if (argument == "--out" && i + 1 < argc)
            output = argv[++i];
        else if (argument == "--version") {
            std::cout << "aura_scan_host " << aura::core::versionString() << "\n";
            return 0;
        } else {
            return fail(1, "unknown or incomplete argument: " + argument);
        }
    }
    if (bundle.empty() || output.empty())
        return fail(1, "usage: aura_scan_host --scan <bundle> --out <file>");

    // Test hooks first: they exist so the supervisor's timeout and crash paths are
    // exercised by something that really does hang and really does die.
    const std::string behaviour = behaviourOverride();
    if (behaviour == "hang") {
        std::this_thread::sleep_for(std::chrono::hours(1));
        return 0;
    }
    if (behaviour == "crash") {
        std::abort();
    }

#if AURA_ENABLE_VST3
    auto scanned = aura::plugin::vst3::scanBundle(bundle);
    if (!scanned) {
        // A bundle that cannot be scanned is normal (wrong architecture, a
        // plug-in that needs a licence dialog, a corrupt download). It is still a
        // successful *scan*: the answer is the JSON, not the exit code.
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream)
            return fail(3, "could not write " + output);
        stream << aura::plugin::vst3::scanResultToJson(bundle, false, scanned.error().message,
                                                       {});
        return 0;
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream)
        return fail(3, "could not write " + output);
    stream << aura::plugin::vst3::scanResultToJson(bundle, true, {}, scanned.value());
    return 0;
#else
    (void)bundle;
    (void)output;
    return fail(2, "this build was configured without AURA_ENABLE_VST3");
#endif
}
