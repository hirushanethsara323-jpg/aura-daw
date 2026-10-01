// ============================================================================
// AURA DAW - graph/AudioGraph.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio graph: the structure that actually produces sound.
//
// Design: a graph is a list of nodes in topological order. The engine compiles
// a user-facing routing description (tracks -> buses -> master, plus sends)
// into that flat, topologically sorted list *off the audio thread*, then swaps
// it in atomically. The audio thread therefore never walks a tree, never
// allocates, and never resolves routing at runtime.
//
//   AudioTrack -> InsertChain -> Sends -> Bus -> Master
//
// Node kinds mirror the brief: Input, Track, AudioClip, Gain, Pan, EQ,
// Compressor, Limiter, Reverb, Delay, Distortion, Bus, Send, Return, Master,
// Plugin, Output.
// ============================================================================
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Math.hpp"
#include "aura/dsp/DelayLine.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::graph {

using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNodeId = 0;

enum class NodeKind : std::int32_t {
    Input = 0,
    Track,
    Bus,
    Send,
    Return,
    Master,
    Output,
    Gain,
    Pan,
    Equalizer,
    Compressor,
    Limiter,
    Reverb,
    Delay,
    Distortion,
    Plugin,
};

const char* nodeKindName(NodeKind kind) noexcept;

/// One incoming connection of a node, plus the delay that aligns it with the
/// node's other inputs.
///
/// Delay compensation in three sentences: every insert chain adds latency to the
/// signal passing through it; when two paths meet (a dry path and a path through a
/// look-ahead limiter, or a send alongside the direct out) the shorter one must be
/// delayed by the difference or the two arrive misaligned and comb-filter; the
/// amount of delay is therefore per *edge*, not per node, because one node can feed
/// several destinations with different path latencies behind them.
struct InputEdge {
    NodeId source = kInvalidNodeId;
    /// Samples of delay applied to this edge. 0 = the edge is already as late as
    /// the node's latest input.
    int compensationSamples = 0;
};

/// A graph node. Buffers are owned by the graph (planar, max block size), so
/// there is no allocation during processing and the layout is stable.
class GraphNode {
public:
    GraphNode(NodeId id, NodeKind kind, std::string name);
    ~GraphNode();

