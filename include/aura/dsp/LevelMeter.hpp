// ============================================================================
// AURA DAW - include/aura/dsp/LevelMeter.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel metering: sample peak, RMS, and a clip indicator with hold.
// The audio thread writes through atomics; the UI reads snapshots. No locking,
// no allocation, and the values are "decaying maxima" so the UI can run at any
// frame rate without missing transients.
// ============================================================================
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "aura/core/Math.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

/// Snapshot read by the UI (all values are linear amplitudes).
struct MeterSnapshot {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float rmsLeft = 0.0f;
    float rmsRight = 0.0f;
    bool clipLeft = false;
    bool clipRight = false;
    float peakDbLeft() const noexcept { return math::gainToDb(peakLeft); }
    float peakDbRight() const noexcept { return math::gainToDb(peakRight); }
    float rmsDbLeft() const noexcept { return math::gainToDb(rmsLeft); }
    float rmsDbRight() const noexcept { return math::gainToDb(rmsRight); }
};

class LevelMeter {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        // RMS window: 300 ms is the broadcast-standard integration time.
        rmsCoefficient_ = 1.0f - std::exp(-1.0f / (0.3f * static_cast<float>(sampleRate_)));
        reset();
    }

    void reset() noexcept {
        for (int channel = 0; channel < 2; ++channel) {
            peak_[channel].store(0.0f, std::memory_order_relaxed);
            rms_[channel].store(0.0f, std::memory_order_relaxed);
            clip_[channel].store(false, std::memory_order_relaxed);
            rmsState_[channel] = 0.0f;
        }
        framesSincePeak_ = 0;
    }

    /// Peak-hold decay: the UI sees a transient for ~1.5 s before it falls.
    void setPeakHoldSeconds(float seconds) noexcept {
        holdFrames_ = static_cast<std::uint32_t>(sampleRate_ * seconds);
    }

    void process(const AudioBlockView& block) noexcept {
        if (block.isEmpty())
            return;
        const int channels = block.numChannels < 2 ? block.numChannels : 2;
        const float blockLength = static_cast<float>(block.numFrames);

        for (int channel = 0; channel < channels; ++channel) {
            const float* data = block.channelPointers[channel];
            float peak = 0.0f;
            bool clipped = false;
            // The RMS is an exponential average of the *samples*: the one-pole has
            // to advance once per frame, not once per block. Updating it per block
            // would make the reading depend on the block size (a 512-frame block
            // would integrate ~10x faster than a 4800-frame one) and, with the
            // per-sample coefficient, leave the meter stuck near zero.
            for (int i = 0; i < block.numFrames; ++i) {
                const float value = data[i];
                const float magnitude = std::abs(value);
                if (magnitude > peak)
                    peak = magnitude;
                const float square = value * value;
                rmsState_[channel] += rmsCoefficient_ * (square - rmsState_[channel]);
                if (magnitude >= 1.0f)
                    clipped = true;
            }
            (void)blockLength;
            rmsState_[channel] = math::flushDenormal(rmsState_[channel]);

            // Peak hold with decay.
            const float previousPeak = peak_[channel].load(std::memory_order_relaxed);
            const float newPeak = peak > previousPeak ? peak : previousPeak;
            peak_[channel].store(newPeak, std::memory_order_relaxed);
            rms_[channel].store(std::sqrt(rmsState_[channel]), std::memory_order_relaxed);
            if (clipped)
                clip_[channel].store(true, std::memory_order_relaxed);
        }

        framesSincePeak_ += static_cast<std::uint32_t>(block.numFrames);
        if (framesSincePeak_ >= holdFrames_) {
            // Decay peaks by 20 dB/s (broadcast-style ballistic).
            const float decay = static_cast<float>(holdFrames_) / static_cast<float>(sampleRate_) * 0.1f;
            for (int channel = 0; channel < 2; ++channel) {
                const float current = peak_[channel].load(std::memory_order_relaxed);
                peak_[channel].store(std::max(0.0f, current - decay), std::memory_order_relaxed);
            }
            framesSincePeak_ = 0;
        }
    }

    [[nodiscard]] MeterSnapshot snapshot() const noexcept {
        MeterSnapshot snapshot;
        snapshot.peakLeft = peak_[0].load(std::memory_order_relaxed);
        snapshot.peakRight = peak_[1].load(std::memory_order_relaxed);
        snapshot.rmsLeft = rms_[0].load(std::memory_order_relaxed);
        snapshot.rmsRight = rms_[1].load(std::memory_order_relaxed);
        snapshot.clipLeft = clip_[0].load(std::memory_order_relaxed);
        snapshot.clipRight = clip_[1].load(std::memory_order_relaxed);
        return snapshot;
    }

    /// Clears the clip indicator (UI "click to reset" behaviour).
    void clearClip() noexcept {
        clip_[0].store(false, std::memory_order_relaxed);
        clip_[1].store(false, std::memory_order_relaxed);
    }

private:
    double sampleRate_ = 48000.0;
    std::atomic<float> peak_[2]{};
    std::atomic<float> rms_[2]{};
    std::atomic<bool> clip_[2]{};
    float rmsState_[2]{};
    float rmsCoefficient_ = 0.0001f;
    std::uint32_t framesSincePeak_ = 0;
    std::uint32_t holdFrames_ = 48000;
};

} // namespace aura::dsp
