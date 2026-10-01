// ============================================================================
// AURA DAW - tests/plugin/SandboxTransportTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// M7's transport, tested before anything is built on top of it.
//
// The sandbox's whole promise is that a plug-in can run in another process without
// the audio thread ever waiting for it. That promise lives in two places: the
// ordering rules in SandboxArena.hpp and the named region in SharedMemory.cpp. Both
// are exercised here with *real* shared memory - a second mapping of the same object,
// which is exactly what the helper will do - rather than with two structs in one
// address space pretending to be processes.
//
// What is deliberately NOT tested here: a second *process*. That needs the helper
// binary, and it arrives with it (phase 2 of the plan in
// docs/research/PLUGIN_SANDBOX_RESEARCH.md); this file proves the protocol, the
// memory and the RT-safety, and the helper's own test will prove the boundary.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "SandboxArena.hpp"
#include "SharedMemory.hpp"
#include "aura/core/Realtime.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

constexpr std::size_t kFrames = 64;
constexpr std::size_t kChannels = 2;

/// A region with a constructed arena in it, plus the housekeeping to clean up.
/// The fixture mirrors what the host will do: create, construct, hand the name out.
class ArenaFixture {
public:
    explicit ArenaFixture(std::size_t frames = kFrames, std::size_t channels = kChannels)
        : region_(SharedRegion::create(makeRegionName("aura-test-arena"), arenaBytes())) {
        REQUIRE(region_.valid());
        arena_ = constructArena(region_.data());
        REQUIRE(arena_ != nullptr);
        arena_->blockSize = static_cast<std::uint32_t>(frames);
        arena_->channels = static_cast<std::uint32_t>(channels);
    }

    ~ArenaFixture() { region_.unlink(); }

    ArenaFixture(const ArenaFixture&) = delete;
    ArenaFixture& operator=(const ArenaFixture&) = delete;

    /// The helper's side: a second mapping of the same object, attached the same way
    /// the real helper will attach.
    [[nodiscard]] SharedRegion attach() const {
        std::string error;
        SharedRegion view = SharedRegion::open(region_.name(), arenaBytes(), error);
        REQUIRE(view.valid());
        CHECK(error.empty());
        return view;
    }

    [[nodiscard]] SandboxArena& arena() noexcept { return *arena_; }
    [[nodiscard]] const std::string& name() const noexcept { return region_.name(); }

private:
    SharedRegion region_;
    SandboxArena* arena_ = nullptr;
};

/// Fills a channel with a recognisable ramp, so a stale or misplaced copy is
/// visible instead of plausible.
void fillRamp(float* data, std::size_t frames, float base) {
    for (std::size_t index = 0; index < frames; ++index)
        data[index] = base + static_cast<float>(index);
}

