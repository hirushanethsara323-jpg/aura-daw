# PROJECT_STATUS.md

The honest state of AURA DAW, maintained per milestone. This file says what
**works**, what is **partial**, and what is **not built** — the project's rule is to
document what exists rather than what is planned, and not to claim completion that
has not been objectively verified.

Last updated with M7 phase 1 (the sandbox transport).

## Current version

| | |
|---|---|
| Version | `0.1.0` (development; no release has been shipped) |
| Milestone | **M7 — Plug-in sandbox + CLAP**, phase 3 of 7 landed |
| Target platform | Windows 10/11 x64; developed and tested on Linux/GCC with MSVC parity enforced by the warning contract |
| Licence | GPL-3.0-or-later |
| Default branch | `main` |

## Build status

| Configuration | State |
|---|---|
| `RelWithDebInfo`, GCC 14, Linux | **green** — configures, builds with no warnings, 173/173 `ctest` entries pass |
| `-DAURA_WARNINGS_AS_ERRORS=ON` | **green** — the full `/W4 /WX` ↔ `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wunused-parameter` contract |
| `-DAURA_ENABLE_ALLOC_TRACKING=ON` | **green** — 173/173 together with `-DAURA_WARNINGS_AS_ERRORS=ON` in one build, 0 warnings, and the transport's allocation counter measures **0** over a 2 000-block run |
| Windows / MSVC (GitHub Actions) | **green on pull request #13** — run 36994237694, **12 of 12 jobs** at `00f8ea3`: Debug and Release `/W4 /WX`, ASan/UBSan, the VST 3 fixture, fuzzing, clang-tidy, the MinGW cross-check and the soak harness. Phase 2 was merged on the strength of that run, so the run predates this row by one documentation-only commit. `main` was previously verified at `1a88a90` (run 36981136588). Those two Windows runs are the reason phase 2 shipped a fix it would otherwise have missed: MSVC at `/O2` found a data race that no Linux configuration and no MSVC Debug build could see |
| `-DAURA_ENABLE_VST3=ON` | not exercised in this environment (the SDK is fetched at a pinned tag; needs network and Windows or a Linux bundle layout) |

## Test status

