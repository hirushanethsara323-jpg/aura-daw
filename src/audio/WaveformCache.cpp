// ============================================================================
// AURA DAW - src/audio/WaveformCache.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audio/WaveformCache.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Log.hpp"
#include "aura/media/AudioFile.hpp"

namespace aura::audio {
namespace {
constexpr const char* kCategory = "Audio.Waveform";
constexpr std::size_t kReadFrames = 1u << 15; // 32768 frames per read
} // namespace

WaveformCache::WaveformCache() = default;

WaveformCache::~WaveformCache() {
    cancel();
    if (worker_.joinable())
        worker_.join();
}

Status WaveformCache::buildFor(const std::string& audioPath, std::uint32_t sampleRate,
                               std::int64_t totalFrames) {
    if (worker_.joinable())
        worker_.join(); // previous build (already cancelled or finished)
    cancel_.store(false, std::memory_order_relaxed);
    complete_.store(false, std::memory_order_relaxed);
    progress_.store(0.0f, std::memory_order_relaxed);
    overallPeak_.store(0.0f, std::memory_order_relaxed);
    levelCount_.store(0, std::memory_order_relaxed);
    sourcePath_ = audioPath;
    sampleRate_ = sampleRate;
    totalFrames_ = totalFrames;

    // Try the on-disk cache first: the UI gets a waveform immediately on a
    // project re-open instead of waiting for a rebuild.
    const std::string cachePath = cachePathFor(audioPath);
    const std::int64_t sourceBytes =
        static_cast<std::int64_t>(filesystem::fileSize(audioPath));
    const std::int64_t modified = filesystem::fileModifiedTimeMs(audioPath);
    if (loadFromCacheFile(cachePath, sourceBytes, modified)) {
        complete_.store(true, std::memory_order_release);
        progress_.store(1.0f, std::memory_order_relaxed);
        AURA_LOG_DEBUG(kCategory, "Loaded peak cache for %s (%u levels)", audioPath.c_str(),
                       levelCount_.load(std::memory_order_relaxed));
        return success();
    }

    worker_ = std::thread([this, audioPath, sampleRate, totalFrames] {
        buildThread(audioPath, sampleRate, totalFrames);
    });
    return success();
}

void WaveformCache::buildThread(std::string audioPath, std::uint32_t sampleRate,
                                std::int64_t totalFrames) {
    media::AudioFileReader reader;
    auto info = reader.open(audioPath);
    if (!info) {
        AURA_LOG_WARN(kCategory, "Cannot build waveform for %s: %s", audioPath.c_str(),
                      info.error().toString().c_str());
        complete_.store(true, std::memory_order_release);
        progress_.store(1.0f, std::memory_order_relaxed);
        return;
    }

    const int channels = info.value().numChannels;
    const std::int64_t frames = info.value().lengthFrames;

    // Level 0: min/max per kBaseFramesPerBucket frames, taken across channels
    // (a mono waveform view is what a clip lane draws; per-channel peaks are a
    // documented extension for the sample editor).
    const std::uint32_t framesPerBucket = kBaseFramesPerBucket;
    const auto bucketCount = static_cast<std::uint32_t>((frames + framesPerBucket - 1) / framesPerBucket);
    PeakLevel& base = levels_[0];
    base.framesPerBucket = framesPerBucket;
    base.buckets.assign(bucketCount, PeakBucket{0.0f, 0.0f});
    base.bucketsReady.store(0, std::memory_order_relaxed);

    std::vector<float> interleaved(static_cast<std::size_t>(channels) * kReadFrames);
    std::int64_t position = 0;
    std::uint32_t bucketIndex = 0;
    float bucketMin = 0.0f;
    float bucketMax = 0.0f;
    std::uint32_t inBucket = 0;
    float overallPeak = 0.0f;

    while (position < frames && !cancel_.load(std::memory_order_relaxed)) {
        const int request = static_cast<int>(std::min<std::int64_t>(
            static_cast<std::int64_t>(kReadFrames), frames - position));
        const int read = reader.readInterleaved(position, interleaved.data(), request);
        if (read <= 0)
            break;
        const float* data = interleaved.data();
        for (int i = 0; i < read; ++i) {
            for (int channel = 0; channel < channels; ++channel) {
                const float value = data[static_cast<std::size_t>(i) * channels + static_cast<std::size_t>(channel)];
                bucketMin = std::min(bucketMin, value);
                bucketMax = std::max(bucketMax, value);
                overallPeak = std::max(overallPeak, std::abs(value));
            }
            if (++inBucket >= framesPerBucket) {
                if (bucketIndex < bucketCount) {
                    base.buckets[bucketIndex] = {bucketMin, bucketMax};
                    base.bucketsReady.store(bucketIndex + 1, std::memory_order_release);
                }
                ++bucketIndex;
                inBucket = 0;
                bucketMin = 0.0f;
                bucketMax = 0.0f;
            }
        }
        position += read;
        progress_.store(static_cast<float>(static_cast<double>(position) /
                                           static_cast<double>(std::max<std::int64_t>(1, frames))) *
                            0.8f,
                        std::memory_order_relaxed);
    }
    if (inBucket > 0 && bucketIndex < bucketCount) {
        base.buckets[bucketIndex] = {bucketMin, bucketMax};
        base.bucketsReady.store(bucketIndex + 1, std::memory_order_release);
    }

    std::uint32_t levels = 1;
    // Build coarser levels by halving: each coarser bucket takes the min/max of
    // the two (or fewer) finer buckets it covers. Cost is linear in level 0.
    while (levels < kMaxLevels) {
        const PeakLevel& previous = levels_[levels - 1];
        if (previous.buckets.size() <= 1)
            break;
        PeakLevel& current = levels_[levels];
        current.framesPerBucket = previous.framesPerBucket * 2;
        const std::size_t count = (previous.buckets.size() + 1) / 2;
        current.buckets.assign(count, PeakBucket{0.0f, 0.0f});
        for (std::size_t i = 0; i < count; ++i) {
            float minimum = previous.buckets[i * 2].minimum;
            float maximum = previous.buckets[i * 2].maximum;
            if (i * 2 + 1 < previous.buckets.size()) {
                minimum = std::min(minimum, previous.buckets[i * 2 + 1].minimum);
                maximum = std::max(maximum, previous.buckets[i * 2 + 1].maximum);
            }
            current.buckets[i] = {minimum, maximum};
        }
        current.bucketsReady.store(static_cast<std::uint32_t>(count), std::memory_order_release);
        ++levels;
        progress_.store(0.8f + 0.18f * static_cast<float>(levels) / static_cast<float>(kMaxLevels),
                        std::memory_order_relaxed);
    }

    levelCount_.store(levels, std::memory_order_release);
    overallPeak_.store(overallPeak, std::memory_order_relaxed);
    progress_.store(1.0f, std::memory_order_relaxed);
    complete_.store(true, std::memory_order_release);

    if (!cancel_.load(std::memory_order_relaxed)) {
        const Status saved = saveToCacheFile(cachePathFor(audioPath));
        if (!saved)
            AURA_LOG_DEBUG(kCategory, "Could not write peak cache for %s: %s", audioPath.c_str(),
                           saved.error().toString().c_str());
    }
    AURA_LOG_DEBUG(kCategory, "Built waveform pyramid for %s: %lld frames, %u levels, peak %.3f",
                   audioPath.c_str(), static_cast<long long>(frames), levels, overallPeak);
}

bool WaveformCache::waitForCompletion(std::uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs == 0 ? 3600000 : timeoutMs);
    while (!isComplete()) {
        if (timeoutMs != 0 && std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (worker_.joinable())
        worker_.join();
    return true;
}

WaveformView WaveformCache::viewFor(double samplesPerPixel) const noexcept {
    WaveformView view;
    view.sampleRate = sampleRate_;
    view.totalFrames = totalFrames_;
    const std::uint32_t levels = levelCount_.load(std::memory_order_acquire);
    if (levels == 0)
        return view;

    // Pick the finest level whose buckets are not smaller than a pixel, so the
    // number of buckets we read is bounded by the pixel count.
    std::uint32_t chosen = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        chosen = level;
        if (static_cast<double>(levels_[level].framesPerBucket) >= samplesPerPixel)
            break;
    }
    view.level = &levels_[chosen];
    view.framesPerBucket = levels_[chosen].framesPerBucket;
    view.complete = isComplete();
    return view;
}

bool WaveformCache::readBuckets(const PeakLevel& level, std::uint32_t firstBucket,
                                std::uint32_t count, PeakBucket* out) const noexcept {
    if (!out || level.buckets.empty())
        return false;
    const std::uint32_t ready = level.bucketsReady.load(std::memory_order_acquire);
    const auto limit = static_cast<std::uint32_t>(level.buckets.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index = firstBucket + i;
        if (index >= limit)
            return false;
        if (index < ready) {
            out[i] = level.buckets[index];
        } else {
            // Not built yet: report silence rather than stale memory so the
            // progressive draw degrades gracefully.
            out[i] = PeakBucket{0.0f, 1.0f};
        }
    }
    return true;
}

std::string WaveformCache::cachePathFor(const std::string& audioPath) {
    return audioPath + ".aurapeaks";
}

Status WaveformCache::saveToCacheFile(const std::string& cachePath) const {
    struct Header {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t sampleRate;
        std::uint32_t levelCount;
        std::int64_t totalFrames;
        std::int64_t sourceBytes;
        std::int64_t sourceModifiedMs;
    } header{};

    header.magic = kCacheMagic;
    header.version = kCacheVersion;
    header.sampleRate = sampleRate_;
    header.levelCount = levelCount_.load(std::memory_order_acquire);
    header.totalFrames = totalFrames_;
    header.sourceBytes = static_cast<std::int64_t>(filesystem::fileSize(sourcePath_));
    header.sourceModifiedMs = filesystem::fileModifiedTimeMs(sourcePath_);

    std::vector<std::uint8_t> bytes;
    bytes.resize(sizeof(Header));
    std::memcpy(bytes.data(), &header, sizeof(Header));

    for (std::uint32_t level = 0; level < header.levelCount; ++level) {
        const PeakLevel& peaks = levels_[level];
        const std::uint32_t framesPerBucket = peaks.framesPerBucket;
        const auto count = static_cast<std::uint32_t>(peaks.buckets.size());
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(std::uint32_t) * 2 + peaks.buckets.size() * sizeof(PeakBucket));
        std::memcpy(bytes.data() + offset, &framesPerBucket, sizeof(std::uint32_t));
        std::memcpy(bytes.data() + offset + 4, &count, sizeof(std::uint32_t));
        if (!peaks.buckets.empty()) {
            std::memcpy(bytes.data() + offset + 8, peaks.buckets.data(),
                        peaks.buckets.size() * sizeof(PeakBucket));
        }
    }
    return filesystem::writeBinaryFileAtomic(cachePath, bytes);
}

