// ============================================================================
// AURA DAW - src/dsp/Delay.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Delay.hpp"

#include <algorithm>
#include <cmath>

namespace aura::dsp {

void Delay::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;

    capacity_ = static_cast<std::size_t>(std::ceil(sampleRate_ * kMaxDelayMs * 0.001)) + 4;
    bufferLeft_.assign(capacity_, 0.0f);
    bufferRight_.assign(capacity_, 0.0f);
    writeIndex_ = 0;

    // 30 ms glide keeps tempo changes musical instead of tape-warping.
    delaySmootherLeft_.reset(sampleRate, sampleRate_ * 0.375, 0.03f);
    delaySmootherRight_.reset(sampleRate, sampleRate_ * 0.375, 0.03f);
    feedbackSmoother_.reset(sampleRate, settings_.feedback, 0.02f);
    mixSmoother_.reset(sampleRate, settings_.mix, 0.02f);

    dampingStateLeft_ = 0.0f;
    dampingStateRight_ = 0.0f;
    tailActive_ = false;
}

void Delay::reset() noexcept {
    snapSmoothers_ = true;
    std::fill(bufferLeft_.begin(), bufferLeft_.end(), 0.0f);
    std::fill(bufferRight_.begin(), bufferRight_.end(), 0.0f);
    writeIndex_ = 0;
    dampingStateLeft_ = 0.0f;
    dampingStateRight_ = 0.0f;
    tailActive_ = false;
}

void Delay::setSettings(const DelaySettings& settings) noexcept {
    settings_ = settings;
    snapSmoothers_ = true;
    feedbackSmoother_.setTarget(math::clamp(settings_.feedback, 0.0f, 0.95f));
    mixSmoother_.setTarget(math::clamp01(settings_.mix));
}

float Delay::delaySamplesFor(bool left, const ProcessContext& context) const noexcept {
    double seconds;
    if (settings_.syncToTempo) {
        const double beats = static_cast<double>(left ? settings_.syncLeftBeats : settings_.syncRightBeats);
        seconds = beats * (60.0 / std::max(1.0, context.tempoBpm));
    } else {
        seconds = static_cast<double>(left ? settings_.timeLeftMs : settings_.timeRightMs) * 0.001;
    }
    const double maxSeconds = static_cast<double>(kMaxDelayMs) * 0.001;
    seconds = math::clamp(seconds, 0.001, maxSeconds);
    // Keep at least 2 samples of delay so interpolation stays valid.
    return std::max(2.0f, static_cast<float>(seconds * sampleRate_));
}

void Delay::process(AudioBlockView& block, const ProcessContext& context) noexcept {
    if (block.isEmpty() || settings_.bypassed)
        return;

    const float targetLeft = delaySamplesFor(true, context);
    const float targetRight = delaySamplesFor(false, context);
    if (snapSmoothers_) {
        // First block after prepare()/setSettings(): take the configured values
        // immediately - ramping towards them would swallow the first echoes.
        delaySmootherLeft_.snapTo(targetLeft);
        delaySmootherRight_.snapTo(targetRight);
        feedbackSmoother_.snapTo(math::clamp(settings_.feedback, 0.0f, 0.95f));
        mixSmoother_.snapTo(math::clamp01(settings_.mix));
        snapSmoothers_ = false;
    } else {
        delaySmootherLeft_.setTarget(targetLeft);
        delaySmootherRight_.setTarget(targetRight);
    }

    // Damping coefficient: one-pole low-pass inside the feedback loop.
    const float damping = math::clamp(settings_.damping, 0.0f, 0.99f);
    const float dampingCoefficient = 1.0f - damping * 0.95f;

    const bool stereo = block.numChannels >= 2;
    float* left = block.channelPointers[0];
    float* right = stereo ? block.channelPointers[1] : nullptr;

    float tailEnergy = 0.0f;

    for (int i = 0; i < block.numFrames; ++i) {
        const float delayLeft = delaySmootherLeft_.next();
        const float delayRight = delaySmootherRight_.next();
        const float feedback = feedbackSmoother_.next();
        const float mix = mixSmoother_.next();
        const float crossFeed = math::clamp01(settings_.crossFeed);

        // Linear-interpolated taps.
        const auto readTap = [&](const std::vector<float>& buffer, float delay) {
            const float readPosition = static_cast<float>(writeIndex_) - delay;
            const float wrapped =
                readPosition < 0.0f ? readPosition + static_cast<float>(capacity_) : readPosition;
            const auto index = static_cast<std::size_t>(wrapped);
            const float fraction = wrapped - static_cast<float>(index);
            const std::size_t next = (index + 1) % capacity_;
            return buffer[index] + fraction * (buffer[next] - buffer[index]);
        };

        const float delayedLeft = readTap(bufferLeft_, delayLeft);
        const float delayedRight = readTap(bufferRight_, delayRight);

        // Damping filters inside the feedback path.
        dampingStateLeft_ += dampingCoefficient * (delayedLeft - dampingStateLeft_);
        dampingStateRight_ += dampingCoefficient * (delayedRight - dampingStateRight_);
        dampingStateLeft_ = math::flushDenormal(dampingStateLeft_);
        dampingStateRight_ = math::flushDenormal(dampingStateRight_);

        const float inputLeft = left[i];
        const float inputRight = right ? right[i] : inputLeft;

        // Cross-feed produces the ping-pong behaviour at crossFeed == 1.
        const float feedbackLeft =
            (1.0f - crossFeed) * dampingStateLeft_ + crossFeed * dampingStateRight_;
        const float feedbackRight =
            (1.0f - crossFeed) * dampingStateRight_ + crossFeed * dampingStateLeft_;

        bufferLeft_[writeIndex_] = math::flushDenormal(inputLeft + feedbackLeft * feedback);
        bufferRight_[writeIndex_] = math::flushDenormal(inputRight + feedbackRight * feedback);

        left[i] = inputLeft * (1.0f - mix) + delayedLeft * mix;
        if (right)
            right[i] = inputRight * (1.0f - mix) + delayedRight * mix;
        else
            left[i] += 0.0f;

        tailEnergy += std::abs(dampingStateLeft_) + std::abs(dampingStateRight_);
        writeIndex_ = (writeIndex_ + 1) % capacity_;
    }

    // Below -80 dBFS integrated over the block: consider the tail finished.
    tailActive_ = tailEnergy > 1.0e-4f;
}

} // namespace aura::dsp
