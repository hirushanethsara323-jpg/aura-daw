// ============================================================================
// AURA DAW - src/plugin/sandbox/SandboxProcessor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The processor registry and the reference processor.
//
// The reference processor is deliberately boring: a gain and a note echo. It has
// no state that can drift, no allocation on the real-time path, and an output a
// test can predict sample by sample, which is the entire reason it exists. A
// cross-process audio path that could only be exercised with a real third-party
// bundle would never run in CI, and would therefore never be known to work.
// ============================================================================

#include "aura/plugin/sandbox/SandboxProcessor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include "aura/core/Math.hpp"
#include "aura/plugin/sandbox/HelperProtocol.hpp"

namespace aura::plugin::sandbox {
namespace {

/// How many notes the reference processor can hold between blocks. Fixed, because
/// the queue lives on the real-time path: a std::deque here would be an allocation
/// per burst of notes, which is the thing the whole sandbox is designed to avoid.
/// A full queue drops the newest note - dropping the oldest would reorder a
/// performance, and a reordering is worse to listen to than a missing note.
constexpr std::size_t kNoteQueueCapacity = 64;

/// Parameter 0 of the reference processor, mapped from normalised 0..1 to a linear
/// gain of 0..2 so that 0.5 is unity. The mapping is stated here and nowhere else
/// because a test asserts the exact samples that come back.
constexpr float kGainRange = 2.0f;

/// The reference processor: gain plus a note echo.
class GainProcessor final : public SandboxProcessor {
public:
    [[nodiscard]] const char* id() const noexcept override { return processorId::kGain; }

    [[nodiscard]] bool prepare(const ProcessorSpec& spec) noexcept override {
        // Nothing to allocate, which is the point: the processor is stateless
        // except for two scalars. It still checks the spec, because a processor
        // that ignores its session's limits is a processor that will be handed a
        // block wider than it assumed.
        prepared_ = spec.maxBlockSize > 0 && spec.maxChannels > 0 && spec.sampleRate > 0.0;
        channels_ = spec.maxChannels;
        return prepared_;
    }

    void process(const dsp::AudioBlockView& input, const dsp::AudioBlockView& output,
                 std::uint32_t frames) noexcept override {
        if (!prepared_ || frames == 0 || input.isEmpty() || output.isEmpty())
            return;
        // The gain is read once per block rather than per sample. A per-sample read
        // of an atomic would be correct and slower, and this processor's job is to
        // be predictable, not to be a smoothing demo - the engine's own gain stages
        // (dsp/) are where smoothing is tested.
        const float gain = gain_;
        const int channels = std::min(input.numChannels, output.numChannels);
        for (int channel = 0; channel < channels; ++channel) {
            const float* const source = input.channelPointers[channel];
            float* const destination = output.channelPointers[channel];
            for (std::uint32_t i = 0; i < frames; ++i)
                destination[i] = math::flushDenormal(source[i] * gain);
        }
    }

    void onParameter(const ParamRecord& record) noexcept override {
        if (record.paramId != 0)
            return;
        // Clamped rather than trusted: the value came from another process, and a
        // normalised parameter outside 0..1 is either a bug or a hostile peer.
        const float normalised = math::clamp(record.value, 0.0f, 1.0f);
        gain_ = normalised * kGainRange;
    }

    void onNoteIn(const midi::Message& message) noexcept override {
        if (noteCount_ >= kNoteQueueCapacity)
            return; // full: drop the newest, never reorder what is already queued
        notes_[(noteHead_ + noteCount_) % kNoteQueueCapacity] = message;
        ++noteCount_;
    }

    [[nodiscard]] bool popNoteOut(midi::Message& out) noexcept override {
        if (noteCount_ == 0)
            return false;
        out = notes_[noteHead_];
        noteHead_ = (noteHead_ + 1) % kNoteQueueCapacity;
        --noteCount_;
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() override {
        // Four bytes of gain. Trivial, and deliberately so: the crash protocol's
        // recovery point has to work for a processor whose entire state is one
        // float before it can be trusted with one whose state is a preset bank.
        std::vector<std::uint8_t> blob(sizeof(float));
        std::memcpy(blob.data(), &gain_, sizeof(float));
        return blob;
    }

    bool restoreState(const std::uint8_t* data, std::size_t bytes) override {
        if (data == nullptr || bytes != sizeof(float))
            return false;
        float restored = 0.0f;
        std::memcpy(&restored, data, sizeof(float));
        if (!std::isfinite(restored))
            return false;
        gain_ = restored;
        return true;
    }

    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

private:
    float gain_ = 1.0f;
    bool prepared_ = false;
    std::uint32_t channels_ = 0;
    std::array<midi::Message, kNoteQueueCapacity> notes_{};
    std::size_t noteHead_ = 0;
    std::size_t noteCount_ = 0;
};

} // namespace

ProcessorSpec processorSpecFor(const ArenaSpec& arena, double sampleRate) noexcept {
    ProcessorSpec spec;
    spec.sampleRate = sampleRate;
    spec.maxBlockSize = arena.maxBlockSize;
    spec.maxChannels = arena.maxChannels;
    return spec;
}

AuraResult<std::unique_ptr<SandboxProcessor>> makeSandboxProcessor(const std::string& id) {
    if (id == processorId::kGain)
        return std::unique_ptr<SandboxProcessor>(std::make_unique<GainProcessor>());

    // An unknown id is reported with the list of ids that ARE known, because the
    // caller is a host that has just been told a plug-in failed to load and the
    // difference between "no such processor" and "this build has no SDK" is the
    // whole diagnosis.
    std::string known;
    for (const auto& candidate : builtinProcessorIds()) {
        if (!known.empty())
            known += ", ";
        known += candidate;
    }
    return failureT<std::unique_ptr<SandboxProcessor>>(makeError(
        ErrorCode::NotFound, "no sandbox processor with that id",
        "asked for '" + id + "'; this build provides: " + known +
            " (a VST 3 or CLAP adapter is compiled in only with its option enabled)"));
}

std::vector<std::string> builtinProcessorIds() {
    std::vector<std::string> ids;
    ids.emplace_back(processorId::kGain);
    return ids;
}

} // namespace aura::plugin::sandbox
