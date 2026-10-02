// ============================================================================
// AURA DAW - plugin/sandbox/SandboxArena.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// The byte layout a sandboxed plug-in's helper process and AURA share.
//
// M7 phase 1 (docs/research/PLUGIN_SANDBOX_RESEARCH.md, Decision 2). The
// transport is written before the process that uses it, because the transport is
// the half that has to be right: a helper that is wrong costs one process, a
// layout that is wrong corrupts audio or stalls the mixer.
//
// Three rules make this safe across a process boundary.
//
//   1. NOTHING IN THE ARENA IS A POINTER. Every region is found by an offset
//      from the base, so the two processes may map the same bytes at different
//      addresses. A layout that stored a pointer would pass every
//      single-process test and corrupt its peer the first time it ran for real,
//      which is why "an arena keeps working after being moved" is a test and not
//      a hope.
//   2. Every value that crosses is POD and fixed size, and every atomic is
//      asserted lock-free, so its representation is the integer itself rather
//      than something a compiler may choose per translation unit. Both ends are
//      built from this repository and attach() refuses a peer whose layout
//      version differs - that is a requirement of the design, not a convenience.
//   3. The header is validated before anything is touched. A peer that is a
//      different build, a stale mapping or garbage yields an error value, not a
//      crash. The same discipline the project manifest and the scan host's JSON
//      already use.
//
// Thread contract: create()/attach() and everything in this header is
// CONTROL THREAD ONLY. The audio thread's entry points live in
// SharedAudioRing.hpp and are marked RT-safe individually.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "aura/core/Errors.hpp"
#include "aura/midi/Midi.hpp"

