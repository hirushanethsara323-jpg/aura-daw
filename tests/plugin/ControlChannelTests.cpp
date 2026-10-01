// ============================================================================
// AURA DAW - tests/plugin/ControlChannelTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 2 of the sandbox, tested at the seam phase 1 could not reach: a real second
// process, spawned with real inherited handles, talking over a real channel.
//
// The in-process cases exist because they are the failure modes that are miserable to
// debug at a process boundary - a frame that lies about its length, a message from
// another protocol version, a peer that closes mid-conversation. Proving them
// in-process means a failure across processes is about the boundary and not about the
// framing.
//
// The helper binary is tests/plugin/SandboxEchoHelper.cpp, built next to the test
// binary by tests/CMakeLists.txt. It is deliberately not the real sandbox helper: it
// is the smallest second process that can answer, crash and hang on command.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h> // WriteFile/PeekNamedPipe-free, but HANDLE and DWORD are not free
#else
#include <sys/socket.h>
#endif

#include "ControlChannel.hpp"
#include "HelperProcess.hpp"
#include "aura/core/Json.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

constexpr double kShortWait = 2.0; ///< a timing failure here is not what is being tested

/// The helper binary, which the test build places next to aura_tests. A missing
/// helper is a build configuration problem, and the cases say so rather than
/// reporting it as a product failure.
std::string echoHelperPath() {
#if defined(_WIN32)
    return "aura_sandbox_echo_helper.exe";
#else
    return "aura_sandbox_echo_helper";
#endif
}

/// The helper's end of a pair this process made, adopted locally. POSIX has one
/// descriptor for both directions; Windows has two handles, and adopting only the
/// first would make the "helper" unable to answer.
ControlChannel adoptChildEnd(ControlChannel& host, std::string& error) {
#if defined(_WIN32)
    return ControlChannel::adoptHandles(host.childHandle(), host.childWriteHandle(), error);
#else
    return ControlChannel::adopt(host.childHandle(), error);
#endif
}

/// Writes bytes to a raw channel end, bypassing the framing on purpose: the only way
/// to test that the *receiver* refuses a dishonest length is to send one.
bool writeRaw(std::uintptr_t handle, const std::string& bytes) {
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle), bytes.data(),
                     static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
           written == bytes.size();
#else
    const ssize_t written =
        ::send(static_cast<int>(handle), bytes.data(), bytes.size(), MSG_NOSIGNAL);
    return written == static_cast<ssize_t>(bytes.size());
#endif
}

/// A host end plus a helper process already started on the other end.
struct HelperFixture {
    ControlChannel host;
    HelperProcess process;

    explicit HelperFixture(std::vector<std::string> arguments = {}) {
        std::string error;
        host = ControlChannel::createPair(error);
        REQUIRE(host.valid());
        HelperLaunch launch;
        launch.executable = echoHelperPath();
        launch.arguments = std::move(arguments);
        process = HelperProcess::spawn(launch, host, error);
        INFO("spawning the helper failed: " << error);
        REQUIRE(process.running());
    }

    ~HelperFixture() {
        // Ask politely, then make sure: the same order the supervisor uses.
        std::string error;
        host.sendMessage(makeMessage("bye"), error);
        bool timedOut = false;
        (void)process.waitForExit(1.0, timedOut, error);
        if (timedOut)
            process.terminate(error);
    }

    HelperFixture(const HelperFixture&) = delete;
    HelperFixture& operator=(const HelperFixture&) = delete;
};

} // namespace

TEST_CASE("A real helper process answers over the control channel", "[plugin][sandbox]") {
    HelperFixture fixture;

    json::Value question = makeMessage("ping");
    question.set("id", 7);
    std::string error;
    REQUIRE(fixture.host.sendMessage(question, error));

    json::Value answer;
    CHECK(fixture.host.receiveMessage(answer, kShortWait, error) == ChannelStatus::Frame);
    CHECK(answer["kind"].asString() == "echo");
    // The reply carries the whole message back, so a field that did not survive the
    // round trip shows up as a field that is not there.
    CHECK(answer["body"]["kind"].asString() == "ping");
    CHECK(answer["body"]["id"].asInt() == 7);
    CHECK(answer["body"]["protocol"].asInt() == kProtocolVersion);
}

