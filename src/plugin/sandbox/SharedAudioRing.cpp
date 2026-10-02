// ============================================================================
// AURA DAW - src/plugin/sandbox/SharedAudioRing.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The deferred-pipeline protocol over the arena's bytes. See SharedAudioRing.hpp
// for the ordering argument and SandboxArena.cpp for why the snapshot's version
// word is seq_cst while these rings are plain release/acquire.
// ============================================================================
#include "aura/plugin/sandbox/SharedAudioRing.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace aura::plugin::sandbox {

// ---------------------------------------------------------------------------
// TransportStats
// ---------------------------------------------------------------------------

TransportStats::Snapshot TransportStats::snapshot() const noexcept {
    Snapshot out{};
    out.overruns = overruns.load(std::memory_order_relaxed);
    out.underruns = underruns.load(std::memory_order_relaxed);
    out.tornReads = tornReads.load(std::memory_order_relaxed);
    out.blocksLost = blocksLost.load(std::memory_order_relaxed);
    out.wrongSideCalls = wrongSideCalls.load(std::memory_order_relaxed);
    out.oversizedBlocks = oversizedBlocks.load(std::memory_order_relaxed);
    return out;
}

void TransportStats::reset() noexcept {
    overruns.store(0, std::memory_order_relaxed);
    underruns.store(0, std::memory_order_relaxed);
    tornReads.store(0, std::memory_order_relaxed);
    blocksLost.store(0, std::memory_order_relaxed);
    wrongSideCalls.store(0, std::memory_order_relaxed);
    oversizedBlocks.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// AudioSlotRing
// ---------------------------------------------------------------------------

void AudioSlotRing::attach(const AudioRingRegion& region) noexcept {
    base_ = region.base;
    slots_ = region.slots;
    maxChannels_ = region.maxChannels;
    maxBlockSize_ = region.maxBlockSize;
    slotStride_ = region.slotStride;
    attached_ = base_ != nullptr && slots_ >= kMinAudioSlots && maxChannels_ > 0u &&
                maxBlockSize_ > 0u && slotStride_ >= sizeof(AudioSlotHeader);
}

std::atomic<std::uint64_t>* AudioSlotRing::publishedCounter() const noexcept {
    return reinterpret_cast<std::atomic<std::uint64_t>*>(base_);
}

std::atomic<std::uint64_t>* AudioSlotRing::consumedCounter() const noexcept {
    return reinterpret_cast<std::atomic<std::uint64_t>*>(base_ + kCacheLine);
}

std::uint8_t* AudioSlotRing::slotsBase() const noexcept {
    return base_ + sizeof(PodRingHeader);
}

AudioSlotHeader* AudioSlotRing::header(std::uint64_t block) const noexcept {
    const std::uint64_t index = block % static_cast<std::uint64_t>(slots_);
    return reinterpret_cast<AudioSlotHeader*>(slotsBase() + index * slotStride_);
}

float* AudioSlotRing::payload(std::uint64_t block) const noexcept {
    return reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(header(block)) +
                                    sizeof(AudioSlotHeader));
}

PushOutcome AudioSlotRing::push(const dsp::AudioBlockView& source,
                                std::uint64_t origin) noexcept {
    if (!attached_)
        return PushOutcome::NotAttached;
    if (source.channelPointers == nullptr || source.isEmpty())
        return PushOutcome::Empty;
    // Refuse rather than truncate: silently publishing half a block would put the
    // peer's channel count out of step with the audio it receives, which is the
    // kind of error that surfaces as a wrong-sounding mix and not as a crash.
    if (source.numChannels < 0 || source.numFrames < 0 ||
        static_cast<std::uint32_t>(source.numChannels) > maxChannels_ ||
        static_cast<std::uint32_t>(source.numFrames) > maxBlockSize_)
        return PushOutcome::TooLarge;

    const std::uint64_t published = publishedCounter()->load(std::memory_order_relaxed);
    const std::uint64_t consumed = consumedCounter()->load(std::memory_order_acquire);
    if (published - consumed >= static_cast<std::uint64_t>(slots_))
        return PushOutcome::RingFull;

    AudioSlotHeader* slot = header(published);
    slot->sequence.store(slotWritingSequence(published), std::memory_order_relaxed);
    // StoreStore: a consumer must never observe the payload before it observes the
    // odd sequence that says the payload is not finished. x86 does not reorder
    // stores, the C++ model permits it, and this transport is supposed to be
    // correct under the model.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    const std::uint32_t frames = static_cast<std::uint32_t>(source.numFrames);
    const std::uint32_t channels = static_cast<std::uint32_t>(source.numChannels);
    slot->frames = frames;
    slot->channels = channels;
    slot->origin = origin;

    float* destination = payload(published);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        std::memcpy(destination + static_cast<std::size_t>(channel) * maxBlockSize_,
                    source.channelPointers[channel],
                    static_cast<std::size_t>(frames) * sizeof(float));
    }

    slot->sequence.store(slotStableSequence(published), std::memory_order_release);
    publishedCounter()->store(published + 1u, std::memory_order_release);
    return PushOutcome::Published;
}

