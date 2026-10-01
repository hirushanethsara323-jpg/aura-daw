// ============================================================================
// AURA DAW - bench/SoakMain.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The memory-growth soak (issue #4). Built as `aura_soak` with
// -DAURA_BUILD_BENCHMARKS=ON, Release, and never part of ctest: it is a
// long-running observation, not an assertion that fits in a test suite.
//
// What it does: runs a real session through the real engine for a long time -
// transport rolling, clips streaming, inserts processing, the graph rebuilt
// periodically (the control-thread path that allocates), recording writing takes
// to disk when asked - and watches the process's resident memory. The interesting
// number is not the peak, it is the *trend*: a well-behaved audio engine reaches a
// steady state and stays there, and an engine that leaks reaches it never.
//
// Why rebuilds are part of it: the steady-state audio path is deliberately
// allocation-free (tests assert that), so a soak that only played back would prove
// little. The leaks that matter in a DAW live on the paths that are *allowed* to
// allocate - graph rebuilds, plan publication, media open/close, recording - and
// those are exactly what this loop keeps exercising.
//
// Usage:
//   ./aura_soak --minutes 480                 # the M5 acceptance run (8 hours)
//   ./aura_soak --minutes 20 --record --json soak.json
//
// Exit code 0 means the trend passed, 1 means it failed, 2 means bad arguments.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "aura/audio/AudioClip.hpp"
#include "aura/audio/RecordingEngine.hpp"
#include "aura/audio/Track.hpp"
#include "aura/dsp/Compressor.hpp"
#include "aura/dsp/Equalizer.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/dsp/Reverb.hpp"
#include "aura/engine/AudioEngine.hpp"
#include "aura/graph/SampleStream.hpp"
#include "aura/project/Project.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

using namespace aura;

namespace {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    double minutes = 1.0;
    int tracks = 12;
    int blockSize = 256;
    double sampleRate = 48000.0;
    int clipsPerTrack = 4;
    bool withInserts = true;
    bool record = false;
    std::int64_t rebuildEveryBlocks = 40000; // ~3.5 minutes at 256/48k
    double sampleIntervalSeconds = 5.0;
    double maxGrowthMbPerHour = 8.0;
    /// Audio seconds written per take. Deliberately NOT wall-clock seconds: this
    /// harness drives a null device at hundreds of times real time, so a take
    /// bounded by wall clock produced multi-gigabyte files (16 GB in five minutes
    /// the first time it was left unattended). Bounded by frames instead.
    double takeSeconds = 30.0;
    /// Wall-clock seconds between takes. Takes are *paced* rather than run
    /// back-to-back: at this throughput an unpaced loop opens and closes hundreds
    /// of files per second, which measures the disk, not the recorder.
    double takeIntervalSeconds = 20.0;
    /// Hard stop for one take, in megabytes written. A safety net, not a target:
    /// the soak must not be able to fill a disk even if the frame bound is wrong.
    double maxTakeMegabytes = 128.0;
    bool keepTakes = false;
    std::string jsonPath;
    std::string workDirectory = "/tmp/aura-soak";
};

