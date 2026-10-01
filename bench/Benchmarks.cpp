// ============================================================================
// AURA DAW - bench/Benchmarks.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
// ============================================================================
#include "Benchmarks.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aura::bench {

Registry& registry() {
    static Registry instance;
    return instance;
}

namespace {

using Clock = std::chrono::steady_clock;

double medianOf(std::vector<double>& values) {
    if (values.empty())
        return 0.0;
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    return values[middle];
}

double percentileOf(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0.0;
    const auto index = static_cast<std::size_t>(
        std::min<double>(fraction * static_cast<double>(values.size() - 1),
                         static_cast<double>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

} // namespace

std::vector<Measurement> runAll(const std::string& filter, std::uint64_t iterations,
                                std::uint64_t warmup) {
    std::vector<Measurement> results;
    if (iterations == 0)
        iterations = 1;

    for (const Case& bench : registry().cases()) {
        if (!filter.empty() && bench.name.find(filter) == std::string::npos)
            continue;
        if (!bench.call)
            continue;

        const std::uint64_t frames = bench.framesPerCall > 0 ? static_cast<std::uint64_t>(bench.framesPerCall)
                                                             : static_cast<std::uint64_t>(bench.blockSize);
        if (frames == 0)
            continue;

        const std::uint64_t warmupCalls = warmup > 0 ? warmup : std::max<std::uint64_t>(3, iterations / 10);
        for (std::uint64_t i = 0; i < warmupCalls; ++i)
            bench.call();

        if (bench.sanityCheck && !bench.sanityCheck()) {
            std::printf("SKIPPED %-34s (sanity check failed: the case is not doing the work it "
                        "claims)\n", bench.name.c_str());
            continue;
        }

        std::vector<double> nsPerFrame;
        nsPerFrame.reserve(static_cast<std::size_t>(iterations));
        const auto started = Clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const auto callStart = Clock::now();
            bench.call();
            const auto callEnd = Clock::now();
            const double ns = std::chrono::duration<double, std::nano>(callEnd - callStart).count();
            nsPerFrame.push_back(ns / static_cast<double>(frames));
        }
        const double wallSeconds = std::chrono::duration<double>(Clock::now() - started).count();

        Measurement measurement;
        measurement.name = bench.name;
        measurement.group = bench.group;
        measurement.description = bench.description;
        measurement.sampleRate = bench.sampleRate;
        measurement.blockSize = bench.blockSize;
        measurement.channels = bench.channels;
        measurement.framesPerCall = static_cast<std::int64_t>(frames);
        measurement.iterations = iterations;
        measurement.medianNsPerFrame = medianOf(nsPerFrame);
        measurement.p95NsPerFrame = percentileOf(nsPerFrame, 0.95);
        measurement.nsPerFrameSpread =
            measurement.medianNsPerFrame > 0.0 ? measurement.p95NsPerFrame / measurement.medianNsPerFrame : 0.0;
        measurement.xRealtime =
            measurement.medianNsPerFrame > 0.0
                ? (1.0e9 / measurement.medianNsPerFrame) / bench.sampleRate
                : 0.0;
        measurement.callsPerSecond =
            wallSeconds > 0.0 ? static_cast<double>(iterations) / wallSeconds : 0.0;

        results.push_back(std::move(measurement));
    }
    return results;
}

std::string formatTable(const std::vector<Measurement>& results) {
    std::ostringstream out;
    out << "name                              block  ch   ns/frame   p95/med   x-realtime   calls/s\n";
    out << "-----------------------------------------------------------------------------------------\n";
    char line[256];
    for (const Measurement& result : results) {
        std::snprintf(line, sizeof(line), "%-32s %6d %3d %10.1f %9.2f %12.1f %10.0f\n",
                      result.name.c_str(), result.blockSize, result.channels,
                      result.medianNsPerFrame, result.nsPerFrameSpread, result.xRealtime,
                      result.callsPerSecond);
        out << line;
    }
    return out.str();
}

std::string formatJson(const std::vector<Measurement>& results,
                       const std::vector<std::pair<std::string, std::string>>& extra) {
    auto escape = [](const std::string& input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (const char c : input) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c; break;
            }
        }
        return out;
    };

    std::ostringstream out;
    out << "{\n";
    for (const auto& [key, value] : extra)
        out << "  \"" << escape(key) << "\": \"" << escape(value) << "\",\n";
    out << "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Measurement& r = results[i];
        out << "    {\"name\": \"" << escape(r.name) << "\","
            << " \"group\": \"" << escape(r.group) << "\","
            << " \"description\": \"" << escape(r.description) << "\","
            << " \"sampleRate\": " << r.sampleRate << ","
            << " \"blockSize\": " << r.blockSize << ","
            << " \"channels\": " << r.channels << ","
            << " \"framesPerCall\": " << r.framesPerCall << ","
            << " \"iterations\": " << r.iterations << ","
            << " \"medianNsPerFrame\": " << r.medianNsPerFrame << ","
            << " \"p95NsPerFrame\": " << r.p95NsPerFrame << ","
            << " \"xRealtime\": " << r.xRealtime << ","
            << " \"callsPerSecond\": " << r.callsPerSecond << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace aura::bench