namespace aura::plugin::sandbox {

// ---------------------------------------------------------------------------
// Identity and limits
// ---------------------------------------------------------------------------

/// 'A''U''R''A' as a little-endian u32. A magic is not a security check; it is a
/// cheap way to turn "the peer mapped something else" into a readable error.
inline constexpr std::uint32_t kArenaMagic = 0x41525541u;

/// Bumped whenever a region moves, changes size or changes meaning. Two builds
/// that disagree on this must not share an arena - silently interpreting a peer's
/// bytes through the wrong offsets is the failure mode this prevents.
inline constexpr std::uint32_t kLayoutVersion = 1u;

/// Cache line used for padding. Both target architectures (x86-64 Windows and
/// the Linux CI runners) use 64; a larger value would only waste arena bytes.
inline constexpr std::size_t kCacheLine = 64u;

/// Depth of each audio slot ring. Two is the design's promise: enough slack that
/// the producer is never waiting on the consumer, and exactly one block of added
/// latency (see latencyFrames()). More slots buy tolerance to a slow helper at
/// the price of PDC the whole session has to compensate.
inline constexpr std::uint32_t kDefaultAudioSlots = 2u;
inline constexpr std::uint32_t kMinAudioSlots = 2u;
inline constexpr std::uint32_t kMaxAudioSlots = 8u;

/// Record rings are power-of-two sized (the index mask depends on it) and
/// deliberately generous: a block carries a handful of MIDI events and a few
/// parameter moves, so 256 is headroom, not a budget.
inline constexpr std::uint32_t kDefaultRecordCapacity = 256u;
inline constexpr std::uint32_t kMinRecordCapacity = 4u;
inline constexpr std::uint32_t kMaxRecordCapacity = 65536u;

// ---------------------------------------------------------------------------
// Records that cross the boundary
// ---------------------------------------------------------------------------

/// One parameter change. The value is NORMALISED (0..1) because the wire format
/// must not depend on a plug-in's own plain ranges: those are discovered in the
/// helper, which is the only process that has loaded the plug-in. `sampleOffset`
/// is carried even though the adapter applies changes at offset 0 today, so that
/// sample-accurate automation (M6 carry-over item 5) is a helper-side change
/// rather than a wire-format change.
struct ParamRecord {
    std::int32_t paramId = 0;
    std::int32_t sampleOffset = 0;
    float value = 0.0f;
};
// The offsets are the assertion that matters: a wire record with padding between
// fields would copy uninitialised stack bytes into shared memory, and two builds
// could disagree about where a field lives. `has_unique_object_representations`
// is NOT the check for that - it is false for any struct containing a float,
// because +0.0/-0.0 and the NaNs are distinct bit patterns with equal values.
static_assert(sizeof(ParamRecord) == 12, "wire records must not gain padding");
static_assert(std::is_trivially_copyable_v<ParamRecord>, "crosses a process boundary");
static_assert(std::is_standard_layout_v<ParamRecord>, "offsetof must be meaningful");
static_assert(offsetof(ParamRecord, paramId) == 0 && offsetof(ParamRecord, sampleOffset) == 4 &&
                  offsetof(ParamRecord, value) == 8,
              "no padding between wire fields");

/// One MIDI event on the wire. This is NOT `midi::Message`: that type carries a
/// `Type` enumerator whose width a compiler may choose and a tick timestamp the
/// helper has no use for (it works in samples). A fixed 8-byte record with a byte
/// type tag is what a peer can be trusted to read.
struct MidiRecord {
    std::uint8_t type = 0;   ///< midi::Message::Type as a byte
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::int32_t sampleOffset = 0;
};
static_assert(sizeof(MidiRecord) == 8, "wire records must not gain padding");
static_assert(std::is_trivially_copyable_v<MidiRecord>, "crosses a process boundary");
static_assert(std::is_standard_layout_v<MidiRecord>, "offsetof must be meaningful");
static_assert(offsetof(MidiRecord, type) == 0 && offsetof(MidiRecord, channel) == 1 &&
                  offsetof(MidiRecord, data1) == 2 && offsetof(MidiRecord, data2) == 3 &&
                  offsetof(MidiRecord, sampleOffset) == 4,
              "no padding between wire fields");

/// Wire conversion. Both directions are RT-safe and total: a record whose type
/// byte is not a `midi::Message::Type` (a corrupt or hostile peer) is rejected by
/// returning false rather than cast into existence. Meta events are refused in
/// the first direction because they are clip-internal and never routed to a
/// plug-in.
[[nodiscard]] bool toMidiRecord(const midi::Message& message, MidiRecord& out) noexcept;
[[nodiscard]] bool toMidiMessage(const MidiRecord& record, midi::Message& out) noexcept;

// ---------------------------------------------------------------------------
// Transport snapshot, control flags, telemetry
// ---------------------------------------------------------------------------

/// Bits in ControlBlock::hostRequests. Set by the host, cleared by the helper
/// once acted on, so a request is an edge the helper cannot miss between blocks.
namespace request {
inline constexpr std::uint32_t kReset = 1u << 0;
inline constexpr std::uint32_t kSaveState = 1u << 1;
inline constexpr std::uint32_t kShutdown = 1u << 2;
} // namespace request

/// Bits in ControlBlock::helperState. The helper owns this word.
namespace helperState {
inline constexpr std::uint32_t kReady = 1u << 0;     ///< plug-in loaded, loop running
inline constexpr std::uint32_t kLoadFailed = 1u << 1; ///< gave up; reason is on the control channel
inline constexpr std::uint32_t kStateReady = 1u << 2; ///< a save-state request was served
inline constexpr std::uint32_t kExiting = 1u << 3;    ///< clean shutdown in progress
} // namespace helperState

/// Bits in TransportSnapshot::flags.
namespace transportFlag {
inline constexpr std::uint32_t kPlaying = 1u << 0;
inline constexpr std::uint32_t kRendering = 1u << 1;
} // namespace transportFlag

/// What the helper needs to know about the session. Published by the host under a
/// seqlock (ControlBlock::transportVersion), read by the helper with a bounded
/// number of retries - see readTransportSnapshot().
struct TransportSnapshot {
    double sampleRate = 48000.0;
    double tempoBpm = 120.0;
    std::int64_t positionSamples = 0;
    std::int64_t positionTicks = 0;
    std::uint32_t blockSize = 0;
    std::uint32_t channelCount = 0;
    std::uint32_t flags = 0;
};
static_assert(std::is_trivially_copyable_v<TransportSnapshot>, "published as a block of bytes");

/// Host -> helper requests plus the transport snapshot. One cache line per
/// independently-written word: the audio thread bumps the snapshot every block
/// while the control thread sets request bits rarely, and sharing a line between
/// them would make the rare write invalidate the hot one.
#if defined(_MSC_VER)
// C4324 ("structure was padded due to alignment specifier") is the point of the
// padding in ControlBlock and Telemetry: each independently-written word owns a
// cache line so a rare control-thread write cannot invalidate the line the audio
// thread reads every block. Scoped here, as ADR-0013 requires, never globally.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ControlBlock {
    alignas(kCacheLine) std::atomic<std::uint32_t> hostRequests{0};
    alignas(kCacheLine) std::atomic<std::uint32_t> helperState{0};
    /// Seqlock version for `transport`: odd while being written, even when
    /// stable. Written only by the host's audio thread.
    alignas(kCacheLine) std::atomic<std::uint32_t> transportVersion{0};
    TransportSnapshot transport{};
};

