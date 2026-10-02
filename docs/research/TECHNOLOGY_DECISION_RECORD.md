# TECHNOLOGY_DECISION_RECORD.md

**Status:** Accepted · **Last revised:** 2026-10-01

The single place where AURA's technology decisions are recorded. Each entry says
what was decided, what was rejected, and why. The detailed reasoning and sources
live in the per-topic documents in this folder; this file is the index of record.

---

## ADR-0001 — Language and standard: C++20

* **Accepted.** C++20 on MSVC (Visual Studio 2022, `/std:c++20`), GCC 13+/Clang 16+
  for development and CI on Linux.
* **Why:** concepts, `<bit>`, `std::atomic` improvements, designated initialisers,
  `constexpr` containers and three-way comparison all earn their keep in a
  real-time engine; C++23 adds little we need and reduces compiler support.
* **Consequences:** no `std::expected` (C++23) — AURA has its own `AuraResult<T>`
  in `include/aura/core/Errors.hpp`. Modules are **not** used: MSVC module support
  is still not dependable for a project this size, and CMake's handling of them
  would add risk for no gain.

## ADR-0002 — Build system: CMake ≥ 3.24 with presets

* **Accepted.** Single top-level `CMakeLists.txt`, explicit source lists (no
  globbing), `CMakePresets.json` for Debug/Release/RelWithDebInfo.
* **Why:** the de-facto standard for C++ projects, first-class MSVC generator
  support, and `FetchContent` covers the small number of dependencies. Ninja is the
  preferred generator on Windows (`-G "Visual Studio 17 2022"` also supported).
* **Consequences:** adding a source file requires editing a list — deliberate, it
  keeps the build deterministic and greppable. See `BUILD_SYSTEM_RESEARCH.md`.

## ADR-0003 — Project licence: GPL-3.0-or-later

* **Accepted.** Every file carries `SPDX-License-Identifier: GPL-3.0-or-later` and
  the project copyright line.
* **Why:** AURA wants to stay open — including the engine — while remaining
  compatible with the licences of the tools it links (ASIO's GPLv3 branch, and the
  MIT/BSL dependencies which are GPL-compatible). It also keeps the project out of
  the business of relicensing third-party code.
* **Consequences:** see ADR-0005/0006 for JUCE and ASIO. Any dependency that is not
  GPL-compatible cannot be linked, however convenient. The licence audit lives in
  `../LICENSES.md`.

## ADR-0004 — Test framework: Catch2 v3 (BSL-1.0)

* **Accepted.** Catch2 v3.7.1 via `FetchContent`, one test binary per area plus a
  shared support library.
* **Rejected:** GoogleTest (slower to compile, heavier, GPL-unfriendly only in the
  sense of bulk), doctest (faster, but Catch2's matchers, sections and generators
  are worth more to us than the compile-time difference at this size — the
  measured gap between doctest and Catch2-as-a-static-library is small).
* **Consequences:** requirement of a network fetch on first configure, with a
  documented offline path (`AURA_FETCH_DEPS=OFF` + a local Catch2 checkout).

## ADR-0005 — Audio device layer: first-party WASAPI + ASIO backends

* **Accepted.** `IAudioDevice` abstraction with WASAPI shared, WASAPI exclusive,
  ASIO and a mock backend. No third-party audio I/O dependency (no RtAudio, no
  PortAudio, no JUCE device layer).
* **Why:** the requirement is Windows-only output with per-device capability
  queries, exclusive-mode format negotiation, hot-plug and sample-rate-change
  recovery — exactly the places where a wrapper library hides the information we
  need. It also keeps the licence surface small.
* **Consequences:** we own the Win32 COM code and its failure modes (documented in
  `AUDIO_BACKEND_RESEARCH.md`); ASIO is opt-in (`AURA_ENABLE_ASIO=OFF` by default)
  because its SDK is not vendored.

## ADR-0006 — UI framework: Win32 + Direct2D first-party shell; JUCE optional, off by default

