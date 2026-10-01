// ============================================================================
// AURA DAW - src/graph/GraphNode.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <algorithm>
#include <cstring>

#include "aura/graph/AudioGraph.hpp"

namespace aura::graph {

const char* nodeKindName(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::Input: return "Input";
    case NodeKind::Track: return "Track";
    case NodeKind::Bus: return "Bus";
    case NodeKind::Send: return "Send";
    case NodeKind::Return: return "Return";
    case NodeKind::Master: return "Master";
    case NodeKind::Output: return "Output";
    case NodeKind::Gain: return "Gain";
    case NodeKind::Pan: return "Pan";
    case NodeKind::Equalizer: return "EQ";
    case NodeKind::Compressor: return "Compressor";
    case NodeKind::Limiter: return "Limiter";
    case NodeKind::Reverb: return "Reverb";
    case NodeKind::Delay: return "Delay";
    case NodeKind::Distortion: return "Distortion";
    case NodeKind::Plugin: return "Plugin";
    }
    return "Node";
}

GraphNode::GraphNode(NodeId id, NodeKind kind, std::string name)
    : id_(id), kind_(kind), name_(std::move(name)) {}

GraphNode::~GraphNode() = default;

void GraphNode::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    // The one-pole fader ramp targets a 10 ms time constant: fast enough to feel
    // instant, slow enough to be inaudible as a click.
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    constexpr double kRampSeconds = 0.010;
    smoothingCoefficient_ =
        static_cast<float>(1.0 - std::exp(-1.0 / (kRampSeconds * rate)));
    currentGainL_ = targetGainL_;
    currentGainR_ = targetGainR_;
    numChannels_ = numChannels > 0 ? numChannels : 2;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 512;

    storage_.assign(static_cast<std::size_t>(numChannels_) * static_cast<std::size_t>(maxBlockSize_),
                    0.0f);
    channelPointers_.resize(static_cast<std::size_t>(numChannels_));
    for (int channel = 0; channel < numChannels_; ++channel) {
        channelPointers_[static_cast<std::size_t>(channel)] =
            storage_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(maxBlockSize_);
    }
    block_.channelPointers = channelPointers_.data();
    block_.numChannels = numChannels_;
    block_.numFrames = maxBlockSize_;

    // Prepare the insert chain. The graph does not own the processors (the engine
    // or the plug-in host does), but it is the graph that runs them, and prepare()
    // is where a processor sizes its internal state - a look-ahead limiter's
    // lookahead buffer, a filter's coefficients, a delay's ring. Node prepare() is
    // the only control-thread call that knows the sample rate, block size and
    // channel count the chain will actually run with, so it is the right place to
    // do it. Processors must treat repeated prepare() as legal: a device change,
    // a sample-rate change or a rebuild all land here again.
    for (std::size_t i = 0; i < inserts_.size(); ++i) {
        if (auto* processor = inserts_.at(i))
            processor->prepare(rate, maxBlockSize_, numChannels_);
    }

    // Delay compensation: one line per incoming edge, sized to exactly what that
    // edge needs. A node with no compensation allocates one float per edge, which
    // keeps the "no latent processors in this session" case at zero cost.
    edgeDelays_.clear();
    edgeDelays_.resize(inputEdges_.size());
    for (std::size_t i = 0; i < inputEdges_.size(); ++i)
        edgeDelays_[i].prepare(inputEdges_[i].compensationSamples);

    // The pre-fader tap, when this node feeds a pre-fader edge. Allocated here (the
    // control thread) and reused forever; a node without pre-fader sends keeps an
    // empty buffer and pays one bool test per block.
    if (preFaderTapRequired_) {
        preFaderStorage_.assign(static_cast<std::size_t>(numChannels_) * static_cast<std::size_t>(maxBlockSize_),
                                0.0f);
        preFaderPointers_.resize(static_cast<std::size_t>(numChannels_));
        for (int channel = 0; channel < numChannels_; ++channel) {
            preFaderPointers_[static_cast<std::size_t>(channel)] =
                preFaderStorage_.data() +
                static_cast<std::size_t>(channel) * static_cast<std::size_t>(maxBlockSize_);
        }
    } else {
        preFaderStorage_.clear();
        preFaderPointers_.clear();
    }

    // A source-indexed view of the edge table, sorted once here so that sumFrom()
    // can binary-search it on the audio thread. A linear scan would be O(inputs)
    // per incoming connection - for a master fed by 48 tracks that is 2,304 id
    // comparisons per block, for nothing. Sorting happens on the control thread
    // and the search allocates nothing.
    edgeLookup_.clear();
    edgeLookup_.reserve(inputEdges_.size());
    for (std::size_t i = 0; i < inputEdges_.size(); ++i)
        edgeLookup_.push_back(EdgeRef{inputEdges_[i].source, static_cast<std::uint32_t>(i)});
    std::sort(edgeLookup_.begin(), edgeLookup_.end(),
              [](const EdgeRef& a, const EdgeRef& b) { return a.source < b.source; });

    prepared_ = true;
    reset();
}

