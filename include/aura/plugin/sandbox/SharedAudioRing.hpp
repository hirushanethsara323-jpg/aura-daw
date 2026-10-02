// ============================================================================
// AURA DAW - plugin/sandbox/SharedAudioRing.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The audio half of the plug-in sandbox transport: a one-block-deferred pipeline
// over the arena defined in SandboxArena.hpp.
//
// Why deferred rather than synchronous (Decision 2 of
// docs/research/PLUGIN_SANDBOX_RESEARCH.md): a mixer that writes a block and
// *waits* for another process to finish it has a deadline it can miss, and when it
// misses, the user hears it. So the host writes block n and reads back block n-1,
// which makes the audio thread's work bounded (a copy in, a copy out) and its
// latency exactly `slots - 1` blocks - measurable, and therefore compensable
// through the graph's existing delay compensation rather than a mystery offset.
//
// Ordering. Each ring is a pair of monotonic counters plus a per-slot sequence:
//
//   published  total blocks the producer has made visible   (producer writes)
//   consumed   total blocks the consumer has finished with   (consumer writes)
//   slot.sequence  0 never written, 2b+1 while block b is being written,
//                  2b+2 when block b is stable
//
// The producer writes the payload plainly and publishes with a RELEASE store; the
// consumer observes with an ACQUIRE load, which is the same discipline SpscQueue
// uses, in shared memory instead of process memory. The per-slot sequence is what
// SpscQueue does not need: it lets the consumer detect a slot that was overwritten
// while it was copying - the thing that happens when the peer misbehaves, hangs or
// dies mid-block - and discard the copy instead of handing torn audio to the
// mixer. Detection costs one extra load; recovery is to go dry for that block,
// which is Decision 3 step 2.
//
// Thread contract is per method and stated on each one. The short version:
// attach()/reset() are control thread; push*/pop*/exchange() are RT-safe,
// wait-free and allocation-free.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/plugin/sandbox/SandboxArena.hpp"

namespace aura::plugin::sandbox {

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

/// What a push did. Returned rather than signalled by exception or log because
/// the audio thread may not do either, and the caller needs to know whether the
/// block reached the peer in order to keep honest counters.
enum class PushOutcome : std::uint8_t {
    Published,  ///< written and visible to the consumer
    RingFull,   ///< the consumer has not released the slot yet; block dropped
    TooLarge,   ///< the block does not fit the arena's channel/frame budget
    Empty,      ///< the caller handed over a view with no channels or no frames
    WrongSide,  ///< called from the side that does not own this direction
    NotAttached,
};

/// What a pop did.
enum class PopOutcome : std::uint8_t {
    Consumed, ///< a block was copied out
    Empty,    ///< nothing new since the last pop - the peer has not produced yet
    Torn,     ///< the slot changed while it was being copied; the copy was discarded
    WrongSide,
    NotAttached,
};

/// A pop's full report. `blocksLost` is non-zero when the producer lapped the
/// consumer, which cannot happen in a correctly-paced pipeline and is therefore
/// either a peer that stopped consuming or a bug - both of which need a number
/// attached rather than a silent gap in the audio.
struct PopResult {
    PopOutcome outcome = PopOutcome::Empty;
    /// The ring's own index for the block that was read - useful for the torn and
    /// lost-block accounting, which are about the ring rather than the audio.
    std::uint64_t block = 0;
    /// The block number the PRODUCER declared this audio belongs to. For an output
    /// ring that is the input block the helper processed, which is how the host
    /// knows how stale a result is when blocks have been dropped.
    std::uint64_t origin = 0;
    std::uint32_t frames = 0;
    std::uint64_t blocksLost = 0;
};

/// What one host audio callback did.
struct BlockExchange {
    PushOutcome pushed = PushOutcome::NotAttached;
    PopResult popped{};
};

/// Anomaly counters. Written by the owning side only, read by the control thread;
/// relaxed atomics because a diagnostic must not become a data race, and because
/// every one of them is incremented on a path that is already an anomaly rather
/// than once per block.
struct TransportStats {
    std::atomic<std::uint64_t> overruns{0};    ///< pushes refused: the ring was full
    std::atomic<std::uint64_t> underruns{0};   ///< pops that found nothing
    std::atomic<std::uint64_t> tornReads{0};   ///< pops discarded: slot overwritten mid-copy
    std::atomic<std::uint64_t> blocksLost{0};  ///< blocks skipped because the producer lapped us
    std::atomic<std::uint64_t> wrongSideCalls{0};
    std::atomic<std::uint64_t> oversizedBlocks{0};

    TransportStats() = default;
    TransportStats(const TransportStats&) = delete;
    TransportStats& operator=(const TransportStats&) = delete;

