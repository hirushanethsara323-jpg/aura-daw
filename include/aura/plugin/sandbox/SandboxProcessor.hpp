// ============================================================================
// AURA DAW - include/aura/plugin/sandbox/SandboxProcessor.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// What the helper process actually runs.
//
// This interface exists so that the helper is format-agnostic. It knows how to
// attach to an arena, move blocks, and drive something with a prepare/process
// shape - it does not know what a VST 3 bundle or a CLAP entry point is. The
// adapters for those live behind this interface and are compiled in only when
// their option is on, which is what keeps `docs/PLUGIN_HOST.md`'s claim true: the
// only code in the tree that knows a plug-in format exists is the code that has to.
//
// It also exists so the transport can be tested at all. A cross-process audio path
// that can only be exercised with a real third-party bundle is a path that CI
// never runs, and a path CI never runs is a path that is broken. The built-in
// reference processor is deterministic on purpose: a test can predict every sample
// and every event that should come back.
//
// Thread contract, per method:
//   prepare()          control thread - allocate everything the RT path needs here
//   process()          RT-safe
//   onParameter()      RT-safe
//   onNoteIn()         RT-safe
//   popNoteOut()       RT-safe
//   saveState()        control thread - may allocate and may touch the disk
//   restoreState()     control thread - may allocate
//
// "RT-safe" here means the helper's audio thread, which is a real-time thread in
// exactly the sense the engine's is: no allocation, no locks, no file IO, no
// exceptions across the boundary, no unbounded loops. tools/rt_audit.py scans
// process(), onParameter(), onNoteIn() and popNoteOut() for it.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"

namespace aura::plugin::sandbox {

/// What a processor is told about the session before it processes anything.
/// Mirrors the arena's budget: a processor that prepared for more than this would
/// be preparing for blocks the transport cannot carry.
struct ProcessorSpec {
    double sampleRate = 48000.0;
    std::uint32_t maxBlockSize = 512;
    std::uint32_t maxChannels = 2;

    [[nodiscard]] bool operator==(const ProcessorSpec&) const noexcept = default;
};

/// Builds a ProcessorSpec from an arena's budget. One place for the mapping so the
/// helper and a test cannot disagree about what a session's limits are.
[[nodiscard]] ProcessorSpec processorSpecFor(const ArenaSpec& arena, double sampleRate) noexcept;

class SandboxProcessor {
public:
    SandboxProcessor() = default;
    virtual ~SandboxProcessor() = default;
    SandboxProcessor(const SandboxProcessor&) = delete;
    SandboxProcessor& operator=(const SandboxProcessor&) = delete;
    SandboxProcessor(SandboxProcessor&&) = delete;
    SandboxProcessor& operator=(SandboxProcessor&&) = delete;

    /// The identifier this processor was made from. Stable for the object's life;
    /// it is what the host logs when the helper reports a failure.
    [[nodiscard]] virtual const char* id() const noexcept = 0;

    /// Control thread. Called once before the first block, and again after a
    /// session's limits change. Everything the real-time path needs - buffers,
    /// lookup tables, per-channel state - is allocated here and nowhere else.
    /// Returning false means "I cannot run in this session", which the helper
    /// reports as helperExit::kProcessorFailed and exits on; a processor that
    /// cannot prepare must not limp along processing half-initialised state.
    [[nodiscard]] virtual bool prepare(const ProcessorSpec& spec) noexcept = 0;

    /// RT-safe. Processes `frames` of `input` into `output`.
    ///
    /// The two views never alias: the helper owns separate input and output
    /// scratch, so a processor may read and write in any order without a copy. A
    /// processor that wants in-place semantics can ignore `input`'s pointers and
    /// write to `output` - the host reads back whatever is in the output ring.
    ///
    /// `frames` is at most `spec.maxBlockSize`, and both views are at least
    /// `spec.maxChannels` wide and `frames` long. Every output frame must be
    /// written: the helper does not clear the buffer afterwards, because clearing
    /// and then overwriting would cost a pass over memory the audio thread does not
    /// have to spare.
    virtual void process(const dsp::AudioBlockView& input, const dsp::AudioBlockView& output,
                         std::uint32_t frames) noexcept = 0;

    /// RT-safe. A parameter change that crossed the boundary. `value` is normalised
    /// 0..1, the same convention the engine's own parameters use, so no adapter has
    /// to invent a second one.
    virtual void onParameter(const ParamRecord&) noexcept {}

    /// RT-safe. A note that crossed the boundary, already validated: the transport
    /// refuses a record whose type byte is out of range or Meta, so a processor
    /// never has to defend itself against a malformed message.
    virtual void onNoteIn(const midi::Message&) noexcept {}

    /// RT-safe. Offers a note the processor generated - an arpeggiator feeding the
    /// session, which is M6 carry-over 5 and the reason the note-out ring exists.
    /// Return false when there is nothing left to emit; the helper drains this until
    /// it does, once per block.
    [[nodiscard]] virtual bool popNoteOut(midi::Message&) noexcept { return false; }

    /// Control thread. The recovery point the crash protocol stands on (Decision 3
    /// in the research doc): after a helper dies, this blob is what a restarted one
    /// is fed. Empty means "no state worth keeping".
    ///
    /// Not wired across the boundary yet: the arena has nowhere to put a blob, and
    /// the helper must not touch the disk from its real-time thread, so connecting
    /// this is a layout change that belongs with the crash protocol in phase 4.
    /// Declared now because the shape of the interface should not be discovered
    /// later.
    [[nodiscard]] virtual std::vector<std::uint8_t> saveState() { return {}; }

    /// Control thread. The inverse. Returns false if the blob is not this
    /// processor's, which the caller treats as "start from defaults" rather than as
    /// a fatal error - a project recorded by a newer AURA is still playable.
    virtual bool restoreState(const std::uint8_t*, std::size_t) { return false; }
};

/// Makes a processor by id. Unknown ids are an error value, not a nullptr and not
/// an exception: the helper turns it into helperExit::kProcessorFailed and a line
/// on its diagnostic stream, and the host turns that into a message naming the
/// plug-in.
[[nodiscard]] AuraResult<std::unique_ptr<SandboxProcessor>> makeSandboxProcessor(
    const std::string& id);

/// Every id makeSandboxProcessor() accepts, for a diagnostic and for tests.
[[nodiscard]] std::vector<std::string> builtinProcessorIds();

} // namespace aura::plugin::sandbox
