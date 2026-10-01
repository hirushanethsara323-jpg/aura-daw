// ============================================================================
// AURA DAW - dsp/Reverb.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "AURA Space" - an original, CPU-efficient algorithmic reverb.
//
// Algorithm (documented for maintenance, and deliberately NOT a reimplementation
// of any existing product):
//   1. Input conditioning: high-pass to keep DC/rumble out of the network,
//      and a 4-tap input diffusion all-pass chain.
//   2. Feedback Delay Network (FDN) with 4 delay lines whose lengths are
//      mutually prime (scaled per sample rate), a lossless 4x4 Hadamard mixing
//      matrix, and per-line damping low-pass + gain decay.
//   3. Two decorrelated early-reflection taps taken from the input diffuser to
//      give the "small room" cue before the late tail builds.
//   4. Output: stereo from the FDN lines through a short all-pass + shelf to
//      mimic air absorption (tone control), mixed with dry.
//
// Why FDN: it is the most reverb per CPU cycle of the classic structures; four
// lines are enough for a dense tail and it maps cleanly to SIMD later. Room
// size changes the delay-line lengths (musically: pre-delay + decay), and the
// parameters are smoothed to avoid zipper noise on automated changes.
// ============================================================================
#pragma once

#include <array>
#include <vector>

#include "aura/dsp/Biquad.hpp"
#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

struct ReverbSettings {
    /// 0 = small tight room, 1 = huge hall.
    float size = 0.5f;
    /// Decay time (RT60-ish) in seconds: 0.2 .. 12.
    float decaySeconds = 1.8f;
    /// Damping (high-frequency absorption) 0 = bright, 1 = dark.
    float damping = 0.5f;
    /// Pre-delay in ms: 0 .. 200.
    float preDelayMs = 12.0f;
    /// Wet/dry: 0 = fully dry, 1 = fully wet.
    float mix = 0.25f;
    /// Output low cut / high cut for tone shaping.
    float lowCutHz = 120.0f;
    float highCutHz = 9000.0f;
    /// Modulation depth (subtle chorusing of the tail) 0 .. 1.
    float modulation = 0.15f;
    bool freeze = false;
    bool bypassed = false;
};

class Reverb final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Reverb"; }
    [[nodiscard]] int latencySamples() const noexcept override { return preDelaySamples_; }

    void setSettings(const ReverbSettings& settings) noexcept;
    [[nodiscard]] const ReverbSettings& settings() const noexcept { return settings_; }

    /// Reports whether the tail is still audible, so the graph can keep
    /// processing (and not truncate the tail when the transport stops).
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] bool hasTail() const noexcept override;

private:
    static constexpr int kNumLines = 4;

    struct AllPass {
        std::vector<float> buffer;
        std::size_t writeIndex = 0;
        float feedback = 0.5f;
    };

    struct CombLine {
        std::vector<float> buffer;
        std::size_t writeIndex = 0;
        float dampingState = 0.0f;
        float delaySamples = 100.0f;
    };

    [[nodiscard]] float readDelayed(const std::vector<float>& buffer, std::size_t writeIndex,
                                    float delaySamples) const noexcept;
    void rebuildDelays();

    std::array<CombLine, kNumLines> lines_{};
    std::array<AllPass, 4> inputDiffusion_{};
    std::array<AllPass, 2> outputDiffusion_{};
    std::vector<float> preDelayBuffer_;
    std::size_t preDelayWrite_ = 0;
    int preDelaySamples_ = 512;

    OnePole inputHighPass_;
    Biquad<2> highCut_;
    Biquad<2> lowCut_;

    // Per-line modulation LFOs (sine, mutually prime rates for decorrelation).
    std::array<float, kNumLines> lfoPhase_{};
    std::array<float, kNumLines> lfoIncrement_{};

    ReverbSettings settings_{};
    LinearSmoothedValue mixSmoother_;
    LinearSmoothedValue decaySmoother_;
    float tailEnergy_ = 0.0f;
    float tailTimer_ = 0.0f;
};

} // namespace aura::dsp
