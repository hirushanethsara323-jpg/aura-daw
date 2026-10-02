// ============================================================================
// AURA DAW - tests/plugin/sandbox/SandboxTransportTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// M7 phase 1: the sandbox transport, tested before there is a process to put it
// in. docs/research/PLUGIN_SANDBOX_RESEARCH.md names four things this phase has to
// prove - publish/consume ordering, overrun and underrun behaviour, sequence
// counter correctness, and no allocation after prepare() - and each has a case
// below. The cases that go further exist because a transport is only worth
// building if a *peer* cannot break it: a header that lies, a counter that jumps, a
// slot that changes mid-copy and a record that does not validate are all reachable
// from another process, so all of them are tested as errors rather than trusted as
// impossible.
//
// One honest gap. The post-copy re-check of a slot's sequence counter (the second
// of the two torn-read checks) cannot be reached deterministically from one
// thread, and reaching it from two would need a seam inside the copy loop. The
// library is not bent to make a test possible - that is this project's rule - so
// the check is covered by the invariant asserted in the two-thread case instead:
// every sample of every block that arrives is internally consistent, which is the
// property the re-check exists to protect. The FIRST check is covered
// deterministically ("a slot that is not stable is reported torn").
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/Realtime.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"
#include "aura/plugin/sandbox/SharedAudioRing.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

constexpr int kChannels = 2;
constexpr int kFrames = 64;

[[nodiscard]] std::size_t alignPad(std::uintptr_t address) noexcept {
    return static_cast<std::size_t>((static_cast<std::uintptr_t>(kCacheLine) -
                                     (address % static_cast<std::uintptr_t>(kCacheLine))) %
                                    kCacheLine);
}

/// A cache-line-aligned byte region. The arena requires one (its counters are
/// alignas(64)) and a real mapping supplies it for free - mmap and VirtualAlloc
/// both return page-aligned memory. Over-allocating and offsetting is portable in
/// a way std::aligned_alloc is not across every MSVC in the CI matrix.
class Region {
public:
    explicit Region(std::size_t bytes) : raw_(bytes + kCacheLine, std::uint8_t{0}) {
        pad_ = alignPad(reinterpret_cast<std::uintptr_t>(raw_.data()));
    }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    [[nodiscard]] void* base() noexcept { return raw_.data() + pad_; }
    [[nodiscard]] std::size_t size() const noexcept { return raw_.size() - pad_; }

private:
    std::vector<std::uint8_t> raw_;
    std::size_t pad_ = 0;
};

/// An aligned COPY of an arena, for the cases that tamper with a peer's header
/// before attaching to it. Tampering with the original would leave the fixture
/// unusable for the rest of the case, and the point of these tests is what a
/// *different process* would see.
class Copy {
public:
    Copy(const void* source, std::size_t bytes) : raw_(bytes + kCacheLine, std::uint8_t{0}) {
        pad_ = alignPad(reinterpret_cast<std::uintptr_t>(raw_.data()));
        std::memcpy(base(), source, bytes);
    }
    Copy(const Copy&) = delete;
    Copy& operator=(const Copy&) = delete;

    [[nodiscard]] void* base() noexcept { return raw_.data() + pad_; }
    [[nodiscard]] std::size_t size() const noexcept { return raw_.size() - pad_; }
    [[nodiscard]] ArenaHeader* header() noexcept {
        return reinterpret_cast<ArenaHeader*>(base());
    }

private:
    std::vector<std::uint8_t> raw_;
    std::size_t pad_ = 0;
};

/// A planar block whose every sample holds one value. A constant fill is what makes
/// a torn copy *visible*: a block that arrives half-filled with another block's
/// value cannot be mistaken for a block that merely arrived late.
class Block {
public:
    Block(int channels, int frames, float fill = 0.0f)
        : planes_(static_cast<std::size_t>(channels > 0 ? channels : 0)) {
        for (auto& plane : planes_)
            plane.assign(static_cast<std::size_t>(frames > 0 ? frames : 0), fill);
        pointers_.reserve(planes_.size());
        for (auto& plane : planes_)
            pointers_.push_back(plane.data());
        view_.channelPointers = pointers_.data();
        view_.numChannels = channels;
        view_.numFrames = frames;
    }

    [[nodiscard]] const dsp::AudioBlockView& view() const noexcept { return view_; }
    [[nodiscard]] dsp::AudioBlockView& view() noexcept { return view_; }

    void fill(float value) noexcept {
        for (auto& plane : planes_)
            for (auto& sample : plane)
                sample = value;
    }

    [[nodiscard]] bool allEqual(float value) const noexcept {
        return countDifferent(value) == 0u;
    }

    /// How many samples do not hold `value`. Reported on failure so a torn copy
    /// shows its shape - "37 of 128 samples were the previous block" says far more
    /// than "the assertion failed".
    [[nodiscard]] std::size_t countDifferent(float value) const noexcept {
        std::size_t different = 0;
        for (const auto& plane : planes_)
            for (const float sample : plane)
                if (sample != value)
                    ++different;
        return different;
    }

private:
    std::vector<std::vector<float>> planes_;
    std::vector<float*> pointers_;
    dsp::AudioBlockView view_{};
};

/// An arena plus both sides' view of it. Attaching two rings to the same bytes is
/// exactly what happens when the helper maps the region; the only difference in the
/// real thing is the address, which has its own case.
struct Fixture {
    ArenaSpec spec{};
    Region region;
    ArenaView arena;
    SharedAudioRing host;
    SharedAudioRing helper;

    explicit Fixture(ArenaSpec wanted = ArenaSpec{})
        : spec(wanted), region(arenaBytes(wanted) + kCacheLine) {
        auto created = ArenaView::create(region.base(), region.size(), wanted);
        REQUIRE(created.hasValue());
        arena = created.value();
        REQUIRE(static_cast<bool>(host.attach(arena, SharedAudioRing::Side::Host)));
        REQUIRE(static_cast<bool>(helper.attach(arena, SharedAudioRing::Side::Helper)));
    }
};

