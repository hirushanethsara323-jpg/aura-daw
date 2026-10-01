// ============================================================================
// AURA DAW - fuzz/FuzzSupport.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Shared plumbing for the fuzz targets.
//
// The targets feed *bytes* to code that ships, not to a test copy of it: the
// manifest target calls the same Project::deserializeDocument() that Open and
// recovery use, and the WAV target calls the same two readers the media pool
// uses. Where a production entry point takes a path rather than a buffer (WAV
// reading is stdio-based), the target writes the bytes to a temporary file and
// calls the real function - slower than an in-memory reader would be, but it
// fuzzes the code that actually runs, which is the point.
//
// The invariant every target asserts is the same: **untrusted bytes must produce
// either a usable object or a reported error - never a crash, a hang, an
// assertion, or unbounded work**. That is what makes these files worth fuzzing at
// all: they read data the user did not write (a project from a colleague, a WAV
// from the internet, a file a plug-in dropped).
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace aura::fuzz {

/// Directory the file-based targets write their temporary input to. Inside the
/// build tree by default so a fuzzing session never scribbles in $HOME, and
/// overridable for CI (AURA_FUZZ_TMPDIR).
[[nodiscard]] inline std::filesystem::path tempDirectory() {
    if (const char* fromEnvironment = std::getenv("AURA_FUZZ_TMPDIR");
        fromEnvironment && *fromEnvironment) {
        return std::filesystem::path(fromEnvironment);
    }
    return std::filesystem::path("aura-fuzz-tmp");
}

/// Writes `data` to `path`, creating parent directories. Returns false when the
/// write could not be completed (the caller then skips the iteration rather than
/// treating it as a finding).
inline bool writeFile(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file)
        return false;
    const std::size_t written = size == 0 ? 0 : std::fwrite(data, 1, size, file);
    std::fclose(file);
    return written == size;
}

/// The seeds the deterministic smoke test and the fuzzers start from. Kept here
/// (rather than in a corpus directory only) so the portable test that runs on
/// every platform and the libFuzzer job that runs on Linux exercise the same
/// starting points.
[[nodiscard]] inline std::vector<std::string> jsonSeeds() {
    return {
        R"({"a":1,"b":[true,false,null],"c":{"d":"text"}})",
        R"([1,2,3,{"x":-1.5e10},[],{}])",
        R"({"versionMajor":1,"versionMinor":0,"name":"Session"})",
        R"({"escapes":"\"\\\/\b\f\n\r\t\u00e9\u0041"})",
        R"({"deep":{"deep":{"deep":{"deep":{"deep":{"deep":{"deep":1}}}}}}})",
        R"({})",
        R"([])",
        R"(   {"whitespace" : [ 1 , 2 ] }   )",
        // Deliberately malformed: they must be reported, not tolerated.
        R"({"unterminated": )",
        R"({,})",
        R"({"a":01})",
        R"({"a":1e})",
        R"([)",
        R"("just a string")",
        R"({"nul":"\u0000"})",
    };
}

/// A minimal but valid 24-bit WAV, built in code so the seed cannot drift from
/// the format the writer produces.
[[nodiscard]] inline std::vector<std::uint8_t> wavSeed(std::uint16_t channels = 2,
                                                      std::uint32_t sampleRate = 48000,
                                                      std::uint16_t bits = 24,
                                                      int frames = 16) {
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bits / 8));
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames) * blockAlign;
    const std::uint32_t riffSize = 36 + dataBytes;

    std::vector<std::uint8_t> out;
    const auto put32 = [&out](std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto put16 = [&out](std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto putId = [&out](const char* id) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<std::uint8_t>(id[i]));
    };

    putId("RIFF");
    put32(riffSize);
    putId("WAVE");
    putId("fmt ");
    put32(16);
    put16(bits == 32 ? 3 : 1); // IEEE float vs PCM
    put16(channels);
    put32(sampleRate);
    put32(byteRate);
    put16(blockAlign);
    put16(bits);
    putId("data");
    put32(dataBytes);
    for (std::uint32_t i = 0; i < dataBytes; ++i)
        out.push_back(static_cast<std::uint8_t>((i * 7) & 0xFF));
    return out;
}

/// A minimal project document: the shape `Project::deserializeDocument()` expects,
/// with nothing optional filled in beyond what is needed to be plausible.
[[nodiscard]] inline std::string manifestSeed() {
    return R"({
  "versionMajor": 1,
  "versionMinor": 0,
  "name": "Fuzz Session",
  "sampleRate": 48000.0,
  "tracks": [
    {"id": 1, "kind": "audio", "name": "Audio 1", "volumeDb": 0.0, "pan": 0.0,
     "routing": {"destination": 0, "sends": []}, "inserts": [], "clips": []}
  ],
  "master": {"volumeDb": 0.0},
  "mediaPool": {"items": []},
  "markers": [],
  "tempoMap": {"entries": [{"positionTicks": 0, "bpm": 120.0, "numerator": 4, "denominator": 4}]}
})";
}

} // namespace aura::fuzz