* **Provisional (M9/M10 decides the final shape).**
* **Why:** JUCE is dual-licensed AGPLv3/commercial. AGPLv3 is *stronger* than our
  GPL-3.0-or-later: GPLv3 §13 permits linking the two, but the combined work then
  carries the AGPL network clause, and the project could no longer describe itself
  as "GPL-3.0-or-later" alone. Combined with a 117 MB dependency and the
  requirement for an original AURA visual identity, a first-party Win32 +
  Direct2D/DirectWrite shell is the better default. JUCE remains available as a
  **build option** for the plugin-editor hosting adapter if that proves necessary.
* **Rejected:** Qt (LGPL + commercial, large runtime, licence churn), Dear ImGui
  (immediate mode redraws the whole window every frame — wrong for a DAW that must
  stay cool on modest PCs, and skinning it into an original identity is hard).
* **Consequences:** the shell and widget toolkit are ours to write and test; the
  engine must stay framework-free (it already is). See `UI_FRAMEWORK_RESEARCH.md`.

## ADR-0007 — Plugin formats: VST3 first, CLAP optional; MIT-safe path

* **Accepted.** VST3 (SDK MIT since 3.8, Oct 2025) is the primary format; CLAP
  (MIT) is the secondary. Hosting is behind `plugin::PluginHost` so the SDK is an
  optional build input, never vendored.
* **Why:** VST3 has the ecosystem and now has a permissive licence; CLAP is
  technically cleaner (C ABI, more metadata, thread-safe by design) but has a
  smaller installed base.
* **Consequences:** scanning and out-of-process isolation are designed for, not
  yet built; the limitation is documented rather than hidden. See
  `PLUGIN_FORMAT_RESEARCH.md`.

## ADR-0008 — Project format: a directory with JSON manifest and sidecar caches

* **Accepted.** `<name>.aura/` with `project.json`, `media/`, `peaks/`, `cache/`,
  `backups/`, and an atomic write + `.bak` rotation.
* **Why:** media stays where the user put it, the manifest is diffable and
  recoverable by hand, and cache directories can be deleted without data loss.
  A single opaque blob would be easier to write and much worse to live with.
* **Consequences:** path handling (relative vs absolute, relinking) becomes a real
  feature with real edge cases. See `PROJECT_FORMAT_RESEARCH.md`.

## ADR-0009 — Internal processing format: 32-bit float, planar, non-interleaved

* **Accepted.** Recording is 24-bit PCM WAV; everything inside the engine is
  `float` planar at the device sample rate.
* **Why:** float has the headroom for gain staging and the denormal handling is
  well understood; planar is what the DSP kernels and the graph want. Conversion
  happens once, at the file boundary (`aura::media`).
* **Consequences:** no internal sample-rate conversion yet — a mismatch is refused
  with an explicit error instead of silently resampling (documented limitation).

## ADR-0010 — Real-time rules are static-checked *and* unit-tested

* **Accepted.** Allocation, locks, file I/O and logging are banned on the audio
  thread; `tools/rt_audit.py` enforces this statically (run as the ctest
  `aura.static.realtime_audit`), and `tests/engine/EngineTests.cpp` renders a block
  with the allocation counter armed.
* **Why:** a rule that is only written down gets broken. Two independent
  mechanisms — one static, one dynamic — turn "we are careful" into a build gate.
* **Consequences:** the static audit is heuristic (it says so), so findings can be
  escaped with a justified `// rt-audit: allow - <reason>` annotation, and the two
  documented escape hatches are visible in the audit output. See
  `THREADING_RESEARCH.md`.

## ADR-0011 — Dependencies: minimal, documented, vendored by fetch not by copy

* **Accepted.** Production dependencies: the C++ standard library and the Windows
  SDK. Development-only: Catch2 v3 (BSL-1.0). Optional: VST3 SDK (MIT), CLAP (MIT),
  ASIO SDK (GPLv3 / commercial, never vendored), JUCE (AGPLv3 / commercial, off by
  default).
* **Why:** every dependency is a licence obligation, an upgrade chore and a build
  risk; the engine needs remarkably few.
