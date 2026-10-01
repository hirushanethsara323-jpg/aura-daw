# TESTING_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0004 · **Detail:** `../TESTING.md`

## Question

Which framework, and — more importantly — what counts as evidence that a DAW
engine works?

## Framework choice

| Framework | Licence | Compile cost | Features that matter to us | Verdict |
|-----------|---------|--------------|----------------------------|---------|
| **Catch2 v3** | BSL-1.0 | moderate; v3 compiles as a static library so header cost is gone | `SECTION`s, generators, matchers, `Approx`, rich failure output, `ctest` XML | **Accepted (3.7.1 via FetchContent)** |
| doctest | MIT | lowest (~20 ms header) | fast, similar macros, no matchers/benchmarks | Rejected — the measured Catch2-v3-as-library gap is small, and matchers/`Approx` earn their keep in DSP tests |
| GoogleTest | BSD-3 | highest (gmock adds ~2 s/file) | mocking, death tests | Rejected — mocking is not how we test DSP, and the build cost is real on 2-core CI |
| Boost.Test | BSL-1.0 | high | — | Rejected — the only reason to take Boost is Boost |

Catch2 is fetched, not vendored, with a documented offline path
(`AURA_FETCH_DEPS=OFF` plus a local checkout at `AURA_CATCH2_DIR`/`/tmp/catch2`),
so a machine without network can still build and test.

## The five layers (this is the real decision)

| Layer | Question it answers | Where it lives | Example |
|-------|--------------------|----------------|---------|
| **Unit** | does this component do what its header says? | `tests/{core,dsp,time,graph,audio,midi,project,commands,engine,automation,timeline,plugin}` | EQ magnitude vs. analytic response |
| **Integration** | do the components work *together* through the real engine? | `tests/engine`, `tests/graph` | a project renders through the graph; fader/pan/mute reach it |
| **Regression** | is the bug we just fixed still fixed? | named test cases everywhere | "Disk streams read a real file from a worker thread"; "a failed plug-in is skipped instead of being called again" |
| **Audio quality** | is the *signal* right, not just the code? | DSP tests measuring the output | delay impulse at the requested sample; limiter ≤ ceiling; loudness within 1.2 LU of BS.1770 |
| **Real-time safety** | is the audio thread still clean? | `aura.static.realtime_audit` + `EngineTests` allocation counter | `realtimeAllocationCount() == 0` across a rendered block |

Two rules that keep this honest:

1. **A regression test is written with the fix, not after it.** Every bug listed
   in `THREADING_RESEARCH.md` and in the project's history has a named test case
   in the tree.
2. **Tests may fix tests; they may not bend the library.** When a test disagrees
   with a header, the test is wrong until proven otherwise. Twice this milestone
   the disagreement was a *real* library bug (the RMS integrator updating per
   block instead of per sample; `readFileToPlanar` overwriting its earlier
   chunks) — which is precisely why the rule is "investigate", not "change the
   library to make the test pass".

## What the current suite covers (as of this milestone)

* **95 ctest entries, 2687 assertions, all green**: 94 Catch2 test cases plus the
  static RT audit.
* Audio: WAV round trips at 16/24/32-bit and float, chunked reads, clip ops,
  mixer solo rules, insert latency, waveform pyramid, recording recovery.
* Engine: graph rendering, fader/pan/mute, latency accounting, offline render
  reuse of the same graph, stems, allocation regression, mock-device real-time run.
* Project/commands: round trip, save-as collision, newer-format refusal, `.bak`
  recovery, media relink, autosave, localisation; undo/redo, transactions, bounded
  history, redo-branch invalidation.
* Plugin: database persistence/search/blacklist, chain order/bypass/latency, failed
  instance skip, state blobs, VST3 → `NotImplemented` when the SDK is absent.

## Not covered yet (stated, not hidden)

* **Stress**: long-running sessions, 500-clip arrangements, 8-hour recordings.
* **Fuzzing**: manifest parsing and WAV chunk parsing are the obvious targets.
* **Sanitizers on Windows**: ASAN/UBSAN run on the Linux job; MSVC ASAN is planned.
* **GUI**: no UI tests exist because the shell does not exist yet (M9).
* **Audio hardware**: no test can prove a driver behaves; the mock device proves
  the engine does.

## Sources

* Catch2 v3 documentation — `SECTION`, generators, matchers, `Approx`, ctest
  integration.
* pistack.xyz, *C++ unit testing frameworks compared* (2026) — compile-time
  benchmarks (doctest ~0.02 s/file, Catch2 ~0.22 s/file amalgamated, GoogleTest
  ~2.10 s/file with gmock).
* GitHub issue doctest#1168 — a large project measuring **no** compile-time win
  switching from Catch2 v3 (static library) to doctest, which is the data point
  that settled our choice.
* Ross Bencina, *Real-time audio programming 101*; Fabian Renn-Giles & Dave
  Rowland, *Real-time 101* — the basis for the real-time-safety layer.