/// Helper -> host mailbox. Single atomics, no seqlock: each value is read on its
/// own and a stale read of a diagnostic is not a correctness problem. The one
/// exception is `blocksProcessed`, which doubles as the watchdog heartbeat -
/// "no progress for N blocks" is the criterion the design chose because it needs
/// no clock on the RT path (Decision 3, step 5).
struct Telemetry {
    /// Doubles as the watchdog heartbeat: "no progress for N blocks" is the
    /// criterion Decision 3 chose, and a counter needs no clock on the RT path.
    alignas(kCacheLine) std::atomic<std::uint64_t> blocksProcessed{0};
    alignas(kCacheLine) std::atomic<std::int32_t> latencySamples{0};
    std::atomic<std::int32_t> tailSamples{0};
    std::atomic<float> cpuLoadEstimate{0.0f};
    std::atomic<std::uint32_t> cpuSpikes{0};
    /// Transport anomalies as seen from the helper: pushes refused because the peer
    /// fell behind, pops that found nothing, pops discarded because a slot changed
    /// mid-copy, and blocks skipped because the producer lapped the consumer.
    std::atomic<std::uint32_t> overruns{0};
    std::atomic<std::uint32_t> underruns{0};
    std::atomic<std::uint32_t> tornReads{0};
    std::atomic<std::uint32_t> blocksLost{0};
    /// Wire records refused because they did not validate (an unknown MIDI type
    /// byte, say). Counted rather than cast into existence.
    std::atomic<std::uint32_t> rejectedRecords{0};

    Telemetry() = default;
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;
};

static_assert(std::atomic<float>::is_always_lock_free,
              "the telemetry mailbox must be a plain word in shared memory");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the block counter is the watchdog heartbeat and may not lock");

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Ring headers: the bytes each region starts with
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
// C4324 ("structure was padded due to alignment specifier") is the point of the
// padding here: every independently-written index owns a cache line, so a rare
// write by one process cannot invalidate the line the other reads every block.
// Scoped to these declarations, as ADR-0013 requires, never project-wide.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

/// Header of one audio slot, followed in the arena by `maxChannels *
/// maxBlockSize` floats in PLANAR order (channel c at `c * maxBlockSize`).
///
/// Planar rather than interleaved, unlike AudioRingBuffer: a slot holds a *block*
/// and the engine's blocks are planar (`dsp::AudioBlockView`), so planar makes the
/// crossing one memcpy per channel with no transpose on either side.
/// AudioRingBuffer is interleaved because it carries a *stream* of frames to and
/// from disk, where the file format decides.
struct AudioSlotHeader {
    /// The slot's only synchronisation point. `frames`, `channels`, `origin` and the
    /// samples are written plainly before the publishing store and read plainly
    /// after the observing load, so they need no atomics of their own - the same
    /// discipline SpscQueue uses.
    alignas(kCacheLine) std::atomic<std::uint64_t> sequence{0};
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    /// The absolute block number this audio belongs to, as declared by the producer.
    ///
    /// It is NOT the slot's ring index, and the difference is the whole reason it
    /// exists: when the host drops a block because the helper fell behind, the two
    /// counters come apart, and a ring index alone would let the host believe the
    /// audio it just received is one block old when it is thirty. The helper copies
    /// the origin of the input block it processed, so the host can compute how stale
    /// a result is and count it instead of playing it as if it were current.
    std::uint64_t origin = 0;
};

