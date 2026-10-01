// ============================================================================
// AURA DAW - tests/graph/GraphTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Graph correctness tests. The first test in this file is a REGRESSION test: it
// pins the bug where a destination node was never cleared, so every block summed
// on top of the previous one (audio grew until it clipped).
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <chrono>
#include <thread>

#include "aura/dsp/Compressor.hpp"
#include "aura/dsp/GainProcessor.hpp"
#include "aura/graph/AudioGraph.hpp"
#include "aura/graph/SampleStream.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {

/// A node that produces a constant value, so a block's output is trivially
/// predictable.
void fillConstant(graph::GraphNode& node, int frames, float value) {
    for (int channel = 0; channel < node.numChannels(); ++channel) {
        float* data = node.mutableChannelPointers()[channel];
        for (int i = 0; i < frames; ++i)
            data[i] = value;
    }
    node.setInputActive(true);
}

float readNode(const graph::GraphNode& node, int channel, int frame) {
    return node.channelPointers()[channel][frame];
}

} // namespace

TEST_CASE("A destination node is cleared every block (no accumulation)", "[graph][regression]") {
    graph::GraphBuilder builder;
    const auto sourceId = builder.createNode(graph::NodeKind::Track, "Source");
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(sourceId, masterId);
    builder.setMasterNode(masterId);

    auto plan = builder.build();
    REQUIRE(plan != nullptr);
    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 64, 2);

    auto* source = plan->find(sourceId);
    auto* master = plan->find(masterId);
    REQUIRE(source != nullptr);
    REQUIRE(master != nullptr);
    REQUIRE(master->incomingConnections() == 1);

    dsp::ProcessContext context;
    context.sampleRate = 48000.0;

    // Ten blocks of a constant 0.5 must leave the master at 0.5 every time.
    // (Before the fix, block N read 0.5 * N and the engine clipped.)
    for (int block = 0; block < 10; ++block) {
        fillConstant(*source, 64, 0.5f);
        plan->process(64, context, false);
        INFO("block " << block);
        REQUIRE(readNode(*master, 0, 63) == Approx(0.5f).margin(1e-6f));
        REQUIRE(readNode(*master, 1, 63) == Approx(0.5f).margin(1e-6f));
    }
}

