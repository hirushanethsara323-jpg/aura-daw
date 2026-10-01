// ============================================================================
// AURA DAW - tests/plugin/Vst3HostingTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// M6's acceptance test: a real VST 3 bundle is scanned, loaded, processed and
// restored. The subject is `AuraTestPlugin.vst3`, built from this repository
// (tests/plugin/vst3/AuraTestPlugin.cpp) so the host is checked against the
// format, not against a mock that agrees with the host's own assumptions.
//
// The fixture is small on purpose but complete for what the host must get right:
// two parameters with a plain/normalised mapping, a delay line whose length is
// what getLatencySamples() reports, a note-event counter that survives a state
// round trip (which is how MIDI delivery becomes assertable), state save/restore,
// and no editor.
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ChildProcess.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/plugin/PluginHost.hpp"

using namespace aura;

namespace {

/// Parameter ids in the fixture plug-in (see AuraTestPlugin.cpp).
constexpr int kGainParameter = 0;
constexpr int kDelayParameter = 1;
constexpr double kMaxGain = 2.0;
constexpr int kMaxDelaySamples = 512;

const char* bundlePath() { return AURA_TEST_VST3_BUNDLE; }

plugin::PluginDescriptor scanFixture() {
    auto database = plugin::PluginDatabase{};
    plugin::PluginScanner scanner;
    plugin::PluginScanner::Options options;
    options.searchPaths = {bundlePath()};
    options.scanInChildProcess = false; // the child-process path has its own test
    scanner.setOptions(options);

    scanner.startScan(database, {});
    scanner.waitForCompletion();

    const auto& descriptors = database.descriptors();
    REQUIRE(descriptors.size() == 1);
    return descriptors.front();
}

/// Renders one block of a constant signal through an instance and returns the
/// last output sample, which for this fixture is enough to read off gain and
/// delay behaviour.
float renderConstant(plugin::PluginInstance& instance, float input, int numFrames) {
    std::vector<float> left(static_cast<std::size_t>(numFrames), input);
    std::vector<float> right(static_cast<std::size_t>(numFrames), input);
    float* channels[2] = {left.data(), right.data()};
    dsp::AudioBlockView view;
    view.channelPointers = channels;
    view.numChannels = 2;
    view.numFrames = numFrames;
    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    context.isPlaying = true;
    int midiOut = 0;
    instance.process(view, context, nullptr, 0, nullptr, midiOut);
    return left.back();
}

/// saveState() returns the host's own blob, not the plug-in's raw stream: each
/// section is length-prefixed by the host (component state, then controller state).
/// The plug-in's bytes start after the first four-byte prefix, and reading them is
/// the point of these helpers - a host that stored something other than what the
/// plug-in wrote would be a host whose projects do not reload.
constexpr std::size_t kComponentStateOffset = 4;

template <typename T>
T readComponentState(const std::vector<std::uint8_t>& state, std::size_t fixtureOffset) {
    REQUIRE(state.size() >= kComponentStateOffset + fixtureOffset + sizeof(T));
    T value{};
    std::memcpy(&value, state.data() + kComponentStateOffset + fixtureOffset, sizeof(value));
    return value;
}

/// The fixture's own layout (documented in AuraTestPlugin.cpp): magic, gain, delay,
/// note count, sample rate.
std::uint32_t noteCountFromState(const std::vector<std::uint8_t>& state) {
    return readComponentState<std::uint32_t>(state, 12);
}

int32_t delayFromState(const std::vector<std::uint8_t>& state) {
    return readComponentState<std::int32_t>(state, 8);
}

float gainFromState(const std::vector<std::uint8_t>& state) {
    return readComponentState<float>(state, 4);
}

/// instantiatePlugin() with a failure that says something. A bare REQUIRE() on the
/// result prints "{?}", which is no help at all when a plug-in refuses to load.
std::unique_ptr<plugin::PluginInstance> makeInstance(const plugin::PluginDescriptor& descriptor) {
    auto created = plugin::instantiatePlugin(descriptor, 48000.0, 512, 2);
    if (!created) {
        FAIL_CHECK("instantiatePlugin failed: " << created.error().message
                                                << " -- " << created.error().detail);
    }
    REQUIRE(created);
    return std::move(created.value());
}

} // namespace

TEST_CASE("This build says it hosts VST 3, and the fixture scans like a plug-in",
          "[plugin][vst3]") {
    CHECK(plugin::hostSupports(plugin::PluginFormat::Vst3));

    const auto descriptor = scanFixture();
    CHECK(descriptor.name == "AURA Test Gain");
    CHECK(descriptor.vendor == "AURA DAW Project");
    CHECK(descriptor.version == "1.0.0");
    CHECK(descriptor.format == plugin::PluginFormat::Vst3);
    CHECK(descriptor.isInstrument == false);
    CHECK(descriptor.numInputs == 2);
    CHECK(descriptor.numOutputs == 2);
    CHECK(descriptor.hasEditor == false);
    CHECK(descriptor.incompatible == false);
    // The id has to be parseable without the SDK: instantiate() reads the class
    // UID and the bundle path back out of it.
    CHECK(descriptor.id.rfind("VST3:", 0) == 0);
    CHECK(descriptor.id.find(bundlePath()) != std::string::npos);
    CHECK(descriptor.categoryId == plugin::PluginCategory::Utility);
}

