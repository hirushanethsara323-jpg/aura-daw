// ============================================================================
// AURA DAW - src/plugin/sandbox/SandboxedPluginInstance.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The proxy. Everything interesting is in process(), which is twelve lines long and
// has to be right about three things: the order of the push and the pop, what
// happens when either one does not work, and never allocating.
// ============================================================================

#include "aura/plugin/sandbox/SandboxedPluginInstance.hpp"

#include <cstring>
#include <utility>

#include "aura/core/Math.hpp"
#include "aura/plugin/sandbox/SharedAudioRing.hpp"

namespace aura::plugin::sandbox {
namespace {

/// Bytes of parameter state this proxy saves: a count, then that many floats.
/// Deliberately trivial. The format is not the point - the point is that a project
/// saved with a sandboxed slot in it loads back with the same parameter values, and
/// that the day a real plug-in state blob crosses the boundary it replaces this
/// rather than being confused with it.
constexpr std::size_t kStateHeaderBytes = sizeof(std::uint32_t);

} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

SandboxedPluginInstance::SandboxedPluginInstance(ProxyConfig config)
    : config_(std::move(config)), descriptor_(config_.descriptor),
      // Sized in the initialiser list and never resized afterwards. Atomic rather
      // than plain because parameterValue() is documented RT-safe while the control
      // thread writes these, and a plain float written on one thread and read on the
      // other is a data race - this project has already shipped one of those and
      // paid a Windows Release runner to find it (ADR-0017 item 4).
      //
      // It has to be the size constructor and not resize(): a vector of atomics
      // cannot be resized at all, because the growth path libstdc++ instantiates
      // moves existing elements and std::atomic does not move. Default-constructing
      // the whole vector in fresh storage is also the only way to get a known value,
      // since C++20's default atomic constructor leaves a float indeterminate.
      parameterValues_(config_.parameterCount > 0
                           ? static_cast<std::size_t>(config_.parameterCount)
                           : std::size_t{0}) {
    // 0.5 rather than 0: the reference processor maps normalised 0..1 onto a gain of
    // 0..2, so 0.5 is unity. A slot that started at silence would be a slot that
    // looks broken until somebody moves a fader.
    for (auto& value : parameterValues_)
        value.store(0.5f, std::memory_order_relaxed);
}

AuraResult<std::unique_ptr<SandboxedPluginInstance>> SandboxedPluginInstance::create(
    ProxyConfig config) {
    if (config.descriptor.id.empty())
        return failureT<std::unique_ptr<SandboxedPluginInstance>>(makeError(
            ErrorCode::InvalidArgument, "a sandboxed plug-in slot needs a descriptor id",
            "the id is what the chain, the project file and the browser all key on"));
    if (config.parameterCount < 0)
        return failureT<std::unique_ptr<SandboxedPluginInstance>>(makeError(
            ErrorCode::InvalidArgument, "a plug-in cannot have a negative parameter count"));

    // std::unique_ptr with a private constructor: the object exists before any
    // process is started, because prepare() is what knows the session's limits.
    return std::unique_ptr<SandboxedPluginInstance>(
        new SandboxedPluginInstance(std::move(config)));
}

SandboxedPluginInstance::~SandboxedPluginInstance() = default;

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

Status SandboxedPluginInstance::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    if (sampleRate <= 0.0)
        return failure(ErrorCode::InvalidArgument,
                       "a sandboxed plug-in needs a positive sample rate");
    if (maxBlockSize <= 0)
        return failure(ErrorCode::InvalidArgument,
                       "a sandboxed plug-in needs a positive block size");
    if (numChannels <= 0)
        return failure(ErrorCode::InvalidArgument, "a sandboxed plug-in needs at least one channel");

    // Stop the old session before starting a new one. Two helpers on one ring would
    // both believe they are the consumer, and the counters would be meaningless.
    if (session_) {
        report_ = session_->report(); // stops it and keeps its diagnostics
        session_.reset();
    }

    SessionConfig sessionConfig;
    sessionConfig.processorId = config_.processorId;
    sessionConfig.helperExecutable = config_.helperExecutable;
    sessionConfig.maxBlockSize = static_cast<std::uint32_t>(maxBlockSize);
    sessionConfig.maxChannels = static_cast<std::uint32_t>(numChannels);
    sessionConfig.sampleRate = sampleRate;
    sessionConfig.readyTimeoutSeconds = config_.readyTimeoutSeconds;
    sessionConfig.maxBlocks = config_.maxBlocks;

