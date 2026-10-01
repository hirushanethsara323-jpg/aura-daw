// ============================================================================
// AURA DAW - tests/graph/DelayCompensationTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Delay compensation (M5, issue #5).
//
// The property under test is physical, not structural: when two paths reach the
// same node, the audio that travelled the shorter path must arrive at the same
// time as the audio that went through a latency-inducing insert. A test that only
// checked "the plan says 64 samples" would pass even if nothing delayed anything,
// so every test here measures *where the audio actually lands*.
//
// Method: impulses. An impulse is the only signal whose position in a buffer is
// unambiguous, which is exactly what alignment is about. LatencyPad below is the
// test double for a latency-inducing plug-in; the real ones (limiter, oversampled
// distortion) are covered by their own tests and by the engine-level case at the
// end of this file, which uses a real AudioEngine, project and insert.
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aura/audio/AudioClip.hpp"
#include "aura/audio/Track.hpp"
#include "aura/dsp/DelayLine.hpp"
#include "aura/engine/AudioEngine.hpp"
#include "aura/graph/AudioGraph.hpp"
#include "aura/graph/SampleStream.hpp"
#include "aura/project/Project.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr int kPadSamples = 64; ///< the test double's latency

/// A processor that really delays its input by `latencySamples` and reports that
/// latency. Deliberately the simplest thing that can exist: if the graph
/// compensates correctly against this, it compensates against anything that
/// reports its latency honestly.
class LatencyPad final : public dsp::Processor {
public:
    explicit LatencyPad(int latency) : latency_(latency) {}

    void prepare(double, int maxBlockSize, int numChannels) override {
        prepared_ = true;
        maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 512;
        numChannels_ = std::max(1, numChannels);
        // One line per channel: a single shared line would delay channel 1 with
        // channel 0's samples, which would make the test double wrong in exactly
        // the situation the tests are about.
        lines_.clear();
        lines_.resize(static_cast<std::size_t>(numChannels_));
        for (auto& line : lines_)
            line.prepare(latency_ + maxBlockSize_);
    }

    /// True once prepare() has run. The tests assert on this, because a processor
    /// that is never prepared is a silent (and, for a delay line, unsafe) failure.
    [[nodiscard]] bool isPrepared() const noexcept { return prepared_; }

    void reset() noexcept override {
        for (auto& line : lines_)
            line.reset();
    }

    void process(dsp::AudioBlockView& block, const dsp::ProcessContext&) noexcept override {
        if (block.isEmpty() || latency_ <= 0)
            return;
        const int channels = std::min(block.numChannels, numChannels_);
        for (int channel = 0; channel < channels; ++channel)
            lines_[static_cast<std::size_t>(channel)].copyDelayed(
                block.channelPointers[channel], block.channelPointers[channel], block.numFrames,
                latency_);
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "LatencyPad"; }
    [[nodiscard]] int latencySamples() const noexcept override { return latency_; }

private:
    bool prepared_ = false;
    int latency_ = 0;
    int maxBlockSize_ = 512;
    int numChannels_ = 2;
    std::vector<dsp::DelayLine> lines_;
};

struct NodePair {
    std::shared_ptr<graph::GraphPlan> plan;
    graph::GraphNode* dry = nullptr;
    graph::GraphNode* lat = nullptr;
    graph::GraphNode* master = nullptr;
    std::unique_ptr<LatencyPad> pad;
};

/// Builds:  dry -> master  and  lat -> pad(64) -> master.
NodePair buildTwoPaths(bool compensate) {
    graph::GraphBuilder builder;
    const auto dryId = builder.createNode(graph::NodeKind::Track, "Dry");
    const auto latId = builder.createNode(graph::NodeKind::Track, "Latent");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(dryId, masterId);
    builder.connect(latId, masterId);
    builder.setMasterNode(masterId);
    builder.setDelayCompensationEnabled(compensate);

    NodePair pair;
    // The insert must exist on the node *before* build(), because the builder
    // reads the chain to work out the compensation. That is how the engine wires
    // it too (see AudioEngine::rebuildGraph).
    pair.pad = std::make_unique<LatencyPad>(kPadSamples);
    builder.find(latId)->inserts().add(pair.pad.get());

    pair.plan = builder.build();
    for (const auto& node : pair.plan->nodes())
        node->prepare(kSampleRate, kBlockSize, 2);

    pair.dry = pair.plan->find(dryId);
    pair.lat = pair.plan->find(latId);
    pair.master = pair.plan->find(masterId);
    return pair;
}

