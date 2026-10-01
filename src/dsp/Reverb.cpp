// ============================================================================
// AURA DAW - src/dsp/Reverb.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "AURA Space" FDN reverb implementation. See Reverb.hpp for the algorithm
// description. Signal flow per sample:
//
//   in -> [pre-delay] -> highpass -> [4x input allpass diffusion]
//        -> inject into 4 delay lines
//   lines -> tap -> damping LP -> lineOut
//   lineOut -> Hadamard(4x4) -> line input (with per-line decay gain)
//   lineOut -> stereo decode -> [2x output allpass] -> tone EQ -> wet mix
// ============================================================================
#include "aura/dsp/Reverb.hpp"

#include <algorithm>
#include <cmath>

namespace aura::dsp {
namespace {

/// Base delay lengths in samples at 48 kHz. Mutually prime so the FDN modes do
/// not align; scaled proportionally for other sample rates and room sizes.
constexpr float kBaseLineLengths[4] = {1553.0f, 1617.0f, 1499.0f, 1427.0f};
/// Input diffusion all-pass lengths (samples @48k) and coefficients.
constexpr float kInputDiffusionLengths[4] = {142.0f, 107.0f, 379.0f, 277.0f};
constexpr float kInputDiffusionGains[4] = {0.75f, 0.72f, 0.70f, 0.68f};
constexpr float kOutputDiffusionLengths[2] = {225.0f, 341.0f};
constexpr float kOutputDiffusionGains[2] = {0.55f, 0.50f};

/// Orthonormal 4x4 Hadamard mixing (scaled by 0.5 to stay lossless).
void hadamardMix(const float* input, float* output) noexcept {
    const float a = input[0] + input[1];
    const float b = input[0] - input[1];
    const float c = input[2] + input[3];
    const float d = input[2] - input[3];
    output[0] = (a + c) * 0.5f;
    output[1] = (b + d) * 0.5f;
    output[2] = (a - c) * 0.5f;
    output[3] = (b - d) * 0.5f;
}

} // namespace

void Reverb::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;

    rebuildDelays();

    const std::size_t preDelayCapacity =
        static_cast<std::size_t>(std::ceil(sampleRate_ * 0.2)) + 4;
    preDelayBuffer_.assign(preDelayCapacity, 0.0f);
    preDelayWrite_ = 0;
    preDelaySamples_ = std::max(
        1, static_cast<int>(sampleRate_ * static_cast<double>(settings_.preDelayMs) * 0.001));

    inputHighPass_.setHighPass(sampleRate_, std::max(20.0f, settings_.lowCutHz * 0.5f));
    highCut_.setCoefficients(BiquadDesign::lowPass(sampleRate_, settings_.highCutHz, 0.707));
    lowCut_.setCoefficients(BiquadDesign::highPass(sampleRate_, settings_.lowCutHz, 0.707));

    mixSmoother_.reset(sampleRate, settings_.mix, 0.05f);
    decaySmoother_.reset(sampleRate, settings_.decaySeconds, 0.15f);

    for (int line = 0; line < kNumLines; ++line) {
        const auto index = static_cast<std::size_t>(line);
        lfoPhase_[index] = static_cast<float>(line) * 0.23f;
        lfoIncrement_[index] = static_cast<float>(math::kTwoPi * (0.31 + 0.17 * line) / sampleRate_);
    }
    tailEnergy_ = 0.0f;
    tailTimer_ = 0.0f;
    reset();
}

void Reverb::rebuildDelays() {
    const double scale = sampleRate_ / 48000.0;
    // Room size scales the network length 0.5x (small) .. 2.0x (hall).
    const float sizeScale = 0.5f + settings_.size * 1.5f;

    for (int line = 0; line < kNumLines; ++line) {
        const auto index = static_cast<std::size_t>(line);
        const std::size_t length = static_cast<std::size_t>(
            std::ceil(kBaseLineLengths[index] * scale * static_cast<double>(sizeScale))) + 4;
        lines_[index].buffer.assign(length, 0.0f);
        lines_[index].writeIndex = 0;
        lines_[index].dampingState = 0.0f;
        lines_[index].delaySamples = static_cast<float>(length - 4);
    }
    for (int diffusion = 0; diffusion < 4; ++diffusion) {
        const auto index = static_cast<std::size_t>(diffusion);
        const std::size_t length = static_cast<std::size_t>(
            std::ceil(kInputDiffusionLengths[index] * scale)) + 4;
        inputDiffusion_[index].buffer.assign(length, 0.0f);
        inputDiffusion_[index].writeIndex = 0;
        inputDiffusion_[index].feedback = kInputDiffusionGains[index];
    }
    for (int diffusion = 0; diffusion < 2; ++diffusion) {
        const auto index = static_cast<std::size_t>(diffusion);
        const std::size_t length = static_cast<std::size_t>(
            std::ceil(kOutputDiffusionLengths[index] * scale)) + 4;
        outputDiffusion_[index].buffer.assign(length, 0.0f);
        outputDiffusion_[index].writeIndex = 0;
        outputDiffusion_[index].feedback = kOutputDiffusionGains[index];
    }
}

