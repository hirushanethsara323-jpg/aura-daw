// ============================================================================
// AURA DAW - src/dsp/Equalizer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/dsp/Equalizer.hpp"

#include <cmath>

namespace aura::dsp {
namespace {

/// Threshold (in dB or Hz) beyond which coefficients are recomputed. Larger
/// values reduce CPU on automation-heavy sessions; smaller values track fast
/// sweeps more exactly. 0.5 % of frequency / 0.02 dB is inaudible.
constexpr double kFrequencyTolerance = 0.005;
constexpr double kGainToleranceDb = 0.02;

} // namespace

void Equalizer::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    for (int band = 0; band < kEqBandCount; ++band) {
        frequencySmoothers_[static_cast<std::size_t>(band)].reset(
            sampleRate, static_cast<float>(settings_.bands[static_cast<std::size_t>(band)].frequency),
            0.01f);
        gainSmoothers_[static_cast<std::size_t>(band)].reset(
            sampleRate, static_cast<float>(settings_.bands[static_cast<std::size_t>(band)].gainDb),
            0.01f);
    }
    needsRebuild_ = true;
    rebuildCoefficients();
}

void Equalizer::reset() noexcept {
    for (auto& filter : filters_)
        filter.reset();
}

void Equalizer::setSettings(const EqSettings& settings) noexcept {
    settings_ = settings;
    for (int band = 0; band < kEqBandCount; ++band) {
        const auto index = static_cast<std::size_t>(band);
        frequencySmoothers_[index].setTarget(static_cast<float>(settings_.bands[index].frequency));
        gainSmoothers_[index].setTarget(static_cast<float>(settings_.bands[index].gainDb));
    }
    needsRebuild_ = true;
}

void Equalizer::setBandEnabled(EqBand band, bool enabled) noexcept {
    if (static_cast<int>(band) < 0 || static_cast<int>(band) >= kEqBandCount)
        return;
    settings_.bands[static_cast<std::size_t>(band)].enabled = enabled;
    needsRebuild_ = true;
}

void Equalizer::setBandFrequency(EqBand band, double hz) noexcept {
    if (static_cast<int>(band) < 0 || static_cast<int>(band) >= kEqBandCount)
        return;
    auto& settings = settings_.bands[static_cast<std::size_t>(band)];
    settings.frequency = math::clamp(hz, 10.0, 30000.0);
    frequencySmoothers_[static_cast<std::size_t>(band)].setTarget(static_cast<float>(settings.frequency));
    needsRebuild_ = true;
}

void Equalizer::setBandQ(EqBand band, double q) noexcept {
    if (static_cast<int>(band) < 0 || static_cast<int>(band) >= kEqBandCount)
        return;
    settings_.bands[static_cast<std::size_t>(band)].q = math::clamp(q, 0.05, 20.0);
    needsRebuild_ = true;
}

void Equalizer::setBandGainDb(EqBand band, double db) noexcept {
    if (static_cast<int>(band) < 0 || static_cast<int>(band) >= kEqBandCount)
        return;
    auto& settings = settings_.bands[static_cast<std::size_t>(band)];
    settings.gainDb = math::clamp(db, -24.0, 24.0);
    gainSmoothers_[static_cast<std::size_t>(band)].setTarget(static_cast<float>(settings.gainDb));
    needsRebuild_ = true;
}

bool Equalizer::isActive() const noexcept {
    for (const auto& band : settings_.bands) {
        if (band.enabled) {
            // A flat shelf/bell/… still costs CPU but not much; report active
            // whenever a band is switched on so the UI response curve and the
            // graph's skip logic agree.
            return true;
        }
    }
    return false;
}

void Equalizer::rebuildCoefficients() noexcept {
    for (int band = 0; band < kEqBandCount; ++band) {
        const auto index = static_cast<std::size_t>(band);
        const EqBandSettings& settings = settings_.bands[index];
        const double frequency = static_cast<double>(frequencySmoothers_[index].current());
        const double gainDb = static_cast<double>(gainSmoothers_[index].current());

        if (!settings.enabled) {
            filters_[index].setCoefficients(BiquadCoefficients{}); // unity
            continue;
        }

        switch (static_cast<EqBand>(band)) {
        case EqBand::HighPass:
            filters_[index].setCoefficients(
                BiquadDesign::highPass(sampleRate_, frequency, settings.q));
            break;
        case EqBand::LowShelf:
            filters_[index].setCoefficients(
                BiquadDesign::lowShelf(sampleRate_, frequency, settings.slope, gainDb));
            break;
        case EqBand::Bell:
            filters_[index].setCoefficients(
                BiquadDesign::peaking(sampleRate_, frequency, settings.q, gainDb));
            break;
        case EqBand::HighShelf:
            filters_[index].setCoefficients(
                BiquadDesign::highShelf(sampleRate_, frequency, settings.slope, gainDb));
            break;
        case EqBand::LowPass:
            filters_[index].setCoefficients(
                BiquadDesign::lowPass(sampleRate_, frequency, settings.q));
            break;
        }
        lastRebuildFrequency_[index] = frequency;
        lastRebuildGain_[index] = gainDb;
    }
    needsRebuild_ = false;
}

void Equalizer::process(AudioBlockView& block, const ProcessContext& /*context*/) noexcept {
    if (block.isEmpty() || !isActive())
        return;

    // Block-rate parameter smoothing: advance the smoothers by the block length
    // and rebuild coefficients only when they moved beyond tolerance.
    bool rebuild = needsRebuild_;
    for (int band = 0; band < kEqBandCount; ++band) {
        const auto index = static_cast<std::size_t>(band);
        const float frequency = frequencySmoothers_[index].advance(block.numFrames);
        const float gainDb = gainSmoothers_[index].advance(block.numFrames);
        const double referenceFrequency = lastRebuildFrequency_[index] > 0.0
                                              ? lastRebuildFrequency_[index]
                                              : 1000.0;
        if (std::abs(static_cast<double>(frequency) - referenceFrequency) >
            referenceFrequency * kFrequencyTolerance)
            rebuild = true;
        if (std::abs(static_cast<double>(gainDb) - lastRebuildGain_[index]) > kGainToleranceDb)
            rebuild = true;
    }
    if (rebuild)
        rebuildCoefficients();

    for (auto& filter : filters_) {
        if (!filter.coefficients().isBypass())
            filter.processBlock(block.channelPointers, block.numChannels, block.numFrames);
    }
}

double Equalizer::responseDb(double hz) const noexcept {
    double total = 0.0;
    for (int band = 0; band < kEqBandCount; ++band) {
        const auto index = static_cast<std::size_t>(band);
        if (!settings_.bands[index].enabled)
            continue;
        total += BiquadDesign::magnitudeDb(filters_[index].coefficients(), hz, sampleRate_);
    }
    return total;
}

} // namespace aura::dsp
