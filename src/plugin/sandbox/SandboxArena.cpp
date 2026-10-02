// ============================================================================
// AURA DAW - src/plugin/sandbox/SandboxArena.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Arena layout, validation and the transport-snapshot seqlock. See the header for
// the three rules that make the layout safe across a process boundary.
// ============================================================================
#include "aura/plugin/sandbox/SandboxArena.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>

#include "aura/core/Math.hpp"
#include "aura/core/Strings.hpp"

namespace aura::plugin::sandbox {

namespace {

/// Upper bounds on the spec. They exist so a corrupt or hostile header cannot ask
/// for an arena larger than the mapping - the size check below would catch that
/// too, but a named limit gives a readable error instead of an arithmetic one.
constexpr std::uint32_t kMaxBlockSize = 8192u;
constexpr std::uint32_t kMaxChannels = 64u;

[[nodiscard]] std::size_t alignUp(std::size_t value) noexcept {
    return (value + kCacheLine - 1u) & ~(kCacheLine - 1u);
}

/// Header, then `maxChannels` planes of `maxBlockSize` floats, rounded up so the
/// next slot's sequence counter starts on a cache line of its own.
[[nodiscard]] std::size_t audioSlotStride(const ArenaSpec& spec) noexcept {
    const std::size_t payload = static_cast<std::size_t>(spec.maxChannels) *
                                static_cast<std::size_t>(spec.maxBlockSize) * sizeof(float);
    return alignUp(sizeof(AudioSlotHeader) + payload);
}

[[nodiscard]] std::uint32_t normaliseCapacity(std::uint32_t requested) noexcept {
    const std::uint64_t clamped = math::clamp<std::uint64_t>(requested, kMinRecordCapacity,
                                                             kMaxRecordCapacity);
    return static_cast<std::uint32_t>(math::nextPowerOfTwo(clamped));
}

/// Layout error with the numbers in it: "the arena needs N bytes" is actionable,
/// "invalid argument" is not.
[[nodiscard]] AuraError layoutError(const char* what, std::uint64_t value, std::uint64_t limit) {
    return makeError(ErrorCode::InvalidArgument, what,
                     "value " + std::to_string(value) + ", limit " + std::to_string(limit));
}

/// The base must be cache-line aligned: every counter in here is alignas(64) so
/// that two processes never share a line, and an atomic placed at an under-aligned
/// address is undefined rather than merely slower. A real mapping satisfies this
/// for free (mmap and VirtualAlloc both hand back page-aligned memory); a caller
/// passing a heap buffer has to align it, and being told beats being quietly UB.
[[nodiscard]] AuraError alignmentError(const void* bytes) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(bytes);
    return makeError(ErrorCode::InvalidArgument,
                     "an arena's base address must be cache-line aligned",
                     "address ends 0x" + strings::toHex(static_cast<std::uint32_t>(address), 8) +
                         ", need a multiple of " + std::to_string(kCacheLine));
}

[[nodiscard]] bool isAlignedBase(const void* bytes) noexcept {
    return (reinterpret_cast<std::uintptr_t>(bytes) % kCacheLine) == 0u;
}


// ---------------------------------------------------------------------------
// The transport snapshot crosses word by word, through relaxed atomics.
//
// It travels inside a seqlock: the writer bumps a version word to odd, writes the
// payload, bumps it to even; the reader brackets a copy with two version reads and
// rejects the copy when they differ. The version word is `memory_order_seq_cst` -
// see ADR-0017 for why release/acquire is not enough - and that ordering is what
// makes a copy taken between two equal versions a consistent one.
//
// Copying the payload with `=` is not part of that argument, and was the bug. A
// plain struct assignment racing with a plain struct write is a data race, and a
// data race is undefined behaviour - which is a licence, not a formality. MSVC at
// /O2 took it: it common-subexpression-eliminated the reader's copy across the
// retry loop, so a payload torn on attempt 0 could be reused on attempt 1 and
// accepted under a version that matched. CI's Windows Release job found exactly one
// such read in 112 026, on a transport that had passed every Linux configuration
// and every MSVC Debug build before it. Relaxed atomics remove the race and
// therefore the licence.
//
// Relaxed is the correct order precisely because the ordering lives in the version
// word: acquire or release here would add barriers that buy nothing. And word-wise
// is real-time safe where `std::atomic_ref<TransportSnapshot>` would not be - a
// 48-byte atomic_ref is not lock-free, so it would put a mutex on the audio path to
// fix a race. Six 8-byte words are lock-free on every platform AURA builds for.
//
// The snapshot has to be a whole number of words for this to be honest, so that is
// asserted rather than assumed: a struct that grew a trailing uint32_t would
// silently leave four bytes outside the copy, and the seqlock would keep reporting
// the result as consistent.
constexpr std::size_t kSnapshotWords = sizeof(TransportSnapshot) / sizeof(std::uint64_t);

static_assert(sizeof(TransportSnapshot) % sizeof(std::uint64_t) == 0,
              "the snapshot is copied word by word, so it must be a whole number of words");
static_assert(alignof(TransportSnapshot) % alignof(std::uint64_t) == 0,
              "std::atomic_ref requires its object to be suitably aligned");
static_assert(std::is_trivially_copyable_v<TransportSnapshot>,
              "copied as raw words, so it cannot own anything");
static_assert(std::is_standard_layout_v<ControlBlock>, "offsetof below is only defined for it");
static_assert(offsetof(ControlBlock, transport) % alignof(std::uint64_t) == 0,
              "the snapshot must start on a word boundary to be copied through atomic_ref");

/// RT-safe. Reads a snapshot that a peer may be writing.
///
/// Named `copySnapshotFrom` and not `loadSnapshotWords`: `^load.*` is in
/// rt_audit.py's CONTROL_THREAD_FUNCTIONS, so a name starting with "load" is
/// exempted from the audit by prefix, and this function would have been scanned by
/// nothing at all while looking like it was covered. The audit now reports that
/// contradiction as an error rather than letting a prefix decide silently.
[[nodiscard]] TransportSnapshot copySnapshotFrom(const TransportSnapshot& source) noexcept {
    std::uint64_t words[kSnapshotWords];
    const auto* base = reinterpret_cast<const std::uint64_t*>(&source);
    for (std::size_t i = 0; i < kSnapshotWords; ++i)
        words[i] = std::atomic_ref<const std::uint64_t>(base[i]).load(std::memory_order_relaxed);
    TransportSnapshot out{};
    std::memcpy(&out, words, sizeof(TransportSnapshot));
    return out;
}

/// RT-safe. Writes a snapshot under the seqlock's odd version.
void copySnapshotTo(TransportSnapshot& destination, const TransportSnapshot& source) noexcept {
    std::uint64_t words[kSnapshotWords];
    std::memcpy(words, &source, sizeof(TransportSnapshot));
    auto* base = reinterpret_cast<std::uint64_t*>(&destination);
    for (std::size_t i = 0; i < kSnapshotWords; ++i)
        std::atomic_ref<std::uint64_t>(base[i]).store(words[i], std::memory_order_relaxed);
}

} // namespace

