// ============================================================================
// AURA DAW - src/dsp/Loudness.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Loudness.hpp"

#include <algorithm>
#include <cmath>

namespace aura::dsp {
namespace {

/// BS.1770-4 reference coefficients for 48 kHz.
constexpr double kShelf48kB[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
constexpr double kShelf48kA[3] = {1.0, -1.69065929318241, 0.73248077421585};
constexpr double kRlb48kB[3] = {1.0, -2.0, 1.0};
constexpr double kRlb48kA[3] = {1.0, -1.99004745483398, 0.99007225036621};

constexpr double kPi = 3.14159265358979323846;
constexpr double kLoudnessOffset = -0.691; ///< BS.1770-4 constant
constexpr float kAbsoluteGateLufs = -70.0f;
constexpr float kRelativeGateOffset = -10.0f;

void designKWeighting(double sampleRate, BiquadCoefficients& shelf, BiquadCoefficients& highPass) {
    if (std::abs(sampleRate - 48000.0) < 0.5) {
        shelf.b0 = static_cast<float>(kShelf48kB[0]);
        shelf.b1 = static_cast<float>(kShelf48kB[1]);
        shelf.b2 = static_cast<float>(kShelf48kB[2]);
        shelf.a1 = static_cast<float>(kShelf48kA[1]);
        shelf.a2 = static_cast<float>(kShelf48kA[2]);
        highPass.b0 = static_cast<float>(kRlb48kB[0]);
        highPass.b1 = static_cast<float>(kRlb48kB[1]);
        highPass.b2 = static_cast<float>(kRlb48kB[2]);
        highPass.a1 = static_cast<float>(kRlb48kA[1]);
        highPass.a2 = static_cast<float>(kRlb48kA[2]);
        return;
    }

    // Stage 1: high shelf +4 dB above ~1681 Hz (BS.1770 stage 1 values).
    const double w0Shelf = 2.0 * kPi * 1681.974450955533 / sampleRate;
    const double K = std::tan(w0Shelf * 0.5);
    const double Vh = std::pow(10.0, 4.0 / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double Q = 0.7071752369554196;
    const double a0 = 1.0 + K / Q + K * K;
    shelf.b0 = static_cast<float>((Vh + Vb * K / Q + K * K) / a0);
    shelf.b1 = static_cast<float>(2.0 * (K * K - Vh) / a0);
    shelf.b2 = static_cast<float>((Vh - Vb * K / Q + K * K) / a0);
    shelf.a1 = static_cast<float>(2.0 * (K * K - 1.0) / a0);
    shelf.a2 = static_cast<float>((1.0 - K / Q + K * K) / a0);

    // Stage 2: RLB high-pass at 38.13 Hz.
    const double w0High = 2.0 * kPi * 38.13547087602444 / sampleRate;
    const double cosHigh = std::cos(w0High);
    const double sinHigh = std::sin(w0High);
    const double QHigh = 0.5003270373238773;
    const double alpha = sinHigh / (2.0 * QHigh);
    const double a0High = 1.0 + alpha;
    highPass.b0 = static_cast<float>((1.0 + cosHigh) * 0.5 / a0High);
    highPass.b1 = static_cast<float>(-(1.0 + cosHigh) / a0High);
    highPass.b2 = highPass.b0;
    highPass.a1 = static_cast<float>(-2.0 * cosHigh / a0High);
    highPass.a2 = static_cast<float>((1.0 - alpha) / a0High);
}

/// K-weighted mean square (summed across channels) -> LUFS.
[[nodiscard]] double meanSquareToLufs(double meanSquare) noexcept {
    return kLoudnessOffset + 10.0 * std::log10(std::max(1.0e-15, meanSquare));
}

} // namespace

void LoudnessMeter::prepare(double sampleRate, int numChannels) {
    sampleRate_ = sampleRate;
    numChannels_ = numChannels;
    BiquadCoefficients shelf;
    BiquadCoefficients highPass;
    designKWeighting(sampleRate, shelf, highPass);
    for (int channel = 0; channel < 2; ++channel) {
        preFilter_[static_cast<std::size_t>(channel)].setCoefficients(shelf);
        rlbFilter_[static_cast<std::size_t>(channel)].setCoefficients(highPass);
    }
    samplesPerStep_ = std::max(1, static_cast<int>(sampleRate * 0.1)); // 100 ms
    stepMeanSquare_.assign(static_cast<std::size_t>(kMaxSteps), 0.0f);
    gatingBlocks_.assign(static_cast<std::size_t>(kMaxSteps), -100.0f);
    scratchGated_.assign(static_cast<std::size_t>(kMaxSteps), 0.0f);
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (auto& filter : preFilter_)
        filter.reset();
    for (auto& filter : rlbFilter_)
        filter.reset();
    stepAccumulator_ = 0.0;
    stepSamples_ = 0;
    stepWrite_.store(0, std::memory_order_relaxed);
    stepsRecorded_.store(0, std::memory_order_relaxed);
    gatingWrite_.store(0, std::memory_order_relaxed);
    gatingRecorded_.store(0, std::memory_order_relaxed);
    momentary_.store(-100.0f, std::memory_order_relaxed);
    shortTerm_.store(-100.0f, std::memory_order_relaxed);
    maxMomentary_.store(-100.0f, std::memory_order_relaxed);
    maxShortTerm_.store(-100.0f, std::memory_order_relaxed);
    std::fill(stepMeanSquare_.begin(), stepMeanSquare_.end(), 0.0f);
    std::fill(gatingBlocks_.begin(), gatingBlocks_.end(), -100.0f);
}

void LoudnessMeter::pushGatingBlock(float lufs) noexcept {
    const std::size_t index = gatingWrite_.load(std::memory_order_relaxed);
    gatingBlocks_[index] = lufs;
    gatingWrite_.store((index + 1) % static_cast<std::size_t>(kMaxSteps), std::memory_order_relaxed);
    const int recorded = gatingRecorded_.load(std::memory_order_relaxed);
    if (recorded < kMaxSteps)
        gatingRecorded_.store(recorded + 1, std::memory_order_relaxed);
}

void LoudnessMeter::feedStep(float meanSquare) noexcept {
    const std::size_t index = stepWrite_.load(std::memory_order_relaxed);
    stepMeanSquare_[index] = meanSquare;
    stepWrite_.store((index + 1) % static_cast<std::size_t>(kMaxSteps), std::memory_order_relaxed);
    const int recorded = stepsRecorded_.load(std::memory_order_relaxed);
    if (recorded < kMaxSteps)
        stepsRecorded_.store(recorded + 1, std::memory_order_relaxed);

    const int count = std::min(recorded + 1, kMaxSteps);
    const auto windowMeanSquare = [&](int steps) {
        const int window = std::min(steps, count);
        if (window <= 0)
            return 0.0f;
        double sum = 0.0;
        const std::size_t write =
            (index + 1) % static_cast<std::size_t>(kMaxSteps); // newest is at index
        for (int i = 0; i < window; ++i) {
            const std::size_t read =
                (write + kMaxSteps - 1 - static_cast<std::size_t>(i)) % kMaxSteps;
            sum += stepMeanSquare_[read];
        }
        return static_cast<float>(sum / static_cast<double>(window));
    };

    // Momentary = 400 ms (4 steps); short-term = 3 s (30 steps).
    const float momentary = static_cast<float>(meanSquareToLufs(windowMeanSquare(4)));
    momentary_.store(momentary, std::memory_order_relaxed);
    if (momentary > maxMomentary_.load(std::memory_order_relaxed))
        maxMomentary_.store(momentary, std::memory_order_relaxed);

    const float shortTerm = static_cast<float>(meanSquareToLufs(windowMeanSquare(30)));
    shortTerm_.store(shortTerm, std::memory_order_relaxed);
    if (shortTerm > maxShortTerm_.load(std::memory_order_relaxed))
        maxShortTerm_.store(shortTerm, std::memory_order_relaxed);

    // Gating blocks start once a full 400 ms window exists.
    if (count >= 4)
        pushGatingBlock(momentary);
}

void LoudnessMeter::process(const AudioBlockView& block) noexcept {
    if (block.isEmpty())
        return;
    const int channels = std::min(block.numChannels, 2);

    for (int i = 0; i < block.numFrames; ++i) {
        double stepSum = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            float sample = block.channelPointers[channel][i];
            sample = preFilter_[static_cast<std::size_t>(channel)].processSample(channel, sample);
            sample = rlbFilter_[static_cast<std::size_t>(channel)].processSample(channel, sample);
            stepSum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        stepAccumulator_ += stepSum;
        if (++stepSamples_ >= samplesPerStep_) {
            feedStep(static_cast<float>(stepAccumulator_ / static_cast<double>(stepSamples_)));
            stepAccumulator_ = 0.0;
            stepSamples_ = 0;
        }
    }
}

LoudnessSnapshot LoudnessMeter::snapshot() const {
    LoudnessSnapshot out;
    out.momentaryLufs = momentary_.load(std::memory_order_relaxed);
    out.shortTermLufs = shortTerm_.load(std::memory_order_relaxed);
    out.maxMomentaryLufs = maxMomentary_.load(std::memory_order_relaxed);
    out.maxShortTermLufs = maxShortTerm_.load(std::memory_order_relaxed);

    const int recorded = std::min(gatingRecorded_.load(std::memory_order_relaxed), kMaxSteps);
    if (recorded < 4) {
        out.integratedValid = false;
        return out;
    }

    // Copy the gating ring (control thread, no allocation: scratch is reserved).
    const std::size_t write = gatingWrite_.load(std::memory_order_relaxed);
    for (int i = 0; i < recorded; ++i) {
        const std::size_t read = (write + kMaxSteps - 1 - static_cast<std::size_t>(i)) % kMaxSteps;
        scratchGated_[static_cast<std::size_t>(i)] = gatingBlocks_[read];
    }

    // Pass 1: absolute gate at -70 LUFS.
    double absoluteSum = 0.0;
    int absoluteCount = 0;
    for (int i = 0; i < recorded; ++i) {
        const float value = scratchGated_[static_cast<std::size_t>(i)];
        if (value <= kAbsoluteGateLufs)
            continue;
        absoluteSum += std::pow(10.0, static_cast<double>(value) / 10.0);
        ++absoluteCount;
    }
    if (absoluteCount == 0) {
        out.integratedValid = false;
        return out;
    }

    // Pass 2: relative gate 10 LU below the absolute-gated mean.
    const double relativeGate =
        meanSquareToLufs(absoluteSum / static_cast<double>(absoluteCount)) + kRelativeGateOffset;

    double gatedSum = 0.0;
    int gatedCount = 0;
    for (int i = 0; i < recorded; ++i) {
        const float value = scratchGated_[static_cast<std::size_t>(i)];
        if (value <= kAbsoluteGateLufs || static_cast<double>(value) < relativeGate)
            continue;
        gatedSum += std::pow(10.0, static_cast<double>(value) / 10.0);
        scratchGated_[static_cast<std::size_t>(gatedCount)] = value;
        ++gatedCount;
    }
    if (gatedCount == 0) {
        out.integratedValid = false;
        return out;
    }

    out.integratedLufs =
        static_cast<float>(meanSquareToLufs(gatedSum / static_cast<double>(gatedCount)));
    out.integratedValid = true;

    // Loudness range: 95th - 10th percentile of gated values (EBU Tech 3342).
    std::sort(scratchGated_.begin(), scratchGated_.begin() + gatedCount);
    const auto percentile = [&](double p) {
        const double index = p * static_cast<double>(gatedCount - 1);
        const auto low = static_cast<std::size_t>(std::floor(index));
        const auto high = static_cast<std::size_t>(std::ceil(index));
        const double fraction = index - static_cast<double>(low);
        return scratchGated_[low] * static_cast<float>(1.0 - fraction) +
               scratchGated_[high] * static_cast<float>(fraction);
    };
    out.rangeLu = percentile(0.95) - percentile(0.10);
    return out;
}

} // namespace aura::dsp