void GraphNode::reset() noexcept {
    std::fill(storage_.begin(), storage_.end(), 0.0f);
    for (std::size_t i = 0; i < inserts_.size(); ++i) {
        if (auto* processor = inserts_.at(i))
            processor->reset();
    }
    inputActive_ = false;
    // A stale compensation delay would smear the first block after a seek or a
    // rebuild, so the lines are cleared with the rest of the node state.
    for (auto& line : edgeDelays_)
        line.reset();
}

void GraphNode::clear(int numFrames) noexcept {
    if (!prepared_)
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    for (int channel = 0; channel < numChannels_; ++channel) {
        std::memset(channelPointers_[static_cast<std::size_t>(channel)], 0,
                    static_cast<std::size_t>(frames) * sizeof(float));
    }
}

void GraphNode::applyGainSmoothing(int numFrames) noexcept {
    const int frames = std::min(numFrames, maxBlockSize_);
    if (frames <= 0)
        return;

    for (int channel = 0; channel < numChannels_; ++channel) {
        float* data = channelPointers_[static_cast<std::size_t>(channel)];
        float current = channel == 0 ? currentGainL_ : currentGainR_;
        const float target = channel == 0 ? targetGainL_ : targetGainR_;

        if (current == target) {
            if (current != 1.0f) {
                for (int i = 0; i < frames; ++i)
                    data[i] *= current;
            }
        } else {
            // The ramp is the same first-order recursion the level meter uses: a
            // per-sample loop-carried dependency that no compiler can vectorise.
            // `applyOnePoleRamp` evaluates it four samples at a time from the
            // closed form of the group, so the four multiplies are independent.
            // Same ramp, same end point (the closed form is the recursion, not an
            // approximation of it).
            current = math::applyOnePoleRamp(data, frames, current, target,
                                            1.0f - smoothingCoefficient_);
        }
        if (channel == 0)
            currentGainL_ = current;
        else
            currentGainR_ = current;
    }
}

void GraphNode::capturePreFaderTap(int numFrames) noexcept {
    if (!prepared_ || preFaderPointers_.empty())
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    if (frames <= 0)
        return;
    for (int channel = 0; channel < numChannels_; ++channel) {
        std::memcpy(preFaderStorage_.data() +
                        static_cast<std::size_t>(channel) * static_cast<std::size_t>(maxBlockSize_),
                    channelPointers_[static_cast<std::size_t>(channel)],
                    static_cast<std::size_t>(frames) * sizeof(float));
    }
}

void GraphNode::clearPreFaderTap(int numFrames) noexcept {
    if (preFaderPointers_.empty())
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    if (frames <= 0)
        return;
    for (int channel = 0; channel < numChannels_; ++channel) {
        std::fill_n(preFaderStorage_.data() +
                        static_cast<std::size_t>(channel) * static_cast<std::size_t>(maxBlockSize_),
                    frames, 0.0f);
    }
}