PopResult AudioSlotRing::pop(const dsp::AudioBlockView& destination) noexcept {
    PopResult result{};
    if (!attached_) {
        result.outcome = PopOutcome::NotAttached;
        return result;
    }
    if (destination.channelPointers == nullptr || destination.isEmpty()) {
        result.outcome = PopOutcome::NotAttached;
        return result;
    }

    const std::uint64_t consumed = consumedCounter()->load(std::memory_order_relaxed);
    const std::uint64_t published = publishedCounter()->load(std::memory_order_acquire);
    if (published <= consumed) {
        result.outcome = PopOutcome::Empty;
        return result;
    }

    std::uint64_t block = consumed;
    if (published - consumed > static_cast<std::uint64_t>(slots_)) {
        // The producer lapped us: the oldest unread blocks have already been
        // overwritten. Skip to the oldest one that still exists and say how many
        // were lost, because a silent gap in the audio is the failure this counter
        // exists to make visible.
        result.blocksLost = (published - consumed) - static_cast<std::uint64_t>(slots_);
        block = published - static_cast<std::uint64_t>(slots_);
    }
    result.block = block;

    AudioSlotHeader* slot = header(block);
    const std::uint64_t expected = slotStableSequence(block);
    if (slot->sequence.load(std::memory_order_acquire) != expected) {
        // Either the producer is mid-write in this slot or the peer died between
        // publishing and here. Release the slot and report it: going dry for one
        // block is Decision 3 step 2, and stalling to wait for a dead peer is how a
        // session freezes instead of losing a plug-in.
        consumedCounter()->store(block + 1u, std::memory_order_release);
        result.outcome = PopOutcome::Torn;
        return result;
    }

    // Clamped to the destination AND to the arena's own budget: a peer that reports
    // more frames or channels than the arena was sized for cannot write past the
    // caller's buffer, and cannot read past its own slot either.
    const std::uint32_t frames = std::min(
        {slot->frames, static_cast<std::uint32_t>(destination.numFrames), maxBlockSize_});
    const std::uint32_t channels = std::min(
        {slot->channels, static_cast<std::uint32_t>(destination.numChannels), maxChannels_});
    result.frames = frames;
    result.origin = slot->origin;

    const float* source = payload(block);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        std::memcpy(destination.channelPointers[channel],
                    source + static_cast<std::size_t>(channel) * maxBlockSize_,
                    static_cast<std::size_t>(frames) * sizeof(float));
    }

    // LoadLoad: the verification read must happen after the copy, not before it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const bool overwritten = slot->sequence.load(std::memory_order_relaxed) != expected;

    // This was a bug: the slot was released BEFORE this re-check, so the producer -
    // legitimately, the moment `consumed` moved - stamped the slot with the next
    // block's sequence and the re-check read that instead. A clean copy was then
    // reported as torn and thrown away, which the two-thread test caught as a lost
    // block roughly once in every thirty thousand. Verification has to complete
    // while the consumer still owns the slot; only then is it safe to let go.
    // Generalised in ADR-0017: a consumer may not release a slot until it has
    // finished verifying what it read from it.
    consumedCounter()->store(block + 1u, std::memory_order_release);

    if (overwritten) {
        // Genuinely overwritten while we were copying it, which means the producer
        // ignored the fullness check - a buggy or dead peer. Zero what we wrote so a
        // caller that ignores the outcome gets silence rather than half of two
        // different blocks. The cost is one memset on a path that should never run.
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            std::memset(destination.channelPointers[channel], 0,
                        static_cast<std::size_t>(frames) * sizeof(float));
        }
        result.outcome = PopOutcome::Torn;
        result.frames = 0;
        return result;
    }

    result.outcome = PopOutcome::Consumed;
    return result;
}

std::uint64_t AudioSlotRing::publishedCount() const noexcept {
    return attached_ ? publishedCounter()->load(std::memory_order_acquire) : 0u;
}

