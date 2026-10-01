// ============================================================================
// AURA DAW - dsp/DelayLine.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// A fixed-capacity, integer-sample delay line used by delay compensation (and
// available to any processor that needs a plain delay). It is not the Delay
// *effect*: no feedback, no modulation, no interpolation - just "the same signal,
// N samples later", which is exactly what aligning parallel paths requires.
//
// Real-time contract:
//   * prepare() allocates and is called on the control thread (from
//     GraphNode::prepare(), i.e. when the graph is rebuilt).
//   * sumDelayed()/copyDelayed() never allocate, lock or branch unpredictably:
//     one write and one read per sample into a circular buffer.
//   * Delay values are clamped to the prepared capacity, so a bad value from a
//     misbehaving plug-in cannot read outside the buffer.
//
// Why integer samples and not fractional: compensation amounts are derived from
// integer latencies reported by processors (VST3's getLatencySamples(), our own
// Processor::latencySamples()), so the delay is exact at sample granularity.
// Fractional delays would need interpolation and would smear the very alignment
// they are supposed to fix.
// ============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace aura::dsp {

class DelayLine {
public:
    /// Allocates room for delays up to `maxDelaySamples` (inclusive).
    /// Control thread only.
    void prepare(int maxDelaySamples) {
        capacity_ = std::max(1, maxDelaySamples);
        // capacity_ + 1 slots so that a delay of exactly capacity_ is
        // representable without aliasing onto the sample being written.
        buffer_.assign(static_cast<std::size_t>(capacity_) + 1, 0.0f);
        writeIndex_ = 0;
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_ = 0;
    }

    [[nodiscard]] int maxDelaySamples() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t capacityBytes() const noexcept {
        return buffer_.size() * sizeof(float);
    }

    /// Adds `input` into `output`, delayed by `delaySamples`.
    /// `delaySamples <= 0` degenerates to a plain sum (no ring traffic).
    void sumDelayed(const float* input, float* output, int frames, int delaySamples) noexcept {
        if (frames <= 0 || input == nullptr || output == nullptr)
            return;
        if (delaySamples <= 0) {
            for (int i = 0; i < frames; ++i)
                output[i] += input[i];
            return;
        }
        const int delay = std::min(delaySamples, capacity_);
        const int size = capacity_ + 1;
        int write = writeIndex_;
        for (int i = 0; i < frames; ++i) {
            buffer_[static_cast<std::size_t>(write)] = input[i];
            int read = write - delay;
            if (read < 0)
                read += size;
            output[i] += buffer_[static_cast<std::size_t>(read)];
            if (++write == size)
                write = 0;
        }
        writeIndex_ = write;
    }

    /// Copies `input` into `output`, delayed by `delaySamples` (overwrites).
    void copyDelayed(const float* input, float* output, int frames, int delaySamples) noexcept {
        if (frames <= 0 || input == nullptr || output == nullptr)
            return;
        if (delaySamples <= 0) {
            std::copy(input, input + frames, output);
            return;
        }
        const int delay = std::min(delaySamples, capacity_);
        const int size = capacity_ + 1;
        int write = writeIndex_;
        for (int i = 0; i < frames; ++i) {
            buffer_[static_cast<std::size_t>(write)] = input[i];
            int read = write - delay;
            if (read < 0)
                read += size;
            output[i] = buffer_[static_cast<std::size_t>(read)];
            if (++write == size)
                write = 0;
        }
        writeIndex_ = write;
    }

private:
    std::vector<float> buffer_;
    int capacity_ = 0;
    int writeIndex_ = 0;
};

} // namespace aura::dsp
