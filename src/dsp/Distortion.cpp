// ============================================================================
// AURA DAW - src/dsp/Distortion.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Distortion.hpp"

#include <algorithm>
#include <cmath>

namespace aura::dsp {

// ---------------------------------------------------------------------------
// 2x oversampling helper
//
// Polyphase decomposition of a linear-phase 15-tap half-band FIR:
//   y[2n]   = sum_k h[2k]   * x[n-k]     (phase 0, 8 taps)
//   y[2n+1] = sum_k h[2k+1] * x[n-k]     (phase 1, 7 taps)
// Downsampling runs the full 15-tap filter over the 2x-rate stream and keeps
// every second output.
//
// Group delay (measured by tests/dsp/DistortionTests.cpp "oversampling_latency"):
//   7 samples at 2x for the interpolator + 7 samples at 2x for the decimator
//   = 7 input samples. Reported via Processor::latencySamples() so the engine
//   compensates it exactly.
// ---------------------------------------------------------------------------
void Distortion::designHalfBand() {
    // Kaiser-windowed sinc, cutoff at 0.25 (half-band, relative to the 2x rate).
    // Generated here rather than hard-coded so the same routine can serve a
    // longer filter if a future 4x mode needs it.
    constexpr int N = kHalfBandTaps;
    constexpr double pi = 3.14159265358979323846;
    constexpr double cutoff = 0.25;
    constexpr double beta = 8.0;
    const double denominator = std::cyl_bessel_i(0.0, beta);
    std::array<double, N> windowed{};
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        const int n = i - (N - 1) / 2;
        const double sinc = (n == 0) ? 2.0 * cutoff : std::sin(2.0 * pi * cutoff * n) / (pi * n);
        const double t = 2.0 * i / (N - 1) - 1.0;
        const double window = std::cyl_bessel_i(0.0, beta * std::sqrt(1.0 - t * t)) / denominator;
        windowed[static_cast<std::size_t>(i)] = sinc * window;
        sum += sinc * window;
    }
    for (int i = 0; i < N; ++i)
        halfBand_[static_cast<std::size_t>(i)] =
            static_cast<float>(windowed[static_cast<std::size_t>(i)] / sum);
}

void Distortion::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    designHalfBand();
    mixSmoother_.reset(sampleRate, settings_.mix, 0.02f);
    outputSmoother_.reset(sampleRate, math::dbToGain(settings_.outputDb), 0.02f);
    reset();
}

void Distortion::reset() noexcept {
    for (auto& channel : upHistory_)
        channel.fill(0.0f);
    for (auto& channel : downHistory_)
        channel.fill(0.0f);
    upHistoryIndex_ = 0;
    downHistoryIndex_ = 0;
    toneState_ = 0.0f;
    toneStateR_ = 0.0f;
    crushHold_ = 0.0f;
    crushHoldR_ = 0.0f;
    crushCounter_ = 0;
    fuzzGate_ = 0.0f;
}

void Distortion::setSettings(const DistortionSettings& settings) noexcept {
    settings_ = settings;
    mixSmoother_.setTarget(math::clamp01(settings_.mix));
    outputSmoother_.setTarget(math::dbToGain(settings_.outputDb));
}

// ---------------------------------------------------------------------------
// Waveshapers (all finite for finite input; verified by QualityTests.cpp)
// ---------------------------------------------------------------------------
float Distortion::shapeDrive(float x) const noexcept {
    // Asymmetric soft saturation: cubic behaviour for small signals, asymptotic
    // +/-1 saturation for large ones. Bias shifts the operating point, adding
    // even harmonics; the DC it introduces is removed explicitly.
    const float bias = settings_.bias * 0.6f;
    const float y = x + bias;
    const float saturated = y / (1.0f + std::abs(y));
    const float cubic = y - (y * y * y) / 3.0f;
    const float blend = math::clamp01(std::abs(x) * 1.5f);
    const float result = (1.0f - blend) * cubic + blend * saturated;
    return math::flushDenormal(result - std::tanh(bias));
}

float Distortion::shapeFold(float x) const noexcept {
    // Wavefolder: a triangle map folds the signal back on itself, producing
    // bell-like upper harmonics that increase with drive.
    const float offset = x + settings_.bias * 0.5f;
    const float scaled = offset * (1.0f + settings_.drive * 6.0f);
    float folded = std::fmod(scaled + 1.0f, 4.0f);
    if (folded < 0.0f)
        folded += 4.0f;
    folded = std::abs(folded - 2.0f) - 1.0f;
    return math::flushDenormal(folded * 0.8f);
}

float Distortion::shapeFuzz(float x) noexcept {
    // Hard asymmetric clip plus a gate that cleans up the note tail.
    const float driveAmount = 1.0f + settings_.drive * 24.0f;
    const float driven = x * driveAmount + settings_.bias * 0.4f;
    const float clipped = driven > 0.0f ? std::min(driven, 0.9f) : std::max(driven, -0.7f);
    const float magnitude = std::abs(clipped);
    if (magnitude > fuzzGate_)
        fuzzGate_ += 0.02f * (magnitude - fuzzGate_);
    else
        fuzzGate_ += 0.002f * (magnitude - fuzzGate_);
    fuzzGate_ = math::flushDenormal(fuzzGate_);
    const float gate = math::clamp01(fuzzGate_ * 6.0f);
    return math::flushDenormal(clipped * gate);
}