/// A transport snapshot that checks itself: every field is a function of `n`, so a
/// reader that gets half of one snapshot and half of another cannot mistake the
/// result for a valid one.
[[nodiscard]] TransportSnapshot selfCheckingSnapshot(std::int64_t n) noexcept {
    TransportSnapshot snapshot{};
    snapshot.sampleRate = 48000.0;
    snapshot.tempoBpm = 60.0 + static_cast<double>(n % 100);
    snapshot.positionSamples = n;
    snapshot.positionTicks = n * 2;
    snapshot.blockSize = static_cast<std::uint32_t>((n % 997) + 1);
    snapshot.channelCount = static_cast<std::uint32_t>(kChannels);
    snapshot.flags = (n % 2 == 0) ? transportFlag::kPlaying : 0u;
    return snapshot;
}

[[nodiscard]] bool isSelfConsistent(const TransportSnapshot& snapshot) noexcept {
    if (snapshot.positionTicks != snapshot.positionSamples * 2)
        return false;
    if (snapshot.blockSize != static_cast<std::uint32_t>((snapshot.positionSamples % 997) + 1))
        return false;
    if (snapshot.tempoBpm != 60.0 + static_cast<double>(snapshot.positionSamples % 100))
        return false;
    const std::uint32_t expectedFlags =
        (snapshot.positionSamples % 2 == 0) ? transportFlag::kPlaying : 0u;
    return snapshot.flags == expectedFlags && snapshot.sampleRate == 48000.0;
}

/// Gain 2.0 applied in place - the stand-in for a plug-in's work. Written once so
/// the helper-side loops in the cases below cannot drift apart.
void applyGain(const dsp::AudioBlockView& block, float gain) noexcept {
    for (int channel = 0; channel < block.numChannels; ++channel)
        for (int frame = 0; frame < block.numFrames; ++frame)
            block.channelPointers[channel][frame] *= gain;
}

} // namespace

// ---------------------------------------------------------------------------
// Layout and validation
// ---------------------------------------------------------------------------

TEST_CASE("An arena is sized from its spec and refuses a mapping that is too small",
          "[plugin][sandbox]") {
    ArenaSpec spec{};
    spec.maxBlockSize = 256;
    spec.maxChannels = 2;

    ArenaSpec normalised{};
    const auto layout = computeLayout(spec, normalised);
    REQUIRE(layout.hasValue());
    REQUIRE(layout.value().totalBytes > 0u);
    CHECK(arenaBytes(spec) == layout.value().totalBytes);

    // Every region starts on a cache line and lies inside the arena, which is the
    // property that keeps one process's counter out of another process's cache line.
    const ArenaLayout at = layout.value();
    const std::vector<std::pair<const char*, std::size_t>> regions = {
        {"control", at.control}, {"telemetry", at.telemetry}, {"input", at.inputRing},
        {"output", at.outputRing}, {"param", at.paramRing},   {"noteIn", at.noteInRing},
        {"noteOut", at.noteOutRing},
    };
    for (const auto& [name, offset] : regions) {
        INFO("region " << name);
        CHECK(offset % kCacheLine == 0u);
        CHECK(offset >= sizeof(ArenaHeader));
        CHECK(offset < at.totalBytes);
    }

    // Record capacities are normalised to a power of two because the index mask
    // depends on it - and a peer's *requested* capacity is not what attach()
    // compares, or an identical arena would be rejected for asking in decimal.
    ArenaSpec odd{};
    odd.paramCapacity = 100;
    ArenaSpec oddNormalised{};
    REQUIRE(static_cast<bool>(computeLayout(odd, oddNormalised)));
    CHECK(oddNormalised.paramCapacity == 128u);

    // Region over-allocates to reach a cache-line-aligned base, so a Region built
    // from a short byte count is not a short mapping - it is a roomy one that starts
    // further in. The too-small case has to be the *size argument* to create(),
    // against a base that is properly aligned, or the test passes for the wrong
    // reason (it did: this assertion was green while asserting nothing).
    Region roomy(layout.value().totalBytes + kCacheLine);
    const auto refused =
        ArenaView::create(roomy.base(), layout.value().totalBytes - 1u, spec);
    REQUIRE(refused.hasError());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    INFO(refused.error().toString());
    CHECK(refused.error().detail.find("need") != std::string::npos);

    ArenaSpec absurd{};
    absurd.maxBlockSize = 0;
    ArenaSpec ignored{};
    const auto bad = computeLayout(absurd, ignored);
    REQUIRE(bad.hasError());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    CHECK(arenaBytes(absurd) == 0u);

    // An arena's atomics are alignas(64); a base that is not aligned is undefined
    // rather than merely slower, so it is refused with the address in the message.
    ArenaSpec defaultSpec{};
    std::vector<std::uint8_t> heap(arenaBytes(defaultSpec) + kCacheLine, std::uint8_t{0});
    void* const offByOne = heap.data() + 1;
    const auto misaligned = ArenaView::create(offByOne, heap.size() - 1u, defaultSpec);
    REQUIRE(misaligned.hasError());
    CHECK(misaligned.error().code == ErrorCode::InvalidArgument);
    CHECK(misaligned.error().message.find("aligned") != std::string::npos);
}