    /// Snapshot for diagnostics and tests.
    struct Snapshot {
        std::uint64_t overruns = 0;
        std::uint64_t underruns = 0;
        std::uint64_t tornReads = 0;
        std::uint64_t blocksLost = 0;
        std::uint64_t wrongSideCalls = 0;
        std::uint64_t oversizedBlocks = 0;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void reset() noexcept;
};

// ---------------------------------------------------------------------------
// Per-slot and per-ring headers in the arena
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
// C4324 ("structure was padded due to alignment specifier") is the point of the
// padding in the arena's ring headers: each independently-written index owns a
// cache line so the two processes never invalidate a line the other is hammering.
// Scoped here, as ADR-0013 requires, rather than silenced project-wide.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// ---------------------------------------------------------------------------
// PodRing: a fixed-capacity SPSC ring of POD records over arena bytes
// ---------------------------------------------------------------------------

/// Non-owning single-producer / single-consumer ring over a region of the arena.
///
/// Unlike SpscQueue this takes its capacity at run time (the arena is sized from
/// the session's block size and channel count, so it cannot be a template
/// parameter) and stores monotonic counters instead of head/tail indices. That
/// buys one extra usable slot: with head/tail, "full" and "empty" are only
/// distinguishable by sacrificing a slot, whereas `published - consumed == capacity`
/// is unambiguous on its own.
///
/// Thread contract: `attach()`/`reset()` are CONTROL THREAD ONLY. `push()` is
/// RT-safe and may be called only by the producer; `pop()` is RT-safe and only by
/// the consumer. Both are wait-free.
template <typename T>
class PodRing {
    static_assert(std::is_trivially_copyable_v<T>, "crosses a process boundary");
    static_assert(std::is_standard_layout_v<T>,
                  "both processes must agree on where every field lives");

public:
    PodRing() = default;

    /// Attaches to a region laid out by ArenaView. The capacity must be a power of
    /// two; computeLayout() guarantees that for arena-built rings.
    void attach(const PodRingRegion& region) noexcept {
        base_ = region.base;
        capacity_ = region.capacity;
        mask_ = capacity_ - 1u;
        attached_ = base_ != nullptr && capacity_ >= 2u && (capacity_ & mask_) == 0u;
    }

