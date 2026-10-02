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

- **M7 phase 2 — the plug-in helper process.** `aura_plugin_host`
  (`tools/plugin_host/PluginHostMain.cpp`) is the other side of the sandbox: a
  separate program that attaches to a mapping it did not create, runs audio
  through it and exits with a number that says why it stopped. It has never heard
  of the engine, the project or a plug-in format — those sit behind
  `SandboxProcessor`. Audio now leaves one process, is processed in another and
  comes back, which is the first point at which this milestone tests isolation
  rather than a data structure. Its audio path runs under **back-pressure**: it
  holds one processed block until the host has room instead of dropping it when
  the output ring is full. The first version dropped, and a 200-block test lost
  two to five blocks depending on scheduling; a bounded retry was tried and was
  worse, because any budget short of "wait for the host" still drops under load.
  Holding one block costs nothing, since the rings are two slots deep and the
  host pops before it pushes — which is exactly why `SharedAudioRing::exchange()`
  is ordered that way.
- `plugin::sandbox::ProcessSupervisor` — starts a long-lived helper, notices when
  it stops and stops it when it will not (polite signal, grace period, then
  `SIGKILL` to the whole process group). Deliberately *not*
  `plugin::runChildProcess()`: that one is spawn-wait-report with a wall-clock
  deadline, the right shape for a scanner that should answer in milliseconds and
  the wrong one for a process meant to run for a session. It widens paths with
  `MultiByteToWideChar` rather than byte by byte, which the scanner's helper does
  and which is wrong for any Windows user whose name holds a character outside
  ASCII.
- `plugin::sandbox::SharedMemory` — a named mapping with **asymmetric ownership**:
  the creator unlinks it on release and an opener only unmaps, so a helper that
  exits cleanly cannot pull a session's buffers out from under the host, and a
  host that shuts down cannot orphan a name that would make the next session's
  `create()` fail with `AlreadyExists`. POSIX `shm_open`/`mmap`, Windows
  `CreateFileMappingW`/`MapViewOfFileEx`, one shared name-validation rule, and
  `platformShmName()` for the `/aura-` and `Local\aura-` prefixes. **Windows has no
  unlink**, so the asymmetry is POSIX-only in the literal sense: a named section
  lives until its last handle closes, and `create()` on a name a live peer still
  holds correctly reports `AlreadyExists` rather than handing back that peer's
  section. The consequence is a rule for callers — *a session's name must be unique
  to that session* — and it was found by CI's Windows jobs after a test asserting
  the POSIX rule unconditionally had passed on Linux for a whole increment.
- `plugin::sandbox::SandboxProcessor` — the format-agnostic seam a helper drives
  (prepare, process, parameter, note-in, note-out, state), a registry, and a
  reference processor: gain on parameter 0, notes echoed back. Deterministic on
  purpose, so a test can predict every sample. Without it the cross-process path
  could only be exercised with a real third-party bundle, which no CI runner has,
  and a path CI never runs is a path that is broken.
- `plugin::sandbox::HelperProtocol` — the contract between the two programs: the
  helper's file name, its switches, its exit codes and its processor ids. It is a
  wire format in all but name, and each side can detect disagreement with it: the
  helper checks the arena's layout version and the host checks the exit code.
- `ArenaView::attach(bytes, size)` — attach using the spec the peer's own header
  declares, because a helper has no independent copy to compare against and
  inventing one would make the comparison meaningless. Trusting an untrusted
  header is safe here only because the header is then *proved* against the bytes:
  magic, layout version, a spec that normalises, offsets equal to what that spec
  implies, and a mapping large enough for the result. Four tests lie about each
  of those in turn and check the refusal.
- 12 tests in `tests/plugin/sandbox/SandboxHelperTests.cpp` — 5 tagged `[shm]`
  for the mapping, 7 tagged `[helper]` for the process. The end-to-end one pushes
  200 blocks whose signal is derived from the block number and asserts that every
  returned sample equals its input times exactly 0.5, that every origin arrives
  once and in order, that both notes come back, that the helper exits 0 on its
  own and that its heartbeat counts 200. Four more cover a helper that finishes
  on its own, one that cannot be started, one whose host goes quiet (it exits
  with `kHostGone` rather than waiting forever), and a supervisor that goes out
  of scope and takes its helper with it.
- The helper's host-gone deadlines: `--startup-timeout-ms` before the first block
  and `--idle-exit-ms` after it, because "no blocks yet" means different things
  before and after a session has been live. Both are backstops — the real
  watchdog counts blocks in the host (phase 4) and a Windows job object with
  `KILL_ON_JOB_CLOSE` (phase 6) does not need the helper to guess anything about
  time at all.

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

- `tools/rt_audit.py` now audits the helper's real-time loop: `RT_SCOPE_GLOBS`
  gains `tools/plugin_host/*.cpp` and `RT_FUNCTION_PATTERNS` gains
  `^runHelperLoop$`. The helper's loop is a real-time thread in every sense the
  engine's callback is — no allocation, no locks, no file IO, no exceptions, no
  logging — and it is *allowed* to sleep when idle, which the engine's callback
  is not, because nothing is waiting on it and a helper that spun would burn the
  core the host's callback needs. A new audit scope nobody verified is a
  decoration, so this one was checked by injecting a `std::vector` into the loop,
  confirming the audit fails the build, and reverting.
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