| Layer | State |
|---|---|
| `ctest` | **173 entries, all passing** — 171 Catch2 test cases + 2 static audits |
| `tools/rt_audit.py` | 72 files, **84 audio-thread functions scanned, 0 errors, 0 warnings**, 1 annotated allowance (`src/midi/Instrument.cpp:250`) and 2 printed exemptions (`sampleInto`, `publishPlan`). Coverage grew from 62 in four steps: the helper's real-time loop was added as a scope, an extractor bug that skipped every `[[nodiscard]]` inline definition was fixed (13 functions), a real-time name was made to beat a control-thread shape (`setTargetGains`), and the proxy's `process()` plus its transport publisher joined — which is what verifies that the drain-on-refusal path allocates nothing, by the audit rather than by inspection. Every new gate was verified by injecting a violation and confirming the audit catches it — including confirming the two deliberate exemptions are still exempt |
| `tools/include_audit.py` | 148 files, **0 findings** |
| Sandbox transport (`[sandbox]`) | 15 cases, 2108 assertions, including a 60 000-block two-thread stress run |
| Sandbox proxy (`[proxy]`) | 8 cases, each spawning a real helper: audio equal to the in-process result one block late compared per sample with `==`, notes crossing both ways, a helper that stops leaving the slot **dry**, and a real `PluginChain` that cannot tell the difference — latency summing, state round trip and bypass all through the chain's own API |
| Sandbox helper (`[shm]`, `[helper]`) | 12 cases across two files: named shared memory on both platforms, and a **really spawned** `aura_plugin_host` that processes 200 blocks across a process boundary and returns every sample bit-exact, every origin once and in order |
| Fuzzing | 3 libFuzzer targets in CI + portable `[fuzz]` tests |
| Soak | `bench/aura_soak` runs 2 minutes in CI; the **8-hour single-take run is still outstanding** (M5's last item) |
| Open GitHub issues at the time of writing | 1 |

## Completed and working

Verified by tests, not by inspection — see [`docs/TESTING.md`](docs/TESTING.md) for
which test proves which claim.

- **Time, tempo and transport** — 960 PPQN, tempo map, time signatures, BBT, SMPTE
  including 29.97 drop-frame, a single owner of playhead state.
- **DSP** — 14 processors plus biquad and smoothing primitives, denormal protection,
  analytic and property tests.
- **Graph and streams** — topological ordering with cycle reporting, fan-in, sends,
  per-connection delay compensation.
- **Audio engine** — `IAudioDevice` with WASAPI shared/exclusive and opt-in ASIO
  backends, a mock device that makes the callback path testable without hardware,
  clip/instrument/recording rendering, offline rendering that reuses the live graph.
- **Project format** — the `.aura` directory (JSON manifest + media/peaks/cache/
  backups), atomic save, `.bak` rotation, autosave, crash flag and recovery, media
  pool with relink.
- **Commands and undo** — a centralised command registry and undo history; edits are
  values, not UI side effects.
- **Recording** — non-destructive, crash-safe, with disk-space monitoring.
- **Plug-in hosting (M6)** — VST3 behind an option, out-of-process scanning with
  timeouts and quarantine, parameters through the plug-in's own ranges, latency
  reporting, MIDI events, state round trip, database with search/favourites/blacklist,
  chains with order/bypass/latency.
- **Sandbox transport (M7 phase 1)** — shared-memory arena and one-block-deferred
  audio exchange, tested across two threads.
- **Sandbox helper process (M7 phase 2)** — named shared memory, a supervisor that
  starts/watches/kills a long-lived child, and `aura_plugin_host` itself: audio
  crosses a real process boundary under back-pressure and comes back bit-exact.
  The helper's real-time loop is audited by `rt_audit.py`, and the helper is
  format-agnostic — it drives `SandboxProcessor`, not an SDK.
- **Sandbox proxy (M7 phase 3)** — `SandboxSession` owns a helper's life and
  `SandboxedPluginInstance` implements `PluginInstance` on top of it, so a
  `PluginChain` cannot tell a sandboxed slot from an in-process one: latency sums,
  state round-trips, bypass and failed-slot skipping all work through the chain's own
  API. Its `process()` is inside `rt_audit.py`'s scope, so the drain-on-refusal path
  is verified allocation-free by the audit rather than by inspection.

## Partial

| Area | What exists | What is missing |
|---|---|---|
| Out-of-process plug-in sandbox | the transport, the named mapping, the supervisor, the helper process and a `PluginInstance` proxy a real chain cannot distinguish — 35 tests | **nothing chooses one**: `PluginHost`'s creation path still returns in-process adapters, so no plug-in a user loads is isolated yet. No crash protocol, no watchdog, no policy plumbing, no format adapter in the helper, no editor, and `saveState()` saves the proxy's parameters rather than the plug-in's own blob (M7 phases 4–7) |
| CLAP | the option and the format enumerator | no adapter; `hostSupports(PluginFormat::Clap)` returns false |
| Performance gates | measured and reported in `docs/PERFORMANCE.md` | not *enforced* — that needs a pinned machine |
| M5 hardening | every gate green | the 8-hour single-take soak |

## Not built

- **The desktop shell.** There is no window. AURA is currently a library and a test
  suite: M9 (Win32 + Direct2D) has not started, so nothing here is clickable, and
  there are consequently no GUI or interaction tests.
- **Arrangement canvas and piano roll** (M10).
- **Installer, portable build, releases** (M11).
- **Localisation catalogues** (M12) — the architecture exists, English only.
- **AI features** — deliberately not started; the master plan scopes them as future
  work that must be previewable and undoable, and never silent.

## Known bugs

None that are reproduced, diagnosed and open at the time of writing. Six were found and
fixed across M7 and each has a regression test: an allocation on the audio thread in
`Clip::render()`; an extractor fault in `tools/rt_audit.py` that left whole translation
units unaudited; a consumer that released a shared-memory slot before it had verified
what it read; a helper that dropped processed audio when the host was briefly behind
(fixed by back-pressure); a data race in the transport's seqlock payload that only
MSVC `/O2` exploited; and two more silent blind spots in `rt_audit.py`. All six are in
[`CHANGELOG.md`](CHANGELOG.md).

Two of those were found only by CI's Windows jobs and by a pull request run *before*
the merge, which is the argument for verifying a branch rather than verifying `main`
after the fact: the data race needed MSVC at `/O2`, and the shared-memory test that
assumed POSIX `shm_unlink` semantics needed a platform with no unlink.

Documented **limits** that are decisions rather than defects:

- A crashing plug-in still crashes AURA (isolation covers scanning, not playback).
  The helper process that would change this exists and is tested, but nothing in
  the engine starts one yet.
- Only bus 0 of a plug-in is the track; sidechain and aux buses are wired to silence.
- A mono track feeding a stereo plug-in leaves the second channel silent, with a
  warning logged at `prepare()`.
- The engine's allocation-counter test does not render a clip from a stream, so a
  per-block allocation down that path would not be caught dynamically — recorded in
  `docs/TESTING.md`'s gap table with the fix.
- The helper's host-gone detection is a **wall-clock backstop**, not the watchdog:
  it infers the host is dead from the absence of blocks, because a dead host cannot
  say so. The real watchdog counts blocks in the host and lands with the crash
  protocol (phase 4), and a Windows job object with `KILL_ON_JOB_CLOSE` (phase 6)
  removes the need for the helper to reason about time at all.
- A sandboxed slot's latency is one block **as long as the helper keeps up**. The ring
  is two deep, so a helper that misses a block deadline costs one dry block and then
  two blocks of effective delay while `latencySamples()` still reports one. Reported
  rather than padded: adding a block of PDC to every sandboxed plug-in to cover a slow
  machine would cost latency the design does not owe, and `dryBlocks()` shows the miss.
  A helper that misses deadlines is phase 4's watchdog's business.
- `request::kSaveState` crosses the boundary and is **acknowledged but refused**:
  the arena has no state buffer and the helper must not touch the disk from its
  real-time thread. Wiring it is a `kLayoutVersion` bump that belongs with phase 4,
  where the saved blob is the crash recovery point.

## Performance notes

Budgets and the way they are measured are in
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md). Two claims were re-measured for this
increment rather than assumed:

