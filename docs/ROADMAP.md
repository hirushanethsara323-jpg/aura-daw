# ROADMAP.md

Milestones M0–M12, what each one delivers, and its acceptance criteria. Status is
updated in the same commit that completes the work — this file is the project's
honest answer to "where is it?".

| # | Milestone | Delivers | Acceptance criteria | Status |
|---|-----------|----------|---------------------|--------|
| **M0** | Foundations | repo layout, CMake build, licence, version header, `AuraResult`/errors, math, JSON, logging, file system, realtime utilities, localisation architecture | clean configure+build on MSVC and GCC; unit tests for core | ✅ **done** |
| **M1** | Time and tempo | 960 PPQN model, tempo map, time signatures, BBT, SMPTE (incl. 29.97 drop-frame), formatting | round trips and tempo-change tests pass | ✅ **done** |
| **M2** | DSP suite | 14 processors + biquad/smoothing primitives, denormal protection, analytic and property tests | unit tests green; limiter ceiling and EQ magnitude verified | ✅ **done** |
| **M3** | Graph and streams | `GraphNode`/`GraphPlan`/`GraphBuilder` (Kahn ordering, cycle reporting), fan-in, sends, plan publication with a grace window, memory and disk streams | graph tests + streaming regression green | ✅ **done** |
| **M4** | Audio engine | `IAudioDevice`, WASAPI shared/exclusive, ASIO (opt-in), mock device, transport, clip/instrument rendering, recording engine, mixer, offline renderer + stems | engine tests green incl. allocation regression and latency accounting | ✅ **done** |
| **M5** | Performance & hardening | `bench/` suite, session-scale benchmark, sanitizer runs (GCC/Clang **and** MSVC ASan), fuzzing the manifest and WAV parsers, memory-growth soak, delay compensation for dry paths, SIMD decision | budgets in `PERFORMANCE.md` met on the reference profile; no ASan/UBSan findings | 🔨 **one item left** — every gate is green: benchmarks, GCC/Clang sanitizers, the MSVC ASan row (*Windows Debug-ASan*), the fuzz campaign and the soak harness. The outstanding item is the 8-hour soak campaign, which `PERFORMANCE.md` marks as a plan rather than a measurement |
| **M6** | Plug-in hosting | VST3 adapter (`AURA_ENABLE_VST3`), out-of-process scanner with timeouts and blacklist, parameter and latency discovery, generic editor fallback | a real VST3 loads, processes and restores state; a crashed scanner does not take the host down | 🔨 **acceptance verified, packaging left** — the adapter loads a real bundle built from this repository, processes through the engine's own buffers, converts parameters through the plug-in's ranges, reports latency (and re-reads it on `kLatencyChanged` and after a state restore) and round-trips state; the scanner runs the bundle in `aura_scan_host`, times it out and quarantines it with a reason. `build-vst3` (VST3 ON): 143/143 tests. Left: the editor *window* (the shell is M9/M10) and the macOS fixture layout |
| **M7** | Plug-in sandbox + CLAP | out-of-process audio host, CLAP adapter, latency re-reporting through the graph | a plug-in crash is survivable; CLAP plug-ins process | ⏳ |
| **M8** | ASIO + device hardening | ASIO backend (GPLv3 SDK path), device matrix testing, sample-rate-change recovery, hot-plug UX | ASIO device at ≤ 64-frame buffers without xruns on the reference machine | ⏳ |
| **M9** | Desktop shell | Win32 + Direct2D window, widget toolkit, theme, DPI awareness, transport + track headers + mixer, preferences, device panel | opens, plays, records, saves a project; idle CPU < 2 % | ⏳ |
| **M10** | Arrangement & piano roll | arrangement canvas from the peak cache, clip editing gestures, piano roll, browser/inspector, dialogs, shortcut manager, accessibility pass | editing, recording and exporting done entirely in the UI | ⏳ |
| **M11** | Packaging & release | Inno Setup installer, portable ZIP, checksums, GPL source tarball, release workflow; installer/update story per `research/PACKAGING_RESEARCH.md` | signed (or checksummed) artefacts attached to a tagged release | ⏳ |
| **M12** | v1.0 polish | localisation (Sinhala/Tamil/German/Japanese catalogues), crash-report flow, docs pass, performance soak, plugin compatibility fixes | documented release criteria met; no known data-loss defects | ⏳ |

## Work queue (M6, in progress)

