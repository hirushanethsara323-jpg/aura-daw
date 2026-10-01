// ============================================================================
// AURA DAW - src/dsp/Biquad.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// RBJ audio EQ cookbook coefficient computation.
// ============================================================================
#include "aura/dsp/Biquad.hpp"

namespace aura::dsp {
namespace {

struct PoleCoefficients {
    double a1 = 0.0;
    double a2 = 0.0;
};

/// Common denominator term from the cookbook: alpha = sin(w0) / (2Q).
PoleCoefficients computePoles(double w0, double sinW0, double cosW0, double alpha) noexcept {
    (void)w0;
    (void)sinW0;
    (void)cosW0;
    PoleCoefficients poles;
    const double a0 = 1.0 + alpha;
    poles.a1 = -2.0 * cosW0 / a0;
    poles.a2 = (1.0 - alpha) / a0;
    return poles;
}

BiquadCoefficients normalize(double b0, double b1, double b2, double a0, double a1,
                            double a2) noexcept {
    BiquadCoefficients out;
    out.b0 = static_cast<float>(b0 / a0);
    out.b1 = static_cast<float>(b1 / a0);
    out.b2 = static_cast<float>(b2 / a0);
    out.a1 = static_cast<float>(a1 / a0);
    out.a2 = static_cast<float>(a2 / a0);
    return out;
}

} // namespace

BiquadCoefficients BiquadDesign::lowPass(double sampleRate, double frequency, double q) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b1 = 1.0 - cosW0;
    const double b0 = b1 * 0.5;
    const double b2 = b0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::highPass(double sampleRate, double frequency, double q) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b0 = (1.0 + cosW0) * 0.5;
    const double b1 = -(1.0 + cosW0);
    const double b2 = b0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::bandPass(double sampleRate, double frequency, double q) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::notch(double sampleRate, double frequency, double q) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b0 = 1.0;
    const double b1 = -2.0 * cosW0;
    const double b2 = 1.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::allPass(double sampleRate, double frequency, double q) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b0 = 1.0 - alpha;
    const double b1 = -2.0 * cosW0;
    const double b2 = 1.0 + alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::peaking(double sampleRate, double frequency, double q,
                                         double gainDb) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    q = math::clamp(q, 0.05, 20.0);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * cosW0;
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cosW0;
    const double a2 = 1.0 - alpha / A;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::lowShelf(double sampleRate, double frequency, double slope,
                                          double gainDb) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    // S is the shelf slope; the cookbook relation uses alpha = sin(w0)/2 *
    // sqrt((A + 1/A) * (1/S - 1) + 2).
    const double S = math::clamp(slope, 0.1, 1.0);
    const double alpha =
        sinW0 * 0.5 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha);
    const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0);
    const double b2 = A * ((A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha);
    const double a0 = (A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha;
    const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW0);
    const double a2 = (A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoefficients BiquadDesign::highShelf(double sampleRate, double frequency, double slope,
                                           double gainDb) noexcept {
    frequency = clampFrequency(frequency, sampleRate);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = math::kTwoPi * frequency / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double S = math::clamp(slope, 0.1, 1.0);
    const double alpha =
        sinW0 * 0.5 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0);
    const double b2 = A * ((A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW0);
    const double a2 = (A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha;
    return normalize(b0, b1, b2, a0, a1, a2);
}

double BiquadDesign::magnitudeDb(const BiquadCoefficients& c, double frequency,
                                 double sampleRate) noexcept {
    // Evaluate |H(e^{jw})| for a TDF-II biquad:
    //   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    const double w = math::kTwoPi * frequency / sampleRate;
    const double cos1 = std::cos(-w);
    const double sin1 = std::sin(-w);
    const double cos2 = std::cos(-2.0 * w);
    const double sin2 = std::sin(-2.0 * w);

    const double numReal = c.b0 + c.b1 * cos1 + c.b2 * cos2;
    const double numImag = c.b1 * sin1 + c.b2 * sin2;
    const double denReal = 1.0 + c.a1 * cos1 + c.a2 * cos2;
    const double denImag = c.a1 * sin1 + c.a2 * sin2;

    const double numerator = std::sqrt(numReal * numReal + numImag * numImag);
    const double denominator = std::sqrt(denReal * denReal + denImag * denImag);
    if (denominator < 1.0e-20)
        return -240.0;
    const double magnitude = numerator / denominator;
    if (magnitude <= 1.0e-12)
        return -240.0;
    return 20.0 * std::log10(magnitude);
}

} // namespace aura::dsp