TEST_CASE("A peer's header is validated before any of its bytes are trusted",
          "[plugin][sandbox]") {
    ArenaSpec spec{};
    spec.maxBlockSize = 128;
    spec.maxChannels = 2;
    Region region(arenaBytes(spec) + kCacheLine);
    auto created = ArenaView::create(region.base(), region.size(), spec);
    REQUIRE(created.hasValue());
    const std::size_t bytes = created.value().layout().totalBytes;

    SECTION("A magic that is not ours") {
        Copy copy(region.base(), bytes);
        copy.header()->magic = 0xDEADBEEFu;
        const auto result = ArenaView::attach(copy.base(), copy.size(), spec);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::UnsupportedFormat);
    }

    SECTION("A layout version from another build") {
        // Refused rather than interpreted: the first thing attach() would do with a
        // foreign header is dereference an offset the peer chose.
        Copy copy(region.base(), bytes);
        copy.header()->layoutVersion = kLayoutVersion + 1u;
        const auto result = ArenaView::attach(copy.base(), copy.size(), spec);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::UnsupportedVersion);
    }

    SECTION("A valid arena sized for a different session") {
        Copy copy(region.base(), bytes);
        ArenaSpec other = spec;
        other.maxBlockSize = 64;
        const auto result = ArenaView::attach(copy.base(), copy.size(), other);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(result.error().message.find("different session") != std::string::npos);
    }

    SECTION("The right version but different offsets") {
        // The case a version check alone cannot catch, which is why the stored
        // layout is compared field by field rather than trusted.
        Copy copy(region.base(), bytes);
        copy.header()->layout.telemetry += kCacheLine;
        const auto result = ArenaView::attach(copy.base(), copy.size(), spec);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(result.error().message.find("offsets") != std::string::npos);
    }

    SECTION("A header that claims more than the mapping holds") {
        Copy copy(region.base(), bytes);
        const auto result = ArenaView::attach(copy.base(), bytes - 1u, spec);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
    }

    SECTION("A mapping too small to hold a header at all") {
        Copy copy(region.base(), bytes);
        const auto result = ArenaView::attach(copy.base(), sizeof(ArenaHeader) - 1u, spec);
        REQUIRE(result.hasError());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
    }

    SECTION("An untouched copy attaches and works") {
        Copy copy(region.base(), bytes);
        const auto result = ArenaView::attach(copy.base(), copy.size(), spec);
        REQUIRE(result.hasValue());
        CHECK(result.value().isValid());
        CHECK(result.value().spec() == spec);
        CHECK(result.value().layout() == created.value().layout());
    }
}

TEST_CASE("An arena keeps working after being moved to another address",
          "[plugin][sandbox][regression]") {
    // The one bug this layout could have that every single-process test would miss:
    // a pointer stored inside the shared region. It would pass here, pass in the
    // helper's own tests, and corrupt the peer the first time two processes mapped
    // the same bytes at different addresses - which is the only way the transport is
    // ever used for real.
    ArenaSpec spec{};
    spec.maxBlockSize = 128;
    spec.maxChannels = 2;
    Fixture fixture(spec);

    Block input(kChannels, kFrames, 0.25f);
    REQUIRE(fixture.host.pushInput(input.view(), 5u) == PushOutcome::Published);

    Copy moved(fixture.region.base(), fixture.arena.layout().totalBytes);
    REQUIRE(moved.base() != fixture.region.base());

    auto attached = ArenaView::attach(moved.base(), moved.size(), spec);
    REQUIRE(attached.hasValue());

    SharedAudioRing relocatedHelper;
    REQUIRE(static_cast<bool>(
        relocatedHelper.attach(attached.value(), SharedAudioRing::Side::Helper)));

    Block work(kChannels, kFrames, 0.0f);
    const PopResult popped = relocatedHelper.popInput(work.view());
    REQUIRE(popped.outcome == PopOutcome::Consumed);
    CHECK(popped.frames == static_cast<std::uint32_t>(kFrames));
    // The origin travelled with the audio, so a helper that restarts against a moved
    // mapping still knows which block it is holding.
    CHECK(popped.origin == 5u);
    INFO("samples that were not 0.25: " << work.countDifferent(0.25f));
    CHECK(work.allEqual(0.25f));
}

// ---------------------------------------------------------------------------
// The deferred pipeline
// ---------------------------------------------------------------------------

TEST_CASE("The pipeline is one block deferred and says so as latency",
          "[plugin][sandbox]") {
    Fixture fixture{};

    // slots - 1, derived rather than hardcoded: a four-slot arena adds three blocks,
    // and this is the number the proxy will report through
    // PluginInstance::latencySamples() so the graph compensates it like any other.
    CHECK(fixture.host.latencyBlocks() == 1u);
    CHECK(fixture.host.latencyFrames(256) == 256);
    CHECK(fixture.host.latencyFrames(64) == 64);

    ArenaSpec deep{};
    deep.audioSlots = 4;
    Fixture deepFixture(deep);
    CHECK(deepFixture.host.latencyBlocks() == 3u);
    CHECK(deepFixture.host.latencyFrames(128) == 384);

    // Nothing has been produced yet, so the first callback's read is an underrun.
    // Not an error to hide: this IS the block of latency the design promises.
    Block out(kChannels, kFrames, -1.0f);
    const PopResult first = fixture.host.popOutput(out.view());
    CHECK(first.outcome == PopOutcome::Empty);
    CHECK(fixture.host.stats().snapshot().underruns == 1u);

    Block in0(kChannels, kFrames, 1.0f);
    CHECK(fixture.host.pushInput(in0.view(), 0u) == PushOutcome::Published);

    // Block 0 is in flight but not yet processed, so there is still nothing to read.
    CHECK(fixture.host.popOutput(out.view()).outcome == PopOutcome::Empty);

    Block work(kChannels, kFrames, 0.0f);
    const PopResult taken = fixture.helper.popInput(work.view());
    REQUIRE(taken.outcome == PopOutcome::Consumed);
    CHECK(taken.block == 0u);
    CHECK(work.allEqual(1.0f));
    CHECK(fixture.helper.pushOutput(work.view(), taken.origin) == PushOutcome::Published);

    // Only now does the host get a result - for the block before this one.
    const PopResult delivered = fixture.host.popOutput(out.view());
    REQUIRE(delivered.outcome == PopOutcome::Consumed);
    CHECK(delivered.block == 0u);
    CHECK(delivered.origin == 0u);
    CHECK(delivered.frames == static_cast<std::uint32_t>(kFrames));
    CHECK(out.allEqual(1.0f));
}

