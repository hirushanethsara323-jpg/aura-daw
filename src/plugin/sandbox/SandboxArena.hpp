// ============================================================================
// AURA DAW - plugin/sandbox/SandboxArena.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The memory two processes share when a plug-in is sandboxed (M7).
//
// This header is deliberately platform-free and allocation-free: it describes a
// layout and the rules for moving audio, notes, parameter changes and telemetry
// across it. The platform half - naming a region so a second process can find it -
// is SharedMemory.hpp; the policies and the reasoning behind them are in
// docs/research/PLUGIN_SANDBOX_RESEARCH.md.
//
// THE ONE RULE
// ------------
// The audio thread never waits for the helper. It writes its block into a slot,
// publishes it with a release store, then takes whatever the helper has finished
// and publishes *that* - one block behind. If the helper is late, the host keeps
// playing: the block goes dry, the underrun is counted, and the session does not
// stutter. A DAW that freezes to protect itself from a slow plug-in has not
// protected the user from anything.
//
// The price is exactly one block of latency, which is measurable and therefore
// compensable: the proxy reports it and the graph's delay compensation aligns it
// with every other path (M5's machinery, verified in
// tests/graph/DelayCompensationTests.cpp).
//
// WHY TWO SLOTS ARE ENOUGH
// ------------------------
// Sequence numbers, not indices, and a two-slot rotation:
//
//   host:   published = uploadSequence, finished = helperSequence (acquire)
//           if published != finished -> the helper is still working on the slot we
//           are about to reuse; the block is dropped and counted, and the audio
//           keeps flowing
//           otherwise: fill slot[published % 2] with this block,
//           uploadSequence = published + 1                    (release)
//   helper: consume the newest published block, process it *in place*,
//           helperSequence = the block it just finished         (release)
//   host:   read the result of block (published - 1) out of its slot, with the
//           helperSequence load *above* the hand-over - see publishAudio()
//
// The host only publishes when the helper has finished everything it was given, so
// exactly one block is ever in flight and the round trip is exactly one block: long
// enough for the helper to work in, short enough to be a latency the graph can
// compensate for. A helper that is late costs a block of audio, never a stall.
//
// Every hand-off is one release/acquire pair - the same discipline AURA's
// lock-free queues already use (core/SpscQueue.hpp). Slots are 64-byte aligned so
// the two sides never false-share the audio itself.
//
// NOT IN THE ARENA, ON PURPOSE: the plug-in's state blob and its parameter *list*.
// Both are variable-size and belong to the control channel (a pipe on Windows, a
// socketpair on POSIX), where blocking is fine because neither ever runs on the
// audio thread.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace aura::plugin::sandbox {

/// Bumped whenever the layout below changes. A helper built by a different AURA
/// must refuse the arena rather than misread it, so magic and version are checked
/// before anything else is touched.
inline constexpr std::uint32_t kArenaMagic = 0x41534131u; // "ASA1": Aura Sandbox Arena, v1
inline constexpr std::uint32_t kArenaVersion = 1u;

/// One slot being processed, one being written. See the header comment for why two
/// is enough - and it is also the minimum that is.
inline constexpr std::size_t kAudioSlots = 2;
inline constexpr std::size_t kSlotAlignment = 64;

/// Compile-time room. The arena is allocated once, before the audio thread starts;
/// a geometry that does not fit is a configuration error, not a runtime surprise.
inline constexpr std::size_t kMaxBlockSize = 4096;
inline constexpr std::size_t kMaxChannels = 8;
inline constexpr std::size_t kMaxEvents = 512;       ///< notes per block, each direction
inline constexpr std::size_t kMaxParamChanges = 512; ///< parameter points per block

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

/// One MIDI event, flattened so the helper can read it without including AURA's MIDI
/// header. Field order and values match midi::Message on purpose: the conversion is
/// a cast-free copy, not a translation.
struct EventRecord {
    std::uint8_t type = 0;  ///< midi::Message::Type
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0; ///< note number / controller
    std::uint8_t data2 = 0; ///< velocity / value
    std::int32_t sampleOffset = 0;
    std::int64_t timestampTicks = 0;
};
static_assert(std::is_trivially_copyable_v<EventRecord>);
// Field offsets, not just the size: a reordering that happens to keep sizeof() the
// same would still be a silent ABI break between two separately-built binaries.
static_assert(offsetof(EventRecord, type) == 0u);
static_assert(offsetof(EventRecord, channel) == 1u);
static_assert(offsetof(EventRecord, data1) == 2u);
static_assert(offsetof(EventRecord, data2) == 3u);
static_assert(offsetof(EventRecord, sampleOffset) == 4u);
static_assert(offsetof(EventRecord, timestampTicks) == 8u);
static_assert(sizeof(EventRecord) == 16, "the arena layout is an ABI between two processes");

