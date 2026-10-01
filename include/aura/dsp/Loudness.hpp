// ============================================================================
// AURA DAW - include/aura/dsp/Loudness.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ITU-R BS.1770-4 / EBU R128 loudness measurement.
//   * K-weighting (high shelf + RLB high-pass) per channel
//   * Momentary (400 ms), short-term (3 s), integrated (gated) loudness
//   * Gating: -70 LUFS absolute, then -10 LU relative
//   * Loudness range (LRA) per EBU Tech 3342
//
// THREADING (important, and the reason this class is shaped the way it is):
//   * The audio thread ONLY appends a 400 ms block loudness value into a
//     preallocated ring and updates the two cheap sliding windows. It never
//     sorts, never allocates, never takes a lock.
//   * Integrated loudness and LRA need a full pass over the block history
//     (gating + percentile), which is O(history) and would be unacceptable on
//     the audio thread. They are therefore computed on demand inside
//     snapshot()/analysis, which the UI calls at frame rate. The ring is read
//     with relaxed atomics; a torn read of a single float in a *meter* is
//     benign (documented deliberate trade-off, the same as peak meters).
//
// Documented limitations (V1):
//   * Stereo only; surround channel weighting (G = 1.41) is an extension point.
//   * True-peak is not measured here (see Limiter.hpp).
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "aura/dsp/Biquad.hpp"
#include "aura/dsp/Processor.hpp"

namespace aura::dsp {

struct LoudnessSnapshot {
    float momentaryLufs = -100.0f;  ///< 400 ms window
    float shortTermLufs = -100.0f;  ///< 3 s window
    float integratedLufs = -100.0f; ///< gated programme loudness
    float rangeLu = 0.0f;           ///< loudness range (LU)
    float maxMomentaryLufs = -100.0f;
    float maxShortTermLufs = -100.0f;
    bool integratedValid = false;
};

class LoudnessMeter {
public:
    static constexpr int kMaxSteps = 36000; ///< 1 hour of 100 ms steps

    void prepare(double sampleRate, int numChannels = 2);
    void reset() noexcept;

    /// Audio thread: feed a block. Allocation-free, lock-free.
    void process(const AudioBlockView& block) noexcept;

    /// Control thread (UI frame rate): full snapshot including integrated LUFS
    /// and LRA. Allocates nothing after prepare().
    [[nodiscard]] LoudnessSnapshot snapshot() const;

    /// True once at least one gated block has been measured.
    [[nodiscard]] bool hasIntegratedMeasurement() const noexcept {
        return stepsRecorded_.load(std::memory_order_relaxed) >= 4;
    }

private:
    void feedStep(float meanSquare) noexcept;
    /// Appends the current 400 ms momentary value to the gating ring.
    void pushGatingBlock(float lufs) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 2;
    std::array<Biquad<2>, 2> preFilter_{};
    std::array<Biquad<2>, 2> rlbFilter_{};

    int samplesPerStep_ = 4800;
    int stepSamples_ = 0;
    double stepAccumulator_ = 0.0;

    /// Ring of 100 ms step mean squares (written by the audio thread).
    std::vector<float> stepMeanSquare_;
    std::atomic<std::size_t> stepWrite_{0};
    std::atomic<int> stepsRecorded_{0};

    /// Ring of 400 ms gating blocks (written by the audio thread).
    std::vector<float> gatingBlocks_;
    std::atomic<std::size_t> gatingWrite_{0};
    std::atomic<int> gatingRecorded_{0};

    /// Control-thread scratch (allocated in prepare()).
    mutable std::vector<float> scratchGated_;

    std::atomic<float> momentary_{-100.0f};
    std::atomic<float> shortTerm_{-100.0f};
    std::atomic<float> maxMomentary_{-100.0f};
    std::atomic<float> maxShortTerm_{-100.0f};
};

} // namespace aura::dsp
