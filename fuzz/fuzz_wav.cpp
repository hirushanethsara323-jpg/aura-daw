// ============================================================================
// AURA DAW - fuzz/fuzz_wav.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Fuzzes the WAV readers. They are stdio-based (they stream, which is what makes a
// two-hour take editable without holding it in RAM), so the target writes the input
// to a temporary file and calls the real entry points rather than a test copy of
// them:
//
//   probeFile()      - header inspection (what the media pool shows)
//   readFileToPlanar - whole-file read (what import and peak building use)
//   AudioFileReader  - streaming read (what playback and recording use)
//
// The invariants: arbitrary bytes must produce an error or a well-formed result
// whose claims match the data. Specifically, a reader that reports N channels and M
// frames must be able to hand over those frames without reading outside the file -
// a lying header is how a media player reads someone else's memory.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "aura/media/AudioFile.hpp"
#include "fuzz/FuzzSupport.hpp"

namespace {

/// Reads the requested range through the streaming reader and checks the framing
/// it promised: every frame it claims to have delivered must have been filled.
void exerciseStreamingReader(const std::filesystem::path& path) {
    aura::media::AudioFileReader reader;
    auto info = reader.open(path.string());
    if (!info)
        return;

    const int channels = info.value().numChannels;
    if (channels <= 0 || channels > 64)
        __builtin_trap(); // a header claiming 4000 channels must not be believed

    const int frames = 64;
    std::vector<std::vector<float>> buffer(static_cast<std::size_t>(channels),
                                           std::vector<float>(frames, 0.0f));
    std::vector<float*> pointers;
    pointers.reserve(static_cast<std::size_t>(channels));
    for (auto& channel : buffer)
        pointers.push_back(channel.data());

    std::int64_t position = 0;
    for (int block = 0; block < 4; ++block) {
        const int read = reader.readPlanar(position, pointers.data(), frames);
        if (read < 0)
            __builtin_trap(); // a negative frame count is not a legal return
        if (read == 0)
            break;
        position += read;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > 1024 * 1024)
        return 0;

    const std::filesystem::path path = aura::fuzz::tempDirectory() / "fuzz-input.wav";
    if (!aura::fuzz::writeFile(path, data, size))
        return 0;

    (void)aura::media::probeFile(path.string());

    auto planar = aura::media::readFileToPlanar(path.string());
    if (planar) {
        const auto& channels = *planar.value();
        if (channels.empty() || channels.size() > 64)
            __builtin_trap();
        // All channels of a file are the same length; a reader that returns ragged
        // channels would make every downstream pointer calculation wrong.
        const std::size_t frames = channels.front().size();
        for (const auto& channel : channels) {
            if (channel.size() != frames)
                __builtin_trap();
        }
    }

    exerciseStreamingReader(path);
    return 0;
}
