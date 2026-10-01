// ============================================================================
// AURA DAW - tests/plugin/ChildProcessTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scanner's crash isolation, tested against the real helper process rather
// than a mock of one.
//
// What is actually being asserted: that a plug-in which hangs is stopped at the
// deadline instead of freezing the scan, that one which dies is reported as a
// crash, and that both are reported as *findings* rather than exceptions - the
// scanner runs on a worker thread with nowhere to throw to. `aura_scan_host`
// grows two behaviours for this (AURA_SCAN_BEHAVIOUR=hang|crash, see
// tools/scan_host/ScanHost.cpp); a test that simulated them inside the parent
// would prove nothing about the parent's ability to kill a child.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

#include "ChildProcess.hpp"
#include "aura/core/FileSystem.hpp"

using namespace aura;

namespace {

/// Sets an environment variable for the duration of one test and restores it
/// afterwards. The helper inherits the parent's environment, which is what makes
/// the simulated hang a real hang in a real process.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
    ~ScopedEnvironment() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
};

std::string scanHostPath() { return plugin::helperExecutablePath("aura_scan_host"); }

} // namespace

TEST_CASE("The scanner helper is built next to the tests", "[plugin][process]") {
    const std::string helper = scanHostPath();
    REQUIRE_FALSE(helper.empty());
    REQUIRE(filesystem::exists(helper));
}

TEST_CASE("A helper that finishes is reported with its output", "[plugin][process]") {
    plugin::ChildProcessRequest request;
    request.executable = scanHostPath();
    request.arguments = {"--version"};
    request.timeoutSeconds = 5.0;

    const auto result = plugin::runChildProcess(request);
    CHECK(result.started);
    CHECK_FALSE(result.timedOut);
    CHECK_FALSE(result.crashed());
    CHECK(result.exitCode == 0);
    CHECK(result.capturedOutput.find("aura_scan_host") != std::string::npos);
}

TEST_CASE("A helper that hangs is killed at the deadline", "[plugin][process]") {
    const ScopedEnvironment behaviour("AURA_SCAN_BEHAVIOUR", "hang");

    plugin::ChildProcessRequest request;
    request.executable = scanHostPath();
    request.arguments = {"--scan", "/nonexistent/AuraTest.vst3", "--out", "/tmp/unused.json"};
    request.timeoutSeconds = 1.0;

    const auto started = std::chrono::steady_clock::now();
    const auto result = plugin::runChildProcess(request);
    const double elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    CHECK(result.started);
    CHECK(result.timedOut);
    CHECK(result.crashed() == false); // killed by us, not by itself
    // The deadline is a deadline: "it eventually returned" is not good enough, so
    // the check allows the kill some slack but not a minute of it.
    CHECK(elapsedSeconds < 5.0);
    CHECK(result.error.find("did not finish") != std::string::npos);
}

TEST_CASE("A helper that crashes is reported as a crash", "[plugin][process]") {
    const ScopedEnvironment behaviour("AURA_SCAN_BEHAVIOUR", "crash");

    plugin::ChildProcessRequest request;
    request.executable = scanHostPath();
    request.arguments = {"--scan", "/nonexistent/AuraTest.vst3", "--out", "/tmp/unused.json"};
    request.timeoutSeconds = 10.0;

    const auto result = plugin::runChildProcess(request);
    CHECK(result.started);
    CHECK_FALSE(result.timedOut);
    CHECK(result.crashed());
    CHECK(result.exitCode != 0);
    CHECK(result.error.find("abnormally") != std::string::npos);
}

TEST_CASE("A helper that does not exist is an error, not a hang", "[plugin][process]") {
    plugin::ChildProcessRequest request;
    request.executable = "/nonexistent/aura_scan_host_does_not_exist";
    request.timeoutSeconds = 5.0;

    const auto started = std::chrono::steady_clock::now();
    const auto result = plugin::runChildProcess(request);
    const double elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    CHECK_FALSE(result.started);
    CHECK_FALSE(result.timedOut);
    CHECK_FALSE(result.error.empty());
    CHECK(elapsedSeconds < 2.0); // no waiting for something that was never created
}
