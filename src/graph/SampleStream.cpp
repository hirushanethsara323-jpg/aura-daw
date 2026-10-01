// ============================================================================
// AURA DAW - src/graph/SampleStream.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/graph/SampleStream.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "aura/core/Log.hpp"

namespace aura::graph {
namespace {
constexpr const char* kCategory = "Graph.SampleStream";
constexpr int kReaderChunkFrames = 8192;
} // namespace

SampleStream::~SampleStream() = default;

// ---------------------------------------------------------------------------
// MemoryStream
// ---------------------------------------------------------------------------
std::shared_ptr<MemoryStream> MemoryStream::fromPlanar(std::vector<std::vector<float>> channels,
                                                       double sampleRate) {
    auto stream = std::make_shared<MemoryStream>();
    stream->channels_ = std::move(channels);
    stream->sampleRate_ = sampleRate;
    stream->length_ = stream->channels_.empty()
                          ? 0
                          : static_cast<std::int64_t>(stream->channels_[0].size());
    for (const auto& channel : stream->channels_)
        stream->length_ = std::min(stream->length_, static_cast<std::int64_t>(channel.size()));
    return stream;
}

int MemoryStream::read(float* const* destinations, int frames) noexcept {
    if (frames <= 0 || position_ >= length_)
        return 0;
    const int available = static_cast<int>(std::min<std::int64_t>(frames, length_ - position_));
    const int channels = numChannels();
    for (int channel = 0; channel < channels; ++channel) {
        const auto& source = channels_[static_cast<std::size_t>(channel)];
        std::memcpy(destinations[channel], source.data() + position_,
                    static_cast<std::size_t>(available) * sizeof(float));
    }
    position_ += available;
    return available;
}

// ---------------------------------------------------------------------------
// DiskStream
// ---------------------------------------------------------------------------
DiskStream::~DiskStream() {
    close();
}

AuraResult<std::shared_ptr<DiskStream>> DiskStream::open(const std::string& path,
                                                         std::size_t ringFrames) {
    auto stream = std::shared_ptr<DiskStream>(new DiskStream());
    auto info = stream->reader_.open(path);
    if (!info)
        return failure(info.error());

    stream->path_ = path;
    stream->lengthFrames_ = info.value().lengthFrames;
    stream->channels_ = info.value().numChannels;
    stream->sampleRate_ = info.value().sampleRate;
    stream->ring_.prepare(ringFrames, static_cast<std::size_t>(stream->channels_));
    stream->scratch_.assign(static_cast<std::size_t>(stream->channels_) * kReaderChunkFrames, 0.0f);
    stream->deinterleaveScratch_.assign(static_cast<std::size_t>(stream->channels_) *
                                            static_cast<std::size_t>(kMaxReadFrames),
                                        0.0f);
    stream->requestedPosition_.store(-1, std::memory_order_relaxed);
    stream->ringStart_.store(0, std::memory_order_relaxed);
    stream->underruns_.store(0, std::memory_order_relaxed);
    stream->quit_.store(false, std::memory_order_relaxed);
    stream->readerThread_ = std::thread([stream] { stream->readerLoop(); });
    return success(stream);
}

void DiskStream::readerLoop() {
    std::int64_t localPosition = 0;

    while (!quit_.load(std::memory_order_relaxed)) {
        // Reposition only when the audio thread published a *new* request
        // (exchange leaves -1 behind, so sequential playback never rewinds).
        const std::int64_t requested = requestedPosition_.exchange(-1, std::memory_order_acq_rel);
        if (requested >= 0 && requested != localPosition) {
            localPosition = requested;
            ring_.clear();
            ringStart_.store(localPosition, std::memory_order_release);
        }

        if (localPosition >= lengthFrames_)
            break; // stream fully buffered

        if (ring_.availableToWrite() < static_cast<std::size_t>(kReaderChunkFrames)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const int framesToRead = static_cast<int>(std::min<std::int64_t>(
            static_cast<std::int64_t>(kReaderChunkFrames), lengthFrames_ - localPosition));
        if (framesToRead <= 0)
            break;

        const int read = reader_.readInterleaved(localPosition, scratch_.data(), framesToRead);
        if (read <= 0)
            break;
        const std::size_t written = ring_.write(scratch_.data(), static_cast<std::size_t>(read));
        localPosition += static_cast<std::int64_t>(written);
        if (written == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DiskStream::seek(std::int64_t frame) noexcept {
    if (frame < 0)
        frame = 0;
    position_ = frame;
    // RT-safe: publish the request; the reader thread repositions and refills.
    requestedPosition_.store(frame, std::memory_order_release);
}

int DiskStream::read(float* const* destinations, int frames) noexcept {
    if (frames <= 0)
        return 0;
    if (position_ >= lengthFrames_)
        return 0;

    const std::int64_t ringStart = ringStart_.load(std::memory_order_acquire);
    const std::size_t buffered = ring_.availableToRead();

    // The ring holds a contiguous forward window starting at ringStart. If our
    // cursor is behind or ahead of that window the reader has not caught up
    // yet: report an underrun and re-request the position.
    if (position_ < ringStart || position_ >= ringStart + static_cast<std::int64_t>(buffered)) {
        if (position_ != ringStart) {
            // Discard the frames between the ring start and our cursor within
            // the buffered window (bounded: straight sequential playback never
            // hits this path).
            if (position_ > ringStart && position_ < ringStart + static_cast<std::int64_t>(buffered)) {
                std::size_t toSkip = static_cast<std::size_t>(position_ - ringStart);
                // Skip in place using a small stack buffer to stay allocation-free.
                float skipBuffer[2 * 256];
                float* skipPointers[2] = {skipBuffer, skipBuffer + 256};
                while (toSkip > 0) {
                    const int chunk = static_cast<int>(std::min<std::size_t>(toSkip, 256));
                    const std::size_t got = ring_.read(skipPointers[0], static_cast<std::size_t>(chunk));
                    (void)got;
                    toSkip -= static_cast<std::size_t>(chunk);
                }
            } else {
                seek(position_); // ask for a refill at the current cursor
            }
        }
        underruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Skip within the window if needed (bounded by the window size).
    std::size_t toSkip = static_cast<std::size_t>(position_ - ringStart);
    float skipBuffer[2 * 256];
    float* skipPointers[2] = {skipBuffer, skipBuffer + 256};
    while (toSkip > 0) {
        const std::size_t chunk = std::min<std::size_t>(toSkip, 256);
        const std::size_t got = ring_.read(skipPointers[0], chunk);
        toSkip -= got;
        if (got == 0)
            break;
    }

    // Now read the requested frames (interleaved storage -> planar output).
    const std::size_t maxFrames = frames < static_cast<int>(buffered - (position_ - ringStart))
                                      ? static_cast<std::size_t>(frames)
                                      : buffered - static_cast<std::size_t>(position_ - ringStart);
    if (maxFrames == 0) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Read interleaved into the preallocated scratch, then de-interleave.
    // Frames per call are capped at kMaxReadFrames so no growth is ever needed.
    const std::size_t capped = std::min<std::size_t>(
        maxFrames, static_cast<std::size_t>(kMaxReadFrames));
    const std::size_t got = ring_.read(deinterleaveScratch_.data(), capped);
    const int channels = channels_;
    for (std::size_t i = 0; i < got; ++i) {
        for (int channel = 0; channel < channels; ++channel)
            destinations[channel][i] =
                deinterleaveScratch_[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
    }
    position_ += static_cast<std::int64_t>(got);
    return static_cast<int>(got);
}

void DiskStream::close() noexcept {
    if (quit_.exchange(true))
        return;
    if (readerThread_.joinable())
        readerThread_.join();
    reader_.close();
    AURA_LOG_DEBUG(kCategory, "Closed disk stream %s (underruns: %llu)", path_.c_str(),
                   static_cast<unsigned long long>(underruns_.load(std::memory_order_relaxed)));
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
AuraResult<std::shared_ptr<SampleStream>> openSampleStream(const std::string& path,
                                                           bool preferMemory,
                                                           double memoryThresholdSeconds) {
    auto info = media::probeFile(path);
    if (!info)
        return failure(info.error());

    const bool fitsInMemory = info.value().lengthSeconds() <= memoryThresholdSeconds;
    if (preferMemory || fitsInMemory) {
        auto loaded = media::readFileToPlanar(path);
        if (!loaded)
            return failure(loaded.error());
        return success<std::shared_ptr<SampleStream>>(
            MemoryStream::fromPlanar(std::move(*loaded.value()), info.value().sampleRate));
    }

    auto stream = DiskStream::open(path);
    if (!stream)
        return failure(stream.error());
    AURA_LOG_INFO(kCategory, "Streaming '%s' from disk: %.1f s, %d ch, %d Hz", path.c_str(),
                  info.value().lengthSeconds(), info.value().numChannels, info.value().sampleRate);
    return success<std::shared_ptr<SampleStream>>(stream.value());
}

} // namespace aura::graph