void Reverb::reset() noexcept {
    for (auto& line : lines_) {
        std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
        line.writeIndex = 0;
        line.dampingState = 0.0f;
    }
    for (auto& ap : inputDiffusion_) {
        std::fill(ap.buffer.begin(), ap.buffer.end(), 0.0f);
        ap.writeIndex = 0;
    }
    for (auto& ap : outputDiffusion_) {
        std::fill(ap.buffer.begin(), ap.buffer.end(), 0.0f);
        ap.writeIndex = 0;
    }
    std::fill(preDelayBuffer_.begin(), preDelayBuffer_.end(), 0.0f);
    preDelayWrite_ = 0;
    inputHighPass_.reset();
    highCut_.reset();
    lowCut_.reset();
    tailEnergy_ = 0.0f;
    tailTimer_ = 0.0f;
}

void Reverb::setSettings(const ReverbSettings& settings) noexcept {
    const bool sizeChanged = std::abs(settings.size - settings_.size) > 0.001f;
    const bool preDelayChanged = std::abs(settings.preDelayMs - settings_.preDelayMs) > 0.01f;
    settings_ = settings;
    // Contract (see header): setSettings runs on a control thread, never the
    // audio thread, so rebuilding delay storage here is safe.
    if (sizeChanged)
        rebuildDelays();
    if (preDelayChanged) {
        preDelaySamples_ = std::max(
            1, static_cast<int>(sampleRate_ * static_cast<double>(settings_.preDelayMs) * 0.001));
        if (!preDelayBuffer_.empty() &&
            static_cast<std::size_t>(preDelaySamples_) + 4 >= preDelayBuffer_.size()) {
            preDelaySamples_ = static_cast<int>(preDelayBuffer_.size()) - 5;
        }
    }
    mixSmoother_.setTarget(math::clamp01(settings_.mix));
    decaySmoother_.setTarget(settings_.decaySeconds);
    inputHighPass_.setHighPass(sampleRate_, std::max(20.0f, settings_.lowCutHz * 0.5f));
    highCut_.setCoefficients(BiquadDesign::lowPass(sampleRate_, settings_.highCutHz, 0.707));
    lowCut_.setCoefficients(BiquadDesign::highPass(sampleRate_, settings_.lowCutHz, 0.707));
}

bool Reverb::isActive() const noexcept {
    return !settings_.bypassed && (tailTimer_ > 0.0f || settings_.freeze);
}

bool Reverb::hasTail() const noexcept {
    return !settings_.bypassed && (tailTimer_ > 0.0f || settings_.freeze);
}

float Reverb::readDelayed(const std::vector<float>& buffer, std::size_t writeIndex,
                          float delaySamples) const noexcept {
    const auto length = static_cast<float>(buffer.size());
    float readPosition = static_cast<float>(writeIndex) - delaySamples;
    while (readPosition < 0.0f)
        readPosition += length;
    while (readPosition >= length)
        readPosition -= length;
    const auto index = static_cast<std::size_t>(readPosition);
    const float fraction = readPosition - static_cast<float>(index);
    const std::size_t next = (index + 1) % buffer.size();
    return buffer[index] + fraction * (buffer[next] - buffer[index]);
}

