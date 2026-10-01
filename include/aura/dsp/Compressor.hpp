// ============================================================================
// AURA DAW - dsp/Compressor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Feed-forward peak compressor with:
//   * soft knee (quadratic interpolation over the knee width)
//   * stereo-linked detection (max of channels) so the image never shifts
//   * optional external side-chain input (architecture in place; the engine
//     wires bus -> side-chain when the user enables it)
//   * program-dependent auto-makeup option, plus manual makeup
//   * attack/release coefficients recalculated per block (standard practice)
//
// NOT implemented in V1 (documented limitations): look-ahead, parallel mix
// (available via the engine's wet/dry insert wrapper), and side-chain EQ.
// The parameter set matches the brief: threshold, ratio, attack, release,
// knee, makeup gain.
// ============================================================================
#pragma once

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

struct CompressorSettings {
    float thresholdDb = -18.0f;
    float ratio = 4.0f;      ///< 1 = no compression, 20 = limiting-ish
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
    bool autoMakeup = false;
    bool bypassed = false;
};

class Compressor final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Compressor"; }

    void setSettings(const CompressorSettings& settings) noexcept;
    [[nodiscard]] const CompressorSettings& settings() const noexcept { return settings_; }

    void setThresholdDb(float db) noexcept;
    void setRatio(float ratio) noexcept;
    void setAttackMs(float ms) noexcept;
    void setReleaseMs(float ms) noexcept;
    void setKneeDb(float db) noexcept;
    void setMakeupDb(float db) noexcept;
    void setAutoMakeup(bool enabled) noexcept;

    /// Current gain reduction in dB (positive number = gain being reduced).
    /// Lock-free read for the UI meter.
    [[nodiscard]] float gainReductionDb() const noexcept {
        return gainReductionDb_.load(std::memory_order_relaxed);
    }

    /// Optional external side-chain. When set, the detector uses this block
    /// instead of the processed audio. The engine clears it each block, so a
    /// disconnected side-chain falls back to internal detection automatically.
    void setSidechainInput(const AudioBlockView& block) noexcept { sidechain_ = block; }
    void clearSidechainInput() noexcept { sidechain_ = {}; }

    [[nodiscard]] int latencySamples() const noexcept override { return 0; }
    [[nodiscard]] bool isActive() const noexcept override { return !settings_.bypassed; }

private:
    [[nodiscard]] float computeGainDb(float inputLevelDb) const noexcept;
    void updateCoefficients() noexcept;

    CompressorSettings settings_{};
    float attackCoefficient_ = 0.0f;
    float releaseCoefficient_ = 0.0f;
    float envelope_ = 0.0f; ///< linear detector envelope
    float makeupGain_ = 1.0f;
    LinearSmoothedValue makeupSmoother_;
    std::atomic<float> gainReductionDb_{0.0f};
    AudioBlockView sidechain_{};
    bool coefficientsDirty_ = true;
};

} // namespace aura::dsp
