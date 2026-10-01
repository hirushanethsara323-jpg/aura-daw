// ============================================================================
// AURA DAW - dsp/Biquad.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Direct-form-II transposed biquad with RBJ cookbook coefficient design.
// Design notes (documented because filter choice is audible and bugs here are
// subtle):
//   * TDF-II has the best numerical behaviour for time-varying coefficients
//     with only two state variables per channel, which matters because AURA
//     sweeps filter parameters from automation and the UI.
//   * Coefficients are computed in double precision, state is float: a 20 Hz
//     high-pass at 192 kHz is not representable accurately in float
//     coefficients.
//   * Each channel gets its own state; coefficients are shared.
// ============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "aura/core/Math.hpp"

namespace aura::dsp {

enum class FilterKind : std::int32_t {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    AllPass,
    Peaking,
    LowShelf,
    HighShelf,
};

struct BiquadCoefficients {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

    [[nodiscard]] bool isBypass() const noexcept {
        return b0 == 1.0f && b1 == 0.0f && b2 == 0.0f && a1 == 0.0f && a2 == 0.0f;
    }
};

/// Cookbook filter design (RBJ audio EQ cookbook), double precision.
class BiquadDesign {
public:
    static BiquadCoefficients lowPass(double sampleRate, double frequency, double q) noexcept;
    static BiquadCoefficients highPass(double sampleRate, double frequency, double q) noexcept;
    static BiquadCoefficients bandPass(double sampleRate, double frequency, double q) noexcept;
    static BiquadCoefficients notch(double sampleRate, double frequency, double q) noexcept;
    static BiquadCoefficients allPass(double sampleRate, double frequency, double q) noexcept;
    /// Bell/peaking filter; gainDb may be positive or negative.
    static BiquadCoefficients peaking(double sampleRate, double frequency, double q,
                                      double gainDb) noexcept;
    static BiquadCoefficients lowShelf(double sampleRate, double frequency, double slope,
                                       double gainDb) noexcept;
    static BiquadCoefficients highShelf(double sampleRate, double frequency, double slope,
                                        double gainDb) noexcept;

    /// Magnitude response in dB at `frequency` - used by EQ unit tests and the
    /// UI's response curve display. Never called from the audio thread.
    [[nodiscard]] static double magnitudeDb(const BiquadCoefficients& coefficients, double frequency,
                                            double sampleRate) noexcept;

    /// Clamps `frequency` into a stable range for the given sample rate.
    [[nodiscard]] static double clampFrequency(double frequency, double sampleRate) noexcept {
        const double nyquist = sampleRate * 0.5;
        return math::clamp(frequency, 1.0, nyquist * 0.999);
    }
};

/// One biquad section with up to N channels of independent state.
///
/// MaxChannels stays an `int` (the engine counts channels as int everywhere); the
/// array extent below is the one place that needs a size_t, hence the explicit
/// conversion - it keeps -Wsign-conversion quiet without weakening the flags.
template <int MaxChannels = 2>
class Biquad {
    static_assert(MaxChannels > 0, "a biquad needs at least one channel of state");
public:
    void reset() noexcept {
        for (auto& channel : state_)
            channel = {};
    }

    void setCoefficients(const BiquadCoefficients& coefficients) noexcept {
        coefficients_ = coefficients;
    }
    [[nodiscard]] const BiquadCoefficients& coefficients() const noexcept { return coefficients_; }

    [[nodiscard]] float processSample(int channel, float input) noexcept {
        State& state = state_[static_cast<std::size_t>(channel)];
        // TDF-II
        const float output = coefficients_.b0 * input + state.z1;
        state.z1 = coefficients_.b1 * input - coefficients_.a1 * output + state.z2;
        state.z2 = coefficients_.b2 * input - coefficients_.a2 * output;
        // Flush denormals in the feedback path (no-op when FTZ is enabled).
        state.z1 = math::flushDenormal(state.z1);
        state.z2 = math::flushDenormal(state.z2);
        return output;
    }

    void processBlock(float* const* channels, int numChannels, int numFrames) noexcept {
        if (coefficients_.isBypass())
            return;
        const int channelCount = numChannels < MaxChannels ? numChannels : MaxChannels;
        for (int channel = 0; channel < channelCount; ++channel) {
            State& state = state_[static_cast<std::size_t>(channel)];
            float z1 = state.z1;
            float z2 = state.z2;
            const float b0 = coefficients_.b0;
            const float b1 = coefficients_.b1;
            const float b2 = coefficients_.b2;
            const float a1 = coefficients_.a1;
            const float a2 = coefficients_.a2;
            float* data = channels[channel];
            for (int i = 0; i < numFrames; ++i) {
                const float input = data[i];
                const float output = b0 * input + z1;
                z1 = b1 * input - a1 * output + z2;
                z2 = b2 * input - a2 * output;
                data[i] = output;
            }
            state.z1 = math::flushDenormal(z1);
            state.z2 = math::flushDenormal(z2);
        }
        // Any channels beyond MaxChannels are intentionally left untouched;
        // the engine only ever instantiates filters for the channel count it
        // prepared, and asserts that in debug builds.
    }

private:
    struct State {
        float z1 = 0.0f;
        float z2 = 0.0f;
    };

    BiquadCoefficients coefficients_{};
    std::array<State, static_cast<std::size_t>(MaxChannels)> state_{};
};

/// First-order one-pole low-pass/high-pass used for DC blockers and tilt EQ.
class OnePole {
public:
    void reset() noexcept { state_ = 0.0f; }

    void setLowPass(double sampleRate, double frequency) noexcept {
        const double coefficient = 1.0 - std::exp(-2.0 * math::kPi * frequency / sampleRate);
        coefficient_ = static_cast<float>(math::clamp(coefficient, 0.0, 1.0));
        mode_ = Mode::LowPass;
    }

    void setHighPass(double sampleRate, double frequency) noexcept {
        setLowPass(sampleRate, frequency);
        mode_ = Mode::HighPass;
    }

    [[nodiscard]] float processSample(float input) noexcept {
        state_ += coefficient_ * (input - state_);
        state_ = math::flushDenormal(state_);
        return mode_ == Mode::LowPass ? state_ : input - state_;
    }

private:
    enum class Mode { LowPass, HighPass };
    Mode mode_ = Mode::LowPass;
    float coefficient_ = 0.1f;
    float state_ = 0.0f;
};

} // namespace aura::dsp
