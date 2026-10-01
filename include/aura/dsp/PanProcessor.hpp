// ============================================================================
// AURA DAW - dsp/PanProcessor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Constant-power stereo panner with three modes, plus a documented design
// limitation: AURA's V1 mixer is stereo (2.0). Surround panning is out of
// scope; the class keeps its math in a `PanLaw` enum so a future
// multichannel panner can reuse the smoother plumbing.
//
// Modes:
//   ConstantPower  - -3 dB centre (standard stereo)
//   Minus4_5dB     - -4.5 dB centre (classic "LCR-ish" feel)
//   Linear         - 0 dB centre, equal-gain (mono-compatible)
// ============================================================================
#pragma once

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

enum class PanLaw : std::int32_t { ConstantPower = 0, Minus4_5dB, Linear };

class PanProcessor final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Pan"; }

    /// pan in [-1, +1]; -1 = hard left, 0 = centre, +1 = hard right.
    void setPan(float pan) noexcept;
    [[nodiscard]] float pan() const noexcept { return pan_; }
    void setPanLaw(PanLaw law) noexcept { law_ = law; }
    [[nodiscard]] PanLaw panLaw() const noexcept { return law_; }

    /// Forces the effect to behave as a dual-mono panner for mono sources.
    void setMonoSource(bool monoSource) noexcept { monoSource_ = monoSource; }

    [[nodiscard]] int latencySamples() const noexcept override { return 0; }

private:
    void updateTargets() noexcept;
    void computeGains(float pan, float& gainL, float& gainR) const noexcept;

    LinearSmoothedValue gainL_;
    LinearSmoothedValue gainR_;
    float pan_ = 0.0f;
    PanLaw law_ = PanLaw::ConstantPower;
    bool monoSource_ = false;
    float targetL_ = 1.0f;
    float targetR_ = 1.0f;
};

} // namespace aura::dsp
