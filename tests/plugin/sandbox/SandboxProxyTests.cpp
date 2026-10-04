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

bool blockEqualsGain(const test::Block& block, std::int64_t index, float gain) noexcept {
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
    return blockEqualsGain(block, index, 1.0f);
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

/// What one process() call handed back, in blocks of delay.
///
/// Classified rather than assumed, because "the block from one call ago" is only true
/// while nothing has been dropped - and a test that asserts it blindly reports a
/// scheduling fact as a transport bug. 0 is the input passed through untouched (dry),
/// a positive N is the block fed N calls ago processed at the expected gain, and -1 is
/// neither, which is the only value that means something is actually wrong.
/// An all-zero block. The ring writes one when it discards a torn read, on purpose:
/// half of two different blocks is not audio. Named separately so the test can tell
/// "the transport protected us" from "the transport returned something wrong".
bool blockIsSilent(const test::Block& block) noexcept {
    for (const auto& channel : block.channels) {
        for (const float sample : channel) {
            if (sample != 0.0f)
                return false;
        }
    }
    return true;
}

/// -2 means silent, which is what a discarded torn read leaves behind.
int classifyReturned(const test::Block& block, int fed, float gain) noexcept {
    if (blockIsSilent(block))
        return -2;
    if (blockIsDry(block, fed))
        return 0;
    for (int delay = 1; delay <= 8; ++delay) {
        if (fed - delay < 0)
            break;
        if (blockEqualsGain(block, fed - delay, gain))
            return delay;
    }
    return -1;
}

/// Feeds one block, retrying until the proxy publishes it, and reports what came back.
///
/// A real host never retries: the device clock gives the helper a whole block period
/// and the helper's work is microseconds. A test loop gives it nothing, so the host
/// outruns a peer in another process that has not been scheduled yet, a push is
/// refused, an input is dropped - and from then on the one-block deferral is off by
/// one for the rest of the run. That is the test measuring its own runner. Retrying
/// until the block is published is the test emulating the clock it does not have.
template <typename Process>
int feedBlock(SandboxedPluginInstance& proxy, Process&& process, int block, float gain,
              int& retries) {
    test::Block in(kChannels, kBlockSize);
    for (int attempt = 0; attempt < 4000; ++attempt) {
        const std::uint64_t publishedBefore = proxy.blocksPublished();
        fillBlock(in, block);
        process(in, block);
        if (proxy.blocksPublished() > publishedBefore)
            return classifyReturned(in, block, gain);
        ++retries;
        std::this_thread::sleep_for(kBlockPace);
    }
    return classifyReturned(in, block, gain);
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

    // Prime the pipeline, then measure. Every block is retried until the proxy has
    // published it - see feedBlock() for why - so no input is dropped and the
    // one-block deferral holds for the whole run instead of drifting the first time
    // the runner outruns a peer in another process.
    std::vector<midi::Message> midiBuffer;
    auto feed = [&](test::Block& in, int block) {
        auto view = in.view(kBlockSize);
        int midiOut = 0;
        midi::Message midiOutput[kChainMidiOutCapacity];
        int activeMidi = 0;
        instance->process(view, contextAt(block), midiBuffer.data(), activeMidi, midiOutput,
                          midiOut);
        std::this_thread::sleep_for(kBlockPace);
    };

    int retries = 0;
    int block = 0;
    int delay = -1;
    for (; block < 80; ++block) {
        delay = feedBlock(*instance, feed, block, kHalfGain, retries);
        INFO("block " << block << " came back with delay " << delay);
        REQUIRE(delay >= 0); // dry or correctly processed; never corrupt
        if (delay == 1)
            break;
    }
    INFO("blocks fed before the pipeline settled: " << block << ", push retries: " << retries);
    REQUIRE(delay == 1);
    // The priming loop breaks with `block` still pointing at the block it just fed, so
    // measurement has to start at the next one. Feeding the same index twice is not a
    // harmless repeat: the second call returns that block's own output, which reads as
    // a delay of 0 against an expectation of 1 and looks exactly like a corrupt block.
    // Found by printing the samples, not by reading the loop.
    ++block;

    constexpr int kBlocks = 40;
    int corrupt = 0;
    int silent = 0;
    int dryInsteadOfProcessed = 0;
    int shifted = 0;
    for (int measured = 0; measured < kBlocks; ++measured, ++block) {
        const int returned = feedBlock(*instance, feed, block, kHalfGain, retries);
        if (returned == 1)
            continue;
        if (returned == -2)
            ++silent;
        else if (returned < 0)
            ++corrupt;
        else if (returned == 0)
            ++dryInsteadOfProcessed;
        else
            ++shifted;
    }

    const auto earlySnapshot = instance->session()->ring().stats().snapshot();
    INFO("blocks compared: " << kBlocks << ", corrupt: " << corrupt << ", silent: " << silent
                             << ", dry: " << dryInsteadOfProcessed << ", wrong delay: " << shifted
                             << ", push retries: " << retries);
    INFO("host ring: torn " << earlySnapshot.tornReads << ", lost " << earlySnapshot.blocksLost
                            << ", overruns " << earlySnapshot.overruns << ", underruns "
                            << earlySnapshot.underruns);
    INFO("helper telemetry: torn "
         << instance->session()->telemetry()->tornReads.load() << ", lost "
         << instance->session()->telemetry()->blocksLost.load() << ", overruns "
         << instance->session()->telemetry()->overruns.load() << ", underruns "
         << instance->session()->telemetry()->underruns.load() << ", processed "
         << instance->session()->telemetry()->blocksProcessed.load());
    // Corrupt is the assertion that matters: a block that is neither the input passed
    // through nor an earlier input processed at the right gain is a block that came
    // back wrong, and no amount of scheduling explains it.
    CHECK(corrupt == 0);
    CHECK(silent == 0);
    CHECK(dryInsteadOfProcessed == 0);
    CHECK(shifted == 0);
    CHECK(instance->blocksReturned() >= static_cast<std::uint64_t>(kBlocks));
    // A push the session could not carry at all is a caller bug; a push that needed a
    // retry is scheduling, and feedBlock() already accounted for it.
    CHECK(instance->rejectedBlocks() == 0u);
    INFO("refused pushes that needed a retry: " << instance->refusedPushes()
                                                << ", of which a drained block fixed: "
                                                << instance->drainRetries());

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

    // Through the chain's own entry point, with the same retry a device clock would
    // never need. The proxy is kept as a raw pointer because the chain owns it now and
    // its counters are what tells the test a block was really published.
    SandboxedPluginInstance* proxy = static_cast<SandboxedPluginInstance*>(chain.at(0));
    REQUIRE(proxy != nullptr);
    std::vector<midi::Message> midiBuffer(64);
    int activeMidi = 0;
    auto feed = [&](test::Block& in, int block) {
        auto view = in.view(kBlockSize);
        chain.process(view, contextAt(block), midiBuffer, activeMidi);
        std::this_thread::sleep_for(kBlockPace);
    };

    int retries = 0;
    int block = 0;
    int delay = -1;
    for (; block < 80; ++block) {
        delay = feedBlock(*proxy, feed, block, kHalfGain, retries);
        REQUIRE(delay >= 0);
        if (delay == 1)
            break;
    }
    INFO("blocks fed before the chain settled: " << block << ", push retries: " << retries);
    REQUIRE(delay == 1);
    ++block; // the priming loop breaks on the block it just fed; see the case above

    int corrupt = 0;
    int silent = 0;
    int wrongDelay = 0;
    for (int measured = 0; measured < 30; ++measured, ++block) {
        const int returned = feedBlock(*proxy, feed, block, kHalfGain, retries);
        if (returned == 1)
            continue;
        if (returned == -2)
            ++silent;
        else if (returned < 0)
            ++corrupt;
        else
            ++wrongDelay;
    }
    const auto chainSnapshot = proxy->session()->ring().stats().snapshot();
    INFO("corrupt: " << corrupt << ", silent: " << silent << ", wrong delay: " << wrongDelay
                     << ", retries: " << retries);
    INFO("host ring: torn " << chainSnapshot.tornReads << ", lost " << chainSnapshot.blocksLost
                            << ", overruns " << chainSnapshot.overruns << ", underruns "
                            << chainSnapshot.underruns);
    CHECK(corrupt == 0);
    CHECK(silent == 0);
    CHECK(wrongDelay == 0);

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
    test::Block bypassed(kChannels, kBlockSize);
    fillBlock(bypassed, block);
    auto bypassView = bypassed.view(kBlockSize);
    chain.process(bypassView, contextAt(block), midiBuffer, activeMidi);
    CHECK(blockIsDry(bypassed, block));
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
    int retries = 0;
    int delay = -1;
    for (int block = 0; block < 80 && delay != 1; ++block) {
        delay = feedBlock(*instance,
                          [&](test::Block& in, int b) {
                              auto view = in.view(kBlockSize);
                              std::vector<midi::Message> none;
                              int midiOut = 0;
                              midi::Message midiOutput[kChainMidiOutCapacity];
                              int activeMidi = 0;
                              instance->process(view, contextAt(b), none.data(), activeMidi,
                                                midiOutput, midiOut);
                              std::this_thread::sleep_for(kBlockPace);
                          },
                          block, 1.5f, retries);
        REQUIRE(delay >= 0);
    }
    INFO("delay after restoring state: " << delay << ", retries: " << retries);
    CHECK(delay == 1);
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
    int retries = 0;
    int delay = -1;
    for (int block = 0; block < 80 && delay != 1; ++block) {
        delay = feedBlock(*instance,
                          [&](test::Block& in, int b) {
                              auto view = in.view(kBlockSize);
                              std::vector<midi::Message> none;
                              int midiOut = 0;
                              midi::Message midiOutput[kChainMidiOutCapacity];
                              int activeMidi = 0;
                              instance->process(view, contextAt(b), none.data(), activeMidi,
                                                midiOutput, midiOut);
                              std::this_thread::sleep_for(kBlockPace);
                          },
                          block, kHalfGain, retries);
        REQUIRE(delay >= 0);
    }
    INFO("delay after a second prepare(): " << delay << ", retries: " << retries);
    CHECK(delay == 1);
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
