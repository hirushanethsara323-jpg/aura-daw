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
// ---------------------------------------------------------------------------
// Block-state unrolling of first-order recursions
// ---------------------------------------------------------------------------
//
// Two of the hottest loops in the engine are first-order recursions that have to
// advance once per sample:
//
//   * the level meter's exponential RMS:   s[n] = a*s[n-1] + c*x[n]^2
//   * a node's one-pole gain ramp:         g[n] = a*g[n-1] + c*T
//
// Per sample they carry a loop-carried dependency (each step needs the previous
// one), so the processor stalls on the FP latency of the multiply-add instead of
// running at throughput, and no compiler can vectorise them. Because both are
// linear and time-invariant *within a block* (the coefficient is constant), the
// group of k steps can be expanded in closed form:
//
//   s[n+3] = a^4*s[n-1] + c*(a^3*x[n]^2 + a^2*x[n+1]^2 + a*x[n+2]^2 + x[n+3]^2)
//
// The four products are independent of one another, so the dependency chain
// shrinks from four steps to one and the loop becomes throughput-bound (and
// auto-vectorisable). Measured on the reference profile: 2.6x on the meter's RMS
// loop.
//
// Numerical contract: this is the same recurrence, not an approximation of it,
// evaluated in a different association order. A single block matches the
// per-sample form to ~1e-6 relative (the rounding of one `a^4` per group); over
// thousands of blocks the difference stays bounded at ~1e-5 relative because the
// recursion is stable, and 1e-5 relative is 0.0001 dB - below the resolution of
// any meter we display. tests/dsp/BlockStateTests.cpp pins both bounds.
//
// These are deliberately free functions on raw pointers: no state, no
// allocation, no branches on signal values, so they are safe on the audio thread
// and cannot be called from a wrong thread.

/// Advances `s[n] = a*s[n-1] + b*x[n]^2` over `frames` samples of `data`,
/// four at a time, and returns the new state. `a` must be in [0, 1) and `b` is
/// the per-sample weight (for the meter's exponential RMS, `a = 1 - c`).
[[nodiscard]] inline float advanceSquareRecursion(const float* data, int frames, float state,
                                                  float a, float b) noexcept {
    if (data == nullptr || frames <= 0)
        return state;
    const float a2 = a * a;
    const float a3 = a2 * a;
    const float a4 = a3 * a;
    const float w0 = b * a3;
    const float w1 = b * a2;
    const float w2 = b * a;
    int i = 0;
    for (; i + 4 <= frames; i += 4) {
        const float x0 = data[i] * data[i];
        const float x1 = data[i + 1] * data[i + 1];
        const float x2 = data[i + 2] * data[i + 2];
        const float x3 = data[i + 3] * data[i + 3];
        state = a4 * state + (w0 * x0 + w1 * x1) + (w2 * x2 + b * x3);
    }
    for (; i < frames; ++i)
        state = a * state + b * (data[i] * data[i]);
    return state;
}

/// Applies a one-pole ramp towards `target` to `data` in place, four samples at
/// a time: `data[i] *= g(i)` where `g(n) = target + (start - target) * a^n`.
/// This is exactly what `g += (target - g) * c` does per sample (with `a = 1 - c`),
/// evaluated with the closed form so the multiplies are independent.
/// Returns the final gain, ready to be stored back into the caller's state.
[[nodiscard]] inline float applyOnePoleRamp(float* data, int frames, float start, float target,
                                            float a) noexcept {
    if (data == nullptr || frames <= 0)
        return start;
    const float a2 = a * a;
    const float a3 = a2 * a;
    const float a4 = a3 * a;
    // The residual snaps to zero once it is within a few ULP of the target.
    // Without the snap the ramp never *reaches* the target: the state is a float,
    // so as soon as the remaining difference is under half an ULP of the target
    // the state parks on the neighbouring representable value - and because each
    // call recomputes the difference from that rounded state, it parks there
    // forever (the per-sample form has exactly the same behaviour, one ULP short).
    // Parking is not merely untidy: the difference then keeps decaying inside the
    // closed form, drifts through the denormal range, and every multiply by a
    // denormal takes an x86 microcode assist - the same loop measured 18 ns/frame
    // instead of 0.5. Snapping at 8 ULP (a gain error of 1e-6, i.e. -120 dB) fixes
    // both: the ramp lands exactly on the target, so the caller's `current ==
    // target` test is true from then on and the fader costs one plain multiply.
    const float magnitude = target < 0.0f ? -target : target;
    const float snap =
        8.0f * std::numeric_limits<float>::epsilon() * (magnitude > 1.0f ? magnitude : 1.0f);
    float difference = start - target;
    int i = 0;
    for (; i + 4 <= frames; i += 4) {
        // g1..g4 are independent: each is `difference` scaled by a fixed power.
        data[i] *= target + difference * a;
        data[i + 1] *= target + difference * a2;
        data[i + 2] *= target + difference * a3;
        data[i + 3] *= target + difference * a4;
        difference *= a4;
        if (difference < snap && difference > -snap)
            difference = 0.0f;
    }
    for (; i < frames; ++i) {
        difference *= a;
        data[i] *= target + difference;
    }
    if (difference < snap && difference > -snap)
        difference = 0.0f;
    return target + difference;
}

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