    GraphNode(const GraphNode&) = delete;
    GraphNode& operator=(const GraphNode&) = delete;

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] NodeKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    /// Allocates buffers and prepares processors. Control thread only.
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    [[nodiscard]] bool isPrepared() const noexcept { return prepared_; }

    /// Clears all buffers and processor state.
    void reset() noexcept;

    /// Channel buffers for this node (planar, `numChannels` pointers).
    [[nodiscard]] float* const* channelPointers() const noexcept { return channelPointers_.data(); }
    [[nodiscard]] float** mutableChannelPointers() noexcept { return channelPointers_.data(); }
    [[nodiscard]] int numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] int maxBlockSize() const noexcept { return maxBlockSize_; }
    [[nodiscard]] const dsp::AudioBlockView& block() const noexcept { return block_; }
    [[nodiscard]] dsp::AudioBlockView& mutableBlock() noexcept { return block_; }

    /// Sums `source` into this node's buffers (used to feed inputs from sends),
    /// applying this connection's compensation delay. Audio thread.
    void sumFrom(const GraphNode& source, int numFrames) noexcept;

    /// Declares the incoming connections and their compensation delays. Control
    /// thread only, called by GraphBuilder before prepare(): the delay lines are
    /// allocated in prepare() from the values set here.
    void setInputEdges(std::vector<InputEdge> edges) {
        compensationSamples_ = 0;
        compensationEdgeCount_ = 0;
        for (const InputEdge& edge : edges) {
            if (edge.compensationSamples > 0) {
                compensationSamples_ += edge.compensationSamples;
                ++compensationEdgeCount_;
            }
        }
        inputEdges_ = std::move(edges);
    }
    [[nodiscard]] const std::vector<InputEdge>& inputEdges() const noexcept { return inputEdges_; }
    /// Total delay this node inserts on its incoming edges (diagnostics).
    [[nodiscard]] int compensationSamples() const noexcept { return compensationSamples_; }
    /// True when at least one incoming edge carries a delay, so the common
    /// no-compensation case keeps the plain summing loop.
    [[nodiscard]] bool hasCompensation() const noexcept { return compensationEdgeCount_ > 0; }
    /// Copies `source` into this node's buffers.
    void copyFrom(const GraphNode& source, int numFrames) noexcept;
    /// Fills this node's buffers with silence.
    void clear(int numFrames) noexcept;

    /// Per-node gain/mute so routing changes never click.
    ///
    /// The fader is RAMPED: `setTargetGains()` may be called from the audio
    /// thread every block (two float stores) and the per-sample one-pole ramp in
    /// applyGainSmoothing() removes zipper noise and the click that a mute or a
    /// routing change would otherwise produce. Channel 0 is left; every further
    /// channel reuses the left gain (multichannel expansion is a documented
    /// post-V1 item).
    void setGain(float linear) noexcept {
        gain_ = linear;
        setTargetGains(linear, linear);
    }
    [[nodiscard]] float gain() const noexcept { return gain_; }
    void setTargetGains(float left, float right) noexcept {
        targetGainL_ = left;
        targetGainR_ = right;
    }
    [[nodiscard]] float currentGain(int channel) const noexcept {
        return channel == 0 ? currentGainL_ : currentGainR_;
    }
    /// Applies the ramped fader across the first `numFrames` of the node.
    void applyGainSmoothing(int numFrames) noexcept;

    /// Number of nodes routing INTO this one (set by GraphBuilder). A node with
    /// inputs accumulates, so the plan clears it at the start of every block.
    void setIncomingConnections(int count) noexcept { incomingConnections_ = count; }
    [[nodiscard]] int incomingConnections() const noexcept { return incomingConnections_; }
    void setMuted(bool muted) noexcept { muted_ = muted; }
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setSoloed(bool soloed) noexcept { soloed_ = soloed; }
    [[nodiscard]] bool isSoloed() const noexcept { return soloed_; }

    /// Insert chain executed when this node is processed, in order.
    class InsertChain {
    public:
        void add(dsp::Processor* processor) { processors_.push_back(processor); }
        void remove(dsp::Processor* processor);
        void clear() { processors_.clear(); }
        [[nodiscard]] std::size_t size() const noexcept { return processors_.size(); }
        [[nodiscard]] dsp::Processor* at(std::size_t index) const noexcept {
            return index < processors_.size() ? processors_[index] : nullptr;
        }
        /// Sum of the chain's reported latencies. Bypassed processors are skipped
        /// for the same reason process() skips them: a bypassed look-ahead
        /// limiter adds no delay, so compensating for it would mis-align the
        /// paths instead of aligning them. (Toggling bypass during playback is
        /// therefore a graph change: the engine rebuilds the plan, which is what
        /// recomputes this number.)
        [[nodiscard]] int totalLatencySamples() const noexcept {
            int total = 0;
            for (const auto* processor : processors_) {
                if (processor && !processor->isBypassed())
                    total += processor->latencySamples();
            }
            return total;
        }
        void process(dsp::AudioBlockView& block, const dsp::ProcessContext& context) noexcept {
            for (auto* processor : processors_) {
                if (processor && !processor->isBypassed())
                    processor->process(block, context);
            }
        }
        /// True when any processor in the chain still has to run even with a
        /// silent input (reverb/delay tails, running generators).
        [[nodiscard]] bool anyActive() const noexcept {
            for (const auto* processor : processors_) {
                if (processor && processor->hasTail())
                    return true;
            }
            return false;
        }

    private:
        /// Raw pointers: the graph does not own insert processors; the engine's
        /// processor pool does, so lifetime is explicit and allocation-free.
        std::vector<dsp::Processor*> processors_;
    };

    [[nodiscard]] InsertChain& inserts() noexcept { return inserts_; }
    [[nodiscard]] const InsertChain& inserts() const noexcept { return inserts_; }

    /// Routing: where this node's output goes (node ids, order preserved).
    void setDestinations(std::vector<NodeId> destinations) {
        destinations_ = std::move(destinations);
    }
    [[nodiscard]] const std::vector<NodeId>& destinations() const noexcept {
        return destinations_;
    }

    /// True while the node produces audio (used to skip silent tracks safely).
    [[nodiscard]] bool isActive() const noexcept;
    void setInputActive(bool active) noexcept { inputActive_ = active; }

