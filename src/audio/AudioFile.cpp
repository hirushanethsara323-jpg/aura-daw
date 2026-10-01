// ============================================================================
// AURA DAW - src/audio/AudioFile.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WAV (RIFF/WAVE) reader and writer.
//
// Supported on read:
//   * PCM 16 / 24 / 32-bit integer
//   * IEEE float 32 / 64-bit
//   * WAVE_FORMAT_EXTENSIBLE (0xFFFE) with the PCM/float sub-format GUID
//   * RF64 (> 4 GB) files - detected and read; writing RF64 is a documented
//     roadmap item (exports are split at 4 GB for now, see docs/PROJECT_FORMAT.md)
//
// Everything else is rejected with a clear UnsupportedFormat error rather than
// guessed at. Chunk parsing is declarative so unknown chunks (LIST/bext/iXML)
// are skipped safely.
// ============================================================================
#include "aura/media/AudioFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Math.hpp"
#include "aura/core/Strings.hpp"

namespace aura::media {
namespace {

constexpr const char* kCategory = "Media.AudioFile";

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

#pragma pack(push, 1)
struct RiffHeader {
    char riff[4];
    std::uint32_t size;
    char wave[4];
};

struct ChunkHeader {
    char id[4];
    std::uint32_t size;
};

struct FormatChunk {
    std::uint16_t formatTag;
    std::uint16_t channels;
    std::uint32_t sampleRate;
    std::uint32_t byteRate;
    std::uint16_t blockAlign;
    std::uint16_t bitsPerSample;
};

struct ExtensibleFormatChunk {
    FormatChunk base;
    std::uint16_t extraSize;
    std::uint16_t validBits;
    std::uint32_t channelMask;
    std::uint8_t subFormat[16];
};
#pragma pack(pop)

/// Sub-format GUIDs from the WAVE_FORMAT_EXTENSIBLE specification.
constexpr std::uint8_t kGuidPcmPrefix[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
constexpr std::uint8_t kGuidFloatPrefix[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                               0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

bool matches(const char id[4], const char* expected) noexcept {
    return std::memcmp(id, expected, 4) == 0;
}

bool chunkEquals(const ChunkHeader& chunk, const char* id) noexcept {
    return std::memcmp(chunk.id, id, 4) == 0;
}

std::uint16_t readU16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeU16(std::uint8_t* p, std::uint16_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value & 0xFF);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void writeU32(std::uint8_t* p, std::uint32_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value & 0xFF);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

} // namespace

const char* sampleFormatName(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::PcmInt16: return "PCM 16-bit";
    case SampleFormat::PcmInt24: return "PCM 24-bit";
    case SampleFormat::PcmInt32: return "PCM 32-bit";
    case SampleFormat::Float32: return "IEEE float 32-bit";
    case SampleFormat::Float64: return "IEEE float 64-bit";
    case SampleFormat::Unknown: return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Sample conversion
// ---------------------------------------------------------------------------
float pcm16ToFloat(std::int16_t value) noexcept {
    return static_cast<float>(value) * (1.0f / 32768.0f);
}
float pcm24ToFloat(std::uint32_t value) noexcept {
    // Sign-extend the 24-bit two's complement value.
    std::int32_t signedValue = static_cast<std::int32_t>(value << 8) >> 8;
    return static_cast<float>(signedValue) * (1.0f / 8388608.0f);
}
float pcm32ToFloat(std::int32_t value) noexcept {
    return static_cast<float>(value) * (1.0f / 2147483648.0f);
}
std::int16_t floatToPcm16(float value) noexcept {
    const float scaled = std::round(math::clamp(value, -1.0f, 1.0f) * 32767.0f);
    return static_cast<std::int16_t>(scaled);
}
std::uint32_t floatToPcm24(float value) noexcept {
    const float scaled = std::round(math::clamp(value, -1.0f, 1.0f) * 8388607.0f);
    const auto signedValue = static_cast<std::int32_t>(scaled);
    return static_cast<std::uint32_t>(signedValue) & 0x00FFFFFFu;
}
std::int32_t floatToPcm32(float value) noexcept {
    const double scaled = std::round(static_cast<double>(math::clamp(value, -1.0f, 1.0f)) *
                                     2147483647.0);
    return static_cast<std::int32_t>(scaled);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
struct AudioFileReader::Impl {
    std::FILE* file = nullptr;
    std::vector<std::uint8_t> frameBuffer; ///< reused read scratch
};

AudioFileReader::AudioFileReader() : impl_(std::make_unique<Impl>()) {}
AudioFileReader::~AudioFileReader() {
    close();
}

void AudioFileReader::close() noexcept {
    if (impl_ && impl_->file) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
    info_ = {};
}

bool AudioFileReader::isOpen() const noexcept {
    return impl_ && impl_->file != nullptr;
}

AuraResult<AudioFileInfo> AudioFileReader::open(const std::string& path) {
    close();

    if (!filesystem::isRegularFile(path))
        return failure(makeError(ErrorCode::NotFound, "Audio file does not exist: " + path, path));

    impl_->file = std::fopen(path.c_str(), "rb");
    if (!impl_->file)
        return failure(makeError(ErrorCode::IoError, "Could not open audio file: " + path, path));

    std::uint8_t header[12];
    if (std::fread(header, 1, sizeof(header), impl_->file) != sizeof(header)) {
        close();
        return failure(makeError(ErrorCode::ParseError, "File is too short to be a WAV file: " + path));
    }

    const std::string riff(reinterpret_cast<const char*>(header), 4);
    const std::string wave(reinterpret_cast<const char*>(header + 8), 4);
    if (riff != "RIFF" && riff != "RF64") {
        close();
        return failure(makeError(ErrorCode::UnsupportedFormat,
                                 "Not a RIFF/WAVE file: " + path,
                                 "first four bytes: " + riff));
    }
    if (wave != "WAVE") {
        close();
        return failure(makeError(ErrorCode::ParseError, "RIFF file is not WAVE: " + path));
    }

    AudioFileInfo info;
    info.path = path;
    info.isRf64 = (riff == "RF64");

    bool haveFormat = false;
    std::uint64_t rf64DataSize = 0;

    // Walk chunks until we find `fmt ` and `data`.
    while (true) {
        std::uint8_t chunkHeader[8];
        if (std::fread(chunkHeader, 1, sizeof(chunkHeader), impl_->file) != sizeof(chunkHeader))
            break;

        ChunkHeader chunk{};
        std::memcpy(chunk.id, chunkHeader, 4);
        chunk.size = readU32(chunkHeader + 4);

        const long chunkStart = std::ftell(impl_->file);
        if (chunkStart < 0)
            break;

        if (chunkEquals(chunk, "ds64")) {
            // RF64: 64-bit sizes follow the standard 32-bit size field.
            std::vector<std::uint8_t> payload(chunk.size < 4096 ? chunk.size : 4096);
            if (std::fread(payload.data(), 1, payload.size(), impl_->file) == payload.size() &&
                payload.size() >= 16) {
                rf64DataSize = static_cast<std::uint64_t>(readU32(payload.data() + 8)) |
                               (static_cast<std::uint64_t>(readU32(payload.data() + 12)) << 32);
            }
        } else if (chunkEquals(chunk, "fmt ")) {
            if (chunk.size < 16 || chunk.size > 4096) {
                close();
                return failure(makeError(ErrorCode::ParseError, "Invalid fmt chunk size"));
            }
            std::vector<std::uint8_t> payload(chunk.size);
            if (std::fread(payload.data(), 1, payload.size(), impl_->file) != payload.size()) {
                close();
                return failure(makeError(ErrorCode::ParseError, "Truncated fmt chunk"));
            }
            const std::uint16_t formatTag = readU16(payload.data());
            info.numChannels = readU16(payload.data() + 2);
            info.sampleRate = readU32(payload.data() + 4);
            info.bitsPerSample = readU16(payload.data() + 14);

            std::uint16_t effectiveTag = formatTag;
            if (formatTag == kFormatExtensible && payload.size() >= 40) {
                info.channelMask = readU32(payload.data() + 20);
                const std::uint8_t* guid = payload.data() + 24;
                if (std::memcmp(guid, kGuidPcmPrefix, 16) == 0)
                    effectiveTag = kFormatPcm;
                else if (std::memcmp(guid, kGuidFloatPrefix, 16) == 0)
                    effectiveTag = kFormatFloat;
            }

            switch (effectiveTag) {
            case kFormatPcm:
                if (info.bitsPerSample == 16)
                    info.format = SampleFormat::PcmInt16;
                else if (info.bitsPerSample == 24)
                    info.format = SampleFormat::PcmInt24;
                else if (info.bitsPerSample == 32)
                    info.format = SampleFormat::PcmInt32;
                else {
                    close();
                    return failure(makeError(ErrorCode::UnsupportedFormat,
                                             "Unsupported PCM bit depth",
                                             std::to_string(info.bitsPerSample) + " bits"));
                }
                info.codecName = "PCM";
                break;
            case kFormatFloat:
                if (info.bitsPerSample == 32) {
                    info.format = SampleFormat::Float32;
                } else if (info.bitsPerSample == 64) {
                    info.format = SampleFormat::Float64;
                } else {
                    close();
                    return failure(makeError(ErrorCode::UnsupportedFormat,
                                             "Unsupported float bit depth"));
                }
                info.isFloat = true;
                info.codecName = "IEEE float";
                break;
            default:
                close();
                return failure(makeError(
                    ErrorCode::UnsupportedFormat,
                    "Compressed WAV formats are not supported yet (found tag " +
                        std::to_string(formatTag) + ")",
                    path));
            }
            haveFormat = true;
        } else if (chunkEquals(chunk, "data")) {
            if (!haveFormat) {
                close();
                return failure(makeError(ErrorCode::ParseError, "data chunk before fmt chunk"));
            }
            info.dataOffset = static_cast<std::int64_t>(chunkStart);
            info.dataBytes = (chunk.size == 0xFFFFFFFFu && rf64DataSize > 0)
                                 ? static_cast<std::uint32_t>(std::min<std::uint64_t>(rf64DataSize, 0xFFFFFFFFu))
                                 : chunk.size;
            const int bytesPerFrame = info.numChannels * (info.bitsPerSample / 8);
            info.lengthFrames = bytesPerFrame > 0 ? static_cast<std::int64_t>(info.dataBytes / static_cast<std::uint32_t>(bytesPerFrame)) : 0;
            break; // data found: done with the header walk
        }

        // Skip to the next chunk (chunks are word aligned).
        std::uint32_t advance = chunk.size + (chunk.size & 1u);
        if (std::fseek(impl_->file, chunkStart + static_cast<long>(advance), SEEK_SET) != 0)
            break;
    }

    if (!haveFormat || info.dataOffset == 0) {
        close();
        return failure(makeError(ErrorCode::ParseError,
                                 "WAV file is missing a fmt or data chunk: " + path));
    }
    if (info.numChannels == 0 || info.numChannels > 64) {
        close();
        return failure(makeError(ErrorCode::ParseError, "Implausible channel count",
                                 std::to_string(info.numChannels)));
    }
    if (!info.isValid()) {
        close();
        return failure(makeError(ErrorCode::ParseError, "Invalid audio file header: " + path));
    }

    info_ = info;
    return success(info_);
}

int AudioFileReader::readPlanar(std::int64_t startFrame, float* const* destinations, int frames) {
    if (!isOpen() || frames <= 0 || startFrame < 0)
        return 0;
    const std::int64_t available = info_.lengthFrames - startFrame;
    if (available <= 0)
        return 0;
    frames = static_cast<int>(std::min<std::int64_t>(frames, available));

    const int channels = info_.numChannels;
    const int bytesPerSample = info_.bitsPerSample / 8;
    const std::size_t interleavedFloats = static_cast<std::size_t>(frames) * channels;
    if (impl_->frameBuffer.size() < interleavedFloats * static_cast<std::size_t>(bytesPerSample))
        impl_->frameBuffer.resize(interleavedFloats * static_cast<std::size_t>(bytesPerSample));

    const std::int64_t byteOffset =
        info_.dataOffset + startFrame * channels * bytesPerSample;
    if (std::fseek(impl_->file, static_cast<long>(byteOffset), SEEK_SET) != 0)
        return 0;

    const std::size_t bytesToRead = interleavedFloats * static_cast<std::size_t>(bytesPerSample);
    const std::size_t bytesRead = std::fread(impl_->frameBuffer.data(), 1, bytesToRead, impl_->file);
    const int framesRead = static_cast<int>(bytesRead / (static_cast<std::size_t>(channels) *
                                                         static_cast<std::size_t>(bytesPerSample)));

    const std::uint8_t* source = impl_->frameBuffer.data();
    for (int i = 0; i < framesRead; ++i) {
        for (int channel = 0; channel < channels; ++channel) {
            const std::size_t index = static_cast<std::size_t>(i) * channels + static_cast<std::size_t>(channel);
            const std::uint8_t* sample = source + index * static_cast<std::size_t>(bytesPerSample);
            float value = 0.0f;
            switch (info_.format) {
            case SampleFormat::PcmInt16:
                value = pcm16ToFloat(static_cast<std::int16_t>(readU16(sample)));
                break;
            case SampleFormat::PcmInt24: {
                const std::uint32_t packed = static_cast<std::uint32_t>(sample[0]) |
                                             (static_cast<std::uint32_t>(sample[1]) << 8) |
                                             (static_cast<std::uint32_t>(sample[2]) << 16);
                value = pcm24ToFloat(packed);
                break;
            }
            case SampleFormat::PcmInt32:
                value = pcm32ToFloat(static_cast<std::int32_t>(readU32(sample)));
                break;
            case SampleFormat::Float32: {
                float f;
                std::memcpy(&f, sample, sizeof(float));
                value = std::isfinite(f) ? f : 0.0f;
                break;
            }
            case SampleFormat::Float64: {
                double d;
                std::memcpy(&d, sample, sizeof(double));
                value = std::isfinite(d) ? static_cast<float>(d) : 0.0f;
                break;
            }
            case SampleFormat::Unknown: break;
            }
            destinations[channel][i] = value;
        }
    }
    return framesRead;
}

int AudioFileReader::readInterleaved(std::int64_t startFrame, float* destination, int frames) {
    if (!isOpen() || frames <= 0)
        return 0;
    const int channels = info_.numChannels;
    std::vector<float*> pointers(static_cast<std::size_t>(channels));
    // Read into the caller's interleaved buffer by viewing it as planar with a
    // stride: simplest is a temporary planar read then interleave.
    std::vector<std::vector<float>> planar(static_cast<std::size_t>(channels),
                                           std::vector<float>(static_cast<std::size_t>(frames)));
    for (int channel = 0; channel < channels; ++channel)
        pointers[static_cast<std::size_t>(channel)] = planar[static_cast<std::size_t>(channel)].data();
    const int framesRead = readPlanar(startFrame, pointers.data(), frames);
    for (int i = 0; i < framesRead; ++i) {
        for (int channel = 0; channel < channels; ++channel) {
            destination[static_cast<std::size_t>(i) * channels + static_cast<std::size_t>(channel)] =
                planar[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)];
        }
    }
    return framesRead;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
struct AudioFileWriter::Impl {
    std::FILE* file = nullptr;
    std::vector<std::uint8_t> encodeBuffer;
};

AudioFileWriter::AudioFileWriter() : impl_(std::make_unique<Impl>()) {}
AudioFileWriter::~AudioFileWriter() {
    (void)close();
}

bool AudioFileWriter::isOpen() const noexcept {
    return impl_ && impl_->file != nullptr;
}

std::int64_t AudioFileWriter::framesWritten() const noexcept {
    return frames_;
}

AuraResult<std::uint64_t> AudioFileWriter::create(const std::string& path, const Settings& settings) {
    (void)close();

    const Status dirs = filesystem::createDirectories(filesystem::fs::path(path).parent_path());
    if (!dirs)
        return failure(dirs.error());

    impl_->file = std::fopen(path.c_str(), "wb");
    if (!impl_->file)
        return failure(makeError(ErrorCode::IoError, "Could not create audio file: " + path, path));

    settings_ = settings;
    path_ = path;
    frames_ = 0;

    const std::uint16_t bits = settings.bitsPerSample;
    const bool isFloat = settings.format == SampleFormat::Float32 ||
                         settings.format == SampleFormat::Float64;
    const std::uint16_t formatTag = isFloat ? kFormatFloat : kFormatPcm;

    std::uint8_t header[44]{};
    std::memcpy(header, "RIFF", 4);
    writeU32(header + 4, 36); // patched on close
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    writeU32(header + 16, 16);
    writeU16(header + 20, formatTag);
    writeU16(header + 22, settings.numChannels);
    writeU32(header + 24, settings.sampleRate);
    const std::uint32_t bytesPerFrame =
        static_cast<std::uint32_t>(settings.numChannels) * static_cast<std::uint32_t>(bits / 8);
    writeU32(header + 28, settings.sampleRate * bytesPerFrame);
    writeU16(header + 32, static_cast<std::uint16_t>(bytesPerFrame));
    writeU16(header + 34, bits);
    std::memcpy(header + 36, "data", 4);
    writeU32(header + 40, 0); // patched on close

    if (std::fwrite(header, 1, sizeof(header), impl_->file) != sizeof(header)) {
        (void)close();
        return failure(makeError(ErrorCode::IoError, "Could not write WAV header: " + path));
    }
    (void)std::fflush(impl_->file);
    return success<std::uint64_t>(44);
}

AuraResult<int> AudioFileWriter::writeInterleaved(const float* source, int frames) {
    if (!isOpen())
        return failure(makeError(ErrorCode::IoError, "Audio file is not open for writing"));
    if (frames <= 0)
        return success(0);

    const int channels = settings_.numChannels;
    const int bytesPerSample = settings_.bitsPerSample / 8;
    const std::size_t sampleCount = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
    const std::size_t bytes = sampleCount * static_cast<std::size_t>(bytesPerSample);
    if (impl_->encodeBuffer.size() < bytes)
        impl_->encodeBuffer.resize(bytes);

    std::uint8_t* destination = impl_->encodeBuffer.data();
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const float value = source[i];
        switch (settings_.format) {
        case SampleFormat::PcmInt16:
            writeU16(destination, static_cast<std::uint16_t>(floatToPcm16(value)));
            destination += 2;
            break;
        case SampleFormat::PcmInt24: {
            const std::uint32_t packed = floatToPcm24(value);
            destination[0] = static_cast<std::uint8_t>(packed & 0xFF);
            destination[1] = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
            destination[2] = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
            destination += 3;
            break;
        }
        case SampleFormat::PcmInt32:
            writeU32(destination, static_cast<std::uint32_t>(floatToPcm32(value)));
            destination += 4;
            break;
        case SampleFormat::Float32:
            std::memcpy(destination, &value, sizeof(float));
            destination += 4;
            break;
        case SampleFormat::Float64: {
            const double wide = static_cast<double>(value);
            std::memcpy(destination, &wide, sizeof(double));
            destination += 8;
            break;
        }
        case SampleFormat::Unknown: break;
        }
    }

    if (std::fwrite(impl_->encodeBuffer.data(), 1, bytes, impl_->file) != bytes)
        return failure(makeError(ErrorCode::DiskFull, "Could not write audio data (disk full?)",
                                 path_));

    frames_ += frames;
    return success(frames);
}

Status AudioFileWriter::flush() {
    if (!isOpen())
        return failure(makeError(ErrorCode::IoError, "Audio file is not open"));
    if (std::fflush(impl_->file) != 0)
        return failure(makeError(ErrorCode::DiskFull, "Could not flush audio data", path_));
    return success();
}

Status AudioFileWriter::close() {
    if (!isOpen())
        return success(); // idempotent: crash recovery calls this twice

    const std::uint32_t bytesPerFrame =
        static_cast<std::uint32_t>(settings_.numChannels) * static_cast<std::uint32_t>(settings_.bitsPerSample / 8);
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(frames_) * bytesPerFrame;
    if (dataBytes > 0xFFFFFFFFull - 44ull) {
        // > 4 GB would need RF64; documented limitation, reported honestly rather
        // than writing a corrupt header.
        (void)std::fclose(impl_->file);
        impl_->file = nullptr;
        return failure(makeError(ErrorCode::UnsupportedFormat,
                                 "Recording exceeded the 4 GB WAV limit; RF64 writing is not "
                                 "implemented yet (see docs/PROJECT_FORMAT.md)",
                                 path_));
    }

    std::uint8_t patch[8];
    if (std::fseek(impl_->file, 4, SEEK_SET) == 0) {
        writeU32(patch, static_cast<std::uint32_t>(36 + dataBytes));
        (void)std::fwrite(patch, 1, 4, impl_->file);
    }
    if (std::fseek(impl_->file, 40, SEEK_SET) == 0) {
        writeU32(patch, static_cast<std::uint32_t>(dataBytes));
        (void)std::fwrite(patch, 1, 4, impl_->file);
    }

    const int result = std::fclose(impl_->file);
    impl_->file = nullptr;
    if (result != 0)
        return failure(makeError(ErrorCode::IoError, "Could not finalise audio file", path_));
    return success();
}

// ---------------------------------------------------------------------------
// Convenience helpers
// ---------------------------------------------------------------------------
AuraResult<std::shared_ptr<std::vector<std::vector<float>>>> readFileToPlanar(
    const std::string& path, AudioFileInfo* outInfo) {
    AudioFileReader reader;
    auto info = reader.open(path);
    if (!info)
        return failure(info.error());
    if (outInfo)
        *outInfo = info.value();

    const int channels = info.value().numChannels;
    const std::int64_t frames = info.value().lengthFrames;
    auto buffers = std::make_shared<std::vector<std::vector<float>>>();
    buffers->resize(static_cast<std::size_t>(channels));
    for (auto& channel : *buffers)
        channel.resize(static_cast<std::size_t>(frames));

    std::vector<float*> pointers(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
        pointers[static_cast<std::size_t>(channel)] = (*buffers)[static_cast<std::size_t>(channel)].data();

    // readPlanar() fills from the start of each destination buffer, so the
    // per-chunk pointers have to be advanced by the number of frames already
    // read. Passing the base pointers every time (the obvious-looking version)
    // makes chunk 2 overwrite chunk 1 - which silently destroys audio whenever
    // the later chunks happen to be quieter, and is invisible in short files.
    std::int64_t position = 0;
    std::vector<float*> chunkPointers(static_cast<std::size_t>(channels));
    while (position < frames) {
        const int chunk = static_cast<int>(std::min<std::int64_t>(65536, frames - position));
        for (int channel = 0; channel < channels; ++channel)
            chunkPointers[static_cast<std::size_t>(channel)] =
                pointers[static_cast<std::size_t>(channel)] + position;
        const int read = reader.readPlanar(position, chunkPointers.data(), chunk);
        if (read <= 0)
            break;
        position += read;
    }
    return success(buffers);
}

Status writePlanarToFile(const std::string& path, const std::vector<std::vector<float>>& channels,
                         std::uint32_t sampleRate, const AudioFileWriter::Settings& settingsIn) {
    AudioFileWriter::Settings settings = settingsIn;
    settings.sampleRate = sampleRate;
    settings.numChannels = static_cast<std::uint16_t>(channels.size());
    if (settings.format == SampleFormat::Unknown)
        settings.format = SampleFormat::PcmInt24;

    AudioFileWriter writer;
    const Status created = [&] {
        auto result = writer.create(path, settings);
        if (!result)
            return failure(result.error());
        return success();
    }();
    if (!created)
        return created;

    const std::size_t frames = channels.empty() ? 0 : channels[0].size();
    std::vector<float> interleaved(static_cast<std::size_t>(settings.numChannels) * frames);
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        for (std::size_t i = 0; i < frames; ++i)
            interleaved[i * channels.size() + channel] = channels[channel][i];
    }

    const int blockSize = 16384;
    std::size_t position = 0;
    while (position < frames) {
        const int block = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(blockSize), frames - position));
        auto written = writer.writeInterleaved(interleaved.data() + position * channels.size(), block);
        if (!written)
            return failure(written.error());
        position += static_cast<std::size_t>(block);
    }
    return writer.close();
}

AuraResult<AudioFileInfo> probeFile(const std::string& path) {
    AudioFileReader reader;
    auto result = reader.open(path);
    if (!result)
        return failure(result.error());
    return success(result.value());
}

bool isSupportedAudioExtension(const std::string& extension) {
    static const std::vector<std::string> supported{"wav", "wave", "bwf"};
    const std::string lower = strings::toLower(extension);
    return std::find(supported.begin(), supported.end(), lower) != supported.end();
}

std::vector<std::string> supportedAudioExtensions() {
    return {"wav", "wave", "bwf"};
}

} // namespace aura::media