TEST_CASE("A round trip through the arena equals the in-process result one block later",
          "[plugin][sandbox]") {
    // Phase 3's acceptance criterion ("audio equals the in-process result within the
    // compensated block") proved at phase-1 level: the helper here is a loop in this
    // process applying gain 2.0, and the assertion is that the samples are
    // bit-identical to what the same gain applied in place would have produced.
    Fixture fixture{};
    Block input(kChannels, kFrames, 0.0f);
    Block work(kChannels, kFrames, 0.0f);
    Block output(kChannels, kFrames, 0.0f);

    constexpr int kBlocks = 64;
    int delivered = 0;
    for (int block = 0; block < kBlocks; ++block) {
        input.fill(static_cast<float>(block) * 0.001f);

        const BlockExchange exchange = fixture.host.exchange(static_cast<std::uint64_t>(block),
                                                             input.view(), output.view());
        CHECK(exchange.pushed == PushOutcome::Published);
        if (exchange.popped.outcome == PopOutcome::Consumed) {
            // Exactly one block behind, and the value that arrives is the gain
            // applied to the block the host published on the previous iteration.
            INFO("host block " << block << " delivered origin " << exchange.popped.origin);
            CHECK(exchange.popped.origin == static_cast<std::uint64_t>(block - 1));
            const float expected = static_cast<float>(exchange.popped.origin) * 0.001f * 2.0f;
            INFO("samples that differ: " << output.countDifferent(expected));
            CHECK(output.allEqual(expected));
            ++delivered;
        }

        const PopResult taken = fixture.helper.popInput(work.view());
        REQUIRE(taken.outcome == PopOutcome::Consumed);
        applyGain(work.view(), 2.0f);
        // The helper echoes the origin it was given, which is what lets the host
        // correlate a result with the block it sent.
        CHECK(fixture.helper.pushOutput(work.view(), taken.origin) == PushOutcome::Published);
    }

    // The last block is still in the output ring when the host's loop ends: there is
    // no helper work left to do, only a result to collect.
    const PopResult last = fixture.host.popOutput(output.view());
    REQUIRE(last.outcome == PopOutcome::Consumed);
    CHECK(last.origin == static_cast<std::uint64_t>(kBlocks - 1));
    CHECK(output.allEqual(static_cast<float>(kBlocks - 1) * 0.001f * 2.0f));

    CHECK(delivered == kBlocks - 1);
    const auto hostStats = fixture.host.stats().snapshot();
    CHECK(hostStats.blocksLost == 0u);
    CHECK(hostStats.tornReads == 0u);
    CHECK(hostStats.overruns == 0u);
    CHECK(hostStats.underruns == 1u); // the first callback, before anything exists
}

TEST_CASE("A peer that never consumes costs blocks, not a stall", "[plugin][sandbox]") {
    // Decision 2 in one case: the audio thread's work is bounded whether or not
    // anybody is at the other end. Two slots means two blocks go in and every block
    // after that is *refused* instead of waited on.
    Fixture fixture{};
    Block input(kChannels, kFrames, 0.5f);
    Block output(kChannels, kFrames, 0.0f);

    CHECK(fixture.host.pushInput(input.view(), 0u) == PushOutcome::Published);
    CHECK(fixture.host.pushInput(input.view(), 0u) == PushOutcome::Published);
    for (int block = 2; block < 100; ++block)
        CHECK(fixture.host.pushInput(input.view(), 0u) == PushOutcome::RingFull);

    for (int block = 0; block < 100; ++block)
        CHECK(fixture.host.popOutput(output.view()).outcome == PopOutcome::Empty);

    const auto stats = fixture.host.stats().snapshot();
    CHECK(stats.overruns == 98u);
    CHECK(stats.underruns == 100u);
    CHECK(stats.blocksLost == 0u);
    CHECK(stats.tornReads == 0u);

    // The helper side sees the same discipline from the other direction.
    CHECK(fixture.helper.pushOutput(input.view(), 0u) == PushOutcome::Published);
    CHECK(fixture.helper.pushOutput(input.view(), 0u) == PushOutcome::Published);
    CHECK(fixture.helper.pushOutput(input.view(), 0u) == PushOutcome::RingFull);
}

TEST_CASE("A peer whose counter jumped is caught, and the ring resynchronises itself",
          "[plugin][sandbox]") {
    // A crashed or hostile peer can leave the arena holding any bytes at all, and
    // `published` far ahead of `consumed` is the shape that matters: it claims blocks
    // exist that were overwritten before anybody read them. Two independent defences
    // have to fire - the consumer skips to the oldest block that can still exist and
    // says how many were lost, and the per-slot sequence refuses a slot whose stamp
    // does not match the block it is being read for. What must NOT happen is a wedge:
    // a session that has to be restarted because a helper died once is the outcome
    // the whole milestone exists to avoid.
    ArenaSpec spec{};
    spec.maxBlockSize = 128;
    Fixture fixture(spec);
    auto* publishedCounter = reinterpret_cast<std::atomic<std::uint64_t>*>(
        fixture.arena.bytes() + fixture.arena.layout().inputRing);
    auto slotAt = [&](std::uint64_t block) {
        return reinterpret_cast<AudioSlotHeader*>(
            fixture.arena.bytes() + fixture.arena.layout().inputRing + sizeof(PodRingHeader) +
            static_cast<std::size_t>(block % 2u) * fixture.arena.layout().audioSlotStride);
    };

    Block input(kChannels, kFrames, 0.75f);
    REQUIRE(fixture.host.pushInput(input.view(), 0u) == PushOutcome::Published);
    REQUIRE(fixture.host.pushInput(input.view(), 1u) == PushOutcome::Published);

    // The jump: nine blocks claimed, two of which can still exist.
    publishedCounter->store(9u, std::memory_order_release);
    Block work(kChannels, kFrames, 0.0f);
    const PopResult jumped = fixture.helper.popInput(work.view());
    CHECK(jumped.blocksLost == 7u); // blocks 0..6 were claimed and lost
    CHECK(jumped.block == 7u);      // the oldest that could still exist
    CHECK(jumped.outcome == PopOutcome::Torn); // slot 1 still carries block 1's stamp
    CHECK(fixture.helper.stats().snapshot().blocksLost == 7u);

    // A peer that jumps the counter AND stamps the slot consistently is served: this
    // is the skip arithmetic on its own, isolated from the sequence check.
    AudioSlotHeader* slot = slotAt(9u);
    slot->frames = static_cast<std::uint32_t>(kFrames);
    slot->channels = static_cast<std::uint32_t>(kChannels);
    slot->origin = 9u;
    slot->sequence.store(slotStableSequence(9u), std::memory_order_release);
    publishedCounter->store(11u, std::memory_order_release);

    const PopResult skipped = fixture.helper.popInput(work.view());
    CHECK(skipped.outcome == PopOutcome::Consumed);
    CHECK(skipped.block == 9u);
    CHECK(skipped.origin == 9u);
    CHECK(skipped.blocksLost == 1u);
    CHECK(work.allEqual(0.75f));

    // Resynchronisation costs at most `slots` torn reads: the counters are out of
    // step with the stamps the jump left behind, and each torn read advances one.
    // Nobody has to rebuild the transport, which is the property that matters.
    REQUIRE(fixture.host.pushInput(input.view(), 11u) == PushOutcome::Published);
    const PopResult stale = fixture.helper.popInput(work.view());
    CHECK(stale.outcome == PopOutcome::Torn);
    const PopResult resynced = fixture.helper.popInput(work.view());
    CHECK(resynced.outcome == PopOutcome::Consumed);
    CHECK(resynced.origin == 11u);
    CHECK(work.allEqual(0.75f));

    // And reset() gives a clean ring, which is what the supervisor calls when it
    // restarts a helper.
    fixture.host.reset();
    fixture.helper.reset();
    CHECK(fixture.host.stats().snapshot().tornReads == 0u);
    REQUIRE(fixture.host.pushInput(input.view(), 0u) == PushOutcome::Published);
    const PopResult fresh = fixture.helper.popInput(work.view());
    CHECK(fresh.outcome == PopOutcome::Consumed);
    CHECK(fresh.block == 0u);
    CHECK(fresh.origin == 0u);
}

