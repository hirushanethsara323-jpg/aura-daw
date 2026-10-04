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

- **M7 phase 3 — the proxy.** `SandboxedPluginInstance` implements `PluginInstance`
  on top of a helper session, and `SandboxSession` owns that session: create the
  mapping, lay out the arena, publish what the session is, start the helper, wait
  for it to report ready, and own its life until it is stopped. A `PluginChain`
  cannot tell the difference — one test puts the proxy in a real chain and checks
  latency summing, the state round trip and bypass through the chain's own API.
  **Nothing in AURA creates one yet**: `PluginHost`'s creation path still returns
  in-process adapters, and choosing between the two is the policy plumbing of
  phase 5. So a plug-in loaded by a user today still runs in AURA's address space,
  and a crashing plug-in still crashes AURA.
- `SandboxedPluginInstance::process()` pushes and then pops, and the order is the
  whole trick: the chain processes audio in place, so the buffer is both the input
  and the destination for the output, and pushing first copies it into the ring's
  slot before anything overwrites it. `SharedAudioRing::exchange()` pops first —
  deliberately, to give the helper the longest run at the previous block — and with
  an in-place caller that order would overwrite the input before publishing it.
- A refused push now drains one output into scratch and retries, which fixes a
  deadlock the first version had. Returning early looked safe: the audio stays dry
  and nothing is lost. But the helper runs under back-pressure, so it holds a
  processed block until the output ring has room and takes no new input while it
  does — a host that will not pop until its push succeeds and a helper that will
  not push until the host pops wait for each other forever, which presents exactly
  as a hung plug-in. The scratch is allocated in `prepare()` and never in
  `process()`. A push refused for any other reason (`TooLarge`, `WrongSide`,
  `NotAttached`) is counted and left dry rather than retried, because retrying a
  caller bug turns it into a spin.
- `plugin::kChainMidiOutCapacity` — one constant, used by both `PluginChain::process()`
  and the proxy. `PluginInstance::process()` takes a raw pointer and a count by
  reference and never says how big the buffer is, so every implementation had been
  assuming 64 independently.
- `TransportSnapshot::operator==`, so a publisher can ask whether the transport moved
  instead of writing the seqlock twelve times a block for nothing.
- 8 tests in `tests/plugin/sandbox/SandboxProxyTests.cpp`, each spawning a real
  helper: audio equal to the reference processor's output at half gain exactly one
  block late (compared with `==` per sample), notes crossing both ways, a helper
  that stops leaving the slot dry rather than silent or hung, a real chain that
  cannot tell the difference, parameter state that round-trips and a blob that is
  not this slot's being refused, `prepare()` twice leaving one reaped helper rather
  than two with a unique shared-memory name for the second, a processor that does
  not exist leaving the slot failed and dry with the helper's own exit code and
  stderr still available, and bad limits refused without breaking a working session.

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

- **The transport's seqlock payload was a data race, and MSVC `/O2` took the
  licence.** `publishTransportSnapshot()` wrote `control->transport = snapshot` and
  `readTransportSnapshot()` read it back the same way: a plain struct assignment
  racing with a plain struct write, which is undefined behaviour regardless of how
  carefully the version word is ordered. ADR-0017 had already fixed the *ordering*
  of that version word and left the payload alone. CI's Windows Release job caught
  the consequence — one read in 112 026 accepted a payload torn across two
  snapshots, on a transport that had passed every Linux configuration and every
  MSVC Debug build before it. The likely mechanism is common-subexpression
  elimination across the reader's retry loop: a copy torn on attempt 0 reused on
  attempt 1 and accepted under a version that matched. Both sides now copy the
  snapshot word by word through `std::atomic_ref<std::uint64_t>` with
  `memory_order_relaxed` — race-free, lock-free, and still ordered by the `seq_cst`
  version word rather than by these accesses. `std::atomic_ref<TransportSnapshot>`
  was rejected: a 48-byte `atomic_ref` is not lock-free, so it would have put a
  mutex on the audio path to fix a race. No layout change — the snapshot is already
  48 bytes on an 8-byte boundary, asserted rather than assumed, so `kLayoutVersion`
  stays at 1. Tests: the tearing case 30 consecutive runs, the two-thread
  60 000-block stress case 12, both clean.
- **`tools/rt_audit.py` had two more silent blind spots.** (1) Its
  `function_name_from_signature()` rejected any signature containing `[` before the
  parameter list — a lambda test that also rejected `[[nodiscard]]`, so every
  inline definition written with a leading attribute was skipped. Thirteen
  real-time functions across the DSP, graph and sandbox headers were being counted
  as audited by nothing. (2) Its two passes over a file made the audit-or-exempt
  decision *separately* and could disagree, so a function could be counted as
  scanned by the first pass and skipped by the second — reporting coverage the
  audit did not have. The decision is now one function used by both passes,
  attributes are stripped before anything inspects the text, and a name in the
  real-time list beats a *shape* in the control-thread list (`^set[A-Z].*` had been
  swallowing `setTargetGains()`, a two-float setter whose own doc comment says it
  may be called from the audio thread). Exemptions are now printed with the run
  rather than applied silently, so coverage is read off the output instead of
  inferred from a count: 62 → 82 functions. Both fixes were found the same way —
  inject a violation into a function that was supposed to be gated and watch
  whether the audit notices.
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

- The proxy tests settle the pipeline before measuring it, and then assert the delay
  does not move. The ring is two slots deep, so a helper that finishes inside a block
  period gives a deferral of exactly one block and one that does not gives two: it
  misses once, the slot returns dry, and from then on two blocks are in flight. Both
  are the transport working. The first version asserted one, which passed on a fast
  runner and failed on an instrumented one and on the Windows runners with the same
  code — `corrupt: 0, silent: 0, torn: 0, lost: 0, wrong delay: 39` is a scheduling
  fact, and reporting it as a transport bug sends somebody to debug the wrong process.
  Each returned block is now classified (dry / delay *n* / silent from a discarded torn
  read / corrupt) and only the last is a failure.
- The helper's idle sleep is 200 µs after 512 spins, down from 1 ms after 64. That
  sleep is the helper's worst-case reaction time to work arriving, and 1 ms against
  a 2.7 ms block at 128 frames and 48 kHz is a third of the budget spent doing
  nothing. It still sleeps — which the engine's callback may not — because nothing
  waits on this thread and spinning would burn the core the host's callback needs.
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