- **Zero allocation on the transport path.** Measured with
  `-DAURA_ENABLE_ALLOC_TRACKING=ON` over 2 000 blocks: 0 allocations, 0 RT violations.
  The same build asserts the engine's audio path allocates nothing.
- **No block lost or torn across a process boundary.** A two-thread run of 60 000
  blocks reports 0 inconsistent, 0 out-of-order, 0 from-the-future, 0 blocks lost and
  0 torn reads, with every published block processed exactly once. The ring's
  refusals are pacing, not loss, and are reported rather than asserted at zero —
  `TransportStats::overruns` counts refused pushes, and a caller that retries racks
  them up by design.

One behaviour was found and fixed inside this increment rather than after it: the
helper's first version **dropped** a processed block whenever the output ring was
full, and a 200-block test lost two to five blocks depending on scheduling. A
bounded retry was tried and was worse — any budget short of "wait for the host"
still drops under load. Back-pressure replaced both: the helper holds one block
until the host has room. The same test now delivers 200 of 200 with zero lost and
zero torn. The lesson recorded with it is about the test, not the transport: the
host side originally yielded in a tight loop while waiting for room, which kept a
core the helper needed, so the helper fell behind. A test that starves the process
it is measuring is measuring the test.

One behaviour was found and fixed inside this increment rather than after it: the
proxy's first version returned early when a push was refused, leaving the audio dry.
That looked like the safe answer and was a deadlock — the helper holds a processed
block until the output ring has room and takes no new input while it does, so a host
that will not pop until its push succeeds, and a helper that will not push until the
host pops, wait for each other forever. It presents exactly as a hung plug-in, and
only a cross-process test could show it: read either side alone and both look
correct.

## Next milestone

**M7 phase 4 — crash and hang.** The proxy exists and a chain cannot tell it from an
in-process slot, but nothing has yet been asked to survive a plug-in that dies, which
is the only reason this milestone exists. Phase 4 is a fixture with a deliberate crash
and hang hook (launched from a *child* process, unlike M6's scanner hooks, because it
runs the real-time loop), a watchdog that counts blocks rather than milliseconds —
`Telemetry::blocksProcessed` is already the heartbeat — and a restart from the last
state blob. That is also the phase that gives the blob somewhere to travel: the arena
has no state buffer, so wiring `saveState()` across the boundary is a `kLayoutVersion`
bump. Then phase 5 makes it reachable: `PluginUserData::sandbox`, the UI-facing API and
the database round trip — the point at which a user can turn it on. Design, decisions
and the remaining phases:
[`docs/research/PLUGIN_SANDBOX_RESEARCH.md`](docs/research/PLUGIN_SANDBOX_RESEARCH.md).
