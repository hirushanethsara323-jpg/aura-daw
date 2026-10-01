// ============================================================================
// AURA DAW - src/engine/AudioEngine.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/engine/AudioEngine.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>

#include "aura/core/Log.hpp"
#include "aura/core/Sort.hpp"
#include "aura/core/Realtime.hpp"
#include "aura/audio/AudioClip.hpp"
#include "aura/core/Version.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/project/Project.hpp"
#include "aura/midi/Instrument.hpp"

namespace aura::engine {
namespace {
constexpr const char* kCategory = "Engine";

std::uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

AudioEngine::AudioEngine() {
    // Preallocated once: the MIDI collection path must never allocate on the
    // audio thread, so the buffer is reserved with generous headroom.
    midiScratch_.reserve(2048);
}

AudioEngine::~AudioEngine() {
    shutdown();
}

Status AudioEngine::initialise(IAudioDevice* device, const EngineSettings& settings) {
    if (!device)
        return failure(makeError(ErrorCode::InvalidArgument, "No audio device supplied"));

    device_ = device;
    settings_ = settings;
    transport_.prepare(settings.sampleRate);
    recorder_.setSettings([&] {
        audio::RecordingSettings recording;
        recording.sampleRate = static_cast<std::uint32_t>(settings.sampleRate);
        recording.numChannels = std::min(2, settings.numOutputChannels);
        return recording;
    }());
    recorder_.prepare();

    if (project_) {
        project_->mixer().prepare(settings.sampleRate, settings.bufferSize, settings.numOutputChannels);
        AURA_RETURN_ON_ERROR(rebuildGraph());
    }

    // The device callback is the audio thread: mark it so RT assertions and the
    // allocation counter know what they are looking at.
    const Status started = device_->start(
        // The device reports its channel counts on every callback; the engine
        // already knows them from prepare(), so they are deliberately unnamed
        // here rather than carried around unused (MSVC /W4 C4100 and GCC
        // -Wunused-parameter both reject unused names, which keeps the two
        // compilers honest about what this callback actually consumes).
        [this](float* const* outputs, const float* const* inputs, int /*numChannels*/,
               int /*numInputChannels*/, int numFrames) {
            rt::enterRealtimeThread();
            rt::ScopedNoDenormals noDenormals;
            processBlock(outputs, inputs, numFrames);
            rt::exitRealtimeThread();
        });
    if (!started) {
        device_ = nullptr;
        return started;
    }

    running_.store(true, std::memory_order_release);
    AURA_LOG_INFO(kCategory, "Engine running: %s, %.0f Hz, %d frames, %d out / %d in channels",
                  device_->name().c_str(), settings.sampleRate, settings.bufferSize,
                  settings.numOutputChannels, settings.numInputChannels);
    return success();
}

void AudioEngine::shutdown() {
    if (device_) {
        device_->stop();
        device_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    if (recorder_.isRecording())
        (void)recorder_.stopTake();
}

void AudioEngine::setProject(project::Project* project) {
    project_ = project;
    if (project_ && device_) {
        (void)rebuildGraph();
    }
}

void AudioEngine::setSettings(const EngineSettings& settings) {
    settings_ = settings;
    transport_.prepare(settings.sampleRate);
    if (project_)
        project_->mixer().prepare(settings.sampleRate, settings.bufferSize, settings.numOutputChannels);
}

void AudioEngine::pruneRetiredPlans() {
    // A retired plan is safe to free once two full blocks have been processed
    // since it was replaced: no block can still be holding the old pointer.
    constexpr std::uint64_t kGraceBlocks = 2;
    const std::uint64_t blocks = blocksProcessedAtomic_.load(std::memory_order_acquire);
    retiredPlans_.erase(
        std::remove_if(retiredPlans_.begin(), retiredPlans_.end(),
                       [blocks](const std::shared_ptr<graph::GraphPlan>&) {
                           return blocks >= kGraceBlocks;
                       }),
        retiredPlans_.end());
}

void AudioEngine::publishPlan(std::shared_ptr<graph::GraphPlan> plan) {
    pruneRetiredPlans();
    if (activePlan_)
        retiredPlans_.push_back(activePlan_); // kept alive for the grace window
    activePlan_ = std::move(plan);
    livePlan_.store(activePlan_.get(), std::memory_order_release);
}

Status AudioEngine::rebuildGraph() {
    if (!project_)
        return failure(makeError(ErrorCode::NotFound,
                                 "No project is attached to the engine"));

    audio::Mixer& mixer = project_->mixer();
    mixer.prepare(settings_.sampleRate, settings_.bufferSize, settings_.numOutputChannels);

    graph::GraphBuilder builder;
    std::unordered_map<audio::TrackId, graph::NodeId> nodeIds;

    // One graph node per track, plus a master node.
    for (audio::Track* track : mixer.tracks()) {
        if (!track)
            continue;
        const graph::NodeId id = builder.createNode(
            track->kind() == audio::TrackKind::Bus
                ? graph::NodeKind::Bus
                : (track->kind() == audio::TrackKind::Aux ? graph::NodeKind::Return
                                                          : graph::NodeKind::Track),
            track->name());
        nodeIds[track->id()] = id;
        track->setGraphNodeId(id);
        if (graph::GraphNode* node = builder.find(id))
            node->setSoloed(track->isSoloed());
    }
    const graph::NodeId masterId = builder.createNode(graph::NodeKind::Master, "Master");
    nodeIds[mixer.masterId()] = masterId;
    mixer.master().setGraphNodeId(masterId);
    builder.setMasterNode(masterId);

    // Routing: each track -> its destination (or master), plus sends.
    for (audio::Track* track : mixer.tracks()) {
        if (!track)
            continue;
        const graph::NodeId source = nodeIds[track->id()];
        const audio::TrackId destinationId = track->routing().destination != audio::kInvalidTrackId
                                                 ? track->routing().destination
                                                 : mixer.masterId();
        const auto found = nodeIds.find(destinationId);
        builder.connect(source, found != nodeIds.end() ? found->second : masterId);

        for (const auto& send : track->sends()) {
            if (!send.enabled)
                continue;
            const auto sendTarget = nodeIds.find(send.destination);
            if (sendTarget != nodeIds.end())
                builder.connect(source, sendTarget->second);
        }
    }

    // Inserts are attached to the builder's nodes *before* build(), because the
    // builder computes each node's insert latency to work out the compensation
    // delays. Attaching them afterwards (as this used to) left the builder
    // believing every chain was latency-free.
    for (audio::Track* track : mixer.tracks()) {
        if (!track)
            continue;
        const auto found = nodeIds.find(track->id());
        if (found == nodeIds.end())
            continue;
        if (graph::GraphNode* node = builder.find(found->second)) {
            node->inserts().clear();
            for (auto& slot : track->inserts()) {
                if (slot.processor)
                    node->inserts().add(slot.processor);
            }
        }
    }

    builder.setDelayCompensationEnabled(settings_.delayCompensation);

    std::string error;
    auto plan = builder.build(&error);
    if (!error.empty())
        AURA_LOG_WARN(kCategory, "Graph build note: %s", error.c_str());

    // Every node must be prepared before it goes live.
    const int channels = std::max(2, settings_.numOutputChannels);
    for (const auto& node : plan->nodes()) {
        if (node)
            node->prepare(settings_.sampleRate, settings_.bufferSize, channels);
    }

    const std::size_t nodeCount = plan->nodes().size();
    const int planLatency = plan->latencySamples();
    const int compensation = plan->compensationSamples();
    const int compensatedEdges = plan->compensatedEdges();
    publishPlan(std::move(plan));

    AURA_LOG_INFO(kCategory,
                  "Graph rebuilt: %zu nodes, longest path %d samples, compensation %d samples "
                  "across %d connection(s), master strip %d samples",
                  nodeCount, planLatency, compensation, compensatedEdges,
                  mixer.masterLatencySamples());
    return success();
}

int AudioEngine::totalLatencySamples() const noexcept {
    // Accumulated in 64-bit; narrowing happens once, saturated, at the end.
    std::int64_t total = device_ ? device_->latencySamples() : 0;
    // The plan reports the latency of its longest path, which already includes
    // the insert chains. Compensation delays are deliberately not added on top:
    // they move the *shorter* paths later so everything lines up, they do not
    // lengthen the chain the audio actually travels.
    if (activePlan_)
        total += activePlan_->latencySamples();
    // The master strip runs after the graph (step 5 of processBlock), so it is
    // part of the round trip but not part of the plan.
    if (project_)
        total += project_->mixer().masterLatencySamples();
    // The engine reports latency as int everywhere (device APIs and the graph
    // plans); saturate instead of wrapping if a pathological chain ever exceeds
    // INT_MAX samples of delay.
    return static_cast<int>(std::clamp<std::int64_t>(total, 0, std::numeric_limits<int>::max()));
}

void AudioEngine::applyMixerStateToGraph() noexcept {
    if (!project_)
        return;
    graph::GraphPlan* plan = livePlan();
    if (!plan)
        return;

    audio::Mixer& mixer = project_->mixer();
    for (audio::Track* track : mixer.allTracksIncludingMaster()) {
        if (!track)
            continue;
        graph::GraphNode* node = plan->find(track->graphNodeId());
        if (!node)
            continue;

        const float volume = track->volume();
        const float pan = track->pan();

        // Stereo balance law: centre leaves both channels untouched, hard left
        // silences the right channel and leaves the left at unity. (An
        // equal-power law would add +3 dB at centre for already-stereo material,
        // which is not what a balance control should do.)
        const float panLeft = pan <= 0.0f ? 1.0f : 1.0f - pan;
        const float panRight = pan >= 0.0f ? 1.0f : 1.0f + pan;

        // Mute is expressed as a ramp to silence rather than a hard cut, so it
        // fades instead of popping. `muted_` stays false on purpose: the plan
        // then keeps processing the strip (its inserts' tails decay naturally).
        const float muteGain = track->isMuted() ? 0.0f : 1.0f;

        node->setTargetGains(volume * panLeft * muteGain, volume * panRight * muteGain);
        node->setSoloed(track->isSoloed());
    }
}

void AudioEngine::renderClips(int numFrames, const dsp::ProcessContext& /*context*/) noexcept {
    if (!project_)
        return;
    audio::Mixer& mixer = project_->mixer();

    // For every track, sum the audio of every clip that overlaps this block.
    for (audio::Track* track : mixer.tracks()) {
        if (!track || track->kind() != audio::TrackKind::Audio)
            continue;
        graph::GraphPlan* plan = livePlan();
        graph::GraphNode* node = plan ? plan->find(track->graphNodeId()) : nullptr;
        if (!node)
            continue;

        node->clear(numFrames);
        bool wroteAnything = false;
        for (const std::uint64_t clipId : track->clips()) {
            audio::Clip* clip = project_->findClip(clipId);
            if (!clip || !clip->isReady())
                continue;
            const int rendered = clip->render(transport_.position(), numFrames,
                                              node->mutableChannelPointers(), node->numChannels());
            if (rendered > 0)
                wroteAnything = true;
        }
        // Only mark the node as an active source when it actually produced audio:
        // that is what lets the graph skip silent tracks and still be correct.
        node->setInputActive(wroteAnything);
    }
}

void AudioEngine::renderInstruments(int numFrames, const dsp::ProcessContext& context) noexcept {
    if (!project_)
        return;
    graph::GraphPlan* plan = livePlan();
    if (!plan)
        return;

    for (audio::Track* track : project_->mixer().tracks()) {
        if (!track)
            continue;
        const bool isMidiTrack = track->kind() == audio::TrackKind::Midi ||
                                 track->kind() == audio::TrackKind::Instrument;
        if (!isMidiTrack)
            continue;
        graph::GraphNode* node = plan->find(track->graphNodeId());
        if (!node)
            continue;

        node->clear(numFrames);
        midi::Instrument* instrument = track->instrument();
        if (!instrument) {
            node->setInputActive(false);
            continue;
        }

        // Collect the clip events that fall inside this block. The tick range is
        // derived from the tempo map so a tempo change inside the block is
        // handled at block resolution (per-sample tempo ramps are deliberately
        // not supported - tempo is a step function in AURA).
        // Bounded collection: the scratch is preallocated, and a pathological
        // clip can never make the audio thread allocate - the extra events are
        // dropped at the block boundary instead.
        constexpr std::size_t kMaxMidiEventsPerBlock = 1024;
        midiScratch_.clear();
        const std::int64_t startTicks = context.positionTicks;
        const std::int64_t endTicks = project_->tempoMap().samplesToTicks(
            context.positionSamples + numFrames, settings_.sampleRate);

        for (const std::uint64_t clipId : track->clips()) {
            midi::MidiClip* clip = project_->findMidiClip(clipId);
            if (!clip || clip->isMuted())
                continue;
            // The clip owns its notes in clip-relative ticks; the engine offsets
            // them into absolute ticks and then into sample offsets.
            if (midiScratch_.size() >= kMaxMidiEventsPerBlock)
                break;
            const std::size_t before = midiScratch_.size();
            clip->collectEvents(startTicks - clip->startTicks(),
                                endTicks - clip->startTicks(), midiScratch_);
            for (std::size_t i = before; i < midiScratch_.size(); ++i) {
                midi::Message& message = midiScratch_[i];
                const std::int64_t absoluteTicks = clip->startTicks() + message.timestampTicks;
                const time::SamplePos sample =
                    project_->tempoMap().ticksToSamples(absoluteTicks, settings_.sampleRate);
                message.sampleOffset = std::clamp<std::int64_t>(sample - context.positionSamples, 0,
                                                                numFrames);
            }
        }
        // Events must be in block order: collectEvents() returns clip-relative
        // sorted events, and multiple clips can interleave, so sort by offset.
        // core::stableSortSmall, not std::stable_sort: the latter allocates a
        // temporary buffer, which is not allowed on the audio thread. The block's
        // event list is a handful of items, and stability matters (the per-clip
        // order of events sharing a timestamp must survive).
        core::stableSortSmall(midiScratch_.begin(), midiScratch_.end(),
                              [](const midi::Message& a, const midi::Message& b) {
                                  return a.sampleOffset < b.sampleOffset;
                              });

        dsp::AudioBlockView block = node->mutableBlock();
        block.numFrames = numFrames;
        instrument->process(block, midiScratch_.data(), static_cast<int>(midiScratch_.size()),
                            context);
        node->setInputActive(instrument->isProducingSound());
    }
}

void AudioEngine::renderRecording(int numFrames, const float* const* inputs,
                                  int numInputChannels) noexcept {
    if (!inputs || numInputChannels <= 0)
        return;
    if (recorder_.isRecording() && transport_.isInsidePunchWindow())
        recorder_.processInput(inputs, numInputChannels, numFrames);
}

void AudioEngine::processBlock(float* const* outputs, const float* const* inputs,
                               int numFrames) noexcept {
    const std::uint64_t blockStart = nowMicros();

    if (!outputs || numFrames <= 0)
        return;

    // --- 1. commands ---------------------------------------------------------
    commands_.drain(32, [this](const EngineCommand& command) {
        switch (command.type) {
        case EngineCommandType::TransportPlay: transport_.play(); break;
        case EngineCommandType::TransportStop:
            transport_.stop();
            if (recorder_.isRecording())
                (void)recorder_.stopTake();
            break;
        case EngineCommandType::TransportPause: transport_.pause(); break;
        case EngineCommandType::TransportRecord: (void)transport_.record(); break;
        case EngineCommandType::TransportSeek: transport_.seek(command.value64); break;
        case EngineCommandType::SetLoopRange:
            // index carries the low half, value64 the end position.
            transport_.setLoopRange(command.value64, command.index);
            break;
        case EngineCommandType::SetClearLoop: transport_.clearLoopRange(); break;
        case EngineCommandType::SetMetronome:
            transport_.mutableSettings().metronomeEnabled = command.value > 0.5f;
            break;
        case EngineCommandType::PanicMidi: break; // handled by the MIDI router
        case EngineCommandType::SwapPlan:
            // Plan swaps are published from the control thread through
            // publishPlan(), which keeps the old plan alive for a grace window.
            // Doing it from here would require an allocation on the audio thread.
            break;
        default: break;
        }
    });

    // --- 2/3. clip sources ---------------------------------------------------
    dsp::ProcessContext context;
    context.sampleRate = settings_.sampleRate;
    context.positionSamples = transport_.position();
    context.isPlaying = transport_.isPlaying();
    if (project_) {
        context.positionTicks =
            project_->tempoMap().samplesToTicks(context.positionSamples, settings_.sampleRate);
        context.tempoBpm = project_->tempoMap().tempoAt(context.positionTicks);
    }

    graph::GraphPlan* plan = livePlan(); // loaded once; see publishPlan()

    applyMixerStateToGraph();

    const bool playing = transport_.isPlaying();
    if (playing) {
        renderClips(numFrames, context);
        renderInstruments(numFrames, context);
    } else if (plan) {
        // Stopped: still process so reverb/delay tails decay naturally instead
        // of being cut off, but do not introduce new clip audio.
        //
        // Source nodes (the ones nothing else feeds) are written by the engine
        // itself, so their buffers still hold whatever the last playing block
        // left there. They must be cleared here: otherwise the graph keeps
        // re-playing that stale block and the transport never falls silent.
        // Nodes with incoming connections are cleared by GraphPlan::process().
        for (const graph::NodeId id : plan->processingOrder()) {
            graph::GraphNode* node = plan->find(id);
            if (!node || node->id() == plan->masterNode())
                continue;
            if (node->incomingConnections() == 0) {
                node->clear(numFrames);
                node->setInputActive(false);
            }
        }
    }

    renderRecording(numFrames, inputs, settings_.numInputChannels);

    // --- 4. graph ------------------------------------------------------------
    bool anySoloed = false;
    if (project_)
        anySoloed = project_->mixer().anySoloed();
    if (plan)
        plan->process(numFrames, context, anySoloed);

    // --- 5. master to the device --------------------------------------------
    graph::GraphNode* masterNode = plan ? plan->find(plan->masterNode()) : nullptr;
    const int outputChannels = settings_.numOutputChannels;
    if (masterNode) {
        dsp::AudioBlockView masterBlock = masterNode->mutableBlock();
        masterBlock.numFrames = numFrames;

        if (project_) {
            // Master strip: limiter, then metering/loudness.
            dsp::Limiter& limiter = project_->mixer().masterLimiter();
            if (!limiter.isBypassed())
                limiter.process(masterBlock, context);
            project_->mixer().masterMeter().process(masterBlock);
            project_->mixer().masterLoudness().process(masterBlock);
        }

        // Any non-finite sample is a bug somewhere upstream: replace it with
        // silence so it can never reach the user's speakers or a recording.
        dsp::sanitizeBlock(masterBlock);

        for (int channel = 0; channel < outputChannels; ++channel) {
            const int sourceChannel = std::min(channel, masterNode->numChannels() - 1);
            if (masterNode->numChannels() <= 0)
                break;
            std::copy(masterNode->channelPointers()[sourceChannel],
                      masterNode->channelPointers()[sourceChannel] + numFrames, outputs[channel]);
        }
    } else {
        for (int channel = 0; channel < outputChannels; ++channel)
            std::fill(outputs[channel], outputs[channel] + numFrames, 0.0f);
    }

    // --- 6. transport --------------------------------------------------------
    if (project_) {
        const auto advance = transport_.advance(numFrames, project_->tempoMap());
        if (advance.startedRecording)
            (void)recorder_.startTake(advance.jumpPosition, "Take");
        (void)advance;
    }

    blocksProcessedAtomic_.fetch_add(1, std::memory_order_acq_rel);
    updateStatistics(blockStart, numFrames);
}

void AudioEngine::updateStatistics(std::uint64_t blockStartMicros, int numFrames) noexcept {
    const std::uint64_t elapsed = nowMicros() - blockStartMicros;
    const double budgetMicros = (static_cast<double>(numFrames) / settings_.sampleRate) * 1.0e6;
    const float load = budgetMicros > 0.0 ? static_cast<float>(static_cast<double>(elapsed) / budgetMicros) : 0.0f;

    statistics_.cpuLoad.store(load, std::memory_order_relaxed);
    float peak = statistics_.peakCpuLoad.load(std::memory_order_relaxed);
    if (load > peak)
        statistics_.peakCpuLoad.store(load, std::memory_order_relaxed);

    // Overload detection: a block that took longer than its budget WILL cause a
    // dropout (or already did), so it is counted as an xrun.
    if (load > 1.0f) {
        statistics_.underrunCount.fetch_add(1, std::memory_order_relaxed);
        statistics_.xrunFrames.fetch_add(static_cast<std::uint64_t>(numFrames),
                                         std::memory_order_relaxed);
    }

    cumulativeBlockMicros_ += static_cast<double>(elapsed);
    if (++blocksProcessed_ % 64 == 0) {
        statistics_.averageBlockMicros.store(
            static_cast<float>(cumulativeBlockMicros_ / 64.0), std::memory_order_relaxed);
        cumulativeBlockMicros_ = 0.0;
    }

    // Propagate recording problems to the UI without logging from the RT thread.
    const bool diskOverloaded = recorder_.diskSpaceLow() || recorder_.droppedFrames() > 0;
    statistics_.diskOverloaded.store(diskOverloaded, std::memory_order_relaxed);
}

int AudioEngine::delayCompensationSamples() const noexcept {
    const std::shared_ptr<graph::GraphPlan> plan = activePlan_;
    return plan ? plan->compensationSamples() : 0;
}

int AudioEngine::compensatedConnectionCount() const noexcept {
    const std::shared_ptr<graph::GraphPlan> plan = activePlan_;
    return plan ? plan->compensatedEdges() : 0;
}

} // namespace aura::engine