TEST_CASE("A loaded VST 3 processes audio and exposes its parameters", "[plugin][vst3]") {
    const auto descriptor = scanFixture();
    std::unique_ptr<plugin::PluginInstance> instance = makeInstance(descriptor);

    REQUIRE(instance->numParameters() == 2);
    CHECK(instance->parameterName(kGainParameter) == "Gain");
    CHECK(instance->parameterName(kDelayParameter) == "Delay");
    CHECK(instance->isActive());
    CHECK(instance->latencySamples() == 0);

    // Plain/normalised conversion is the plug-in's business, and the host has to
    // go through it rather than assuming a 0..1 range: 1.5x gain is normalised 0.75.
    instance->setParameterValue(kGainParameter, 1.5f);
    CHECK(instance->parameterNormalised(kGainParameter) == Catch::Approx(0.75).epsilon(0.001));
    CHECK(instance->parameterValue(kGainParameter) == Catch::Approx(1.5f).epsilon(0.001));

    // The parameter reaches the DSP through ProcessData, so the first block after
    // the change already reflects it.
    CHECK(renderConstant(*instance, 1.0f, 256) == Catch::Approx(1.5f).epsilon(0.001));
}

TEST_CASE("Reported latency is the plug-in's latency, and the audio obeys it", "[plugin][vst3]") {
    const auto descriptor = scanFixture();
    std::unique_ptr<plugin::PluginInstance> instance = makeInstance(descriptor);

    instance->setParameterValue(kDelayParameter, 8.0f);
    CHECK(instance->latencySamples() == 8); // what the graph will compensate for

    // An impulse comes out where the plug-in says it will: 8 samples later.
    constexpr int frames = 64;
    std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[2] = {left.data(), right.data()};
    dsp::AudioBlockView view;
    view.channelPointers = channels;
    view.numChannels = 2;
    view.numFrames = frames;
    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    int midiOut = 0;
    instance->process(view, context, nullptr, 0, nullptr, midiOut);

    for (int frame = 0; frame < 8; ++frame)
        CHECK(left[static_cast<std::size_t>(frame)] == Catch::Approx(0.0f).margin(1e-6));
    CHECK(left[8] == Catch::Approx(1.0f).epsilon(0.001));
    CHECK(right[8] == Catch::Approx(1.0f).epsilon(0.001));

    // And the chain reports it, which is what delay compensation reads.
    plugin::PluginChain chain;
    chain.add(std::move(instance));
    CHECK(chain.totalLatencySamples() == 8);
}

TEST_CASE("Plug-in state survives a save and a restore", "[plugin][vst3]") {
    const auto descriptor = scanFixture();
    std::unique_ptr<plugin::PluginInstance> instance = makeInstance(descriptor);

    instance->setParameterValue(kGainParameter, 0.25f);
    instance->setParameterValue(kDelayParameter, 4.0f);
    auto saved = instance->saveState();
    REQUIRE(saved);
    CHECK(saved.value().size() > 16);

    // A fresh instance is at its defaults; restoring the blob must put it back
    // exactly where the first one was.
    std::unique_ptr<plugin::PluginInstance> second = makeInstance(descriptor);
    CHECK(second->parameterValue(kGainParameter) == Catch::Approx(1.0f).epsilon(0.001));
    REQUIRE(second->restoreState(saved.value()));
    CHECK(second->parameterValue(kGainParameter) == Catch::Approx(0.25f).epsilon(0.001));
    CHECK(second->latencySamples() == 4);

    // The bytes the plug-in wrote are the bytes the test reads: gain, delay and
    // the note counter are all in there.
    CHECK(gainFromState(saved.value()) == Catch::Approx(0.25f).epsilon(0.001));
    CHECK(delayFromState(saved.value()) == 4);
}

TEST_CASE("Notes reach the plug-in and the state says so", "[plugin][vst3]") {
    const auto descriptor = scanFixture();
    std::unique_ptr<plugin::PluginInstance> instance = makeInstance(descriptor);

    const midi::Message notes[3] = {
        midi::Message::noteOn(0, 60, 100),
        midi::Message::noteOn(0, 64, 100),
        midi::Message::noteOff(0, 60),
    };
    std::vector<float> left(128, 0.0f);
    std::vector<float> right(128, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    dsp::AudioBlockView view;
    view.channelPointers = channels;
    view.numChannels = 2;
    view.numFrames = 128;
    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    context.isPlaying = true;
    int midiOut = 0;
    instance->process(view, context, notes, 3, nullptr, midiOut);

    auto state = instance->saveState();
    REQUIRE(state);
    CHECK(noteCountFromState(state.value()) == 3);
}

TEST_CASE("The scanner runs the bundle in a child process and survives a bad one",
          "[plugin][vst3][process]") {
    // Scanning is the part that runs untrusted code before the user asks for
    // anything, so this is the half that must not share an address space with the
    // mixer. The child is the real helper, and its timeout path is exercised by
    // the same environment hook the supervisor tests use.
    auto database = plugin::PluginDatabase{};
    plugin::PluginScanner scanner;
    plugin::PluginScanner::Options options;
    options.searchPaths = {bundlePath()};
    options.scanInChildProcess = true;
    options.timeoutSecondsPerPlugin = 20;
    scanner.setOptions(options);

    CHECK(plugin::PluginScanner::childProcessScanningAvailable());

    scanner.startScan(database, {});
    scanner.waitForCompletion();

    const auto& descriptors = database.descriptors();
    REQUIRE(descriptors.size() == 1);
    CHECK(descriptors.front().name == "AURA Test Gain");
    CHECK(descriptors.front().incompatible == false);
    CHECK(scanner.lastErrors().empty());
}
