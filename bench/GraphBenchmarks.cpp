// ============================================================================
// AURA DAW - bench/GraphBenchmarks.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The graph's own per-block work, measured without the engine around it (issue
// #6, the SIMD decision). Two holes this fills:
//
//   1. FAN-IN. A node sums every incoming connection into its buffer, so a master
//      with 48 tracks runs 48 summing loops per block no matter how many effects
//      the session has. That loop is elementwise, cache-resident and exactly the
//      shape a hand-written SIMD kernel would target - and until now the suite
//      had no case for it, so any claim about vectorising the mixer would have
//      been made without data.
//   2. FADER. A node's gain ramp is a first-order recursion: expensive while a
//      ramp is in flight, a plain multiply once it has landed. The pair of cases
//      (`gain-steady`, `gain-ramp`) separates the two so a change to the ramp
//      maths shows up as a ramp-shaped result and not as noise.
//
// The plan is driven directly (no device, no transport): the number is the
// graph's, which is what the decision needs.
// ============================================================================
#include "Benchmarks.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Math.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/graph/AudioGraph.hpp"

namespace aura::bench {
namespace {

constexpr double kSampleRate = 48000.0;

/// A plan of `sources` tracks summing into one master, plus the buffer the
/// sources produce. Sources have no incoming connections, so the plan leaves
/// their buffers alone between calls - they are filled once, here.
class FanInPlan {
public:
    FanInPlan(int sources, int blockSize) : blockSize_(blockSize) {
        graph::GraphBuilder builder;
        for (int i = 0; i < sources; ++i) {
            const graph::NodeId id =
                builder.createNode(graph::NodeKind::Track, "Source " + std::to_string(i + 1));
            sources_.push_back(id);
            builder.connect(id, masterId_);
        }
        builder.connect(builder.createNode(graph::NodeKind::Master, "Master"), masterId_);
        builder.setMasterNode(masterId_);

        plan_ = builder.build();
        if (!plan_)
            return;
        for (const auto& node : plan_->nodes())
            node->prepare(kSampleRate, blockSize, 2);

        master_ = plan_->find(masterId_);
        // Deterministic content that differs per source: identical buffers would
        // map to the same lines and flatter the memory path.
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            graph::GraphNode* node = plan_->find(sources_[index]);
            if (!node)
                continue;
            const double frequency = 110.0 + 13.0 * static_cast<double>(index);
            for (int channel = 0; channel < node->numChannels(); ++channel) {
                float* data = node->mutableChannelPointers()[channel];
                for (int i = 0; i < blockSize; ++i) {
                    const double t = static_cast<double>(i) / kSampleRate;
                    data[i] = static_cast<float>(0.2 * std::sin(math::kTwoPi * frequency * t));
                }
            }
            node->setInputActive(true);
        }
        context_.sampleRate = kSampleRate;
        context_.isPlaying = true;
    }

    [[nodiscard]] bool isValid() const noexcept { return plan_ != nullptr && master_ != nullptr; }

    void process() {
        plan_->process(blockSize_, context_, false);
        checksum += static_cast<double>(master_->channelPointers()[0][0]);
    }

    /// The sanity check: a fan-in whose master stays silent is measuring a plan
    /// that does not sum, and every number after that would be meaningless.
    [[nodiscard]] float probePeak() {
        plan_->process(blockSize_, context_, false);
        float peak = 0.0f;
        const float* data = master_->channelPointers()[0];
        for (int i = 0; i < blockSize_; ++i)
            peak = std::max(peak, std::abs(data[i]));
        return peak;
    }

    static double checksum;

private:
    std::shared_ptr<graph::GraphPlan> plan_;
    std::vector<graph::NodeId> sources_;
    graph::NodeId masterId_ = 1;
    graph::GraphNode* master_ = nullptr;
    dsp::ProcessContext context_;
    int blockSize_ = 256;
};

double FanInPlan::checksum = 0.0;

/// One track node feeding the master, used for the fader cases.
class FaderPlan {
public:
    explicit FaderPlan(int blockSize) : blockSize_(blockSize) {
        graph::GraphBuilder builder;
        const auto sourceId = builder.createNode(graph::NodeKind::Track, "Source");
        const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
        builder.connect(sourceId, masterId);
        builder.setMasterNode(masterId);

        plan_ = builder.build();
        if (!plan_)
            return;
        for (const auto& node : plan_->nodes())
            node->prepare(kSampleRate, blockSize, 2);
        source_ = plan_->find(sourceId);
        master_ = plan_->find(masterId);
        if (source_) {
            for (int channel = 0; channel < source_->numChannels(); ++channel) {
                float* data = source_->mutableChannelPointers()[channel];
                for (int i = 0; i < blockSize; ++i)
                    data[i] = static_cast<float>(0.2 * std::sin(0.05 * static_cast<double>(i)));
            }
            source_->setInputActive(true);
        }
        context_.sampleRate = kSampleRate;
        context_.isPlaying = true;
    }

