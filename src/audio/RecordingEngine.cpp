// ============================================================================
// AURA DAW - src/audio/RecordingEngine.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audio/RecordingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

namespace aura::audio {
namespace {
constexpr const char* kCategory = "Audio.Recording";
constexpr int kWriterChunkFrames = 8192;
} // namespace

RecordingEngine::RecordingEngine() = default;

RecordingEngine::~RecordingEngine() {
    if (recording_.load(std::memory_order_acquire)) {
        // AURA is shutting down: finalise the take so nothing is lost.
        (void)stopTake(finalName_);
    }
    quit_.store(true, std::memory_order_relaxed);
    if (writerThread_.joinable())
        writerThread_.join();
}

void RecordingEngine::prepare(std::size_t ringFrames) {
    ring_.prepare(ringFrames, static_cast<std::size_t>(settings_.numChannels));
    interleaveScratch_.assign(static_cast<std::size_t>(settings_.numChannels) * kWriterChunkFrames,
                              0.0f);
}

Status RecordingEngine::startTake(time::SamplePos position, const std::string& nameHint) {
    if (recording_.load(std::memory_order_acquire))
        return failure(makeError(ErrorCode::AlreadyExists, "A recording is already in progress"));

    if (settings_.temporaryDirectory.empty())
        settings_.temporaryDirectory = (filesystem::fs::path(settings_.outputDirectory) / "incomplete").string();
    if (settings_.outputDirectory.empty())
        return failure(makeError(ErrorCode::InvalidArgument, "No recording directory configured"));

    AURA_RETURN_ON_ERROR(filesystem::createDirectories(settings_.temporaryDirectory));
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(settings_.outputDirectory));

    // Disk space gate: never start a take we cannot finish.
    const std::uint64_t freeBytes = filesystem::freeSpaceBytes(settings_.temporaryDirectory);
    diskFreeBytes_.store(freeBytes, std::memory_order_relaxed);
    if (freeBytes > 0 && freeBytes < settings_.minimumFreeMegabytes * 1024ull * 1024ull) {
        return failure(makeError(ErrorCode::DiskFull,
                                 "Not enough free space to start recording",
                                 std::to_string(freeBytes / (1024 * 1024)) + " MB free"));
    }

    const std::string stamp = strings::padLeft(std::to_string(position), 10, '0');
    const std::string baseName =
        strings::trim(nameHint).empty() ? settings_.filePrefix + " " + stamp : nameHint;
    const auto tempPath = filesystem::fs::path(settings_.temporaryDirectory) /
                          (filesystem::sanitizeFileName(baseName) + ".partial.wav");
    temporaryPath_ = tempPath.string();
    finalName_ = baseName;

    media::AudioFileWriter::Settings writerSettings;
    writerSettings.sampleRate = settings_.sampleRate;
    writerSettings.numChannels = static_cast<std::uint16_t>(settings_.numChannels);
    writerSettings.bitsPerSample = settings_.bitsPerSample;
    writerSettings.format = settings_.format;
    auto created = writer_.create(temporaryPath_, writerSettings);
    if (!created)
        return failure(created.error());

    ring_.clear();
    recordedFrames_.store(0, std::memory_order_relaxed);
    droppedFrames_.store(0, std::memory_order_relaxed);
    startPosition_ = position;
    diskSpaceLow_.store(false, std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);

    if (writerThread_.joinable())
        writerThread_.join();
    quit_.store(false, std::memory_order_relaxed);
    writerThread_ = std::thread([this] { writerLoop(); });

    AURA_LOG_INFO(kCategory, "Recording started: %s (%d ch, %u Hz, %u-bit)",
                  temporaryPath_.c_str(), settings_.numChannels, settings_.sampleRate,
                  settings_.bitsPerSample);
    return success();
}

void RecordingEngine::processInput(const float* const* inputChannels, int numInputChannels,
                                   int frames) noexcept {
    if (!recording_.load(std::memory_order_relaxed) || frames <= 0 || numInputChannels <= 0)
        return;

    // Interleave into the scratch-free path: write each channel through the
    // ring one frame at a time is too slow, so we interleave into a small
    // stack buffer in blocks.
    const int channels = settings_.numChannels;
    float block[2 * 512];
    if (channels > 2)
        return; // V1 records stereo; multichannel is a documented extension
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, 512);
        for (int i = 0; i < chunk; ++i) {
            for (int channel = 0; channel < channels; ++channel) {
                const int sourceChannel = std::min(channel, numInputChannels - 1);
                block[i * channels + channel] = inputChannels[sourceChannel][offset + i];
            }
        }
        const std::size_t written = ring_.write(block, static_cast<std::size_t>(chunk));
        if (written < static_cast<std::size_t>(chunk))
            droppedFrames_.fetch_add(static_cast<std::uint64_t>(chunk) - written,
                                     std::memory_order_relaxed);
        recordedFrames_.fetch_add(static_cast<std::int64_t>(written), std::memory_order_relaxed);
        remaining -= chunk;
        offset += chunk;
    }
}