    SessionReport startReport;
    auto started = SandboxSession::start(sessionConfig, startReport);
    if (started.hasError()) {
        // The session is gone, so its report is the only record of what the helper
        // said - including the helper's own exit code and stderr, which is the whole
        // explanation when a processor could not be created.
        report_ = startReport;
        if (report_.failure.empty())
            report_.failure = started.error().toString();
        // Marked failed so the chain skips this slot and passes audio through dry,
        // which is what it already does for a plug-in that crashed. A slot that could
        // not start is not a reason for the session to be silent.
        setFailed(true);
        return started.error();
    }

    session_ = std::move(started.value());
    sampleRate_ = sampleRate;
    blockSize_ = maxBlockSize;
    channels_ = numChannels;

    const std::int32_t latency = session_->latencyFrames(static_cast<std::uint32_t>(maxBlockSize));
    latencySamples_ = latency > 0 ? static_cast<int>(latency) : 0;

    // The drain scratch, sized for the worst block this session can carry. Allocated
    // here and only here: process() may need it at any moment and may not allocate.
    const auto scratchChannels = static_cast<std::size_t>(numChannels);
    const auto scratchFrames = static_cast<std::size_t>(maxBlockSize);
    drainScratch_.assign(scratchChannels, std::vector<float>(scratchFrames, 0.0f));
    drainPointers_.assign(scratchChannels, nullptr);
    for (std::size_t channel = 0; channel < scratchChannels; ++channel)
        drainPointers_[channel] = drainScratch_[channel].data();
    drainView_.channelPointers = drainPointers_.data();
    drainView_.numChannels = numChannels;
    drainView_.numFrames = maxBlockSize;
    drainedFrames_ = 0;

    // The new session has never seen a transport, and the block numbering continues
    // where it left off: a fresh helper is a fresh consumer, but the producer's
    // origin is still monotonic, so a block that arrives is never "from the past".
    transportPublished_ = false;
    report_ = SessionReport{};
    setFailed(false);
    return success();
}

void SandboxedPluginInstance::reset() noexcept {
    if (session_)
        session_->requestReset();
    // The transport is republished on the next block, because a reset is usually a
    // seek and the position the helper would prepare for has just changed.
    transportPublished_ = false;
}

// ---------------------------------------------------------------------------
// the audio path
// ---------------------------------------------------------------------------

void SandboxedPluginInstance::publishTransportIfChanged(const dsp::ProcessContext& context,
                                                        int numFrames) noexcept {
    TransportSnapshot next{};
    next.sampleRate = context.sampleRate;
    next.tempoBpm = context.tempoBpm;
    next.positionSamples = context.positionSamples;
    next.positionTicks = context.positionTicks;
    next.blockSize = numFrames > 0 ? static_cast<std::uint32_t>(numFrames) : 0u;
    next.channelCount = channels_ > 0 ? static_cast<std::uint32_t>(channels_) : 0u;
    next.flags = (context.isPlaying ? transportFlag::kPlaying : 0u) |
                 (context.isRendering ? transportFlag::kRendering : 0u);

    // Compared before publishing: a stopped transport does not move, and writing the
    // same snapshot into a seqlock every block would be twelve stores for nothing.
    if (transportPublished_ && next == lastTransport_)
        return;
    session_->publishTransport(next);
    lastTransport_ = next;
    transportPublished_ = true;
}