    [[nodiscard]] bool isValid() const noexcept { return plan_ && source_ && master_; }

    /// Steady state: the fader sits at 0.5 and the loop is a plain multiply.
    void processSteady() {
        plan_->process(blockSize_, context_, false);
        checksum += static_cast<double>(master_->channelPointers()[0][0]);
    }

    /// A ramp in flight: a small fader ride around unity (1.0 <-> 1.02), moved
    /// every call, so the node never settles and every sample takes the ramp path.
    ///
    /// Why the ride is *around unity* and not down to 0.25: this case used to ramp
    /// down and back, with nothing ever restoring the material. The source buffer
    /// then decayed geometrically into the denormal range, where every multiply
    /// carries a microcode assist on x86, and the case reported 72 ns/frame for a
    /// fader that costs about 0.6. A benchmark that measures the CPU's denormal
    /// handling instead of the code is worse than no benchmark, which is why
    /// `probeBand()` below fails the case if the material leaves a sane amplitude
    /// band in either direction (drained to silence or run away to clipping).
    void processRamping() {
        rampTarget_ = rampTarget_ > 1.01f ? 1.0f : 1.02f;
        source_->setGain(rampTarget_);
        plan_->process(blockSize_, context_, false);
        checksum += static_cast<double>(master_->channelPointers()[0][0]);
    }

    /// Peak of one processed block, without touching the material.
    [[nodiscard]] float probePeak() {
        plan_->process(blockSize_, context_, false);
        float peak = 0.0f;
        const float* output = master_->channelPointers()[0];
        for (int i = 0; i < blockSize_; ++i)
            peak = std::max(peak, std::abs(output[i]));
        return peak;
    }

    /// The material must stay audible: neither decayed into the denormal range
    /// (which would flatter the timing) nor grown until the numbers are meaningless.
    [[nodiscard]] bool probeBand() {
        const float peak = probePeak();
        return peak > 1.0e-3f && peak < 1.0e4f;
    }

    static double checksum;

private:
    std::shared_ptr<graph::GraphPlan> plan_;
    graph::GraphNode* source_ = nullptr;
    graph::GraphNode* master_ = nullptr;
    dsp::ProcessContext context_;
    int blockSize_ = 256;
    float rampTarget_ = 1.0f;
};

double FaderPlan::checksum = 0.0;

std::string fanInName(int sources) { return "graph/fan-in-" + std::to_string(sources); }

} // namespace

void registerGraphBenchmarks(const std::vector<int>& blockSizes) {
    // Fan-in at the session sizes that matter: 8 (small), 16 (typical), 48
    // (large), 64 (stress). One block size - the summing loop's cost is linear in
    // frames, so the block sizes would say the same thing four times.
    for (const int sources : {8, 16, 48, 64}) {
        auto plan = std::make_shared<FanInPlan>(sources, 256);
        if (!plan->isValid()) {
            std::fprintf(stderr, "bench: fan-in plan %d could not be built\n", sources);
            continue;
        }
        Case testCase;
        testCase.name = fanInName(sources);
        testCase.group = "graph";
        testCase.description = std::to_string(sources) +
                               " tracks summing into one master (the fan-in loop)";
        testCase.sampleRate = kSampleRate;
        testCase.blockSize = 256;
        testCase.channels = 2;
        testCase.call = [plan] { plan->process(); };
        testCase.sanityCheck = [plan] { return plan->probePeak() > 0.01f; };
        registry().add(std::move(testCase));
    }

    // Fader: the two paths through GraphNode::applyGainSmoothing.
    for (const int blockSize : blockSizes) {
        auto steady = std::make_shared<FaderPlan>(blockSize);
        if (!steady->isValid())
            continue;
        Case steadyCase;
        steadyCase.name = "graph/fader-steady";
        steadyCase.group = "graph";
        steadyCase.description = "one track's fader at a fixed gain (multiply path, no ramp)";
        steadyCase.sampleRate = kSampleRate;
        steadyCase.blockSize = blockSize;
        steadyCase.channels = 2;
        steadyCase.call = [steady] { steady->processSteady(); };
        steadyCase.sanityCheck = [steady] { return steady->probeBand(); };
        registry().add(std::move(steadyCase));

        auto ramping = std::make_shared<FaderPlan>(blockSize);
        Case rampCase;
        rampCase.name = "graph/fader-ramp";
        rampCase.group = "graph";
        rampCase.description = "one track's fader with a ramp in flight every block";
        rampCase.sampleRate = kSampleRate;
        rampCase.blockSize = blockSize;
        rampCase.channels = 2;
        rampCase.call = [ramping] { ramping->processRamping(); };
        rampCase.sanityCheck = [ramping] { return ramping->probeBand(); };
        registry().add(std::move(rampCase));
    }
}

} // namespace aura::bench