std::uint64_t AudioSlotRing::consumedCount() const noexcept {
    return attached_ ? consumedCounter()->load(std::memory_order_acquire) : 0u;
}

std::uint64_t AudioSlotRing::available() const noexcept {
    if (!attached_)
        return 0u;
    const std::uint64_t published = publishedCounter()->load(std::memory_order_acquire);
    const std::uint64_t consumed = consumedCounter()->load(std::memory_order_acquire);
    return published - consumed;
}

void AudioSlotRing::reset() noexcept {
    if (!attached_)
        return;
    publishedCounter()->store(0, std::memory_order_relaxed);
    consumedCounter()->store(0, std::memory_order_relaxed);
    // Only the sequence counters are cleared. The sample data is left alone because
    // a slot whose sequence says "never written" can never be read, and zeroing two
    // full slot rings on every plug-in restart is work nobody hears.
    for (std::uint32_t slot = 0; slot < slots_; ++slot) {
        AudioSlotHeader* headerAt = reinterpret_cast<AudioSlotHeader*>(
            slotsBase() + static_cast<std::size_t>(slot) * slotStride_);
        headerAt->sequence.store(0, std::memory_order_relaxed);
        headerAt->frames = 0;
        headerAt->channels = 0;
        headerAt->origin = 0;
    }
}

// ---------------------------------------------------------------------------
// SharedAudioRing
// ---------------------------------------------------------------------------

Status SharedAudioRing::attach(const ArenaView& arena, Side side) {
    if (!arena.isValid())
        return failure(ErrorCode::InvalidArgument,
                       "the sandbox transport needs a created or attached arena");

    input_.attach(arena.inputRing());
    output_.attach(arena.outputRing());
    params_.attach(arena.paramRing());
    notesIn_.attach(arena.noteInRing());
    notesOut_.attach(arena.noteOutRing());
    control_ = arena.control();
    telemetry_ = arena.telemetry();
    side_ = side;

    attached_ = input_.isAttached() && output_.isAttached() && params_.isAttached() &&
                notesIn_.isAttached() && notesOut_.isAttached();
    if (!attached_)
        return failure(ErrorCode::InvalidArgument,
                       "one of the arena's rings did not attach",
                       "the arena header was validated, so this means the layout and the "
                       "region descriptors disagree - a bug in computeLayout()");
    return success();
}

std::uint32_t SharedAudioRing::latencyBlocks() const noexcept {
    // slots - 1: with two slots the host writes block n and reads back n-1, which is
    // the one block of PDC Decision 2 promises and the graph compensates for.
    return attached_ ? input_.slots() - 1u : 0u;
}

std::int32_t SharedAudioRing::latencyFrames(std::uint32_t blockSize) const noexcept {
    if (!attached_ || blockSize == 0u)
        return 0;
    return static_cast<std::int32_t>(latencyBlocks()) * static_cast<std::int32_t>(blockSize);
}

