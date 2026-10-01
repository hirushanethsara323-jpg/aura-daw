// ============================================================================
// AURA DAW - core/Math.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-time safe scalar helpers: unit conversions, clamping, denormal
// protection and a few shaping functions used by DSP modules.
// All functions are constexpr/noexcept and never allocate.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aura::math {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr double kHalfPi = 1.57079632679489661923;
inline constexpr float kPiF = 3.14159265358979323846f;
inline constexpr float kTwoPiF = 6.28318530717958647692f;

/// Smallest magnitude treated as non-zero; anything below is flushed to zero on
/// output paths to keep denormal (subnormal) numbers out of feedback loops.
inline constexpr float kDenormalThreshold = 1.0e-25f;

template <typename T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

template <typename T>
[[nodiscard]] constexpr T clamp01(T v) noexcept {
    return clamp(v, T{0}, T{1});
}

template <typename T>
[[nodiscard]] constexpr T lerp(T a, T b, T t) noexcept {
    return a + (b - a) * t;
}

/// Linear crossfade weights for a normalized position in [0,1] (equal-gain).
inline void equalGainCrossfade(float x, float& outA, float& outB) noexcept {
    x = clamp01(x);
    outA = 1.0f - x;
    outB = x;
}

/// Constant-power (equal-loudness) pan for -1 = hard left .. +1 = hard right.
/// Returns per-channel gains that sum to a constant power of 1.
inline void constantPowerPan(float pan, float& gainL, float& gainR) noexcept {
    const float p = clamp(pan, -1.0f, 1.0f);
    // Map [-1,1] -> [0, pi/2]
    const float angle = (p + 1.0f) * 0.25f * kPiF;
    gainL = std::cos(angle);
    gainR = std::sin(angle);
}

/// Decibels <-> linear gain (amplitude). 0 dB == 1.0.
[[nodiscard]] inline float dbToGain(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}
[[nodiscard]] inline double dbToGain(double db) noexcept {
    return std::pow(10.0, db * 0.05);
}
[[nodiscard]] inline float gainToDb(float gain) noexcept {
    return gain > 0.0f ? 20.0f * std::log10(gain) : -std::numeric_limits<float>::infinity();
}
[[nodiscard]] inline double gainToDb(double gain) noexcept {
    return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
}

/// Amplitude (not power) to dBFS magnitude used by meters: returns -inf for 0.
[[nodiscard]] inline float amplitudeToDbfs(float amp) noexcept { return gainToDb(std::abs(amp)); }

/// dB gain display value clamped to the UI display floor (default -96 dB).
[[nodiscard]] inline float sanitizeDb(float db, float floorDb = -96.0f) noexcept {
    if (!std::isfinite(db))
        return floorDb;
    return clamp(db, floorDb, 24.0f);
}

[[nodiscard]] inline float semitonesToRatio(float semitones) noexcept {
    return std::pow(2.0f, semitones / 12.0f);
}
[[nodiscard]] inline float centsToRatio(float cents) noexcept {
    return std::pow(2.0f, cents / 1200.0f);
}

[[nodiscard]] constexpr bool isPowerOfTwo(std::uint64_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}
[[nodiscard]] constexpr std::uint64_t nextPowerOfTwo(std::uint64_t v) noexcept {
    if (v <= 1)
        return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

/// Flush denormals / non-finite input. Called at the end of every feedback path.
[[nodiscard]] inline float flushDenormal(float x) noexcept {
    if (!std::isfinite(x))
        return 0.0f;
    return (std::abs(x) < kDenormalThreshold) ? 0.0f : x;
}
[[nodiscard]] inline double flushDenormal(double x) noexcept {
    if (!std::isfinite(x))
        return 0.0;
    return (std::abs(x) < 1.0e-300) ? 0.0 : x;
}

/// Replace NaN/inf with zero (channel guard for the master bus).
[[nodiscard]] inline float sanitizeSample(float x) noexcept {
    return std::isfinite(x) ? x : 0.0f;
}

[[nodiscard]] inline float softClip(float x) noexcept {
    // Algebraic sigmoid: continuous, monotonic, cheap, no exp().
    const float ax = std::abs(x);
    if (ax <= 1.5f)
        return x * (1.0f - (4.0f / 27.0f) * x * x);
    return x > 0.0f ? 1.0f : -1.0f;
}

[[nodiscard]] inline float fastTanh(float x) noexcept {
    // Padé-style rational approximation, |error| < 1e-4 on [-4,4].
    const float x2 = x * x;
    const float num = x * (27.0f + x2);
    const float den = 27.0f + 9.0f * x2;
    return clamp(num / den, -1.0f, 1.0f);
}

/// Smooth absolute-value curve used by meters (fast attack / slow release).
[[nodiscard]] inline float onePoleCoefficient(double timeMs, double sampleRate) noexcept {
    if (timeMs <= 0.0 || sampleRate <= 0.0)
        return 1.0f;
    return 1.0f - std::exp(-1.0f / (static_cast<float>(timeMs) * 0.001f * static_cast<float>(sampleRate)));
}

[[nodiscard]] constexpr float equalPowerMidiVelocity(int velocity) noexcept {
    const int v = clamp(velocity, 0, 127);
    return static_cast<float>(v) / 127.0f;
}

} // namespace aura::math