// ---------------------------------------------------------------------------
// MIDI wire conversion
// ---------------------------------------------------------------------------

bool toMidiRecord(const midi::Message& message, MidiRecord& out) noexcept {
    // Meta events live inside MIDI clips (tempo, markers, text) and are never
    // routed to a plug-in, so there is no honest wire encoding for one. Refusing
    // beats inventing a meaning the peer would have to guess at.
    if (message.type == midi::Message::Type::Meta)
        return false;
    out.type = static_cast<std::uint8_t>(message.type);
    out.channel = message.channel;
    out.data1 = message.data1;
    out.data2 = message.data2;
    // A block offset outside the block is a bug in the caller, not something to
    // transport; clamp rather than refuse so one bad event cannot mute the block.
    out.sampleOffset = static_cast<std::int32_t>(math::clamp<std::int64_t>(message.sampleOffset, 0,
                                                                          std::int64_t{1} << 30));
    return true;
}

bool toMidiMessage(const MidiRecord& record, midi::Message& out) noexcept {
    using Type = midi::Message::Type;
    // The type byte arrives from another process, so it is validated rather than
    // cast: an out-of-range enumerator would put every downstream switch on a
    // value nobody wrote a case for. Meta is rejected for the same reason
    // toMidiRecord refuses to produce one.
    if (record.type > static_cast<std::uint8_t>(Type::Meta) ||
        record.type == static_cast<std::uint8_t>(Type::Meta))
        return false;
    out.type = static_cast<Type>(record.type);
    out.channel = record.channel;
    out.data1 = record.data1;
    out.data2 = record.data2;
    out.metaType = 0;
    out.timestampTicks = 0; // the helper works in samples; ticks are a host concern
    out.sampleOffset = record.sampleOffset;
    return true;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

AuraResult<ArenaLayout> computeLayout(const ArenaSpec& spec, ArenaSpec& specOut) {
    if (spec.maxBlockSize == 0u || spec.maxBlockSize > kMaxBlockSize)
        return failureT<ArenaLayout>(layoutError("maxBlockSize is out of range", spec.maxBlockSize,
                                                 kMaxBlockSize));
    if (spec.maxChannels == 0u || spec.maxChannels > kMaxChannels)
        return failureT<ArenaLayout>(layoutError("maxChannels is out of range", spec.maxChannels,
                                                 kMaxChannels));
    if (spec.audioSlots < kMinAudioSlots || spec.audioSlots > kMaxAudioSlots)
        return failureT<ArenaLayout>(layoutError("audioSlots is out of range", spec.audioSlots,
                                                 kMaxAudioSlots));

    ArenaSpec normalised = spec;
    normalised.paramCapacity = normaliseCapacity(spec.paramCapacity);
    normalised.noteCapacity = normaliseCapacity(spec.noteCapacity);

    const std::size_t stride = audioSlotStride(normalised);
    // Each audio ring starts with the same PodRingHeader a record ring uses (its
    // published/consumed counters), followed by the slots. sizeof(PodRingHeader) is
    // already a multiple of the cache line, so the first slot starts aligned.
    const std::size_t audioRingBytes =
        alignUp(sizeof(PodRingHeader)) + stride * static_cast<std::size_t>(normalised.audioSlots);

    ArenaLayout layout{};
    std::size_t offset = alignUp(sizeof(ArenaHeader));

    layout.control = offset;
    offset = alignUp(offset + sizeof(ControlBlock));
    layout.telemetry = offset;
    offset = alignUp(offset + sizeof(Telemetry));
    layout.inputRing = offset;
    offset = alignUp(offset + audioRingBytes);
    layout.outputRing = offset;
    offset = alignUp(offset + audioRingBytes);
    layout.paramRing = offset;
    offset = alignUp(offset + podRingBytes<ParamRecord>(normalised.paramCapacity));
    layout.noteInRing = offset;
    offset = alignUp(offset + podRingBytes<MidiRecord>(normalised.noteCapacity));
    layout.noteOutRing = offset;
    offset = alignUp(offset + podRingBytes<MidiRecord>(normalised.noteCapacity));

    layout.audioSlotStride = stride;
    layout.totalBytes = offset;

    specOut = normalised;
    return layout;
}

std::size_t arenaBytes(const ArenaSpec& spec) noexcept {
    ArenaSpec ignored{};
    const auto layout = computeLayout(spec, ignored);
    return layout.hasValue() ? layout.value().totalBytes : 0u;
}

// ---------------------------------------------------------------------------
// ArenaView
// ---------------------------------------------------------------------------

AuraResult<ArenaView> ArenaView::create(void* bytes, std::size_t size, const ArenaSpec& spec) {
    if (bytes == nullptr)
        return failureT<ArenaView>(
            makeError(ErrorCode::InvalidArgument, "an arena needs mapped bytes to be created in"));
    if (!isAlignedBase(bytes))
        return failureT<ArenaView>(alignmentError(bytes));

    ArenaSpec normalised{};
    auto layoutResult = computeLayout(spec, normalised);
    if (layoutResult.hasError())
        return failureT<ArenaView>(layoutResult.error());
    const ArenaLayout layout = layoutResult.value();

    if (size < layout.totalBytes)
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument, "the mapping is smaller than the arena needs",
            "have " + std::to_string(size) + " bytes, need " + std::to_string(layout.totalBytes)));

    auto* base = static_cast<std::uint8_t*>(bytes);
    // Zeroed first: the audio payload must start silent and every counter must
    // start at zero, and neither is something a constructor below would do for the
    // bytes that are not part of a struct.
    std::memset(base, 0, layout.totalBytes);

    ::new (static_cast<void*>(base)) ArenaHeader{};
    auto* header = reinterpret_cast<ArenaHeader*>(base);
    header->magic = kArenaMagic;
    header->layoutVersion = kLayoutVersion;
    header->spec = normalised;
    header->layout = layout;

    // The atomics are constructed rather than merely zeroed. On every target
    // compiler a lock-free atomic's representation is the underlying integer, so
    // the memset already produced the right bytes - but "already the right bytes"
    // is an implementation detail, and placement new is the thing the standard
    // actually promises.
    ::new (static_cast<void*>(base + layout.control)) ControlBlock();
    ::new (static_cast<void*>(base + layout.telemetry)) Telemetry();
    // Each audio ring begins with the counters that pace it, then the slots.
    ::new (static_cast<void*>(base + layout.inputRing)) PodRingHeader();
    ::new (static_cast<void*>(base + layout.outputRing)) PodRingHeader();
    for (std::uint32_t slot = 0; slot < normalised.audioSlots; ++slot) {
        const std::size_t at = sizeof(PodRingHeader) +
                               static_cast<std::size_t>(slot) * layout.audioSlotStride;
        ::new (static_cast<void*>(base + layout.inputRing + at)) AudioSlotHeader();
        ::new (static_cast<void*>(base + layout.outputRing + at)) AudioSlotHeader();
    }
    ::new (static_cast<void*>(base + layout.paramRing)) PodRingHeader();
    ::new (static_cast<void*>(base + layout.noteInRing)) PodRingHeader();
    ::new (static_cast<void*>(base + layout.noteOutRing)) PodRingHeader();

    ArenaView view;
    view.bytes_ = base;
    view.size_ = size;
    view.spec_ = normalised;
    view.layout_ = layout;
    return view;
}