TEST_CASE("A node keeps every destination it is connected to (fan-out)", "[graph][regression]") {
    // REGRESSION: connect() used to *replace* the source's destination list, so the
    // second connection erased the first. Fan-out is the normal case in a DAW - a
    // track's own output plus one or more sends - so in the engine a send silently
    // deleted the track's main output and the dry signal vanished from the mix.
    graph::GraphBuilder builder;
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    const auto busId = builder.createNode(graph::NodeKind::Bus, "Reverb");
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Vocal");
    builder.connect(trackId, masterId); // the direct out
    builder.connect(trackId, busId);    // the send
    builder.connect(busId, masterId);
    builder.setMasterNode(masterId);

    auto plan = builder.build();
    REQUIRE(plan != nullptr);

    const auto* track = plan->find(trackId);
    REQUIRE(track != nullptr);
    REQUIRE(track->destinations().size() == 2);

    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 64, 2);

    auto* source = plan->find(trackId);
    auto* master = plan->find(masterId);
    REQUIRE(source != nullptr);
    REQUIRE(master != nullptr);

    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    fillConstant(*source, 64, 0.5f);
    plan->process(64, context, false);

    // Both routes carry the signal: 0.5 direct + 0.5 through the bus = 1.0. With the
    // direct connection lost, this read 0.5 (and in the engine, with a muted return,
    // it read 0.0).
    REQUIRE(readNode(*master, 0, 63) == Approx(1.0f).margin(1e-6f));
    REQUIRE(readNode(*master, 1, 63) == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("An edge gain scales the connection it belongs to", "[graph][sends]") {
    // A send is a connection with a level, so the gain lives on the edge: the same
    // source feeds the master at unity and a bus at -6 dB, and only the bus's copy
    // is attenuated.
    graph::GraphBuilder builder;
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    const auto auxId = builder.createNode(graph::NodeKind::Bus, "Aux");
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Vocal");
    builder.connect(trackId, masterId, 1.0f);   // the dry route
    builder.connect(trackId, auxId, 0.25f);     // the send, at -12 dB
    builder.connect(auxId, masterId);
    builder.setMasterNode(masterId);

    auto plan = builder.build();
    REQUIRE(plan != nullptr);
    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 64, 2);

    auto* source = plan->find(trackId);
    auto* master = plan->find(masterId);
    REQUIRE(source != nullptr);
    REQUIRE(master != nullptr);

    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    fillConstant(*source, 64, 0.5f);
    plan->process(64, context, false);

    // 0.5 dry + 0.5 * 0.25 through the aux = 0.625. Ignoring the gain gave 1.0.
    REQUIRE(readNode(*master, 0, 63) == Approx(0.625f).margin(1e-6f));

    SECTION("a zero-level send contributes nothing at all") {
        // Distinct names from the enclosing scope: the CI build uses -Werror=shadow,
        // and a shadowed `master` here is a warning, not a style preference.
        graph::GraphBuilder mutedBuilder;
        const auto mutedMaster = mutedBuilder.createNode(graph::NodeKind::Master, "Master");
        const auto mutedAux = mutedBuilder.createNode(graph::NodeKind::Bus, "Aux");
        const auto mutedTrack = mutedBuilder.createNode(graph::NodeKind::Track, "Vocal");
        mutedBuilder.connect(mutedTrack, mutedMaster, 1.0f);
        mutedBuilder.connect(mutedTrack, mutedAux, 0.0f);
        mutedBuilder.connect(mutedAux, mutedMaster);
        mutedBuilder.setMasterNode(mutedMaster);

        auto mutedPlan = mutedBuilder.build();
        REQUIRE(mutedPlan != nullptr);
        for (const auto& node : mutedPlan->nodes())
            node->prepare(48000.0, 64, 2);
        fillConstant(*mutedPlan->find(mutedTrack), 64, 0.5f);
        mutedPlan->process(64, context, false);
        REQUIRE(readNode(*mutedPlan->find(mutedMaster), 0, 63) == Approx(0.5f).margin(1e-6f));
    }
}

TEST_CASE("A pre-fader edge reads the tap, not the fader", "[graph][sends]") {
    // The cue-mix property: the same source feeds the master through its fader and a
    // cue bus before it. Moving the fader must change only the first.
    graph::GraphBuilder builder;
    const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
    const auto cueId = builder.createNode(graph::NodeKind::Bus, "Cue");
    const auto trackId = builder.createNode(graph::NodeKind::Track, "Vocal");
    builder.connect(trackId, masterId, 1.0f, /*preFader=*/false);
    builder.connect(trackId, cueId, 1.0f, /*preFader=*/true);
    builder.connect(cueId, masterId);
    builder.setMasterNode(masterId);

    auto plan = builder.build();
    REQUIRE(plan != nullptr);

    // An insert with a known gain, so the test can tell *where* the tap sits: the
    // cue bus must see the insert (post-insert tap) but not the fader.
    auto insertGain = std::make_unique<dsp::GainProcessor>();
    insertGain->setGain(0.25f);
    plan->find(trackId)->inserts().add(insertGain.get());

    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 128, 2);

    auto* source = plan->find(trackId);
    auto* cue = plan->find(cueId);
    auto* master = plan->find(masterId);
    REQUIRE(source != nullptr);
    REQUIRE(cue != nullptr);
    REQUIRE(master != nullptr);
    REQUIRE(source->preFaderTapRequired());
    REQUIRE(source->hasPreFaderTap());

    source->setGain(0.5f); // the fader, applied at the end of the node's chain

    dsp::ProcessContext context;
    context.sampleRate = 48000.0;

    // Run enough blocks for the ramps to settle, then measure. The ramped fader is
    // deliberate (no clicks), so the first blocks are not the steady state.
    for (int block = 0; block < 40; ++block) {
        fillConstant(*source, 128, 0.5f);
        plan->process(128, context, false);
    }
    const float tap = readNode(*cue, 0, 127);
    INFO("master " << readNode(*master, 0, 127) << ", tap " << tap);
    // The tap is post-insert, pre-fader: 0.5 source * 0.25 insert = 0.125, and the
    // fader must not appear in it.
    REQUIRE(tap == Approx(0.125f).margin(1e-3f));
    // The master sums both routes: post-fader 0.5 * 0.25 * 0.5 = 0.0625, plus the
    // tap's 0.125 = 0.1875. A tap taken before the insert would make this 0.5625; a
    // tap taken after the fader (i.e. not a pre-fader send at all) would make it
    // 0.125.
    REQUIRE(readNode(*master, 0, 127) == Approx(0.1875f).margin(1e-3f));
}

