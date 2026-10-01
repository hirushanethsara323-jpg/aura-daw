// ============================================================================
// AURA DAW - src/dsp/GainProcessor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/GainProcessor.hpp"

namespace aura::dsp {

void GainProcessor::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    smoothedGain_.reset(sampleRate, gainLinear_, rampSeconds_);
}

void GainProcessor::reset() noexcept {
    smoothedGain_.snapTo(gainLinear_);
}

void GainProcessor::setGain(float linearGain) noexcept {
    gainLinear_ = math::clamp(linearGain, 0.0f, 64.0f);
    smoothedGain_.setTarget(gainLinear_);
}

void GainProcessor::setGainDb(float decibels) noexcept {
    setGain(decibels <= -96.0f ? 0.0f : math::dbToGain(decibels));
}

void GainProcessor::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty())
        return;

    const bool invert = invert_.load(std::memory_order_relaxed);
    // Fast path: static gain, no ramp in flight -> plain multiply loop with one
    // coefficient, which the compiler vectorises.
    if (!smoothedGain_.isSmoothing()) {
        const float gain = smoothedGain_.current() * (invert ? -1.0f : 1.0f);
        if (gain == 1.0f)
            return;
        for (int channel = 0; channel < block.numChannels; ++channel) {
            float* data = block.channelPointers[channel];
            for (int i = 0; i < block.numFrames; ++i)
                data[i] *= gain;
        }
        return;
    }

    for (int channel = 0; channel < block.numChannels; ++channel) {
        float* data = block.channelPointers[channel];
        for (int i = 0; i < block.numFrames; ++i) {
            const float gain = smoothedGain_.next() * (invert ? -1.0f : 1.0f);
            data[i] *= gain;
        }
    }
}

} // namespace aura::dsp