AuraResult<ArenaView> ArenaView::attach(void* bytes, std::size_t size) {
    // Only the checks needed to READ the header safely are done here; the 3-argument
    // overload repeats them and then does the real work. Repeating three comparisons
    // is cheaper than splitting the validation in two, and it keeps one code path
    // that can disagree with itself about what a valid header is.
    if (bytes == nullptr)
        return failureT<ArenaView>(
            makeError(ErrorCode::InvalidArgument, "an arena needs mapped bytes to be attached to"));
    if (size < sizeof(ArenaHeader))
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument, "the mapping is too small to hold an arena header",
            "have " + std::to_string(size) + " bytes, need " + std::to_string(sizeof(ArenaHeader))));
    if (!isAlignedBase(bytes))
        return failureT<ArenaView>(alignmentError(bytes));

    const auto* header = reinterpret_cast<const ArenaHeader*>(bytes);
    if (header->magic != kArenaMagic)
        return failureT<ArenaView>(makeError(
            ErrorCode::UnsupportedFormat, "the shared region is not an AURA sandbox arena",
            "magic 0x" + strings::toHex(header->magic, 8)));
    if (header->layoutVersion != kLayoutVersion)
        return failureT<ArenaView>(makeError(
            ErrorCode::UnsupportedVersion,
            "the peer's arena layout is a different version than this build's",
            "peer " + std::to_string(header->layoutVersion) + ", this build " +
                std::to_string(kLayoutVersion)));

    // From here the header describes an arena this build understands, so the spec it
    // declares is worth validating - and the overload below proves it against the
    // offsets actually present.
    return attach(bytes, size, header->spec);
}

