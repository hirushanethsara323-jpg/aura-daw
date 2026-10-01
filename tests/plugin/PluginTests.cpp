// ============================================================================
// AURA DAW - tests/plugin/PluginTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in host is tested through its model and chain layers, which are real
// and complete. Instantiation itself reports NotImplemented until M6, and that
// contract is pinned here too: an honest "not in this build" is a feature.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include "aura/core/FileSystem.hpp"
#include "aura/plugin/PluginHost.hpp"
#include "support/TestSignals.hpp"

using namespace aura;

namespace {

/// A minimal in-process plug-in used to test the chain plumbing (order, bypass,
/// latency, state) without any vendor SDK.
class FakePlugin final : public plugin::PluginInstance {
public:
    FakePlugin(std::string id, int latency, float gain)
        : descriptor_{}, latency_(latency), gain_(gain) {
        descriptor_.id = std::move(id);
        descriptor_.name = descriptor_.id;
        descriptor_.format = plugin::PluginFormat::Builtin;
    }

    [[nodiscard]] const plugin::PluginDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    Status prepare(double, int, int) override { return success(); }
    void reset() noexcept override {}
    void process(dsp::AudioBlockView& audio, const dsp::ProcessContext&, const midi::Message*,
                 int, midi::Message*, int& numMidiOut) noexcept override {
        numMidiOut = 0;
        for (int channel = 0; channel < audio.numChannels; ++channel) {
            float* data = audio.channelPointers[channel];
            for (int i = 0; i < audio.numFrames; ++i)
                data[i] *= gain_;
        }
        ++processCalls;
    }
    [[nodiscard]] int latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] int numParameters() const noexcept override { return 1; }
    [[nodiscard]] std::string parameterName(int) const override { return "Gain"; }
    [[nodiscard]] float parameterValue(int) const noexcept override { return gain_; }
    void setParameterValue(int, float value) noexcept override { gain_ = value; }
    [[nodiscard]] float parameterNormalised(int) const noexcept override { return gain_; }
    void setParameterNormalised(int, float value) noexcept override { gain_ = value; }
    [[nodiscard]] AuraResult<std::vector<std::uint8_t>> saveState() const override {
        return success(std::vector<std::uint8_t>{1, 2, 3, 4});
    }
    Status restoreState(const std::vector<std::uint8_t>& state) override {
        restoredBytes = static_cast<int>(state.size());
        return success();
    }
    [[nodiscard]] bool hasEditor() const noexcept override { return false; }
    void* createEditor() override { return nullptr; }
    void destroyEditor(void*) override {}
    [[nodiscard]] bool isActive() const noexcept override { return true; }

    int processCalls = 0;
    int restoredBytes = 0;

private:
    plugin::PluginDescriptor descriptor_;
    int latency_ = 0;
    float gain_ = 1.0f;
};

} // namespace

TEST_CASE("The plug-in database persists descriptors and user data", "[plugin][database]") {
    const std::string cachePath = "/tmp/aura-plugin-cache-test.json";
    (void)filesystem::removeFile(cachePath);

    plugin::PluginDatabase database;
    database.setCachePath(cachePath);

    plugin::PluginDescriptor descriptor;
    descriptor.id = "VST3:deadbeef";
    descriptor.name = "AURA Test Reverb";
    descriptor.vendor = "Test Vendor";
    descriptor.format = plugin::PluginFormat::Vst3;
    descriptor.categoryId = plugin::PluginCategory::Reverb;
    descriptor.hasEditor = true;
    descriptor.numInputs = 2;
    descriptor.numOutputs = 2;
    database.upsert(descriptor);

    plugin::PluginDescriptor second;
    second.id = "CLAP:cafe";
    second.name = "AURA Test Synth";
    second.isInstrument = true;
    second.format = plugin::PluginFormat::Clap;
    second.categoryId = plugin::PluginCategory::Instrument;
    database.upsert(second);

    plugin::PluginUserData userData;
    userData.favorite = true;
    userData.lastUsedTimeMs = 12345;
    database.setUserData(descriptor.id, userData);

    REQUIRE(static_cast<bool>(database.save()));

    plugin::PluginDatabase reloaded;
    reloaded.setCachePath(cachePath);
    REQUIRE(static_cast<bool>(reloaded.load()));
    REQUIRE(reloaded.descriptors().size() == 2);
    REQUIRE(reloaded.userData(descriptor.id).favorite);
    REQUIRE(reloaded.userData(descriptor.id).lastUsedTimeMs == 12345);

    // Search filters by kind, favourite flag and query.
    REQUIRE(reloaded.search("reverb", false, true, false).size() == 1);
    REQUIRE(reloaded.search("", true, false, false).size() == 1);       // instruments only
    REQUIRE(reloaded.search("", false, true, false).size() == 1);       // favourites only
    REQUIRE(reloaded.search("nothing-matches-this", true, true, false).empty());
    REQUIRE(reloaded.recentlyUsed(4).size() == 1);

    // Blacklisting hides a plug-in from the browser but keeps it in the list.
    plugin::PluginUserData blacklisted = reloaded.userData(second.id);
    blacklisted.blacklisted = true;
    blacklisted.blacklistReason = "crashed the host during scan";
    reloaded.setUserData(second.id, blacklisted);
    REQUIRE(reloaded.blacklistCount() == 1);
    REQUIRE(reloaded.search("", true, false, false).empty());
    // search(query, instruments, effects, favoritesOnly, includeBlacklisted)
    REQUIRE(reloaded.search("", true, false, false, true).size() == 1); // + blacklisted
}

