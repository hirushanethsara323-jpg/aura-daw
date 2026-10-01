// ============================================================================
// AURA DAW - bench/main.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Benchmark runner. Builds with AURA_BUILD_BENCHMARKS=ON (off by default: a
// benchmark is a measurement, not a test, and it must never run inside ctest
// where a shared CI machine decides the numbers).
//
// Usage:
//   aura_bench                          # everything, default iteration count
//   aura_bench --filter engine/session  # substring match on the case name
//   aura_bench --list                   # names and descriptions only
//   aura_bench --iterations 200 --warmup 20
//   aura_bench --json benchmarks.json   # machine-readable, for PERFORMANCE.md
// ============================================================================
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Benchmarks.hpp"

namespace aura::bench {
void registerDspBenchmarks();
void registerEngineBenchmarks(const std::vector<int>& blockSizes);
double dspBenchmarkChecksum();
double engineBenchmarkChecksum();
} // namespace aura::bench

namespace {

void printUsage() {
    std::printf(
        "aura_bench [options]\n"
        "  --filter <substring>   run cases whose name contains this (default: all)\n"
        "  --iterations <n>       timed calls per case (default 500)\n"
        "  --warmup <n>           untimed calls per case (default 10%% of iterations)\n"
        "  --json <path>          also write the machine-readable report here\n"
        "  --list                 list the cases and exit\n"
        "  --quiet                print only the summary line\n"
        "  --help                 this text\n");
}

std::string compilerString() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string buildTypeString() {
#if defined(NDEBUG)
    return "Release/RelWithDebInfo (NDEBUG)";
#else
    return "Debug (assertions on: numbers are not representative)";
#endif
}

} // namespace

int main(int argc, char** argv) {
    using namespace aura::bench;

    std::string filter;
    std::string jsonPath;
    std::uint64_t iterations = 500;
    std::uint64_t warmup = 0;
    bool listOnly = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto nextValue = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "aura_bench: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--filter")
            filter = nextValue("--filter");
        else if (argument == "--iterations")
            iterations = std::strtoull(nextValue("--iterations").c_str(), nullptr, 10);
        else if (argument == "--warmup")
            warmup = std::strtoull(nextValue("--warmup").c_str(), nullptr, 10);
        else if (argument == "--json")
            jsonPath = nextValue("--json");
        else if (argument == "--list")
            listOnly = true;
        else if (argument == "--quiet")
            quiet = true;
        else if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        } else {
            std::fprintf(stderr, "aura_bench: unknown argument '%s'\n", argument.c_str());
            printUsage();
            return 2;
        }
    }

    const std::vector<int> blockSizes{64, 128, 256, 512, 1024, 2048};
    registerDspBenchmarks();
    registerEngineBenchmarks(blockSizes);

    if (listOnly) {
        for (const Case& bench : registry().cases()) {
            if (!filter.empty() && bench.name.find(filter) == std::string::npos)
                continue;
            std::printf("%-34s [%s] %s\n", bench.name.c_str(), bench.group.c_str(),
                        bench.description.c_str());
        }
        return 0;
    }

    std::printf("aura_bench: %s, %s\n", compilerString().c_str(), buildTypeString().c_str());
    if (iterations < 20)
        std::printf("aura_bench: warning - fewer than 20 iterations makes the p95 column meaningless\n");

    const auto results = runAll(filter, iterations, warmup);
    if (results.empty()) {
        std::fprintf(stderr, "aura_bench: no case matched '%s'\n", filter.c_str());
        return 1;
    }

    if (!quiet)
        std::printf("%s", formatTable(results).c_str());

    // Head-room summary: the worst case is what decides the buffer size a user
    // can run, so it is reported rather than left for the reader to find.
    const Measurement* worst = &results.front();
    for (const Measurement& result : results) {
        if (result.xRealtime < worst->xRealtime)
            worst = &result;
    }
    std::printf("aura_bench: %zu case(s); lowest head-room %.1fx realtime at %s (%d-frame blocks)\n",
                results.size(), worst->xRealtime, worst->name.c_str(), worst->blockSize);

    // The checksums keep the measured work observable; printing them also gives a
    // quick way to see whether a change altered the audio (any non-zero value is
    // fine, the value itself is not a contract).
    std::printf("aura_bench: checksums dsp=%.6f engine=%.6f\n", dspBenchmarkChecksum(),
                engineBenchmarkChecksum());

    if (!jsonPath.empty()) {
        const std::vector<std::pair<std::string, std::string>> extra{
            {"compiler", compilerString()},
            {"buildType", buildTypeString()},
            {"iterations", std::to_string(iterations)},
        };
        std::ofstream out(jsonPath);
        if (!out) {
            std::fprintf(stderr, "aura_bench: cannot write '%s'\n", jsonPath.c_str());
            return 1;
        }
        out << formatJson(results, extra);
        std::printf("aura_bench: wrote %s\n", jsonPath.c_str());
    }
    return 0;
}
