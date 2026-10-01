// ============================================================================
// AURA DAW - tests/engine/EngineTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine tests drive the OFFLINE path (no device, no real-time thread) so they
// are deterministic on CI, plus one test that runs the mock device for real.
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "aura/audio/AudioClip.hpp"
#include "aura/audioio/AudioDeviceManager.hpp"
#include "aura/core/Realtime.hpp"
#include "aura/dsp/GainProcessor.hpp"
#include "aura/dsp/Reverb.hpp"
#include "aura/engine/AudioEngine.hpp"
#include "aura/engine/OfflineRenderer.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/media/AudioFile.hpp"

#include <thread>
#include "aura/project/Project.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {

/// Deterministic offline device: the test calls run() explicitly.
class TestDevice final : public engine::IAudioDevice {
public:
    Status start(Callback callback) override {
        callback_ = std::move(callback);
        running_ = true;
        return success();
    }
    void stop() override { running_ = false; }
    [[nodiscard]] bool isRunning() const noexcept override { return running_; }
    [[nodiscard]] std::string name() const override { return "Test"; }
    [[nodiscard]] double sampleRate() const noexcept override { return 48000.0; }
    [[nodiscard]] int bufferSize() const noexcept override { return 256; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return 2; }
    [[nodiscard]] int numInputChannels() const noexcept override { return 2; }
    [[nodiscard]] int latencySamples() const noexcept override { return 256; }

    void run(int blocks, float* peakOut = nullptr) {
        std::vector<float> left(256);
        std::vector<float> right(256);
        float* outputs[2] = {left.data(), right.data()};
        float peak = 0.0f;
        for (int block = 0; block < blocks; ++block) {
            if (callback_)
                callback_(outputs, nullptr, 2, 0, 256);
            peak = std::max(peak, test::peak(left));
        }
        if (peakOut)
            *peakOut = peak;
    }

    /// Runs the callback and keeps every sample, so a live render can be compared
    /// with an offline one sample by sample (not just by peak).
    void runCapture(int blocks, std::vector<float>& leftOut, std::vector<float>& rightOut) {
        std::vector<float> left(256);
        std::vector<float> right(256);
        float* outputs[2] = {left.data(), right.data()};
        leftOut.clear();
        rightOut.clear();
        for (int block = 0; block < blocks; ++block) {
            if (callback_)
                callback_(outputs, nullptr, 2, 0, 256);
            leftOut.insert(leftOut.end(), left.begin(), left.end());
            rightOut.insert(rightOut.end(), right.begin(), right.end());
        }
    }

private:
    Callback callback_;
    bool running_ = false;
};

struct Fixture {
    std::unique_ptr<project::Project> project;
    audio::TrackId trackId = 0;
    audio::ClipId clipId = 0;

    Fixture() {
        project = project::Project::createEmpty("Engine Test");
        project->setSampleRate(48000.0);
        project->mixer().prepare(48000.0, 256, 2);

        trackId = project->mixer().createTrack(audio::TrackKind::Audio, "Guitar");

        std::vector<std::vector<float>> channels(2, std::vector<float>(48000));
        test::fillSine(channels[0], 440.0, 48000.0, 0.5f);
        test::fillSine(channels[1], 440.0, 48000.0, 0.5f);

        audio::Clip clip(project->nextClipId(), audio::ClipType::Audio, "Take");
        clip.setStart(0);
        clip.setLength(48000);
        clip.setStream(graph::MemoryStream::fromPlanar(std::move(channels), 48000.0));
        auto* stored = project->addClip(std::move(clip));
        clipId = stored->id();
        project->mixer().findTrack(trackId)->clips().push_back(clipId);
    }
};

} // namespace