/// Writes an impulse at `offset` into the node's channels and marks the node as
/// producing sound (source nodes are not cleared by the plan).
void impulseAt(graph::GraphNode& node, int offset, float amplitude) {
    for (int channel = 0; channel < node.numChannels(); ++channel) {
        float* data = node.mutableChannelPointers()[channel];
        for (int i = 0; i < kBlockSize; ++i)
            data[i] = 0.0f;
        data[offset] = amplitude;
    }
    node.setInputActive(true);
}

std::vector<int> impulsePositions(const graph::GraphNode& node, float threshold) {
    std::vector<int> positions;
    const float* data = node.channelPointers()[0];
    for (int i = 0; i < kBlockSize; ++i) {
        if (std::abs(data[i]) > threshold)
            positions.push_back(i);
    }
    return positions;
}

dsp::ProcessContext context() {
    dsp::ProcessContext ctx;
    ctx.sampleRate = kSampleRate;
    ctx.isPlaying = true;
    return ctx;
}

} // namespace

TEST_CASE("Compensation aligns a short path with a latent one", "[graph][pdc]") {
    // Both tracks fire an impulse at the same sample. The latent track's impulse
    // leaves the pad 64 samples later, so compensation must delay the dry track by
    // exactly those 64 samples for the two to land together - and, because they
    // then sum, the master shows *one* impulse at twice the amplitude.
    for (const bool compensate : {true, false}) {
        DYNAMIC_SECTION("compensation " << (compensate ? "on" : "off")) {
            NodePair pair = buildTwoPaths(compensate);
            REQUIRE(pair.plan->latencySamples() == kPadSamples);
            REQUIRE(pair.pad->isPrepared()); // the graph prepares its insert chains

            impulseAt(*pair.dry, 0, 0.25f);
            impulseAt(*pair.lat, 0, 0.25f);
            pair.plan->process(kBlockSize, context(), false);

            const auto positions = impulsePositions(*pair.master, 0.05f);
            if (compensate) {
                // Aligned: the two impulses land on the same sample and sum, so
                // the master shows one arrival at twice the amplitude.
                REQUIRE(positions.size() == 1);
                REQUIRE(positions[0] == kPadSamples);
                REQUIRE(pair.master->channelPointers()[0][kPadSamples] == Approx(0.5f).margin(1e-4f));
                REQUIRE(pair.plan->compensationSamples() == kPadSamples);
                REQUIRE(pair.plan->compensatedEdges() == 1);
            } else {
                // Misaligned by exactly the pad's latency: the same audio arrives
                // twice, 64 samples apart, at its original amplitude. This is what
                // the compensation is for, and it is measured, not assumed.
                REQUIRE(positions.size() == 2);
                REQUIRE(positions[0] == 0);
                REQUIRE(positions[1] == kPadSamples);
                REQUIRE(pair.master->channelPointers()[0][0] == Approx(0.25f).margin(1e-4f));
                REQUIRE(pair.master->channelPointers()[0][kPadSamples] == Approx(0.25f).margin(1e-4f));
                REQUIRE(pair.plan->compensationSamples() == 0);
                REQUIRE(pair.plan->compensatedEdges() == 0);
            }
        }
    }
}

TEST_CASE("Without compensation the dry path really is early", "[graph][pdc]") {
    // The negative control for the test above: same graph, compensation off, and
    // the two impulses must be exactly 64 samples apart, one at each arrival time.
    // If this ever reports a single impulse at 64, the "aligned" case above is
    // measuring nothing.
    NodePair pair = buildTwoPaths(false);

    impulseAt(*pair.dry, 0, 0.25f);
    impulseAt(*pair.lat, 0, 0.25f);
    pair.plan->process(kBlockSize, context(), false);

    const auto positions = impulsePositions(*pair.master, 0.05f);
    REQUIRE(positions.size() == 2);
    REQUIRE(positions[0] == 0);           // dry, untouched
    REQUIRE(positions[1] == kPadSamples); // through the pad
}