private:
    NodeId id_ = kInvalidNodeId;
    NodeKind kind_ = NodeKind::Track;
    std::string name_;
    bool prepared_ = false;
    int numChannels_ = 2;
    int maxBlockSize_ = 512;

    std::vector<float> storage_;            ///< channels * maxBlockSize floats
    std::vector<float*> channelPointers_;   ///< stable pointer array
    dsp::AudioBlockView block_{};

    float gain_ = 1.0f;
    float targetGainL_ = 1.0f;
    float targetGainR_ = 1.0f;
    float currentGainL_ = 1.0f;
    float currentGainR_ = 1.0f;
    float smoothingCoefficient_ = 0.01f;
    int incomingConnections_ = 0;
    bool muted_ = false;
    bool soloed_ = false;
    bool inputActive_ = false;

    InsertChain inserts_;
    std::vector<NodeId> destinations_;

    /// Incoming connections with their compensation delay, and one delay line per
    /// edge (same index). The delay lines are sized in prepare() to exactly the
    /// compensation they carry: a session with no latent processors allocates
    /// nothing (DelayLine::prepare(0) still makes one slot per edge).
    /// `(source, index into inputEdges_)`, sorted by source in prepare() so the
    /// audio thread can find an edge with a binary search instead of a scan.
    struct EdgeRef {
        NodeId source = kInvalidNodeId;
        std::uint32_t index = 0;
    };

    std::vector<InputEdge> inputEdges_;
    std::vector<dsp::DelayLine> edgeDelays_;
    std::vector<EdgeRef> edgeLookup_;
    int compensationSamples_ = 0;
    int compensationEdgeCount_ = 0;
};

/// Immutable, topologically sorted processing plan. Swapped atomically into the
/// running engine so the audio thread sees either the old or the new plan, never
/// a half-built one.
class GraphPlan {
public:
    GraphPlan() = default;

    void addNode(std::shared_ptr<GraphNode> node) { nodes_.push_back(std::move(node)); }
    void setProcessingOrder(std::vector<NodeId> order) { order_ = std::move(order); }
    void setMasterNode(NodeId id) noexcept { masterNode_ = id; }
    void setLatencySamples(int samples) noexcept { latencySamples_ = samples; }
    /// How much delay compensation this plan inserts, and across how many edges.
    void setCompensationStats(int samples, int edges) noexcept {
        compensationSamples_ = samples;
        compensatedEdges_ = edges;
    }

    [[nodiscard]] const std::vector<std::shared_ptr<GraphNode>>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::vector<NodeId>& processingOrder() const noexcept { return order_; }
    [[nodiscard]] NodeId masterNode() const noexcept { return masterNode_; }
    /// Latency of the longest path through the graph, in samples: what the
    /// engine reports and what the transport compensates for. Latency that is
    /// common to every path (the master strip) is added to it separately.
    [[nodiscard]] int latencySamples() const noexcept { return latencySamples_; }
    /// Total compensation delay inserted anywhere in the plan (diagnostics: it is
    /// alignment, not added round-trip latency, so it is NOT part of
    /// latencySamples()).
    [[nodiscard]] int compensationSamples() const noexcept { return compensationSamples_; }
    [[nodiscard]] int compensatedEdges() const noexcept { return compensatedEdges_; }
    [[nodiscard]] GraphNode* find(NodeId id) const noexcept;

    /// Processes the whole plan for `numFrames`. Audio thread.
    void process(int numFrames, const dsp::ProcessContext& context,
                 bool anySoloed) noexcept;

private:
    std::vector<std::shared_ptr<GraphNode>> nodes_;
    std::vector<NodeId> order_;
    NodeId masterNode_ = kInvalidNodeId;
    int latencySamples_ = 0;
    int compensationSamples_ = 0;
    int compensatedEdges_ = 0;
};

/// Builder used by the engine (control thread) to describe routing in plain
/// terms; it produces a GraphPlan with a valid topological order.
class GraphBuilder {
public:
    NodeId createNode(NodeKind kind, std::string name);
    void addNode(std::shared_ptr<GraphNode> node);
    void connect(NodeId source, NodeId destination);
    void setMasterNode(NodeId id) noexcept { masterNode_ = id; }

    /// Delay compensation on/off (mirrors EngineSettings::delayCompensation).
    /// When off, no delays are inserted and parallel paths stay misaligned - the
    /// choice a user makes when they would rather have the lower latency.
    void setDelayCompensationEnabled(bool enabled) noexcept { compensationEnabled_ = enabled; }
    [[nodiscard]] bool delayCompensationEnabled() const noexcept { return compensationEnabled_; }

    /// Sorts nodes topologically (Kahn's algorithm) and returns the plan.
    /// Cycles are broken deterministically and reported through `outError`.
    [[nodiscard]] std::shared_ptr<GraphPlan> build(std::string* outError = nullptr) const;

    [[nodiscard]] const std::vector<std::shared_ptr<GraphNode>>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] GraphNode* find(NodeId id) const noexcept;

private:
    std::vector<std::shared_ptr<GraphNode>> nodes_;
    std::vector<std::pair<NodeId, NodeId>> connections_;
    NodeId nextId_ = 1;
    NodeId masterNode_ = kInvalidNodeId;
    bool compensationEnabled_ = true;
};

} // namespace aura::graph
