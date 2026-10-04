// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/SandboxedPluginInstance.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// A PluginInstance whose audio happens in another process.
//
// The point of this class is that nothing above it can tell. `PluginChain::process()`
// walks its slots and calls `process()` on each one; this answers that call by
// publishing a block into a shared ring and taking back the block the helper
// processed, and the chain, the graph and the engine are unchanged. That is what
// makes the sandbox a swap rather than a rewrite, and it is the whole reason
// `PluginInstance` was an interface before there was anything to put behind it.
//
// What it costs, stated plainly because a plug-in slot is not the place to discover
// it:
//
//   * One block of latency, `(slots - 1) * blockSize`, reported through
//     latencySamples() so the graph compensates for it the way it already does for a
//     look-ahead limiter. At 128 frames that is 2.7 ms.
//   * Two block-sized copies per block instead of none. A 128-frame stereo block is
//     1 KB each way, which is tens of nanoseconds - measurable, and smaller than the
//     plug-in it is carrying.
//   * A helper that dies leaves this slot dry rather than leaving the session silent:
//     process() passes the input through untouched, which is Decision 3 in
//     docs/research/PLUGIN_SANDBOX_RESEARCH.md. Dry is not silence, and it is not a
//     crash.
//
// What it does not do yet: load a real plug-in. The helper runs a SandboxProcessor,
// so the processor id selects the reference gain-and-echo processor until the VST 3
// and CLAP adapters move into the helper (phases 6-7). Everything above this class
// is written so that swapping the processor for a bundle does not change this file.
//
// Thread contract, per method:
//   prepare(), reset(), setParameter*(), saveState(), restoreState(), report()
//                                     CONTROL THREAD ONLY
//   process()                         RT-safe - no allocation, no locks, no file IO,
//                                     no logging, no unbounded work
//   latencySamples(), numParameters(), parameterValue(), parameterNormalised(),
//   isActive(), descriptor()          RT-safe reads
//   parameterName()                   control thread (returns a std::string)
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/PluginHost.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"
#include "aura/plugin/sandbox/SandboxSession.hpp"

namespace aura::plugin::sandbox {

/// How to build a sandboxed slot.
struct ProxyConfig {
    /// What the browser and the chain show. The proxy does not invent a name for a
    /// plug-in it has not loaded.
    PluginDescriptor descriptor;

    /// Which SandboxProcessor the helper runs. Empty means the reference processor.
    std::string processorId;

    /// Helper executable; empty means "find it next to the running executable".
    std::string helperExecutable;

    /// How many parameters the slot exposes. The proxy has no way to ask a processor
    /// it has not started yet, and starting one to find out would make construction
    /// spawn a process - so the caller states it. The reference processor has one.
    int parameterCount = 1;

    /// Names for those parameters. Shorter than parameterCount means the rest are
    /// "Param N", which is honest: a proxy that guessed names would be inventing a
    /// plug-in's UI.
    std::vector<std::string> parameterNames;

    /// Passed through to the session.
    double readyTimeoutSeconds = 5.0;

    /// Stops the helper after N blocks; 0 means no limit. Tests use it to make a
    /// helper leave on schedule, which is the only way to check what a slot does when
    /// its helper dies without arranging an actual crash - and phase 4's crash fixture
    /// will want the same knob.
    std::uint64_t maxBlocks = 0;
};

class SandboxedPluginInstance final : public PluginInstance {
public:
    ~SandboxedPluginInstance() override;
    SandboxedPluginInstance(const SandboxedPluginInstance&) = delete;
    SandboxedPluginInstance& operator=(const SandboxedPluginInstance&) = delete;

    /// Builds the object. Does NOT start a helper - that is prepare()'s job, because
    /// prepare() is what knows the sample rate, the block size and the channel count,
    /// and a session sized before then would have to be thrown away and rebuilt.
    [[nodiscard]] static AuraResult<std::unique_ptr<SandboxedPluginInstance>> create(
        ProxyConfig config);

