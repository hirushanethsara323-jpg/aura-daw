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