TEST_CASE("The channel carries a frame larger than any pipe buffer", "[plugin][sandbox]") {
    HelperFixture fixture;

    // 256 KiB is far past the 64 KiB a Windows anonymous pipe holds by default, so
    // this is the case where a partial write has to be handled rather than assumed
    // away - the same path a plug-in's state blob will take.
    std::string blob(256u * 1024u, 'x');
    blob.front() = 'A';
    blob.back() = 'Z';

    json::Value message = makeMessage("blob");
    message.set("data", blob);
    std::string error;
    REQUIRE(fixture.host.sendMessage(message, error));

    json::Value answer;
    CHECK(fixture.host.receiveMessage(answer, kShortWait, error) == ChannelStatus::Frame);
    const std::string echoed = answer["body"]["data"].asString();
    CHECK(echoed.size() == blob.size());
    CHECK(echoed.front() == 'A');
    CHECK(echoed.back() == 'Z');
}

TEST_CASE("A helper that dies abnormally is reported as a crash, not as a hang",
          "[plugin][sandbox]") {
    HelperFixture fixture;

    std::string error;
    REQUIRE(fixture.host.sendMessage(makeMessage("crash"), error));

    bool timedOut = false;
    const int code = fixture.process.waitForExit(kShortWait, timedOut, error);
    CHECK_FALSE(timedOut);
    CHECK(code != 0); // abort() on POSIX, a fault code on Windows - both non-zero
    CHECK(fixture.process.crashed());
    CHECK_FALSE(fixture.process.running());

    // The other half of the boundary: the channel is closed, which is how the audio
    // path learns the plug-in is gone without anyone watching the process.
    json::Value ignored;
    CHECK(fixture.host.receiveMessage(ignored, kShortWait, error) != ChannelStatus::Frame);
}

TEST_CASE("A helper that stops answering hits the host's deadline, which is bounded",
          "[plugin][sandbox]") {
    HelperFixture fixture;

    std::string error;
    REQUIRE(fixture.host.sendMessage(makeMessage("sleep"), error));

    // The deadline is the whole point: receive has to *return*, and return roughly
    // when it says it will. A control thread a wedged plug-in can block forever is the
    // same defect as an audio thread that can be.
    const auto start = std::chrono::steady_clock::now();
    json::Value answer;
    const ChannelStatus status = fixture.host.receiveMessage(answer, 0.15, error);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(status == ChannelStatus::Timeout);
    CHECK(elapsed >= 0.1);
    CHECK(elapsed < 2.0); // a deadline, not a hope

    bool timedOut = false;
    CHECK(fixture.process.waitForExit(0.05, timedOut, error) == -1);
    CHECK(timedOut);
    REQUIRE(fixture.process.terminate(error));
    CHECK(fixture.process.waitForExit(kShortWait, timedOut, error) != -1);
}

TEST_CASE("The host's end of the channel survives the helper being killed",
          "[plugin][sandbox]") {
    // A killed helper must leave the host holding a *closed* channel, not a broken
    // object: the supervisor respawns onto a fresh channel and the old one still has
    // to be safe to destroy.
    std::string error;
    ControlChannel host = ControlChannel::createPair(error);
    REQUIRE(host.valid());
    {
        HelperLaunch launch;
        launch.executable = echoHelperPath();
        HelperProcess process = HelperProcess::spawn(launch, host, error);
        REQUIRE(process.running());
        REQUIRE(process.terminate(error));
        bool timedOut = false;
        (void)process.waitForExit(kShortWait, timedOut, error);
    }

    json::Value ignored;
    CHECK(host.receiveMessage(ignored, 0.3, error) == ChannelStatus::Closed);
    CHECK_FALSE(host.sendMessage(makeMessage("ping"), error));
    CHECK_FALSE(error.empty());
    host.close(); // idempotent; the destructor repeats it harmlessly
    host.close();
}