    // ------------------------------------------------------------- PluginInstance
    [[nodiscard]] const PluginDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    /// Starts (or restarts) the session for these limits. Control thread.
    ///
    /// Restarting is the normal case, not an error case: a device change calls
    /// prepare() again, and a session is sized for one block size and one channel
    /// count. The old helper is stopped before the new one starts, so a slot never has
    /// two helpers competing for one ring.
    Status prepare(double sampleRate, int maxBlockSize, int numChannels) override;

    /// Asks the helper to clear its processor's tails. Control thread. Does not touch
    /// the transport's counters - see SandboxSession.hpp for why that split exists.
    void reset() noexcept override;

    /// RT-safe. Publishes this block and takes back the one the helper finished.
    void process(dsp::AudioBlockView& audio, const dsp::ProcessContext& context,
                 const midi::Message* midiIn, int numMidiIn, midi::Message* midiOut,
                 int& numMidiOut) noexcept override;

    /// `(slots - 1) * blockSize` once prepared, 0 before. The graph already knows how
    /// to compensate for a plug-in that reports latency; this is the same mechanism a
    /// look-ahead limiter uses, and it is why the sandbox costs a block rather than
    /// costing the audio thread a wait.
    [[nodiscard]] int latencySamples() const noexcept override;

    [[nodiscard]] int numParameters() const noexcept override;
    [[nodiscard]] std::string parameterName(int index) const override;
    [[nodiscard]] float parameterValue(int index) const noexcept override;
    void setParameterValue(int index, float value) noexcept override;
    /// The wire carries normalised 0..1 and nothing else, so for a sandboxed slot the
    /// normalised and the raw value are the same number. That is a limitation of not
    /// having a format adapter yet, not a design choice: a VST 3 plug-in's own ranges
    /// arrive with its adapter, and until then a proxy that reported a range it had
    /// never been told would be making one up.
    [[nodiscard]] float parameterNormalised(int index) const noexcept override;
    void setParameterNormalised(int index, float value) noexcept override;

    /// The parameters this proxy last sent, as opaque bytes.
    ///
    /// Not the plug-in's own state: the arena has nowhere to put a blob and the
    /// helper must not touch the disk, so a real state transfer is a layout change
    /// that lands with the crash protocol (phase 4), where the blob is the recovery
    /// point and has to be right. Saving what the proxy does know keeps project
    /// save/load working in the meantime, and is stated here rather than left to be
    /// discovered by somebody whose reverb preset came back as a gain value.
    [[nodiscard]] AuraResult<std::vector<std::uint8_t>> saveState() const override;
    Status restoreState(const std::vector<std::uint8_t>& state) override;

    /// The editor stays in the helper and needs the shell (M9/M10) to be embedded, so
    /// a sandboxed slot reports no editor - the same fallback the VST 3 path uses, and
    /// the browser shows the generic parameter list.
    [[nodiscard]] bool hasEditor() const noexcept override { return false; }
    void* createEditor() override { return nullptr; }
    void destroyEditor(void*) override {}

    /// False, always. Whether a plug-in is still producing a tail is something only
    /// the plug-in knows, and asking it would mean a round trip on a path that cannot
    /// wait for one. A wrong `true` costs CPU forever; a wrong `false` costs a tail
    /// that was going to be cut off by the next block anyway. Giving the telemetry
    /// block a tail flag is a layout change, and belongs with phase 4.
    [[nodiscard]] bool isActive() const noexcept override { return false; }

    // ---------------------------------------------------------- sandbox-specific
    /// The session, or nullptr before prepare() succeeded. Control thread only -
    /// an audio thread reaches the transport through process().
    [[nodiscard]] SandboxSession* session() noexcept { return session_.get(); }
    [[nodiscard]] const SandboxSession* session() const noexcept { return session_.get(); }

    /// What the last session did, including the helper's own stderr. Read this when
    /// prepare() failed or the slot went dry: a process that crashed cannot be asked
    /// what happened, so its last words are the only evidence there is.
    [[nodiscard]] const SessionReport& lastReport() const noexcept { return report_; }