void SandboxedPluginInstance::process(dsp::AudioBlockView& audio,
                                      const dsp::ProcessContext& context,
                                      const midi::Message* midiIn, int numMidiIn,
                                      midi::Message* midiOut, int& numMidiOut) noexcept {
    numMidiOut = 0;
    if (session_ == nullptr || audio.isEmpty())
        return;

    SharedAudioRing& ring = session_->ring();

    publishTransportIfChanged(context, audio.numFrames);

    // Notes first, so a plug-in sees the note that belongs to this block rather than
    // the next one. The chain hands over at most kChainMidiOutCapacity events and the
    // note ring holds more than that, so a refusal here means the helper has stopped
    // draining - which the ring counts and the watchdog reads.
    if (midiIn != nullptr) {
        for (int i = 0; i < numMidiIn; ++i)
            ring.pushNote(midiIn[i]);
    }

    // PUSH, THEN POP - and the order is the whole trick. The chain processes in
    // place, so `audio` is both the input and the destination for the output. Pushing
    // first copies it into the ring's slot, after which it may be overwritten by the
    // block that comes back. Popping first is what SharedAudioRing::exchange() does,
    // to give the helper the longest possible run at the previous block, and with an
    // in-place caller it would overwrite the input before the input was published.
    const PushOutcome pushed = ring.pushInput(audio, origin_);
    bool drained = false;
    if (pushed == PushOutcome::Published) {
        ++origin_;
        ++blocksPublished_;
    } else if (pushed == PushOutcome::RingFull) {
        // The helper is behind. Two causes, and both need the same answer:
        //
        //   * it is merely slow, and taking one of its outputs makes room for it to
        //     catch up;
        //   * it is holding a processed block it cannot publish because the output
        //     ring is full - which is back-pressure working - and it will not take
        //     another input until the host takes that block. A host that returned
        //     here without popping would leave both sides waiting for each other
        //     forever, which is a deadlock that looks exactly like a hung plug-in.
        //
        // So drain one output into scratch rather than into `audio`, which is still
        // holding the input this call has to publish, and try the push once more.
        ++refusedPushes_;
        dsp::AudioBlockView drain = drainView_;
        drain.numChannels = audio.numChannels < drain.numChannels ? audio.numChannels
                                                                 : drain.numChannels;
        drain.numFrames = audio.numFrames < drain.numFrames ? audio.numFrames : drain.numFrames;
        const PopResult scratchPop = ring.popOutput(drain);
        drained = scratchPop.outcome == PopOutcome::Consumed;
        drainedFrames_ = drained ? scratchPop.frames : 0u;
        if (ring.pushInput(audio, origin_) == PushOutcome::Published) {
            ++drainRetries_;
            ++origin_;
            ++blocksPublished_;
        }
    } else {
        // TooLarge, Empty, WrongSide or NotAttached: this block cannot be carried by
        // this session at all, which is a caller bug rather than a scheduling fact.
        // Counted and left dry, never retried - retrying a bug turns it into a spin.
        ++rejectedBlocks_;
        return;
    }

    if (drained) {
        // The block taken into scratch is this call's audio. Copying it out is safe
        // whichever way the retry went: a published push copied `audio` into the ring
        // first, and a refused one never wanted it.
        const int frames = static_cast<int>(drainedFrames_);
        for (int channel = 0; channel < audio.numChannels; ++channel) {
            float* const destination = audio.channelPointers[channel];
            const float* const source = drainPointers_[static_cast<std::size_t>(channel)];
            for (int frame = 0; frame < frames && frame < audio.numFrames; ++frame)
                destination[frame] = source[frame];
            for (int frame = frames; frame < audio.numFrames; ++frame)
                destination[frame] = 0.0f;
        }
        ++blocksReturned_;
    } else {
        const PopResult popped = ring.popOutput(audio);
        if (popped.outcome == PopOutcome::Consumed) {
            ++blocksReturned_;
            // A short block would leave the tail holding the input, which is half a
            // plug-in and half dry - the kind of fault that is audible and nearly
            // impossible to attribute. The helper always returns what it was given, so
            // this guards against a peer disagreeing about the frame count rather than
            // a case that happens.
            if (popped.frames < static_cast<std::uint32_t>(audio.numFrames)) {
                for (int channel = 0; channel < audio.numChannels; ++channel) {
                    float* const data = audio.channelPointers[channel];
                    for (int frame = static_cast<int>(popped.frames); frame < audio.numFrames;
                         ++frame)
                        data[frame] = 0.0f;
                }
            }
        } else {
            // Nothing came back: the first block after prepare(), which is what one
            // block of latency means, or a helper that has stopped. `audio` still holds
            // the input because the push copied rather than moved it, so this is a dry
            // pass-through and not a dropout. A torn read is the one case where the ring
            // has already zeroed the destination, on purpose: half of two different
            // blocks is not audio.
            ++dryBlocks_;
        }
    }

    // Drained whether or not a block came back. Notes the helper generated are
    // waiting in the ring regardless, and leaving them there until a call that
    // happens to get audio would bunch them up at the wrong block.
    if (midiOut != nullptr) {
        midi::Message note;
        while (numMidiOut < kChainMidiOutCapacity && ring.popNoteOut(note))
            midiOut[numMidiOut++] = note;
    }
}