TEST_CASE("The fader is applied after the inserts, so it cannot re-voice them",
          "[graph][regression]") {
    // REGRESSION: the fader used to be applied before the insert chain, so moving it
    // changed what the inserts saw. For a linear insert that is invisible; for a
    // non-linear one it rewrites the sound - a saturator driven harder, a compressor
    // crossing its threshold. The check is arithmetic: halving the fader has to halve
    // the output exactly, which is only true if the insert saw the same input.
    const auto renderWithFader = [](float fader) {
        graph::GraphBuilder builder;
        const auto masterId = builder.createNode(graph::NodeKind::Master, "Master");
        const auto trackId = builder.createNode(graph::NodeKind::Track, "Bass");
        builder.connect(trackId, masterId);
        builder.setMasterNode(masterId);

        auto plan = builder.build();
        REQUIRE(plan != nullptr);

        auto compressor = std::make_unique<dsp::Compressor>();
        dsp::CompressorSettings settings;
        settings.thresholdDb = -30.0f; // well into gain reduction at these levels
        settings.ratio = 8.0f;
        settings.attackMs = 0.5f;
        settings.releaseMs = 50.0f;
        settings.autoMakeup = false;
        compressor->setSettings(settings);
        plan->find(trackId)->inserts().add(compressor.get());

        for (const auto& node : plan->nodes())
            node->prepare(48000.0, 128, 2);

        auto* source = plan->find(trackId);
        auto* master = plan->find(masterId);
        REQUIRE(source != nullptr);
        REQUIRE(master != nullptr);
        source->setGain(fader);

        dsp::ProcessContext context;
        context.sampleRate = 48000.0;
        for (int block = 0; block < 64; ++block) { // let the compressor and the ramp settle
            fillConstant(*source, 128, 0.5f);
            plan->process(128, context, false);
        }
        return readNode(*master, 0, 127);
    };

    const float unity = renderWithFader(1.0f);
    const float halved = renderWithFader(0.5f);
    INFO("unity " << unity << ", halved " << halved);
    REQUIRE(unity > 0.0f);
    // Exactly half. With the fader in front of the compressor the second render is
    // louder than this: the compressor would be working on a quieter signal.
    REQUIRE(halved == Approx(unity * 0.5f).epsilon(0.001f));
}

TEST_CASE("Topological order puts sources before destinations", "[graph]") {
    graph::GraphBuilder builder;
    const auto master = builder.createNode(graph::NodeKind::Master, "Master");
    const auto bus = builder.createNode(graph::NodeKind::Bus, "Drums");
    const auto track = builder.createNode(graph::NodeKind::Track, "Kick");
    builder.connect(track, bus);
    builder.connect(bus, master);
    builder.setMasterNode(master);

    auto plan = builder.build();
    REQUIRE(plan != nullptr);
    const auto& order = plan->processingOrder();
    REQUIRE(order.size() == 3);

    const auto positionOf = [&order](graph::NodeId id) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == id)
                return i;
        }
        return order.size();
    };
    REQUIRE(positionOf(track) < positionOf(bus));
    REQUIRE(positionOf(bus) < positionOf(master));
}

