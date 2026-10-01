// ============================================================================
// AURA DAW - media/AudioFile.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio file IO. WAV (PCM + IEEE float, 16/24/32-bit) is the format AURA
// guarantees, because it is the only one needed for lossless recording,
// consolidation and export. Other formats are read through optional codecs
// (see MEDIA_RESEARCH.md): the reader interface is codec-shaped so FLAC/MP3/
// OGG support can be added without touching callers.
//
// Design notes:
//   * The reader is seekable and streaming: it never loads a whole file.
//   * 24-bit is unpacked to float in the reader; internal processing is always
//     32-bit float (documented in ARCHITECTURE.md).
//   * A declarative chunk walk means unknown chunks (LIST, iXML, bext,
//     cue points) are skipped safely rather than corrupting the parse.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::media {

enum class SampleFormat : std::int32_t {
    PcmInt16 = 0,
    PcmInt24,
    PcmInt32,
    Float32,
    Float64,
    Unknown,
};

const char* sampleFormatName(SampleFormat format) noexcept;

struct AudioFileInfo {
    std::string path;
    std::uint32_t sampleRate = 48000;
    std::uint16_t numChannels = 2;
    std::uint16_t bitsPerSample = 24;
    SampleFormat format = SampleFormat::PcmInt24;
    std::int64_t lengthFrames = 0;      ///< total frames in the data chunk
    std::int64_t dataOffset = 0;        ///< byte offset of audio data
    std::uint32_t dataBytes = 0;
    bool isFloat = false;
    bool isRf64 = false;                ///< WAVE_FORMAT_EXTENSIBLE wrapper
    /// The data chunk claims more bytes than the file holds. That is what an
    /// interrupted recording looks like: the frames that were written are real and
    /// playable, so the file opens with the length it actually has - it is only the
    /// *claim* that is wrong. Reported so a caller can say so.
    bool truncated = false;
    std::uint32_t channelMask = 0;
    std::string codecName = "PCM";
    bool isValid() const noexcept { return sampleRate > 0 && numChannels > 0 && lengthFrames >= 0; }
    [[nodiscard]] double lengthSeconds() const noexcept {
        return sampleRate ? static_cast<double>(lengthFrames) / sampleRate : 0.0;
    }
};

/// Streaming, seekable reader. Not thread-safe: one reader per thread.
class AudioFileReader {
public:
    AudioFileReader();
    ~AudioFileReader();
    AudioFileReader(const AudioFileReader&) = delete;
    AudioFileReader& operator=(const AudioFileReader&) = delete;

    /// Opens a file and parses its header. Control thread only.
    AuraResult<AudioFileInfo> open(const std::string& path);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const AudioFileInfo& info() const noexcept { return info_; }

    /// Reads up to `frames` frames from `startFrame` into planar float buffers,
    /// converting integer PCM to float in [-1, 1). Returns frames read.
    /// (Not for the audio thread: DiskStream wraps this in a worker thread.)
    int readPlanar(std::int64_t startFrame, float* const* destinations, int frames);

    /// Reads interleaved (used by the offline renderer and the peak builder).
    int readInterleaved(std::int64_t startFrame, float* destination, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AudioFileInfo info_{};
};

/// Streaming writer for WAV output (recording, consolidation, export).
class AudioFileWriter {
public:
    AudioFileWriter();
    ~AudioFileWriter();
    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator=(const AudioFileWriter&) = delete;

    struct Settings {
        std::uint32_t sampleRate = 48000;
        std::uint16_t numChannels = 2;
        std::uint16_t bitsPerSample = 24;
        SampleFormat format = SampleFormat::PcmInt24;
        /// Writes a placeholder header immediately and patches sizes on close,
        /// so an interrupted recording is still a readable WAV file. This is
        /// the crash-recovery guarantee described in docs/PROJECT_FORMAT.md.
        bool patchHeaderOnClose = true;
    };

    AuraResult<std::uint64_t> create(const std::string& path, const Settings& settings);
    /// Appends interleaved float samples (converted to the file format).
    /// Returns frames written, or an error on disk failure.
    AuraResult<int> writeInterleaved(const float* source, int frames);
    /// Flushes buffered data; safe to call periodically (recording durability).
    Status flush();
    /// Finalises the header and closes. IDEMPOTENT: calling twice is safe,
    /// which the crash-recovery path relies on.
    Status close();
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::int64_t framesWritten() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    Settings settings_{};
    std::int64_t frames_ = 0;
};

/// Reads a whole file into planar float buffers (used by tests, short-clip
/// import and the offline renderer for small files).
AuraResult<std::shared_ptr<std::vector<std::vector<float>>>> readFileToPlanar(
    const std::string& path, AudioFileInfo* outInfo = nullptr);

/// Writes planar buffers to a WAV file (tests, export helpers).
Status writePlanarToFile(const std::string& path, const std::vector<std::vector<float>>& channels,
                         std::uint32_t sampleRate, const AudioFileWriter::Settings& settings);

/// Probes a file's format without opening it for reading (metadata-only path
/// used by the media pool).
AuraResult<AudioFileInfo> probeFile(const std::string& path);

/// True when the extension is a format AURA can currently read.
bool isSupportedAudioExtension(const std::string& extension);

/// List of extensions AURA can read (for the file browser filter).
std::vector<std::string> supportedAudioExtensions();

// ---------------------------------------------------------------------------
// Sample format conversion helpers (shared with the export path).
// ---------------------------------------------------------------------------
float pcm16ToFloat(std::int16_t value) noexcept;
float pcm24ToFloat(std::uint32_t value) noexcept; ///< 24-bit little-endian in low bytes
float pcm32ToFloat(std::int32_t value) noexcept;
std::int16_t floatToPcm16(float value) noexcept;
std::uint32_t floatToPcm24(float value) noexcept;
std::int32_t floatToPcm32(float value) noexcept;

} // namespace aura::media
