// ============================================================================
// AURA DAW - src/dsp/PanProcessor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/PanProcessor.hpp"

#include <cmath>

namespace aura::dsp {

void PanProcessor::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    updateTargets();
    gainL_.reset(sampleRate, targetL_, 0.015f);
    gainR_.reset(sampleRate, targetR_, 0.015f);
}

void PanProcessor::reset() noexcept {
    gainL_.snapTo(targetL_);
    gainR_.snapTo(targetR_);
}

void PanProcessor::setPan(float pan) noexcept {
    pan_ = math::clamp(pan, -1.0f, 1.0f);
    updateTargets();
    gainL_.setTarget(targetL_);
    gainR_.setTarget(targetR_);
}

void PanProcessor::updateTargets() noexcept {
    computeGains(pan_, targetL_, targetR_);
}

void PanProcessor::computeGains(float pan, float& gainL, float& gainR) const noexcept {
    const float p = math::clamp(pan, -1.0f, 1.0f);
    switch (law_) {
    case PanLaw::ConstantPower: {
        // cos/sin law: total power stays constant across the sweep.
        const float angle = (monoSource_ ? (p + 1.0f) * 0.5f : (p + 1.0f) * 0.25f + 0.25f) *
                            math::kPiF;
        if (monoSource_) {
            gainL = std::cos(angle);
            gainR = std::sin(angle);
        } else {
            // For a stereo source we attenuate the far channel only, which
            // keeps the centre image intact (a "balance" control).
            gainL = p <= 0.0f ? 1.0f : std::cos((p) * 0.5f * math::kPiF);
            gainR = p >= 0.0f ? 1.0f : std::cos((-p) * 0.5f * math::kPiF);
        }
        break;
    }
    case PanLaw::Minus4_5dB: {
        const float centre = math::dbToGain(-4.5f);
        gainL = p <= 0.0f ? centre + (1.0f - centre) * (1.0f + p) : centre * (1.0f - p);
        gainR = p >= 0.0f ? centre + (1.0f - centre) * (1.0f - p) : centre * (1.0f + p);
        break;
    }
    case PanLaw::Linear:
    default: {
        gainL = p <= 0.0f ? 1.0f : 1.0f - p;
        gainR = p >= 0.0f ? 1.0f : 1.0f + p;
        break;
    }
    }
}

void PanProcessor::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty())
        return;

    if (block.numChannels == 1) {
        // Mono source panned hard left/right must still be positioned; we
        // cannot do that without a second channel, so the engine up-mixes mono
        // tracks to stereo before the panner (documented behaviour).
        if (!gainL_.isSmoothing()) {
            const float gain = gainL_.current();
            applyGainRamp(block.channelPointers[0], block.numFrames, gain, gain);
        } else {
            for (int i = 0; i < block.numFrames; ++i)
                block.channelPointers[0][i] *= gainL_.next();
        }
        return;
    }

    float* left = block.channelPointers[0];
    float* right = block.channelPointers[1];
    for (int i = 0; i < block.numFrames; ++i) {
        const float gainL = gainL_.next();
        const float gainR = gainR_.next();
        left[i] *= gainL;
        right[i] *= gainR;
    }
}

} // namespace aura::dsp
