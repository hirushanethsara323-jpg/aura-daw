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
| **M6** | Plug-in hosting | VST3 adapter (`AURA_ENABLE_VST3`), out-of-process scanner with timeouts and blacklist, parameter and latency discovery, generic editor fallback | a real VST3 loads, processes and restores state; a crashed scanner does not take the host down | ✅ **done** — verified by a real bundle built from this repository on **both** platforms: Linux (`Linux VST 3 hosting`) and Windows (`Debug-VST3`), 143/143 tests each, in CI on every push. The scanner runs bundles in `aura_scan_host`, kills what does not finish and quarantines it *with the reason*. Carried forward rather than pretended: the editor *window* needs the shell (M9/M10 — a generic parameter list is the fallback until then), the macOS fixture layout, and a Windows job object for the scanner |
| **M7** | Plug-in sandbox + CLAP | out-of-process audio host, CLAP adapter, latency re-reporting through the graph | a plug-in crash is survivable; CLAP plug-ins process | 🔨 **in progress — phase 3 of 7 landed.** Design decided and written down ([`research/PLUGIN_SANDBOX_RESEARCH.md`](research/PLUGIN_SANDBOX_RESEARCH.md)): per-plug-in policy with a shared sandbox by default, a shared-memory one-block-deferred transport so the audio thread never waits, a crash protocol built on the last state blob, and a single exported symbol (`clap_entry`) for the CLAP half. **Done, phase 1:** `SandboxArena` + `SharedAudioRing`, 15 tests including a 60 000-block two-thread stress run and a measured zero-allocation claim. **Done, phase 2:** `SharedMemory`, `ProcessSupervisor`, `SandboxProcessor` + a deterministic reference processor, and `aura_plugin_host` — the helper itself, whose real-time loop `rt_audit.py` scans; 200 blocks cross a process boundary and come back bit-exact. **Done, phase 3:** `SandboxSession` + `SandboxedPluginInstance` — a `PluginInstance` whose audio happens in another process, which a real `PluginChain` cannot distinguish from an in-process slot (latency sums, state round-trips, bypass and failed-slot skipping all work through the chain's own API); 8 more tests. **Not done, and the reason it matters:** nothing *chooses* a sandboxed slot yet — `PluginHost` still creates in-process adapters, so a crashing plug-in still crashes AURA. That is phase 5's policy plumbing, with phase 4's crash protocol and watchdog in between. **Next:** phase 4 — a crash/hang fixture, a block-counting watchdog, and a restart from the last state blob |
| **M8** | ASIO + device hardening | ASIO backend (GPLv3 SDK path), device matrix testing, sample-rate-change recovery, hot-plug UX | ASIO device at ≤ 64-frame buffers without xruns on the reference machine | ⏳ |
| **M9** | Desktop shell | Win32 + Direct2D window, widget toolkit, theme, DPI awareness, transport + track headers + mixer, preferences, device panel | opens, plays, records, saves a project; idle CPU < 2 % | ⏳ |
| **M10** | Arrangement & piano roll | arrangement canvas from the peak cache, clip editing gestures, piano roll, browser/inspector, dialogs, shortcut manager, accessibility pass | editing, recording and exporting done entirely in the UI | ⏳ |
| **M11** | Packaging & release | Inno Setup installer, portable ZIP, checksums, GPL source tarball, release workflow; installer/update story per `research/PACKAGING_RESEARCH.md` | signed (or checksummed) artefacts attached to a tagged release | ⏳ |
| **M12** | v1.0 polish | localisation (Sinhala/Tamil/German/Japanese catalogues), crash-report flow, docs pass, performance soak, plugin compatibility fixes | documented release criteria met; no known data-loss defects | ⏳ |

## Carried forward from M6

1. **The editor window.** `PluginInstance::createEditor()` hands back the plug-in's
   own view handle; embedding it needs the shell (M9/M10). Until then the browser
   shows a generic parameter list, which is why the milestone note calls it a
   fallback rather than a feature.
2. **The macOS fixture layout**, if a macOS build ever becomes a target. The adapter
   is platform-neutral; nothing builds the fixture there today, so configure warns.
3. **A Windows job object for the scanner.** A plug-in that spawns its own children
   is not cleaned up with the helper. POSIX gets a process group. M7 phase 6 adds
   `CreateJobObject` + `KILL_ON_JOB_CLOSE` for the *sandbox* helper, where the
   supervisor already kills the group on POSIX and `TerminateProcess` is the only
   verb Windows offers; the scanner's one-shot helper is worth folding into the same
   mechanism rather than growing a second one.
4. **Sidechain and aux buses are wired to silence**, not routed: the graph needs a
   sidechain send before a compressor's detector means anything (M7/M8).
5. **Sample-accurate parameter changes.** Block-aligned today; the format supports
   per-sample points and the transfer ring is already in place.
6. **A plug-in that misreports latency** needs the manual override the M5 research
   called for.
7. **A bundle that crashes on Windows can be reported as a timeout** rather than a
   crash, because the process may still be alive inside Windows Error Reporting.
   Both outcomes quarantine the bundle, so nothing is lost; a scanner process that
   opts out of crash reporting would make the report exact.

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