TEST_CASE("A slot that is not stable is reported torn and released", "[plugin][sandbox]") {
    Fixture fixture{};
    Block input(kChannels, kFrames, 0.125f);
    REQUIRE(fixture.host.pushInput(input.view(), 0u) == PushOutcome::Published);

    // The state a peer leaves behind when it starts overwriting a slot mid-read: the
    // sequence says "being written", not "written".
    auto* slot = reinterpret_cast<AudioSlotHeader*>(fixture.arena.bytes() +
                                                    fixture.arena.layout().inputRing +
                                                    sizeof(PodRingHeader));
    slot->sequence.store(slotWritingSequence(0), std::memory_order_release);

    Block work(kChannels, kFrames, 0.0f);
    const PopResult torn = fixture.helper.popInput(work.view());
    CHECK(torn.outcome == PopOutcome::Torn);
    CHECK(fixture.helper.stats().snapshot().tornReads == 1u);

    // The slot was released, so the ring is not wedged: a fresh block flows on the
    // next round instead of the transport having to be rebuilt. Going dry for one
    // block and carrying on is Decision 3 step 2.
    REQUIRE(fixture.host.pushInput(input.view(), 1u) == PushOutcome::Published);
    const PopResult next = fixture.helper.popInput(work.view());
    CHECK(next.outcome == PopOutcome::Consumed);
    CHECK(next.origin == 1u);
    CHECK(work.allEqual(0.125f));
}

TEST_CASE("A block larger than the arena is refused rather than truncated",
          "[plugin][sandbox]") {
    Fixture fixture{};

    // Truncating would put the peer's channel count out of step with the audio it
    // receives, which surfaces as a wrong-sounding mix rather than as an error.
    Block tooWide(kChannels + 1, kFrames, 0.5f);
    CHECK(fixture.host.pushInput(tooWide.view(), 0u) == PushOutcome::TooLarge);
    Block tooLong(kChannels, static_cast<int>(fixture.arena.spec().maxBlockSize) + 1, 0.5f);
    CHECK(fixture.host.pushInput(tooLong.view(), 0u) == PushOutcome::TooLarge);
    Block nothing(0, 0, 0.0f);
    CHECK(fixture.host.pushInput(nothing.view(), 0u) == PushOutcome::Empty);

    CHECK(fixture.host.stats().snapshot().oversizedBlocks == 2u);
    Block work(kChannels, kFrames, 0.0f);
    CHECK(fixture.helper.popInput(work.view()).outcome == PopOutcome::Empty);
}

TEST_CASE("The side that does not own a direction is refused", "[plugin][sandbox]") {
    // A ring with two writers is a corrupted ring, and the mistake is one word wide:
    // pushInput and pushOutput. Refusing and counting turns that bug into a number in
    // the diagnostics instead of noise in the mix.
    Fixture fixture{};
    Block block(kChannels, kFrames, 0.5f);

    CHECK(fixture.helper.pushInput(block.view(), 0u) == PushOutcome::WrongSide);
    CHECK(fixture.helper.popOutput(block.view()).outcome == PopOutcome::WrongSide);
    CHECK(fixture.host.popInput(block.view()).outcome == PopOutcome::WrongSide);
    CHECK(fixture.host.pushOutput(block.view(), 0u) == PushOutcome::WrongSide);

    CHECK(fixture.host.stats().snapshot().wrongSideCalls == 2u);
    CHECK(fixture.helper.stats().snapshot().wrongSideCalls == 2u);

    // The correct directions still work afterwards, and a refused call published
    // nothing: the input ring is still empty, so the block the helper reads is the
    // one the host pushed just now.
    CHECK(fixture.host.pushInput(block.view(), 7u) == PushOutcome::Published);
    const PopResult helperTook = fixture.helper.popInput(block.view());
    REQUIRE(helperTook.outcome == PopOutcome::Consumed);
    CHECK(helperTook.origin == 7u);
    CHECK(fixture.helper.pushOutput(block.view(), helperTook.origin) == PushOutcome::Published);
    const PopResult hostTook = fixture.host.popOutput(block.view());
    REQUIRE(hostTook.outcome == PopOutcome::Consumed);
    CHECK(hostTook.origin == 7u);
}

// ---------------------------------------------------------------------------
// Record rings
// ---------------------------------------------------------------------------

