// ============================================================================
// AURA DAW - graph/SampleStream.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A "sample stream" is what an audio clip uses to deliver samples to the audio
// thread. Two implementations:
//
//   MemoryStream - whole file resident (short clips, offline render)
//   DiskStream   - background reader thread fills a ring buffer; the audio
//                  thread only ever reads sequentially and never blocks
//
// POSITIONING CONTRACT (important):
//   Audio playback is inherently sequential. `read()` therefore reads from the
//   stream's current position and advances it; it never blocks and never
//   allocates. Random access is expressed with `seek()`, which for DiskStream
//   is an RT-safe *request*: the reader thread repositions and re-fills, and
//   reads return 0 (underrun) until the first frames arrive. Callers that jump
//   the playhead do the seek from a control thread *before* audio resumes, so
//   the ring is warm by the time the audio thread reads (see AudioEngine).
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/RingBuffer.hpp"
#include "aura/media/AudioFile.hpp"

namespace aura::graph {

class SampleStream {
public:
    virtual ~SampleStream();

    [[nodiscard]] virtual int numChannels() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t lengthFrames() const noexcept = 0;
    [[nodiscard]] virtual double sampleRate() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t position() const noexcept = 0;

    /// Requests a position. RT-safe for both implementations.
    virtual void seek(std::int64_t frame) noexcept = 0;

    /// Reads the next `frames` frames into planar buffers starting at the
    /// current position, advancing it. Returns frames produced (0 at end of
    /// stream or on underrun). The caller is responsible for zero-filling the
    /// remainder so a stalled disk can never produce recycled audio.
    virtual int read(float* const* destinations, int frames) noexcept = 0;

    /// Convenience: read and zero-fill the rest. The zero-fill is bounded by
    /// numChannels() (never by a null terminator: the contract is "the caller
    /// passes exactly one pointer per channel").
    int readWithSilence(float* const* destinations, int frames) noexcept {
        const int produced = read(destinations, frames);
        if (produced < frames && destinations != nullptr) {
            const int channels = numChannels();
            for (int channel = 0; channel < channels; ++channel) {
                if (destinations[channel] == nullptr)
                    continue;
                for (int i = produced; i < frames; ++i)
                    destinations[channel][i] = 0.0f;
            }
        }
        return produced;
    }

    /// Releases file handles / joins the reader thread. Control thread only.
    virtual void close() noexcept {}

    [[nodiscard]] virtual bool isStreaming() const noexcept { return false; }
    /// Frames served as silence because the reader could not keep up.
    [[nodiscard]] virtual std::uint64_t underrunCount() const noexcept { return 0; }
};

/// Fully resident in RAM.
class MemoryStream final : public SampleStream {
public:
    MemoryStream() = default;

    static std::shared_ptr<MemoryStream> fromPlanar(std::vector<std::vector<float>> channels,
                                                    double sampleRate);

    [[nodiscard]] int numChannels() const noexcept override {
        return static_cast<int>(channels_.size());
    }
    [[nodiscard]] std::int64_t lengthFrames() const noexcept override { return length_; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] std::int64_t position() const noexcept override { return position_; }

    void seek(std::int64_t frame) noexcept override {
        position_ = frame < 0 ? 0 : frame;
    }

    int read(float* const* destinations, int frames) noexcept override;

private:
    std::vector<std::vector<float>> channels_;
    std::int64_t length_ = 0;
    std::int64_t position_ = 0;
    double sampleRate_ = 48000.0;
};

/// Streams from disk through a ring buffer filled by a background thread.
class DiskStream final : public SampleStream {
public:
    static constexpr int kMaxReadFrames = 4096;

    DiskStream() = default;
    ~DiskStream() override;

    /// Opens the file and starts the reader thread. Control thread only.
    static AuraResult<std::shared_ptr<DiskStream>> open(const std::string& path,
                                                        std::size_t ringFrames = 1u << 16);

    [[nodiscard]] int numChannels() const noexcept override {
        return static_cast<int>(channels_);
    }
    [[nodiscard]] std::int64_t lengthFrames() const noexcept override { return lengthFrames_; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] bool isStreaming() const noexcept override { return true; }
    [[nodiscard]] std::int64_t position() const noexcept override { return position_; }
    [[nodiscard]] std::uint64_t underrunCount() const noexcept override {
        return underruns_.load(std::memory_order_relaxed);
    }

    void seek(std::int64_t frame) noexcept override;

    /// RT-safe: consumes `frames` from the ring starting at the current
    /// position. Returns fewer frames (possibly 0) when the reader has not
    /// caught up, which the caller turns into silence.
    int read(float* const* destinations, int frames) noexcept override;

    void close() noexcept override;

    /// Frames currently buffered ahead of the read position (diagnostics/UI).
    [[nodiscard]] std::size_t bufferedFrames() const noexcept {
        return ring_.availableToRead();
    }

private:
    void readerLoop();

    AudioRingBuffer ring_;
    std::thread readerThread_;
    std::atomic<bool> quit_{false};
    /// Absolute frame position of the first frame currently in the ring.
    std::atomic<std::int64_t> ringStart_{0};
    /// Position the reader should (re)fill from. Negative means "no pending
    /// request": the reader must distinguish "0 was requested" from "nothing was
    /// requested", or it rewinds to frame 0 after every chunk.
    std::atomic<std::int64_t> requestedPosition_{-1};
    std::atomic<std::uint64_t> underruns_{0};

    std::int64_t lengthFrames_ = 0;
    std::int64_t position_ = 0; ///< audio-thread read cursor
    int channels_ = 2;
    double sampleRate_ = 48000.0;
    std::string path_;
    std::vector<float> scratch_; ///< interleaved scratch owned by the reader thread
    /// De-interleaving scratch used by read(). Preallocated so the audio thread
    /// never allocates; reads are therefore capped at kMaxReadFrames per call
    /// (the engine's block size is <= 2048, so this is not a restriction).
    std::vector<float> deinterleaveScratch_;
    media::AudioFileReader reader_;
};

/// Picks the right stream for the job:
///   * `preferMemory` (offline render) or files shorter than
///     `memoryThresholdSeconds` load fully;
///   * everything else streams from disk so a 3-hour recording stays editable.
[[nodiscard]] AuraResult<std::shared_ptr<SampleStream>> openSampleStream(
    const std::string& path, bool preferMemory = false, double memoryThresholdSeconds = 30.0);

} // namespace aura::graph