AuraResult<ArenaView> ArenaView::attach(void* bytes, std::size_t size, const ArenaSpec& expected) {
    if (bytes == nullptr)
        return failureT<ArenaView>(
            makeError(ErrorCode::InvalidArgument, "an arena needs mapped bytes to be attached to"));
    if (size < sizeof(ArenaHeader))
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument, "the mapping is too small to hold an arena header",
            "have " + std::to_string(size) + " bytes, need " + std::to_string(sizeof(ArenaHeader))));
    if (!isAlignedBase(bytes))
        return failureT<ArenaView>(alignmentError(bytes));

    auto* base = static_cast<std::uint8_t*>(bytes);
    const auto* header = reinterpret_cast<const ArenaHeader*>(base);

    // Everything below is a refusal, not a correction: a peer whose header we do
    // not understand must not be interpreted, because the first thing we would do
    // with it is dereference an offset it chose.
    if (header->magic != kArenaMagic)
        return failureT<ArenaView>(makeError(
            ErrorCode::UnsupportedFormat, "the shared region is not an AURA sandbox arena",
            "magic 0x" + strings::toHex(header->magic, 8)));
    if (header->layoutVersion != kLayoutVersion)
        return failureT<ArenaView>(makeError(
            ErrorCode::UnsupportedVersion,
            "the peer's arena layout is a different version than this build's",
            "peer " + std::to_string(header->layoutVersion) + ", this build " +
                std::to_string(kLayoutVersion)));

    ArenaSpec normalised{};
    auto layoutResult = computeLayout(expected, normalised);
    if (layoutResult.hasError())
        return failureT<ArenaView>(layoutResult.error());

    if (!(header->spec == normalised))
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument, "the peer's arena is sized for a different session",
            "peer block=" + std::to_string(header->spec.maxBlockSize) +
                " ch=" + std::to_string(header->spec.maxChannels) +
                ", expected block=" + std::to_string(normalised.maxBlockSize) +
                " ch=" + std::to_string(normalised.maxChannels)));

    const ArenaLayout layout = layoutResult.value();
    if (!(header->layout == layout))
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument,
            "the peer's region offsets do not match this build's layout",
            "a matching version with different offsets means the header cannot be trusted"));

    if (size < layout.totalBytes)
        return failureT<ArenaView>(makeError(
            ErrorCode::InvalidArgument, "the mapping is smaller than the arena header claims",
            "have " + std::to_string(size) + " bytes, need " + std::to_string(layout.totalBytes)));

    ArenaView view;
    view.bytes_ = base;
    view.size_ = size;
    view.spec_ = normalised;
    view.layout_ = layout;
    return view;
}