    /// Blocks published and blocks that came back, for a test or a diagnostic. Both
    /// are counted in process(), which is the only place that knows.
    [[nodiscard]] std::uint64_t blocksPublished() const noexcept { return blocksPublished_; }
    [[nodiscard]] std::uint64_t blocksReturned() const noexcept { return blocksReturned_; }
    /// Pushes the helper refused. Zero in a healthy session; a rising count is the
    /// first sign of a helper that has stopped taking blocks, and it is visible a
    /// whole watchdog interval before the audio does.
    [[nodiscard]] std::uint64_t refusedPushes() const noexcept { return refusedPushes_; }
    /// Blocks that went out and did not come back in time, so the slot passed audio
    /// through dry. Equal to the pipeline depth at startup and zero afterwards in a
    /// healthy session - which is what makes it useful: any other number is a
    /// helper that is behind, and it is visible before the audio is.
    [[nodiscard]] std::uint64_t dryBlocks() const noexcept { return dryBlocks_; }
    /// Pushes refused because the helper was behind, and how many of those a single
    /// drained block turned into a successful retry. In a healthy session both are
    /// zero or very small; a refused count climbing without the retries keeping up is
    /// a helper that has stopped consuming, which is visible here a whole watchdog
    /// interval before it is audible.
    [[nodiscard]] std::uint64_t drainRetries() const noexcept { return drainRetries_; }
    /// Blocks this session could not carry at all - wider or taller than the arena was
    /// sized for, which is a caller bug and not a scheduling fact. Never retried,
    /// because retrying a bug turns it into a spin.
    [[nodiscard]] std::uint64_t rejectedBlocks() const noexcept { return rejectedBlocks_; }

private:
    explicit SandboxedPluginInstance(ProxyConfig config);

    /// Publishes the transport only when it has actually moved. Control thread and
    /// audio thread both end up here, and a seqlock write per block would be cheap
    /// but pointless: the helper reads it once at prepare() and again on a reset.
    void publishTransportIfChanged(const dsp::ProcessContext& context, int numFrames) noexcept;

    ProxyConfig config_;
    PluginDescriptor descriptor_;
    std::unique_ptr<SandboxSession> session_;
    SessionReport report_;

    double sampleRate_ = 0.0;
    int blockSize_ = 0;
    int channels_ = 0;
    int latencySamples_ = 0;

    /// Monotonic and never reset, including by reset(): the helper compares each
    /// block's origin against the one it expects, and restarting the numbering would
    /// look like a block from the past. A transport seek is not a new session.
    std::uint64_t origin_ = 0;

    TransportSnapshot lastTransport_{};
    bool transportPublished_ = false;
    /// The last value sent for each parameter. Atomic because `parameterValue()` is
    /// documented RT-safe while `setParameterNormalised()` is a control-thread call:
    /// a plain float written on one thread and read on the other is a data race, and
    /// this project has already shipped one of those (see ADR-0017 item 4). Sized
    /// once in the constructor and never resized, because atomics do not move.
    std::vector<std::atomic<float>> parameterValues_;

    /// Scratch for the one case where a block arrives at the wrong moment: a push
    /// refused because the helper is behind. The caller processes in place, so
    /// `audio` is still holding the input this call has to publish and cannot receive
    /// the output that draining makes available. Allocated in prepare() and never in
    /// process(), because the audio path may not allocate.
    std::vector<std::vector<float>> drainScratch_;
    std::vector<float*> drainPointers_;
    dsp::AudioBlockView drainView_{};
    std::uint32_t drainedFrames_ = 0;

    std::uint64_t blocksPublished_ = 0;
    std::uint64_t blocksReturned_ = 0;
    std::uint64_t refusedPushes_ = 0;
    std::uint64_t dryBlocks_ = 0;
    std::uint64_t rejectedBlocks_ = 0;
    std::uint64_t drainRetries_ = 0;
};

} // namespace aura::plugin::sandbox
