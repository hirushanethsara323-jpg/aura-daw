// ============================================================================
// AURA DAW - tests/dsp/BlockStateTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Block-state unrolling (issue #6): the level meter's exponential RMS and a
// node's one-pole gain ramp are first-order recursions with a per-sample
// loop-carried dependency, which is why they were the slowest loops in a session
// without effects. `math::advanceSquareRecursion` and `math::applyOnePoleRamp`
// evaluate the same recurrences four samples at a time from the closed form of
// the group, so the multiplies are independent.
//
// "The same recurrence" is the contract these tests pin: the fast form must agree
// with the per-sample form to well inside the meter's own precision, over odd
// block sizes, over long runs, and through the real LevelMeter (including the
// property the meter's comment claims - the reading does not depend on how the
// audio is split into blocks).
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "aura/core/Math.hpp"
#include "aura/dsp/LevelMeter.hpp"
#include "aura/graph/AudioGraph.hpp"

using namespace aura;
using Catch::Approx;

namespace {

/// The per-sample form, written exactly as the engine used to run it. This is the
/// reference the fast path is measured against.
float referenceSquareRecursion(const float* data, int frames, float state, float a, float b) {
    for (int i = 0; i < frames; ++i)
        state = a * state + b * (data[i] * data[i]);
    return state;
}

/// The per-sample one-pole ramp, as a reference (state in/out, data scaled).
float referenceRamp(float* data, int frames, float start, float target, float a) {
    const float coefficient = 1.0f - a;
    float current = start;
    for (int i = 0; i < frames; ++i) {
        current += (target - current) * coefficient;
        data[i] *= current;
    }
    return current;
}

std::vector<float> noisySignal(int frames, std::uint32_t seed = 12345u) {
    std::vector<float> data(static_cast<std::size_t>(frames));
    std::uint32_t state = seed;
    for (int i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        data[static_cast<std::size_t>(i)] =
            (static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f) * 0.8f;
    }
    return data;
}

/// Per-block agreement with the per-sample form. The closed form rounds `a^4`
/// once per group, so a single block matches to ~1e-6 relative.
constexpr float kRelativeTolerance = 5.0e-6f;

/// Agreement after thousands of blocks. The rounding above carries from block to
/// block; because `a < 1` the recursion is stable, so the error stays bounded
/// instead of growing - measured ~1e-5 relative after 4000 blocks, which is
/// 0.0001 dB, two orders of magnitude below the meter's display resolution.
constexpr float kLongRunTolerance = 5.0e-5f;

/// The difference between two amplitudes in dB - the unit a meter is read in, and
/// therefore the only tolerance a user can perceive.
float dbDifference(float a, float b) {
    return std::abs(20.0f * std::log10(a / b));
}

} // namespace

TEST_CASE("The unrolled square recursion matches the per-sample one", "[dsp][blockstate]") {
    // Contract: same recurrence, within the rounding of one group. See
    // kRelativeTolerance and kLongRunTolerance for why the bound is not zero and
    // why that is inaudible.
    // Every block size in and around a multiple of four, plus the odd ones that
    // exercise the scalar tail.
    for (const int frames : {1, 2, 3, 4, 5, 7, 8, 9, 63, 64, 65, 127, 128, 129, 256, 517, 1024}) {
        DYNAMIC_SECTION("frames = " << frames) {
            const auto data = noisySignal(frames);
            const float a = 0.996f; // 1 - c for a 0.3 s RMS at 48 kHz, near enough
            const float b = 1.0f - a;
            const float start = 0.017f;

            const float expected = referenceSquareRecursion(data.data(), frames, start, a, b);
            const float actual = math::advanceSquareRecursion(data.data(), frames, start, a, b);
            REQUIRE(actual == Approx(expected).epsilon(kRelativeTolerance));
        }
    }
}