TEST_CASE("Cycles are broken deterministically and reported", "[graph]") {
    graph::GraphBuilder builder;
    const auto a = builder.createNode(graph::NodeKind::Bus, "A");
    const auto b = builder.createNode(graph::NodeKind::Bus, "B");
    builder.connect(a, b);
    builder.connect(b, a); // feedback loop: illegal in an audio graph

    std::string error;
    auto plan = builder.build(&error);
    REQUIRE(plan != nullptr);
    REQUIRE_FALSE(error.empty()); // the user (and the log) are told
    REQUIRE(plan->processingOrder().size() == 2);
}

TEST_CASE("Node faders ramp instead of stepping", "[graph][gain]") {
    graph::GraphBuilder builder;
    const auto source = builder.createNode(graph::NodeKind::Track, "Track");
    const auto master = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(source, master);
    builder.setMasterNode(master);
    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 256, 1);

    auto* sourceNode = plan->find(source);
    dsp::ProcessContext context;
    context.sampleRate = 48000.0;

    fillConstant(*sourceNode, 256, 1.0f);
    plan->process(256, context, false);
    REQUIRE(sourceNode->currentGain(0) == Approx(1.0f));

    // Ask for silence: the gain must walk down, not jump.
    sourceNode->setTargetGains(0.0f, 0.0f);
    float previousGain = 1.0f;
    for (int block = 0; block < 12; ++block) {
        fillConstant(*sourceNode, 256, 1.0f);
        plan->process(256, context, false);
        const float current = sourceNode->currentGain(0);
        REQUIRE(current <= previousGain + 1e-6f); // monotonic towards the target
        previousGain = current;
    }
    // 12 blocks = 3072 samples; a 10 ms ramp at 48 kHz is ~480 samples, so the
    // fader must be essentially silent by now.
    REQUIRE(previousGain < 0.01f);
}

TEST_CASE("Insert chains run in order and respect bypass", "[graph][inserts]") {
    graph::GraphBuilder builder;
    const auto track = builder.createNode(graph::NodeKind::Track, "Track");
    const auto master = builder.createNode(graph::NodeKind::Master, "Master");
    builder.connect(track, master);
    builder.setMasterNode(master);
    auto plan = builder.build();
    for (const auto& node : plan->nodes())
        node->prepare(48000.0, 128, 2);

    dsp::GainProcessor firstGain;
    dsp::GainProcessor secondGain;
    firstGain.prepare(48000.0, 128, 2);
    secondGain.prepare(48000.0, 128, 2);
    firstGain.setGainDb(-6.0f);
    secondGain.setGainDb(-6.0f);

    auto* trackNode = plan->find(track);
    trackNode->inserts().add(&firstGain);
    trackNode->inserts().add(&secondGain);
    REQUIRE(trackNode->inserts().size() == 2);

    dsp::ProcessContext context;
    context.sampleRate = 48000.0;
    // The node faders ramp (10 ms = 480 samples), so the first blocks are not at
    // their target gain yet: settle them before measuring.
    for (int block = 0; block < 16; ++block) {
        fillConstant(*trackNode, 128, 1.0f);
        plan->process(128, context, false);
    }
    fillConstant(*trackNode, 128, 1.0f);
    plan->process(128, context, false);

    // Two -6 dB stages: 0.5012^2 = 0.2512.
    REQUIRE(readNode(*plan->find(master), 0, 127) == Approx(0.2512f).margin(0.005f));

    // Bypassing one gain leaves a single -6 dB stage.
    firstGain.setBypassed(true);
    fillConstant(*trackNode, 128, 1.0f);
    plan->process(128, context, false);
    REQUIRE(readNode(*plan->find(master), 0, 127) == Approx(0.5012f).margin(0.01f));
}