/// One parameter change. Normalised, because that is what a plug-in's own
/// plain/normalised mapping takes; the host converts on its side of the fence.
struct ParamRecord {
    std::uint32_t id = 0;
    std::int32_t sampleOffset = 0;
    float normalised = 0.0f;
};
static_assert(std::is_trivially_copyable_v<ParamRecord>);
static_assert(offsetof(ParamRecord, id) == 0u);
static_assert(offsetof(ParamRecord, sampleOffset) == 4u);
static_assert(offsetof(ParamRecord, normalised) == 8u);
static_assert(sizeof(ParamRecord) == 12, "the arena layout is an ABI between two processes");

/// Transport as the helper's plug-in needs to see it - the shape VST 3's
/// ProcessContext and CLAP's transport both want.
struct TransportSnapshot {
    double sampleRate = 48000.0;
    double positionSamples = 0.0;
    double positionTicks = 0.0;
    double tempoBpm = 120.0;
    std::int32_t isPlaying = 0;
    std::int32_t isRendering = 0;
};
static_assert(std::is_trivially_copyable_v<TransportSnapshot>);

/// What the helper tells the host between blocks. Plain atomics: either side may
/// read them at any time without a handshake, and none of them synchronises audio
/// (the sequence numbers do that).
struct Mailbox {
    std::atomic<std::int32_t> reportedLatencySamples{0};
    std::atomic<std::int32_t> tailSamples{0};
    std::atomic<std::int32_t> active{0};
    std::atomic<std::uint32_t> helperGeneration{0}; ///< increments on every helper start
    std::atomic<std::uint32_t> processedBlocks{0};
    std::atomic<std::uint32_t> failedBlocks{0};
    std::atomic<std::uint32_t> helperParamCount{0};
    std::atomic<std::uint64_t> heartbeatMs{0}; ///< the helper's clock, for the watchdog
};

/// Fixed-capacity SPSC ring over shared memory. Wait-free on both sides, at most one
/// record of work per call, no allocation ever - the same contract as
/// core/SpscQueue.hpp, written here so the helper (which includes no AURA headers)
/// and the host agree on the bytes.
template <typename T, std::size_t Capacity>
class SharedRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "the mask-based ring needs a power-of-two capacity");
    static_assert(std::is_trivially_copyable_v<T>);

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Producer side. Returns false when full; the caller decides whether that is an
    /// overrun worth counting (for notes it is).
    bool tryPush(const T& value) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity)
            return false; // full
        records_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer side. Reads in publication order.
    bool tryPop(T& out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // empty
        out = records_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }
    /// Control thread only, and only while nothing is processing.
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    alignas(kSlotAlignment) std::atomic<std::uint64_t> head_{0};
    alignas(kSlotAlignment) std::atomic<std::uint64_t> tail_{0};
    alignas(kSlotAlignment) T records_[Capacity]{};
};

/// One block of audio, for the one trip it makes. The helper processes in place: the
/// slot it read is the slot it writes, which is what an insert does and what halves
/// the shared memory.
struct alignas(kSlotAlignment) AudioSlot {
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint64_t sequence = 0; ///< which block this is; diagnostics, not ordering
    alignas(kSlotAlignment) float samples[kMaxChannels * kMaxBlockSize]{};

    [[nodiscard]] float* channel(std::size_t index) noexcept {
        return samples + index * kMaxBlockSize;
    }
    [[nodiscard]] const float* channel(std::size_t index) const noexcept {
        return samples + index * kMaxBlockSize;
    }
};

// ---------------------------------------------------------------------------
// The arena
// ---------------------------------------------------------------------------

/// The whole shared region. Both sides build a *view* of the same bytes; the host
/// creates and initialises it (SharedMemory.hpp).
struct alignas(kSlotAlignment) SandboxArena {
    // --- identity ----------------------------------------------------------
    std::uint32_t magic = kArenaMagic;
    std::uint32_t version = kArenaVersion;
    std::uint32_t blockSize = 0; ///< frames per block the host sends
    std::uint32_t channels = 0;  ///< channels per block, both sides
    std::atomic<std::uint32_t> helperReady{0};
    std::atomic<std::uint32_t> hostQuit{0};

    // --- audio hand-off (protocol in the file header) -----------------------
    alignas(kSlotAlignment) AudioSlot audio[kAudioSlots];
    alignas(kSlotAlignment) std::atomic<std::uint64_t> uploadSequence{0}; ///< host publishes
    alignas(kSlotAlignment) std::atomic<std::uint64_t> helperSequence{0}; ///< helper publishes