* **Consequences:** anything new must be justified in `../LICENSES.md` (purpose,
  version, licence, maintenance status, build and runtime impact) before it lands.

## ADR-0012 — Versioning, branching, releases

* **Accepted.** Semantic version in `include/aura/core/Version.hpp`; milestones
  M0–M12 as GitHub milestones; feature branches, no force-push to `main`, tests
  green before merge.
* **Why:** the master plan asks for a professional history, and a version number
  that other code can read.
* **Consequences:** the project format version (`AURA_PROJECT_FORMAT_VERSION`) is
  versioned separately from the application, and opening a newer project in an
  older build is refused with a clear message rather than half-loaded.

## ADR-0013 — One warning contract for both toolchains

* **Accepted.** The MSVC and GCC flag sets are kept aligned, warnings are errors in
  CI (`/WX`, `-Werror`), and the only suppression in the tree is a scoped
  `#pragma warning(disable : 4324)` around cache-line-padded lock-free structures.
  The inventory, kept here because the rule requires an entry per suppression:
  `SpscQueue` and `AudioRingBuffer` (`core/`), then `PodRingHeader` and
  `ControlBlock` (`plugin/sandbox/SandboxArena.hpp`) and `AudioSlotHeader`
  (`plugin/sandbox/SharedAudioRing.hpp`) — five sites, all `pragma push`/`pop` pairs
  with the reason in the comment above them. The sandbox ones are non-negotiable in a
  way the core ones merely are: that layout is shared between two processes and every
  offset is part of a contract as fixed as a file format, so the padding is *stated*
  rather than designed around.
* **Why:** the first CI run was green on the "Repo hygiene" job and red on every
  build job — with failures that could not appear locally: GCC 13's `-Wshadow`
  rejecting `using Array = …` next to a `Type::Array` enumerator (GCC 14 accepts
  it), MSVC C4324 padding, and `GetCurrentThreadId` without `<windows.h>`. Two
  hours of CI were spent proving that "builds on my machine" is not evidence.
* **Research:** Microsoft's own pages define the levels we now design for — C4324
  is level 4 and fires on any `alignas`/`__declspec(align)` that pads a class
  ([C4324](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4324)),
  C4127 does *not* fire for trivial literals such as `while (false)` since VS2015
  update 3, which keeps the `do { … } while (false)` macro idiom safe
  ([C4127](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4127)),
  and C4100 (unreferenced formal parameter) is reported at `/W4`, so
  `-Wunused-parameter` stays **on** for GCC — a warning Linux forgives is a build
  break on Windows.
* **Rejected:** a global `/wd4324` or `/wd4100` in `CMakeLists.txt` (hides the next
  real instance in unrelated code), lowering `/W4`, and turning warnings-as-errors
  off in CI. Also rejected: naming locals after members and silencing `-Wshadow`
  instead of renaming the local.
* **Consequences:** a new suppression must be a scoped `pragma push/pop` with a
  comment saying why the warning is expected, plus an entry here. The rule has a
  second, mechanical half: `tools/include_audit.py` (ctest
  `aura.static.include_audit`) fails on a file that uses a standard type without
  including its header, because libstdc++ supplies `std::mutex`, `std::thread`,
  `std::chrono` and others transitively and MSVC's STL does not — the exact way the
  *second* Windows CI run failed after the first fix landed. Both toolchains
  are in the matrix, so an asymmetry is caught by the first push rather than by a
  user. `docs/DEVELOPMENT.md` documents the contract for contributors.

## ADR-0014 — Windows-only code is cross-checked on Linux, not trusted

* **Accepted.** `tools/win_crosscheck.sh` compiles every `src/**/*.cpp` and every
  public header standalone with a MinGW-w64 cross compiler (`-fsyntax-only`, the
  same warning set as the native build). It runs as the CI job "Windows
  cross-check" (Ubuntu, ~1 minute) and is documented for local use in
  `../BUILDING.md`.