/// Sequence values for a slot. `2b + 1` marks the slot mid-write, so a consumer
/// that arrives at that moment can tell "being written" from "written" instead of
/// inferring it from a ring counter that may already have moved on.
[[nodiscard]] constexpr std::uint64_t slotWritingSequence(std::uint64_t block) noexcept {
    return block * 2u + 1u;
}
[[nodiscard]] constexpr std::uint64_t slotStableSequence(std::uint64_t block) noexcept {
    return block * 2u + 2u;
}

/// Header of a record ring, followed in the arena by `capacity` records.
/// Monotonic counters rather than head/tail indices: with head/tail, "full" and
/// "empty" are only distinguishable by sacrificing a slot, whereas
/// `published - consumed == capacity` is unambiguous on its own.
struct PodRingHeader {
    alignas(kCacheLine) std::atomic<std::uint64_t> published{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> consumed{0};
};

/// Byte size of a record ring holding `capacity` records of `T`.
template <typename T>
[[nodiscard]] constexpr std::size_t podRingBytes(std::uint32_t capacity) noexcept {
    return sizeof(PodRingHeader) + static_cast<std::size_t>(capacity) * sizeof(T);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

/// The sizes both ends must agree on. Validated by computeLayout(), which rounds
/// record capacities up to a power of two and refuses anything out of range.
struct ArenaSpec {
    std::uint32_t maxBlockSize = 512;
    std::uint32_t maxChannels = 2;
    std::uint32_t audioSlots = kDefaultAudioSlots;
    std::uint32_t paramCapacity = kDefaultRecordCapacity;
    std::uint32_t noteCapacity = kDefaultRecordCapacity;

    [[nodiscard]] bool operator==(const ArenaSpec&) const noexcept = default;
};

/// Byte offset of each region from the arena base, plus the total size. Offsets,
/// never pointers - see rule 1 at the top of this file.
struct ArenaLayout {
    std::size_t totalBytes = 0;
    std::size_t control = 0;
    std::size_t telemetry = 0;
    std::size_t inputRing = 0;   ///< audio, host -> helper
    std::size_t outputRing = 0;  ///< audio, helper -> host
    std::size_t paramRing = 0;   ///< host -> helper
    std::size_t noteInRing = 0;  ///< host -> helper
    std::size_t noteOutRing = 0; ///< helper -> host (an arpeggiator feeding the session)
    /// Distance between the starts of two adjacent audio slots.
    std::size_t audioSlotStride = 0;

    /// Compared against a peer's header on attach(): a layout version that matches
    /// but offsets that differ means the header cannot be trusted, and trusting it
    /// would mean dereferencing an offset the peer chose.
    [[nodiscard]] bool operator==(const ArenaLayout&) const noexcept = default;
};

/// Written at offset 0 by the process that creates the mapping and checked by the
/// process that attaches to it.
struct ArenaHeader {
    std::uint32_t magic = kArenaMagic;
    std::uint32_t layoutVersion = kLayoutVersion;
    ArenaSpec spec{};
    ArenaLayout layout{};
};

/// Where a record ring lives: a base pointer (local to this process) and a
/// power-of-two capacity.
struct PodRingRegion {
    std::uint8_t* base = nullptr;
    std::uint32_t capacity = 0;
};

/// Where an audio slot ring lives. `base` addresses the PodRingHeader that carries
/// the ring's published/consumed counters; the slots follow it at
/// `sizeof(PodRingHeader)` and are `slotStride` apart.
struct AudioRingRegion {
    std::uint8_t* base = nullptr;
    std::uint32_t slots = 0;
    std::uint32_t maxChannels = 0;
    std::uint32_t maxBlockSize = 0;
    std::size_t slotStride = 0;
};

/// Validates a spec and returns the layout it implies. `specOut` receives the
/// normalised spec (capacities rounded up to a power of two), which is what must
/// be compared against a peer's header - comparing the caller's un-normalised
/// request would reject a peer that is in fact identical.
AuraResult<ArenaLayout> computeLayout(const ArenaSpec& spec, ArenaSpec& specOut);

/// Total bytes an arena of this spec needs. Returns 0 for an invalid spec; prefer
/// computeLayout() when the reason matters.
[[nodiscard]] std::size_t arenaBytes(const ArenaSpec& spec) noexcept;

/// A non-owning view of a prepared arena. Cheap to copy, holds no resource: the
/// bytes belong to whoever mapped them, and a view never frees them. Both the
/// host and the helper hold one over the same physical region.
class ArenaView {
public:
    ArenaView() = default;

    /// Takes ownership of freshly mapped bytes: zeroes them, writes the header and
    /// constructs every atomic. CONTROL THREAD ONLY, once, by the creating side.
    [[nodiscard]] static AuraResult<ArenaView> create(void* bytes, std::size_t size,
                                                      const ArenaSpec& spec);

    /// Validates a peer's header against `expected` and refuses to touch the bytes
    /// if they disagree. CONTROL THREAD ONLY.
    [[nodiscard]] static AuraResult<ArenaView> attach(void* bytes, std::size_t size,
                                                      const ArenaSpec& expected);

    [[nodiscard]] bool isValid() const noexcept { return bytes_ != nullptr; }
    [[nodiscard]] std::uint8_t* bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const ArenaSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] const ArenaLayout& layout() const noexcept { return layout_; }

    [[nodiscard]] ControlBlock* control() const noexcept;
    [[nodiscard]] Telemetry* telemetry() const noexcept;
    [[nodiscard]] AudioRingRegion inputRing() const noexcept;
    [[nodiscard]] AudioRingRegion outputRing() const noexcept;
    [[nodiscard]] PodRingRegion paramRing() const noexcept;
    [[nodiscard]] PodRingRegion noteInRing() const noexcept;
    [[nodiscard]] PodRingRegion noteOutRing() const noexcept;

    /// Audio slot payload capacity in floats per channel.
    [[nodiscard]] std::size_t slotChannelStride() const noexcept {
        return static_cast<std::size_t>(spec_.maxBlockSize);
    }

private:
    [[nodiscard]] AudioRingRegion audioRingAt(std::size_t offset) const noexcept;
    [[nodiscard]] PodRingRegion recordRingAt(std::size_t offset, std::uint32_t capacity) const noexcept;

    std::uint8_t* bytes_ = nullptr;
    std::size_t size_ = 0;
    ArenaSpec spec_{};
    ArenaLayout layout_{};
};

// ---------------------------------------------------------------------------
// Transport snapshot publication (seqlock)
// ---------------------------------------------------------------------------

/// Publishes `snapshot`. Called by the host's audio thread once per block: the
/// position moves every block, and a snapshot that is a block old makes a
/// tempo-synced plug-in audibly wrong. RT-safe - no allocation, no lock, and the
/// writer never waits for a reader.
void publishTransportSnapshot(ControlBlock* control, const TransportSnapshot& snapshot) noexcept;

/// Reads the snapshot with at most `maxRetries` attempts. Returns false when no
/// snapshot has been published yet (version 0 is "never published", not "stable and
/// zeroed", so a helper that starts first cannot act on a 0 Hz sample rate), when
/// the writer is mid-publish, or when the retries run out.
///
/// RT-safe and BOUNDED:
/// the audio thread may not spin on a peer that is mid-write, so a reader that
/// keeps seeing a torn version gives up and leaves `out` untouched, which lets the
/// caller carry on with its previous snapshot. Returns false in that case, and
/// `tornReads` is incremented so the event is visible instead of silent.
[[nodiscard]] bool readTransportSnapshot(const ControlBlock* control, TransportSnapshot& out,
                                         Telemetry* telemetry = nullptr,
                                         std::uint32_t maxRetries = 3) noexcept;

} // namespace aura::plugin::sandbox