bool isRamp(const float* data, std::size_t frames, float base) {
    for (std::size_t index = 0; index < frames; ++index) {
        if (data[index] != base + static_cast<float>(index))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("The arena layout is the ABI both processes agree on", "[plugin][sandbox]") {
    // These numbers are the contract between the host and a helper built by any AURA
    // version that shares this format version. Changing one is a deliberate act, in
    // the same commit as the version bump - a helper that misreads the arena would be
    // far worse than one that refuses it.
    CHECK(kArenaVersion == 1u);
    CHECK(kAudioSlots == 2u);
    CHECK(sizeof(EventRecord) == 16u);
    CHECK(offsetof(EventRecord, sampleOffset) == 4u);
    CHECK(offsetof(EventRecord, timestampTicks) == 8u);
    CHECK(sizeof(ParamRecord) == 12u);
    CHECK(arenaBytes() > 0);
    CHECK(arenaBytes() < 8u * 1024u * 1024u); // modest PCs are a design constraint

    // The slot geometry has to cover the largest block the engine offers.
    CHECK(kMaxBlockSize >= 2048u);
    CHECK(kMaxChannels >= 2u);

    // Every field the helper reads at a fixed offset must be lock-free, or the
    // "never waits" promise is `std::atomic` waiting on a mutex inside shared memory.
    CHECK(std::atomic<std::uint64_t>::is_always_lock_free);
    CHECK(std::atomic<std::uint32_t>::is_always_lock_free);
    CHECK(std::atomic<std::int32_t>::is_always_lock_free);
}

TEST_CASE("Shared memory is created, attached by a second view and cleaned up",
          "[plugin][sandbox]") {
    ArenaFixture fixture;
    fixture.arena().mailbox.reportedLatencySamples.store(64);

    auto helperView = fixture.attach();
    auto* attached = attachArena(helperView.data());
    REQUIRE(attached != nullptr);
    REQUIRE(attached != &fixture.arena()); // a different mapping of the same bytes
    CHECK(attached->valid());
    CHECK(attached->blockSize == kFrames);

    // Writes cross in both directions, which is the entire point of the region.
    attached->mailbox.tailSamples.store(128);
    CHECK(fixture.arena().mailbox.tailSamples.load() == 128);

    // A helper that does not understand the layout refuses it rather than misreading
    // it: that check is the first thing a helper does after mapping.
    attached->version = kArenaVersion + 1;
    CHECK(attachArena(helperView.data()) == nullptr);
    attached->version = kArenaVersion;
    CHECK(attachArena(helperView.data()) != nullptr);
}

TEST_CASE("Two regions never collide", "[plugin][sandbox]") {
    ArenaFixture first;
    ArenaFixture second;
    CHECK(first.name() != second.name());
    CHECK(first.arena().blockSize == second.arena().blockSize);
    // Distinct objects, not one shared by accident.
    first.arena().channels = 1;
    CHECK(second.arena().channels == kChannels);
}

TEST_CASE("The helper answers one block behind, and the host keeps its order",
          "[plugin][sandbox]") {
    ArenaFixture fixture;
    auto helperView = fixture.attach();
    SandboxArena& hostArena = fixture.arena();
    SandboxArena& helperArena = *attachArena(helperView.data());
    REQUIRE(&hostArena != &helperArena);

    HostAudioPort host(hostArena);
    HelperAudioPort helper(helperArena);

    std::vector<float> left(kFrames), right(kFrames);
    float* channels[2] = {left.data(), right.data()};

    // Block 0: published, but nothing can come back yet.
    fillRamp(left.data(), kFrames, 0.0f);
    fillRamp(right.data(), kFrames, 1000.0f);
    CHECK(host.publishAudio(channels, 2, kFrames) == BlockOutcome::NothingYet);

    // The helper picks it up and does the work a plug-in would: gain 2x, in place.
    AudioSlot* slot = helper.acquireBlock();
    REQUIRE(slot != nullptr);
    CHECK(slot->frames == kFrames);
    CHECK(slot->channels == kChannels);
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        float* data = slot->channel(channel);
        for (std::size_t index = 0; index < kFrames; ++index)
            data[index] *= 2.0f;
    }
    helper.publishResult();

    // Block 1: the host gets block 0's *processed* audio, while block 1 goes the
    // other way. That is the one-block pipeline, visible in the numbers.
    fillRamp(left.data(), kFrames, 0.0f); // fresh input for block 1
    fillRamp(right.data(), kFrames, 1000.0f);
    CHECK(host.publishAudio(channels, 2, kFrames) == BlockOutcome::Processed);
    for (std::size_t index = 0; index < kFrames; ++index) {
        CHECK(left[index] == 2.0f * static_cast<float>(index));
        CHECK(right[index] == 2.0f * (1000.0f + static_cast<float>(index)));
    }
    CHECK(host.droppedBlocks() == 0);
    CHECK(helperArena.mailbox.processedBlocks.load() == 1);
}

TEST_CASE("A helper that is late costs output, never time", "[plugin][sandbox]") {
    ArenaFixture fixture;
    SandboxArena& arena = fixture.arena();
    HostAudioPort host(arena);
    HelperAudioPort helper(arena);

    std::vector<float> left(kFrames), right(kFrames);
    float* channels[2] = {left.data(), right.data()};

    fillRamp(left.data(), kFrames, 0.0f);
    fillRamp(right.data(), kFrames, 0.0f);
    CHECK(host.publishAudio(channels, 2, kFrames) == BlockOutcome::NothingYet);

    // The helper never runs. The host must not wait for it - the second block is
    // reported as dropped, the audio is left exactly as the engine wrote it (dry, not
    // silence: the caller decides what a dropped sandbox means), and the count is what
    // the UI will show as a performance warning.
    fillRamp(left.data(), kFrames, 500.0f);
    fillRamp(right.data(), kFrames, 500.0f);
    CHECK(host.publishAudio(channels, 2, kFrames) == BlockOutcome::HelperLate);
    CHECK(isRamp(left.data(), kFrames, 500.0f));
    CHECK(host.droppedBlocks() == 1);

    // The helper catches up, and the host carries on from where it was - no block was
    // silently skipped in the sequence the helper sees.
    AudioSlot* slot = helper.acquireBlock();
    REQUIRE(slot != nullptr);
    helper.publishResult();

    fillRamp(left.data(), kFrames, 0.0f);
    fillRamp(right.data(), kFrames, 0.0f);
    CHECK(host.publishAudio(channels, 2, kFrames) == BlockOutcome::Processed);
    CHECK(host.droppedBlocks() == 1);
}

TEST_CASE("A helper that fell behind skips what it missed and says so", "[plugin][sandbox]") {
    ArenaFixture fixture;
    SandboxArena& arena = fixture.arena();
    HelperAudioPort helper(arena);

    // The host's own path cannot let the sequence jump (it drops instead of running
    // ahead), so this drives the helper's safety net directly: if the upload sequence
    // ever advances by more than one - a future three-slot ring, or a host that chose
    // to keep the newest - the helper processes the newest block and *reports* the
    // ones it never saw, rather than pretending it processed them.
    // Give the two slots different contents, so "it took the newest" is something the
    // test can see rather than a field that happens to be zero.
    arena.audio[0].sequence = 0; // block 1
    arena.audio[1].sequence = 3; // block 4
    arena.uploadSequence.store(1, std::memory_order_release);
    CHECK(helper.acquireBlock() != nullptr);

    arena.uploadSequence.store(4, std::memory_order_release);
    AudioSlot* newest = helper.acquireBlock();
    REQUIRE(newest != nullptr);
    CHECK(newest->sequence == 3); // block 4's slot: the newest, not the one it just missed
    CHECK(helper.skippedBlocks() == 2);
    // Taking a block is not answering it: the host's watermark only moves on publish.
    CHECK(arena.helperSequence.load() == 0);
}

TEST_CASE("Every block comes back exactly once, exactly one block later", "[plugin][sandbox]") {
    ArenaFixture fixture;
    SandboxArena& arena = fixture.arena();
    HostAudioPort host(arena);
    HelperAudioPort helper(arena);

    std::vector<float> left(kFrames), right(kFrames);
    float* channels[2] = {left.data(), right.data()};

    // The helper here is prompt: it finishes each block before the host hands over the
    // next one. What is being pinned down is the invariant the graph's delay
    // compensation is built on - block k's processed audio arrives on call k + 1, never
    // on call k, never on call k + 2. (A real helper thread can also finish a block
    // *inside* the host's call; the host reads its watermark before the hand-over so
    // that this cannot shift the answer by a block.)
    constexpr int kBlocks = 8;
    for (int block = 0; block < kBlocks; ++block) {
        const float base = static_cast<float>(block) * 100.0f;
        std::fill(left.begin(), left.end(), base);
        std::fill(right.begin(), right.end(), base + 1.0f);

        const BlockOutcome outcome = host.publishAudio(channels, 2, kFrames);
        if (block == 0) {
            CHECK(outcome == BlockOutcome::NothingYet); // the pipeline starts empty
        } else {
            REQUIRE(outcome == BlockOutcome::Processed);
            // What came back is the block before this one, as the plug-in left it:
            // the helper doubles every sample.
            CHECK(left[0] == 2.0f * static_cast<float>(block - 1) * 100.0f);
            CHECK(right[0] == 2.0f * (static_cast<float>(block - 1) * 100.0f + 1.0f));
        }

        AudioSlot* slot = helper.acquireBlock();
        REQUIRE(slot != nullptr);
        CHECK(slot->sequence == static_cast<std::uint64_t>(block));
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            float* data = slot->channel(channel);
            for (std::size_t index = 0; index < kFrames; ++index)
                data[index] *= 2.0f;
        }
        helper.publishResult();
    }

    CHECK(host.droppedBlocks() == 0); // the helper never made the host miss a block
    CHECK(helper.skippedBlocks() == 0); // and never missed one itself
    CHECK(arena.mailbox.processedBlocks.load() == static_cast<std::uint32_t>(kBlocks));
}

