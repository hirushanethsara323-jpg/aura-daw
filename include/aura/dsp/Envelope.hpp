// ============================================================================
// AURA DAW - include/aura/dsp/Envelope.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADSR (for instruments/gain shaping) and an AR envelope follower (for
// detection tasks such as gating and level display).
// ============================================================================
#pragma once

#include <cstdint>

#include "aura/core/Math.hpp"

namespace aura::dsp {

enum class AdsrStage : std::int32_t { Idle = 0, Attack, Decay, Sustain, Release };

struct AdsrSettings {
    float attackMs = 5.0f;
    float decayMs = 120.0f;
    float sustainLevel = 0.8f; ///< 0 .. 1
    float releaseMs = 200.0f;
    /// 0 = linear, 1 = exponential (single-pole) segments.
    float curve = 0.7f;
};

class AdsrEnvelope {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void setSettings(const AdsrSettings& settings) noexcept {
        settings_ = settings;
        updateCoefficients();
    }
    [[nodiscard]] const AdsrSettings& settings() const noexcept { return settings_; }

    void noteOn() noexcept {
        stage_ = AdsrStage::Attack;
        if (useLinear())
            linearStep_ = 1.0f / std::max(1.0f, attackSamples_);
    }

    void noteOff() noexcept {
        if (stage_ != AdsrStage::Idle)
            stage_ = AdsrStage::Release;
    }

    void reset() noexcept {
        stage_ = AdsrStage::Idle;
        value_ = 0.0f;
        updateCoefficients();
    }

    [[nodiscard]] bool isActive() const noexcept { return stage_ != AdsrStage::Idle; }
    [[nodiscard]] AdsrStage stage() const noexcept { return stage_; }
    [[nodiscard]] float value() const noexcept { return value_; }

    [[nodiscard]] float nextSample() noexcept {
        switch (stage_) {
        case AdsrStage::Idle:
            value_ = 0.0f;
            break;
        case AdsrStage::Attack:
            if (useLinear()) {
                value_ += linearStep_;
                if (value_ >= 1.0f) {
                    value_ = 1.0f;
                    stage_ = AdsrStage::Decay;
                }
            } else {
                value_ += attackCoefficient_ * (1.0f - value_);
                if (value_ >= 0.999f) {
                    value_ = 1.0f;
                    stage_ = AdsrStage::Decay;
                }
            }
            break;
        case AdsrStage::Decay:
            if (useLinear()) {
                value_ -= decayStep_;
                if (value_ <= settings_.sustainLevel) {
                    value_ = settings_.sustainLevel;
                    stage_ = AdsrStage::Sustain;
                }
            } else {
                value_ += decayCoefficient_ * (settings_.sustainLevel - value_);
                if (std::abs(value_ - settings_.sustainLevel) < 0.001f) {
                    value_ = settings_.sustainLevel;
                    stage_ = AdsrStage::Sustain;
                }
            }
            break;
        case AdsrStage::Sustain:
            value_ = settings_.sustainLevel;
            break;
        case AdsrStage::Release:
            if (useLinear()) {
                value_ -= releaseStep_;
                if (value_ <= 0.0f) {
                    value_ = 0.0f;
                    stage_ = AdsrStage::Idle;
                }
            } else {
                value_ += releaseCoefficient_ * (0.0f - value_);
                if (value_ <= 0.0001f) {
                    value_ = 0.0f;
                    stage_ = AdsrStage::Idle;
                }
            }
            break;
        }
        value_ = math::flushDenormal(math::clamp01(value_));
        return value_;
    }

    void processGain(float* buffer, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i)
            buffer[i] *= nextSample();
    }

private:
    [[nodiscard]] bool useLinear() const noexcept { return settings_.curve < 0.5f; }

    void updateCoefficients() noexcept {
        attackSamples_ = static_cast<float>(sampleRate_ * settings_.attackMs * 0.001);
        const float decaySamples = static_cast<float>(sampleRate_ * settings_.decayMs * 0.001);
        const float releaseSamples = static_cast<float>(sampleRate_ * settings_.releaseMs * 0.001);
        attackCoefficient_ = math::onePoleCoefficient(settings_.attackMs, sampleRate_);
        decayCoefficient_ = math::onePoleCoefficient(settings_.decayMs, sampleRate_);
        releaseCoefficient_ = math::onePoleCoefficient(settings_.releaseMs, sampleRate_);
        decayStep_ = (1.0f - settings_.sustainLevel) / std::max(1.0f, decaySamples);
        releaseStep_ = settings_.sustainLevel > 0.0f
                           ? settings_.sustainLevel / std::max(1.0f, releaseSamples)
                           : 1.0f / std::max(1.0f, releaseSamples);
    }

    double sampleRate_ = 48000.0;
    AdsrSettings settings_{};
    AdsrStage stage_ = AdsrStage::Idle;
    float value_ = 0.0f;
    float attackSamples_ = 240.0f;
    float attackCoefficient_ = 0.01f;
    float decayCoefficient_ = 0.01f;
    float releaseCoefficient_ = 0.01f;
    float linearStep_ = 0.01f;
    float decayStep_ = 0.001f;
    float releaseStep_ = 0.001f;
};

/// Envelope follower with independent attack/release for detection paths.
class EnvelopeFollower {
public:
    void prepare(double sampleRate, float attackMs = 5.0f, float releaseMs = 80.0f) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        attackCoefficient_ = math::onePoleCoefficient(attackMs, sampleRate_);
        releaseCoefficient_ = math::onePoleCoefficient(releaseMs, sampleRate_);
    }
    void reset() noexcept { envelope_ = 0.0f; }
    [[nodiscard]] float processSample(float input) noexcept {
        const float magnitude = std::abs(input);
        const float coefficient =
            magnitude > envelope_ ? attackCoefficient_ : releaseCoefficient_;
        envelope_ = math::flushDenormal(envelope_ + coefficient * (magnitude - envelope_));
        return envelope_;
    }
    [[nodiscard]] float value() const noexcept { return envelope_; }

private:
    double sampleRate_ = 48000.0;
    float attackCoefficient_ = 0.05f;
    float releaseCoefficient_ = 0.001f;
    float envelope_ = 0.0f;
};

} // namespace aura::dsp