void Reverb::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || settings_.bypassed)
        return;

    const float mix = mixSmoother_.current();
    const float decaySeconds = decaySmoother_.current();
    const float damping = math::clamp(settings_.damping, 0.0f, 1.0f);
    const float modulation = math::clamp01(settings_.modulation);
    const bool freeze = settings_.freeze;
    const float dampingCoefficient = 1.0f - damping * 0.85f;

    // Per-line feedback from the target RT60: g = 10^(-3 * delay / (RT60 * fs)).
    std::array<float, kNumLines> feedbackGain{};
    for (int line = 0; line < kNumLines; ++line) {
        const float delaySeconds =
            lines_[static_cast<std::size_t>(line)].delaySamples / static_cast<float>(sampleRate_);
        const float rt60 = freeze ? 1.0e9f : std::max(0.05f, decaySeconds);
        feedbackGain[static_cast<std::size_t>(line)] = math::clamp(
            std::pow(10.0f, -3.0f * delaySeconds / rt60), 0.0f, 0.99999f);
    }

    const bool stereo = block.numChannels >= 2;
    float* left = block.channelPointers[0];
    float* right = stereo ? block.channelPointers[1] : nullptr;
    float blockEnergy = 0.0f;
    const std::size_t preDelayLength = preDelayBuffer_.size();

    for (int i = 0; i < block.numFrames; ++i) {
        const float inputLeft = left[i];
        const float inputRight = right ? right[i] : inputLeft;
        const float inputMono = (inputLeft + inputRight) * 0.5f;

        // --- pre-delay ------------------------------------------------------
        const std::size_t preDelayRead =
            (preDelayWrite_ + preDelayLength - static_cast<std::size_t>(preDelaySamples_)) %
            preDelayLength;
        const float preDelayed = preDelayBuffer_[preDelayRead];
        preDelayBuffer_[preDelayWrite_] = inputMono;
        preDelayWrite_ = (preDelayWrite_ + 1) % preDelayLength;

        // --- input conditioning + diffusion ---------------------------------
        float diffused = inputHighPass_.processSample(preDelayed);
        for (auto& ap : inputDiffusion_) {
            const float delayed = ap.buffer[ap.writeIndex];
            const float output = delayed - diffused;
            ap.buffer[ap.writeIndex] = math::flushDenormal(diffused + delayed * ap.feedback);
            ap.writeIndex = (ap.writeIndex + 1) % ap.buffer.size();
            diffused = output;
        }

        // --- FDN: tap the lines --------------------------------------------
        std::array<float, kNumLines> lineOut{};
        for (int line = 0; line < kNumLines; ++line) {
            const auto index = static_cast<std::size_t>(line);
            CombLine& comb = lines_[index];

            lfoPhase_[index] += lfoIncrement_[index];
            if (lfoPhase_[index] > math::kTwoPiF)
                lfoPhase_[index] -= math::kTwoPiF;
            const float modSamples = std::sin(lfoPhase_[index]) * modulation * 3.0f;

            float tap = readDelayed(comb.buffer, comb.writeIndex, comb.delaySamples + modSamples);
            comb.dampingState += dampingCoefficient * (tap - comb.dampingState);
            comb.dampingState = math::flushDenormal(comb.dampingState);
            lineOut[index] = comb.dampingState;
            blockEnergy += std::abs(comb.dampingState);
        }

        // --- FDN: mix and re-inject -----------------------------------------
        std::array<float, kNumLines> mixed{};
        hadamardMix(lineOut.data(), mixed.data());
        for (int line = 0; line < kNumLines; ++line) {
            const auto index = static_cast<std::size_t>(line);
            CombLine& comb = lines_[index];
            // Asymmetric injection (line 0 louder) breaks the symmetry of the
            // Hadamard matrix, which otherwise gives an unnaturally even tail.
            const float injection = (line == 0) ? diffused * 0.6f : diffused * 0.25f;
            comb.buffer[comb.writeIndex] =
                math::flushDenormal(injection + mixed[index] * feedbackGain[index]);
            comb.writeIndex = (comb.writeIndex + 1) % comb.buffer.size();
        }

        // --- stereo decode + output diffusion -------------------------------
        float wetLeft = lineOut[0] * 0.6f + lineOut[2] * 0.4f;
        float wetRight = lineOut[1] * 0.6f + lineOut[3] * 0.4f;
        for (auto& ap : outputDiffusion_) {
            const float delayedL = ap.buffer[ap.writeIndex];
            const float delayedR = ap.buffer[(ap.writeIndex + 1) % ap.buffer.size()];
            ap.buffer[ap.writeIndex] = math::flushDenormal(
                wetLeft + delayedL * ap.feedback + wetRight * 0.0f);
            ap.buffer[(ap.writeIndex + 1) % ap.buffer.size()] =
                math::flushDenormal(wetRight + delayedR * ap.feedback);
            wetLeft = delayedL - wetLeft * 0.4f;
            wetRight = delayedR - wetRight * 0.4f;
            ap.writeIndex = (ap.writeIndex + 2) % ap.buffer.size();
        }

        // Tone shaping on the wet signal only.
        wetLeft = lowCut_.processSample(0, highCut_.processSample(0, wetLeft)) * 0.55f;
        wetRight = lowCut_.processSample(1, highCut_.processSample(1, wetRight)) * 0.55f;

        left[i] = inputLeft * (1.0f - mix) + wetLeft * mix;
        if (right)
            right[i] = inputRight * (1.0f - mix) + wetRight * mix;
    }

    mixSmoother_.advance(block.numFrames);
    decaySmoother_.advance(block.numFrames);

    tailEnergy_ = blockEnergy / static_cast<float>(std::max(1, block.numFrames));
    if (blockEnergy > 1.0e-5f * static_cast<float>(block.numFrames))
        tailTimer_ = 2.0f;
    else if (tailTimer_ > 0.0f)
        tailTimer_ -= static_cast<float>(block.numFrames) / static_cast<float>(sampleRate_) * 20.0f;
}

} // namespace aura::dsp