TEST_CASE("The host refuses geometry the arena was not built for", "[plugin][sandbox]") {
    ArenaFixture fixture(kFrames, kChannels);
    SandboxArena& arena = fixture.arena();
    HostAudioPort host(arena);

    std::vector<float> left(kMaxBlockSize + 1), right(kMaxBlockSize + 1);
    float* channels[2] = {left.data(), right.data()};

    CHECK(host.publishAudio(nullptr, 2, kFrames) == BlockOutcome::GeometryGone);
    CHECK(host.publishAudio(channels, 2, 0) == BlockOutcome::GeometryGone);
    CHECK(host.publishAudio(channels, kMaxChannels + 1, kFrames) ==
          BlockOutcome::GeometryGone);
    CHECK(host.publishAudio(channels, 2, kMaxBlockSize + 1) == BlockOutcome::GeometryGone);
    // A mono track for a stereo arena is fine: the extra channel is sent silent rather
    // than read from a pointer that is not there.
    CHECK(host.publishAudio(channels, 1, kFrames) == BlockOutcome::NothingYet);
}

TEST_CASE("Notes and parameter changes cross in order, and overflow is refused",
          "[plugin][sandbox]") {
    ArenaFixture fixture;
    SandboxArena& arena = fixture.arena();

    // Notes: the host queues what the transport generated, the helper reads them in
    // the order they were played - order is the whole point of a note queue.
    for (std::uint32_t index = 0; index < 32; ++index) {
        EventRecord event;
        event.type = 1; // note on
        event.channel = 0;
        event.data1 = static_cast<std::uint8_t>(60 + index);
        event.data2 = 100;
        event.sampleOffset = static_cast<std::int32_t>(index);
        REQUIRE(arena.eventsToHelper.tryPush(event));
    }
    CHECK(arena.eventsToHelper.size() == 32);
    for (std::uint32_t index = 0; index < 32; ++index) {
        EventRecord read;
        REQUIRE(arena.eventsToHelper.tryPop(read));
        CHECK(read.data1 == 60 + index);
        CHECK(read.sampleOffset == static_cast<std::int32_t>(index));
    }
    CHECK(arena.eventsToHelper.empty());
    EventRecord unused;
    CHECK_FALSE(arena.eventsToHelper.tryPop(unused));

    // Parameter changes: normalised values with a sample offset, which is what the
    // plug-in's own mapping expects. Overflow is refused, never grown into.
    for (std::size_t index = 0; index < kMaxParamChanges; ++index) {
        ParamRecord change;
        change.id = 1000 + static_cast<std::uint32_t>(index);
        change.normalised = static_cast<float>(index) / static_cast<float>(kMaxParamChanges);
        REQUIRE(arena.paramsToHelper.tryPush(change));
    }
    ParamRecord overflow;
    overflow.id = 7;
    CHECK_FALSE(arena.paramsToHelper.tryPush(overflow)); // full: the caller counts the drop
    CHECK(arena.paramsToHelper.size() == kMaxParamChanges);

    // And a long run past the capacity: indices are masked, so the ring wraps without
    // either side noticing anything but its own counters.
    arena.eventsToHelper.reset();
    for (std::uint32_t round = 0; round < 5000; ++round) {
        EventRecord event;
        event.data1 = static_cast<std::uint8_t>(round % 128);
        REQUIRE(arena.eventsToHelper.tryPush(event));
        EventRecord read;
        REQUIRE(arena.eventsToHelper.tryPop(read));
        REQUIRE(read.data1 == event.data1);
    }
}