// ---------------------------------------------------------------------------
// parameters
// ---------------------------------------------------------------------------

int SandboxedPluginInstance::latencySamples() const noexcept { return latencySamples_; }

int SandboxedPluginInstance::numParameters() const noexcept {
    return static_cast<int>(parameterValues_.size());
}

std::string SandboxedPluginInstance::parameterName(int index) const {
    if (index < 0 || index >= numParameters())
        return {};
    const auto i = static_cast<std::size_t>(index);
    if (i < config_.parameterNames.size() && !config_.parameterNames[i].empty())
        return config_.parameterNames[i];
    // Numbered rather than invented. A proxy that made up a name for a parameter it
    // has never seen would be lying in the one place a user looks for the truth.
    return "Param " + std::to_string(index + 1);
}

float SandboxedPluginInstance::parameterValue(int index) const noexcept {
    return parameterNormalised(index);
}

void SandboxedPluginInstance::setParameterValue(int index, float value) noexcept {
    setParameterNormalised(index, value);
}

float SandboxedPluginInstance::parameterNormalised(int index) const noexcept {
    if (index < 0 || index >= numParameters())
        return 0.0f;
    return parameterValues_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
}

void SandboxedPluginInstance::setParameterNormalised(int index, float value) noexcept {
    if (index < 0 || index >= numParameters())
        return;
    // Clamped here rather than trusted: this value crosses a process boundary, and a
    // parameter outside 0..1 is either a caller's bug or a peer's.
    const float clamped = math::clamp(value, 0.0f, 1.0f);
    parameterValues_[static_cast<std::size_t>(index)].store(clamped, std::memory_order_relaxed);
    if (session_ == nullptr)
        return;
    ParamRecord record{};
    record.paramId = index;
    record.sampleOffset = 0; // block-aligned, like every other parameter path in AURA
    record.value = clamped;
    session_->ring().pushParameter(record);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

AuraResult<std::vector<std::uint8_t>> SandboxedPluginInstance::saveState() const {
    const auto count = static_cast<std::uint32_t>(parameterValues_.size());
    std::vector<std::uint8_t> blob(kStateHeaderBytes +
                                   static_cast<std::size_t>(count) * sizeof(float));
    std::memcpy(blob.data(), &count, sizeof(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        const float value = parameterValues_[static_cast<std::size_t>(i)].load(
            std::memory_order_relaxed);
        std::memcpy(blob.data() + kStateHeaderBytes + static_cast<std::size_t>(i) * sizeof(float),
                    &value, sizeof(float));
    }
    return blob;
}

Status SandboxedPluginInstance::restoreState(const std::vector<std::uint8_t>& state) {
    if (state.size() < kStateHeaderBytes)
        return failure(ErrorCode::InvalidArgument,
                       "a sandboxed plug-in's state is too short to hold a count",
                       "have " + std::to_string(state.size()) + " bytes, need " +
                           std::to_string(kStateHeaderBytes));
    std::uint32_t count = 0;
    std::memcpy(&count, state.data(), sizeof(count));
    const std::size_t expected = kStateHeaderBytes + static_cast<std::size_t>(count) * sizeof(float);
    if (state.size() != expected)
        return failure(ErrorCode::InvalidArgument,
                       "a sandboxed plug-in's state does not match its own count",
                       "claims " + std::to_string(count) + " parameters, have " +
                           std::to_string(state.size()) + " bytes, expected " +
                           std::to_string(expected));

    const auto available = static_cast<std::uint32_t>(parameterValues_.size());
    // Restoring fewer than the blob holds is not an error: a project saved by a newer
    // AURA, or by a build with a different processor, is still worth playing.
    for (std::uint32_t i = 0; i < count && i < available; ++i) {
        float value = 0.0f;
        std::memcpy(&value,
                    state.data() + kStateHeaderBytes + static_cast<std::size_t>(i) * sizeof(float),
                    sizeof(float));
        setParameterNormalised(static_cast<int>(i), value);
    }
    return success();
}

} // namespace aura::plugin::sandbox
