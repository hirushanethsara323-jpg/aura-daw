// ============================================================================
// AURA DAW - tools/plugin_host/PluginHostMain.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// `aura_plugin_host`: the process a sandboxed plug-in runs in.
//
// It does three things and knows nothing else. It attaches to a shared mapping the
// host created, it drives something with a prepare/process shape through the rings
// in that mapping, and it exits with a number that says why it stopped. It has never
// heard of the engine, the project, the graph or a window, and it does not know what
// a VST 3 bundle is - that is the adapter's business, behind SandboxProcessor.
//
// Being a separate process is the entire point: whatever runs in here can crash,
// hang, leak, spin or corrupt its own heap, and the session keeps playing. That
// promise is only worth what the loop below is worth, so the loop is written to the
// same rules as the engine's audio callback - no allocation, no locks, no file IO,
// no exceptions, no unbounded work - and tools/rt_audit.py is pointed at it so the
// claim is checked rather than intended.
//
// Two of its rules differ from the engine's, and both differences are deliberate:
//
//   * It SLEEPS when idle. The engine's callback must not, because a device is
//     waiting on it. This thread has no device waiting: the host's callback returns
//     whether or not the helper has finished, because the exchange is one block
//     deferred. A helper that spun instead would burn a core the host's real-time
//     thread needs, which is the opposite of what isolating it was for.
//
//   * It exits on its own when the host goes quiet. A helper cannot be told by a
//     host that is dead, and an orphan holding a shared mapping is a session that
//     cannot create its next one. See HelperProtocol.hpp for why there are two
//     deadlines and what replaces them in phase 4.
// ============================================================================

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/Version.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/sandbox/HelperProtocol.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"
#include "aura/plugin/sandbox/SandboxProcessor.hpp"
#include "aura/plugin/sandbox/SharedAudioRing.hpp"
#include "aura/plugin/sandbox/SharedMemory.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

/// Why the loop stopped.
///
/// Reported rather than acted on, because two of these need things the loop must not
/// do: emptying a ring is a control-thread operation, and re-preparing a processor
/// allocates. So the loop returns and main() does them.
enum class LoopExit : std::uint8_t {
    Shutdown,       ///< the host asked, cleanly
    ResetRequested, ///< the host asked for the rings and the processor to be reset
    HostGone,       ///< a deadline passed with no blocks; the host is not coming back
    BlockLimit,     ///< --max-blocks reached, which only a test asks for
    NotAttached,    ///< the ring had no control block, which cannot happen while attached
};

/// The scratch the loop reads into and writes out of. Built once before the loop;
/// the views inside are plain structs, so copying one to adjust its frame count
/// costs three assignments and no allocation.
struct LoopBuffers {
    dsp::AudioBlockView input{};
    dsp::AudioBlockView output{};
};

struct LoopLimits {
    std::uint64_t maxBlocks = 0; ///< 0 means no limit
    std::int64_t idleExitMs = kDefaultIdleExitMs;
    std::int64_t startupTimeoutMs = kDefaultStartupTimeoutMs;
};

/// Idle spins before the first sleep. Spinning briefly is worth it: a block usually
/// follows within microseconds of the last one, and a 1 ms sleep on every gap would
/// add up to a block of latency the transport does not owe.
constexpr std::uint64_t kSpinsBeforeSleep = 64;
constexpr int kIdleSleepMs = 1;

