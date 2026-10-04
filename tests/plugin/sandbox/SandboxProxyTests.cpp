// ============================================================================
// AURA DAW - tests/plugin/sandbox/SandboxProxyTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// M7 phase 3: the proxy. What has to be true here is narrower than it looks, and
// the cases are ordered by it:
//
//   1. a sandboxed slot returns the same audio the processor would, one block later;
//   2. parameters and notes cross;
//   3. a helper that stops leaves the slot DRY - not silent, not hung, not crashed;
//   4. a PluginChain cannot tell the difference, which is the whole reason
//      PluginInstance was an interface before there was anything to put behind it.
//
// Every case here spawns a real helper process. That is not thoroughness for its own
// sake: the point of the milestone is that the audio happens somewhere else, and a
// test that stubbed the other side would be testing the stub.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/PluginHost.hpp"
#include "aura/plugin/sandbox/HelperProtocol.hpp"
#include "aura/plugin/sandbox/SandboxSession.hpp"
#include "aura/plugin/sandbox/SandboxedPluginInstance.hpp"
#include "support/TestSignals.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace aura;
using namespace aura::plugin;
using namespace aura::plugin::sandbox;

namespace {

constexpr int kBlockSize = 128;
constexpr int kChannels = 2;
constexpr double kSampleRate = 48000.0;
/// The reference processor maps normalised 0..1 onto a gain of 0..2, so a quarter
/// turn is a half gain - and a half is exact in binary floating point, which is what
/// lets the comparisons below be `==` instead of "close enough".
constexpr float kQuarterTurn = 0.25f;
constexpr float kHalfGain = 0.5f;

/// How long a test waits between blocks.
///
/// A real host calls process() once per device callback - every 2.7 ms at 128 frames
/// and 48 kHz - and the helper is paced by that too. Calling it back to back with no
/// gap is not a harsher test, it is a different one: the host outruns a peer that is
/// in another process and has not been scheduled yet, and what gets measured is the
/// test runner's loop rather than the transport. One millisecond is well inside any
/// block period AURA supports and well outside the helper's 200 us idle reaction.
constexpr auto kBlockPace = std::chrono::milliseconds(1);

ProxyConfig gainProxyConfig(std::uint64_t maxBlocks = 0) {
    ProxyConfig config;
    config.descriptor.id = "aura.test.sandbox.gain";
    config.descriptor.name = "Sandbox Reference Gain";
    config.descriptor.vendor = "AURA";
    config.descriptor.format = PluginFormat::Unknown;
    config.processorId = processorId::kGain;
    config.parameterCount = 1;
    config.parameterNames = {"Gain"};
    config.readyTimeoutSeconds = 10.0;
    config.maxBlocks = maxBlocks;
    return config;
}

dsp::ProcessContext contextAt(std::int64_t block, bool playing = true) {
    dsp::ProcessContext context;
    context.sampleRate = kSampleRate;
    context.tempoBpm = 120.0;
    context.positionSamples = block * kBlockSize;
    context.positionTicks = context.positionSamples * 2;
    context.isPlaying = playing;
    return context;
}

/// A signal that depends on the block number, so a block returned out of order,
/// twice, or one block early is wrong in a way a listener could hear.
float signalAt(std::int64_t block, int channel, int frame) noexcept {
    const std::int64_t index = block * kBlockSize + channel * 31 + frame;
    const std::int64_t step = ((index % 97) + 97) % 97;
    return (static_cast<float>(step) / 97.0f) * 0.8f - 0.4f;
}

void fillBlock(test::Block& block, std::int64_t index) {
    for (int channel = 0; channel < kChannels; ++channel) {
        for (int frame = 0; frame < kBlockSize; ++frame)
            block.channels[static_cast<std::size_t>(channel)]
                       [static_cast<std::size_t>(frame)] = signalAt(index, channel, frame);
    }
}

bool blockEquals(const test::Block& block, std::int64_t index, float gain) noexcept {
    for (int channel = 0; channel < kChannels; ++channel) {
        for (int frame = 0; frame < kBlockSize; ++frame) {
            const float expected = signalAt(index, channel, frame) * gain;
            const float actual = block.channels[static_cast<std::size_t>(channel)]
                                          [static_cast<std::size_t>(frame)];
            if (actual != expected)
                return false;
        }
    }
    return true;
}

bool blockIsDry(const test::Block& block, std::int64_t index) noexcept {
    return blockEquals(block, index, 1.0f);
}

/// Makes a proxy and prepares it, failing the case loudly if it could not start.
/// Split out because eight cases need it and none of them is about it.
std::unique_ptr<SandboxedPluginInstance> makePrepared(ProxyConfig config) {
    auto made = SandboxedPluginInstance::create(std::move(config));
    REQUIRE(made.hasValue());
    std::unique_ptr<SandboxedPluginInstance> instance = std::move(made.value());
    const Status prepared = instance->prepare(kSampleRate, kBlockSize, kChannels);
    if (!prepared.hasValue()) {
        INFO("helper said: " << instance->lastReport().failure);
        INFO("exit code: " << instance->lastReport().exitCode);
    }
    REQUIRE(prepared.hasValue());
    return instance;
}

/// Feeds blocks until the returned audio shows a parameter change has landed.
///
/// A parameter is published the moment it is set, and the helper drains parameters
/// before it pops a block - so it lands within a block or two, but *which* block is
/// a scheduling fact and not a protocol one. Pumping until it is visible keeps the
/// assertions that follow about the transport rather than about the runner.
template <typename Action>
bool pumpUntil(SandboxedPluginInstance& instance, Action&& action, int maxBlocks) {
    test::Block in(kChannels, kBlockSize);
    std::vector<midi::Message> midiBuffer;
    for (int block = 0; block < maxBlocks; ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        int activeMidi = 0;
        instance.process(view, contextAt(block), midiBuffer.data(), activeMidi, midiOutput,
                         midiOut);
        std::this_thread::sleep_for(kBlockPace);
        // The action sees the block index too, because what comes back belongs to the
        // PREVIOUS block - a predicate that compares against the block just fed in is
        // true for the wrong reason on every call but the first.
        if (action(block, view))
            return true;
    }
    return false;
}

bool processExists(std::uint64_t pid) noexcept {
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    ::CloseHandle(handle);
    return true;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

} // namespace

TEST_CASE("A sandboxed slot returns the processor's audio one block later",
          "[sandbox][proxy]") {
    // The claim the whole phase rests on: what comes back is what the reference
    // processor would have produced in this process, delayed by exactly the latency
    // the slot reports.
    auto instance = makePrepared(gainProxyConfig());
    REQUIRE(instance->session() != nullptr);

    CHECK(instance->latencySamples() == kBlockSize); // (2 slots - 1) * 128 frames
    CHECK(instance->numParameters() == 1);
    CHECK(instance->parameterName(0) == "Gain");
    CHECK(instance->parameterName(7).empty()); // out of range: empty, not invented
    CHECK_FALSE(instance->hasEditor());
    CHECK_FALSE(instance->isActive());
    CHECK_FALSE(instance->hasFailed());

    instance->setParameterNormalised(0, kQuarterTurn);
    CHECK(instance->parameterNormalised(0) == kQuarterTurn);
    // Values are clamped on the way in: this number crosses a process boundary, and
    // a peer that sends 4.0 is either broken or hostile.
    instance->setParameterValue(0, 4.0f);
    CHECK(instance->parameterValue(0) == 1.0f);
    instance->setParameterNormalised(0, kQuarterTurn);

    // Prime the pipeline, then measure. Block numbers run CONTIGUOUSLY through both
    // phases, because the assertion is "what comes back is the block fed in last
    // time" - and jumping the index between phases breaks that relationship in the
    // test rather than in the transport. The first version of this case started
    // measuring at block 100 after priming at block 3, and its first measured block
    // was wrong for exactly that reason.
    test::Block in(kChannels, kBlockSize);
    std::vector<midi::Message> midiBuffer;
    int wrong = 0;
    int dryInsteadOfProcessed = 0;
    int block = 0;
    bool aligned = false;

    for (; block < 60 && !aligned; ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        int activeMidi = 0;
        instance->process(view, contextAt(block), midiBuffer.data(), activeMidi, midiOutput,
                          midiOut);
        std::this_thread::sleep_for(kBlockPace);
        aligned = block > 0 && blockEquals(in, block - 1, kHalfGain);
    }
    INFO("blocks fed before the parameter change came back: " << block);
    REQUIRE(aligned);

    // Now the real assertion: 40 more blocks, and every one comes back at half gain
    // and exactly one block late.
    constexpr int kBlocks = 40;
    for (int measured = 0; measured < kBlocks; ++measured, ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        int activeMidi = 0;
        instance->process(view, contextAt(block), midiBuffer.data(), activeMidi, midiOutput,
                          midiOut);
        std::this_thread::sleep_for(kBlockPace);
        if (!blockEquals(in, block - 1, kHalfGain)) {
            ++wrong;
            if (blockIsDry(in, block))
                ++dryInsteadOfProcessed;
        }
    }

    INFO("blocks compared: " << kBlocks << ", wrong: " << wrong
                             << ", of which were dry pass-throughs: " << dryInsteadOfProcessed);
    CHECK(wrong == 0);
    CHECK(instance->blocksReturned() >= static_cast<std::uint64_t>(kBlocks));
    CHECK(instance->refusedPushes() == 0u);

    const auto snapshot = instance->session()->ring().stats().snapshot();
    INFO("torn " << snapshot.tornReads << ", lost " << snapshot.blocksLost);
    CHECK(snapshot.tornReads == 0u);
    CHECK(snapshot.blocksLost == 0u);
}

TEST_CASE("Notes cross the boundary in both directions", "[sandbox][proxy]") {
    auto instance = makePrepared(gainProxyConfig());

    test::Block in(kChannels, kBlockSize);
    std::vector<midi::Message> midiBuffer(64);
    midiBuffer[0] = midi::Message::noteOn(3, 62, 90);
    midiBuffer[1] = midi::Message::noteOff(3, 62);
    int activeMidi = 2;

    std::vector<midi::Message> echoed;
    for (int block = 0; block < 30; ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        // Only the first call carries the notes; the rest prove nothing extra arrives.
        const int count = block == 0 ? activeMidi : 0;
        instance->process(view, contextAt(block), midiBuffer.data(), count, midiOutput, midiOut);
        std::this_thread::sleep_for(kBlockPace);
        for (int i = 0; i < midiOut; ++i)
            echoed.push_back(midiOutput[i]);
        if (echoed.size() >= 2)
            break;
    }

    INFO("notes echoed: " << echoed.size());
    REQUIRE(echoed.size() == 2u);
    CHECK(echoed[0].isNoteOn());
    CHECK(echoed[0].channel == 3);
    CHECK(echoed[0].data1 == 62);
    CHECK(echoed[0].data2 == 90);
    CHECK(echoed[1].isNoteOff());
    CHECK(echoed[1].data1 == 62);
}

TEST_CASE("A slot whose helper has stopped passes audio through dry", "[sandbox][proxy]") {
    // The property the sandbox exists for, tested without arranging a real crash: the
    // helper is told to stop after four blocks, and the slot has to keep answering
    // process() with the audio it was given - dry, not silent, and never hanging.
    auto instance = makePrepared(gainProxyConfig(4));
    instance->setParameterNormalised(0, kQuarterTurn);

    test::Block in(kChannels, kBlockSize);
    std::vector<midi::Message> midiBuffer;
    std::vector<bool> wasDry;

    for (int block = 0; block < 60; ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        int activeMidi = 0;
        instance->process(view, contextAt(block), midiBuffer.data(), activeMidi, midiOutput,
                          midiOut);
        std::this_thread::sleep_for(kBlockPace);
        wasDry.push_back(blockIsDry(in, block));
    }

    // The last ten blocks are all after the helper left, so all ten must be dry.
    const std::size_t tail = 10;
    std::size_t dryInTail = 0;
    for (std::size_t i = wasDry.size() - tail; i < wasDry.size(); ++i)
        dryInTail += wasDry[i] ? 1u : 0u;
    INFO("dry blocks in the last " << tail << ": " << dryInTail);
    INFO("refused pushes: " << instance->refusedPushes() << ", dry blocks: "
                            << instance->dryBlocks());
    CHECK(dryInTail == tail);
    CHECK(instance->dryBlocks() > 0u);
    // The helper really is gone, and the slot noticed by the rings going quiet rather
    // than by asking - which is the only way an audio thread may find out.
    CHECK_FALSE(instance->session()->isAlive());
}

TEST_CASE("A chain cannot tell a sandboxed slot from an in-process one", "[sandbox][proxy]") {
    // Integration rather than unit: the chain walks its slots, calls process(), sums
    // latency and skips failures. None of that has a sandbox case in it, and that is
    // the point being tested.
    auto instance = makePrepared(gainProxyConfig());
    instance->setParameterNormalised(0, kQuarterTurn);

    PluginChain chain;
    const int beforeLatency = chain.totalLatencySamples();
    chain.add(std::move(instance));
    REQUIRE(chain.size() == 1u);
    CHECK(chain.totalLatencySamples() == beforeLatency + kBlockSize);
    CHECK(chain.at(0) != nullptr);
    CHECK(chain.at(0)->descriptor().name == "Sandbox Reference Gain");

    // Let the parameter land through the chain's own entry point, then measure - with
    // contiguous block numbers across both phases, for the reason the case above gives.
    std::vector<midi::Message> midiBuffer(64);
    int activeMidi = 0;
    test::Block in(kChannels, kBlockSize);
    bool settled = false;
    int block = 0;
    for (; block < 60 && !settled; ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        chain.process(view, contextAt(block), midiBuffer, activeMidi);
        std::this_thread::sleep_for(kBlockPace);
        settled = block > 0 && blockEquals(in, block - 1, kHalfGain);
    }
    INFO("blocks fed before the chain settled: " << block);
    REQUIRE(settled);

    int wrong = 0;
    for (int measured = 0; measured < 30; ++measured, ++block) {
        fillBlock(in, block);
        auto view = in.view(kBlockSize);
        chain.process(view, contextAt(block), midiBuffer, activeMidi);
        std::this_thread::sleep_for(kBlockPace);
        if (!blockEquals(in, block - 1, kHalfGain))
            ++wrong;
    }
    INFO("wrong blocks through the chain: " << wrong);
    CHECK(wrong == 0);

    // And the chain's own state round trip works on a sandboxed slot, which is what
    // makes saving a project with one in it possible.
    const auto states = chain.saveStates();
    REQUIRE(states.hasValue());
    chain.at(0)->setParameterNormalised(0, 0.9f);
    CHECK(chain.at(0)->parameterNormalised(0) == 0.9f);
    CHECK(chain.restoreStates(states.value()).hasValue());
    CHECK(chain.at(0)->parameterNormalised(0) == kQuarterTurn);

    // Bypassing a sandboxed slot is the same flag as any other slot, and the chain
    // skips it before process() is ever called.
    chain.setBypassed(0, true);
    CHECK(chain.isBypassed(0));
    fillBlock(in, block);
    auto view = in.view(kBlockSize);
    chain.process(view, contextAt(block), midiBuffer, activeMidi);
    CHECK(blockIsDry(in, block));
}

TEST_CASE("Parameter state round-trips through a sandboxed slot", "[sandbox][proxy]") {
    auto instance = makePrepared(gainProxyConfig());
    instance->setParameterNormalised(0, 0.75f);

    const auto saved = instance->saveState();
    REQUIRE(saved.hasValue());
    CHECK_FALSE(saved.value().empty());

    instance->setParameterNormalised(0, 0.1f);
    CHECK(instance->parameterNormalised(0) == 0.1f);

    CHECK(instance->restoreState(saved.value()).hasValue());
    CHECK(instance->parameterNormalised(0) == 0.75f);

    // A blob that is not this slot's is refused rather than guessed at: a project
    // saved by a different processor must not turn into a random gain.
    const std::vector<std::uint8_t> tooShort{1, 2};
    CHECK(instance->restoreState(tooShort).hasError());
    const std::vector<std::uint8_t> lyingCount{9, 0, 0, 0};
    CHECK(instance->restoreState(lyingCount).hasError());

    // And the restored value reached the helper, not just the proxy's cache: 0.75
    // normalised is a gain of 1.5, so the block coming back is 1.5 times the one fed
    // in before it.
    const bool applied = pumpUntil(
        *instance,
        [&](int block, const dsp::AudioBlockView& view) {
            if (block == 0)
                return false;
            for (int channel = 0; channel < kChannels; ++channel) {
                for (int frame = 0; frame < kBlockSize; ++frame) {
                    const float want = signalAt(block - 1, channel, frame) * 1.5f;
                    if (view.channelPointers[channel][frame] != want)
                        return false;
                }
            }
            return true;
        },
        40);
    INFO("restored gain visible in the returned audio: " << applied);
    CHECK(applied);
}

TEST_CASE("Preparing twice leaves one helper, not two", "[sandbox][proxy]") {
    // A device change calls prepare() again. Two helpers on one ring would both
    // believe they are the consumer, so the old one has to be gone before the new one
    // starts - and gone means reaped, not merely signalled.
    auto instance = makePrepared(gainProxyConfig());
    const std::uint64_t firstPid = instance->session()->processId();
    REQUIRE(firstPid != 0u);
    const std::string firstName = instance->session()->name();

    REQUIRE(instance->prepare(kSampleRate, 256, kChannels).hasValue());
    const std::uint64_t secondPid = instance->session()->processId();
    const std::string secondName = instance->session()->name();
    REQUIRE(secondPid != 0u);

    CHECK(secondPid != firstPid);
    // Unique per session: on Windows a name a previous helper still holds is a name
    // this session cannot have, and a restart is exactly when that would happen.
    CHECK(secondName != firstName);
    CHECK(instance->latencySamples() == 256); // (2 slots - 1) * the new block size
    CHECK_FALSE(processExists(firstPid));

    // And the new session actually works.
    instance->setParameterNormalised(0, kQuarterTurn);
    const bool landed = pumpUntil(
        *instance,
        [&](int block, const dsp::AudioBlockView& view) {
            if (block == 0)
                return false;
            for (int channel = 0; channel < kChannels; ++channel) {
                for (int frame = 0; frame < kBlockSize; ++frame) {
                    const float want = signalAt(block - 1, channel, frame) * kHalfGain;
                    if (view.channelPointers[channel][frame] != want)
                        return false;
                }
            }
            return true;
        },
        40);
    CHECK(landed);
}

TEST_CASE("A processor that does not exist leaves the slot failed and dry",
          "[sandbox][proxy]") {
    // The helper exits with its own code and a line on its diagnostic stream; the slot
    // becomes failed, which is the marker the chain already skips. A plug-in that
    // could not load is not a reason for the session to be silent.
    ProxyConfig config = gainProxyConfig();
    config.processorId = "aura.sandbox.does.not.exist";
    config.readyTimeoutSeconds = 10.0;

    auto made = SandboxedPluginInstance::create(std::move(config));
    REQUIRE(made.hasValue());
    std::unique_ptr<SandboxedPluginInstance> instance = std::move(made.value());

    const Status prepared = instance->prepare(kSampleRate, kBlockSize, kChannels);
    REQUIRE(prepared.hasError());
    INFO("failure: " << instance->lastReport().failure);
    CHECK(instance->hasFailed());
    CHECK(instance->session() == nullptr);
    CHECK(instance->latencySamples() == 0);

    // The helper's own words are the only explanation of a failure that will ever
    // exist, so they have to survive as far as the thing that reports it.
    CHECK(instance->lastReport().exitCode == helperExit::kProcessorFailed);
    CHECK(instance->lastReport().helperOutput.find("no sandbox processor") != std::string::npos);

    test::Block in(kChannels, kBlockSize);
    fillBlock(in, 7);
    auto view = in.view(kBlockSize);
    std::vector<midi::Message> midiBuffer;
    int midiOut = 0;
    midi::Message midiOutput[kChainMidiOutCapacity];
    int activeMidi = 0;
    instance->process(view, contextAt(7), midiBuffer.data(), activeMidi, midiOutput, midiOut);
    CHECK(blockIsDry(in, 7));
    CHECK(midiOut == 0);
}

TEST_CASE("A slot with no descriptor is refused before anything is started",
          "[sandbox][proxy]") {
    ProxyConfig config = gainProxyConfig();
    config.descriptor.id.clear();
    const auto made = SandboxedPluginInstance::create(std::move(config));
    REQUIRE(made.hasError());
    CHECK(made.error().code == ErrorCode::InvalidArgument);

    ProxyConfig negative = gainProxyConfig();
    negative.parameterCount = -1;
    CHECK(SandboxedPluginInstance::create(std::move(negative)).hasError());

    // And prepare() refuses limits it cannot build a session from.
    auto instance = makePrepared(gainProxyConfig());
    CHECK(instance->prepare(0.0, kBlockSize, kChannels).hasError());
    CHECK(instance->prepare(kSampleRate, 0, kChannels).hasError());
    CHECK(instance->prepare(kSampleRate, kBlockSize, 0).hasError());
    // The session it had is still usable, which is the difference between refusing an
    // argument and breaking a slot.
    CHECK(instance->session() != nullptr);
}