bool parseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto next = [&](double& target) {
            if (i + 1 >= argc)
                return false;
            target = std::atof(argv[++i]);
            return true;
        };
        const auto nextInt = [&](int& target) {
            if (i + 1 >= argc)
                return false;
            target = std::atoi(argv[++i]);
            return true;
        };
        const auto nextString = [&](std::string& target) {
            if (i + 1 >= argc)
                return false;
            target = argv[++i];
            return true;
        };

        if (argument == "--minutes")
            next(options.minutes);
        else if (argument == "--tracks")
            nextInt(options.tracks);
        else if (argument == "--block")
            nextInt(options.blockSize);
        else if (argument == "--rate")
            next(options.sampleRate);
        else if (argument == "--clips")
            nextInt(options.clipsPerTrack);
        else if (argument == "--no-inserts")
            options.withInserts = false;
        else if (argument == "--record")
            options.record = true;
        else if (argument == "--take-seconds")
            next(options.takeSeconds);
        else if (argument == "--take-interval")
            next(options.takeIntervalSeconds);
        else if (argument == "--max-take-megabytes")
            next(options.maxTakeMegabytes);
        else if (argument == "--keep-takes")
            options.keepTakes = true;
        else if (argument == "--rebuild-every") {
            if (i + 1 < argc)
                options.rebuildEveryBlocks = std::atoll(argv[++i]);
        } else if (argument == "--interval")
            next(options.sampleIntervalSeconds);
        else if (argument == "--max-growth-mb-per-hour")
            next(options.maxGrowthMbPerHour);
        else if (argument == "--json")
            nextString(options.jsonPath);
        else if (argument == "--workdir")
            nextString(options.workDirectory);
        else if (argument == "--help") {
            std::printf("aura_soak [--minutes N] [--tracks N] [--block N] [--rate HZ]\n"
                        "          [--clips N] [--no-inserts] [--record] [--rebuild-every N]\n"
                        "          [--interval SEC] [--max-growth-mb-per-hour N]\n"
                        "          [--take-seconds SEC] [--take-interval SEC]\n"
                        "          [--max-take-megabytes MB] [--keep-takes]\n"
                        "          [--json PATH] [--workdir DIR]\n");
            return false;
        } else {
            std::fprintf(stderr, "aura_soak: unknown argument '%s' (try --help)\n", argument.c_str());
            return false;
        }
    }
    if (options.minutes <= 0.0 || options.tracks <= 0 || options.blockSize < 32) {
        std::fprintf(stderr, "aura_soak: implausible options\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Memory reporting
// ---------------------------------------------------------------------------
/// Resident set size in bytes. Linux reads /proc (no allocation, no syscall per
/// page); Windows asks the process itself. Both are what a user would see in a
/// task manager, which is the number a soak is about.
std::size_t residentBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<std::size_t>(counters.WorkingSetSize);
    return 0;
#else
    std::ifstream status("/proc/self/statm");
    long totalPages = 0;
    long residentPages = 0;
    if (status >> totalPages >> residentPages) {
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        return static_cast<std::size_t>(residentPages) * static_cast<std::size_t>(pageSize > 0 ? pageSize : 4096);
    }
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// A device that never sleeps: the soak wants throughput, not real time
// ---------------------------------------------------------------------------
class NullDevice final : public engine::IAudioDevice {
public:
    Status start(Callback callback) override {
        callback_ = std::move(callback);
        running_ = true;
        return success();
    }
    void stop() override {
        running_ = false;
        callback_ = {};
    }
    [[nodiscard]] bool isRunning() const noexcept override { return running_; }
    [[nodiscard]] std::string name() const override { return "soak-null"; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] int bufferSize() const noexcept override { return bufferSize_; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return 2; }
    [[nodiscard]] int numInputChannels() const noexcept override { return 2; }
    [[nodiscard]] int latencySamples() const noexcept override { return 0; }

    void setFormat(double sampleRate, int bufferSize) noexcept {
        sampleRate_ = sampleRate;
        bufferSize_ = bufferSize;
    }

private:
    Callback callback_;
    bool running_ = false;
    double sampleRate_ = 48000.0;
    int bufferSize_ = 256;
};

// ---------------------------------------------------------------------------
// The session under test
// ---------------------------------------------------------------------------
struct SoakSession {
    // DESTRUCTION ORDER IS LOAD-BEARING. Members are destroyed in reverse
    // declaration order, so `project` first would leave the engine mid-block, and
    // `device` first would leave the engine holding a freed pointer (that exact
    // bug -- a use-after-free in AudioEngine::shutdown() -- is what the ASAN run
    // of this harness caught; it is fixed in the engine, and this ordering plus
    // the explicit teardown below keeps the harness honest about it too).
    std::vector<float> inputLeft;
    std::vector<float> inputRight;

    std::vector<std::unique_ptr<dsp::Processor>> processors; ///< owns what the inserts point at
    std::unique_ptr<audio::RecordingEngine> recorder;
    std::unique_ptr<NullDevice> device;
    std::unique_ptr<engine::AudioEngine> engine;
    std::unique_ptr<project::Project> project;

    /// Stops streaming and releases the device in the order the ownership
    /// contract requires: detach the engine's pointer, stop, then destroy.
    void teardown() {
        if (recorder && recorder->isRecording())
            (void)recorder->stopTake(); // finishes the take; never leave a partial file
        if (engine) {
            engine->setProject(nullptr);
            engine->detachDevice(); // stops streaming while the device is alive
            engine->shutdown();     // idempotent: nothing left to stop
        }
        if (device)
            device->stop();
        device.reset();
        project.reset();
        engine.reset();
    }

    ~SoakSession() { teardown(); }
};

std::unique_ptr<SoakSession> buildSession(const Options& options) {
    auto session = std::make_unique<SoakSession>();
    session->project = project::Project::createEmpty("Soak Session");
    session->project->setSampleRate(options.sampleRate);

    const int blockSize = options.blockSize;
    session->project->mixer().prepare(options.sampleRate, blockSize, 2);

    const auto clipFrames = static_cast<std::int64_t>(options.sampleRate); // 1 s per clip
    std::int64_t seed = 0;

    // A reverb on a bus, fed by a send from the first track: a tail that keeps
    // sounding across block boundaries is the state that hides a buffer bug.
    const audio::TrackId busId =
        session->project->mixer().createTrack(audio::TrackKind::Aux, "Space");
    if (audio::Track* bus = session->project->mixer().findTrack(busId)) {
        auto reverb = std::make_unique<dsp::Reverb>();
        dsp::ReverbSettings settings;
        settings.decaySeconds = 3.0f;
        settings.mix = 0.3f;
        reverb->setSettings(settings);
        session->processors.push_back(std::move(reverb));
        (void)bus->addInsert({"Reverb", session->processors.back().get(), false, "", 1.0f});
    }

    for (int track = 0; track < options.tracks; ++track) {
        const audio::TrackId id = session->project->mixer().createTrack(
            audio::TrackKind::Audio, "Track " + std::to_string(track + 1));
        audio::Track* created = session->project->mixer().findTrack(id);
        if (!created)
            continue;

        if (options.withInserts) {
            auto eq = std::make_unique<dsp::Equalizer>();
            dsp::EqSettings eqSettings;
            eqSettings.bands[0] = {true, 90.0, 0.707, 0.0, 0.7};
            eqSettings.bands[2] = {true, 2500.0, 1.1, -1.5, 0.7};
            eq->setSettings(eqSettings);
            session->processors.push_back(std::move(eq));

            auto compressor = std::make_unique<dsp::Compressor>();
            compressor->setAutoMakeup(true);
            session->processors.push_back(std::move(compressor));

            (void)created->addInsert({"EQ", session->processors[session->processors.size() - 2].get(),
                                      false, "", 1.0f});
            (void)created->addInsert({"Comp", session->processors.back().get(), false, "", 1.0f});
        }

        for (int index = 0; index < options.clipsPerTrack; ++index) {
            std::vector<std::vector<float>> channels(
                2, std::vector<float>(static_cast<std::size_t>(clipFrames)));
            const double frequency = 90.0 + 13.0 * static_cast<double>(seed % 30);
            for (std::size_t i = 0; i < channels[0].size(); ++i) {
                const double t = static_cast<double>(i) / options.sampleRate;
                channels[0][i] = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * frequency * t));
                channels[1][i] = channels[0][i] * 0.7f;
            }
            ++seed;

            audio::Clip clip(session->project->nextClipId(), audio::ClipType::Audio,
                             "Clip " + std::to_string(index + 1));
            clip.setStart(static_cast<time::SamplePos>(index) * clipFrames);
            clip.setLength(clipFrames);
            clip.setStream(graph::MemoryStream::fromPlanar(std::move(channels), options.sampleRate));
            if (auto* stored = session->project->addClip(std::move(clip)))
                created->clips().push_back(stored->id());
        }

        if (track == 0) {
            audio::Send send;
            send.destination = busId;
            send.level = 0.35f;
            send.enabled = true;
            created->addSend(send);
        }
    }

    session->device = std::make_unique<NullDevice>();
    session->device->setFormat(options.sampleRate, blockSize);
    session->engine = std::make_unique<engine::AudioEngine>();
    session->engine->setProject(session->project.get());

    engine::EngineSettings settings;
    settings.sampleRate = options.sampleRate;
    settings.bufferSize = blockSize;
    settings.numOutputChannels = 2;
    settings.numInputChannels = 2;
    if (!session->engine->initialise(session->device.get(), settings)) {
        std::fprintf(stderr, "aura_soak: engine failed to initialise\n");
        return nullptr;
    }
    if (!session->engine->rebuildGraph()) {
        std::fprintf(stderr, "aura_soak: graph failed to build\n");
        return nullptr;
    }

    // A quiet input signal, so recording (when enabled) writes real audio rather
    // than a file of zeroes, and the input path is exercised at the same time.
    session->inputLeft.assign(static_cast<std::size_t>(blockSize), 0.05f);
    session->inputRight.assign(static_cast<std::size_t>(blockSize), -0.05f);

    if (options.record) {
        std::error_code error;
        std::filesystem::create_directories(options.workDirectory + "/takes", error);
        session->recorder = std::make_unique<audio::RecordingEngine>();
        audio::RecordingSettings recording;
        recording.outputDirectory = options.workDirectory + "/takes";
        recording.temporaryDirectory = options.workDirectory + "/incomplete";
        recording.sampleRate = static_cast<std::uint32_t>(options.sampleRate);
        recording.numChannels = 2;
        recording.bitsPerSample = 24;
        recording.minimumFreeMegabytes = 0; // the sandbox may report very little
        session->recorder->setSettings(recording);
        session->recorder->prepare();
    }

    return session;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------
struct Report {
    double minutes = 0.0;
    std::int64_t blocks = 0;
    std::int64_t frames = 0;
    int rebuilds = 0;
    int takesWritten = 0;
    double peakTakeMegabytes = 0.0;
    bool takeLimitHit = false;
    std::size_t firstMb = 0;
    std::size_t secondMb = 0;
    std::size_t minMb = 0;
    std::size_t maxMb = 0;
    std::size_t finalMb = 0;
    double growthMbPerHour = 0.0;
    bool passed = false;
};

/// One sample point: seconds since the run started, resident bytes.
using Sample = std::pair<double, std::size_t>;

/// Fills the memory-trend fields of `report` from the samples taken so far.
///
/// Warm-up is excluded: first-touch page faults, the allocator's initial arena
/// growth and the clip buffers all land in the first seconds, and counting them
/// would make every run look like a leak. The steady-state samples are then split
/// in half and the two means are compared - the *trend* is the answer, and it is
/// turned into MB/hour because 1 MB/hour (~3 KB/s) is invisible in any single
/// minute and obvious over a session.
void applyTrend(const std::vector<Sample>& samples, Report& report) {
    if (samples.empty())
        return;

    const auto toMb = [](std::size_t bytes) { return bytes / (1024u * 1024u); };
    std::size_t minimum = samples.front().second;
    std::size_t maximum = samples.front().second;
    for (const auto& [seconds, bytes] : samples) {
        (void)seconds;
        minimum = std::min(minimum, bytes);
        maximum = std::max(maximum, bytes);
    }
    report.minMb = toMb(minimum);
    report.maxMb = toMb(maximum);
    report.finalMb = toMb(samples.back().second);
    report.firstMb = report.secondMb = report.finalMb;

    const double warmupSeconds = std::max(30.0, samples.back().first * 0.1);
    std::vector<Sample> steady;
    for (const auto& entry : samples) {
        if (entry.first >= warmupSeconds)
            steady.push_back(entry);
    }
    if (steady.size() < 4)
        return;

    const std::size_t half = steady.size() / 2;
    double firstSum = 0.0;
    double secondSum = 0.0;
    for (std::size_t i = 0; i < half; ++i)
        firstSum += static_cast<double>(steady[i].second);
    for (std::size_t i = half; i < steady.size(); ++i)
        secondSum += static_cast<double>(steady[i].second);
    const double firstMean = firstSum / static_cast<double>(half);
    const double secondMean = secondSum / static_cast<double>(steady.size() - half);

    const double midpointFirst = (steady.front().first + steady[half - 1].first) * 0.5;
    const double midpointSecond = (steady[half].first + steady.back().first) * 0.5;
    const double hours = std::max(1.0 / 3600.0, (midpointSecond - midpointFirst) / 3600.0);
    const double growthBytes = static_cast<double>(secondMean) - static_cast<double>(firstMean);
    report.growthMbPerHour = growthBytes / (1024.0 * 1024.0) / hours;
    report.firstMb = toMb(static_cast<std::size_t>(firstMean));
    report.secondMb = toMb(static_cast<std::size_t>(secondMean));
}

/// Writes the machine-readable report. Called with `complete = false` after every
/// sample, so a long run that is interrupted (Ctrl-C, a killed agent session, a
/// reboot) still leaves the evidence behind instead of nothing: the file says
/// `"complete": false` and holds the trend as far as it got.
bool writeJson(const std::string& path, const Options& options, const Report& report,
               int blockSize, bool complete) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << "{\n";
    out << "  \"complete\": " << (complete ? "true" : "false") << ",\n";
    out << "  \"minutes\": " << report.minutes << ",\n";
    out << "  \"blocks\": " << report.blocks << ",\n";
    out << "  \"tracks\": " << options.tracks << ",\n";
    out << "  \"blockSize\": " << blockSize << ",\n";
    out << "  \"sampleRate\": " << options.sampleRate << ",\n";
    out << "  \"inserts\": " << (options.withInserts ? "true" : "false") << ",\n";
    out << "  \"recording\": " << (options.record ? "true" : "false") << ",\n";
    out << "  \"rebuilds\": " << report.rebuilds << ",\n";
    out << "  \"takesWritten\": " << report.takesWritten << ",\n";
    out << "  \"takeSeconds\": " << options.takeSeconds << ",\n";
    out << "  \"takeMegabytesPeak\": " << report.peakTakeMegabytes << ",\n";
    out << "  \"takeSizeGuardHit\": " << (report.takeLimitHit ? "true" : "false") << ",\n";
    out << "  \"rssFirstHalfMb\": " << report.firstMb << ",\n";
    out << "  \"rssSecondHalfMb\": " << report.secondMb << ",\n";
    out << "  \"rssMinMb\": " << report.minMb << ",\n";
    out << "  \"rssMaxMb\": " << report.maxMb << ",\n";
    out << "  \"rssFinalMb\": " << report.finalMb << ",\n";
    out << "  \"growthMbPerHour\": " << report.growthMbPerHour << ",\n";
    out << "  \"limitMbPerHour\": " << options.maxGrowthMbPerHour << ",\n";
    out << "  \"passed\": " << (report.passed ? "true" : "false") << "\n";
    out << "}\n";
    return true;
}


} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, options))
        return 2;

    auto session = buildSession(options);
    if (!session)
        return 2;

    const int blockSize = options.blockSize;
    std::vector<float> left(static_cast<std::size_t>(blockSize), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(blockSize), 0.0f);
    float* outputs[2] = {left.data(), right.data()};
    const float* inputs[2] = {session->inputLeft.data(), session->inputRight.data()};

    const auto start = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>(options.minutes * 60.0);
    auto nextSample = start;
    const auto sampleInterval = std::chrono::duration<double>(options.sampleIntervalSeconds);

    std::vector<Sample> samples; // (seconds, bytes)
    std::int64_t blocks = 0;
    int rebuilds = 0;
    int takes = 0;
    bool recording = false;
    std::int64_t recordedFrames = 0;
    double nextTakeAt = 0.0;
    std::vector<double> takeMegabytes;
    double peakTakeMegabytes = 0.0;
    bool takeLimitHit = false;

    std::printf("aura_soak: %d tracks, %d clips each, block %d, %.0f Hz, inserts %s, record %s, "
                "%.1f minutes\n",
                options.tracks, options.clipsPerTrack, blockSize, options.sampleRate,
                options.withInserts ? "on" : "off", options.record ? "on" : "off", options.minutes);

    bool transportStarted = false;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= duration.count())
            break;

        // The engine drives the block; the soak owns the loop.
        session->engine->processBlock(outputs, inputs, blockSize);
        ++blocks;

        if (!transportStarted) {
            session->engine->transport().play();
            transportStarted = true;
        }

        // Recording: open a take, write a BOUNDED amount of audio, close it, and
        // repeat. What the soak is testing is the file-open/close path (a recorder
        // that leaks a handle or a buffer leaks one per take), not the disk: the
        // bound is frames, and the file is deleted again unless --keep-takes.
        if (options.record && session->recorder) {
            if (!recording && elapsed >= nextTakeAt) {
                if (session->recorder->startTake(static_cast<time::SamplePos>(blocks) * blockSize,
                                                 "Soak Take")) {
                    recording = true;
                    recordedFrames = 0;
                }
            }
            if (recording) {
                session->recorder->processInput(inputs, 2, blockSize);
                recordedFrames += blockSize;
                // 24-bit stereo = 6 bytes per frame.
                const double writtenMb =
                    static_cast<double>(recordedFrames) * 6.0 / (1024.0 * 1024.0);
                peakTakeMegabytes = std::max(peakTakeMegabytes, writtenMb);
                const bool framesDone =
                    static_cast<double>(recordedFrames) >= options.takeSeconds * options.sampleRate;
                const bool sizeGuard = writtenMb >= options.maxTakeMegabytes;
                if (sizeGuard)
                    takeLimitHit = true;
                if (framesDone || sizeGuard) {
                    const auto stopped = session->recorder->stopTake();
                    if (stopped) {
                        ++takes;
                        takeMegabytes.push_back(writtenMb);
                        if (!options.keepTakes) {
                            std::error_code removeError;
                            std::filesystem::remove(stopped.value(), removeError);
                        }
                    }
                    recording = false;
                    nextTakeAt = elapsed + options.takeIntervalSeconds;
                }
            }
        }

        // Graph rebuilds: the control-thread path, and the one that allocates.
        if (options.rebuildEveryBlocks > 0 && blocks % options.rebuildEveryBlocks == 0) {
            if (session->engine->rebuildGraph())
                ++rebuilds;
        }

        if (now >= nextSample) {
            samples.emplace_back(elapsed, residentBytes());
            nextSample = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(sampleInterval);

            // Checkpoint the JSON every sample: a long run that is interrupted is
            // still evidence, and watching the trend while it runs is how a leak is
            // caught early instead of eight hours later.
            if (!options.jsonPath.empty()) {
                Report checkpoint;
                checkpoint.minutes = elapsed / 60.0;
                checkpoint.blocks = blocks;
                checkpoint.rebuilds = rebuilds;
                checkpoint.takesWritten = takes;
                checkpoint.peakTakeMegabytes = peakTakeMegabytes;
                checkpoint.takeLimitHit = takeLimitHit;
                applyTrend(samples, checkpoint);
                checkpoint.passed = checkpoint.growthMbPerHour <= options.maxGrowthMbPerHour;
                (void)writeJson(options.jsonPath, options, checkpoint, blockSize, false);
            }
        }
    }

    if (recording && session->recorder) {
        const auto stopped = session->recorder->stopTake();
        if (stopped) {
            ++takes;
            takeMegabytes.push_back(static_cast<double>(recordedFrames) * 6.0 / (1024.0 * 1024.0));
            if (!options.keepTakes)
                std::filesystem::remove(stopped.value());
        }
    }
    samples.emplace_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                         residentBytes());

    // --- analyse -----------------------------------------------------------
    Report report;
    report.minutes = samples.empty() ? 0.0 : samples.back().first / 60.0;
    report.blocks = blocks;
    report.frames = blocks * blockSize;
    report.rebuilds = rebuilds;
    report.takesWritten = takes;
    report.peakTakeMegabytes = peakTakeMegabytes;
    report.takeLimitHit = takeLimitHit;
    applyTrend(samples, report);

    report.passed = report.growthMbPerHour <= options.maxGrowthMbPerHour;

    // --- print -------------------------------------------------------------
    std::printf("aura_soak: %.2f minutes, %lld blocks (%.1f Gframes), %d rebuilds, %d takes\n",
                report.minutes, static_cast<long long>(report.blocks),
                static_cast<double>(report.frames) / 1.0e9, report.rebuilds, report.takesWritten);
    std::printf("aura_soak: RSS first half %zu MB, second half %zu MB (min %zu, max %zu, final %zu)\n",
                report.firstMb, report.secondMb, report.minMb, report.maxMb, report.finalMb);
    if (options.record) {
        double total = 0.0;
        for (const double megabytes : takeMegabytes)
            total += megabytes;
        std::printf("aura_soak: takes %d, %.1f MB written in total, peak %.1f MB/take "
                    "(%.0f s of audio each, %s)%s\n",
                    report.takesWritten, total, report.peakTakeMegabytes, options.takeSeconds,
                    options.keepTakes ? "kept" : "deleted after closing",
                    report.takeLimitHit ? "  [SIZE GUARD HIT: raise --take-seconds carefully]"
                                        : "");
    }
    std::printf("aura_soak: growth %+.2f MB/hour (limit %.2f) -> %s\n", report.growthMbPerHour,
                options.maxGrowthMbPerHour, report.passed ? "PASS" : "FAIL");

    if (!options.jsonPath.empty() && writeJson(options.jsonPath, options, report, blockSize, true)) {
        std::printf("aura_soak: wrote %s\n", options.jsonPath.c_str());
    }

    return report.passed ? 0 : 1;
}

