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
| Milestone | **M7 — Plug-in sandbox + CLAP**, phase 1 of 7 landed |
| Target platform | Windows 10/11 x64; developed and tested on Linux/GCC with MSVC parity enforced by the warning contract |
| Licence | GPL-3.0-or-later |
| Default branch | `main` |

## Build status

| Configuration | State |
|---|---|
| `RelWithDebInfo`, GCC 14, Linux | **green** — configures, builds with no warnings, 153/153 `ctest` entries pass |
| `-DAURA_WARNINGS_AS_ERRORS=ON` | **green** — the full `/W4 /WX` ↔ `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wunused-parameter` contract |
| `-DAURA_ENABLE_ALLOC_TRACKING=ON` | **green** — 153/153, and the transport's allocation counter measures **0** over a 2 000-block run |
| Windows / MSVC (GitHub Actions) | last recorded **green at M6** with 143 entries; the count has moved since (153 now) and CI has not been re-run against this branch on a Windows runner |
| `-DAURA_ENABLE_VST3=ON` | not exercised in this environment (the SDK is fetched at a pinned tag; needs network and Windows or a Linux bundle layout) |

## Test status

| Layer | State |
|---|---|
| `ctest` | **153 entries, all passing** — 151 Catch2 test cases + 2 static audits |
| `tools/rt_audit.py` | 60 files, **63 audio-thread functions scanned, 0 errors, 0 warnings**, 1 annotated allowance (`src/midi/Instrument.cpp:250`) |
| `tools/include_audit.py` | 135 files, **0 findings** |
| Sandbox transport (`[sandbox]`) | 15 cases, 2108 assertions, including a 60 000-block two-thread stress run |
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

## Partial

| Area | What exists | What is missing |
|---|---|---|
| Out-of-process plug-in sandbox | the transport (arena + rings), 15 tests | **no helper process, no proxy, no crash protocol, no watchdog** — nothing is isolated yet (M7 phases 2–6) |
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

None that are reproduced, diagnosed and open at the time of writing. Three were found
and fixed in this increment and each has a regression test: an allocation on the audio
thread in `Clip::render()`, an extractor fault in `tools/rt_audit.py` that left whole
translation units unaudited, and a consumer that released a shared-memory slot before
it had verified what it read. All three are in [`CHANGELOG.md`](CHANGELOG.md).

Documented **limits** that are decisions rather than defects:

- A crashing plug-in still crashes AURA (isolation covers scanning, not playback).
- Only bus 0 of a plug-in is the track; sidechain and aux buses are wired to silence.
- A mono track feeding a stereo plug-in leaves the second channel silent, with a
  warning logged at `prepare()`.
- The engine's allocation-counter test does not render a clip from a stream, so a
  per-block allocation down that path would not be caught dynamically — recorded in
  `docs/TESTING.md`'s gap table with the fix.

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

## Next milestone

**M7 phase 2 — the helper process.** `aura_plugin_host` loads one bundle, runs the
real-time loop against the arena and reports state over the control channel, started
by a supervisor. Then phase 3: `SandboxedPluginInstance` implementing `PluginInstance`
so the chain and the engine see no difference. Design, decisions and the remaining
phases: [`docs/research/PLUGIN_SANDBOX_RESEARCH.md`](docs/research/PLUGIN_SANDBOX_RESEARCH.md).