TEST_CASE("A track with a send still plays through its own output", "[engine][regression]") {
    // REGRESSION: the graph builder replaced a node's destination list on every
    // connect(), so attaching a send removed the track's direct out - the dry
    // signal disappeared from the mix. The return here is muted, which makes the
    // failure unambiguous: the track's own path is the only one that can reach the
    // master, so a silent master means the send ate it.
    Fixture fixture;
    audio::Track* track = fixture.project->mixer().findTrack(fixture.trackId);
    REQUIRE(track != nullptr);

    const audio::TrackId busId = fixture.project->mixer().createTrack(audio::TrackKind::Bus, "Return");
    audio::Track* bus = fixture.project->mixer().findTrack(busId);
    REQUIRE(bus != nullptr);
    bus->setMuted(true);

    audio::Send send;
    send.destination = busId;
    send.level = 1.0f;
    send.enabled = true;
    track->addSend(send);

    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));

    const auto* trackNode = engine.currentPlan()->find(track->graphNodeId());
    REQUIRE(trackNode != nullptr);
    REQUIRE(trackNode->destinations().size() == 2); // the direct out and the send

    engine.transport().play();
    // The muted return fades over ~24 ms, so the first blocks still carry a little
    // of the ramp; the measurement starts once it has settled. With the direct
    // output lost, everything after the ramp is silence.
    float settling = 0.0f;
    device.run(16, &settling);
    float peak = 0.0f;
    device.run(16, &peak);
    INFO("master peak " << peak);
    // The clip is a 0.5 sine at a unity fader, so the direct path alone gives 0.5.
    REQUIRE(peak == Approx(0.5f).margin(0.02f));
    // The clip is a 0.5 sine at unity fader, so the direct path alone gives 0.5.
    REQUIRE(peak == Approx(0.5f).margin(0.02f));
}

TEST_CASE("A send plays at the level it is set to", "[engine][sends][regression]") {
    // REGRESSION: Send::level was stored, documented ("sends write into a
    // destination node's input with a level") and never applied - every send played
    // at unity, so a 25 % reverb send arrived as loud as the source.
    //
    // The measurement is the master peak of a 0.5 sine on the track: the dry route
    // contributes 0.5 and the return contributes 0.5 * level, in phase, so the sum
    // is arithmetic rather than a judgement call.
    const auto renderPeak = [](float sendLevel) {
        Fixture fixture;
        audio::Track* track = fixture.project->mixer().findTrack(fixture.trackId);
        REQUIRE(track != nullptr);

        // The master limiter would compress a 1.0 peak, so the arithmetic under test
        // (dry + level * wet) is measured without it: this test is about the send
        // level, and the limiter is verified by its own tests.
        fixture.project->mixer().masterLimiter().setBypassed(true);

        const audio::TrackId busId = fixture.project->mixer().createTrack(audio::TrackKind::Bus, "Return");
        audio::Send send;
        send.destination = busId;
        send.level = sendLevel;
        send.enabled = true;
        track->addSend(send);

        TestDevice device;
        engine::AudioEngine engine;
        engine::EngineSettings settings;
        settings.sampleRate = 48000.0;
        settings.bufferSize = 256;
        engine.setProject(fixture.project.get());
        REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
        REQUIRE(static_cast<bool>(engine.rebuildGraph()));

        engine.transport().play();
        float settling = 0.0f;
        device.run(16, &settling); // the fader ramps settle in ~24 ms
        float peak = 0.0f;
        device.run(16, &peak);
        return peak;
    };

    INFO("unity " << renderPeak(1.0f) << ", half " << renderPeak(0.5f) << ", zero " << renderPeak(0.0f));
    REQUIRE(renderPeak(0.0f) == Approx(0.5f).margin(0.02f));  // dry only
    REQUIRE(renderPeak(0.5f) == Approx(0.75f).margin(0.02f)); // dry + half-level return
    REQUIRE(renderPeak(1.0f) == Approx(1.0f).margin(0.02f));  // dry + unity return
}

TEST_CASE("The engine renders project audio through the graph", "[engine]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    engine.setProject(fixture.project.get());

    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));
    REQUIRE(engine.currentPlan() != nullptr);
    REQUIRE(engine.isRunning());

    float peak = 0.0f;
    engine.transport().play();
    device.run(16, &peak);

    // The clip is a 0.5 sine on a unity fader, so the master peak is 0.5.
    INFO("master peak " << peak);
    REQUIRE(peak == Approx(0.5f).margin(0.02f));

    // Stopping silences the output. The first block after the stop still
    // contains the master limiter's lookahead line (1.5 ms = 72 samples of the
    // audio that was already in flight) - that is physical, not a defect - so the
    // measurement starts once it has drained.
    engine.transport().stop();
    float draining = 0.0f;
    device.run(1, &draining); // flushes the limiter's lookahead
    float stoppedPeak = 0.0f;
    device.run(48, &stoppedPeak);
    REQUIRE(stoppedPeak < 0.001f);

    engine.shutdown();
    REQUIRE_FALSE(engine.isRunning());
}

