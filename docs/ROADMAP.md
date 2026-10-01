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
| **M5** | Performance & hardening | `bench/` suite, session-scale benchmark, sanitizer runs, fuzzing the manifest and WAV parsers, memory-growth soak, delay compensation for dry paths, SIMD decision | budgets in `PERFORMANCE.md` met on the reference profile; no ASAN/UBSAN findings | ⏳ next |
| **M6** | Plug-in hosting | VST3 adapter (`AURA_ENABLE_VST3`), out-of-process scanner with timeouts and blacklist, parameter and latency discovery, generic editor fallback | a real VST3 loads, processes and restores state; a crashed scanner does not take the host down | ⏳ |
| **M7** | Plug-in sandbox + CLAP | out-of-process audio host, CLAP adapter, latency re-reporting through the graph | a plug-in crash is survivable; CLAP plug-ins process | ⏳ |
| **M8** | ASIO + device hardening | ASIO backend (GPLv3 SDK path), device matrix testing, sample-rate-change recovery, hot-plug UX | ASIO device at ≤ 64-frame buffers without xruns on the reference machine | ⏳ |
| **M9** | Desktop shell | Win32 + Direct2D window, widget toolkit, theme, DPI awareness, transport + track headers + mixer, preferences, device panel | opens, plays, records, saves a project; idle CPU < 2 % | ⏳ |
| **M10** | Arrangement & piano roll | arrangement canvas from the peak cache, clip editing gestures, piano roll, browser/inspector, dialogs, shortcut manager, accessibility pass | editing, recording and exporting done entirely in the UI | ⏳ |
| **M11** | Packaging & release | Inno Setup installer, portable ZIP, checksums, GPL source tarball, release workflow; installer/update story per `research/PACKAGING_RESEARCH.md` | signed (or checksummed) artefacts attached to a tagged release | ⏳ |
| **M12** | v1.0 polish | localisation (Sinhala/Tamil/German/Japanese catalogues), crash-report flow, docs pass, performance soak, plugin compatibility fixes | documented release criteria met; no known data-loss defects | ⏳ |

## Near-term work queue (M5)

1. `bench/` micro-benchmarks + the session-scale benchmark; publish numbers in
   `PERFORMANCE.md`.
2. ASAN/UBSAN job findings triaged; MSVC ASAN attempt.
3. Fuzz the manifest parser (`project.json`) and the WAV chunk parser.
4. Memory-growth soak: 8 hours of simulated playback with recording on and off.
5. Delay compensation: insert the dry-path delays that `compensatedLatency_` is
   currently (correctly) reporting as zero.
6. SIMD decision, made from profile data rather than taste.

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