/// The helper's real-time loop. Audited by tools/rt_audit.py: it is in
/// RT_SCOPE_GLOBS and its name is in RT_FUNCTION_PATTERNS, so anything below that
/// allocates, locks, touches a file or throws fails the build the same way it would
/// in the engine's callback.
LoopExit runHelperLoop(SharedAudioRing& ring, SandboxProcessor& processor,
                       const LoopBuffers& buffers, const LoopLimits& limits) noexcept {
    ControlBlock* const control = ring.control();
    Telemetry* const telemetry = ring.telemetry();
    if (control == nullptr || telemetry == nullptr)
        return LoopExit::NotAttached;

    std::uint64_t processed = 0;
    std::uint64_t idleSpins = 0;
    bool sawFirstBlock = false;
    // One processed block, held until the host has room for it. See step 3.
    dsp::AudioBlockView pending{};
    std::uint64_t pendingOrigin = 0;
    bool pendingValid = false;
    const auto startedAt = std::chrono::steady_clock::now();
    auto lastBlockAt = startedAt;

    for (;;) {
        // 1. What the host has asked for. Requests are bits, cleared once acted on,
        //    so one set between two blocks cannot be missed.
        const std::uint32_t requests = control->hostRequests.load(std::memory_order_acquire);
        if ((requests & request::kShutdown) != 0u) {
            control->hostRequests.fetch_and(~request::kShutdown, std::memory_order_acq_rel);
            // A block processed but not delivered is dropped here, deliberately: the
            // host has said it is stopping, so it is not going to listen to it, and
            // waiting to push it would mean waiting on a host that just left.
            return LoopExit::Shutdown;
        }
        if ((requests & request::kReset) != 0u) {
            control->hostRequests.fetch_and(~request::kReset, std::memory_order_acq_rel);
            return LoopExit::ResetRequested;
        }
        if ((requests & request::kSaveState) != 0u) {
            // Cleared without setting kStateReady, which is the honest answer: the
            // request was seen and nothing was saved. The arena has nowhere to put a
            // blob and this thread must not touch the disk, so wiring state save is a
            // layout change that lands with the crash protocol in phase 4 - where the
            // blob is the recovery point and therefore has to be right.
            control->hostRequests.fetch_and(~request::kSaveState, std::memory_order_acq_rel);
        }

        // 2. Parameters and notes that arrived since the last block. Bounded by the
        //    ring's capacity: a full param ring drains in one pass, and the host
        //    cannot make it deeper than it was sized.
        ParamRecord parameter;
        while (ring.popParameter(parameter))
            processor.onParameter(parameter);

        midi::Message note;
        while (ring.popNoteIn(note))
            processor.onNoteIn(note);

        // 3. Audio, under back-pressure: deliver the block already processed before
        //    taking a new one, and drop neither.
        //
        //    The obvious alternative - pop a block, process it, and give up if the
        //    output ring happens to be full - loses audio a plug-in has already spent
        //    CPU on, and loses it exactly when the host is briefly late, which is when
        //    losing it is most audible. A bounded retry does not fix that either: any
        //    budget short of "wait for the host" drops under load, and a budget long
        //    enough to wait for the host is just this, spelled worse.
        //
        //    Holding one block costs nothing. The rings are two slots deep and the host
        //    pops before it pushes - that is why SharedAudioRing::exchange() is ordered
        //    that way - so a host that is exchanging at all makes room within one call.
        //    A host that has stopped exchanging leaves this loop sitting with a pending
        //    block while it keeps checking requests and keeps watching the deadlines
        //    below, which is the difference between back-pressure and a hang.
        bool didWork = false;
        if (pendingValid) {
            if (ring.pushOutput(pending, pendingOrigin) == PushOutcome::Published) {
                pendingValid = false;
                // The heartbeat the host's watchdog counts, incremented on delivery
                // rather than on processing: progress means "the host can hear it", not
                // "the plug-in finished". Not a clock, because "no progress for N
                // blocks" means the same thing at 44.1 kHz and 192 kHz and a wall clock
                // does not (Decision 3).
                telemetry->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
                ++processed;
                didWork = true;
            }
        } else {
            const PopResult popped = ring.popInput(buffers.input);
            if (popped.outcome == PopOutcome::Consumed && popped.frames > 0) {
                // A copy of the view carrying the real frame count: publishing the full
                // capacity after processing a partial block would send whatever the tail
                // of the scratch still held from last time. The scratch is not written
                // again until this block is delivered, which is what makes holding the
                // view across iterations safe.
                pending = buffers.output;
                pending.numFrames = static_cast<int>(popped.frames);
                pendingOrigin = popped.origin;
                processor.process(buffers.input, pending, popped.frames);
                pendingValid = true;
                sawFirstBlock = true;
                lastBlockAt = std::chrono::steady_clock::now();
                didWork = true;
            }
        }

        // 4. Notes the processor generated, drained until it has none. A full out
        //    ring stops the drain rather than spinning: the rest wait for the next
        //    block, which is a dropped event and not a hung helper.
        while (processor.popNoteOut(note)) {
            if (!ring.pushNoteOut(note))
                break;
        }

        if (limits.maxBlocks != 0 && processed >= limits.maxBlocks)
            return LoopExit::BlockLimit;

        if (didWork) {
            idleSpins = 0;
            continue;
        }

        if (++idleSpins < kSpinsBeforeSleep)
            continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));

        // 5. Is the host still there? Inferred from the arrival of blocks, because a
        //    dead host cannot say so. See HelperProtocol.hpp for why this is a
        //    backstop and not the mechanism.
        const std::int64_t limit = sawFirstBlock ? limits.idleExitMs : limits.startupTimeoutMs;
        if (limit <= 0)
            continue;
        const auto now = std::chrono::steady_clock::now();
        const auto silentFor = sawFirstBlock ? (now - lastBlockAt) : (now - startedAt);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(silentFor).count() >= limit)
            return LoopExit::HostGone;
    }
}