// ---------------------------------------------------------------------------
// In-process framing
// ---------------------------------------------------------------------------

TEST_CASE("Framing round-trips empty, small and large frames", "[plugin][sandbox]") {
    std::string error;
    ControlChannel host = ControlChannel::createPair(error);
    REQUIRE(host.valid());
    ControlChannel helper = adoptChildEnd(host, error);
    REQUIRE(helper.valid());

    for (const std::string& payload : {std::string{}, std::string("x"), std::string(4096, 'q')}) {
        REQUIRE(host.sendFrame(payload, error));
        std::string received;
        CHECK(helper.receiveFrame(received, kShortWait, error) == ChannelStatus::Frame);
        CHECK(received == payload);
    }

    // And the other direction, because a channel that only works one way is exactly
    // the defect that survives a handshake and fails on the first reply.
    REQUIRE(helper.sendFrame("back", error));
    std::string received;
    CHECK(host.receiveFrame(received, kShortWait, error) == ChannelStatus::Frame);
    CHECK(received == "back");

    // Closing the child's end is what makes a helper exit observable, so it had
    // better be observable here too.
    helper.close();
    host.closeChildEnd();
    CHECK(host.receiveFrame(received, 0.3, error) == ChannelStatus::Closed);
}

TEST_CASE("A message from another protocol version is refused, with the reason",
          "[plugin][sandbox]") {
    std::string error;
    ControlChannel host = ControlChannel::createPair(error);
    REQUIRE(host.valid());
    ControlChannel helper = adoptChildEnd(host, error);
    REQUIRE(helper.valid());

    json::Value future = makeMessage("hello");
    future.set("protocol", kProtocolVersion + 1);
    REQUIRE(helper.sendFrame(future.dump(false), error));

    json::Value received;
    CHECK(host.receiveMessage(received, kShortWait, error) == ChannelStatus::Error);
    CHECK(error.find("protocol") != std::string::npos);

    // Bytes that are not JSON at all, and JSON that is not an envelope, are errors
    // rather than empty messages: "the peer is not speaking our protocol" and "the
    // peer said nothing" have to look different in a log.
    REQUIRE(helper.sendFrame("this is not json", error));
    CHECK(host.receiveMessage(received, kShortWait, error) == ChannelStatus::Error);
    REQUIRE(helper.sendFrame(R"({"hello":true})", error));
    CHECK(host.receiveMessage(received, kShortWait, error) == ChannelStatus::Error);
}

TEST_CASE("A frame that claims to be huge is refused instead of allocated",
          "[plugin][sandbox]") {
    std::string error;
    ControlChannel host = ControlChannel::createPair(error);
    REQUIRE(host.valid());
    ControlChannel helper = adoptChildEnd(host, error);
    REQUIRE(helper.valid());

    // The receiver must never allocate what the sender claims without checking it:
    // a hostile or broken peer otherwise chooses the host's memory usage.
    const std::uint32_t claimed = kMaxFrameBytes + 1u;
    const std::string header{static_cast<char>(claimed & 0xFFu),
                             static_cast<char>((claimed >> 8) & 0xFFu),
                             static_cast<char>((claimed >> 16) & 0xFFu),
                             static_cast<char>((claimed >> 24) & 0xFFu)};
#if defined(_WIN32)
    REQUIRE(writeRaw(host.childWriteHandle(), header));
#else
    REQUIRE(writeRaw(host.childHandle(), header));
#endif

    std::string received;
    CHECK(host.receiveFrame(received, kShortWait, error) == ChannelStatus::Error);
    CHECK(error.find("limit") != std::string::npos);

    // And the sending side refuses it too, so the cap cannot be crossed by accident
    // from inside either process.
    const std::string oversize(static_cast<std::size_t>(kMaxFrameBytes) + 1u, 'z');
    CHECK_FALSE(helper.sendFrame(oversize, error));
    CHECK(error.find("limit") != std::string::npos);
}