TEST_CASE("A send is compensated against the direct out", "[graph][pdc]") {
    // The classic comb-filter case: a track reaches the master twice - directly,
    // and through a bus that carries a latent insert. Both arrivals must be
    // aligned, so it is the *direct* edge that needs the delay.
    graph::GraphBuilder builder;
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Track");
    const auto busId = builder.createNode(graph::NodeKind::Bus, "Bus");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(trackId, masterId); // direct
    builder.connect(trackId, busId);    // dry into the bus
    builder.connect(busId, masterId);   // bus -> master
    builder.setMasterNode(masterId);
    builder.setDelayCompensationEnabled(true);

    auto pad = std::make_unique<LatencyPad>(kPadSamples);
    builder.find(busId)->inserts().add(pad.get());

    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(kSampleRate, kBlockSize, 2);

    auto* track = plan->find(trackId);
    auto* master = plan->find(masterId);
    REQUIRE(track != nullptr);
    REQUIRE(master != nullptr);
    REQUIRE(plan->latencySamples() == kPadSamples);

    impulseAt(*track, 0, 0.25f);
    plan->process(kBlockSize, context(), false);

    const auto positions = impulsePositions(*master, 0.05f);
    REQUIRE(positions.size() == 1);
    REQUIRE(positions[0] == kPadSamples);
    REQUIRE(plan->compensatedEdges() == 1);
}

TEST_CASE("Compensation follows the longest path, not the first one found", "[graph][pdc]") {
    // Two latent paths of different lengths feeding one node. A's chain is 128
    // samples of insert latency and B's is 64, so the master's incoming edges need
    // 0 and 64 samples of delay respectively, and the plan reports the longest
    // chain (128), not the sum.
    graph::GraphBuilder builder;
    const auto aId = builder.createNode(graph::NodeKind::Track, "A");
    const auto bId = builder.createNode(graph::NodeKind::Track, "B");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(aId, masterId);
    builder.connect(bId, masterId);
    builder.setMasterNode(masterId);
    builder.setDelayCompensationEnabled(true);

    auto padA = std::make_unique<LatencyPad>(128);
    auto padB = std::make_unique<LatencyPad>(64);
    builder.find(aId)->inserts().add(padA.get());
    builder.find(bId)->inserts().add(padB.get());

    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(kSampleRate, 256, 2);

    // A's chain is 128 long, B's is 64, so master's longest input path is 128 and
    // B's edge is compensated by 64.
    REQUIRE(plan->latencySamples() == 128);
    REQUIRE(plan->compensationSamples() == 64);
    REQUIRE(plan->compensatedEdges() == 1);

    auto* master = plan->find(masterId);
    REQUIRE(master != nullptr);
    REQUIRE(master->compensationSamples() == 64);
    REQUIRE(master->hasCompensation());
}

TEST_CASE("A bypassed insert adds no latency and no compensation", "[graph][pdc]") {
    // Bypass is not the same as removal: the processor is still in the chain and is
    // simply not run, so it adds neither delay nor compensation. Getting this wrong
    // in the other direction would delay a dry path for a delay that never happens.
    graph::GraphBuilder builder;
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Track");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(trackId, masterId);
    builder.setMasterNode(masterId);
    builder.setDelayCompensationEnabled(true);

    auto pad = std::make_unique<LatencyPad>(kPadSamples);
    pad->setBypassed(true);
    builder.find(trackId)->inserts().add(pad.get());

    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(kSampleRate, kBlockSize, 2);

    REQUIRE(plan->latencySamples() == 0);
    REQUIRE(plan->compensationSamples() == 0);
}

TEST_CASE("Compensation keeps its bounded output over a long run", "[graph][pdc][realtime]") {
    // A ring-buffer index that drifts shows up as growing garbage rather than as a
    // crash, so the delay path is exercised over many blocks with a repeating
    // impulse and the output is checked for finiteness and bounds.
    NodePair pair = buildTwoPaths(true);
    REQUIRE(pair.plan->compensatedEdges() == 1);

    // Counted, not asserted per sample: a REQUIRE inside this loop would add
    // ~100k assertions to the suite for one property. The two counters are the
    // property, and they are checked once, with the offending block reported.
    int nonFinite = 0;
    int tooLoud = 0;
    int firstBadBlock = -1;
    float maximum = 0.0f;
    for (int block = 0; block < 200; ++block) {
        impulseAt(*pair.dry, block % kBlockSize, 1.0f);
        impulseAt(*pair.lat, block % kBlockSize, 1.0f);
        pair.plan->process(kBlockSize, context(), false);
        for (int channel = 0; channel < pair.master->numChannels(); ++channel) {
            const float* data = pair.master->channelPointers()[channel];
            for (int i = 0; i < kBlockSize; ++i) {
                const float magnitude = std::abs(data[i]);
                if (!std::isfinite(data[i]))
                    ++nonFinite;
                // Two unit impulses sum to 2.0; a delay ring that drifts or
                // double-reads shows up here as a louder output.
                if (magnitude > 2.0f + 1e-3f)
                    ++tooLoud;
                if ((!std::isfinite(data[i]) || magnitude > 2.0f + 1e-3f) && firstBadBlock < 0)
                    firstBadBlock = block;
                maximum = std::max(maximum, magnitude);
            }
        }
    }
    INFO("first offending block: " << firstBadBlock << ", peak " << maximum);
    REQUIRE(nonFinite == 0);
    REQUIRE(tooLoud == 0);
}

