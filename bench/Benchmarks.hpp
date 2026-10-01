// ============================================================================
// AURA DAW - bench/Benchmarks.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The measurement harness for bench/. Dependency-free on purpose: the project
// policy is that a new dependency must earn its place (ADR-0011), and a
// benchmark runner is ~150 lines. Google Benchmark would bring a build
// dependency, its own CMake logic and its own statistics model for something
// this file already does in a way the project controls.
//
// What is measured, and the three decisions behind it:
//   1. WARM-UP is separate and untimed. The first pass through a processor
//      touches pages that are not resident and runs branch predictors cold;
//      including it would report the machine's page-fault rate, not the code.
//   2. MEDIAN and P95, never the mean. Benchmarks run on shared CI machines and
//      developer laptops with other things happening; one 20 ms scheduling gap
//      would move a mean by more than any optimisation in this tree moves the
//      real figure.
//   3. The reported unit is NANOSECONDS PER FRAME, plus an x-realtime factor
//      (frames per second of wall clock, divided by the sample rate). "ms" hides
//      the block size; nanoseconds per frame lets a 64-frame and a 2048-frame
//      run be compared directly.
// ============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aura::bench {

/// One benchmark: a description of the work and a callable that performs it
/// exactly once. The harness decides how many times to call it.
struct Case {
    std::string name;        ///< stable, unique, e.g. "dsp/gain"
    std::string group;       ///< "dsp", "chain", "engine", ...
    std::string description; ///< what a reader needs to know to interpret it
    double sampleRate = 48000.0;
    int blockSize = 128;     ///< frames processed per call
    int channels = 2;
    /// Frames processed by one call (defaults to blockSize). A session benchmark
    /// renders one block for the whole session, so it also equals blockSize; a
    /// multi-block benchmark can override it.
    std::int64_t framesPerCall = 0;
    std::function<void()> call;
    /// Optional guard, run once after warm-up. A benchmark can silently measure
    /// nothing - the session benchmarks did exactly that at large block sizes,
    /// where 300 x 2048 frames ran past the end of the material and every
    /// iteration after that processed silence, reporting 3x better numbers than
    /// the truth. Returning false marks the case SKIPPED instead of publishing
    /// the number.
    std::function<bool()> sanityCheck;
};

struct Measurement {
    std::string name;
    std::string group;
    std::string description;
    double sampleRate = 0.0;
    int blockSize = 0;
    int channels = 0;
    std::int64_t framesPerCall = 0;
    std::uint64_t iterations = 0;
    double medianNsPerFrame = 0.0;
    double p95NsPerFrame = 0.0;
    double nsPerFrameSpread = 0.0; ///< p95/median - steady measurements sit near 1.0
    double xRealtime = 0.0;        ///< 1.0 = one core exactly keeps up; > 1 = headroom
    double callsPerSecond = 0.0;
};

class Registry {
public:
    void add(Case bench) { cases_.push_back(std::move(bench)); }

    [[nodiscard]] const std::vector<Case>& cases() const noexcept { return cases_; }

    [[nodiscard]] Case* find(const std::string& name) {
        for (auto& bench : cases_) {
            if (bench.name == name)
                return &bench;
        }
        return nullptr;
    }

private:
    std::vector<Case> cases_;
};

/// The process-wide registry every bench file appends to.
Registry& registry();

/// Runs the registered cases and returns the measurements.
/// @param filter      substring match on the case name (empty = everything)
/// @param iterations  timed calls per case
/// @param warmup      untimed calls per case (0 = auto: max(3, iterations/10))
std::vector<Measurement> runAll(const std::string& filter, std::uint64_t iterations,
                                std::uint64_t warmup = 0);

/// Human-readable table (used by the console report).
std::string formatTable(const std::vector<Measurement>& results);

/// Machine-readable report; `extra` is merged into the top-level object so a
/// caller can record the compiler, build type and machine alongside the numbers.
std::string formatJson(const std::vector<Measurement>& results,
                       const std::vector<std::pair<std::string, std::string>>& extra);

} // namespace aura::bench