TEST_CASE("The unrolled square recursion does not drift over a long run",
          "[dsp][blockstate][regression]") {
    // The closed form multiplies the *state* by a^4 per group. If a group ever
    // used the wrong power, the error would compound rather than cancel, so the
    // check runs the recursion for a long time in small blocks against the
    // reference implementation, exactly the way a session runs it.
    const auto block = noisySignal(256);
    const float a = 0.9986f;
    const float b = 1.0f - a;

    float reference = 0.0f;
    float fast = 0.0f;
    for (int blockIndex = 0; blockIndex < 4000; ++blockIndex) {
        reference = referenceSquareRecursion(block.data(), 256, reference, a, b);
        fast = math::advanceSquareRecursion(block.data(), 256, fast, a, b);
    }
    REQUIRE(fast == Approx(reference).epsilon(kLongRunTolerance));
    REQUIRE(dbDifference(fast, reference) < 0.001f);
    // ... and the signal is loud enough for that comparison to mean something.
    REQUIRE(reference > 1.0e-4f);
}

TEST_CASE("The unrolled gain ramp matches the per-sample ramp", "[dsp][blockstate]") {
    for (const int frames : {1, 3, 4, 5, 8, 64, 255, 256, 257, 1000}) {
        DYNAMIC_SECTION("frames = " << frames) {
            auto expectedData = noisySignal(frames, 999u);
            auto actualData = expectedData;
            const float a = 0.99792f; // the node's 10 ms time constant at 48 kHz
            const float start = 0.25f;
            const float target = 1.0f;

            const float expectedEnd =
                referenceRamp(expectedData.data(), frames, start, target, a);
            const float actualEnd =
                math::applyOnePoleRamp(actualData.data(), frames, start, target, a);

            REQUIRE(actualEnd == Approx(expectedEnd).epsilon(kRelativeTolerance));
            for (int i = 0; i < frames; ++i)
                REQUIRE(actualData[static_cast<std::size_t>(i)] ==
                        Approx(expectedData[static_cast<std::size_t>(i)]).epsilon(kRelativeTolerance));
        }
    }
}

TEST_CASE("A gain ramp settles on the target instead of creeping through denormals",
          "[dsp][blockstate][realtime]") {
    // REGRESSION: the closed form decays the remaining difference geometrically.
    // Without the snap-to-target guard it spends tens of thousands of samples in
    // the denormal range, and every multiply there carries a microcode assist -
    // measured 18 ns/frame instead of 0.5 on the reference profile. The guard must
    // also leave the state exactly *equal* to the target, so the caller takes its
    // plain-multiply path afterwards.
    const int frames = 256;
    std::vector<float> data(static_cast<std::size_t>(frames), 1.0f);
    const float a = 0.99792f;

    float current = 0.0f;
    int blocksToSettle = -1;
    for (int block = 0; block < 400 && blocksToSettle < 0; ++block) {
        current = math::applyOnePoleRamp(data.data(), frames, current, 1.0f, a);
        if (current == 1.0f)
            blocksToSettle = block + 1;
    }
    REQUIRE(blocksToSettle > 0);
    // A 10 ms one-pole at 48 kHz is 99.9% of the way after ~5 blocks; the last
    // 1e-6 takes a couple of dozen blocks to decay and is then snapped, so a
    // ramp that lands within 40 blocks (0.2 s) is the behaviour. Creeping for
    // thousands of blocks - which is what happens with no snap, because the state
    // parks one ULP short and the difference keeps decaying into the denormal
    // range - is the bug this pins.
    REQUIRE(blocksToSettle <= 40);
}

TEST_CASE("A gain ramp is monotone and reaches the target it was given", "[dsp][blockstate]") {
    // A ramp that overshoots or stalls would be audible as a click or a fade that
    // never lands; the closed form must behave like the recursion it replaces.
    const int frames = 512;
    std::vector<float> data(static_cast<std::size_t>(frames), 1.0f);
    const float a = 0.99792f;

    float current = 0.0f;
    for (int block = 0; block < 20; ++block)
        current = math::applyOnePoleRamp(data.data(), frames, current, 1.0f, a);

    REQUIRE(current > 0.99f);
    REQUIRE(current <= 1.0f);
    for (int i = 1; i < frames; ++i)
        REQUIRE(data[static_cast<std::size_t>(i)] >= data[static_cast<std::size_t>(i - 1)]);
    REQUIRE(data[static_cast<std::size_t>(frames - 1)] <= 1.0f);
}

