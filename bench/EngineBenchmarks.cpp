// ============================================================================
// AURA DAW - bench/EngineBenchmarks.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Session-scale benchmarks (issue #2): how much of a block budget a realistic
// session consumes, measured through the real graph and the real
// AudioEngine::processBlock() entry point.
//
// Two deliberate choices:
//   1. The device is a NULL device that runs the callback in a tight loop.
//      Driving MockAudioDevice would sleep to real time, so every result would
//      be x-realtime = 1.0 by construction and the benchmark would prove
//      nothing.
//   2. Clip audio lives in MemoryStream, not on disk. That keeps the measurement
//      about the graph, not about the filesystem of whatever machine runs the
//      benchmarks (disk streaming has its own case, added with the media pool
//      work). A machine with a cold page cache would otherwise report its SSD.
// ============================================================================
#include "Benchmarks.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "aura/audio/AudioClip.hpp"
#include "aura/audio/Track.hpp"
#include "aura/core/Math.hpp"
#include "aura/dsp/Compressor.hpp"
#include "aura/dsp/Equalizer.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/engine/AudioEngine.hpp"
#include "aura/graph/SampleStream.hpp"
#include "aura/project/Project.hpp"

namespace aura::bench {
namespace {

/// A device that never sleeps: the harness calls the callback itself, so the
/// measured time is the engine's, not the operating system's timer.
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
    [[nodiscard]] std::string name() const override { return "null"; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] int bufferSize() const noexcept override { return bufferSize_; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return 2; }
    [[nodiscard]] int numInputChannels() const noexcept override { return 0; }
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

struct SessionSettings {
    int tracks = 16;
    int clipsPerTrack = 8;
    double clipSeconds = 0.5;
    bool withInserts = false; ///< EQ + compressor on every track
    double sampleRate = 48000.0;
};

/// A complete, playable session: project, mixer, engine, graph.
class Session {
public:
    explicit Session(const SessionSettings& settings, int blockSize) : settings_(settings) {
        project_ = project::Project::createEmpty("Benchmark Session");
        project_->setSampleRate(settings_.sampleRate);
        project_->mixer().prepare(settings_.sampleRate, blockSize, 2);

        const auto clipFrames = static_cast<std::int64_t>(settings_.clipSeconds * settings_.sampleRate);
        std::int64_t streamSeed = 0;

        for (int track = 0; track < settings_.tracks; ++track) {
            const audio::TrackId trackId =
                project_->mixer().createTrack(audio::TrackKind::Audio, "Track " + std::to_string(track + 1));
            audio::Track* created = project_->mixer().findTrack(trackId);
            if (!created)
                continue;

            if (settings_.withInserts) {
                // Two inserts per track is the common "compressed and EQ'd" case
                // in a real session, and it is what makes the per-track cost
                // meaningful rather than a memcpy measurement.
                auto eq = std::make_unique<dsp::Equalizer>();
                dsp::EqSettings eqSettings;
                eqSettings.bands[0] = {true, 80.0, 0.707, 0.0, 0.7};
                eqSettings.bands[2] = {true, 2000.0, 1.2, -2.0, 0.7};
                eq->setSettings(eqSettings);
                auto compressor = std::make_unique<dsp::Compressor>();
                compressor->setAutoMakeup(true);
                insertProcessors_.push_back(std::move(eq));
                insertProcessors_.push_back(std::move(compressor));
                dsp::Processor* eqPtr = insertProcessors_[insertProcessors_.size() - 2].get();
                dsp::Processor* compPtr = insertProcessors_.back().get();
                // The chain limit is 16 slots and a benchmark session adds two
                // per track, so a failure here would mean the model changed.
                if (!created->addInsert({"EQ", eqPtr, false, "", 1.0f}) ||
                    !created->addInsert({"Comp", compPtr, false, "", 1.0f}))
                    std::fprintf(stderr, "bench: could not add inserts to track %d\n", track);
            }

            for (int index = 0; index < settings_.clipsPerTrack; ++index) {
                std::vector<std::vector<float>> channels(2, std::vector<float>(static_cast<std::size_t>(clipFrames)));
                // Different content per clip: identical buffers would all map to
                // the same pages and flatter the memory path in a way a real
                // session never does.
                const double frequency = 110.0 + 17.0 * static_cast<double>(streamSeed % 24);
                for (std::size_t i = 0; i < channels[0].size(); ++i) {
                    const double t = static_cast<double>(i) / settings_.sampleRate;
                    const float value = static_cast<float>(
                        0.3 * std::sin(math::kTwoPi * frequency * t) +
                        0.1 * std::sin(math::kTwoPi * (frequency * 3.0) * t));
                    channels[0][i] = value;
                    channels[1][i] = value * 0.8f;
                }
                ++streamSeed;

                audio::Clip clip(project_->nextClipId(), audio::ClipType::Audio,
                                 "Clip " + std::to_string(index + 1));
                clip.setStart(static_cast<time::SamplePos>(index) * clipFrames);
                clip.setLength(clipFrames);
                clip.setStream(graph::MemoryStream::fromPlanar(std::move(channels), settings_.sampleRate));
                if (auto* stored = project_->addClip(std::move(clip)))
                    created->clips().push_back(stored->id());
            }
        }

        engine_ = std::make_unique<engine::AudioEngine>();
        engine_->setProject(project_.get());
        device_.setFormat(settings_.sampleRate, blockSize);

        engine::EngineSettings engineSettings;
        engineSettings.sampleRate = settings_.sampleRate;
        engineSettings.bufferSize = blockSize;
        engineSettings.numOutputChannels = 2;
        engineSettings.numInputChannels = 0;
        if (!engine_->initialise(&device_, engineSettings) || !engine_->rebuildGraph()) {
            std::fprintf(stderr, "bench: engine setup failed\n");
            return;
        }

        // The transport loops over the material. Without this, a run of
        // iterations at a large block size walks past the last clip and then
        // measures silence at 3x the realtime factor of the actual session.
        materialFrames_ = static_cast<time::SamplePos>(settings_.clipsPerTrack) * clipFrames;
        engine_->transport().setLoopRange(0, materialFrames_);
        engine_->transport().setLoopMode(transport::LoopMode::Loop);
        engine_->transport().play();

        outputLeft_.assign(static_cast<std::size_t>(blockSize), 0.0f);
        outputRight_.assign(static_cast<std::size_t>(blockSize), 0.0f);
        outputs_[0] = outputLeft_.data();
        outputs_[1] = outputRight_.data();
    }

