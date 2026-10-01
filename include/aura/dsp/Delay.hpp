// ============================================================================
// AURA DAW - dsp/Delay.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stereo delay with tempo sync, feedback damping, cross-feed (ping-pong) and a
// dry/wet mix. Delay times are smoothed and read with linear interpolation so
// tempo-synced changes glide instead of clicking.
// ============================================================================
#pragma once

#include <vector>

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

struct DelaySettings {
    /// Delay time in ms (used when `syncToTempo` is false).
    float timeLeftMs = 375.0f;
    float timeRightMs = 375.0f;
    /// Tempo-synced note value as a fraction of a beat (0.25 = 1/4 note...).
    /// Only used when syncToTempo is true.
    float syncLeftBeats = 0.75f;  // dotted eighth
    float syncRightBeats = 0.75f;
    bool syncToTempo = true;
    float feedback = 0.35f;      ///< 0 .. 0.95
    float crossFeed = 0.0f;      ///< 0 = stereo delay, 1 = full ping-pong
    float mix = 0.3f;            ///< 0 = dry, 1 = wet
    /// One-pole low-pass inside the feedback path (0 = bright, 1 = very dark).
    float damping = 0.25f;
    bool bypassed = false;
};

class Delay final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Delay"; }

    void setSettings(const DelaySettings& settings) noexcept;
    [[nodiscard]] const DelaySettings& settings() const noexcept { return settings_; }

    /// True while the delay line still has audible content. The graph uses this
    /// to keep a track processing until its tail has decayed (correctness: a
    /// delay that stops early truncates music).
    [[nodiscard]] bool isActive() const noexcept override {
        return !settings_.bypassed && (tailActive_ || settings_.mix > 0.0f);
    }
    /// Only the decaying delay line counts: a freshly prepared delay that has
    /// never been fed holds silence, so skipping it drops nothing.
    [[nodiscard]] bool hasTail() const noexcept override {
        return !settings_.bypassed && tailActive_;
    }

    /// Max delay time supported (ms). 2 s covers synced delays at 40 BPM.
    static constexpr float kMaxDelayMs = 2000.0f;

private:
    [[nodiscard]] float delaySamplesFor(bool left, const ProcessContext& context) const noexcept;

    DelaySettings settings_{};
    std::vector<float> bufferLeft_;
    std::vector<float> bufferRight_;
    std::size_t writeIndex_ = 0;
    std::size_t capacity_ = 0;
    LinearSmoothedValue delaySmootherLeft_;
    LinearSmoothedValue delaySmootherRight_;
    LinearSmoothedValue feedbackSmoother_;
    LinearSmoothedValue mixSmoother_;
    float dampingStateLeft_ = 0.0f;
    float dampingStateRight_ = 0.0f;
    bool tailActive_ = false;
    /// Set when the delay time/feedback/mix changed: the smoothers are snapped on
    /// the next block, otherwise a freshly loaded delay ramps in from zero and
    /// swallows the first echoes (audible as "the delay starts late").
    bool snapSmoothers_ = true;
};

} // namespace aura::dsp