void printUsage() {
    std::cerr << "usage: aura_plugin_host " << helperArg::kArena << " <name> " << helperArg::kBytes
              << " <n> " << helperArg::kProcessor << " <id>\n"
              << "       [" << helperArg::kMaxBlocks << " <n>] [" << helperArg::kIdleExitMs
              << " <ms>] [" << helperArg::kStartupTimeoutMs << " <ms>]\n"
              << "       aura_plugin_host " << helperArg::kVersion << "\n"
              << "processors: ";
    for (const auto& id : builtinProcessorIds())
        std::cerr << id << ' ';
    std::cerr << '\n';
}

/// Reads one `--option value` pair. Returns false and reports when the value is
/// missing, so a truncated command line is a clear error rather than a helper that
/// runs with half a configuration.
bool takeValue(int argc, char** argv, int& index, const char* option, std::string& out) {
    if (index + 1 >= argc) {
        std::cerr << "aura_plugin_host: " << option << " needs a value\n";
        return false;
    }
    out = argv[++index];
    return true;
}

bool parseCount(const std::string& text, std::uint64_t& out) {
    if (text.empty())
        return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0')
        return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool parseSigned(const std::string& text, std::int64_t& out) {
    if (text.empty())
        return false;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0')
        return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

/// Fails loudly and returns the exit code the host will report. Every message names
/// the thing that failed, because these lines are what a user sees when a plug-in
/// would not start and the host can only show them a number otherwise.
void reportFailure(const char* what, const AuraError& error) {
    std::cerr << "aura_plugin_host: " << what << ": " << error.toString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::string arenaName;
    std::string processorId;
    std::size_t bytes = 0;
    bool haveBytes = false;
    bool versionOnly = false;
    LoopLimits limits;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[static_cast<std::size_t>(i)];
        std::string value;
        if (argument == helperArg::kVersion) {
            versionOnly = true;
        } else if (argument == helperArg::kArena) {
            if (!takeValue(argc, argv, i, helperArg::kArena, arenaName))
                return helperExit::kBadArguments;
        } else if (argument == helperArg::kProcessor) {
            if (!takeValue(argc, argv, i, helperArg::kProcessor, processorId))
                return helperExit::kBadArguments;
        } else if (argument == helperArg::kBytes) {
            if (!takeValue(argc, argv, i, helperArg::kBytes, value))
                return helperExit::kBadArguments;
            std::uint64_t parsed = 0;
            if (!parseCount(value, parsed) || parsed == 0) {
                std::cerr << "aura_plugin_host: " << helperArg::kBytes << " needs a positive "
                          << "number, got '" << value << "'\n";
                return helperExit::kBadArguments;
            }
            bytes = static_cast<std::size_t>(parsed);
            haveBytes = true;
        } else if (argument == helperArg::kMaxBlocks) {
            if (!takeValue(argc, argv, i, helperArg::kMaxBlocks, value))
                return helperExit::kBadArguments;
            if (!parseCount(value, limits.maxBlocks)) {
                std::cerr << "aura_plugin_host: " << helperArg::kMaxBlocks << " needs a number\n";
                return helperExit::kBadArguments;
            }
        } else if (argument == helperArg::kIdleExitMs) {
            if (!takeValue(argc, argv, i, helperArg::kIdleExitMs, value))
                return helperExit::kBadArguments;
            if (!parseSigned(value, limits.idleExitMs)) {
                std::cerr << "aura_plugin_host: " << helperArg::kIdleExitMs << " needs a number\n";
                return helperExit::kBadArguments;
            }
        } else if (argument == helperArg::kStartupTimeoutMs) {
            if (!takeValue(argc, argv, i, helperArg::kStartupTimeoutMs, value))
                return helperExit::kBadArguments;
            if (!parseSigned(value, limits.startupTimeoutMs)) {
                std::cerr << "aura_plugin_host: " << helperArg::kStartupTimeoutMs
                          << " needs a number\n";
                return helperExit::kBadArguments;
            }
        } else {
            std::cerr << "aura_plugin_host: unknown argument '" << argument << "'\n";
            printUsage();
            return helperExit::kBadArguments;
        }
    }

    if (versionOnly) {
        std::cout << "aura_plugin_host " << aura::core::versionString() << '\n';
        for (const auto& id : builtinProcessorIds())
            std::cout << "processor " << id << '\n';
        return helperExit::kOk;
    }

    if (arenaName.empty() || !haveBytes || processorId.empty()) {
        std::cerr << "aura_plugin_host: " << helperArg::kArena << ", " << helperArg::kBytes
                  << " and " << helperArg::kProcessor << " are all required\n";
        printUsage();
        return helperExit::kBadArguments;
    }

    // ---------------------------------------------------------------- the mapping
    // Opened, never created: a helper that brought a mapping into existence would be
    // a helper talking to itself.
    auto mapping = SharedMemory::open(arenaName, bytes);
    if (mapping.hasError()) {
        reportFailure("could not open the shared mapping", mapping.error());
        return helperExit::kShmOpenFailed;
    }

    auto arena = ArenaView::attach(mapping.value().data(), mapping.value().size());
    if (arena.hasError()) {
        reportFailure("the shared mapping is not an arena this build understands", arena.error());
        return helperExit::kAttachFailed;
    }

    SharedAudioRing ring;
    const Status attached = ring.attach(arena.value(), SharedAudioRing::Side::Helper);
    if (attached.hasError()) {
        reportFailure("could not attach to the arena's rings", attached.error());
        return helperExit::kAttachFailed;
    }

    // ------------------------------------------------------------- the processor
    auto made = makeSandboxProcessor(processorId);
    if (made.hasError()) {
        reportFailure("could not create the processor", made.error());
        return helperExit::kProcessorFailed;
    }
    std::unique_ptr<SandboxProcessor> processor = std::move(made.value());

    // The session's sample rate comes from the transport snapshot the host published
    // before starting this process. Absent one - a host that spawned a helper and
    // then forgot to say what the session is - the arena's own default applies, and
    // the processor is told a rate that is wrong rather than zero, because a
    // processor that prepares for 0 Hz cannot be blamed for what it does next.
    double sampleRate = 48000.0;
    TransportSnapshot transport{};
    if (readTransportSnapshot(ring.control(), transport, ring.telemetry()) &&
        transport.sampleRate > 0.0) {
        sampleRate = transport.sampleRate;
    }

    const ArenaSpec& arenaSpec = arena.value().spec();
    ProcessorSpec spec = processorSpecFor(arenaSpec, sampleRate);
    if (!processor->prepare(spec)) {
        std::cerr << "aura_plugin_host: '" << processorId << "' could not prepare for "
                  << spec.maxChannels << "ch x " << spec.maxBlockSize << " frames at "
                  << sampleRate << " Hz\n";
        return helperExit::kProcessorFailed;
    }

    // ------------------------------------------------------------------- scratch
    // Allocated here and never again. maxChannels x maxBlockSize is the arena's own
    // budget, so a block the transport can carry always fits.
    const std::size_t channels = static_cast<std::size_t>(spec.maxChannels);
    const std::size_t blockSize = static_cast<std::size_t>(spec.maxBlockSize);
    std::vector<std::vector<float>> inputScratch(channels, std::vector<float>(blockSize, 0.0f));
    std::vector<std::vector<float>> outputScratch(channels, std::vector<float>(blockSize, 0.0f));
    std::vector<float*> inputPointers(channels, nullptr);
    std::vector<float*> outputPointers(channels, nullptr);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        inputPointers[channel] = inputScratch[channel].data();
        outputPointers[channel] = outputScratch[channel].data();
    }

    LoopBuffers buffers;
    buffers.input.channelPointers = inputPointers.data();
    buffers.input.numChannels = static_cast<int>(channels);
    buffers.input.numFrames = static_cast<int>(blockSize);
    buffers.output.channelPointers = outputPointers.data();
    buffers.output.numChannels = static_cast<int>(channels);
    buffers.output.numFrames = static_cast<int>(blockSize);

    // Ready. Published before the first block is consumed, so a host that polls for
    // readiness cannot see it and then wait for a loop that has not started.
    ring.control()->helperState.store(helperState::kReady, std::memory_order_release);

    int exitCode = helperExit::kOk;
    for (;;) {
        const LoopExit why = runHelperLoop(ring, *processor, buffers, limits);
        if (why == LoopExit::ResetRequested) {
            // Control-thread work, which is why the loop returned instead of doing
            // it: reset() empties rings and prepare() allocates.
            ring.reset();
            TransportSnapshot afterReset{};
            double rate = sampleRate;
            if (readTransportSnapshot(ring.control(), afterReset, ring.telemetry()) &&
                afterReset.sampleRate > 0.0) {
                rate = afterReset.sampleRate;
            }
            spec = processorSpecFor(arenaSpec, rate);
            if (!processor->prepare(spec)) {
                std::cerr << "aura_plugin_host: '" << processorId
                          << "' could not prepare after a reset\n";
                exitCode = helperExit::kProcessorFailed;
                break;
            }
            continue;
        }
        if (why == LoopExit::HostGone)
            exitCode = helperExit::kHostGone;
        else if (why == LoopExit::NotAttached)
            exitCode = helperExit::kAttachFailed;
        break;
    }

    // Last words: the state bit that says this side is going away. kReady is replaced
    // rather than left set, because a host that polls for readiness after a restart
    // must not see a stale yes.
    //
    // The anomaly counters in telemetry are not flushed here: the helper-side
    // push and pop publish them as they go, so what the host reads is at most one
    // block old, and a private method is not something this file should be reaching
    // for to shave one block off that.
    ring.control()->helperState.store(helperState::kExiting, std::memory_order_release);
    return exitCode;
}
