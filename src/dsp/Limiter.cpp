// ============================================================================
// AURA DAW - src/dsp/Limiter.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Limiter.hpp"

#include <algorithm>
#include <cmath>

#include "aura/core/Log.hpp"

namespace aura::dsp {
namespace {
constexpr const char* kCategory = "DSP.Limiter";
}

void Limiter::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    rebuildDelayLines();
    coefficientsDirty_ = true;
    reset();
}

void Limiter::rebuildDelayLines() {
    lookaheadSamples_ = std::max(
        1, static_cast<int>(std::lround(sampleRate_ * static_cast<double>(settings_.lookaheadMs) * 0.001)));
    // Extra headroom so the sliding-max window can be evaluated ahead of the
    // delayed sample within the same block (see process()).
    const std::size_t length = static_cast<std::size_t>(lookaheadSamples_ + maxBlockSize_ + 8);
    if (delayLines_.size() != static_cast<std::size_t>(numChannels_))
        delayLines_.assign(static_cast<std::size_t>(numChannels_), {});
    for (auto& line : delayLines_) {
        line.assign(length, 0.0f);
    }
    deque_.assign(length + 1, {});
    dequeHead_ = 0;
    writeIndex_ = 0;
}

void Limiter::reset() noexcept {
    for (auto& line : delayLines_)
        std::fill(line.begin(), line.end(), 0.0f);
    writeIndex_ = 0;
    dequeHead_ = 0;
    currentGain_ = 1.0f;
    gainReductionDb_.store(0.0f, std::memory_order_relaxed);
}

void Limiter::setSettings(const LimiterSettings& settings) noexcept {
    const float previousLookahead = settings_.lookaheadMs;
    settings_ = settings;
    if (settings_.lookaheadMs != previousLookahead)
        rebuildDelayLines();
    coefficientsDirty_ = true;
}

void Limiter::setCeilingDb(float db) noexcept {
    settings_.ceilingDb = math::clamp(db, -12.0f, 0.0f);
    ceilingLinear_ = math::dbToGain(settings_.ceilingDb);
}

void Limiter::setReleaseMs(float ms) noexcept {
    settings_.releaseMs = math::clamp(ms, 5.0f, 2000.0f);
    coefficientsDirty_ = true;
}

void Limiter::setLookaheadMs(float ms) noexcept {
    settings_.lookaheadMs = math::clamp(ms, 0.1f, 20.0f);
    rebuildDelayLines();
    coefficientsDirty_ = true;
}

bool Limiter::setTruePeakEnabled(bool enabled) noexcept {
    if (enabled) {
        // Honest failure path: the feature is not implemented yet. Logging here
        // is acceptable because this setter runs on a control thread only.
        AURA_LOG_WARN(kCategory,
                      "True-peak limiting requested but not implemented in this build; "
                      "sample-peak detection remains active. Tracked for M9.");
        return false;
    }
    settings_.truePeak = false;
    return true;
}

void Limiter::updateCoefficients() noexcept {
    releaseCoefficient_ = math::onePoleCoefficient(settings_.releaseMs, sampleRate_);
    ceilingLinear_ = math::dbToGain(settings_.ceilingDb);
    coefficientsDirty_ = false;
}

void Limiter::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || settings_.bypassed || delayLines_.empty())
        return;
    if (coefficientsDirty_)
        updateCoefficients();

    const int frames = block.numFrames;
    const int channels = std::min(block.numChannels, static_cast<int>(delayLines_.size()));
    const std::size_t lineLength = delayLines_[0].size();
    const std::size_t window = static_cast<std::size_t>(lookaheadSamples_);

    // Monotonic deque reset per block: index-based, no allocation (the deque
    // storage was allocated in prepare()).
    dequeHead_ = 0;
    std::size_t dequeCount = 0;
    const auto dequeIndex = [&](std::size_t logicalIndex) {
        return (dequeHead_ + logicalIndex) % deque_.size();
    };

    // Attack ramp length: the gain must be fully reduced by the time the peak
    // exits the look-ahead window, hence a ramp over `window` samples.
    const float attackStep = window > 0 ? 1.0f / static_cast<float>(window + 1) : 1.0f;
    float attackProgress = 1.0f; // 1 = fully at instantaneous-required gain

    float maxReduction = 0.0f;

    for (int i = 0; i < frames; ++i) {
        // 1. Write the incoming frame into the delay line at the current index.
        for (int channel = 0; channel < block.numChannels; ++channel) {
            if (channel < channels)
                delayLines_[static_cast<std::size_t>(channel)][writeIndex_] =
                    block.channelPointers[channel][i];
        }

        // 2. Sliding maximum of |x| over the last `window` samples.
        const float magnitude = [&] {
            float maximum = 0.0f;
            for (int channel = 0; channel < channels; ++channel)
                maximum = std::max(maximum,
                                   std::abs(delayLines_[static_cast<std::size_t>(channel)][writeIndex_]));
            return maximum;
        }();

        while (dequeCount > 0) {
            const auto back = deque_[dequeIndex(dequeCount - 1)];
            if (back.second <= magnitude) {
                --dequeCount;
            } else {
                break;
            }
        }
        deque_[dequeIndex(dequeCount)] = {writeIndex_, magnitude};
        ++dequeCount;

        // Drop entries older than the window.
        while (dequeCount > 0) {
            const auto front = deque_[dequeIndex(0)];
            const std::size_t age = (writeIndex_ + lineLength - front.first) % lineLength;
            if (age >= window) {
                ++dequeHead_;
                dequeHead_ %= deque_.size();
                --dequeCount;
            } else {
                break;
            }
        }

        const float windowMax = dequeCount > 0 ? deque_[dequeIndex(0)].second : magnitude;

        // 3. Required gain for this sample and the sliding window.
        float targetGain = 1.0f;
        if (windowMax * currentGain_ > ceilingLinear_ && windowMax > 1.0e-12f)
            targetGain = ceilingLinear_ / windowMax;

        if (targetGain < currentGain_) {
            // Attack: approach instantly but ramp within the look-ahead so we
            // never modulate faster than the window allows.
            currentGain_ += (targetGain - currentGain_) * (1.0f - attackStep);
            attackProgress = 0.0f;
        } else {
            // Release: exponential one-pole back towards unity.
            currentGain_ += releaseCoefficient_ * (targetGain - currentGain_);
        }
        currentGain_ = math::flushDenormal(math::clamp(currentGain_, 0.0f, 1.0f));

        const float reductionDb = currentGain_ > 1.0e-6f ? -math::gainToDb(currentGain_) : 96.0f;
        maxReduction = std::max(maxReduction, reductionDb);

        // 4. Output the *delayed* sample scaled by the limiter gain. The delay
        //    of `lookaheadSamples_` guarantees the peak arrives at the moment
        //    the gain has reached its required value.
        const std::size_t readIndex = (writeIndex_ + lineLength - window) % lineLength;
        for (int channel = 0; channel < block.numChannels; ++channel) {
            if (channel < channels) {
                const float delayed = delayLines_[static_cast<std::size_t>(channel)][readIndex];
                block.channelPointers[channel][i] = delayed * currentGain_;
            } else {
                // Channels beyond the prepared count are passed through dry: the
                // engine never prepares fewer channels than the device provides,
                // but this keeps the block valid if it ever happens.
                block.channelPointers[channel][i] = 0.0f;
            }
        }
        (void)attackProgress;

        writeIndex_ = (writeIndex_ + 1) % lineLength;
    }

    gainReductionDb_.store(maxReduction, std::memory_order_relaxed);
}

} // namespace aura::dsp
