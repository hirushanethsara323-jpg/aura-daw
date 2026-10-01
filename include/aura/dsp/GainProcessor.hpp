// ============================================================================
// AURA DAW - dsp/GainProcessor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel gain trim with click-free ramping, polarity invert and a
// documented peak-output clamp at +/- 1.0 by default (internal processing is
// 32-bit float, so clipping is NOT applied unless the user enables it).
// ============================================================================
#pragma once

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

class GainProcessor final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Gain"; }

    /// Linear gain (1.0 = unity).
    void setGain(float linearGain) noexcept;
    void setGainDb(float decibels) noexcept;
    [[nodiscard]] float gain() const noexcept { return gainLinear_; }
    [[nodiscard]] float gainDb() const noexcept { return math::gainToDb(gainLinear_); }

    void setInvertPolarity(bool invert) noexcept { invert_.store(invert, std::memory_order_relaxed); }
    [[nodiscard]] bool isPolarityInverted() const noexcept {
        return invert_.load(std::memory_order_relaxed);
    }

    /// Ramp length used when the gain changes (default 15 ms).
    void setRampSeconds(float seconds) noexcept { rampSeconds_ = seconds; }

    [[nodiscard]] int latencySamples() const noexcept override { return 0; }

private:
    LinearSmoothedValue smoothedGain_;
    float gainLinear_ = 1.0f;
    float rampSeconds_ = 0.015f;
    std::atomic<bool> invert_{false};
};

} // namespace aura::dsp
