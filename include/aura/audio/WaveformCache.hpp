// ============================================================================
// AURA DAW - audio/WaveformCache.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Waveform rendering data: multi-resolution min/max peak pyramids built in a
// background thread, cached on disk next to the audio ("<file>.aurapeaks"), and
// read by the UI at whatever zoom level it needs.
//
// Why a pyramid:
//   * At 1 hour of 48 kHz audio a single min/max pair per sample would be
//     345 MB of floats - unacceptable.
//   * The UI must never block: it draws from the coarsest level that is ready
//     and refines as finer levels arrive (progressive rendering).
//
// Structure:
//   Level 0: 256 frames per bucket   (the finest level we ever draw)
//   Level k: 2^k * 256 frames per bucket
//   ... up to a single bucket covering the whole file (8 levels for an hour)
//
// The cache file is self-describing (magic + version + frame count + sample
// rate + level count), so a stale or foreign file is detected and rebuilt
// rather than mis-drawn. Building a level is a single sequential pass: level n
// is derived from level n-1, so the whole pyramid costs one file read.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::audio {

struct PeakBucket {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

/// One resolution level of the pyramid.
struct PeakLevel {
    std::uint32_t framesPerBucket = 256;
    std::vector<PeakBucket> buckets;
    /// Number of buckets written so far (built progressivel by the worker).
    std::atomic<std::uint32_t> bucketsReady{0};

    [[nodiscard]] std::size_t bucketCount() const noexcept { return buckets.size(); }
};

/// Prepared, read-only view the UI uses to draw.
struct WaveformView {
    const PeakLevel* level = nullptr;
    std::uint32_t framesPerBucket = 256;
    double sampleRate = 48000.0;
    std::int64_t totalFrames = 0;
    bool complete = false;
};

class WaveformCache {
public:
    static constexpr std::uint32_t kBaseFramesPerBucket = 256;
    static constexpr std::uint32_t kMaxLevels = 12;
    static constexpr std::uint32_t kCacheMagic = 0x4155505AU; // 'AUPZ'
    static constexpr std::uint32_t kCacheVersion = 2;

    WaveformCache();
    ~WaveformCache();

    WaveformCache(const WaveformCache&) = delete;
    WaveformCache& operator=(const WaveformCache&) = delete;

    /// Starts building (or loading) the pyramid for `audioPath`. Returns
    /// immediately; progress is polled through `progress()` and `isComplete()`.
    /// Concurrent calls for the same path are de-duplicated.
    Status buildFor(const std::string& audioPath, std::uint32_t sampleRate,
                    std::int64_t totalFrames);

    /// Loads a previously written cache file if it matches `audioPath`
    /// (same size + modified time). Returns false when a rebuild is needed.
    [[nodiscard]] bool loadFromCacheFile(const std::string& cachePath, std::int64_t sourceBytes,
                                         std::int64_t sourceModifiedMs);

    /// Blocks until the build finishes (used by tests and by export-time peak
    /// generation). `timeoutMs == 0` waits indefinitely.
    bool waitForCompletion(std::uint32_t timeoutMs = 0);

    [[nodiscard]] std::atomic<float>& progress() noexcept { return progress_; }
    [[nodiscard]] float progress() const noexcept { return progress_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isComplete() const noexcept {
        return complete_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isCancelled() const noexcept {
        return cancel_.load(std::memory_order_relaxed);
    }
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }

    /// Chooses the level to draw with for a given samples-per-pixel zoom.
    [[nodiscard]] WaveformView viewFor(double samplesPerPixel) const noexcept;

    /// Reads buckets for drawing: fills `out` with up to `count` buckets from
    /// `firstBucket` at the chosen level. Real-time-ish safe for the UI thread
    /// (no allocation, no locking).
    [[nodiscard]] bool readBuckets(const PeakLevel& level, std::uint32_t firstBucket,
                                   std::uint32_t count, PeakBucket* out) const noexcept;

    /// Peak magnitude of the whole file (for Normalize and clip display).
    [[nodiscard]] float overallPeak() const noexcept {
        return overallPeak_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t levelCount() const noexcept {
        return levelCount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const PeakLevel* level(std::uint32_t index) const noexcept {
        return index < kMaxLevels ? &levels_[index] : nullptr;
    }
    [[nodiscard]] std::int64_t totalFrames() const noexcept { return totalFrames_; }
    [[nodiscard]] const std::string& sourcePath() const noexcept { return sourcePath_; }

    /// Writes the pyramid to disk (atomic: temp + rename).
    Status saveToCacheFile(const std::string& cachePath) const;

    /// Default cache path next to the audio file ("song.wav.aurapeaks").
    [[nodiscard]] static std::string cachePathFor(const std::string& audioPath);

private:
    void buildThread(std::string audioPath, std::uint32_t sampleRate, std::int64_t totalFrames);
    void buildLevelsFromSource(const std::vector<float>& interleavedPeaks, std::uint32_t channels);

    std::string sourcePath_;
    std::int64_t totalFrames_ = 0;
    std::uint32_t sampleRate_ = 48000;
    PeakLevel levels_[kMaxLevels];
    std::atomic<std::uint32_t> levelCount_{0};
    std::atomic<float> progress_{0.0f};
    std::atomic<float> overallPeak_{0.0f};
    std::atomic<bool> complete_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

/// Background peak builder used by the recorder: computes peaks incrementally
/// while recording so the waveform is available the moment recording stops.
class LivePeakAccumulator {
public:
    void reset(std::uint32_t framesPerBucket) noexcept;
    /// Feed a block (audio thread). Lock-free, allocation-free.
    void process(const float* const* channels, int numChannels, int frames) noexcept;
    /// Copy out the peaks built so far (worker thread).
    [[nodiscard]] std::vector<PeakBucket> takePeaks() const;

private:
    std::uint32_t framesPerBucket_ = 1024;
    std::uint32_t framesInBucket_ = 0;
    float bucketMin_ = 0.0f;
    float bucketMax_ = 0.0f;
    std::vector<PeakBucket> buckets_;
};

} // namespace aura::audio