ControlBlock* ArenaView::control() const noexcept {
    return bytes_ == nullptr ? nullptr : reinterpret_cast<ControlBlock*>(bytes_ + layout_.control);
}

Telemetry* ArenaView::telemetry() const noexcept {
    return bytes_ == nullptr ? nullptr : reinterpret_cast<Telemetry*>(bytes_ + layout_.telemetry);
}

AudioRingRegion ArenaView::audioRingAt(std::size_t offset) const noexcept {
    AudioRingRegion region{};
    if (bytes_ == nullptr)
        return region;
    region.base = bytes_ + offset;
    region.slots = spec_.audioSlots;
    region.maxChannels = spec_.maxChannels;
    region.maxBlockSize = spec_.maxBlockSize;
    region.slotStride = layout_.audioSlotStride;
    return region;
}

AudioRingRegion ArenaView::inputRing() const noexcept { return audioRingAt(layout_.inputRing); }
AudioRingRegion ArenaView::outputRing() const noexcept { return audioRingAt(layout_.outputRing); }

PodRingRegion ArenaView::recordRingAt(std::size_t offset, std::uint32_t capacity) const noexcept {
    PodRingRegion region{};
    if (bytes_ == nullptr)
        return region;
    region.base = bytes_ + offset;
    region.capacity = capacity;
    return region;
}