TEST_CASE("The transport does not allocate on the audio path", "[plugin][sandbox][realtime]") {
    ArenaFixture fixture;
    SandboxArena& arena = fixture.arena();
    HostAudioPort host(arena);

    std::vector<float> left(kFrames), right(kFrames);
    float* channels[2] = {left.data(), right.data()};
    fillRamp(left.data(), kFrames, 0.0f);

    // Warm-up: the first calls touch nothing lazy, but the same discipline the engine
    // test uses applies - measure the steady state, not the first block.
    for (int block = 0; block < 4; ++block)
        (void)host.publishAudio(channels, 2, kFrames);

    rt::setTrackingEnabled(true);
    rt::resetRealtimeAllocationCount();
    rt::enterRealtimeThread();
    for (int block = 0; block < 256; ++block) {
        (void)host.publishAudio(channels, 2, kFrames);
        EventRecord event;
        event.type = 1;
        (void)arena.eventsToHelper.tryPush(event);
        (void)arena.eventsToHelper.tryPop(event);
    }
    rt::exitRealtimeThread();
    const auto allocations = rt::realtimeAllocationCount();
    const auto violations = rt::violationCount();
    rt::setTrackingEnabled(false);

    INFO("allocations observed on the audio path: " << allocations);
    INFO("RT violations recorded: " << violations);
    CHECK(allocations == 0);
    CHECK(violations == 0);
}

TEST_CASE("A region that is not there is an error, not a crash", "[plugin][sandbox]") {
    std::string error;
    SharedRegion missing = SharedRegion::open("aura-does-not-exist-" + std::to_string(
                                                  std::uint64_t{0x5EED}),
                                              arenaBytes(), error);
    CHECK_FALSE(missing.valid());
    CHECK_FALSE(error.empty());
    CHECK(missing.data() == nullptr);
    // Unlinking and releasing nothing is a no-op, not undefined behaviour: the helper
    // calls both on paths where the region never existed.
    missing.unlink();
    missing.release();
}
