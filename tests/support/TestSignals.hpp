// ============================================================================
// AURA DAW - tests/support/TestSignals.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared measurement helpers. Test code is allowed to allocate, use doubles and
// be slow: it never runs on the audio thread. What it must NOT do is measure the
// wrong thing - every helper here documents what it actually measures.
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "aura/dsp/Processor.hpp"

namespace aura::test {

inline constexpr double kTwoPiLocal = 6.28318530717958647692;

/// Fills a buffer with a sine at `frequency` Hz. The amplitude is the exact
/// peak of the discrete signal, so peak measurements are comparable to the
/// theoretical value (a common source of false test failures).
void fillSine(std::vector<float>& buffer, double frequency, double sampleRate,
              float amplitude = 1.0f, double phase = 0.0);

/// Adds a sine to an existing buffer (for multi-tone tests).
void addSine(std::vector<float>& buffer, double frequency, double sampleRate, float amplitude,
             double phase = 0.0);

/// Peak absolute value.
[[nodiscard]] float peak(const std::vector<float>& buffer);

/// Peak absolute value in a sub-range (skipping a warm-up region).
[[nodiscard]] float peakFrom(const std::vector<float>& buffer, std::size_t firstSample);

/// Root mean square.
[[nodiscard]] double rms(const std::vector<float>& buffer);

/// Root mean square of a sub-range.
[[nodiscard]] double rmsFrom(const std::vector<float>& buffer, std::size_t firstSample);

/// Magnitude (linear) of a single frequency bin via a direct Goertzel-style
/// correlation. `normalise` divides by the number of samples so the result is
/// comparable with the amplitude of a pure sine at that frequency.
[[nodiscard]] double magnitudeAtFrequency(const std::vector<float>& buffer, double frequency,
                                          double sampleRate, bool normalise = true);

/// Magnitude in dBFS, with a floor instead of -inf.
[[nodiscard]] double magnitudeDbAtFrequency(const std::vector<float>& buffer, double frequency,
                                            double sampleRate);

/// True when every sample is finite (no NaN/Inf leaked out of a processor).
[[nodiscard]] bool allFinite(const std::vector<float>& buffer);

/// Largest absolute difference between two buffers of equal length.
[[nodiscard]] float maxDifference(const std::vector<float>& a, const std::vector<float>& b);

/// A planar block view over a vector of channel buffers (test convenience).
struct Block {
    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;

    explicit Block(int channels, int frames, float initial = 0.0f);
    [[nodiscard]] dsp::AudioBlockView view(int frames);
    [[nodiscard]] const dsp::AudioBlockView view(int frames) const;
};

} // namespace aura::test