PushOutcome SharedAudioRing::pushInput(const dsp::AudioBlockView& input,
                                       std::uint64_t origin) noexcept {
    if (!attached_)
        return PushOutcome::NotAttached;
    if (side_ != Side::Host) {
        stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return PushOutcome::WrongSide;
    }
    const PushOutcome outcome = input_.push(input, origin);
    if (outcome == PushOutcome::RingFull) {
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
    } else if (outcome == PushOutcome::TooLarge) {
        stats_.oversizedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    return outcome;
}

PopResult SharedAudioRing::popOutput(const dsp::AudioBlockView& output) noexcept {
    PopResult result{};
    if (!attached_) {
        result.outcome = PopOutcome::NotAttached;
        return result;
    }
    if (side_ != Side::Host) {
        stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        result.outcome = PopOutcome::WrongSide;
        return result;
    }
    result = output_.pop(output);
    accountPop(result);
    return result;
}

BlockExchange SharedAudioRing::exchange(std::uint64_t origin, const dsp::AudioBlockView& input,
                                        const dsp::AudioBlockView& output) noexcept {
    BlockExchange result{};
    // Pop before push: the helper has been working on block n-1 since the last
    // callback, and taking its result first gives it every microsecond of that time.
    // Pushing first would not be wrong, only needlessly pessimistic.
    result.popped = popOutput(output);
    result.pushed = pushInput(input, origin);
    return result;
}

PopResult SharedAudioRing::popInput(const dsp::AudioBlockView& input) noexcept {
    PopResult result{};
    if (!attached_) {
        result.outcome = PopOutcome::NotAttached;
        return result;
    }
    if (side_ != Side::Helper) {
        stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        result.outcome = PopOutcome::WrongSide;
        return result;
    }
    result = input_.pop(input);
    accountPop(result);
    return result;
}

PushOutcome SharedAudioRing::pushOutput(const dsp::AudioBlockView& output,
                                        std::uint64_t origin) noexcept {
    if (!attached_)
        return PushOutcome::NotAttached;
    if (side_ != Side::Helper) {
        stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return PushOutcome::WrongSide;
    }
    const PushOutcome outcome = output_.push(output, origin);
    if (outcome == PushOutcome::RingFull) {
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
    } else if (outcome == PushOutcome::TooLarge) {
        stats_.oversizedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    publishAnomalies();
    return outcome;
}

void SharedAudioRing::accountPop(const PopResult& result) noexcept {
    switch (result.outcome) {
    case PopOutcome::Consumed:
        break;
    case PopOutcome::Empty:
        stats_.underruns.fetch_add(1, std::memory_order_relaxed);
        break;
    case PopOutcome::Torn:
        stats_.tornReads.fetch_add(1, std::memory_order_relaxed);
        break;
    case PopOutcome::WrongSide:
    case PopOutcome::NotAttached:
        break;
    }
    if (result.blocksLost > 0u)
        stats_.blocksLost.fetch_add(result.blocksLost, std::memory_order_relaxed);
    publishAnomalies();
}

void SharedAudioRing::publishAnomalies() noexcept {
    // Helper side only. The mailbox is documented as helper -> host: the host's own
    // anomalies stay in stats_, where the supervisor (same process) reads them
    // directly, and letting both processes write one counter would make the number
    // mean nothing.
    if (telemetry_ == nullptr || side_ != Side::Helper)
        return;
    const TransportStats::Snapshot now = stats_.snapshot();
    telemetry_->overruns.store(static_cast<std::uint32_t>(now.overruns), std::memory_order_relaxed);
    telemetry_->underruns.store(static_cast<std::uint32_t>(now.underruns),
                                std::memory_order_relaxed);
    telemetry_->tornReads.store(static_cast<std::uint32_t>(now.tornReads),
                                std::memory_order_relaxed);
    telemetry_->blocksLost.store(static_cast<std::uint32_t>(now.blocksLost),
                                 std::memory_order_relaxed);
}

bool SharedAudioRing::pushParameter(const ParamRecord& record) noexcept {
    if (!attached_ || side_ != Side::Host) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return params_.push(record);
}

bool SharedAudioRing::pushNote(const midi::Message& message) noexcept {
    if (!attached_ || side_ != Side::Host) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MidiRecord record{};
    if (!toMidiRecord(message, record)) {
        if (telemetry_ != nullptr)
            telemetry_->rejectedRecords.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return notesIn_.push(record);
}

bool SharedAudioRing::popParameter(ParamRecord& out) noexcept {
    if (!attached_ || side_ != Side::Helper) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return params_.pop(out);
}

bool SharedAudioRing::popNoteIn(midi::Message& out) noexcept {
    if (!attached_ || side_ != Side::Helper) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MidiRecord record{};
    if (!notesIn_.pop(record))
        return false;
    if (!toMidiMessage(record, out)) {
        if (telemetry_ != nullptr)
            telemetry_->rejectedRecords.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool SharedAudioRing::pushNoteOut(const midi::Message& message) noexcept {
    if (!attached_ || side_ != Side::Helper) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MidiRecord record{};
    if (!toMidiRecord(message, record))
        return false;
    return notesOut_.push(record);
}

bool SharedAudioRing::popNoteOut(midi::Message& out) noexcept {
    if (!attached_ || side_ != Side::Host) {
        if (attached_)
            stats_.wrongSideCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MidiRecord record{};
    if (!notesOut_.pop(record))
        return false;
    if (!toMidiMessage(record, out)) {
        if (telemetry_ != nullptr)
            telemetry_->rejectedRecords.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void SharedAudioRing::reset() noexcept {
    input_.reset();
    output_.reset();
    params_.reset();
    notesIn_.reset();
    notesOut_.reset();
    stats_.reset();
    if (control_ != nullptr) {
        control_->hostRequests.store(0, std::memory_order_relaxed);
        control_->transportVersion.store(0, std::memory_order_relaxed);
    }
}

} // namespace aura::plugin::sandbox