TEST_CASE("Track fader, pan and mute reach the graph", "[engine][mixer]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));
    engine.transport().play();

    audio::Track* track = fixture.project->mixer().findTrack(fixture.trackId);

    // The fader is ramped over ~10 ms, so settle before measuring.
    track->setVolumeDb(-6.0f);
    float peak = 0.0f;
    device.run(24, &peak);
    device.run(24, &peak);
    REQUIRE(peak == Approx(0.25f).margin(0.02f));

    // A hard pan leaves the left channel at unity and silences the right.
    track->setVolumeDb(0.0f);
    track->setPan(-1.0f);
    device.run(48, &peak);
    {
        std::vector<float> left(256);
        std::vector<float> right(256);
        float* outputs[2] = {left.data(), right.data()};
        engine.processBlock(outputs, nullptr, 256);
        engine.processBlock(outputs, nullptr, 256);
        engine.processBlock(outputs, nullptr, 256);
        REQUIRE(test::peak(left) > 0.4f);
        REQUIRE(test::peak(right) < 0.01f);
    }

    // Mute ramps to silence rather than cutting.
    track->setPan(0.0f);
    track->setMuted(true);
    device.run(48, &peak);
    device.run(24, &peak);
    REQUIRE(peak < 0.01f);
}

TEST_CASE("Reported latency includes the master limiter", "[engine][latency]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));

    const int deviceLatency = device.latencySamples();
    const int limiterLatency = fixture.project->mixer().masterLimiter().latencySamples();
    REQUIRE(engine.totalLatencySamples() == deviceLatency + limiterLatency);
}

TEST_CASE("Offline render reuses the live graph and writes a valid file", "[engine][export]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));

    engine::OfflineRenderer renderer(engine);
    engine::RenderSettings renderSettings;
    renderSettings.sampleRate = 48000.0;
    renderSettings.bitsPerSample = 24;
    renderSettings.dither = false; // deterministic output for the assertions below

    const std::string path = "/tmp/aura-render-test.wav";
    auto result = renderer.renderAll(path, renderSettings);
    REQUIRE(result.hasValue());
    REQUIRE_FALSE(result.value().outputFiles.empty());
    REQUIRE(result.value().framesRendered >= 48000);

    auto info = media::probeFile(path);
    REQUIRE(info.hasValue());
    REQUIRE(info.value().sampleRate == 48000);
    REQUIRE(info.value().numChannels == 2);

    // The rendered audio must be the same signal the live path produces: a
    // 0.5 sine, limited at -0.3 dBFS (so the peak equals 0.5 here, below the
    // ceiling).
    auto planar = media::readFileToPlanar(path);
    REQUIRE(planar.hasValue());
    const float renderedPeak = test::peakFrom(planar.value()->at(0), 2048);
    INFO("rendered peak " << renderedPeak);
    REQUIRE(renderedPeak == Approx(0.5f).margin(0.02f));

    // ... and the same measurement on the live path, for comparison.
    float livePeak = 0.0f;
    engine.transport().play();
    device.run(16, &livePeak);
    REQUIRE(livePeak == Approx(renderedPeak).margin(0.01f));
}