1. ~~VST3 adapter: scan, load, process, parameters, latency, state~~ **done** — see
   [`PLUGIN_HOST.md`](PLUGIN_HOST.md). Three real host bugs were found by the tests
   and fixed: `ProcessData` buffer ownership (a plug-in silently processed silence),
   the state-restore offset (every reload would have lost plug-in state) and latency
   not being re-read after a restart notification.
2. ~~**CI has to build it.**~~ **done** - `Linux VST 3 hosting` and the Windows
   `Debug-VST3` row.
3. **Windows is the product platform.** CI now builds and runs the adapter there
   (`Debug-VST3`) against a real Windows bundle. Still open: the child process has no
   job object, so a plug-in that spawns its own children is not cleaned up with the
   scanner.
4. **Sidechain and aux buses are wired to silence**, not routed: the graph needs a
   sidechain send before a compressor's detector means anything (M7/M8).
5. **Sample-accurate parameter changes.** Block-aligned today; the format supports
   per-sample points and the transfer ring is already in place.
6. **A plug-in that misreports latency** needs the manual override the M5 research
   called for.

## Work queue (M5)

1. `bench/` micro-benchmarks + the session-scale benchmark; publish numbers in
   `PERFORMANCE.md`.
2. ~~ASAN/UBSAN job findings triaged~~ **done** — the Linux *Debug+Sanitizers* job is
   green, and running the new soak harness under ASAN found a real
   use-after-free in the device/engine ownership handshake (fixed in
   `AudioEngine::detachDevice()` + `AudioDeviceManager::releaseDevice()`,
   regression test in `tests/api/DeviceLifetimeTests.cpp`). Remaining: an MSVC
   ASAN attempt on the Windows runner.
3. ~~Fuzz the manifest parser (`project.json`) and the WAV chunk parser~~ **done** —
   three libFuzzer targets (`fuzz/fuzz_{json,manifest,wav}.cpp`) with committed
   seed corpora, a bounded CI job, and a portable `[fuzz]` test that runs under
   MSVC as well. The first finding was a WAV header that over-claimed its data
   size (fixed; the regression test pins it).
4. ~~Memory-growth soak: 8 hours of simulated playback with recording on and off~~
   **harness done, campaign running** — `bench/aura_soak` (trend in MB/hour, exit
   code as a gate, JSON checkpointed every sample), 2 minutes of it in CI; the
   8-hour campaign is reported in `PERFORMANCE.md`.
5. ~~Delay compensation: insert the dry-path delays.~~ **done** — per-edge
   compensation in `GraphBuilder::build()`, delays in `dsp::DelayLine`, verified by
   impulse measurements in `tests/graph/DelayCompensationTests.cpp`; see
   `research/LATENCY_COMPENSATION_RESEARCH.md`. Remaining follow-ups: manual latency
   override for plug-ins that misreport, and turning the adapter's latency re-read
   into a graph-plan rebuild (the adapter re-reports on `kLatencyChanged` and after a
   state restore as of M6; the plan still has to be rebuilt from it).
6. ~~SIMD decision, made from profile data rather than taste.~~ **done** — measured:
   hand-written AVX2 is worth 1.5–2× on elementwise loops that are ~3 % of a session,
   so it is deferred; the profile pointed at the per-sample recursions instead, which
   are now evaluated in blocks (`research/SIMD_RESEARCH.md`, ADR-0016).

## Deliberate non-goals for v1

* macOS/Linux builds of the *shell* (the engine builds and is tested on Linux, which
  is how CI works; the product is Windows).
* Video import, notation/MIDI scoring, collaboration/cloud features.
* A plug-in *format* of our own, or bundling third-party instruments.
* Bundled compressed export formats (MP3/AAC/FLAC) — they need encoder dependencies
  and a licence review.

## How milestones map to GitHub

Labels: `area:engine`, `area:dsp`, `area:ui`, `area:plugins`, `area:project`,
`area:build`, `area:docs`, `type:bug`, `type:feature`, `type:task`, `type:research`,
`good first issue`, `priority:high|medium|low`. Issues are attached to the milestone
above; `scripts/github_setup.py` creates the labels, the M0–M12 milestones and the
starter issues on GitHub from a `GITHUB_TOKEN` environment variable — idempotent,
standard-library only, and it stores no credentials (see `SECURITY.md` and
`DEVELOPMENT.md#repository-administration`).
