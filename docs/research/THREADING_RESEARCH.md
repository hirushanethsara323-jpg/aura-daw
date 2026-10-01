# THREADING_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0010 · **Code:** `aura::rt`, `aura::engine`, `tools/rt_audit.py`

## The rules, stated once

| Thread | Owns | Never does |
|--------|------|------------|
| **Audio** (WASAPI event / ASIO callback / mock) | the graph, transport advance, clip rendering, instruments, metering, recording write buffers | allocate, lock, open files, log, call the OS for anything but atomics |
| **Control/UI** | the project model, commands, undo, plugin scanning, device changes, file I/O | touch graph buffers that the audio thread reads |
| **Reader workers** (disk streams, waveform, media probe) | file I/O and de-interleaving into rings | touch the graph |
| **Scanner processes** | plug-in binaries | anything else |

Ownership is *documented per class* in the headers (`Control thread only.`,
`RT-safe.`, `Called by the audio thread.`) — that is a maintenance rule, not a
comment style.

## How the audio thread talks to the control thread

| Direction | Mechanism | Notes |
|-----------|-----------|-------|
| Control → audio | `EngineCommandQueue` (lock-free SPSC, 256 entries, POD commands) | drained at the top of every block, bounded work per block (32) |
| Control → audio (plan swap) | `std::atomic<GraphPlan*>` published with release, old plan kept alive for a grace window (2 blocks) | the audio thread only *loads* a pointer |
| Audio → control | atomics only: position, peak/LUFS meters, CPU load, underruns, `bucketsReady`, recording state | snapshots are read at UI rate, no locks |
| Audio → control (errors) | error counters + a small fixed-size record ring | never a heap-allocated log message from the callback |

## The traps, and what AURA does about them

1. **`std::stable_sort` allocates.** Found by `tools/rt_audit.py` on the MIDI path.
   Replaced with `core::stableSortSmall` (insertion sort — the sequences are a
   handful of events per block, and stability is required).
2. **"Active" is not "has a tail".** A processor that is *enabled* but silent kept
   its node alive; a stopped transport then replayed its last block forever.
   `Processor::hasTail()` splits the two questions, and the graph skips silent,
   tail-less nodes.
3. **A gate that is always open.** The delay's `isActive()` reported true whenever
   `mix > 0`, so nothing downstream could ever be skipped. Now the tail flag alone
   drives the skip decision.
4. **Publication vs. destruction.** A plan can only be freed once no block can be
   holding it. `pruneRetiredPlans()` keeps retired plans for two processed blocks.
5. **Locks that look cheap.** A `try_lock` that "practically never" blocks is still
   a priority-inversion risk. AURA has no mutex on any audio path at all.
6. **Denormals.** Not a race, but the same class of real-time hazard: recursive
   tails flush to zero (`math::flushDenormal`) rather than burning 100× the CPU.
7. **Block-size assumptions.** Every buffer is preallocated in `prepare(sampleRate,
   maxBlockSize, channels)`; the offline renderer clamps its block to the prepared
   size and refuses a sample-rate mismatch instead of overrunning anything.
8. **Message-from-UI during a block.** UI actions never write into the graph; they
   either post a command or write an atomic that the next block reads.

## Two independent enforcement mechanisms

1. **Static:** `tools/rt_audit.py` scans the functions that the audio thread can
   reach and rejects allocation, container growth, locks, file I/O, exceptions and
   logging. It is heuristic by design and says so; escape hatches require a written
   reason (`// rt-audit: allow - <why>`) and are printed in the audit output, so an
   escape is visible rather than silent. Registered as the ctest
   `aura.static.realtime_audit`, so CI fails on a regression.
2. **Dynamic:** `tests/engine/EngineTests.cpp` arms the allocation tracker
   (`rt::setTrackingEnabled`, `rt::resetRealtimeAllocationCount`) and renders a
   block through the real engine, asserting `realtimeAllocationCount() == 0`. A
   second test runs the `MockAudioDevice` on its own thread for real, so the
   production callback path is exercised, not just a direct call.

## Priorities and MMCSS

* The audio thread requests the MMCSS "Pro Audio" task on Windows; the reader
  threads stay at normal priority (they must never outrank the callback), and the
  UI thread is left to the compositor's scheduling.
* Cancellation is cooperative everywhere: streams and scans check an atomic quit
  flag and never block on shutdown.

## Open questions

* Should the reader threads be given a *lower* priority class explicitly, rather
  than relying on default? (Measure first on a modest machine with a big session.)
* Whether the command queue needs to carry variable payloads (currently POD only,
  by design — variable payloads would reintroduce allocation).
* Whether to adopt `std::atomic<std::shared_ptr>` (C++20) for plan publication
  instead of the raw pointer + grace window once compiler support is uniform.

## Sources

* Ross Bencina, *Real-time audio programming 101* — the canonical statement of the
  no-alloc/no-lock/no-I/O rules and why they are about worst-case time, not average.
* Microsoft, *AvSetMmThreadCharacteristics* / *MMCSS* — "Pro Audio" task and
  thread priority behaviour on Windows.
* Fabian Renn-Giles & Dave Rowland, *Real-time 101* (JUCE ADC talks) — practical
  lock-free patterns for audio threads, including why `try_lock` is a trap.
* AURA's own findings this milestone: the `stable_sort`, `hasTail`, plan-lifetime
  and stale-block bugs, all now fixed with regression tests.