    [[nodiscard]] bool isAttached() const noexcept { return attached_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

    /// Producer. Returns false when the ring is full - the caller decides, because
    /// a dropped MIDI event is a glitch and a blocked audio thread is a dropout.
    bool push(const T& value) noexcept {
        if (!attached_)
            return false;
        const std::uint64_t published = publishedCounter()->load(std::memory_order_relaxed);
        const std::uint64_t consumed = consumedCounter()->load(std::memory_order_acquire);
        if (published - consumed >= static_cast<std::uint64_t>(capacity_)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        storage()[published & mask_] = value;
        publishedCounter()->store(published + 1, std::memory_order_release);
        return true;
    }

    /// Consumer. Returns false when the ring is empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        if (!attached_)
            return false;
        const std::uint64_t consumed = consumedCounter()->load(std::memory_order_relaxed);
        const std::uint64_t published = publishedCounter()->load(std::memory_order_acquire);
        if (published <= consumed) {
            starved_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        out = storage()[consumed & mask_];
        consumedCounter()->store(consumed + 1, std::memory_order_release);
        return true;
    }

    /// Producer-side: pushes refused because the ring was full.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    /// Consumer-side: pops that found the ring empty. Not an error - a block with
    /// no MIDI in it is the common case - but it belongs in the diagnostics.
    [[nodiscard]] std::uint64_t starved() const noexcept {
        return starved_.load(std::memory_order_relaxed);
    }

    /// Empties the ring. CONTROL THREAD ONLY, and only while neither side is
    /// running: it rewinds both counters, which is meaningless mid-flight.
    void reset() noexcept {
        if (!attached_)
            return;
        publishedCounter()->store(0, std::memory_order_relaxed);
        consumedCounter()->store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        starved_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t publishedCount() const noexcept {
        return attached_ ? publishedCounter()->load(std::memory_order_acquire) : 0u;
    }
    [[nodiscard]] std::uint64_t consumedCount() const noexcept {
        return attached_ ? consumedCounter()->load(std::memory_order_acquire) : 0u;
    }

private:
    [[nodiscard]] std::atomic<std::uint64_t>* publishedCounter() const noexcept {
        return reinterpret_cast<std::atomic<std::uint64_t>*>(base_);
    }
    [[nodiscard]] std::atomic<std::uint64_t>* consumedCounter() const noexcept {
        return reinterpret_cast<std::atomic<std::uint64_t>*>(base_ + kCacheLine);
    }
    [[nodiscard]] T* storage() const noexcept {
        return reinterpret_cast<T*>(base_ + sizeof(PodRingHeader));
    }

    std::uint8_t* base_ = nullptr;
    std::uint32_t capacity_ = 0;
    std::uint32_t mask_ = 0;
    bool attached_ = false;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> starved_{0};
};

/// The audio slot ring: `slots` planar blocks with a per-slot sequence counter.
///
/// Thread contract: `attach()`/`reset()` are CONTROL THREAD ONLY; `push()` is
/// RT-safe for the single producer; `pop()` is RT-safe for the single consumer.
class AudioSlotRing {
public:
    AudioSlotRing() = default;

    /// Attaches to a region laid out by ArenaView. CONTROL THREAD ONLY.
    void attach(const AudioRingRegion& region) noexcept;

    [[nodiscard]] bool isAttached() const noexcept { return attached_; }
    [[nodiscard]] std::uint32_t slots() const noexcept { return slots_; }
    [[nodiscard]] std::uint32_t maxChannels() const noexcept { return maxChannels_; }
    [[nodiscard]] std::uint32_t maxBlockSize() const noexcept { return maxBlockSize_; }

    /// Producer. Copies `source` into the next slot and publishes it. The ring owns
    /// the block index - the caller cannot desynchronise it by passing a wrong one,
    /// and a producer that runs ahead of its consumer gets RingFull rather than
    /// silently overwriting a block the consumer has not read.
    ///
    /// RT-safe: one bounded memcpy per channel and two atomic stores, no
    /// allocation, no lock, and it never waits for the consumer.
    PushOutcome push(const dsp::AudioBlockView& source, std::uint64_t origin) noexcept;

    /// Consumer. Copies the oldest unread block into `destination` and releases its
    /// slot. The view itself is not modified; the samples its pointers address are.
    /// Frames and channels are clamped to the destination's, so a peer that reports
    /// a larger block than the arena allows cannot write past the caller's buffer.
    ///
    /// RT-safe and bounded: one verification read of the slot sequence, never a
    /// loop. A slot that changed while it was being copied is reported as Torn and
    /// released, so a misbehaving peer costs one dry block rather than a stall.
    PopResult pop(const dsp::AudioBlockView& destination) noexcept;

    [[nodiscard]] std::uint64_t publishedCount() const noexcept;
    [[nodiscard]] std::uint64_t consumedCount() const noexcept;
    /// Blocks readable right now.
    [[nodiscard]] std::uint64_t available() const noexcept;

    /// Rewinds both counters and clears every slot sequence, so the ring can be
    /// reused after a plug-in restart. CONTROL THREAD ONLY, while both sides are
    /// stopped - and note that the audio itself is NOT zeroed, because a slot that
    /// is never published can never be read.
    void reset() noexcept;

private:
    /// The ring's own counters live in a PodRingHeader at the region's base; the
    /// slots follow it. Accessors rather than members, because a view over shared
    /// bytes must not keep a second copy of anything the peer can change.
    [[nodiscard]] std::atomic<std::uint64_t>* publishedCounter() const noexcept;
    [[nodiscard]] std::atomic<std::uint64_t>* consumedCounter() const noexcept;
    [[nodiscard]] std::uint8_t* slotsBase() const noexcept;
    [[nodiscard]] AudioSlotHeader* header(std::uint64_t block) const noexcept;
    [[nodiscard]] float* payload(std::uint64_t block) const noexcept;

    std::uint8_t* base_ = nullptr;
    std::uint32_t slots_ = 0;
    std::uint32_t maxChannels_ = 0;
    std::uint32_t maxBlockSize_ = 0;
    std::size_t slotStride_ = 0;
    bool attached_ = false;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// SharedAudioRing: the two directions plus the deferred-pipeline protocol
// ---------------------------------------------------------------------------

/// The transport as the host and the helper each see it. One instance per side,
/// attached to the same arena with a different `Side`; the side is part of the
/// object so that a caller which pushes in the wrong direction gets a
/// `WrongSide` outcome and a counter instead of a corrupted ring.
///
/// Thread contract: `attach()`/`reset()` are CONTROL THREAD ONLY. The host
/// methods (`pushInput`, `popOutput`, `exchange`) are called by the audio thread;
/// the helper methods (`popInput`, `pushOutput`) by the helper's RT loop. All are
/// RT-safe, wait-free and allocation-free.
class SharedAudioRing {
public:
    enum class Side : std::uint8_t { Host, Helper };

    SharedAudioRing() = default;
    SharedAudioRing(const SharedAudioRing&) = delete;
    SharedAudioRing& operator=(const SharedAudioRing&) = delete;

    /// Attaches to a validated arena. `arena` must have come from
    /// ArenaView::create() or ArenaView::attach(); this does no validation of its
    /// own, because the header check has already happened and doing it twice would
    /// only hide which side failed. CONTROL THREAD ONLY.
    Status attach(const ArenaView& arena, Side side);

    [[nodiscard]] bool isAttached() const noexcept { return attached_; }
    [[nodiscard]] Side side() const noexcept { return side_; }

    /// Blocks of latency this transport adds: `slots - 1`. Two slots, the default,
    /// is one block - the number Decision 2 promises and the graph compensates.
    [[nodiscard]] std::uint32_t latencyBlocks() const noexcept;
    /// The same in samples, which is what a SandboxedPluginInstance reports through
    /// PluginInstance::latencySamples(). Returns 0 when not attached, because a
    /// transport that is not running adds no latency and claiming otherwise would
    /// shift the session by a block for nothing.
    [[nodiscard]] std::int32_t latencyFrames(std::uint32_t blockSize) const noexcept;

    // ---- host side, called by the audio thread ----------------------------

    /// Publishes a block to the helper. `origin` is the host's absolute block
    /// counter, which the helper echoes back on the output so the host can tell how
    /// stale a result is.
    PushOutcome pushInput(const dsp::AudioBlockView& input, std::uint64_t origin) noexcept;
    /// Takes the block the helper finished.
    PopResult popOutput(const dsp::AudioBlockView& output) noexcept;
    /// One audio callback: take the previous result, then publish the current block.
    /// Pop happens first because it gives the helper the longest possible time to
    /// have finished block n-1 before the host commits to block n.
    BlockExchange exchange(std::uint64_t origin, const dsp::AudioBlockView& input,
                           const dsp::AudioBlockView& output) noexcept;

    // ---- helper side, called by the helper's RT loop ----------------------

    PopResult popInput(const dsp::AudioBlockView& input) noexcept;
    /// Publishes a processed block. `origin` should be the popped input's origin, so
    /// the host can correlate the result with the block it sent.
    PushOutcome pushOutput(const dsp::AudioBlockView& output, std::uint64_t origin) noexcept;

    // ---- record rings -----------------------------------------------------
    //
    // Direction is fixed by the design, not chosen per call: parameters and
    // incoming notes flow host -> helper, and note output (an arpeggiator feeding
    // the session) flows helper -> host. The producer pushes, the consumer pops,
    // and a call from the wrong side is refused.

    [[nodiscard]] PodRing<ParamRecord>& parameters() noexcept { return params_; }
    [[nodiscard]] PodRing<MidiRecord>& notesIn() noexcept { return notesIn_; }
    [[nodiscard]] PodRing<MidiRecord>& notesOut() noexcept { return notesOut_; }

    /// Host side: queues a normalised parameter change for the helper.
    bool pushParameter(const ParamRecord& record) noexcept;
    /// Host side: queues a MIDI event for the helper.
    bool pushNote(const midi::Message& message) noexcept;
    /// Helper side: takes a parameter change the host queued.
    bool popParameter(ParamRecord& out) noexcept;
    /// Helper side: takes a MIDI event the host queued.
    bool popNoteIn(midi::Message& out) noexcept;
    /// Helper side: queues a note the plug-in produced (an arpeggiator feeding the
    /// session, M6 carry-over item 5).
    bool pushNoteOut(const midi::Message& message) noexcept;
    /// Host side: takes a note the plug-in produced.
    bool popNoteOut(midi::Message& out) noexcept;

    /// The arena's telemetry mailbox, or nullptr when not attached. Both sides may
    /// read it; only the helper writes it.
    [[nodiscard]] Telemetry* telemetry() const noexcept { return telemetry_; }
    [[nodiscard]] ControlBlock* control() const noexcept { return control_; }

    [[nodiscard]] const TransportStats& stats() const noexcept { return stats_; }
    [[nodiscard]] TransportStats& stats() noexcept { return stats_; }

    /// Clears counters, rewinds both audio rings and empties the record rings.
    /// CONTROL THREAD ONLY, with the helper stopped.
    void reset() noexcept;

private:
    /// Folds a pop's outcome into the local counters. Split out so the four pop
    /// entry points account identically instead of four copies drifting apart.
    void accountPop(const PopResult& result) noexcept;

    /// Publishes this side's anomaly counters into the telemetry mailbox so the
    /// supervisor can see a peer degrading without asking it. Called on the anomaly
    /// path only, never once per block, and only by the helper - see the definition
    /// for why the host keeps its counters to itself.
    void publishAnomalies() noexcept;

    Side side_ = Side::Host;
    bool attached_ = false;
    AudioSlotRing input_;
    AudioSlotRing output_;
    PodRing<ParamRecord> params_;
    PodRing<MidiRecord> notesIn_;
    PodRing<MidiRecord> notesOut_;
    ControlBlock* control_ = nullptr;
    Telemetry* telemetry_ = nullptr;
    TransportStats stats_;
};

} // namespace aura::plugin::sandbox
