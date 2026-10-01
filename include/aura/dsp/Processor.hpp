// ============================================================================
// AURA DAW - dsp/Processor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Processor contract shared by every DSP module in AURA.
//
// Real-time rules that every implementation must obey (enforced by
// tests/engine/RealtimeSafetyTests.cpp and tools/rt_audit.py):
//   1. process() must not allocate, lock, log, or touch the file system.
//   2. prepare() may allocate - it is called from the UI/worker thread and the
//      engine only ever swaps prepared instances while the audio thread is
//      stopped or through the lock-free command queue.
//   3. Parameter setters are lock-free (atomics / plain floats written by one
//      writer). No setter may allocate.
//   4. Output must be finite and denormal-free for any finite input.
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

#include "aura/core/Math.hpp"
#include "aura/core/Realtime.hpp"

namespace aura::dsp {

/// Audio block passed through the processor chain.
/// Planar layout (channel-major) as used by the engine's mix buffers.
/// `channelPointers` always contains at least `numChannels` valid pointers.
struct AudioBlockView {
    float* const* channelPointers = nullptr;
    int numChannels = 0;
    int numFrames = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return numChannels <= 0 || numFrames <= 0; }
};

/// Transport/playhead information made available to processors that need it
/// (tempo-synced delay, LFO sync, metering windows, automation).
struct ProcessContext {
    double sampleRate = 48000.0;
    std::int64_t positionSamples = 0; ///< absolute position of block start
    double tempoBpm = 120.0;
    std::int64_t positionTicks = 0;
    bool isPlaying = false;
    bool isRendering = false; ///< true for offline bounce (affects, e.g., metering)
};

/// The parameter identifiers a processor exposes for automation/undo.
struct ParameterInfo {
    const char* id = "";
    const char* name = "";
    const char* unit = "";
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    float skew = 1.0f;             ///< 1 = linear; used by the UI mapping
    bool isAutomatable = true;
};

/// Base class. Virtual dispatch happens once per block, never per sample.
class Processor {
public:
    virtual ~Processor() = default;

    /// Called when the sample rate or maximum block size changes. Allocation is
    /// allowed here. Must be idempotent.
    virtual void prepare(double sampleRate, int maxBlockSize, int numChannels) = 0;

    /// Clear internal state (delay lines, filter memories, meters).
    virtual void reset() noexcept = 0;

    /// Process audio in place. Real-time safe.
    virtual void process(AudioBlockView& block, const ProcessContext& context) noexcept = 0;

    /// Human-readable type name used by the UI and by tests.
    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;

    /// Reported latency in samples introduced by this processor (0 if none).
    [[nodiscard]] virtual int latencySamples() const noexcept { return 0; }

    /// True while the processor is producing audible output that must keep the
    /// graph alive (reverb tails, delay lines). The engine uses this to decide
    /// whether an idle track can be skipped - a correctness-critical
    /// optimisation, not just a CPU one.
    [[nodiscard]] virtual bool isActive() const noexcept { return true; }

    /// True while the processor is still emitting audio of its own - a delay or
    /// reverb tail, a running noise generator - and therefore has to keep being
    /// processed even when its *input* is silent.
    ///
    /// This is deliberately separate from isActive(): isActive() answers "is this
    /// processor switched on / not bypassed", which is what the UI and the
    /// parameter layer care about, while hasTail() answers "would skipping this
    /// processor drop audio". A silent, tail-less node can be skipped entirely,
    /// which is what makes a 24-track session cheap to run.
    [[nodiscard]] virtual bool hasTail() const noexcept { return false; }

    [[nodiscard]] virtual int numParameters() const noexcept { return 0; }
    [[nodiscard]] virtual ParameterInfo parameterInfo(int index) const noexcept {
        (void)index;
        return {};
    }
    virtual void setParameterValue(int index, float value) noexcept {
        (void)index;
        (void)value;
    }
    [[nodiscard]] virtual float parameterValue(int index) const noexcept {
        (void)index;
        return 0.0f;
    }

    /// Bypass is applied by the wrapper below, not by each processor.
    void setBypassed(bool bypassed) noexcept { bypassed_.store(bypassed, std::memory_order_relaxed); }
    [[nodiscard]] bool isBypassed() const noexcept {
        return bypassed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int numChannels() const noexcept { return numChannels_; }

protected:
    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    int numChannels_ = 2;
    std::atomic<bool> bypassed_{false};
};

/// Bypass wrapper: keeps processors from duplicating the "skip but stay
/// prepared" logic. When bypassed, processing is skipped but state is NOT reset
/// so A/B comparisons do not click.
class BypassProcessor final : public Processor {
public:
    explicit BypassProcessor(Processor* inner) : inner_(inner) {}

    void prepare(double sampleRate, int maxBlockSize, int numChannels) override {
        sampleRate_ = sampleRate;
        if (inner_)
            inner_->prepare(sampleRate, maxBlockSize, numChannels);
    }
    void reset() noexcept override {
        if (inner_)
            inner_->reset();
    }
    void process(AudioBlockView& block, const ProcessContext& context) noexcept override {
        if (!inner_ || inner_->isBypassed())
            return;
        inner_->process(block, context);
    }
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return inner_ ? inner_->typeName() : std::string_view{"Bypassed"};
    }
    [[nodiscard]] int latencySamples() const noexcept override {
        // Even bypassed, latency must still be reported so the host keeps its
        // delay compensation graph stable when the user toggles bypass.
        return inner_ ? inner_->latencySamples() : 0;
    }
    [[nodiscard]] bool isActive() const noexcept override {
        return inner_ && inner_->isActive();
    }
    [[nodiscard]] bool hasTail() const noexcept override {
        return inner_ && inner_->hasTail();
    }

private:
    Processor* inner_ = nullptr;
};

/// Utility: true when every sample of the block is finite.
[[nodiscard]] bool blockIsFinite(const AudioBlockView& block) noexcept;

/// Utility: replace non-finite samples with 0 (used at the master bus).
void sanitizeBlock(AudioBlockView& block) noexcept;

/// Utility: measure the peak level of a block.
[[nodiscard]] float blockPeak(const AudioBlockView& block) noexcept;

/// Utility: apply a per-channel gain ramp from `from` to `to`.
void applyGainRamp(float* buffer, int numFrames, float from, float to) noexcept;

/// Utility: zero a block.
void clearBlock(AudioBlockView& block) noexcept;

} // namespace aura::dsp
