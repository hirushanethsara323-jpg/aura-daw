// ============================================================================
// AURA DAW - core/RingBuffer.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Single-producer / single-consumer float ring buffer used for
//   * recording:  audio thread -> disk writer thread
//   * streaming:  file reader thread -> audio thread (clip playback)
//
// Both sides are wait-free. The ring is a power-of-two sized array of
// interleaved-free "frames" (channels x samples) so a block copy is a plain
// memcpy per interleaved run.
// ============================================================================
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <vector>

#include "aura/core/Math.hpp"

namespace aura {

/// Multi-channel float ring buffer, planar-interleaved-block layout:
/// samples are stored interleaved as [frame][channel].
#if defined(_MSC_VER)
// C4324 ("structure was padded due to alignment specifier", /W4) is intentional
// here: writePos_ and readPos_ each own a cache line so the producer and the
// consumer never share one. Scoped to this class so the rest of the file - and of
// the project - keeps the warning.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class AudioRingBuffer {
public:
    AudioRingBuffer() = default;

    /// Allocates storage. NOT real-time safe - call from a setup/worker thread.
    void prepare(std::size_t capacityFrames, std::size_t channels) {
        // The ring keeps one frame empty to tell "full" from "empty", so the
        // allocation is one frame larger than the usable capacity: asking for
        // 512 frames gives the caller 512 usable frames, not 511.
        capacity_ = static_cast<std::size_t>(math::nextPowerOfTwo(capacityFrames + 1));
        channels_ = channels;
        mask_ = capacity_ - 1;
        storage_.assign(capacity_ * channels_, 0.0f);
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacityFrames() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t channels() const noexcept { return channels_; }
    [[nodiscard]] bool isPrepared() const noexcept { return capacity_ > 0 && channels_ > 0; }

    /// Frames currently readable.
    [[nodiscard]] std::size_t availableToRead() const noexcept {
        const std::size_t w = writePos_.load(std::memory_order_acquire);
        const std::size_t r = readPos_.load(std::memory_order_relaxed);
        return w - r;
    }

    /// Free space for the writer.
    [[nodiscard]] std::size_t availableToWrite() const noexcept {
        const std::size_t w = writePos_.load(std::memory_order_relaxed);
        const std::size_t r = readPos_.load(std::memory_order_acquire);
        return capacity_ - (w - r) - 1; // keep one frame gap to distinguish full/empty
    }

    /// Producer: writes `frames` of interleaved data. Returns frames written.
    std::size_t write(const float* interleaved, std::size_t frames) noexcept {
        const std::size_t freeFrames = availableToWrite();
        const std::size_t toWrite = frames < freeFrames ? frames : freeFrames;
        if (toWrite == 0 || channels_ == 0)
            return 0;

        const std::size_t w = writePos_.load(std::memory_order_relaxed);
        std::size_t remaining = toWrite;
        std::size_t sourceOffset = 0;
        while (remaining > 0) {
            const std::size_t index = w & mask_;
            const std::size_t run = (capacity_ - index) < remaining ? (capacity_ - index) : remaining;
            std::memcpy(storage_.data() + index * channels_,
                        interleaved + sourceOffset * channels_, run * channels_ * sizeof(float));
            remaining -= run;
            sourceOffset += run;
        }
        writePos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    /// Consumer: reads up to `frames` into interleaved storage. Returns frames read.
    std::size_t read(float* interleaved, std::size_t frames) noexcept {
        const std::size_t available = availableToRead();
        const std::size_t toRead = frames < available ? frames : available;
        if (toRead == 0 || channels_ == 0)
            return 0;

        const std::size_t r = readPos_.load(std::memory_order_relaxed);
        std::size_t remaining = toRead;
        std::size_t destOffset = 0;
        while (remaining > 0) {
            const std::size_t index = r & mask_;
            const std::size_t run = (capacity_ - index) < remaining ? (capacity_ - index) : remaining;
            std::memcpy(interleaved + destOffset * channels_, storage_.data() + index * channels_,
                        run * channels_ * sizeof(float));
            remaining -= run;
            destOffset += run;
        }
        readPos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    /// Zero-fills the gap when a producer stalls (prevents stale-repeat artefacts).
    void readWithSilencePad(float* interleaved, std::size_t frames, std::size_t& framesRead) noexcept {
        framesRead = read(interleaved, frames);
        if (framesRead < frames)
            std::memset(interleaved + framesRead * channels_, 0,
                        (frames - framesRead) * channels_ * sizeof(float));
    }

    void clear() noexcept {
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> storage_;
    std::size_t capacity_ = 0;
    std::size_t channels_ = 0;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> writePos_{0};
    alignas(64) std::atomic<std::size_t> readPos_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aura
