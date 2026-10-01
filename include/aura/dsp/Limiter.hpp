// ============================================================================
// AURA DAW - dsp/Limiter.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look-ahead brick-wall limiter for the master bus.
//
// Design:
//   * Delay line of `lookaheadMs` so gain reduction is applied *before* the
//     peak reaches the output -> no overshoot, no clipping.
//   * Sliding-maximum (monotonic deque) over the look-ahead window gives the
//     exact maximum needed for the window without scanning, O(1) amortised.
//   * Attack smoothed over the look-ahead window (so gain reduction ramps in
//     and reaches the required value exactly when the peak arrives); release is
//     an exponential one-pole.
//   * Reported latency = look-ahead length in samples, which the engine
//     compensates on the master output path.
//
// Documented limitation (V1): sample-peak detection, not true-peak. Inter-sample
// peaks can still exceed the ceiling by up to ~3 dB on heavily limited program
// material at low sample rates. True-peak (ITU-R BS.1770-4, 4x oversampled)
// detection is planned for M9; the class exposes `setTruePeakEnabled()` as the
// integration point and currently returns the honest error path (no-op with a
// diagnostic log) rather than silently pretending.
// ============================================================================
#pragma once

#include <vector>

#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

struct LimiterSettings {
    float ceilingDb = -0.3f;
    float releaseMs = 80.0f;
    /// Look-ahead in ms; 1.5 ms is the default sweet spot (transparent, still
    /// catches drum transients). Range 0.1 .. 20 ms.
    float lookaheadMs = 1.5f;
    bool truePeak = false;
    bool bypassed = false;
};

class Limiter final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Limiter"; }
    [[nodiscard]] int latencySamples() const noexcept override { return lookaheadSamples_; }

    void setSettings(const LimiterSettings& settings) noexcept;
    [[nodiscard]] const LimiterSettings& settings() const noexcept { return settings_; }

    void setCeilingDb(float db) noexcept;
    void setReleaseMs(float ms) noexcept;
    void setLookaheadMs(float ms) noexcept;
    /// Enables/disables true-peak detection. Returns false (and does nothing)
    /// while the feature is not implemented - callers must handle that.
    [[nodiscard]] bool setTruePeakEnabled(bool enabled) noexcept;

    [[nodiscard]] float gainReductionDb() const noexcept {
        return gainReductionDb_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isActive() const noexcept override { return !settings_.bypassed; }

private:
    void rebuildDelayLines();
    void updateCoefficients() noexcept;

    LimiterSettings settings_{};
    std::vector<std::vector<float>> delayLines_; ///< [channel][frame]
    std::size_t writeIndex_ = 0;
    int lookaheadSamples_ = 0;
    float ceilingLinear_ = 0.7f;
    float releaseCoefficient_ = 0.0f;
    float currentGain_ = 1.0f;
    /// Monotonic deque of (index, magnitude) for sliding maximum over the window.
    std::vector<std::pair<std::size_t, float>> deque_;
    std::size_t dequeHead_ = 0;
    std::atomic<float> gainReductionDb_{0.0f};
    bool coefficientsDirty_ = true;
};

} // namespace aura::dsp
