// ============================================================================
// AURA DAW - include/aura/dsp/Distortion.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Four original waveshaping algorithms written for AURA (none of them is a
// reimplementation of a commercial product):
//
//   Drive - asymmetric soft saturation (even/odd harmonics, bias control)
//   Fold  - wavefolder, rich bell-like upper harmonics
//   Fuzz  - hard asymmetric clipping with a note-tail gate
//   Crush - bit-depth reduction + decimation with optional anti-alias filter
//
// Anti-aliasing: a 2x polyphase half-band upsample/shape/downsample stage
// (15-tap, generated at prepare time). It is bypassable for deliberate lo-fi
// settings. The latency it introduces is reported honestly through
// Processor::latencySamples() and verified by DistortionTests.cpp.
// ============================================================================
#pragma once

#include <array>

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

enum class DistortionKind : std::int32_t { Drive = 0, Fold, Fuzz, Crush };

struct DistortionSettings {
    DistortionKind kind = DistortionKind::Drive;
    float drive = 0.4f;      ///< 0 .. 1, mapped to 0 .. +36 dB of input gain
    float bias = 0.0f;       ///< -1 .. 1 asymmetry (adds even harmonics)
    float tone = 0.5f;       ///< 0 = dark, 1 = bright (post one-pole)
    float mix = 1.0f;        ///< 0 = dry, 1 = processed
    float outputDb = -3.0f;  ///< output trim
    float bits = 8.0f;       ///< Crush: quantisation bits (1..16)
    float decimation = 4.0f; ///< Crush: hold factor (1..64)
    bool oversample = true;
    bool bypassed = false;
};

class Distortion final : public Processor {
public:
    /// 31-tap Kaiser half-band. A 15-tap Blackman filter was measured to give
    /// only ~10 dB of anti-alias suppression on a hard-folded 5 kHz tone; the
    /// 31-tap Kaiser (beta = 8) improves the stopband meaningfully while the
    /// extra cost stays negligible next to the waveshaper itself. Measured
    /// suppression figures live in docs/DSP.md and DistortionTests.cpp.
    static constexpr int kHalfBandTaps = 31;
    /// Round-trip latency of the 2x stage in input samples: (taps-1)/2 samples
    /// of group delay at 2x for the interpolator, the same again for the
    /// decimator, i.e. (31-1)/2 = 15 samples at 2x = 15 input samples.
    /// Verified by the impulse test in DistortionTests.cpp.
    static constexpr int kOversamplingLatency = 15;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Distortion"; }
    [[nodiscard]] int latencySamples() const noexcept override {
        return settings_.oversample ? kOversamplingLatency : 0;
    }

    void setSettings(const DistortionSettings& settings) noexcept;
    [[nodiscard]] const DistortionSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] bool isActive() const noexcept override { return !settings_.bypassed; }

private:
    void designHalfBand();
    [[nodiscard]] float shapeDrive(float x) const noexcept;
    [[nodiscard]] float shapeFold(float x) const noexcept;
    [[nodiscard]] float shapeFuzz(float x) noexcept;
    [[nodiscard]] float shapeCrush(float x) noexcept;

    DistortionSettings settings_{};
    LinearSmoothedValue mixSmoother_;
    LinearSmoothedValue outputSmoother_;

    std::array<float, kHalfBandTaps> halfBand_{};
    std::array<std::array<float, kHalfBandTaps>, 2> upHistory_{};
    std::array<std::array<float, kHalfBandTaps>, 2> downHistory_{};
    std::size_t upHistoryIndex_ = 0;
    std::size_t downHistoryIndex_ = 0;
    int crushCounter_ = 0;

    float toneState_ = 0.0f;
    float toneStateR_ = 0.0f;
    float crushHold_ = 0.0f;
    float crushHoldR_ = 0.0f;
    float fuzzGate_ = 0.0f;
};

} // namespace aura::dsp
