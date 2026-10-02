// ============================================================================
// AURA DAW - tests/plugin/sandbox/SandboxHelperTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// M7 phase 2: the operating-system half of the sandbox. Phase 1 proved the
// transport correct between two *threads* over a heap buffer; this proves it
// between two *processes* over a real named mapping, and proves the supervisor
// can start, watch and kill the thing on the other end.
//
// The order of the cases here follows the order the failures would be noticed in
// production: a mapping that cannot be created, a mapping two sides disagree
// about, a helper that never starts, a helper that will not stop, and finally a
// full session in which real audio crosses a real process boundary and comes back
// processed.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ChildProcess.hpp"
#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/sandbox/HelperProtocol.hpp"
#include "aura/plugin/sandbox/ProcessSupervisor.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"
#include "aura/plugin/sandbox/SandboxProcessor.hpp"
#include "aura/plugin/sandbox/SharedAudioRing.hpp"
#include "aura/plugin/sandbox/SharedMemory.hpp"
#include "support/TestSignals.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

/// Writes a recognisable pattern so a reader can tell "the same bytes" from "two
/// buffers that both happen to be zero".
void stamp(void* base, std::size_t bytes, std::uint8_t seed) {
    auto* out = static_cast<std::uint8_t*>(base);
    for (std::size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<std::uint8_t>((seed + i * 31u) & 0xFFu);
}

bool matchesStamp(const void* base, std::size_t bytes, std::uint8_t seed) {
    const auto* in = static_cast<const std::uint8_t*>(base);
    for (std::size_t i = 0; i < bytes; ++i) {
        if (in[i] != static_cast<std::uint8_t>((seed + i * 31u) & 0xFFu))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("Two handles to one named mapping are the same memory", "[sandbox][shm]") {
    // This is the whole premise of the sandbox: the host and a helper each map the
    // arena and neither can tell the other's address. If this case fails there is
    // no phase 3, so it is asserted from both directions - writes are visible the
    // other way round as well, which is what separates a shared mapping from two
    // private copies of a file.
    constexpr std::size_t kBytes = 8192;
    auto created = SharedMemory::create("helper-test-shared", kBytes);
    REQUIRE(created.hasValue());
    CHECK(created.value().isValid());
    CHECK(created.value().size() == kBytes);
    CHECK(created.value().name() == "helper-test-shared");
    CHECK(created.value().data() != nullptr);

    // The platform name carries the namespace guard and the caller's name, and is
    // not asserted character by character because its shape differs per platform
    // ('/aura-x' on POSIX, 'Local\aura-x' on Windows). What matters is that it is
    // namespaced at all, or AURA collides with anything else on the machine.
    INFO("platform name: " << created.value().platformName());
    CHECK(created.value().platformName().find("aura-") != std::string::npos);
    CHECK(created.value().platformName().find("helper-test-shared") != std::string::npos);

    auto opened = SharedMemory::open("helper-test-shared", kBytes);
    REQUIRE(opened.hasValue());
    CHECK(opened.value().data() != created.value().data()); // different addresses...
    CHECK(opened.value().size() == kBytes);

    stamp(created.value().data(), kBytes, 0x11);
    CHECK(matchesStamp(opened.value().data(), kBytes, 0x11)); // ...same bytes

    stamp(opened.value().data(), kBytes, 0x7A);
    CHECK(matchesStamp(created.value().data(), kBytes, 0x7A));

    // A mapping opened by the peer is not owned by it: releasing the opener must
    // leave the creator's mapping usable, or a helper that exits cleanly would
    // take the session's buffers with it.
    opened.value().release();
    CHECK_FALSE(opened.value().isValid());
    CHECK(matchesStamp(created.value().data(), kBytes, 0x7A));
    CHECK(created.value().isValid());
}

TEST_CASE("Creating a mapping that already exists is refused rather than adopted",
          "[sandbox][shm]") {
    // AlreadyExists is not a formality. A mapping left behind by a crashed session
    // holds a header whose counters and payload belong to a process that is gone,
    // and adopting it would mean a new session starting with a ring that looks
    // half-full of somebody else's audio.
    auto first = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(first.hasValue());

    auto second = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(second.hasError());
    INFO(second.error().toString());
    CHECK(second.error().code == ErrorCode::AlreadyExists);

    // And the creator's release really unlinks, so the name is usable again in the
    // same process lifetime. Without this, one crash would cost the user every
    // later session until they found the stale file by hand.
    first.value().release();
    auto third = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(third.hasValue());
}

TEST_CASE("Opening a mapping nobody created reports NotFound", "[sandbox][shm]") {
    // For a helper this is the "the host went away before I started" case, which is
    // a clean exit and not a retry loop.
    auto missing = SharedMemory::open("helper-test-never-created", 4096);
    REQUIRE(missing.hasError());
    INFO(missing.error().toString());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("A mapping name that could reach outside the namespace is refused",
          "[sandbox][shm]") {
    // A mapping name is a machine-wide namespace and it usually originates from an
    // identifier AURA did not invent, so the accepted set is narrow and the refusal
    // is the whole mechanism - there is no sanitising step that a cleverer name
    // could get past.
    const std::vector<std::string> refused = {
        "",           "/",        "a/b",        "a\\b",      ".",        "..",
        "has space",  "tab\tchar", "semi;colon", "quote\"dq", "nl\nchar", "/dev/shm/x",
        "..\\..\\win", std::string(kMaxShmNameLength + 1, 'a'),
    };
    for (const auto& name : refused) {
        INFO("name: '" << name << "'");
        CHECK_FALSE(isUsableShmName(name));
        CHECK(platformShmName(name).empty());

        const auto created = SharedMemory::create(name, 4096);
        REQUIRE(created.hasError());
        CHECK(created.error().code == ErrorCode::InvalidArgument);

        const auto opened = SharedMemory::open(name, 4096);
        REQUIRE(opened.hasError());
        CHECK(opened.error().code == ErrorCode::InvalidArgument);
    }

    // The accepted set is still wide enough for a session identifier.
    const std::vector<std::string> accepted = {
        "a", "session-1", "vst3_4CC.ab12", "A.a-_", std::string(kMaxShmNameLength, 'n'),
    };
    for (const auto& name : accepted) {
        INFO("name: '" << name << "'");
        CHECK(isUsableShmName(name));
        CHECK_FALSE(platformShmName(name).empty());
    }

    // Zero bytes is refused too: a zero-length mapping is not a smaller arena, it
    // is no arena, and ArenaView would report it as a size mismatch instead.
    CHECK(SharedMemory::create("helper-test-zero", 0).hasError());
    CHECK(SharedMemory::open("helper-test-zero", 0).hasError());
}

TEST_CASE("A mapping is a single owner: moving it does not unmap twice",
          "[sandbox][shm]") {
    // Two owners of one mapping would unmap it twice, which on POSIX is a
    // munmap(EINVAL) at best and a hole in the address space at worst. The move
    // leaves the source empty, and the destination still shares bytes with a peer.
    constexpr std::size_t kBytes = 4096;
    auto created = SharedMemory::create("helper-test-move", kBytes);
    REQUIRE(created.hasValue());

    SharedMemory moved = std::move(created.value());
    CHECK(moved.isValid());
    CHECK_FALSE(created.value().isValid());
    CHECK(created.value().data() == nullptr);
    CHECK(created.value().size() == 0);

    auto peer = SharedMemory::open("helper-test-move", kBytes);
    REQUIRE(peer.hasValue());
    stamp(moved.data(), kBytes, 0x33);
    CHECK(matchesStamp(peer.value().data(), kBytes, 0x33));

    // Move-assignment onto a live mapping must release what it held first, or the
    // old object leaks its name until process exit.
    auto other = SharedMemory::create("helper-test-move-other", kBytes);
    REQUIRE(other.hasValue());
    moved = std::move(other.value());
    CHECK(moved.isValid());
    CHECK(moved.name() == "helper-test-move-other");
    // What "released" means differs by platform, and the difference is worth a test
    // rather than a comment because it decides how a session picks its name. This
    // case originally asserted the POSIX answer unconditionally and passed on Linux
    // for a whole increment; CI's Windows jobs are what disagreed with it.
#if defined(_WIN32)
    // Windows has no shm_unlink: a named section lives until its last handle closes,
    // and `peer` is still holding one. AlreadyExists is the correct report here -
    // create() refusing is what stops it handing back a live peer's section.
    auto whilePeerLives = SharedMemory::create("helper-test-move", kBytes);
    REQUIRE(whilePeerLives.hasError());
    CHECK(whilePeerLives.error().code == ErrorCode::AlreadyExists);
    peer.value().release();
#else
    // POSIX: the creator's release() unlinked the NAME, so the peer is now reading
    // an unnamed object that is still perfectly valid - which is the property that
    // lets a host shut down without pulling buffers out from under a live helper.
    CHECK(matchesStamp(peer.value().data(), kBytes, 0x33));
    auto whilePeerLives = SharedMemory::create("helper-test-move", kBytes);
    CHECK(whilePeerLives.hasValue()); // the name is already free again
    whilePeerLives.value().release();
#endif

    // With nothing holding the old object, the name is free on both platforms -
    // proof that the move-assignment above released what it held and orphaned
    // nothing.
    auto recreated = SharedMemory::create("helper-test-move", kBytes);
    CHECK(recreated.hasValue());
}

// ===========================================================================
// Phase 2: the helper process, the supervisor that owns it, and the attach
// overload that exists because a helper has no spec of its own.
// ===========================================================================

namespace {

constexpr std::uint32_t kBlockSize = 128;
constexpr std::uint32_t kChannels = 2;

ArenaSpec sessionSpec() noexcept {
    ArenaSpec spec{};
    spec.maxBlockSize = kBlockSize;
    spec.maxChannels = kChannels;
    return spec;
}

std::string helperLogPath(const char* tag) {
    static std::atomic<int> counter{0};
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    const std::filesystem::path base = error ? std::filesystem::path(".") : directory;
    const std::string name = std::string("aura-helper-test-") + tag + "-" +
                             std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
                             ".log";
    return (base / name).string();
}

/// A signal that depends on the block number, so a block that came back out of
/// order, twice, or from a different origin is wrong in a way a listener could
/// hear and a test can measure. Every value is a normal float and every product
/// with the gain is exact, so the comparison below is == and not "close enough".
float signalAt(std::uint64_t origin, std::uint32_t channel, std::uint32_t frame) noexcept {
    const std::uint64_t index = origin * kBlockSize + channel * 31u + frame;
    const std::uint32_t step = static_cast<std::uint32_t>(index % 97u);
    return (static_cast<float>(step) / 97.0f) * 0.8f - 0.4f;
}

void fillBlock(test::Block& block, std::uint64_t origin) {
    for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
        for (std::uint32_t frame = 0; frame < kBlockSize; ++frame)
            block.channels[channel][frame] = signalAt(origin, channel, frame);
    }
}

void clearBlock(test::Block& block) {
    for (auto& channel : block.channels) {
        for (auto& sample : channel)
            sample = 0.0f;
    }
}

bool processExists(std::uint64_t pid) noexcept {
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    ::CloseHandle(handle);
    return true;
#else
    // Signal 0 does not send anything; it reports whether the pid exists and is
    // ours to signal. A zombie would still answer, so this is only proof because the
    // supervisor reaps - which is itself the thing under test.
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

/// Waits for a predicate with a deadline, because everything here is waiting on
/// another process and "eventually" needs a bound or a failure becomes a hang.
template <typename Predicate>
bool waitFor(Predicate predicate, double timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(timeoutSeconds));
    for (;;) {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("A helper with no spec of its own attaches from the arena header",
          "[sandbox][helper][arena]") {
    // The host chooses a session's limits and the helper is told nothing but where
    // to look, so it has no independent copy of the spec to compare against. This
    // overload exists for that, and it is only safe because the header is then
    // proved against the bytes - which the second half of this case checks by lying.
    const ArenaSpec spec = sessionSpec();
    ArenaSpec normalised{};
    REQUIRE(static_cast<bool>(computeLayout(spec, normalised)));
    const std::size_t bytes = arenaBytes(normalised);
    REQUIRE(bytes > 0);

    auto mapping = SharedMemory::create("helper-attach", bytes);
    REQUIRE(mapping.hasValue());
    auto created = ArenaView::create(mapping.value().data(), mapping.value().size(), normalised);
    REQUIRE(created.hasValue());

    auto attached = ArenaView::attach(mapping.value().data(), mapping.value().size());
    REQUIRE(attached.hasValue());
    CHECK(attached.value().spec() == normalised);
    CHECK(attached.value().layout() == created.value().layout());
    CHECK(attached.value().size() == created.value().size());

    // The rings agree with it too, which is the only reason the helper wants a view.
    SharedAudioRing helperSide;
    CHECK(helperSide.attach(attached.value(), SharedAudioRing::Side::Helper).hasValue());

    auto* header = reinterpret_cast<ArenaHeader*>(mapping.value().data());

    SECTION("a header that lies about its spec is refused") {
        const ArenaSpec honest = header->spec;
        header->spec.maxBlockSize = honest.maxBlockSize * 2u;
        const auto lied = ArenaView::attach(mapping.value().data(), mapping.value().size());
        REQUIRE(lied.hasError());
        INFO(lied.error().toString());
        // The spec normalises fine on its own; what betrays the lie is that the
        // offsets it implies are not the offsets the header claims.
        CHECK(lied.error().code == ErrorCode::InvalidArgument);
        header->spec = honest;
    }

    SECTION("a header that lies about its magic is refused as a foreign format") {
        const std::uint32_t honest = header->magic;
        header->magic = 0xDEADBEEFu;
        const auto foreign = ArenaView::attach(mapping.value().data(), mapping.value().size());
        REQUIRE(foreign.hasError());
        CHECK(foreign.error().code == ErrorCode::UnsupportedFormat);
        header->magic = honest;
    }

    SECTION("a header from a newer layout is refused rather than guessed at") {
        const std::uint32_t honest = header->layoutVersion;
        header->layoutVersion = honest + 1u;
        const auto newer = ArenaView::attach(mapping.value().data(), mapping.value().size());
        REQUIRE(newer.hasError());
        CHECK(newer.error().code == ErrorCode::UnsupportedVersion);
        header->layoutVersion = honest;
    }

    SECTION("a mapping too short to hold a header is refused before it is read") {
        const auto truncated = ArenaView::attach(mapping.value().data(), sizeof(ArenaHeader) - 1u);
        REQUIRE(truncated.hasError());
        CHECK(truncated.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("The supervisor reports a helper that finishes on its own", "[sandbox][helper][process]") {
    const std::string helperPath = plugin::helperExecutablePath(kHelperExecutableName);
    REQUIRE_FALSE(helperPath.empty());

    ProcessSupervisor supervisor;
    CHECK(supervisor.liveness() == HelperLiveness::NotStarted);
    CHECK(supervisor.exitCode() == -1);

    HelperLaunch launch;
    launch.executable = helperPath;
    launch.arguments = {helperArg::kVersion};
    launch.outputPath = helperLogPath("version");
    REQUIRE(supervisor.start(launch).hasValue());
    CHECK(supervisor.processId() != 0u);

    const HelperLiveness finished = supervisor.waitForExit(10.0);
    INFO("liveness: " << toString(finished) << ", exit code " << supervisor.exitCode());
    CHECK(finished == HelperLiveness::Exited);
    CHECK(supervisor.liveness() == HelperLiveness::Exited);
    CHECK(supervisor.exitCode() == helperExit::kOk);

    const std::string output = supervisor.takeOutput();
    INFO("helper output: " << output);
    CHECK(output.find("aura_plugin_host") != std::string::npos);
    CHECK(output.find(processorId::kGain) != std::string::npos);
    // Taking the output removes it: a diagnostic file per session is a directory
    // full of files nobody deletes.
    CHECK(supervisor.takeOutput().empty());
}

TEST_CASE("A helper that cannot be started is an error, not a hang",
          "[sandbox][helper][process]") {
    ProcessSupervisor supervisor;
    HelperLaunch launch;
#if defined(_WIN32)
    launch.executable = "C:\\nonexistent\\aura_plugin_host_does_not_exist.exe";
#else
    launch.executable = "/nonexistent/aura_plugin_host_does_not_exist";
#endif
    launch.arguments = {helperArg::kVersion};

    const Status started = supervisor.start(launch);
    REQUIRE(started.hasError());
    INFO(started.error().toString());
    CHECK(supervisor.liveness() == HelperLiveness::NotStarted);
    CHECK(supervisor.exitCode() == -1);

    // And an empty executable is refused before anything is attempted.
    HelperLaunch empty;
    CHECK(supervisor.start(empty).hasError());
}

TEST_CASE("A supervisor refuses to lose track of a helper it is already running",
          "[sandbox][helper][process]") {
    const std::string helperPath = plugin::helperExecutablePath(kHelperExecutableName);
    REQUIRE_FALSE(helperPath.empty());

    ArenaSpec normalised{};
    REQUIRE(static_cast<bool>(computeLayout(sessionSpec(), normalised)));
    auto mapping = SharedMemory::create("helper-double-start", arenaBytes(normalised));
    REQUIRE(mapping.hasValue());
    REQUIRE(ArenaView::create(mapping.value().data(), mapping.value().size(), normalised)
                .hasValue());

    ProcessSupervisor supervisor;
    HelperLaunch launch;
    launch.executable = helperPath;
    launch.arguments = {helperArg::kArena,       "helper-double-start",
                        helperArg::kBytes,       std::to_string(arenaBytes(normalised)),
                        helperArg::kProcessor,   processorId::kGain,
                        helperArg::kStartupTimeoutMs, "30000"};
    REQUIRE(supervisor.start(launch).hasValue());

    // A second start would overwrite the pid or handle of the first, and a helper
    // nobody is watching is a helper nobody will kill.
    const Status second = supervisor.start(launch);
    REQUIRE(second.hasError());
    INFO(second.error().toString());
    CHECK(second.error().code == ErrorCode::InvalidArgument);
    CHECK(supervisor.liveness() == HelperLiveness::Running);

    supervisor.stop(1.0);
    CHECK(supervisor.liveness() == HelperLiveness::Killed);
}

TEST_CASE("A supervisor that goes out of scope takes its helper with it",
          "[sandbox][helper][process]") {
    const std::string helperPath = plugin::helperExecutablePath(kHelperExecutableName);
    REQUIRE_FALSE(helperPath.empty());

    ArenaSpec normalised{};
    REQUIRE(static_cast<bool>(computeLayout(sessionSpec(), normalised)));
    auto mapping = SharedMemory::create("helper-orphan", arenaBytes(normalised));
    REQUIRE(mapping.hasValue());
    REQUIRE(ArenaView::create(mapping.value().data(), mapping.value().size(), normalised)
                .hasValue());

    std::uint64_t pid = 0;
    {
        ProcessSupervisor supervisor;
        HelperLaunch launch;
        launch.executable = helperPath;
        // Long deadlines: without the destructor this helper would sit here for a
        // minute, which is exactly the orphan the sandbox must not leave behind.
        launch.arguments = {helperArg::kArena,       "helper-orphan",
                            helperArg::kBytes,       std::to_string(arenaBytes(normalised)),
                            helperArg::kProcessor,   processorId::kGain,
                            helperArg::kStartupTimeoutMs, "60000",
                            helperArg::kIdleExitMs,       "60000"};
        REQUIRE(supervisor.start(launch).hasValue());
        pid = supervisor.processId();
        REQUIRE(pid != 0u);
        REQUIRE(waitFor([&] { return supervisor.poll() == HelperLiveness::Running; }, 1.0));
    }

    // An orphan holding the shared mapping is not a curiosity: it is the reason a
    // later session's create() would fail with AlreadyExists after a host crash.
    CHECK_FALSE(processExists(pid));
}

TEST_CASE("A helper whose host goes quiet gives up rather than waiting forever",
          "[sandbox][helper][process]") {
    // The backstop, tested the way it would happen: a host that created the arena
    // and then died before pushing a single block. Nothing tells the helper the host
    // is gone, so it infers it from the absence of work.
    const std::string helperPath = plugin::helperExecutablePath(kHelperExecutableName);
    REQUIRE_FALSE(helperPath.empty());

    ArenaSpec normalised{};
    REQUIRE(static_cast<bool>(computeLayout(sessionSpec(), normalised)));
    auto mapping = SharedMemory::create("helper-hostgone", arenaBytes(normalised));
    REQUIRE(mapping.hasValue());
    auto arena = ArenaView::create(mapping.value().data(), mapping.value().size(), normalised);
    REQUIRE(arena.hasValue());

    ProcessSupervisor supervisor;
    HelperLaunch launch;
    launch.executable = helperPath;
    launch.arguments = {helperArg::kArena,       "helper-hostgone",
                        helperArg::kBytes,       std::to_string(arenaBytes(normalised)),
                        helperArg::kProcessor,   processorId::kGain,
                        helperArg::kStartupTimeoutMs, "3000"};
    launch.outputPath = helperLogPath("hostgone");
    REQUIRE(supervisor.start(launch).hasValue());

    // Readiness is observed WHILE the helper lives, which is the only time it can
    // be: the helper replaces kReady with kExiting when it leaves, because the word
    // reports its current state and not its history. A host that wants to know
    // whether a helper ever became ready has to watch it, which is what having a
    // supervisor is for.
    ControlBlock* const control = arena.value().control();
    const bool ready = waitFor([&] {
        return (control->helperState.load(std::memory_order_acquire) & helperState::kReady) != 0u;
    }, 10.0);
    if (!ready) {
        supervisor.stop(1.0);
        INFO("helper diagnostics: " << supervisor.takeOutput());
    }
    REQUIRE(ready);

    // It announced itself and then heard nothing, so what follows is the backstop
    // firing rather than a startup failure.
    const HelperLiveness finished = supervisor.waitForExit(20.0);
    INFO("liveness: " << toString(finished) << ", exit code " << supervisor.exitCode());
    REQUIRE(finished == HelperLiveness::Exited);
    // Its own exit code, not a kill: a helper that notices its host is gone should
    // say so in a way the host can distinguish from a crash.
    CHECK(supervisor.exitCode() == helperExit::kHostGone);
    CHECK((control->helperState.load(std::memory_order_acquire) & helperState::kExiting) != 0u);
}

TEST_CASE("A real helper process runs audio through the transport end to end",
          "[sandbox][helper][process][e2e]") {
    // The case the whole milestone exists for: audio leaves this process, is
    // processed in another one, and comes back - every block, in order, with the
    // right samples, with a parameter change honoured and notes echoed.
    const std::string helperPath = plugin::helperExecutablePath(kHelperExecutableName);
    REQUIRE_FALSE(helperPath.empty());

    ArenaSpec normalised{};
    REQUIRE(static_cast<bool>(computeLayout(sessionSpec(), normalised)));
    const std::size_t bytes = arenaBytes(normalised);
    REQUIRE(bytes > 0);

    auto mapping = SharedMemory::create("helper-e2e", bytes);
    REQUIRE(mapping.hasValue());
    auto arena = ArenaView::create(mapping.value().data(), mapping.value().size(), normalised);
    REQUIRE(arena.hasValue());

    // Published before the helper starts, so it prepares for this session's rate
    // rather than for a default it guessed.
    TransportSnapshot transport{};
    transport.sampleRate = 48000.0;
    transport.tempoBpm = 120.0;
    transport.blockSize = kBlockSize;
    transport.channelCount = kChannels;
    transport.flags = transportFlag::kPlaying;
    publishTransportSnapshot(arena.value().control(), transport);

    SharedAudioRing host;
    REQUIRE(host.attach(arena.value(), SharedAudioRing::Side::Host).hasValue());
    CHECK(host.latencyBlocks() == 1u);

    constexpr std::uint64_t kBlocks = 200;
    ProcessSupervisor supervisor;
    HelperLaunch launch;
    launch.executable = helperPath;
    launch.arguments = {helperArg::kArena,       "helper-e2e",
                        helperArg::kBytes,       std::to_string(bytes),
                        helperArg::kProcessor,   processorId::kGain,
                        helperArg::kMaxBlocks,   std::to_string(kBlocks),
                        helperArg::kStartupTimeoutMs, "10000",
                        helperArg::kIdleExitMs,       "5000"};
    launch.outputPath = helperLogPath("e2e");
    REQUIRE(supervisor.start(launch).hasValue());

    ControlBlock* const control = arena.value().control();
    Telemetry* const telemetry = arena.value().telemetry();
    const bool ready = waitFor([&] {
        return (control->helperState.load(std::memory_order_acquire) & helperState::kReady) != 0u;
    }, 10.0);
    if (!ready) {
        // Only on failure, and only after stopping: takeOutput() removes the file, so
        // reading it early would leave nothing to report when it matters.
        supervisor.stop(1.0);
        INFO("helper diagnostics: " << supervisor.takeOutput());
    }
    REQUIRE(ready);

    // A parameter first, and waited for: the helper drains parameters before it pops
    // a block, so pushing one and immediately pushing audio would leave the first
    // blocks processed at the old gain. Waiting on the ring's consumed counter is
    // what makes the expected output exact rather than eventual.
    ParamRecord gain{};
    gain.paramId = 0;
    gain.sampleOffset = 0;
    gain.value = 0.25f; // normalised; the reference processor maps 0..1 to 0..2
    REQUIRE(host.pushParameter(gain));
    REQUIRE(waitFor([&] { return host.parameters().consumedCount() >= 1u; }, 5.0));

    // Notes the helper should echo back.
    const midi::Message first = midi::Message::noteOn(0, 60, 100);
    const midi::Message second = midi::Message::noteOff(0, 60);
    REQUIRE(host.pushNote(first));
    REQUIRE(host.pushNote(second));

    test::Block input(static_cast<int>(kChannels), static_cast<int>(kBlockSize));
    test::Block output(static_cast<int>(kChannels), static_cast<int>(kBlockSize));

    std::vector<std::uint64_t> origins;
    std::vector<midi::Message> echoed;
    origins.reserve(static_cast<std::size_t>(kBlocks));
    std::size_t wrongSamples = 0;
    std::uint64_t firstWrongOrigin = 0;
    float firstWrongExpected = 0.0f;
    float firstWrongActual = 0.0f;
    std::size_t refusals = 0;

    // Records one returned block and checks it sample by sample. The gain is exactly
    // 0.5 and every value in the signal is a normal float, so the product is exact in
    // IEEE single precision on both sides of the process boundary: this is == and not
    // "close enough", which is the only comparison that would catch a byte order, a
    // stride or an origin mix-up.
    auto record = [&](const PopResult& popped) {
        origins.push_back(popped.origin);
        if (popped.frames != kBlockSize) {
            ++wrongSamples;
            return;
        }
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            for (std::uint32_t frame = 0; frame < kBlockSize; ++frame) {
                const float expected = signalAt(popped.origin, channel, frame) * 0.5f;
                const float actual = output.channels[static_cast<std::size_t>(channel)]
                                                [static_cast<std::size_t>(frame)];
                if (actual != expected) {
                    if (wrongSamples == 0) {
                        firstWrongOrigin = popped.origin;
                        firstWrongExpected = expected;
                        firstWrongActual = actual;
                    }
                    ++wrongSamples;
                }
            }
        }
    };
    auto drainNotes = [&] {
        midi::Message note;
        while (host.popNoteOut(note))
            echoed.push_back(note);
    };

    // Steady state: publish this block, take back the previous one. This is what a
    // host's callback actually does, and it is why the rings are two slots deep
    // rather than deep enough to hold a backlog - they absorb jitter between two
    // processes, they are not a queue. An earlier version of this case pushed all
    // 200 blocks before draining any, and the helper duly dropped 196 outputs
    // because nobody was taking them; that was the test misusing the transport, not
    // the transport losing audio. `exchange()` pops before it pushes, so a refused
    // push never costs the block that came back with it.
    // One deadline for the whole push phase, checked rather than asserted per refusal:
    // a refusal happens hundreds of times in a run and is not itself a failure, so
    // making each one an assertion would bury the report in the transport's normal
    // back-and-forth.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool timedOut = false;
    for (std::uint64_t origin = 0; origin < kBlocks && !timedOut; ++origin) {
        fillBlock(input, origin);
        for (;;) {
            clearBlock(output);
            auto inView = input.view(static_cast<int>(kBlockSize));
            auto outView = output.view(static_cast<int>(kBlockSize));
            const BlockExchange exchanged = host.exchange(origin, inView, outView);
            if (exchanged.popped.outcome == PopOutcome::Consumed)
                record(exchanged.popped);
            drainNotes();
            if (exchanged.pushed == PushOutcome::Published || timedOut)
                break;
            ++refusals;
            if (std::chrono::steady_clock::now() >= deadline)
                timedOut = true;
            // Sleep rather than spin. Yielding in a tight loop looked harmless and
            // was not: it kept this thread on the core the helper needed, so the
            // helper fell behind, its output ring filled, and it dropped processed
            // blocks. A real host never spins - its callback returns and the device
            // sleeps until the next one - and a test that starves the process it is
            // measuring is measuring the test.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // The tail: the last block published has not come back yet.
    const bool drained = waitFor([&] {
        while (origins.size() < static_cast<std::size_t>(kBlocks)) {
            clearBlock(output);
            auto outView = output.view(static_cast<int>(kBlockSize));
            const PopResult popped = host.popOutput(outView);
            drainNotes();
            if (popped.outcome != PopOutcome::Consumed)
                return false;
            record(popped);
        }
        return true;
    }, 30.0);

    INFO("blocks delivered: " << origins.size() << " of " << kBlocks);
    INFO("notes echoed: " << echoed.size());
    INFO("host push refusals while waiting for room: " << refusals);
    CHECK_FALSE(timedOut);
    INFO("helper blocksProcessed: "
         << telemetry->blocksProcessed.load(std::memory_order_relaxed));
    INFO("helper overruns (outputs refused for want of room): "
         << telemetry->overruns.load(std::memory_order_relaxed));
    INFO("helper underruns: " << telemetry->underruns.load(std::memory_order_relaxed)
                              << ", torn " << telemetry->tornReads.load(std::memory_order_relaxed)
                              << ", lost " << telemetry->blocksLost.load(std::memory_order_relaxed));
    INFO("helper liveness now: " << toString(supervisor.poll()) << ", exit code "
                                 << supervisor.exitCode());
    {
        const auto hostSnapshot = host.stats().snapshot();
        INFO("host: refusals " << hostSnapshot.overruns << ", underruns " << hostSnapshot.underruns
                               << ", torn " << hostSnapshot.tornReads << ", lost "
                               << hostSnapshot.blocksLost);
    }
    if (wrongSamples != 0) {
        INFO("first wrong sample in block " << firstWrongOrigin << ": expected "
                                            << firstWrongExpected << ", got " << firstWrongActual);
    }
    REQUIRE(drained);
    CHECK(wrongSamples == 0u);
    CHECK(origins.size() == static_cast<std::size_t>(kBlocks));

    // In order, once each. A ring index would have hidden a reorder; the origin the
    // producer declared cannot (ADR-0017).
    bool inOrder = true;
    bool noDuplicates = true;
    for (std::size_t i = 0; i < origins.size(); ++i) {
        if (origins[i] != static_cast<std::uint64_t>(i))
            inOrder = false;
        if (i > 0 && origins[i] == origins[i - 1])
            noDuplicates = false;
    }
    CHECK(inOrder);
    CHECK(noDuplicates);

    REQUIRE(echoed.size() == 2u);
    CHECK(echoed[0].isNoteOn());
    CHECK(echoed[0].data1 == 60);
    CHECK(echoed[0].data2 == 100);
    CHECK(echoed[1].isNoteOff());
    CHECK(echoed[1].data1 == 60);

    // The helper reached its block limit and left on its own.
    const HelperLiveness finished = supervisor.waitForExit(15.0);
    INFO("liveness: " << toString(finished) << ", exit code " << supervisor.exitCode());
    INFO("helper diagnostics: " << supervisor.takeOutput());
    CHECK(finished == HelperLiveness::Exited);
    CHECK(supervisor.exitCode() == helperExit::kOk);

    // The heartbeat the host's watchdog will count, and the helper's own view of how
    // the exchange went.
    CHECK(telemetry->blocksProcessed.load(std::memory_order_relaxed) == kBlocks);
    const auto hostStats = host.stats().snapshot();
    INFO("host refusals " << hostStats.overruns << ", underruns " << hostStats.underruns
                          << ", torn " << hostStats.tornReads << ", lost " << hostStats.blocksLost);
    CHECK(hostStats.blocksLost == 0u);
    CHECK(hostStats.tornReads == 0u);
    CHECK(telemetry->tornReads.load(std::memory_order_relaxed) == 0u);
    CHECK(telemetry->blocksLost.load(std::memory_order_relaxed) == 0u);
    // The helper's own counters, published into the arena as it went. What is
    // asserted is that nothing was LOST, not that nothing was ever refused:
    // `overruns` counts refused push attempts, and under back-pressure a refusal is
    // the helper being told "not yet" about a block it then holds and retries. A run
    // with a thousand refusals and zero losses is the transport working; a run with
    // zero refusals and one loss would be the transport lying. The refusal count is
    // still worth printing, because it is how much slack the two processes needed
    // between them, and a session that shows it climbing is a session whose host
    // callback is getting slower.
    INFO("helper output refusals (retries under back-pressure, not losses): "
         << telemetry->overruns.load(std::memory_order_relaxed));

    // And the helper said it was leaving, which is how a host tells a clean shutdown
    // from a crash before it has an exit code to look at.
    CHECK((control->helperState.load(std::memory_order_acquire) & helperState::kExiting) != 0u);
}
