// ============================================================================
// AURA DAW - src/dsp/NoiseGenerator.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/NoiseGenerator.hpp"

namespace aura::dsp {

void NoiseGenerator::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    levelSmoother_.reset(sampleRate, 0.0f, 0.02f);
    reset();
}

void NoiseGenerator::reset() noexcept {
    for (auto& state : pinkState_)
        state = 0.0f;
    brownState_ = 0.0f;
    lastSample_ = 0.0f;
}

float NoiseGenerator::nextSample() noexcept {
    const float white = rng_.nextFloat();
    switch (colour_) {
    case NoiseColour::White: return white;
    case NoiseColour::Pink: {
        // Cascade of one-pole filters approximating -3 dB/octave across the
        // audio band (the classic 7-state structure).
        pinkState_[0] = 0.99886f * pinkState_[0] + white * 0.0555179f;
        pinkState_[1] = 0.99332f * pinkState_[1] + white * 0.0750759f;
        pinkState_[2] = 0.96900f * pinkState_[2] + white * 0.1538520f;
        pinkState_[3] = 0.86650f * pinkState_[3] + white * 0.3104856f;
        pinkState_[4] = 0.55000f * pinkState_[4] + white * 0.5329522f;
        pinkState_[5] = -0.7616f * pinkState_[5] - white * 0.0168980f;
        const float pink = pinkState_[0] + pinkState_[1] + pinkState_[2] + pinkState_[3] +
                           pinkState_[4] + pinkState_[5] + pinkState_[6] + white * 0.5362f;
        pinkState_[6] = white * 0.115926f;
        for (auto& state : pinkState_)
            state = math::flushDenormal(state);
        return pink * 0.11f; // normalise to roughly +/-1
    }
    case NoiseColour::Brown: {
        brownState_ = math::flushDenormal((brownState_ + white * 0.02f) * 0.998f);
        return math::clamp(brownState_ * 3.5f, -1.0f, 1.0f);
    }
    }
    return 0.0f;
}

void NoiseGenerator::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || !enabled_.load(std::memory_order_relaxed))
        return;
    for (int i = 0; i < block.numFrames; ++i) {
        const float level = levelSmoother_.next();
        const float value = nextSample() * level;
        for (int channel = 0; channel < block.numChannels; ++channel)
            block.channelPointers[channel][i] += value;
    }
    lastSample_ = 0.0f;
}

} // namespace aura::dsp
