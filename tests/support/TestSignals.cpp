// ============================================================================
// AURA DAW - tests/support/TestSignals.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "support/TestSignals.hpp"

#include <algorithm>
#include <limits>

namespace aura::test {

void fillSine(std::vector<float>& buffer, double frequency, double sampleRate, float amplitude,
              double phase) {
    if (sampleRate <= 0.0)
        return;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        buffer[i] = amplitude * static_cast<float>(std::sin(kTwoPiLocal * frequency * t + phase));
    }
}

void addSine(std::vector<float>& buffer, double frequency, double sampleRate, float amplitude,
             double phase) {
    if (sampleRate <= 0.0)
        return;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        buffer[i] += amplitude * static_cast<float>(std::sin(kTwoPiLocal * frequency * t + phase));
    }
}

float peak(const std::vector<float>& buffer) {
    float result = 0.0f;
    for (const float value : buffer)
        result = std::max(result, std::abs(value));
    return result;
}

float peakFrom(const std::vector<float>& buffer, std::size_t firstSample) {
    float result = 0.0f;
    for (std::size_t i = std::min(firstSample, buffer.size()); i < buffer.size(); ++i)
        result = std::max(result, std::abs(buffer[i]));
    return result;
}

double rms(const std::vector<float>& buffer) {
    if (buffer.empty())
        return 0.0;
    double sum = 0.0;
    for (const float value : buffer)
        sum += static_cast<double>(value) * static_cast<double>(value);
    return std::sqrt(sum / static_cast<double>(buffer.size()));
}

double rmsFrom(const std::vector<float>& buffer, std::size_t firstSample) {
    const std::size_t start = std::min(firstSample, buffer.size());
    if (start >= buffer.size())
        return 0.0;
    double sum = 0.0;
    for (std::size_t i = start; i < buffer.size(); ++i)
        sum += static_cast<double>(buffer[i]) * static_cast<double>(buffer[i]);
    return std::sqrt(sum / static_cast<double>(buffer.size() - start));
}

double magnitudeAtFrequency(const std::vector<float>& buffer, double frequency, double sampleRate,
                            bool normalise) {
    if (buffer.empty() || sampleRate <= 0.0)
        return 0.0;
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const double angle = kTwoPiLocal * frequency * static_cast<double>(i) / sampleRate;
        real += static_cast<double>(buffer[i]) * std::cos(angle);
        imaginary += static_cast<double>(buffer[i]) * std::sin(angle);
    }
    double magnitude = std::sqrt(real * real + imaginary * imaginary);
    if (normalise)
        magnitude *= 2.0 / static_cast<double>(buffer.size());
    return magnitude;
}

double magnitudeDbAtFrequency(const std::vector<float>& buffer, double frequency,
                              double sampleRate) {
    const double magnitude = magnitudeAtFrequency(buffer, frequency, sampleRate);
    if (magnitude <= 1.0e-12)
        return -240.0;
    return 20.0 * std::log10(magnitude);
}

bool allFinite(const std::vector<float>& buffer) {
    for (const float value : buffer) {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

float maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t count = std::min(a.size(), b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < count; ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

Block::Block(int channelCount, int frames, float initial)
    : channels(static_cast<std::size_t>(std::max(1, channelCount)),
               std::vector<float>(static_cast<std::size_t>(std::max(1, frames)), initial)) {
    pointers.reserve(channels.size());
    for (auto& channel : this->channels)
        pointers.push_back(channel.data());
}

dsp::AudioBlockView Block::view(int frames) {
    dsp::AudioBlockView block;
    block.channelPointers = pointers.data();
    block.numChannels = static_cast<int>(pointers.size());
    block.numFrames = frames;
    return block;
}

const dsp::AudioBlockView Block::view(int frames) const {
    dsp::AudioBlockView block;
    block.channelPointers = const_cast<float* const*>(pointers.data());
    block.numChannels = static_cast<int>(pointers.size());
    block.numFrames = frames;
    return block;
}

} // namespace aura::test
