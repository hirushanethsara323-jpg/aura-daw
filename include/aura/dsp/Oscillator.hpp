// ============================================================================
// AURA DAW - include/aura/dsp/Oscillator.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio-rate oscillator with anti-aliased waveforms (PolyBLEP for saw/square,
// integrated square for triangle) plus a pure sine. Used by the built-in test
// tone generator, the metronome and future built-in instruments.
// ============================================================================
#pragma once

#include <cstdint>

#include "aura/core/Math.hpp"

namespace aura::dsp {

enum class Waveform : std::int32_t { Sine = 0, Triangle, Saw, Square, Noise };

class Oscillator {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateIncrement();
        phase_ = 0.0;
    }

    void reset(double phase = 0.0) noexcept { phase_ = phase; }

    void setFrequency(double hz) noexcept {
        frequency_ = math::clamp(hz, 0.0, sampleRate_ * 0.49);
        updateIncrement();
    }
    [[nodiscard]] double frequency() const noexcept { return frequency_; }

    void setWaveform(Waveform waveform) noexcept { waveform_ = waveform; }
    [[nodiscard]] Waveform waveform() const noexcept { return waveform_; }

    /// Generates `numSamples` into `destination` (adds if `additive`).
    void generate(float* destination, int numSamples, bool additive = false) noexcept {
        for (int i = 0; i < numSamples; ++i) {
            const float value = nextSample() * amplitude_;
            destination[i] = additive ? destination[i] + value : value;
        }
    }

    [[nodiscard]] float nextSample() noexcept {
        const double phase = phase_;
        const double increment = increment_;
        phase_ = phase + increment;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        switch (waveform_) {
        case Waveform::Sine: return static_cast<float>(std::sin(phase * math::kTwoPi));
        case Waveform::Saw: {
            // Naive saw minus a PolyBLEP correction at the discontinuity.
            const auto naive = static_cast<float>(2.0 * phase - 1.0);
            return naive - polyBlep(phase, increment);
        }
        case Waveform::Square: {
            float value = phase < 0.5 ? 1.0f : -1.0f;
            value += polyBlep(phase, increment);
            value -= polyBlep(std::fmod(phase + 0.5, 1.0), increment);
            return value;
        }
        case Waveform::Triangle: {
            // Band-limited triangle by integrating a band-limited square.
            float square = phase < 0.5 ? 1.0f : -1.0f;
            square += polyBlep(phase, increment);
            square -= polyBlep(std::fmod(phase + 0.5, 1.0), increment);
            triangleState_ = math::flushDenormal(triangleState_ +
                                                 static_cast<float>(increment * 4.0) * square);
            // Leaky integration keeps DC out without an explicit high-pass.
            triangleState_ *= 0.9995f;
            return math::clamp(triangleState_, -1.0f, 1.0f);
        }
        case Waveform::Noise: return noise_.nextFloat();
        }
        return 0.0f;
    }

    void setAmplitude(float amplitude) noexcept { amplitude_ = math::clamp(amplitude, 0.0f, 4.0f); }
    [[nodiscard]] float amplitude() const noexcept { return amplitude_; }

private:
    void updateIncrement() noexcept {
        increment_ = frequency_ / sampleRate_;
    }

    /// PolyBLEP residual for the discontinuity at phase 0 of a saw.
    [[nodiscard]] float polyBlep(double phase, double increment) const noexcept {
        if (increment <= 0.0)
            return 0.0f;
        if (phase < increment) {
            const double t = phase / increment;
            return static_cast<float>(t + t - t * t - 1.0);
        }
        if (phase > 1.0 - increment) {
            const double t = (phase - 1.0) / increment;
            return static_cast<float>(t * t + t + t + 1.0);
        }
        return 0.0f;
    }

    double sampleRate_ = 48000.0;
    double frequency_ = 440.0;
    double increment_ = 440.0 / 48000.0;
    double phase_ = 0.0;
    float amplitude_ = 1.0f;
    float triangleState_ = 0.0f;
    Waveform waveform_ = Waveform::Sine;
    struct FastNoise {
        std::uint32_t state = 0x12345678u;
        float nextFloat() noexcept {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f;
        }
    };
    FastNoise noise_{};
};

} // namespace aura::dsp