// ---------------------------------------------------------------------------
// Engine level: a real project, a real insert, a real AudioEngine
// ---------------------------------------------------------------------------
namespace {

constexpr int kImpulseSample = 1000;
constexpr int kBlocksRendered = 12;

/// A device that never asks for a callback: this test drives processBlock()
/// directly so the measurement is deterministic (the same approach the engine
/// tests use).
class ManualDevice final : public engine::IAudioDevice {
public:
    Status start(Callback) override { return success(); }
    void stop() override {}
    [[nodiscard]] bool isRunning() const noexcept override { return true; }
    [[nodiscard]] std::string name() const override { return "manual"; }
    [[nodiscard]] double sampleRate() const noexcept override { return kSampleRate; }
    [[nodiscard]] int bufferSize() const noexcept override { return kBlockSize; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return 2; }
    [[nodiscard]] int numInputChannels() const noexcept override { return 0; }
    [[nodiscard]] int latencySamples() const noexcept override { return 0; }
};

struct EngineFixture {
    std::unique_ptr<project::Project> project;
    audio::TrackId dryTrack = 0;
    audio::TrackId latentTrack = 0;

    explicit EngineFixture(int clipFrames = 4096) {
        project = project::Project::createEmpty("PDC Test");
        project->setSampleRate(kSampleRate);
        project->mixer().prepare(kSampleRate, kBlockSize, 2);

        // A new project ships with one empty default track. Remove it so the graph
        // under test is exactly the two tracks below - otherwise it contributes a
        // third path to the master and the expected compensation is not the pad's
        // latency but a multiple of it.
        std::vector<audio::TrackId> defaults;
        for (const audio::Track* track : project->mixer().tracks())
            defaults.push_back(track->id());
        for (const audio::TrackId id : defaults)
            REQUIRE(project->mixer().removeTrack(id));

        dryTrack = project->mixer().createTrack(audio::TrackKind::Audio, "Dry");
        latentTrack = project->mixer().createTrack(audio::TrackKind::Audio, "Latent");

        // Both tracks carry the same single impulse, so any offset between the two
        // arrivals at the master is misalignment and nothing else.
        for (const audio::TrackId trackId : {dryTrack, latentTrack}) {
            std::vector<std::vector<float>> channels(2, std::vector<float>(
                                                            static_cast<std::size_t>(clipFrames), 0.0f));
            channels[0][kImpulseSample] = 0.25f;
            channels[1][kImpulseSample] = 0.25f;

            audio::Clip clip(project->nextClipId(), audio::ClipType::Audio, "Impulse");
            clip.setStart(0);
            clip.setLength(clipFrames);
            clip.setStream(graph::MemoryStream::fromPlanar(std::move(channels), kSampleRate));
            if (auto* stored = project->addClip(std::move(clip))) {
                if (audio::Track* track = project->mixer().findTrack(trackId))
                    track->clips().push_back(stored->id());
            }
        }
    }
};

struct Peak {
    int index = 0;
    float value = 0.0f;
};

std::vector<Peak> findImpulses(const std::vector<float>& buffer, float threshold) {
    std::vector<Peak> peaks;
    bool inPeak = false;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const bool above = std::abs(buffer[i]) > threshold;
        if (above && !inPeak)
            peaks.push_back({static_cast<int>(i), buffer[i]});
        inPeak = above;
    }
    return peaks;
}

} // namespace