void GraphNode::sumFrom(const GraphNode& source, int numFrames) noexcept {
    if (!prepared_ || !source.prepared_)
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    const int channels = std::min(numChannels_, source.numChannels_);

    // Delay compensation is applied here, on the connection, because that is
    // where the misalignment exists: the source has already added the latency of
    // its own insert chain, and this edge is short by (latest input - source
    // path). The lookup is a binary search over the edge table sorted in
    // prepare(): no allocation, no hashing, and a cost that grows with log(inputs)
    // rather than with the session size.
    if (!edgeLookup_.empty()) {
        const auto found = std::lower_bound(
            edgeLookup_.begin(), edgeLookup_.end(), source.id(),
            [](const EdgeRef& ref, NodeId id) { return ref.source < id; });
        if (found != edgeLookup_.end() && found->source == source.id() &&
            found->index < edgeDelays_.size()) {
            const InputEdge& edge = inputEdges_[found->index];
            // A send at zero level carries nothing: skip it entirely rather than
            // running a block of multiply-accumulate into silence.
            if (edge.gain == 0.0f)
                return;

            // Pre-fader edges read the tap; everything else reads the output. A
            // pre-fader edge whose source has no tap (unprepared, or the source was
            // rebuilt without it) reads nothing rather than the wrong signal.
            const float* const* input = source.channelPointers();
            if (edge.preFader) {
                if (!source.hasPreFaderTap())
                    return;
                input = source.preFaderPointers();
            }

            const int delay = edge.compensationSamples;
            if (delay > 0 || edge.gain != 1.0f) {
                for (int channel = 0; channel < channels; ++channel) {
                    edgeDelays_[found->index].sumDelayed(
                        input[channel], channelPointers_[static_cast<std::size_t>(channel)], frames,
                        delay, edge.gain);
                }
                if (frames > 0)
                    inputActive_ = true;
                return;
            }

            // Plain unity path: same loop, now with the edge's tap choice.
            for (int channel = 0; channel < channels; ++channel) {
                float* destination = channelPointers_[static_cast<std::size_t>(channel)];
                const float* sourceChannel = input[channel];
                for (int i = 0; i < frames; ++i)
                    destination[i] += sourceChannel[i];
            }
            if (frames > 0)
                inputActive_ = true;
            return;
        }
    }

    // No edge entry (a source that feeds us without being in the table): unity sum.
    for (int channel = 0; channel < channels; ++channel) {
        float* destination = channelPointers_[static_cast<std::size_t>(channel)];
        const float* input = source.channelPointers()[channel];
        for (int i = 0; i < frames; ++i)
            destination[i] += input[i];
    }
    if (frames > 0)
        inputActive_ = true;
}

void GraphNode::copyFrom(const GraphNode& source, int numFrames) noexcept {
    if (!prepared_ || !source.prepared_)
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    const int channels = std::min(numChannels_, source.numChannels_);
    for (int channel = 0; channel < channels; ++channel) {
        std::memcpy(channelPointers_[static_cast<std::size_t>(channel)],
                    source.channelPointers()[channel],
                    static_cast<std::size_t>(frames) * sizeof(float));
    }
    inputActive_ = frames > 0;
}

bool GraphNode::isActive() const noexcept {
    if (muted_)
        return false;
    if (inputActive_)
        return true;
    // A node whose inserts still have a tail (reverb, delay) must keep being
    // processed even with silent input, otherwise the tail is truncated.
    return inserts_.anyActive() && kind_ != NodeKind::Input;
}

void GraphNode::InsertChain::remove(dsp::Processor* processor) {
    processors_.erase(std::remove(processors_.begin(), processors_.end(), processor),
                      processors_.end());
}

// ---------------------------------------------------------------------------
// GraphPlan
// ---------------------------------------------------------------------------
GraphNode* GraphPlan::find(NodeId id) const noexcept {
    for (const auto& node : nodes_) {
        if (node && node->id() == id)
            return node.get();
    }
    return nullptr;
}