TEST_CASE("Offline render is sample-identical to the live path", "[engine][export][regression]") {
    // The requirement is that export reuses the live DSP graph rather than a second
    // implementation of it. A peak comparison cannot show that - two different
    // reverbs can share a peak - so this compares the samples themselves, over a
    // session built to diverge if anything is duplicated: two tracks, insert chains
    // with a real reverb (stateful, long tail), a send with a level, a bus, and the
    // master limiter engaged.
    constexpr int kBlocks = 40;
    constexpr std::int64_t kFrames = kBlocks * 256;

    // The processors are owned by the caller for the duration of the test; the
    // insert slots hold the raw pointers the graph runs.
    const auto buildSession = [](Fixture& fixture, std::vector<std::unique_ptr<dsp::Processor>>& owners) {
        audio::Track* track = fixture.project->mixer().findTrack(fixture.trackId);
        REQUIRE(track != nullptr);

        auto gain = std::make_unique<dsp::GainProcessor>();
        gain->setGain(0.5f);
        REQUIRE(track->addInsert({"Gain", gain.get(), false, "", 1.0f}));
        owners.push_back(std::move(gain));

        const audio::TrackId busId = fixture.project->mixer().createTrack(audio::TrackKind::Bus, "Space");
        audio::Track* bus = fixture.project->mixer().findTrack(busId);
        REQUIRE(bus != nullptr);
        auto reverb = std::make_unique<dsp::Reverb>();
        dsp::ReverbSettings reverbSettings;
        reverbSettings.mix = 0.35f;
        reverb->setSettings(reverbSettings);
        REQUIRE(bus->addInsert({"Reverb", reverb.get(), false, "", 1.0f}));
        owners.push_back(std::move(reverb));

        audio::Send send;
        send.destination = busId;
        send.level = 0.4f;
        send.enabled = true;
        track->addSend(send);
    };

    // The live path: a device callback driving the same engine entry point the audio
    // thread uses.
    Fixture liveFixture;
    std::vector<std::unique_ptr<dsp::Processor>> liveProcessors;
    buildSession(liveFixture, liveProcessors);
    TestDevice liveDevice;
    engine::AudioEngine liveEngine;
    engine::EngineSettings liveSettings;
    liveSettings.sampleRate = 48000.0;
    liveSettings.bufferSize = 256;
    liveEngine.setProject(liveFixture.project.get());
    REQUIRE(static_cast<bool>(liveEngine.initialise(&liveDevice, liveSettings)));
    REQUIRE(static_cast<bool>(liveEngine.rebuildGraph()));
    liveEngine.transport().play();
    std::vector<float> liveLeft;
    std::vector<float> liveRight;
    liveDevice.runCapture(kBlocks, liveLeft, liveRight);
    REQUIRE(static_cast<std::int64_t>(liveLeft.size()) == kFrames);

    // The offline path: an identical session (same content, fresh state), rendered
    // from 0 to the same frame. Float32 + no dither, so the file is a copy of what
    // the renderer produced rather than a quantised version of it.
    Fixture offlineFixture;
    std::vector<std::unique_ptr<dsp::Processor>> offlineProcessors;
    buildSession(offlineFixture, offlineProcessors);
    TestDevice offlineDevice;
    engine::AudioEngine offlineEngine;
    offlineEngine.setProject(offlineFixture.project.get());
    REQUIRE(static_cast<bool>(offlineEngine.initialise(&offlineDevice, liveSettings)));
    REQUIRE(static_cast<bool>(offlineEngine.rebuildGraph()));

    engine::OfflineRenderer renderer(offlineEngine);
    engine::RenderSettings renderSettings;
    renderSettings.sampleRate = 48000.0;
    renderSettings.blockSize = 256; // the block size the engine is prepared for
    renderSettings.bitsPerSample = 32;
    renderSettings.format = media::SampleFormat::Float32;
    renderSettings.dither = false;
    renderSettings.tailSeconds = 0.0;

    const std::string path = "/tmp/aura-parity-test.wav";
    auto result = renderer.renderRange(path, 0, kFrames, renderSettings);
    REQUIRE(result.hasValue());
    REQUIRE(result.value().framesRendered == kFrames);

    auto planar = media::readFileToPlanar(path);
    REQUIRE(planar.hasValue());
    const auto& renderedChannels = *planar.value();
    REQUIRE(renderedChannels.size() == 2);
    REQUIRE(static_cast<std::int64_t>(renderedChannels.at(0).size()) >= kFrames);

    // Sample by sample. The two paths run the same plan with the same block size and
    // the same prepared state, so a difference here means export is doing its own
    // signal processing somewhere.
    float worst = 0.0f;
    int worstFrame = -1;
    for (std::int64_t frame = 0; frame < kFrames; ++frame) {
        const float rendered = renderedChannels.at(0)[static_cast<std::size_t>(frame)];
        const float difference = std::abs(rendered - liveLeft[static_cast<std::size_t>(frame)]);
        if (difference > worst) {
            worst = difference;
            worstFrame = static_cast<int>(frame);
        }
    }
    INFO("worst sample difference " << worst << " at frame " << worstFrame);
    // Bit-exact, not "close": both paths run the same plan, in the same block size,
    // from the same prepared state, through the same code. A non-zero difference
    // means export has started doing its own signal processing - which is exactly
    // the thing the architecture forbids, so this assertion is deliberately strict.
    REQUIRE(worst == 0.0f);
}