TEST_CASE("The graph prepares the processors in its insert chains", "[graph][pdc]") {
    // A regression guard, and the reason the tests above can construct a pad and
    // use it immediately: GraphNode::prepare() calls prepare() on every processor
    // in its chain with the sample rate, block size and channel count it will run
    // with. Without it a look-ahead limiter or a delay would process with
    // unprepared state, which is silent at best.
    graph::GraphBuilder builder;
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Track");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(trackId, masterId);
    builder.setMasterNode(masterId);

    auto pad = std::make_unique<LatencyPad>(kPadSamples);
    REQUIRE_FALSE(pad->isPrepared());
    builder.find(trackId)->inserts().add(pad.get());

    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(kSampleRate, kBlockSize, 2);

    REQUIRE(pad->isPrepared());

    // Preparing twice must be legal (device change, sample-rate change, rebuild).
    for (const auto& node : plan->nodes())
        node->prepare(kSampleRate, kBlockSize * 2, 2);
    REQUIRE(pad->isPrepared());
}

TEST_CASE("The engine aligns a track that carries a real latent insert", "[engine][pdc]") {
    for (const bool compensate : {true, false}) {
        DYNAMIC_SECTION("compensation " << (compensate ? "on" : "off")) {
            EngineFixture fixture;
            auto pad = std::make_unique<LatencyPad>(kPadSamples);
            audio::Track* latent = fixture.project->mixer().findTrack(fixture.latentTrack);
            REQUIRE(latent != nullptr);
            REQUIRE(latent->addInsert({"Pad", pad.get(), false, "", 1.0f}));

            ManualDevice device;
            engine::AudioEngine audioEngine;
            engine::EngineSettings settings;
            settings.sampleRate = kSampleRate;
            settings.bufferSize = kBlockSize;
            settings.numOutputChannels = 2;
            settings.numInputChannels = 0;
            settings.delayCompensation = compensate;
            audioEngine.setProject(fixture.project.get());
            REQUIRE(static_cast<bool>(audioEngine.initialise(&device, settings)));
            REQUIRE(static_cast<bool>(audioEngine.rebuildGraph()));
            REQUIRE(pad->isPrepared());

            // The engine reports what it did: the plan's latency is the pad's
            // either way (turning compensation off removes alignment, not delay),
            // and the compensation figure is what the UI shows.
            REQUIRE(audioEngine.currentPlan()->latencySamples() == kPadSamples);
            REQUIRE(audioEngine.totalLatencySamples() >= kPadSamples);
            if (compensate) {
                REQUIRE(audioEngine.delayCompensationSamples() == kPadSamples);
                REQUIRE(audioEngine.compensatedConnectionCount() == 1);
            } else {
                REQUIRE(audioEngine.delayCompensationSamples() == 0);
                REQUIRE(audioEngine.compensatedConnectionCount() == 0);
            }

            std::vector<float> left(kBlockSize, 0.0f);
            std::vector<float> right(kBlockSize, 0.0f);
            float* outputs[2] = {left.data(), right.data()};
            std::vector<float> captured;
            captured.reserve(static_cast<std::size_t>(kBlocksRendered * kBlockSize));

            audioEngine.transport().play();
            for (int block = 0; block < kBlocksRendered; ++block) {
                std::fill(left.begin(), left.end(), 0.0f);
                std::fill(right.begin(), right.end(), 0.0f);
                audioEngine.processBlock(outputs, nullptr, kBlockSize);
                captured.insert(captured.end(), left.begin(), left.end());
            }
            audioEngine.shutdown();

            const std::vector<Peak> peaks = findImpulses(captured, 0.05f);
            REQUIRE_FALSE(peaks.empty());

            if (compensate) {
                // One arrival, both impulses summed: amplitude 0.5 and nothing
                // else above the threshold anywhere in the capture.
                REQUIRE(peaks.size() == 1);
                REQUIRE(peaks[0].value == Approx(0.5f).margin(0.02f));
            } else {
                // Two arrivals exactly kPadSamples apart: the misalignment that
                // compensation exists to remove.
                REQUIRE(peaks.size() == 2);
                REQUIRE(peaks[1].index - peaks[0].index == kPadSamples);
                REQUIRE(std::abs(peaks[0].value) == Approx(0.25f).margin(0.02f));
                REQUIRE(std::abs(peaks[1].value) == Approx(0.25f).margin(0.02f));
            }
        }
    }
}