void GraphPlan::process(int numFrames, const dsp::ProcessContext& context,
                        bool anySoloed) noexcept {
    // Pass 1 - clear every accumulator. A node with incoming connections, and the
    // master node, must start each block from silence: otherwise the previous
    // block's audio is still in the buffer and every sum accumulates forever.
    // Source nodes (tracks fed by clips/instruments) are deliberately NOT cleared
    // here; the engine has already filled them with this block's audio.
    for (const NodeId id : order_) {
        GraphNode* node = find(id);
        if (!node || !node->isPrepared())
            continue;
        if (node->incomingConnections() > 0 || id == masterNode_)
            node->clear(numFrames);
    }

    for (const NodeId id : order_) {
        GraphNode* node = find(id);
        if (!node || !node->isPrepared())
            continue;

        const bool soloMuted = anySoloed && !node->isSoloed() &&
                               (node->kind() == NodeKind::Track || node->kind() == NodeKind::Return);
        const bool active = node->isActive() && !soloMuted;

        if (!active && !node->inserts().anyActive()) {
            // Silent and no tail: clear outputs so a previous block's audio is
            // never recycled, and skip the DSP entirely. The pre-fader tap is
            // cleared too, or a cue mix would keep hearing the last block the
            // track produced.
            node->clear(numFrames);
            node->clearPreFaderTap(numFrames);
            continue;
        }

        // Inserts first, then the fader. The fader is the last thing before the
        // output, so riding it cannot change what the inserts do - the classic
        // reason for this order is that the same move on the fader must not
        // change how hard a saturator is driven or where a compressor works.
        // (This engine had it the other way round: the inserts were fed from the
        // post-fader signal, so a fader move re-voiced every non-linear insert.)
        dsp::AudioBlockView block = node->mutableBlock();
        block.numFrames = numFrames;
        node->inserts().process(block, context);

        // Pre-fader tap: the signal after the inserts, before the fader, pan and
        // mute. This is what a pre-fader send reads.
        if (node->preFaderTapRequired())
            node->capturePreFaderTap(numFrames);

        // Fader last, ramped so automation and mute changes never click. A
        // `muted_` node is a hard cut used by renderers/tests; the mixer expresses
        // mute as a ramp to zero so it fades instead of popping - and either way a
        // pre-fader send is unaffected, which is the point of a cue mix.
        node->applyGainSmoothing(numFrames);

        // Route to destinations.
        for (const NodeId destinationId : node->destinations()) {
            GraphNode* destination = find(destinationId);
            if (destination)
                destination->sumFrom(*node, numFrames);
        }

        // A master node is terminal; its destinations are the output device.
    }
}

// ---------------------------------------------------------------------------
// GraphBuilder
// ---------------------------------------------------------------------------
NodeId GraphBuilder::createNode(NodeKind kind, std::string name) {
    const NodeId id = nextId_++;
    nodes_.push_back(std::make_shared<GraphNode>(id, kind, std::move(name)));
    return id;
}

void GraphBuilder::addNode(std::shared_ptr<GraphNode> node) {
    if (!node)
        return;
    if (node->id() >= nextId_)
        nextId_ = node->id() + 1;
    nodes_.push_back(std::move(node));
}

GraphNode* GraphBuilder::find(NodeId id) const noexcept {
    for (const auto& node : nodes_) {
        if (node && node->id() == id)
            return node.get();
    }
    return nullptr;
}

void GraphBuilder::connect(NodeId source, NodeId destination, float gain, bool preFader) {
    if (source == kInvalidNodeId || destination == kInvalidNodeId || source == destination)
        return;
    if (!(gain > 0.0f))
        gain = 0.0f; // NaN-safe; a zero-gain edge is silent but still routed
    if (gain > 1.0f)
        gain = 1.0f; // a connection cannot amplify; sends are 0..1

    // One edge per (source, destination) pair. A repeated connect() must not add a
    // second edge: the audio would be summed twice and the in-degree that drives
    // Kahn's ordering (and the incoming-connection count that drives clearing)
    // would be inflated. Two nominal edges with the same endpoints do sum, though,
    // so their gains add (a unity route plus a send into the same destination).
    for (Connection& connection : connections_) {
        if (connection.source == source && connection.destination == destination) {
            connection.gain = std::min(1.0f, connection.gain + gain);
            connection.preFader = connection.preFader && preFader; // two taps that disagree = the louder one
            return; // the edge table is rebuilt from this list anyway
        }
    }

    connections_.push_back(Connection{source, destination, gain, preFader});
    if (GraphNode* node = find(source))
        node->addDestination(destination);
}

