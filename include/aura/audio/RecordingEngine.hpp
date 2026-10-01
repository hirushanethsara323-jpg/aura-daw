// ============================================================================
// AURA DAW - audio/RecordingEngine.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording. Design constraints that shape this class:
//
//   * The audio thread must never write to disk. It pushes interleaved frames
//     into a lock-free ring; a disk-writer thread drains it into a WAV file.
//   * A crash or power loss must not destroy the take. The writer patches the
//     WAV header on close, and every recording is first written to a temporary
//     file in `recordings/incomplete/` and only moved into place once finalised
//     (see docs/PROJECT_FORMAT.md "crash recovery").
//   * Disk space is monitored while recording; running out stops the recording
//     cleanly with the audio preserved rather than failing mid-write.
//   * Input monitoring and punch-in are transport concerns (Transport.hpp).
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
#include "aura/time/Time.hpp"

namespace aura::audio {

struct RecordingSettings {
    std::string outputDirectory;      ///< final destination for finished takes
    std::string temporaryDirectory;   ///< used while recording (crash recovery)
    int numChannels = 2;
    std::uint32_t sampleRate = 48000;
    std::uint16_t bitsPerSample = 24; ///< 24-bit is the default per the brief
    media::SampleFormat format = media::SampleFormat::PcmInt24;
    /// Flush interval: shorter = safer against power loss, more disk activity.
    double flushIntervalSeconds = 2.0;
    /// Stop recording when free space drops below this (MB).
    std::uint64_t minimumFreeMegabytes = 512;
    std::string filePrefix = "Take";
    bool writePeaksWhileRecording = true;
};

/// Per-track recording target.
struct RecordTarget {
    std::string trackName;
    std::vector<int> inputChannels; ///< device channels feeding this target
    bool armed = false;
};

/// One realisation of a recording: the writer plus counters.
class RecordingEngine {
public:
    RecordingEngine();
    ~RecordingEngine();

    void setSettings(const RecordingSettings& settings) { settings_ = settings; }
    [[nodiscard]] const RecordingSettings& settings() const noexcept { return settings_; }

    void setTargets(std::vector<RecordTarget> targets) { targets_ = std::move(targets); }
    [[nodiscard]] const std::vector<RecordTarget>& targets() const noexcept { return targets_; }

    /// Prepares buffers/ring for the current block size. Control thread.
    void prepare(std::size_t ringFrames = 1u << 18);

    /// Starts a take. Creates the temporary file immediately so that even a
    /// crash one block later leaves a readable WAV. Control thread.
    Status startTake(time::SamplePos position, const std::string& nameHint);

    /// Audio thread: pushes one block of input into the record ring.
    void processInput(const float* const* inputChannels, int numInputChannels, int frames) noexcept;

    /// Stops the take, finalises the file and returns its path. Control thread.
    AuraResult<std::string> stopTake(const std::string& finalName = {});

    [[nodiscard]] bool isRecording() const noexcept {
        return recording_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::int64_t recordedFrames() const noexcept {
        return recordedFrames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t startPosition() const noexcept { return startPosition_; }
    [[nodiscard]] std::string currentTemporaryPath() const { return temporaryPath_; }

    /// Frames dropped because the writer could not keep up (diagnostics: an
    /// increase here means the disk is too slow for the chosen buffer size).
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept {
        return droppedFrames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t diskFreeBytes() const noexcept {
        return diskFreeBytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool diskSpaceLow() const noexcept {
        return diskSpaceLow_.load(std::memory_order_relaxed);
    }

    /// Recovers takes left behind by a crash. Returns the paths of readable
    /// files found in the temporary directory.
    std::vector<std::string> recoverIncompleteTakes() const;

private:
    void writerLoop();

    RecordingSettings settings_{};
    std::vector<RecordTarget> targets_;

    AudioRingBuffer ring_;
    std::thread writerThread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> recording_{false};
    std::atomic<std::int64_t> recordedFrames_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint64_t> diskFreeBytes_{0};
    std::atomic<bool> diskSpaceLow_{false};

    time::SamplePos startPosition_ = 0;
    std::string temporaryPath_;
    std::string finalName_;
    media::AudioFileWriter writer_;
    std::vector<float> interleaveScratch_; ///< writer thread only
    double lastFlushTime_ = 0.0;
};

} // namespace aura::audio