TEST_CASE("Parameters and notes cross in order and drop rather than block",
          "[plugin][sandbox]") {
    Fixture fixture{};
    const std::uint32_t capacity = fixture.arena.spec().paramCapacity;

    for (std::uint32_t index = 0; index < capacity; ++index) {
        ParamRecord record{};
        record.paramId = static_cast<std::int32_t>(index);
        record.sampleOffset = static_cast<std::int32_t>(index % 64u);
        record.value = static_cast<float>(index) / 1000.0f;
        CHECK(fixture.host.pushParameter(record));
    }

    // A full ring refuses the write. It must not wait: the caller is the audio
    // thread, and a dropped parameter move is a glitch where a blocked one is a
    // dropout.
    ParamRecord overflow{};
    overflow.paramId = 9999;
    CHECK_FALSE(fixture.host.pushParameter(overflow));
    CHECK(fixture.host.parameters().dropped() == 1u);

    for (std::uint32_t index = 0; index < capacity; ++index) {
        ParamRecord out{};
        REQUIRE(fixture.helper.popParameter(out));
        INFO("record " << index);
        CHECK(out.paramId == static_cast<std::int32_t>(index));
        CHECK(out.sampleOffset == static_cast<std::int32_t>(index % 64u));
        CHECK(out.value == static_cast<float>(index) / 1000.0f);
    }
    ParamRecord drained{};
    CHECK_FALSE(fixture.helper.popParameter(drained));

    // Notes in both directions: host -> helper is the track's MIDI, helper -> host is
    // note output (an arpeggiator feeding the session), which M6 carried forward.
    const std::vector<midi::Message> sent = {
        midi::Message::noteOn(3, 60, 100),
        midi::Message::noteOff(3, 60),
        midi::Message::controlChange(0, 64, 127),
        midi::Message::pitchBend(1, 12345),
        midi::Message::programChange(2, 42),
    };
    for (const auto& message : sent)
        CHECK(fixture.host.pushNote(message));

    for (std::size_t index = 0; index < sent.size(); ++index) {
        midi::Message out{};
        REQUIRE(fixture.helper.popNoteIn(out));
        INFO("note " << index);
        CHECK(out.type == sent[index].type);
        CHECK(out.channel == sent[index].channel);
        CHECK(out.data1 == sent[index].data1);
        CHECK(out.data2 == sent[index].data2);
    }

    const midi::Message generated = midi::Message::noteOn(9, 72, 90);
    CHECK(fixture.helper.pushNoteOut(generated));
    midi::Message received{};
    REQUIRE(fixture.host.popNoteOut(received));
    CHECK(received.isNoteOn());
    CHECK(received.channel == 9);
    CHECK(received.data1 == 72);
    CHECK(received.data2 == 90);

    // A within-block offset survives the crossing, because sample-accurate
    // automation (M6 carry-over 5) is meant to be a helper-side change later, not a
    // wire-format change.
    midi::Message offset{};
    offset.type = midi::Message::Type::NoteOn;
    offset.sampleOffset = 37;
    CHECK(fixture.host.pushNote(offset));
    midi::Message offsetBack{};
    REQUIRE(fixture.helper.popNoteIn(offsetBack));
    CHECK(offsetBack.sampleOffset == 37);
}

TEST_CASE("A wire record that does not validate is refused, not cast into existence",
          "[plugin][sandbox]") {
    // Meta events are clip-internal (tempo, markers, text) and are never routed to a
    // plug-in, so there is no honest encoding for one and the conversion says so.
    midi::Message meta{};
    meta.type = midi::Message::Type::Meta;
    MidiRecord record{};
    CHECK_FALSE(toMidiRecord(meta, record));

    // A type byte from another process is data, not a promise. Accepting an
    // out-of-range enumerator would put every downstream switch on a value nobody
    // wrote a case for.
    MidiRecord corrupt{};
    corrupt.type = 200;
    midi::Message out{};
    CHECK_FALSE(toMidiMessage(corrupt, out));
    corrupt.type = static_cast<std::uint8_t>(midi::Message::Type::Meta);
    CHECK_FALSE(toMidiMessage(corrupt, out));

    // Every legal type does round trip.
    for (int type = 0; type <= static_cast<int>(midi::Message::Type::PitchBend); ++type) {
        midi::Message message{};
        message.type = static_cast<midi::Message::Type>(type);
        message.channel = 7;
        message.data1 = 11;
        message.data2 = 22;
        message.sampleOffset = 33;
        MidiRecord wire{};
        REQUIRE(toMidiRecord(message, wire));
        midi::Message back{};
        REQUIRE(toMidiMessage(wire, back));
        INFO("type " << type);
        CHECK(back.type == message.type);
        CHECK(back.channel == message.channel);
        CHECK(back.data1 == message.data1);
        CHECK(back.data2 == message.data2);
        CHECK(back.sampleOffset == message.sampleOffset);
    }

    // A rejected record is counted where the supervisor can see it, so a plug-in
    // being fed garbage is a diagnosis rather than a mystery.
    Fixture fixture{};
    CHECK_FALSE(fixture.host.pushNote(meta));
    CHECK(fixture.arena.telemetry()->rejectedRecords.load() == 1u);
}

// ---------------------------------------------------------------------------
// Transport snapshot
// ---------------------------------------------------------------------------