std::shared_ptr<GraphPlan> GraphBuilder::build(std::string* outError) const {
    auto plan = std::make_shared<GraphPlan>();
    for (const auto& node : nodes_)
        plan->addNode(node);

    // Adjacency + in-degree for Kahn's algorithm.
    std::vector<std::vector<NodeId>> adjacency(nodes_.size() + 1);
    std::vector<int> inDegree(nodes_.size() + 1, 0);
    const auto indexOf = [this](NodeId id) -> std::size_t {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i] && nodes_[i]->id() == id)
                return i;
        }
        return nodes_.size(); // sentinel (ignored)
    };

    for (const Connection& connection : connections_) {
        const NodeId source = connection.source;
        const NodeId destination = connection.destination;
        const std::size_t s = indexOf(source);
        const std::size_t d = indexOf(destination);
        if (s >= nodes_.size() || d >= nodes_.size())
            continue;
        adjacency[s].push_back(destination);
        ++inDegree[d];
        // Record the fan-in on the destination node: GraphPlan uses it to decide
        // which nodes must be cleared at the start of every block.
        if (nodes_[d])
            nodes_[d]->setIncomingConnections(nodes_[d]->incomingConnections() + 1);
    }

    // A source with a pre-fader edge has to keep a second buffer: tell it before
    // prepare() so the allocation happens once, on the control thread.
    for (const Connection& connection : connections_) {
        if (!connection.preFader)
            continue;
        if (GraphNode* source = plan->find(connection.source))
            source->setPreFaderTapRequired(true);
    }

    // Deterministic order: process nodes in creation order when several are
    // ready, which keeps renders reproducible (important for tests and for
    // byte-identical project exports).
    std::vector<NodeId> order;
    std::vector<bool> visited(nodes_.size(), false);
    while (order.size() < nodes_.size()) {
        bool progressed = false;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (visited[i] || inDegree[i] != 0)
                continue;
            visited[i] = true;
            order.push_back(nodes_[i]->id());
            for (const NodeId destination : adjacency[i]) {
                const std::size_t d = indexOf(destination);
                if (d < nodes_.size())
                    --inDegree[d];
            }
            progressed = true;
        }
        if (!progressed) {
            // A cycle exists (e.g. a feedback send). Break it by emitting the
            // remaining nodes in creation order and reporting the problem.
            if (outError && outError->empty())
                *outError = "audio graph contains a routing cycle; remaining nodes were "
                            "processed in creation order";
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (!visited[i]) {
                    visited[i] = true;
                    order.push_back(nodes_[i]->id());
                }
            }
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Delay compensation
    // -----------------------------------------------------------------------
    // Every insert chain adds latency to the signal that passes through it. When
    // two paths meet - a dry path and a path through a look-ahead limiter, a send
    // alongside the direct out - the shorter one has to be delayed by the
    // difference, or the two arrive misaligned and comb-filter.
    //
    // The walk is a single pass over the topological order:
    //   pathLatency[node] = max(pathLatency[source]) + node's own insert latency
    //   edge delay        = max(pathLatency[source]) - pathLatency[source]
    // which leaves every input of a node arriving together, and the latency of
    // the longest chain (the one the engine reports) unchanged: compensation
    // moves signals earlier in the mix, it does not shorten the chain.
    //
    // Node kinds are irrelevant here: a bus, an aux return and a track are all
    // "a node with inputs and a chain". The one thing that cannot be compensated
    // is a live input - you cannot play a musician before they play - so input
    // monitoring is deliberately outside the plan (see docs/AUDIO_ENGINE.md).
    std::vector<std::pair<NodeId, int>> pathLatency;
    const auto latencyOf = [&pathLatency](NodeId id) {
        for (const auto& [node, latency] : pathLatency) {
            if (node == id)
                return latency;
        }
        return 0;
    };

    int compensationTotal = 0;
    int compensatedEdges = 0;
    int longestPath = 0;
    for (const NodeId id : order) {
        GraphNode* node = plan->find(id);
        if (!node)
            continue;

        std::vector<InputEdge> edges;
        int latestInput = 0;
        for (const Connection& connection : connections_) {
            if (connection.destination != id || !plan->find(connection.source))
                continue;
            const int latency = latencyOf(connection.source);
            latestInput = std::max(latestInput, latency);
            edges.push_back(InputEdge{connection.source, 0, connection.gain, connection.preFader});
        }
        for (InputEdge& edge : edges) {
            edge.compensationSamples =
                compensationEnabled_ ? std::max(0, latestInput - latencyOf(edge.source)) : 0;
        }
        node->setInputEdges(std::move(edges));
        for (const InputEdge& edge : node->inputEdges()) {
            if (edge.compensationSamples > 0) {
                compensationTotal += edge.compensationSamples;
                ++compensatedEdges;
            }
        }

        const int path = latestInput + node->inserts().totalLatencySamples();
        pathLatency.emplace_back(id, path);
        if (masterNode_ == kInvalidNodeId || id == masterNode_)
            longestPath = std::max(longestPath, path);
    }

    plan->setLatencySamples(longestPath);
    plan->setCompensationStats(compensationTotal, compensatedEdges);
    plan->setProcessingOrder(std::move(order));
    plan->setMasterNode(masterNode_);
    return plan;
}

} // namespace aura::graph
