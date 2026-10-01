// ============================================================================
// AURA DAW - src/dsp/Processor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Processor.hpp"

#include <cmath>
#include <cstring>

namespace aura::dsp {

bool blockIsFinite(const AudioBlockView& block) noexcept {
    for (int channel = 0; channel < block.numChannels; ++channel) {
        const float* data = block.channelPointers[channel];
        if (!data)
            return false;
        for (int i = 0; i < block.numFrames; ++i) {
            if (!std::isfinite(data[i]))
                return false;
        }
    }
    return true;
}

void sanitizeBlock(AudioBlockView& block) noexcept {
    for (int channel = 0; channel < block.numChannels; ++channel) {
        float* data = block.channelPointers[channel];
        for (int i = 0; i < block.numFrames; ++i) {
            const float value = data[i];
            if (!std::isfinite(value))
                data[i] = 0.0f;
            else if (std::abs(value) < math::kDenormalThreshold)
                data[i] = 0.0f;
        }
    }
}

float blockPeak(const AudioBlockView& block) noexcept {
    float peak = 0.0f;
    for (int channel = 0; channel < block.numChannels; ++channel) {
        const float* data = block.channelPointers[channel];
        for (int i = 0; i < block.numFrames; ++i) {
            const float magnitude = std::abs(data[i]);
            if (magnitude > peak)
                peak = magnitude;
        }
    }
    return peak;
}

void applyGainRamp(float* buffer, int numFrames, float from, float to) noexcept {
    if (numFrames <= 0)
        return;
    if (numFrames == 1) {
        buffer[0] *= to;
        return;
    }
    const float step = (to - from) / static_cast<float>(numFrames - 1);
    float gain = from;
    for (int i = 0; i < numFrames; ++i) {
        buffer[i] *= gain;
        gain += step;
    }
}

void clearBlock(AudioBlockView& block) noexcept {
    for (int channel = 0; channel < block.numChannels; ++channel) {
        if (block.channelPointers[channel])
            std::memset(block.channelPointers[channel], 0,
                        static_cast<std::size_t>(block.numFrames) * sizeof(float));
    }
}

} // namespace aura::dsp