* **Why:** the third CI run failed on `src/audioio/WasapiDevice.cpp` for three
  separate reasons that a Linux build can never show, because GCC never compiles
  that file's Windows branch: the WASAPI/WRL headers were included *inside*
  `namespace aura::audioio` (so MSVC reported `C2653: 'Microsoft': is not a class
  or namespace name` and `C2786: invalid operand for __uuidof`), `ComPtr` was
  aliased from a class template without arguments (invalid everywhere — the file
  had simply never been compiled), and `EnumAudioEndpoints(eRender | eCapture, …)`
  passed an `int` where `EDataFlow` was expected *and* meant "all endpoints" while
  the value it produced (1) means "capture only".
* **Rejected:** relying on the MSVC job alone (10–20 minutes per discovery, three
  findings per run because the compiler stops at the first file); making MinGW a
  supported build target (it is a check, not a product); vendoring the Windows SDK
  headers; adding a Windows-targeting LLVM toolchain (no MSVC headers on Linux).
* **Consequences:** MinGW-w64 is a documented **development-only** dependency
  (`docs/LICENSES.md`), never linked or shipped. The MSVC job stays the gate that
  says "it really builds and runs on Windows"; the cross-check only makes sure it
  is not asked to discover what a type checker can.

## ADR-0015 — Delay compensation is per connection, and it delays the early path