TEST_CASE("Memory streams read sequentially and report the end", "[graph][stream]") {
    std::vector<std::vector<float>> channels(2, std::vector<float>(1000));
    for (int i = 0; i < 1000; ++i) {
        channels[0][static_cast<std::size_t>(i)] = static_cast<float>(i);
        channels[1][static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.5f;
    }
    auto stream = graph::MemoryStream::fromPlanar(std::move(channels), 48000.0);
    REQUIRE(stream != nullptr);
    REQUIRE(stream->numChannels() == 2);
    REQUIRE(stream->lengthFrames() == 1000);
    REQUIRE_FALSE(stream->isStreaming());

    std::vector<float> left(400);
    std::vector<float> right(400);
    float* destinations[2] = {left.data(), right.data()};

    REQUIRE(stream->read(destinations, 400) == 400);
    REQUIRE(left[0] == Approx(0.0f));
    REQUIRE(left[399] == Approx(399.0f));
    REQUIRE(right[10] == Approx(5.0f));

    REQUIRE(stream->read(destinations, 400) == 400);
    REQUIRE(left[0] == Approx(400.0f));
    REQUIRE(stream->read(destinations, 400) == 200); // end of stream
    REQUIRE(stream->read(destinations, 400) == 0);

    stream->seek(0);
    REQUIRE(stream->read(destinations, 10) == 10);
    REQUIRE(left[0] == Approx(0.0f));
}

TEST_CASE("readWithSilence zero-fills past the end", "[graph][stream]") {
    std::vector<std::vector<float>> channels(1, std::vector<float>(10, 1.0f));
    auto stream = graph::MemoryStream::fromPlanar(std::move(channels), 48000.0);
    std::vector<float> buffer(32, 9.0f);
    float* destinations[1] = {buffer.data()};

    const int produced = stream->readWithSilence(destinations, 32);
    REQUIRE(produced == 10);
    REQUIRE(buffer[9] == Approx(1.0f));
    REQUIRE(buffer[10] == Approx(0.0f));
    REQUIRE(buffer[31] == Approx(0.0f));
}

TEST_CASE("Disk streams read a real file from a worker thread", "[graph][stream][io]") {
    // Write a known WAV, then stream it back through the disk path so the reader
    // thread, the ring buffer and seek() are all exercised.
    const std::string path = "/tmp/aura-stream-test.wav";
    std::vector<std::vector<float>> channels(2, std::vector<float>(20000));
    test::fillSine(channels[0], 440.0, 48000.0, 0.25f);
    test::fillSine(channels[1], 440.0, 48000.0, 0.25f);

    media::AudioFileWriter::Settings settings;
    settings.sampleRate = 48000;
    settings.numChannels = 2;
    settings.bitsPerSample = 24;
    REQUIRE(static_cast<bool>(media::writePlanarToFile(path, channels, 48000, settings)));

    // memoryThresholdSeconds = 0 forces the disk path even for a short file (the
    // default would load anything under 30 s into memory, which is what makes the
    // default fast for edits - but it is not what this test is about).
    auto streamResult = graph::openSampleStream(path, /*preferMemory=*/false,
                                                /*memoryThresholdSeconds=*/0.0);
    REQUIRE(streamResult.hasValue());
    auto stream = streamResult.value();
    REQUIRE(stream != nullptr);
    REQUIRE(stream->isStreaming());

    std::vector<float> left(1024);
    std::vector<float> right(1024);
    float* destinations[2] = {left.data(), right.data()};

    // The reader thread may not have filled the ring yet: retry briefly rather
    // than asserting on a race.
    int produced = 0;
    for (int attempt = 0; attempt < 200 && produced == 0; ++attempt) {
        produced = stream->read(destinations, 1024);
        if (produced == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(produced == 1024);
    REQUIRE(test::allFinite(left));

    // A seek forwards must be honoured without re-reading from the start.
    stream->seek(15000);
    produced = 0;
    for (int attempt = 0; attempt < 200 && produced == 0; ++attempt) {
        produced = stream->read(destinations, 1024);
        if (produced == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(produced > 0);

    stream->close();
}
