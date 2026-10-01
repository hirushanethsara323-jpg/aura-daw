// ============================================================================
// AURA DAW - src/dsp/Compressor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Compressor.hpp"

#include <cmath>

namespace aura::dsp {

void Compressor::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    coefficientsDirty_ = true;
    makeupSmoother_.reset(sampleRate, 1.0f, 0.02f);
    reset();
}

void Compressor::reset() noexcept {
    envelope_ = 0.0f;
    gainReductionDb_.store(0.0f, std::memory_order_relaxed);
    makeupSmoother_.snapTo(makeupGain_);
}

void Compressor::setSettings(const CompressorSettings& settings) noexcept {
    settings_ = settings;
    coefficientsDirty_ = true;
}

void Compressor::setThresholdDb(float db) noexcept {
    settings_.thresholdDb = math::clamp(db, -60.0f, 0.0f);
}
void Compressor::setRatio(float ratio) noexcept {
    settings_.ratio = math::clamp(ratio, 1.0f, 40.0f);
}
void Compressor::setAttackMs(float ms) noexcept {
    settings_.attackMs = math::clamp(ms, 0.05f, 500.0f);
    coefficientsDirty_ = true;
}
void Compressor::setReleaseMs(float ms) noexcept {
    settings_.releaseMs = math::clamp(ms, 5.0f, 5000.0f);
    coefficientsDirty_ = true;
}
void Compressor::setKneeDb(float db) noexcept {
    settings_.kneeDb = math::clamp(db, 0.0f, 24.0f);
}
void Compressor::setMakeupDb(float db) noexcept {
    settings_.makeupDb = math::clamp(db, -12.0f, 36.0f);
    makeupGain_ = math::dbToGain(settings_.makeupDb);
    makeupSmoother_.setTarget(makeupGain_);
}
void Compressor::setAutoMakeup(bool enabled) noexcept {
    settings_.autoMakeup = enabled;
}

void Compressor::updateCoefficients() noexcept {
    // Time constants: 1 - exp(-1 / (time * fs)) is the standard one-pole form.
    // Attack is the *detector rising* behaviour, so it uses the attack time.
    attackCoefficient_ = math::onePoleCoefficient(settings_.attackMs, sampleRate_);
    releaseCoefficient_ = math::onePoleCoefficient(settings_.releaseMs, sampleRate_);

    // Auto-makeup: compensate for the static gain lost at 0 dBFS input. This is
    // an approximation for a soft-knee detector, chosen because it is
    // predictable and does not "pump" when the input level changes.
    if (settings_.autoMakeup) {
        const float over = std::max(0.0f, -settings_.thresholdDb);
        const float reductionDb = over - over / settings_.ratio;
        makeupGain_ = math::dbToGain(reductionDb * 0.6f);
    } else {
        makeupGain_ = math::dbToGain(settings_.makeupDb);
    }
    makeupSmoother_.setTarget(makeupGain_);
    coefficientsDirty_ = false;
}

float Compressor::computeGainDb(float inputLevelDb) const noexcept {
    const float threshold = settings_.thresholdDb;
    const float ratio = settings_.ratio;
    const float knee = settings_.kneeDb;

    if (knee > 0.01f) {
        const float kneeStart = threshold - knee * 0.5f;
        const float kneeEnd = threshold + knee * 0.5f;
        if (inputLevelDb <= kneeStart)
            return inputLevelDb; // below threshold: unity
        if (inputLevelDb >= kneeEnd) {
            const float over = inputLevelDb - threshold;
            return threshold + over / ratio;
        }
        // Quadratic knee interpolation (standard soft-knee curve).
        const float x = inputLevelDb - kneeStart;
        const float slope = 1.0f / ratio - 1.0f;
        return inputLevelDb + slope * x * x / (2.0f * knee);
    }

    if (inputLevelDb <= threshold)
        return inputLevelDb;
    const float over = inputLevelDb - threshold;
    return threshold + over / ratio;
}

void Compressor::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || settings_.bypassed)
        return;
    if (coefficientsDirty_)
        updateCoefficients();

    const bool hasSidechain = !sidechain_.isEmpty() &&
                              sidechain_.numFrames >= block.numFrames &&
                              sidechain_.numChannels > 0;

    const float threshold = settings_.thresholdDb;
    const float ratio = settings_.ratio;
    const float kneeStart = threshold - settings_.kneeDb * 0.5f;
    const float kneeEnd = threshold + settings_.kneeDb * 0.5f;
    const float knee = settings_.kneeDb;
    const float slope = 1.0f / ratio - 1.0f;

    float maximumReduction = 0.0f;

    for (int i = 0; i < block.numFrames; ++i) {
        // --- detector ------------------------------------------------------
        float detector = 0.0f;
        if (hasSidechain) {
            for (int channel = 0; channel < sidechain_.numChannels; ++channel)
                detector = std::max(detector, std::abs(sidechain_.channelPointers[channel][i]));
        } else {
            for (int channel = 0; channel < block.numChannels; ++channel)
                detector = std::max(detector, std::abs(block.channelPointers[channel][i]));
        }

        // Peak detector with attack/release ballistics on the linear magnitude.
        const float coefficient = detector > envelope_ ? attackCoefficient_ : releaseCoefficient_;
        envelope_ += coefficient * (detector - envelope_);
        envelope_ = math::flushDenormal(envelope_);

        const float levelDb = envelope_ > 1.0e-9f ? math::gainToDb(envelope_) : -120.0f;

        // --- static curve --------------------------------------------------
        float outputDb;
        if (levelDb <= kneeStart) {
            outputDb = levelDb;
        } else if (levelDb >= kneeEnd || knee <= 0.01f) {
            outputDb = threshold + (levelDb - threshold) / ratio;
        } else {
            const float x = levelDb - kneeStart;
            outputDb = levelDb + slope * x * x / (2.0f * knee);
        }

        const float reductionDb = outputDb - levelDb; // <= 0
        maximumReduction = std::max(maximumReduction, -reductionDb);

        // Convert the *reduction* to a linear gain. Unity when nothing happens,
        // which keeps the common case cheap (a multiply by 1.0f).
        const float gain = reductionDb <= -0.0001f ? math::dbToGain(reductionDb) : 1.0f;
        const float makeup = makeupSmoother_.next();

        for (int channel = 0; channel < block.numChannels; ++channel)
            block.channelPointers[channel][i] *= gain * makeup;
    }

    gainReductionDb_.store(maximumReduction, std::memory_order_relaxed);
}

} // namespace aura::dsp
