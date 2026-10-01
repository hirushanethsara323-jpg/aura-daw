// ============================================================================
// AURA DAW - dsp/ParameterSmoothing.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every user-facing parameter that can be dragged, automated or modulated must
// be smoothed, otherwise zipper noise is guaranteed. These smoothers are
// allocation-free, denormal-safe and deterministic.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aura/core/Math.hpp"

namespace aura::dsp {

/// Linear ramp across a fixed number of samples (block-accurate).
/// Use when the target must land exactly at the next block boundary.
class LinearSmoothedValue {
public:
    void reset(double sampleRate, float initialValue, float rampSeconds = 0.02f) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setRampTime(rampSeconds);
        current_ = initialValue;
        target_ = initialValue;
        step_ = 0.0f;
        counter_ = 0;
    }

    void setRampTime(float seconds) noexcept {
        const auto samples = static_cast<int>(std::max(1.0, sampleRate_ * static_cast<double>(seconds)));
        rampSamples_ = std::max(1, samples);
    }

    void setTarget(float value) noexcept {
        if (value == target_)
            return;
        target_ = value;
        step_ = (target_ - current_) / static_cast<float>(rampSamples_);
        counter_ = rampSamples_;
    }

    /// Snap immediately (used on load, on reset, and for "no smoothing" params).
    void snapTo(float value) noexcept {
        current_ = value;
        target_ = value;
        step_ = 0.0f;
        counter_ = 0;
    }

    [[nodiscard]] bool isSmoothing() const noexcept { return counter_ > 0; }
    [[nodiscard]] float current() const noexcept { return current_; }
    [[nodiscard]] float target() const noexcept { return target_; }

    /// Advance one sample and return the value to use.
    [[nodiscard]] float next() noexcept {
        if (counter_ > 0) {
            current_ += step_;
            if (--counter_ == 0)
                current_ = target_;
        }
        return current_;
    }

    /// Advance the smoother by `numSamples` (block-rate parameter updates).
    void advance(int numSamples) noexcept {
        for (int i = 0; i < numSamples; ++i)
            (void)next();
    }

    /// Value at the end of this block (for feeding block-based DSP math).
    [[nodiscard]] float valueAfter(int numSamples) noexcept {
        if (counter_ <= 0)
            return current_;
        const int steps = std::min(numSamples, counter_);
        return current_ + step_ * static_cast<float>(steps);
    }

    /// Apply the ramp to a buffer in place.
    void applyGain(float* buffer, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i)
            buffer[i] *= next();
    }

    /// Fill a ramp buffer (useful for modulation of effects with wet/dry mixes).
    void fillRamp(float* destination, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i)
            destination[i] = next();
    }

private:
    double sampleRate_ = 48000.0;
    int rampSamples_ = 960;
    int counter_ = 0;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
};

/// One-pole exponential smoother - cheap, always-continuous, ideal for meters
/// and for parameters where an exact arrival time does not matter.
class OnePoleSmoother {
public:
    void reset(double sampleRate, float initialValue, float timeConstantSeconds = 0.01f) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setTimeConstant(timeConstantSeconds);
        value_ = initialValue;
        target_ = initialValue;
    }

    void setTimeConstant(float seconds) noexcept {
        coefficient_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate_) /
                                       std::max(seconds, 1.0e-6f));
    }

    void setTarget(float value) noexcept { target_ = value; }
    void snapTo(float value) noexcept { value_ = value; target_ = value; }
    [[nodiscard]] float current() const noexcept { return value_; }
    [[nodiscard]] float target() const noexcept { return target_; }

    [[nodiscard]] float next() noexcept {
        value_ += coefficient_ * (target_ - value_);
        return value_;
    }

    /// Advance `n` samples at once (block-rate parameter update).
    [[nodiscard]] float advance(int n) noexcept {
        for (int i = 0; i < n; ++i)
            (void)next();
        return value_;
    }

    [[nodiscard]] bool settled(float tolerance = 1.0e-5f) const noexcept {
        return std::abs(target_ - value_) < tolerance;
    }

private:
    double sampleRate_ = 48000.0;
    float coefficient_ = 0.01f;
    float value_ = 0.0f;
    float target_ = 0.0f;
};

/// Slew limiter - limits how fast a value may change; used for gain ramps that
/// protect against instantaneous jumps from automation or preset changes.
class SlewLimiter {
public:
    void reset(double sampleRate, float initialValue, float maxUnitsPerSecond) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setMaxRate(maxUnitsPerSecond);
        value_ = initialValue;
    }
    void setMaxRate(float unitsPerSecond) noexcept {
        step_ = std::max(1.0e-9f, unitsPerSecond / static_cast<float>(sampleRate_));
    }
    void setTarget(float value) noexcept { target_ = value; }
    [[nodiscard]] float next() noexcept {
        const float delta = target_ - value_;
        if (delta > step_)
            value_ += step_;
        else if (delta < -step_)
            value_ -= step_;
        else
            value_ = target_;
        return value_;
    }
    [[nodiscard]] float current() const noexcept { return value_; }

private:
    double sampleRate_ = 48000.0;
    float step_ = 0.01f;
    float value_ = 0.0f;
    float target_ = 0.0f;
};

/// Stereo-linked smoothing helper: guarantees identical gain on both channels
/// (a panned source must not drift when its fader is moved).
class LinkedSmoother {
public:
    void reset(double sampleRate, float initialValue, float rampSeconds = 0.02f) noexcept {
        left_.reset(sampleRate, initialValue, rampSeconds);
        right_.reset(sampleRate, initialValue, rampSeconds);
    }
    void setTarget(float value) noexcept {
        left_.setTarget(value);
        right_.setTarget(value);
    }
    [[nodiscard]] float next() noexcept {
        (void)left_.next();
        return right_.next();
    }

private:
    LinearSmoothedValue left_;
    LinearSmoothedValue right_;
};

} // namespace aura::dsp
