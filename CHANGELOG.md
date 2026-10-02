# Changelog

All notable changes to AURA DAW are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
semantic versioning.

AURA has not shipped a release yet, so everything below is development history
organised by the milestones in [`docs/ROADMAP.md`](docs/ROADMAP.md). An entry is
added in the **same commit** as the change it describes; a change that is not
written down here did not land.

## [Unreleased]

### Added

- **M7 phase 1 — the shared-memory sandbox transport.**
  `include/aura/plugin/sandbox/SandboxArena.hpp` and `SharedAudioRing.hpp` with
  `src/plugin/sandbox/*.cpp`: the transport an out-of-process plug-in host will
  speak over. `SandboxArena` lays out a fixed mapping whose every region is an
  *offset* from the base, so the engine and a helper may map it at different
  addresses; `SharedAudioRing` runs a one-block-deferred audio exchange plus
  parameter, note-in and note-out rings, a control block and a telemetry block.
  Latency is `(slots − 1) × blockSize` and is reported so the graph can compensate
  for it. **It is a library with no caller yet** — nothing spawns a helper, so no
  plug-in is isolated by this change.
- 15 tests tagged `[sandbox]` in `tests/plugin/sandbox/SandboxTransportTests.cpp`,
  including a two-thread run of 60 000 blocks asserting every published block came
  back processed exactly once, and a zero-allocation claim *measured* against
  `-DAURA_ENABLE_ALLOC_TRACKING=ON` rather than read off the source.
- `audio::kMaxClipChannels` (64), the channel budget `Clip::render()`'s stack
  scratch is sized by.
- ADR-0017 in `docs/research/TECHNOLOGY_DECISION_RECORD.md`: the three corrections
  the transport's design needed, with the reasoning.
- This file, and `PROJECT_STATUS.md` at the repository root.

### Fixed

- **`Clip::render()` allocated on the audio thread.** It built a
  `std::vector<float*>` of per-channel write pointers on every call — roughly 24
  heap round trips every 2.7 ms in a 24-track session at 128 frames. Replaced with
  a stack array; a channel count outside `1..kMaxClipChannels` now renders silence
  and trips `AURA_RT_ASSERT` instead of overflowing it. Regression test:
  *"A clip refuses a channel count its stack scratch cannot hold"*.
- **`tools/rt_audit.py` audited less than it claimed.** Its function extractor never
  counted braces on the same line as an opening brace, so a one-line definition left
  it believing it was still inside that function and it swallowed the rest of the
  translation unit. On this tree the audit looked at 32 audio-thread functions and
  now looks at 44 — and the first thing it saw was the `Clip::render()` allocation
  above.
- **The transport's consumer released a slot before verifying it.** The reader
  advanced `consumed` and then re-checked the slot's sequence, so the producer —
  correctly, the instant `consumed` moved — restamped the slot and the re-check
  reported a clean copy as torn. Roughly one lost block in thirty thousand, which is
  also roughly the rate at which it would have been written off as a flaky test.
- A test that asserted nothing: the too-small-mapping case built a `Region` from a
  short byte count, but `Region` over-allocates to reach an aligned base, so the
  mapping was roomy and the refusal under test never happened. Now expressed as the
  *size argument* to `ArenaView::create()`.

### Changed

- `tools/rt_audit.py` scope: the sandbox headers and sources are on the audio path by
  definition, and boundary-crossing names (`push*`, `pop*`, `exchange`, `publish*`)
  are audited. `AudioEngine::publishPlan` and `Automation Lane::sampleInto` are
  classified **control-thread** with the reason written at the entry — classified,
  not suppressed.
- The engine's real-time test now *asserts* `realtimeAllocationCount() == 0` instead
  of only printing it, and documents honestly that its fixture does not render a
  clip from a stream, so that path is not covered dynamically (see
  `docs/TESTING.md`'s gap table).
- ADR-0013's suppression inventory now lists all five scoped
  `#pragma warning(disable : 4324)` sites. Open decision **O-1** (child process vs.
  host process) is marked settled by `docs/research/PLUGIN_SANDBOX_RESEARCH.md`.
- `docs/TESTING.md`'s entry count corrected to what `ctest` actually runs: **153
  entries** — 151 Catch2 test cases plus two static audits. It previously said 111.

## [0.1.0] — development baseline (M0–M6)

Not released; recorded here so the history above has something to stand on. See
[`docs/ROADMAP.md`](docs/ROADMAP.md) for each milestone's exit criteria.

- **M0 Foundations** — repository layout, CMake build, GPLv3 licence, version
  header, `AuraResult`/`Status` error model, math, JSON, logging, filesystem.
- **M1 Time and tempo** — 960 PPQN model, tempo map, time signatures, BBT, SMPTE
  including 29.97 drop-frame.
- **M2 DSP suite** — 14 processors plus biquad and smoothing primitives, denormal
  protection, analytic and property tests.
- **M3 Graph and streams** — `GraphNode`/`GraphPlan`/`GraphBuilder` with Kahn
  ordering and cycle reporting, fan-in, sends, per-connection delay compensation.
- **M4 Audio engine** — `IAudioDevice`, WASAPI shared and exclusive, ASIO (opt-in),
  a mock device for tests, transport, clip/instrument/recording rendering.
- **M5 Performance and hardening** — `bench/` suite, session-scale benchmarks,
  sanitizer runs on GCC, Clang and MSVC, libFuzzer targets, a soak binary. *One item
  still open: the 8-hour single-take soak, which needs a workstation rather than a
  CI runner.*
- **M6 Plug-in hosting** — VST3 adapter behind `-DAURA_ENABLE_VST3`, out-of-process
  scanner with timeouts and quarantine, parameters, latency, events, state round
  trip, plugin database and chains; CLAP still not written.