    /// Counters that make a misbehaving helper visible instead of mysterious.
    alignas(kSlotAlignment) std::atomic<std::uint32_t> droppedBlocks{0}; ///< helper late
    alignas(kSlotAlignment) std::atomic<std::uint32_t> skippedBlocks{0}; ///< helper caught up by dropping

    // --- queues the audio thread itself uses -------------------------------
    alignas(kSlotAlignment) SharedRing<EventRecord, kMaxEvents> eventsToHelper;
    alignas(kSlotAlignment) SharedRing<EventRecord, kMaxEvents> eventsToHost;
    alignas(kSlotAlignment) SharedRing<ParamRecord, kMaxParamChanges> paramsToHelper;

    alignas(kSlotAlignment) TransportSnapshot transport{};
    alignas(kSlotAlignment) Mailbox mailbox{};

    // --- geometry helpers --------------------------------------------------
    [[nodiscard]] bool valid() const noexcept {
        return magic == kArenaMagic && version == kArenaVersion;
    }
    [[nodiscard]] bool fits(std::size_t frames, std::size_t channelCount) const noexcept {
        return frames > 0 && frames <= kMaxBlockSize && channelCount > 0 &&
               channelCount <= kMaxChannels;
    }
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the arena's sequence numbers must be lock-free: both sides are real-time");
static_assert(std::is_trivially_copyable_v<SharedRing<EventRecord, kMaxEvents>>);

/// Byte size of the arena: the host allocates this, the helper maps exactly this, so
/// it is one function and not two numbers that can drift.
[[nodiscard]] constexpr std::size_t arenaBytes() noexcept { return sizeof(SandboxArena); }

/// Places the arena into freshly created shared memory. Placement new rather than a
/// memset: the arena's members have initialisers, and "the bytes are zero" is not the
/// same thing as "the object exists" for anything with a non-trivial type - including
/// the atomics both sides read.
[[nodiscard]] inline SandboxArena* constructArena(void* memory) noexcept {
    return ::new (memory) SandboxArena();
}

/// The helper's view of the same bytes. It must *not* construct anything: the host
/// owns the object's lifetime, and re-initialising these atomics under a running host
/// would be a data race. Identity is checked first, so a helper built by a different
/// AURA refuses the arena instead of misreading it.
[[nodiscard]] inline SandboxArena* attachArena(void* memory) noexcept {
    auto* arena = static_cast<SandboxArena*>(memory);
    return arena->valid() ? arena : nullptr;
}

/// Why a block did not carry plug-in audio. The distinction matters: "the helper has
/// not started" is normal at the beginning of a session, "the helper is too slow" is
/// a performance problem the user should be told about.
enum class BlockOutcome : std::uint8_t {
    Processed,   ///< the helper's audio for the previous block was used
    NothingYet,  ///< the helper has not finished anything yet
    HelperLate,  ///< the helper still holds the slot we needed: dry, counted
    GeometryGone ///< the result does not match this block's size (reconfigured?)
};

/// The host's side of the audio hand-off. Real-time safe by construction: fixed
/// storage, bounded work, no allocation, no locks, no waiting. A helper that is late
/// costs output, never time.
class HostAudioPort {
public:
    explicit HostAudioPort(SandboxArena& arena) noexcept : arena_(arena) {}

