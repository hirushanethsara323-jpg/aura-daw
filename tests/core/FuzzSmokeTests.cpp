// ============================================================================
// AURA DAW - tests/core/FuzzSmokeTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The portable half of fuzzing (issue #3).
//
// The libFuzzer targets in fuzz/ run on Linux/Clang with a sanitizer, for as long
// as CI gives them. That covers one platform and one compiler. This file runs
// everywhere - Windows/MSVC included - and checks the same property with a
// deterministic mutation schedule instead of a coverage-guided search:
//
//     untrusted bytes must yield either a usable object or a reported error.
//
// Never a crash, never a hang, never a resource blow-up. The mutations are seeded
// from the same inputs the fuzzers start from (fuzz/FuzzSupport.hpp), so a bug the
// portable test finds is reproducible in the fuzzer and vice versa.
//
// Why a fixed seed matters: this test must fail for *the same reason* on every
// machine. A random seed would turn "we found a bug" into "we found a bug on CI
// that does not reproduce locally", which is the least useful kind of report.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "aura/core/Json.hpp"
#include "aura/media/AudioFile.hpp"
#include "aura/project/Project.hpp"
#include "fuzz/FuzzSupport.hpp"

using namespace aura;

namespace {

/// A deterministic little mutation engine: bit flips, byte substitutions, chunk
/// deletions, duplications, truncations and splices. Deliberately simple - the
/// goal is breadth of damage, not cleverness, and every mutation must be
/// reproducible from the round number.
class Mutator {
public:
    explicit Mutator(std::uint32_t seed) : engine_(seed) {}

    [[nodiscard]] std::uint32_t next(std::uint32_t bound) {
        return bound == 0 ? 0 : distribution_(engine_) % bound;
    }

    [[nodiscard]] std::string mutate(const std::string& input) {
        std::string out = input;
        const int rounds = 1 + static_cast<int>(next(4));
        for (int round = 0; round < rounds; ++round) {
            if (out.empty())
                out.push_back(static_cast<char>(next(256)));
            switch (next(7)) {
            case 0: // flip one bit
                out[static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size())))] ^=
                    static_cast<char>(1u << next(8));
                break;
            case 1: // replace a byte
                out[static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size())))] =
                    static_cast<char>(next(256));
                break;
            case 2: { // delete a chunk
                const auto position = static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size())));
                const auto length = static_cast<std::size_t>(1 + next(16));
                out.erase(position, length);
                break;
            }
            case 3: { // duplicate a chunk
                const auto position = static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size())));
                const auto length = static_cast<std::size_t>(1 + next(32));
                out.insert(position, out.substr(position, length));
                break;
            }
            case 4: // truncate
                out.resize(static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size()) + 1)));
                break;
            case 5: // splice a structural character in - the parser's own grammar as a hint
                out.insert(static_cast<std::size_t>(next(static_cast<std::uint32_t>(out.size()) + 1)),
                           std::string(1 + next(8), "\"{[,:}]\\ \t\n"[static_cast<std::size_t>(next(11))]));
                break;
            default:
                out.push_back(static_cast<char>(next(256)));
                break;
            }
        }
        return out;
    }

private:
    std::mt19937 engine_;
    std::uniform_int_distribution<std::uint32_t> distribution_{0, 0xFFFFFFFFu};
};

std::filesystem::path scratchDirectory() {
    auto directory = std::filesystem::temp_directory_path() / "aura-fuzz-smoke";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

} // namespace