TEST_CASE("Stem rendering produces one file per track", "[engine][export]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));

    engine::OfflineRenderer renderer(engine);
    engine::RenderSettings renderSettings;
    renderSettings.sampleRate = 48000.0;
    renderSettings.bitsPerSample = 24;
    renderSettings.renderStems = true;

    auto result = renderer.renderStems("/tmp/aura-stem-test.wav", 0, 48000, renderSettings);
    REQUIRE(result.hasValue());
    REQUIRE(result.value().outputFiles.size() >= 1);
    for (const auto& file : result.value().outputFiles)
        REQUIRE(filesystem::isRegularFile(file));

    // The solo state is restored after the stem pass.
    for (audio::Track* track : fixture.project->mixer().tracks())
        REQUIRE_FALSE(track->isSoloed());
}

TEST_CASE("The engine performs no allocations on the audio path", "[engine][realtime][regression]") {
    Fixture fixture;
    TestDevice device;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(&device, settings)));
    REQUIRE(static_cast<bool>(engine.rebuildGraph()));
    engine.transport().play();

    // Render several blocks to warm up every lazy path (meters, MIDI scratch,
    // graph plans), then measure. A first-block allocation is not an RT bug; a
    // steady-state allocation is.
    device.run(16);

    rt::setTrackingEnabled(true);
    rt::resetRealtimeAllocationCount();
    rt::enterRealtimeThread();
    rt::ScopedNoDenormals noDenormals;
    for (int block = 0; block < 64; ++block) {
        std::vector<float> left(256);
        std::vector<float> right(256);
        float* outputs[2] = {left.data(), right.data()};
        engine.processBlock(outputs, nullptr, 256);
    }
    rt::exitRealtimeThread();

    INFO("RT violations recorded on the audio path: " << rt::violationCount());
    REQUIRE(rt::violationCount() == 0);
    rt::setTrackingEnabled(false);
}

TEST_CASE("The mock device streams in real time and reports callback health",
          "[engine][audioio]") {
    audioio::AudioDeviceInfo info;
    info.id = "mock:test";
    info.name = "Mock";
    info.driver = audioio::DriverType::Mock;
    info.maxOutputChannels = 2;
    info.maxInputChannels = 2;
    info.defaultSampleRate = 48000.0;
    info.defaultBufferSize = 256;
    info.minimumBufferSize = 32;
    info.maximumBufferSize = 2048;

    auto device = audioio::AudioDeviceManager::createMockDevice(info);
    REQUIRE(device != nullptr);
    REQUIRE(device->name() == "Mock");

    Fixture fixture;
    engine::AudioEngine engine;
    engine::EngineSettings settings;
    engine.setProject(fixture.project.get());
    REQUIRE(static_cast<bool>(engine.initialise(device.get(), settings)));
    engine.transport().play();

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    engine.shutdown();
    REQUIRE_FALSE(device->isRunning());

    auto* mock = static_cast<audioio::MockAudioDevice*>(device.get());
    // 120 ms at 256 frames / 48 kHz is ~22 callbacks; require a healthy count
    // without pinning the exact number (CI machines are noisy).
    INFO("callbacks: " << mock->callbackCount());
    REQUIRE(mock->callbackCount() >= 5);
}

TEST_CASE("Device capability checks reject impossible formats", "[engine][audioio]") {
    audioio::AudioDeviceInfo info;
    info.id = "mock:caps";
    info.name = "Caps";
    info.driver = audioio::DriverType::Mock;
    info.maxOutputChannels = 2;
    info.supportedSampleRates = {44100.0, 48000.0, 96000.0};
    info.minimumBufferSize = 64;
    info.maximumBufferSize = 512;
    info.defaultBufferSize = 256;
    info.defaultSampleRate = 48000.0;

    audioio::AudioDeviceManager manager;
    // The manager's own device list is not populated here; the support query is
    // what the UI uses, so that is what the test pins.
    const auto support = manager.formatSupport("mock:caps");
    REQUIRE(support.supportsSampleRate(44100.0));
    REQUIRE(support.supportsBufferSize(256));
    REQUIRE_FALSE(support.supportsBufferSize(4096));
}
