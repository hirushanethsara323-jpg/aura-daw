// ============================================================================
// AURA DAW - dsp/Equalizer.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Five-node parametric EQ used as the channel strip EQ:
//   Band 0 : High-Pass  (12 dB/oct, Q-adjustable)
//   Band 1 : Low Shelf
//   Band 2 : Bell / Peaking  (the primary tone control)
//   Band 3 : High Shelf
//   Band 4 : Low-Pass   (12 dB/oct)
//
// Each band has an on/off switch (per the brief: "at minimum high-pass,
// low-pass, low shelf, high shelf, parametric bell"). Frequency, Q and gain
// are smoothed per block so automation sweeps do not zipper.
//
// Architecture note: coefficient updates happen at block rate only, which is
// the standard trade-off (per-sample coefficient recalculation costs ~40
// flops/channel/sample with no audible benefit at <= 512 frame blocks except
// for deliberate filter sweeps; those use the same path as automation).
// ============================================================================
#pragma once

#include <array>

#include "aura/dsp/Biquad.hpp"
#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

enum class EqBand : int {
    HighPass = 0,
    LowShelf = 1,
    Bell = 2,
    HighShelf = 3,
    LowPass = 4,
};

/// Number of EQ bands (kept as a separate constant so `std::array` sizes and
/// loop bounds stay readable and cannot silently drift from the enum).
inline constexpr int kEqBandCount = 5;

struct EqBandSettings {
    bool enabled = false;
    double frequency = 1000.0;
    double q = 0.707;
    double gainDb = 0.0;
    /// Shelf slope for the shelf bands (ignored by HP/LP/Bell).
    double slope = 0.7;
};

/// Defaults chosen to be musically useful out of the box:
/// HP at 20 Hz (safety), shelves flat, bell "ready" at 1 kHz.
struct EqSettings {
    std::array<EqBandSettings, kEqBandCount> bands{{
        {false, 20.0, 0.707, 0.0, 0.7},    // HighPass
        {false, 120.0, 0.707, 0.0, 0.7},   // LowShelf
        {false, 1000.0, 1.0, 0.0, 0.7},    // Bell
        {false, 8000.0, 0.707, 0.0, 0.7},  // HighShelf
        {false, 20000.0, 0.707, 0.0, 0.7}, // LowPass
    }};
    bool bypassed = false;
};

class Equalizer final : public Processor {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "EQ"; }

    /// Applies a complete EQ setting. Safe to call from the UI thread; the audio
    /// thread picks changes up at the next block through a double-buffered swap
    /// (see AudioEngineCommandQueue). Here we simply store targets.
    void setSettings(const EqSettings& settings) noexcept;
    [[nodiscard]] const EqSettings& settings() const noexcept { return settings_; }

    void setBandEnabled(EqBand band, bool enabled) noexcept;
    void setBandFrequency(EqBand band, double hz) noexcept;
    void setBandQ(EqBand band, double q) noexcept;
    void setBandGainDb(EqBand band, double db) noexcept;

    /// Frequency response magnitude in dB at `hz`. Control-thread only
    /// (allocation-free but calls std::cos/sin per band per point).
    [[nodiscard]] double responseDb(double hz) const noexcept;

    [[nodiscard]] int latencySamples() const noexcept override { return 0; }
    /// True when at least one band is enabled - lets the graph skip the EQ
    /// entirely for untouched tracks.
    [[nodiscard]] bool isActive() const noexcept override;

private:
    void rebuildCoefficients() noexcept;

    EqSettings settings_{};
    std::array<Biquad<2>, kEqBandCount> filters_{};
    /// Frequency/Q/gain smoothing is applied at block rate: we interpolate the
    /// *coefficient targets* by rebuilding from smoothed parameter values.
    std::array<OnePoleSmoother, kEqBandCount> frequencySmoothers_{};
    std::array<OnePoleSmoother, kEqBandCount> gainSmoothers_{};
    bool needsRebuild_ = true;
    double lastRebuildFrequency_[kEqBandCount]{};
    double lastRebuildGain_[kEqBandCount]{};
};

} // namespace aura::dsp