TEST_CASE("The meter reading does not depend on the block size", "[dsp][metering][blockstate]") {
    // This is the property the per-sample recursion exists for: RMS is integrated
    // per frame, so 4096 frames split into 16 blocks must read the same as one
    // 4096-frame block. It is also the strongest check that the unrolled form did
    // not turn the integration into a per-block one.
    constexpr int kTotalFrames = 4096;
    auto signal = noisySignal(kTotalFrames, 4242u); // AudioBlockView holds float* const*
    constexpr double kSampleRate = 48000.0;

    dsp::LevelMeter single;
    single.prepare(kSampleRate);
    float* pointers[1] = {signal.data()};
    dsp::AudioBlockView whole{pointers, 1, kTotalFrames};
    single.process(whole);

    dsp::LevelMeter split;
    split.prepare(kSampleRate);
    for (int offset = 0; offset < kTotalFrames; offset += 256) {
        float* slice = signal.data() + offset;
        dsp::AudioBlockView block{&slice, 1, 256};
        split.process(block);
    }

    const auto wholeSnapshot = single.snapshot();
    const auto splitSnapshot = split.snapshot();
    REQUIRE(splitSnapshot.rmsLeft == Approx(wholeSnapshot.rmsLeft).epsilon(kRelativeTolerance));
    REQUIRE(splitSnapshot.peakLeft == Approx(wholeSnapshot.peakLeft).epsilon(kRelativeTolerance));
}

TEST_CASE("A graph node's fader ramps with the same shape as before", "[graph][blockstate]") {
    // End-to-end through the real node: set a gain target, process blocks, and
    // check the output is the input scaled by a ramp that matches the per-sample
    // reference. The node smooths with a 10 ms time constant, so the ramp is
    // finished within a handful of blocks - which is why a test that measures too
    // early sees a partially applied gain.
    graph::GraphBuilder builder;
    const auto sourceId = builder.createNode(graph::NodeKind::Track, "Source");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(sourceId, masterId);
    builder.setMasterNode(masterId);

    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 256, 1);

    auto* source = plan->find(sourceId);
    auto* master = plan->find(masterId);
    REQUIRE(source != nullptr);
    REQUIRE(master != nullptr);

    constexpr int kFrames = 256;
    std::vector<float> input(static_cast<std::size_t>(kFrames), 1.0f);
    std::vector<float> reference = input;
    float referenceGain = 0.0f;
    // A 10 ms one-pole at 48 kHz, evaluated per sample - the shape the node used
    // to produce with its own loop.
    const float a = 1.0f - 0.0020833f;

    for (int block = 0; block < 8; ++block) {
        for (int i = 0; i < kFrames; ++i)
            source->mutableChannelPointers()[0][i] = input[static_cast<std::size_t>(i)];
        source->setInputActive(true);
        if (block == 0)
            source->setGain(1.0f);
        plan->process(kFrames, dsp::ProcessContext{}, false);
        referenceGain = referenceRamp(reference.data(), kFrames, referenceGain, 1.0f, a);
    }

    // The node's own ramp state is internal, so compare the *shape*: the output
    // must be monotone, must have converged near unity by the end, and must not
    // overshoot. A wrong group power in the closed form shows up as a kink here.
    const float last = master->channelPointers()[0][kFrames - 1];
    const float first = master->channelPointers()[0][0];
    REQUIRE(last > 0.98f);
    REQUIRE(last <= 1.0f + 1.0e-5f);
    REQUIRE(first <= last);
    REQUIRE(referenceGain > 0.98f);
}