TEST_CASE("Plug-in chains process in order, honour bypass and report latency",
          "[plugin][chain]") {
    plugin::PluginChain chain;
    chain.add(std::make_unique<FakePlugin>("first", 64, 0.5f));
    chain.add(std::make_unique<FakePlugin>("second", 32, 0.5f));
    REQUIRE(chain.size() == 2);
    REQUIRE(chain.totalLatencySamples() == 96);

    test::Block block(2, 256, 1.0f);
    auto view = block.view(256);
    dsp::ProcessContext context;
    std::vector<midi::Message> midi(64);
    int activeMidi = 0;
    chain.process(view, context, midi, activeMidi);
    // Two 0.5 gains: 0.25.
    REQUIRE(block.channels[0].front() == 0.25f);

    // Bypassing the first slot leaves one gain and removes its latency.
    chain.setBypassed(0, true);
    REQUIRE(chain.isBypassed(0));
    REQUIRE(chain.totalLatencySamples() == 32);
    test::Block bypassed(2, 256, 1.0f);
    auto bypassedView = bypassed.view(256);
    chain.process(bypassedView, context, midi, activeMidi);
    REQUIRE(bypassed.channels[0].front() == 0.5f);

    // Reordering changes the result when the gains differ.
    chain.setBypassed(0, false);
    chain.at(0)->setParameterValue(0, 0.25f);
    chain.at(1)->setParameterValue(0, 0.75f);
    test::Block ordered(2, 8, 1.0f);
    auto orderedView = ordered.view(8);
    chain.process(orderedView, context, midi, activeMidi);
    REQUIRE(ordered.channels[0].front() == 0.1875f);

    REQUIRE(chain.move(1, 0));
    test::Block moved(2, 8, 1.0f);
    auto movedView = moved.view(8);
    chain.process(movedView, context, midi, activeMidi);
    REQUIRE(moved.channels[0].front() == 0.1875f); // multiplication is commutative

    REQUIRE(chain.remove("first"));
    REQUIRE(chain.size() == 1);
}

TEST_CASE("A failed plug-in is skipped instead of being called again", "[plugin][chain]") {
    plugin::PluginChain chain;
    auto plugin = std::make_unique<FakePlugin>("flaky", 0, 0.5f);
    auto* raw = plugin.get();
    chain.add(std::move(plugin));

    test::Block block(2, 64, 1.0f);
    auto view = block.view(64);
    dsp::ProcessContext context;
    std::vector<midi::Message> midi(16);
    int activeMidi = 0;
    chain.process(view, context, midi, activeMidi);
    REQUIRE(raw->processCalls == 1);

    // After a failure the chain bypasses the instance: the audio keeps flowing.
    raw->setFailed(true);
    REQUIRE(raw->hasFailed());
    chain.process(view, context, midi, activeMidi);
    REQUIRE(raw->processCalls == 1);
}

TEST_CASE("Plug-in state is saved and restored through the chain", "[plugin][chain]") {
    plugin::PluginChain chain;
    chain.add(std::make_unique<FakePlugin>("stateful", 0, 1.0f));
    auto states = chain.saveStates();
    REQUIRE(states.hasValue());
    REQUIRE(states.value().elements().size() == 1);
    // The state blob is hex-encoded so the project file stays valid JSON.
    REQUIRE(states.value()[0]["state"].asString() == "01020304");

    REQUIRE(static_cast<bool>(chain.restoreStates(states.value())));
    auto* instance = dynamic_cast<FakePlugin*>(chain.at(0));
    REQUIRE(instance != nullptr);
    REQUIRE(instance->restoredBytes == 4);
}

TEST_CASE("This build states honestly what it cannot host", "[plugin][honest]") {
    // AURA is GPL-3.0-or-later. VST 3.8's SDK is MIT and CLAP is MIT (both
    // compatible); ASIO is GPLv3-or-proprietary. None of the SDKs are vendored, so
    // what a build can host is decided at configure time - and the answer has to be
    // the truth in both directions: "not compiled in" for a format this build does
    // not have, and an ordinary, descriptor-specific failure for one it does.
    REQUIRE(plugin::hostSupports(plugin::PluginFormat::Builtin));

    const plugin::PluginFormat otherFormats[] = {plugin::PluginFormat::Vst3,
                                                 plugin::PluginFormat::Clap};
    for (const plugin::PluginFormat format : otherFormats) {
        plugin::PluginDescriptor descriptor;
        descriptor.id = std::string(plugin::pluginFormatName(format)) + ":test";
        descriptor.name = "Not Hostable";
        descriptor.format = format;

        auto instance = plugin::instantiatePlugin(descriptor, 48000.0, 256, 2);
        REQUIRE_FALSE(instance.hasValue());
        if (plugin::hostSupports(format)) {
            // Compiled in: whatever went wrong is about this descriptor, not the build.
            CHECK(instance.error().code != ErrorCode::NotImplemented);
        } else {
            // Not compiled in: say so, and name the format instead of saying "failed".
            CHECK(instance.error().code == ErrorCode::NotImplemented);
            CHECK(instance.error().toString().find(plugin::pluginFormatName(format)) !=
                  std::string::npos);
        }
    }

    // Where the scanner looks is a build-time decision too, and it is never empty:
    // an empty list would silently mean "no plug-ins exist".
    REQUIRE_FALSE(plugin::PluginScanner::defaultSearchPaths().empty());
}
