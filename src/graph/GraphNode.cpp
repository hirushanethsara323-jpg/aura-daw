// ============================================================================
// AURA DAW - src/graph/GraphNode.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
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
            const float coefficient = smoothingCoefficient_;
            for (int i = 0; i < frames; ++i) {
                current += (target - current) * coefficient;
                data[i] *= current;
            }
        }
        if (channel == 0)
            currentGainL_ = current;
        else
            currentGainR_ = current;
    }
}

void GraphNode::sumFrom(const GraphNode& source, int numFrames) noexcept {
    if (!prepared_ || !source.prepared_)
        return;
    const int frames = std::min(numFrames, maxBlockSize_);
    const int channels = std::min(numChannels_, source.numChannels_);
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
            // never recycled, and skip the DSP entirely.
            node->clear(numFrames);
            continue;
        }

        // Fader before the insert chain (the standard DAW model), ramped so
        // automation and mute changes never click. A `muted_` node is a hard cut
        // used by renderers/tests; the mixer expresses mute as a ramp to zero so
        // it fades instead of popping.
        node->applyGainSmoothing(numFrames);

        dsp::AudioBlockView block = node->mutableBlock();
        block.numFrames = numFrames;
        node->inserts().process(block, context);

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

void GraphBuilder::connect(NodeId source, NodeId destination) {
    if (source == kInvalidNodeId || destination == kInvalidNodeId || source == destination)
        return;
    connections_.emplace_back(source, destination);
    if (GraphNode* node = find(source))
        node->setDestinations({destination});
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

    for (const auto& [source, destination] : connections_) {
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

    plan->setProcessingOrder(std::move(order));
    plan->setMasterNode(masterNode_);

    int latency = 0;
    for (const auto& node : nodes_) {
        if (node)
            latency = std::max(latency, node->inserts().totalLatencySamples());
    }
    plan->setLatencySamples(latency);
    return plan;
}

} // namespace aura::graph