TEST_CASE("The transport snapshot crosses without tearing", "[plugin][sandbox]") {
    Fixture fixture{};
    ControlBlock* control = fixture.arena.control();
    REQUIRE(control != nullptr);

    // Nothing published yet: a reader must fail rather than invent a snapshot. A
    // zeroed one would claim a 0 Hz sample rate and a 0-frame block, and a helper
    // that started first would divide by it.
    TransportSnapshot unread{};
    CHECK_FALSE(readTransportSnapshot(control, unread));
    CHECK(unread.positionSamples == 0);

    publishTransportSnapshot(control, selfCheckingSnapshot(4242));
    TransportSnapshot read{};
    REQUIRE(readTransportSnapshot(control, read));
    CHECK(read.positionSamples == 4242);
    CHECK(isSelfConsistent(read));

    // A writer running flat out against a reader. The assertion is not "no torn read
    // ever happened" - the retries exist precisely because they do - but "no torn
    // read was ever ACCEPTED", and that the reader's attempts are bounded so a peer
    // cannot make it spin.
    //
    // This was a bug: `inconsistent` was 1 in 112 026 reads on MSVC at /O2, and 0
    // everywhere else - every Linux configuration and every MSVC Debug build. The
    // payload was crossing as a plain struct assignment, which races with the
    // writer's plain struct write, and a data race is undefined behaviour: the
    // optimiser appears to have common-subexpression-eliminated the copy across the
    // retry loop below, so a payload torn on one attempt was reused on the next and
    // accepted under a version that matched. The snapshot now crosses word by word
    // through relaxed atomics (see ADR-0017 item 4). The case is unchanged, because
    // the case was right - it is the only reason this was ever found.
    std::atomic<bool> writerDone{false};
    std::atomic<std::int64_t> accepted{0};
    std::atomic<std::int64_t> refused{0};
    constexpr std::int64_t kWrites = 200000;

    std::thread writer([&] {
        for (std::int64_t n = 0; n < kWrites; ++n)
            publishTransportSnapshot(control, selfCheckingSnapshot(n));
        writerDone.store(true, std::memory_order_release);
    });

    TransportSnapshot last{};
    std::int64_t inconsistent = 0;
    while (!writerDone.load(std::memory_order_acquire) || accepted.load() < 1) {
        TransportSnapshot candidate{};
        if (readTransportSnapshot(control, candidate, fixture.arena.telemetry(), 3)) {
            if (!isSelfConsistent(candidate))
                ++inconsistent;
            last = candidate;
            accepted.fetch_add(1, std::memory_order_relaxed);
        } else {
            refused.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    writer.join();

    INFO("accepted " << accepted.load() << ", refused " << refused.load() << ", torn "
                     << fixture.arena.telemetry()->tornReads.load());
    CHECK(inconsistent == 0);
    CHECK(accepted.load() > 0);
    CHECK(isSelfConsistent(last));

    // Once the writer has stopped, a single attempt is enough: the retries are for a
    // live writer, not a permanent tax.
    TransportSnapshot settled{};
    REQUIRE(readTransportSnapshot(control, settled, nullptr, 0));
    CHECK(settled.positionSamples == kWrites - 1);
    CHECK(isSelfConsistent(settled));
}

// ---------------------------------------------------------------------------
// Real-time discipline
// ---------------------------------------------------------------------------

TEST_CASE("The transport allocates nothing once it is attached",
          "[plugin][sandbox][realtime][regression]") {
    Fixture fixture{};
    Block input(kChannels, kFrames, 0.0f);
    Block work(kChannels, kFrames, 0.0f);
    Block output(kChannels, kFrames, 0.0f);
    TransportSnapshot snapshot = selfCheckingSnapshot(0);
    ParamRecord parameter{};
    const midi::Message note = midi::Message::noteOn(0, 60, 100);
    Telemetry* telemetry = fixture.arena.telemetry();

    int snapshotReads = 0;

    // Warm up first: a first-call allocation is a lazy path, not an RT bug, and
    // measuring it would make this a test of the warm-up. The allocation counter
    // only counts in a build with -DAURA_ENABLE_ALLOC_TRACKING=ON; without it the
    // violation counter and the static audit carry the claim, and the loop still
    // proves the path runs 256 blocks without a stall.
    for (int block = 0; block < 16; ++block) {
        input.fill(static_cast<float>(block));
        fixture.host.exchange(static_cast<std::uint64_t>(block), input.view(), output.view());
        publishTransportSnapshot(fixture.arena.control(), snapshot);
        TransportSnapshot ignored{};
        if (readTransportSnapshot(fixture.arena.control(), ignored, telemetry))
            ++snapshotReads;
        fixture.host.pushParameter(parameter);
        fixture.host.pushNote(note);
        const PopResult warm = fixture.helper.popInput(work.view());
        if (warm.outcome == PopOutcome::Consumed)
            fixture.helper.pushOutput(work.view(), warm.origin);
        midi::Message ignoredNote{};
        fixture.helper.popNoteIn(ignoredNote);
        fixture.host.popNoteOut(ignoredNote);
        ParamRecord ignoredParameter{};
        fixture.helper.popParameter(ignoredParameter);
    }

    // No Catch2 assertions inside the measured region: the assertion machinery is
    // not something to run under an allocation counter, so failures are accumulated
    // here and asserted after tracking is switched off.
    int unexpectedPushes = 0;
    rt::setTrackingEnabled(true);
    rt::resetRealtimeAllocationCount();
    rt::enterRealtimeThread();
    {
        rt::ScopedNoDenormals noDenormals;
        for (int block = 0; block < 256; ++block) {
            input.fill(static_cast<float>(block) * 0.01f);
            const BlockExchange exchange =
                fixture.host.exchange(static_cast<std::uint64_t>(block), input.view(),
                                      output.view());
            if (exchange.pushed != PushOutcome::Published)
                ++unexpectedPushes;
            snapshot.positionSamples = block;
            publishTransportSnapshot(fixture.arena.control(), snapshot);
            TransportSnapshot read{};
            if (readTransportSnapshot(fixture.arena.control(), read, telemetry))
                ++snapshotReads;
            parameter.paramId = block % 8;
            fixture.host.pushParameter(parameter);
            fixture.host.pushNote(note);
            const PopResult measured = fixture.helper.popInput(work.view());
            if (measured.outcome == PopOutcome::Consumed)
                fixture.helper.pushOutput(work.view(), measured.origin);
            midi::Message ignoredNote{};
            fixture.helper.popNoteIn(ignoredNote);
            fixture.host.popNoteOut(ignoredNote);
            ParamRecord ignoredParameter{};
            fixture.helper.popParameter(ignoredParameter);
            telemetry->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    rt::exitRealtimeThread();

    rt::setTrackingEnabled(false);

    INFO("allocations on the transport path: " << rt::realtimeAllocationCount());
    INFO("RT violations recorded: " << rt::violationCount());
    CHECK(rt::realtimeAllocationCount() == 0u);
    CHECK(rt::violationCount() == 0u);
    CHECK(telemetry->blocksProcessed.load() >= 256u);
    CHECK(unexpectedPushes == 0);
    CHECK(snapshotReads == 16 + 256);
}

TEST_CASE("Two threads run the deferred pipeline without losing or tearing a block",
          "[plugin][sandbox][realtime]") {
    // The pipeline's real shape: a host thread and a helper thread doing work per
    // block, over the same bytes. Both sides wait for room rather than dropping, the
    // way a hardware-paced callback and a helper with a whole block period to work in
    // actually behave; the dropping path is covered deterministically by "a peer that
    // never consumes". What must hold is that every block which arrives is intact, in
    // increasing origin order, and never from the future - a transport that loses or
    // mixes blocks quietly is worse than one that reports losing them.
    ArenaSpec spec{};
    spec.maxBlockSize = 128;
    spec.maxChannels = 2;
    Fixture fixture(spec);

    constexpr int kHostBlocks = 60000;
    constexpr int kHelperFrames = 128;
    constexpr float kGain = 2.0f;
    std::atomic<bool> hostDone{false};
    std::atomic<std::int64_t> helperBlocks{0};
    std::atomic<std::int64_t> inconsistent{0};
    std::atomic<std::int64_t> outOfOrder{0};
    std::atomic<std::int64_t> fromTheFuture{0};
    std::atomic<std::int64_t> delivered{0};
    std::atomic<std::int64_t> maxStaleness{0};
    std::atomic<std::int64_t> helperPushWaits{0};

    std::thread helperThread([&] {
        Block in(kChannels, kHelperFrames, 0.0f);
        Block out(kChannels, kHelperFrames, 0.0f);

        auto processOne = [&]() {
            const PopResult popped = fixture.helper.popInput(in.view());
            if (popped.outcome != PopOutcome::Consumed) {
                if (popped.outcome == PopOutcome::Torn)
                    inconsistent.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            for (int channel = 0; channel < kChannels; ++channel)
                for (int frame = 0; frame < kHelperFrames; ++frame)
                    out.view().channelPointers[channel][frame] =
                        in.view().channelPointers[channel][frame] * kGain;
            // Wait for room instead of dropping, so an output that was produced is an
            // output that arrives: the alternative would make "did the transport lose
            // anything" unanswerable from inside this test. Bounded by `hostDone`,
            // because a helper that spins forever on a host which has stopped
            // consuming is the same bug from the other side - and the real helper has
            // to answer it too (a dead host must cost the helper its life, not hang
            // it), which is the supervisor's kill in phase 2.
            while (fixture.helper.pushOutput(out.view(), popped.origin) !=
                       PushOutcome::Published &&
                   !hostDone.load(std::memory_order_acquire)) {
                helperPushWaits.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            helperBlocks.fetch_add(1, std::memory_order_relaxed);
            return true;
        };

        for (;;) {
            if (processOne())
                continue;
            if (hostDone.load(std::memory_order_acquire)) {
                // One more attempt after observing `done`: the host may have published
                // between the failed pop and the load.
                if (!processOne())
                    break;
                continue;
            }
            std::this_thread::yield();
        }
    });

    {
        Block input(kChannels, kHelperFrames, 0.0f);
        Block output(kChannels, kHelperFrames, 0.0f);
        std::int64_t lastOrigin = -1;
        for (int block = 0; block < kHostBlocks; ++block) {
            input.fill(static_cast<float>(block));

            const PopResult popped = fixture.host.popOutput(output.view());
            if (popped.outcome == PopOutcome::Consumed) {
                const std::int64_t origin = static_cast<std::int64_t>(popped.origin);
                const float expected = static_cast<float>(origin) * kGain;
                const std::size_t wrong = output.countDifferent(expected);
                if (wrong > 0u) {
                    INFO("origin " << origin << " arrived with " << wrong << " of "
                                   << (kChannels * kHelperFrames) << " samples wrong");
                    inconsistent.fetch_add(1, std::memory_order_relaxed);
                }
                if (origin <= lastOrigin)
                    outOfOrder.fetch_add(1, std::memory_order_relaxed);
                if (origin > block)
                    fromTheFuture.fetch_add(1, std::memory_order_relaxed);
                const std::int64_t staleness = static_cast<std::int64_t>(block) - origin - 1;
                if (staleness > maxStaleness.load(std::memory_order_relaxed))
                    maxStaleness.store(staleness, std::memory_order_relaxed);
                lastOrigin = origin;
                delivered.fetch_add(1, std::memory_order_relaxed);
            }

            while (fixture.host.pushInput(input.view(), static_cast<std::uint64_t>(block)) !=
                   PushOutcome::Published)
                std::this_thread::yield();
        }
        hostDone.store(true, std::memory_order_release);
    }
    helperThread.join();

    const auto hostStats = fixture.host.stats().snapshot();
    const auto helperStats = fixture.helper.stats().snapshot();
    INFO("delivered " << delivered.load() << " of " << kHostBlocks);
    INFO("helper processed " << helperBlocks.load() << ", waited for room "
                             << helperPushWaits.load());
    INFO("max staleness beyond the promised block: " << maxStaleness.load());
    INFO("host underruns " << hostStats.underruns << ", helper underruns "
                           << helperStats.underruns);

    // Nothing torn, nothing out of order, nothing from the future, nothing silently
    // lost. `overruns` is deliberately NOT asserted at zero: it counts the number of
    // pushes the ring refused, and a side that retries until it is accepted racks
    // those up by design. What proves nothing was dropped is that every block the
    // host published came back processed exactly once - see below. A real host
    // cannot retry (the callback has to return), so for it one refusal is one
    // dropped block, which is why the counter exists at all.
    INFO("host refusals " << hostStats.overruns << ", helper refusals "
                          << helperStats.overruns);
    CHECK(inconsistent.load() == 0);
    CHECK(outOfOrder.load() == 0);
    CHECK(fromTheFuture.load() == 0);
    CHECK(hostStats.blocksLost == 0u);
    CHECK(hostStats.tornReads == 0u);
    CHECK(helperStats.blocksLost == 0u);
    CHECK(helperStats.tornReads == 0u);
    // Every block the host published was processed exactly once: the host retried
    // until each one was accepted and the helper drained before it exited. How many
    // results came back is a function of how the two threads interleaved, so it is
    // reported rather than pinned - but it must be nearly all of them, or the
    // pipeline is losing audio somewhere this test cannot see.
    CHECK(helperBlocks.load() == kHostBlocks);
    INFO("delivered fraction: " << (static_cast<double>(delivered.load()) / kHostBlocks));
    CHECK(delivered.load() > kHostBlocks - kHostBlocks / 100);
}
