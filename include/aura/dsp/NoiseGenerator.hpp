// ============================================================================
// AURA DAW - include/aura/dsp/NoiseGenerator.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// White / pink / brown noise generators. Pink noise uses the Voss-McCartney
// style filtered-white approach with the standard -3 dB/octave shaping
// (Paul Kellet's widely published coefficients are a public-domain algorithm;
// implemented here from the filter maths directly).
// ============================================================================
#pragma once

#include <atomic>

#include "aura/core/Random.hpp"
#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

enum class NoiseColour : std::int32_t { White = 0, Pink, Brown };

class NoiseGenerator final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Noise"; }
    [[nodiscard]] bool isActive() const noexcept override { return enabled_.load(); }
    /// A running noise generator is a source: it produces signal with no input.
    [[nodiscard]] bool hasTail() const noexcept override { return enabled_.load(); }

    void setColour(NoiseColour colour) noexcept { colour_ = colour; }
    void setLevel(float linearLevel) noexcept { levelSmoother_.setTarget(linearLevel); }
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled); }
    [[nodiscard]] float lastSample() const noexcept { return lastSample_; }

private:
    [[nodiscard]] float nextSample() noexcept;

    XorShift128 rng_{0xA17A0001u};
    OnePoleSmoother levelSmoother_;
    NoiseColour colour_ = NoiseColour::White;
    std::atomic<bool> enabled_{false};
    // Pink noise state (7 poles) and brown noise integrator.
    float pinkState_[7]{};
    float brownState_ = 0.0f;
    float lastSample_ = 0.0f;
};

} // namespace aura::dsp