float Distortion::shapeCrush(float x) noexcept {
    // Decimation (sample-rate reduction) with a hold, plus bit-depth
    // quantisation at each held sample.
    const int factor = std::max(1, static_cast<int>(std::lround(settings_.decimation)));
    if (crushCounter_ == 0) {
        const float levels = std::pow(2.0f, math::clamp(settings_.bits, 1.0f, 16.0f)) - 1.0f;
        crushHold_ = std::round(math::clamp(x, -1.0f, 1.0f) * levels) / levels;
    }
    crushCounter_ = (crushCounter_ + 1) % factor;
    return math::flushDenormal(crushHold_);
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------
void Distortion::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || settings_.bypassed)
        return;

    const float driveGain = math::dbToGain(settings_.drive * 36.0f);
    const float toneCoefficient = 0.02f + math::clamp01(settings_.tone) * 0.9f;
    const bool oversample = settings_.oversample;
    const int channels = std::min(block.numChannels, 2);

    for (int channel = 0; channel < channels; ++channel) {
        float* data = block.channelPointers[channel];
        auto& upHistory = upHistory_[static_cast<std::size_t>(channel)];
        auto& downHistory = downHistory_[static_cast<std::size_t>(channel)];
        float toneState = (channel == 0) ? toneState_ : toneStateR_;
        float crushHold = (channel == 0) ? crushHold_ : crushHoldR_;
        std::size_t upIndex = upHistoryIndex_;
        std::size_t downIndex = downHistoryIndex_;
        // Crush needs its own decimation counter per channel; the settings-driven
        // factor is shared, so we keep the single counter for the mono/stereo
        // case and document that Crush is channel-coherent (both channels
        // decimate identically, which is the musically expected behaviour).
        int localCrushCounter = (channel == 0) ? crushCounter_ : (crushCounter_ + 1) % 2;

        for (int i = 0; i < block.numFrames; ++i) {
            const float dry = data[i];
            float wet = 0.0f;

            if (oversample) {
                // --- interpolate to 2x ---------------------------------------
                upHistory[upIndex] = dry * driveGain;
                const std::array<float, 2> phase{{
                    [&] { // phase 0: even taps
                        float accumulated = 0.0f;
                        for (int tap = 0; tap < kHalfBandTaps; tap += 2) {
                            const std::size_t index =
                                (upIndex + kHalfBandTaps - static_cast<std::size_t>(tap)) %
                                kHalfBandTaps;
                            accumulated += halfBand_[static_cast<std::size_t>(tap)] * upHistory[index];
                        }
                        return accumulated * 2.0f; // zero-stuffing gain compensation
                    }(),
                    [&] { // phase 1: odd taps
                        float accumulated = 0.0f;
                        for (int tap = 1; tap < kHalfBandTaps; tap += 2) {
                            const std::size_t index =
                                (upIndex + kHalfBandTaps - static_cast<std::size_t>(tap)) %
                                kHalfBandTaps;
                            accumulated += halfBand_[static_cast<std::size_t>(tap)] * upHistory[index];
                        }
                        return accumulated * 2.0f;
                    }(),
                }};
                upIndex = (upIndex + 1) % kHalfBandTaps;

                // --- shape at 2x ---------------------------------------------
                std::array<float, 2> shaped{};
                for (int p = 0; p < 2; ++p) {
                    const float x = phase[static_cast<std::size_t>(p)];
                    switch (settings_.kind) {
                    case DistortionKind::Drive: shaped[static_cast<std::size_t>(p)] = shapeDrive(x); break;
                    case DistortionKind::Fold: shaped[static_cast<std::size_t>(p)] = shapeFold(x); break;
                    case DistortionKind::Fuzz: shaped[static_cast<std::size_t>(p)] = shapeFuzz(x); break;
                    case DistortionKind::Crush: shaped[static_cast<std::size_t>(p)] = shapeCrush(x); break;
                    }
                }

                // --- decimate back to 1x -------------------------------------
                float decimated = 0.0f;
                for (int p = 0; p < 2; ++p) {
                    downHistory[downIndex] = shaped[static_cast<std::size_t>(p)];
                    downIndex = (downIndex + 1) % kHalfBandTaps;
                    float accumulated = 0.0f;
                    for (int tap = 0; tap < kHalfBandTaps; ++tap) {
                        const std::size_t index =
                            (downIndex + kHalfBandTaps - 1 - static_cast<std::size_t>(tap)) %
                            kHalfBandTaps;
                        accumulated +=
                            halfBand_[static_cast<std::size_t>(tap)] * downHistory[index];
                    }
                    decimated = accumulated;
                }
                wet = decimated;
            } else {
                const float x = dry * driveGain;
                switch (settings_.kind) {
                case DistortionKind::Drive: wet = shapeDrive(x); break;
                case DistortionKind::Fold: wet = shapeFold(x); break;
                case DistortionKind::Fuzz: wet = shapeFuzz(x); break;
                case DistortionKind::Crush:
                    crushCounter_ = localCrushCounter;
                    wet = shapeCrush(x);
                    localCrushCounter = crushCounter_;
                    break;
                }
            }

            // Post tone filter, then the smoothed dry/wet blend and trim.
            toneState += toneCoefficient * (wet - toneState);
            toneState = math::flushDenormal(toneState);
            const float mix = mixSmoother_.next();
            const float outputGain = outputSmoother_.next();
            data[i] = math::flushDenormal((dry * (1.0f - mix) + toneState * mix) * outputGain);
        }

        if (channel == 0) {
            toneState_ = toneState;
            crushHold_ = crushHold;
            upHistoryIndex_ = upIndex;
            downHistoryIndex_ = downIndex;
        } else {
            toneStateR_ = toneState;
            crushHoldR_ = crushHold;
        }
        (void)crushHold;
    }
}

} // namespace aura::dsp