* **Accepted and implemented** (M5, issue #5). `GraphBuilder::build()` walks the
  topological order once: `pathLatency[node] = max(pathLatency[inputs]) + own insert
  latency`, and every incoming edge whose source path is shorter than the node's
  latest input gets a delay of the difference. Delays live in a `dsp::DelayLine` per
  edge, allocated in `prepare()`. `GraphPlan::latencySamples()` stays the *longest*
  path, so switching compensation on does not change the reported round-trip figure.
* **Why per edge and not per node:** an obvious first implementation gives each node
  a delay of `longestPath − itsPathLatency`, which is wrong as soon as a node feeds
  two destinations with different path latencies. Compensation is a property of the
  connection, so that is where it is stored (`graph::InputEdge`).
* **Why the early path and not read-ahead:** delaying the early path needs no future
  audio, leaves the transport's meaning alone, and works with a live input in the
  same graph. Read-ahead (Ardour's playback default) is better where it applies — it
  keeps audio and timecode together — but it cannot be applied to a monitored input
  and it redefines "play position" for every consumer. The mechanism lives in the
  graph, so read-ahead remains an option for the transport layer later.
* **Rejected:** reporting latency only and letting the user nudge tracks (silently
  produces comb filtering when forgotten — manual override stays as a planned
  affordance for plug-ins that *lie* about their latency); including the master
  strip's look-ahead in the plan (the limiter runs after the plan, so there is
  nothing downstream to align it with); compensating live monitoring (impossible
  without time travel, documented as a limitation in `../AUDIO_ENGINE.md`).
* **Consequences:** bypass is a graph change (a bypassed processor contributes no
  latency, exactly as it produces no delay); the engine's plan rebuild is the
  re-learning path for late latency reports (VST3 `kLatencyChanged`); and the tests
  measure where impulses *land*, not what the plan claims — a compensation test that
  only reads the number would pass with no compensation at all.
* **Found while testing this:** `GraphNode::prepare()` now calls `prepare()` on the
  processors in its insert chains. Nothing did before, so a track insert processed
  with unprepared state (silent delay lines, garbage filters). Fixed as part of this
  ADR, with a regression test.

## ADR-0016 — The mixing path is optimised algorithmically, not with SIMD intrinsics

* **Accepted and implemented** (M5, issue #6). Measured ceiling first: hand-written
  AVX2 + FMA beats an *auto-vectorizable* SSE2 baseline by only 1.5–2× on gain and pan,
  and by **1.0×** on the fan-in summing loop (it is load/store bound at 256-frame
  blocks). The whole fan-in path is ~3 % of a 48-track session. Verdict: two code
  paths plus a CPU-feature dispatch for well under 1 % end to end is not a trade this
  project makes.
* **What was adopted instead:** block-state unrolling of the two loops that are
  actually expensive and that no compiler can vectorize, because they are first-order
  recursions with a loop-carried dependency — the level meter's exponential RMS and a
  node's gain ramp. A group of *k* steps has a closed form (the recurrence is linear
  and time-invariant inside a block), which shortens the dependency chain from four
  steps to one. Measured: meter −41 %, fader ramp −63 %, a 1-track session −6 %; the
  same recurrence re-associated, so the audio is unchanged to 1e-5 relative
  (0.0001 dB), pinned by `tests/dsp/BlockStateTests.cpp`.
* **Rejected:** `-ffast-math` / `/fp:fast` (breaks reproducible DSP, which tests and
  byte-identical renders depend on); `-march=native` (binaries illegal on older CPUs,
  and untestable in CI); AVX-512 (absent from the reference "modest PC" profile);
  intrinsics behind a runtime `cpuid` dispatch (the maintenance cost lands on every
  future edit of the mixer for a gain the profile does not show).
* **Why the "measure the ceiling" step mattered:** the first probe reported a 7× win
  for AVX2 because its *scalar* baseline was written without `__restrict` and therefore
  could not be vectorized by the compiler at all. A benchmark that flatters its
  favourite answer is worse than none — the same lesson as the denormal trap in the
  fader case (ADR-0015's benchmark note), where a case that drained its material into
  subnormals reported 72 ns/frame for a 2.8 ns/frame loop.
* **Consequences:** the mixer's elementwise loops stay compiler-vectorizable and
  alias-friendly (no hand-written kernels to keep in step with a CPU feature table);
  the recursion helpers in `core/Math.hpp` are the shared, tested home for this pattern
  and future smoothers should use them; the decision has a written revisit condition
  (elementwise loop > 10 % of the block budget on the reference profile, then ≥2×
  measured in isolation), and `std::simd` from C++26 is named as the development that
  would remove the cost side of the trade.

## ADR-0017 — The sandbox transport's identity and ordering are explicit, not inferred

* **Accepted.** Four things the M7 design assumed turned out to be wrong. The first
  three were found by the transport's own two-thread test rather than by reading the
  code; the fourth was found by a Windows Release runner, and only there.
  1. **A block's identity travels with the block.** `AudioSlotHeader` carries an
     `origin` field: the producer's own absolute block number. The consumer compares
     it against what it expects and reports `inconsistent`/`outOfOrder` rather than
     trusting the ring index.
  2. **The seqlock version word is `memory_order_seq_cst`.** Both the writer's and the
     reader's accesses to `ControlBlock::transportVersion` are `seq_cst`.
  3. **A consumer finishes verifying a slot before it releases it.** The reader's
     re-check of a slot's sequence happens before it advances `consumed`.
  4. **The seqlock's payload is copied word by word through relaxed atomics.**
     `copySnapshotFrom()` and `copySnapshotTo()` in `src/plugin/sandbox/SandboxArena.cpp`
     move `TransportSnapshot` as six 8-byte `std::atomic_ref` accesses, never as a
     struct assignment.
* **Why:**
  * *The index is not a tag.* The design's pipeline diagram implies the consumer can
    work out which block a slot holds from where the ring has got to. After **any**
    dropped block the input and output rings have different indices, so that inference
    silently mislabels every subsequent block — and a mislabelled block is worse than a
    lost one, because it sounds like the plug-in is working. `origin` costs 8 bytes in a
    header already padded to a cache line.
  * *Release/acquire is not enough for a seqlock.* The writer's `store(version + 1)`
    must not be reordered after the payload stores, and the reader's payload loads must
    not be reordered before its first `load(version)`. `release`/`acquire` order each
    pair correctly but permit the reader's *second* version load to be hoisted above its
    payload loads — the C++ abstract machine allows a reader to accept torn data under
    an old, even version. x86-64 TSO forbids it, so the bug is invisible on the machine
    it was written on and live on ARM. Correctness-by-ISA is not acceptable in a
    boundary whose entire purpose is surviving a peer that misbehaves.
  * *Ordering the version word does not make the payload copy safe.* Item 2 fixed the
    fences and left `control->transport = snapshot` — a plain struct assignment racing
    with a plain struct write, which is a data race and therefore undefined behaviour.
    UB is a licence, not a formality: MSVC at `/O2` appears to have
    common-subexpression-eliminated the reader's copy across its retry loop, so a
    payload torn on attempt 0 was reused on attempt 1 and accepted under a version that
    matched. CI's Windows Release job caught **one such read in 112 026**, on a
    transport that had passed every Linux configuration and every MSVC Debug build —
    including the two-thread test that found the other three. Relaxed word accesses
    remove the race and therefore the licence, and cost nothing: 48 bytes is six
    lock-free words, and the ordering that makes the result consistent still comes from
    the `seq_cst` version word, not from these loads.
  * *Releasing first fabricates tears.* The reader stored `consumed` and *then*
    re-checked the sequence. The producer, correctly, took the slot the instant
    `consumed` moved and stamped it with the next block's sequence — so the re-check
    read the new value and reported `Torn` for a copy that was perfectly clean. The
    stress test caught it at roughly one lost block in thirty thousand, which is also
    roughly the rate at which it would have been dismissed as a flaky test instead of a
    protocol bug. It was flaky for exactly one reason: the protocol was wrong.
* **Rejected:** inferring block numbers from ring indices; a `release`/`acquire` seqlock
  (plus `std::atomic_thread_fence(acq_rel)`, which orders the fences but still leaves the
  reader's second load free to move relative to the payload); reporting `Torn` without
  zeroing the destination (half of two different blocks is not audio); treating an
  occasional lost block as acceptable because a retry loop hides it; a plain struct
  assignment for the payload (a data race, and one an optimiser exploited); and
  `std::atomic_ref<TransportSnapshot>` for the whole struct, which is race-free but not
  lock-free at 48 bytes and would put a mutex on the audio path to remove a race from
  it. Also rejected: growing the snapshot to a word multiple to make the copy possible.
  It already is one, which is asserted at the copy rather than assumed, so a future
  field that breaks it fails the build instead of silently leaving bytes outside the
  seqlock.
* **Consequences:** `seq_cst` on one word per transport exchange is a `mfence`-class cost
  on x86 and is worth it — it is once per block, not once per sample. The rule generalises
  and is written into the header: **a consumer may not release a slot until it has
  finished verifying what it read from it.** The rings keep plain release-store/
  acquire-load SPSC publication, which *is* sufficient there because each has a single
  writer and a single reader and no read-modify-write of shared state. Separately,
  `TransportStats::overruns` counts *refused pushes*, not dropped blocks — documented at
  the field, because a caller that retries (a test) racks refusals up by design while a
  real host, which cannot retry from inside an audio callback, sees exactly one dropped
  block per refusal.

---

## Open decisions

| # | Question | Needed by | What settles it |
|---|----------|-----------|-----------------|
| O-1 | ~~Out-of-process plug-in sandbox: child process vs. JUCE-style plugin host process~~ — **settled** by [`PLUGIN_SANDBOX_RESEARCH.md`](PLUGIN_SANDBOX_RESEARCH.md): one *shared* helper process per policy (`SandboxPolicy::Shared` is the default for third-party plug-ins), not one per instance and not in-process. Phase 1 (the transport) is landed; ADR-0017 records the three design corrections it forced | M7 | done |
| O-2 | VST3 SDK: statically linked and vendored into the build, or loaded through an optional adapter module | M6 | whether the MIT text can be redistributed in binary releases as we intend |
| O-3 | MSIX in addition to an Inno Setup EXE | M11 | whether the Store/sideload story is worth the signing cost |
| O-4 | ~~SIMD path (SSE2/AVX2) for the graph and EQ, or leave it to the compiler~~ — **settled by ADR-0016** (leave the elementwise loops to the compiler; unroll the recursions instead) | M5 | done |
| O-5 | Whether to ship an internal sample-rate converter | after M5 | user reports; until then a mismatch is an explicit error |