PodRingRegion ArenaView::paramRing() const noexcept {
    return recordRingAt(layout_.paramRing, spec_.paramCapacity);
}
PodRingRegion ArenaView::noteInRing() const noexcept {
    return recordRingAt(layout_.noteInRing, spec_.noteCapacity);
}
PodRingRegion ArenaView::noteOutRing() const noexcept {
    return recordRingAt(layout_.noteOutRing, spec_.noteCapacity);
}

// ---------------------------------------------------------------------------
// Transport snapshot
// ---------------------------------------------------------------------------
//
// Why seq_cst on the version word and plain release/acquire everywhere else in
// this file: the rings publish with a release store and observe with an acquire
// load, which is exactly the SPSC pattern SpscQueue uses and is sufficient when
// the only question is "has this block been published?". The snapshot asks a
// second question - "is the writer in here right now?" - and answering it needs
// the odd-version store to be observable *before* the data stores that follow it.
// The C++ memory model permits a StoreStore reorder there even though every
// architecture AURA ships on (x86-64) does not perform one, so the version word is
// seq_cst: correct under the standard rather than under an ISA. The cost is two
// locked stores per block on the writer and nothing extra on the reader, against a
// block budget of at least 1.3 ms - well under a thousandth of it.

void publishTransportSnapshot(ControlBlock* control, const TransportSnapshot& snapshot) noexcept {
    if (control == nullptr)
        return;
    const std::uint32_t version = control->transportVersion.load(std::memory_order_relaxed);
    control->transportVersion.store(version + 1u, std::memory_order_seq_cst); // odd: writing
    copySnapshotTo(control->transport, snapshot);
    control->transportVersion.store(version + 2u, std::memory_order_seq_cst); // even: stable
}

bool readTransportSnapshot(const ControlBlock* control, TransportSnapshot& out, Telemetry* telemetry,
                           std::uint32_t maxRetries) noexcept {
    if (control == nullptr)
        return false;

    // Bounded on purpose. The reader is an RT thread: spinning until a peer
    // finishes writing is how a mixer misses its deadline, so a reader that keeps
    // seeing a torn version gives up and the caller carries on with the snapshot it
    // already had. A transport position one block old is inaudible; a stalled
    // audio thread is not.
    for (std::uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
        const std::uint32_t before = control->transportVersion.load(std::memory_order_seq_cst);
        // Version 0 is "never published", not "stable and zeroed": a helper that
        // started before the host's first block would otherwise read a snapshot
        // claiming a 0 Hz sample rate and a 0-frame block, and act on it.
        if (before == 0u)
            break;
        if ((before & 1u) != 0u)
            continue; // mid-write
        const TransportSnapshot candidate = copySnapshotFrom(control->transport);
        const std::uint32_t after = control->transportVersion.load(std::memory_order_seq_cst);
        if (after == before) {
            out = candidate;
            return true;
        }
    }

    if (telemetry != nullptr)
        telemetry->tornReads.fetch_add(1u, std::memory_order_relaxed);
    return false;
}

} // namespace aura::plugin::sandbox