    /// Publishes one block of the engine's audio and, once the helper has answered one,
    /// overwrites `channels` with the *previous* block's processed audio (in place - an
    /// insert processes the engine's buffer, and the engine refills it before the next
    /// call). The delay is one block, always, and the caller compensates for it.
    ///
    /// The engine's audio is copied *into* the shared slot and the processed audio is
    /// copied *out*: two copies per block, which is the price of the process boundary
    /// and is what the sandbox benchmark will measure.
    BlockOutcome publishAudio(float* const* channels, std::size_t channelCount,
                              std::size_t frames) noexcept {
        if (channels == nullptr || !arena_.fits(frames, channelCount))
            return BlockOutcome::GeometryGone;

        const std::uint64_t published = arena_.uploadSequence.load(std::memory_order_relaxed);
        // Loaded *before* the hand-over on purpose: it is the "is the helper done" check
        // below, and reading it here is also what keeps the round trip exactly one block
        // when the helper is fast. See the note on the read-back at the end.
        const std::uint64_t finished = arena_.helperSequence.load(std::memory_order_acquire);

        // Is the helper done with the block we handed it last time? The slot we are
        // about to reuse is the one it was working in, so this single check is what
        // makes two slots sufficient - and it never waits. `finished == published` also
        // means block `published - 1` is complete, which is the block read back below.
        if (published != finished) {
            arena_.droppedBlocks.fetch_add(1, std::memory_order_relaxed);
            return BlockOutcome::HelperLate;
        }

        const std::uint32_t busChannels = arena_.channels;
        AudioSlot& slot = arena_.audio[published % kAudioSlots];
        slot.frames = static_cast<std::uint32_t>(frames);
        slot.channels = busChannels;
        slot.sequence = published;
        for (std::uint32_t channel = 0; channel < busChannels; ++channel) {
            float* destination = slot.channel(channel);
            if (channel < channelCount && channels[channel] != nullptr)
                std::memcpy(destination, channels[channel], frames * sizeof(float));
            else
                std::memset(destination, 0, frames * sizeof(float));
        }
        arena_.uploadSequence.store(published + 1, std::memory_order_release);

        // Take the *previous* block's answer, out of the slot this one is not using.
        // The load it depends on happened above, before the hand-over, and that order
        // is the whole reason this is deterministic: read after the store instead, and
        // a helper quick enough to answer inside this call would have finished the
        // block we just published - one block of latency sometimes, two blocks other
        // times. Latency that moves from block to block is latency no host can
        // compensate, so the fast case is deliberately ignored: block n is answered on
        // call n + 1, always.
        if (published == 0)
            return BlockOutcome::NothingYet; // nothing has been answered yet
        const AudioSlot& result = arena_.audio[(published - 1) % kAudioSlots];
        if (result.frames != frames || result.channels != busChannels)
            return BlockOutcome::GeometryGone;
        for (std::uint32_t channel = 0; channel < busChannels; ++channel) {
            if (channel < channelCount && channels[channel] != nullptr)
                std::memcpy(channels[channel], result.channel(channel), frames * sizeof(float));
        }
        return BlockOutcome::Processed;
    }

    [[nodiscard]] std::size_t droppedBlocks() const noexcept {
        return arena_.droppedBlocks.load(std::memory_order_relaxed);
    }

private:
    SandboxArena& arena_;
};

/// The helper's side. Consumes the newest block the host published, processes it in
/// place, publishes the result. Nothing here waits either: a helper that ran late
/// skips what it missed and the host counts it.
class HelperAudioPort {
public:
    /// `consumed_` starts at the helper's own watermark, not at zero: a helper that
    /// attaches to an arena the host was already running (a respawn after a crash)
    /// must not report the blocks it never had a chance to take as skipped.
    explicit HelperAudioPort(SandboxArena& arena) noexcept
        : arena_(arena), consumed_(arena.helperSequence.load(std::memory_order_relaxed)) {}

    /// The newest published block, or nullptr when there is nothing new to do. The
    /// helper is not the real-time-critical side, so it is allowed to sleep on
    /// nullptr rather than spin.
    [[nodiscard]] AudioSlot* acquireBlock() noexcept {
        const std::uint64_t available = arena_.uploadSequence.load(std::memory_order_acquire);
        // The helper's own record of the last block it took - deliberately not
        // helperSequence, which means "finished". Reading that one here would count a
        // block as skipped merely because it had been taken but not yet published.
        if (available == consumed_)
            return nullptr;
        if (available > consumed_ + 1)
            arena_.skippedBlocks.fetch_add(static_cast<std::uint32_t>(available - consumed_ - 1),
                                           std::memory_order_relaxed);
        consumed_ = available;
        return &arena_.audio[(available - 1) % kAudioSlots];
    }

    /// Publishes the result of the block `acquireBlock()` returned. Ordering matters:
    /// the payload is already in the slot (the plug-in wrote it there), so this store
    /// is the release that tells the host it may read it. It names the block that was
    /// actually processed, never "whatever the host has published since" - the host
    /// would otherwise read back a block no plug-in has touched.
    void publishResult() noexcept {
        arena_.helperSequence.store(consumed_, std::memory_order_release);
        arena_.mailbox.processedBlocks.fetch_add(1, std::memory_order_relaxed);
    }

    /// The plug-in failed on this block: publish it (dry - the slot still holds the
    /// input) and count it, so the host can see the sandbox is not doing work instead
    /// of wondering why a plug-in went silent.
    void publishFailure() noexcept {
        arena_.helperSequence.store(consumed_, std::memory_order_release);
        arena_.mailbox.failedBlocks.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t skippedBlocks() const noexcept {
        return arena_.skippedBlocks.load(std::memory_order_relaxed);
    }

private:
    SandboxArena& arena_;
    std::uint64_t consumed_ = 0; ///< the last block this helper took. Helper-side only.
};

} // namespace aura::plugin::sandbox