TEST_CASE("Mutated JSON is always answered with a value or an error", "[fuzz][json]") {
    // The parser is the first thing every persisted artefact touches, and it reads
    // bytes that came from somewhere else. A rejection is a fine answer; a crash,
    // an assertion or a hang is not.
    Mutator mutator(0xA0EA0001u);
    int parsed = 0;
    int rejected = 0;

    for (const std::string& seed : fuzz::jsonSeeds()) {
        for (int i = 0; i < 200; ++i) {
            const std::string input = i == 0 ? seed : mutator.mutate(seed);
            auto result = json::parse(input);
            if (!result) {
                ++rejected;
                continue;
            }
            ++parsed;

            // Anything that parses must survive its own serialisation and come back.
            const std::string dumped = result.value().dump(false);
            auto reparsed = json::parse(dumped);
            INFO("input: " << input.substr(0, 120));
            REQUIRE(reparsed.hasValue());
            REQUIRE(reparsed.value().dump(false) == dumped);
        }
    }

    INFO("parsed " << parsed << ", rejected " << rejected);
    REQUIRE(parsed > 0); // the mutations must not all be garbage, or this proves nothing
    REQUIRE(rejected > 0);
}

TEST_CASE("Mutated project documents never crash the loader", "[fuzz][project]") {
    // Open, the backup fallback and crash recovery all go through this function
    // with text that arrived from another machine. It must classify, not trust.
    Mutator mutator(0xA0EA0002u);
    int loaded = 0;
    int rejected = 0;

    const std::string seed = fuzz::manifestSeed();
    for (int i = 0; i < 400; ++i) {
        const std::string input = i == 0 ? seed : mutator.mutate(seed);
        auto project = project::Project::deserializeDocument("/nonexistent-aura-fuzz", input);
        if (!project) {
            ++rejected;
            continue;
        }
        ++loaded;

        // A document that loads must be walkable, and must survive a save/load
        // cycle: a loader that half-builds an object, or a serialiser that writes
        // something it cannot read back, is a data-loss bug rather than a crash.
        for (const audio::Track* track : project.value()->mixer().tracks()) {
            REQUIRE(track != nullptr);
            (void)track->clips().size();
            (void)track->sends().size();
        }
        const std::string reserialized = project.value()->serializeDocument();
        auto reloaded =
            project::Project::deserializeDocument("/nonexistent-aura-fuzz", reserialized);
        INFO("input: " << input.substr(0, 120));
        REQUIRE(reloaded.hasValue());
    }

    INFO("loaded " << loaded << ", rejected " << rejected);
    REQUIRE(loaded > 0);
    REQUIRE(rejected > 0);
}

TEST_CASE("Mutated WAV bytes are refused or read within their own bounds", "[fuzz][wav]") {
    // The WAV readers are what a two-hour take, an imported stem or a file from the
    // internet goes through. A header that claims more than the file holds must be
    // caught by the reader: that is the difference between a failed import and
    // reading whatever happens to be next in memory.
    const std::filesystem::path directory = scratchDirectory();
    REQUIRE(!directory.empty());

    Mutator mutator(0xA0EA0003u);
    const std::vector<std::uint8_t> seed = fuzz::wavSeed();
    const std::string seedText(reinterpret_cast<const char*>(seed.data()), seed.size());

    int accepted = 0;
    int refused = 0;
    for (int i = 0; i < 150; ++i) {
        const std::string input = i == 0 ? seedText : mutator.mutate(seedText);
        const auto path = directory / ("mutated-" + std::to_string(i) + ".wav");
        REQUIRE(fuzz::writeFile(path, reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));

        auto probed = media::probeFile(path.string());
        if (probed) {
            ++accepted;
            // Whatever the probe reports must be internally consistent: a file
            // cannot have negative channels, frames or bits per sample.
            REQUIRE(probed.value().numChannels <= 64);
            REQUIRE(probed.value().lengthFrames >= 0);
        } else {
            ++refused;
        }

        auto planar = media::readFileToPlanar(path.string());
        if (planar) {
            ++accepted;
            const auto& channels = *planar.value();
            REQUIRE(!channels.empty());
            const std::size_t frames = channels.front().size();
            for (const auto& channel : channels)
                REQUIRE(channel.size() == frames);
        } else {
            ++refused;
        }
    }

    INFO("accepted " << accepted << ", refused " << refused);
    REQUIRE(accepted > 0);
    REQUIRE(refused > 0);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
