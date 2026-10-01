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
#include <chrono>
#include <thread>

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