bool WaveformCache::loadFromCacheFile(const std::string& cachePath, std::int64_t sourceBytes,
                                      std::int64_t sourceModifiedMs) {
    struct Header {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t sampleRate;
        std::uint32_t levelCount;
        std::int64_t totalFrames;
        std::int64_t sourceBytes;
        std::int64_t sourceModifiedMs;
    };

    auto bytes = filesystem::readBinaryFile(cachePath);
    if (!bytes || bytes.value().size() < sizeof(Header))
        return false;

    Header header{};
    std::memcpy(&header, bytes.value().data(), sizeof(Header));
    if (header.magic != kCacheMagic || header.version != kCacheVersion)
        return false;
    // Stale when the source changed: size + modified time is the cheap check.
    if (header.sourceBytes != sourceBytes || header.sourceModifiedMs != sourceModifiedMs)
        return false;
    if (header.levelCount == 0 || header.levelCount > kMaxLevels)
        return false;

    std::size_t offset = sizeof(Header);
    for (std::uint32_t level = 0; level < header.levelCount; ++level) {
        if (offset + 8 > bytes.value().size())
            return false;
        std::uint32_t framesPerBucket = 0;
        std::uint32_t count = 0;
        std::memcpy(&framesPerBucket, bytes.value().data() + offset, 4);
        std::memcpy(&count, bytes.value().data() + offset + 4, 4);
        offset += 8;
        const std::size_t payload = static_cast<std::size_t>(count) * sizeof(PeakBucket);
        if (offset + payload > bytes.value().size())
            return false;
        PeakLevel& current = levels_[level];
        current.framesPerBucket = framesPerBucket;
        current.buckets.resize(count);
        if (payload > 0)
            std::memcpy(current.buckets.data(), bytes.value().data() + offset, payload);
        current.bucketsReady.store(count, std::memory_order_release);
        offset += payload;
    }

    levelCount_.store(header.levelCount, std::memory_order_release);
    totalFrames_ = header.totalFrames;
    sampleRate_ = header.sampleRate;
    float peak = 0.0f;
    if (header.levelCount > 0 && !levels_[0].buckets.empty()) {
        for (const auto& bucket : levels_[0].buckets)
            peak = std::max(peak, std::max(std::abs(bucket.minimum), std::abs(bucket.maximum)));
    }
    overallPeak_.store(peak, std::memory_order_relaxed);
    complete_.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// LivePeakAccumulator (recording)
// ---------------------------------------------------------------------------
void LivePeakAccumulator::reset(std::uint32_t framesPerBucket) noexcept {
    framesPerBucket_ = framesPerBucket == 0 ? 1024 : framesPerBucket;
    framesInBucket_ = 0;
    bucketMin_ = 0.0f;
    bucketMax_ = 0.0f;
    buckets_.clear();
}

void LivePeakAccumulator::process(const float* const* channels, int numChannels,
                                  int frames) noexcept {
    if (!channels || numChannels <= 0 || frames <= 0)
        return;
    // Buckets are appended without reserving: this runs on the audio thread, so
    // the vector is reserved by the caller before recording starts (see
    // RecordingEngine::prepare). If it ever grows, that is a bug the
    // RealtimeSafety test catches.
    for (int i = 0; i < frames; ++i) {
        for (int channel = 0; channel < numChannels; ++channel) {
            const float value = channels[channel][i];
            bucketMin_ = std::min(bucketMin_, value);
            bucketMax_ = std::max(bucketMax_, value);
        }
        if (++framesInBucket_ >= framesPerBucket_) {
            buckets_.push_back({bucketMin_, bucketMax_});
            framesInBucket_ = 0;
            bucketMin_ = 0.0f;
            bucketMax_ = 0.0f;
        }
    }
}

std::vector<PeakBucket> LivePeakAccumulator::takePeaks() const {
    return buckets_;
}

} // namespace aura::audio