void RecordingEngine::writerLoop() {
    const auto start = std::chrono::steady_clock::now();
    std::int64_t framesSinceFlush = 0;
    bool errorState = false;

    while (!quit_.load(std::memory_order_relaxed) ||
           ring_.availableToRead() > 0) {
        const std::size_t available = ring_.availableToRead();
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const auto toWrite = static_cast<int>(
            std::min<std::size_t>(available, static_cast<std::size_t>(kWriterChunkFrames)));
        const std::size_t read = ring_.read(interleaveScratch_.data(), static_cast<std::size_t>(toWrite));
        if (read == 0)
            continue;

        auto written = writer_.writeInterleaved(interleaveScratch_.data(), static_cast<int>(read));
        if (!written) {
            AURA_LOG_ERROR(kCategory, "Recording write failed: %s",
                           written.error().toString().c_str());
            errorState = true;
            recording_.store(false, std::memory_order_release);
            break;
        }
        framesSinceFlush += static_cast<std::int64_t>(read);

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed - lastFlushTime_ >= settings_.flushIntervalSeconds) {
            lastFlushTime_ = elapsed;
            (void)writer_.flush(); // durability: bounded loss window on power failure
            // Monitor free space while recording.
            const std::uint64_t freeBytes = filesystem::freeSpaceBytes(temporaryPath_);
            diskFreeBytes_.store(freeBytes, std::memory_order_relaxed);
            if (freeBytes > 0 && freeBytes < settings_.minimumFreeMegabytes * 1024ull * 1024ull) {
                AURA_LOG_WARN(kCategory, "Disk space low (%llu MB); stopping recording to protect "
                                         "the take",
                              static_cast<unsigned long long>(freeBytes / (1024 * 1024)));
                diskSpaceLow_.store(true, std::memory_order_relaxed);
                recording_.store(false, std::memory_order_release);
                break;
            }
        }
    }
    (void)framesSinceFlush;
    (void)errorState;
}

AuraResult<std::string> RecordingEngine::stopTake(const std::string& finalName) {
    const bool wasRecording = recording_.exchange(false, std::memory_order_acq_rel);
    quit_.store(true, std::memory_order_relaxed);
    if (writerThread_.joinable())
        writerThread_.join();

    if (temporaryPath_.empty() || !writer_.isOpen()) {
        if (!wasRecording)
            return failure(makeError(ErrorCode::NotFound, "No recording is in progress"));
    }

    // Drain anything still in the ring before closing (final block).
    while (ring_.availableToRead() > 0) {
        const auto toWrite = static_cast<int>(std::min<std::size_t>(
            ring_.availableToRead(), static_cast<std::size_t>(kWriterChunkFrames)));
        const std::size_t read = ring_.read(interleaveScratch_.data(), static_cast<std::size_t>(toWrite));
        if (read == 0)
            break;
        (void)writer_.writeInterleaved(interleaveScratch_.data(), static_cast<int>(read));
    }

    // Reporting the honest limit: if the 4 GB WAV limit was exceeded, close()
    // fails but the audio already on disk stays valid and recoverable.
    const Status closed = writer_.close();
    const std::string takeName = finalName.empty() ? finalName_ : finalName;
    const auto desired = filesystem::fs::path(settings_.outputDirectory) /
                         (filesystem::sanitizeFileName(takeName) + ".wav");
    const auto finalPath = filesystem::makeUniquePath(desired);

    const Status moved = filesystem::renameFile(temporaryPath_, finalPath);
    if (!moved) {
        AURA_LOG_ERROR(kCategory, "Could not move finished take to %s: %s", finalPath.string().c_str(),
                       moved.error().toString().c_str());
        temporaryPath_.clear();
        return failure(moved.error());
    }

    AURA_LOG_INFO(kCategory, "Recording stopped: %s (%lld frames, %llu dropped)",
                  finalPath.string().c_str(), static_cast<long long>(recordedFrames()),
                  static_cast<unsigned long long>(droppedFrames()));
    temporaryPath_.clear();

    if (!closed) {
        // The file is on disk and playable up to the 4 GB limit; surface the
        // reason so the UI can explain it instead of pretending all is well.
        return AuraResult<std::string>(closed.error());
    }
    return success(finalPath.string());
}

std::vector<std::string> RecordingEngine::recoverIncompleteTakes() const {
    std::vector<std::string> recovered;
    if (settings_.temporaryDirectory.empty() ||
        !filesystem::isDirectory(settings_.temporaryDirectory))
        return recovered;

    for (const auto& file : filesystem::listFiles(settings_.temporaryDirectory, {"wav"})) {
        // Any .partial.wav left behind means AURA (or the machine) died mid-take.
        // The header was patched by the writer's periodic flush, so the file is
        // readable; we move it next to the finished recordings.
        const std::string name = file.filename().string();
        if (name.find(".partial") == std::string::npos)
            continue;
        const auto destination = filesystem::makeUniquePath(
            filesystem::fs::path(settings_.outputDirectory) /
            ("Recovered " + strings::replaceAll(name, ".partial", "")));
        if (filesystem::renameFile(file, destination)) {
            recovered.push_back(destination.string());
            AURA_LOG_INFO(kCategory, "Recovered interrupted recording: %s",
                          destination.string().c_str());
        }
    }
    return recovered;
}

} // namespace aura::audio