    ~Session() {
        if (engine_)
            engine_->shutdown();
    }

    void renderBlock(int frames) noexcept {
        engine_->processBlock(outputs_, nullptr, frames);
        // Keep the result observable.
        checksum += static_cast<double>(outputLeft_[0]);
    }

    /// Renders one block (untimed) and returns its peak. Used as the case's
    /// sanity check: a session benchmark whose output is silent is measuring
    /// nothing.
    [[nodiscard]] float probePeak(int frames) noexcept {
        engine_->processBlock(outputs_, nullptr, frames);
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            peak = std::max(peak, std::abs(outputLeft_[static_cast<std::size_t>(i)]));
            peak = std::max(peak, std::abs(outputRight_[static_cast<std::size_t>(i)]));
        }
        return peak;
    }

    [[nodiscard]] double materialSeconds() const noexcept {
        return static_cast<double>(materialFrames_) / settings_.sampleRate;
    }

    [[nodiscard]] int clipCount() const noexcept { return settings_.tracks * settings_.clipsPerTrack; }

    static double checksum;

private:
    SessionSettings settings_;
    std::unique_ptr<project::Project> project_;
    std::unique_ptr<engine::AudioEngine> engine_;
    NullDevice device_;
    std::vector<std::unique_ptr<dsp::Processor>> insertProcessors_;
    std::vector<float> outputLeft_;
    std::vector<float> outputRight_;
    float* outputs_[2] = {nullptr, nullptr};
    time::SamplePos materialFrames_ = 0;
};

double Session::checksum = 0.0;

/// Registers a session benchmark for one block size.
void addSession(const std::string& name, const std::string& description,
                const SessionSettings& settings, const std::vector<int>& blockSizes) {
    for (const int blockSize : blockSizes) {
        Case bench;
        bench.name = name + "/" + std::to_string(blockSize);
        bench.group = "engine";
        bench.sampleRate = settings.sampleRate;
        bench.blockSize = blockSize;
        bench.channels = 2;

        // The session is built once and reused; building it inside the timed
        // callable would measure Project::createEmpty, not playback.
        auto session = std::make_shared<Session>(settings, blockSize);
        char material[64];
        std::snprintf(material, sizeof(material), "%.1f s of material, looping",
                      session->materialSeconds());
        bench.description = description + " (" + material + ")";
        bench.call = [session, blockSize]() { session->renderBlock(blockSize); };
        bench.sanityCheck = [session, blockSize]() { return session->probePeak(blockSize) > 1.0e-4f; };
        registry().add(std::move(bench));
    }
}

} // namespace

void registerEngineBenchmarks(const std::vector<int>& blockSizes) {
    SessionSettings oneTrack;
    oneTrack.tracks = 1;
    oneTrack.clipsPerTrack = 1;
    oneTrack.clipSeconds = 1.0;
    addSession("engine/session-1x1", "Baseline: 1 track, 1 clip, empty insert chain",
               oneTrack, blockSizes);

    SessionSettings medium;
    medium.tracks = 16;
    medium.clipsPerTrack = 8;
    medium.clipSeconds = 0.5;
    addSession("engine/session-16x8", "16 tracks x 8 clips (2 s of material per track), no inserts",
               medium, blockSizes);

    SessionSettings withInserts = medium;
    withInserts.withInserts = true;
    addSession("engine/session-16x8-fx", "16 tracks x 8 clips, EQ + compressor on every track",
               withInserts, blockSizes);

    SessionSettings large;
    large.tracks = 48;
    large.clipsPerTrack = 8;
    large.clipSeconds = 0.5;
    addSession("engine/session-48x8", "48 tracks x 8 clips: a dense arrangement",
               large, blockSizes);

    SessionSettings largeWithInserts = large;
    largeWithInserts.withInserts = true;
    addSession("engine/session-48x8-fx", "48 tracks x 8 clips with EQ + compressor on every track",
               largeWithInserts, {256});
}

double engineBenchmarkChecksum() {
    return Session::checksum;
}

} // namespace aura::bench
