# TESTING.md

How AURA is tested, what each layer is for, and what is still uncovered. The
research behind these choices (framework comparison, why five layers) is in
[`research/TESTING_RESEARCH.md`](research/TESTING_RESEARCH.md).

## Quick start

```bash
cmake -S . -B build -DAURA_BUILD_TESTS=ON
cmake --build build -j 2
cd build && ctest --output-on-failure          # everything
./tests/aura_tests "[dsp]"                     # one tag
./tests/aura_tests "Delay produces*"           # one case
python3 tools/rt_audit.py .                    # the static RT audit, by hand
```

`ctest` runs **111 entries**: 109 Catch2 test cases plus two static-analysis tests
(`rt_audit` and `include_audit`). Current status: all passing.

## The layers

### 1. Unit tests

Question answered: *does this component behave the way its header says?*

| Area | File | Highlights |
|------|------|-----------|
| Core | `tests/core/CoreTests.cpp` | `AuraResult` value/error paths, error-code catalogue, logging ring, JSON round trip, atomic file write, SPSC ring, two-thread producer/consumer, localisation fallback |
| DSP | `tests/dsp/DspTests.cpp` | biquad vs. analytic magnitude, EQ band isolation, compressor knee, limiter ceiling, delay time, reverb finiteness/decay, distortion spectrum, oscillator frequency, envelope stages, level meter peak/RMS, loudness vs. BS.1770, smoothing ramps |
| DSP | `tests/dsp/BlockStateTests.cpp` | block-state unrolling: the closed-form recursion vs. the per-sample one over odd block sizes and long runs, the ramp's monotonicity and landing behaviour, block-size independence of the meter reading |
| Time | `tests/time/TimeTests.cpp` | 960 PPQN, tempo → sample math, tempo change mid-song, BBT round trip, time signatures, SMPTE including 29.97 drop-frame |
| Graph | `tests/graph/GraphTests.cpp` | destination clearing, topological order, cycle reporting, fader ramping, insert chain + bypass, memory stream, disk stream, `readWithSilence` |
| Graph | `tests/graph/DelayCompensationTests.cpp` | delay compensation measured by where impulses *land*: aligned vs. misaligned arrivals, send vs. direct out, longest-path arithmetic, bypass, long-run ring stability, insert preparation, and the same property end-to-end through `AudioEngine` |
| Audio | `tests/audio/AudioTests.cpp` | WAV 16/24/32-bit and float round trips, clip windows, split/trim/slip/normalise, mixer solo rules, insert latency, peak pyramid, take recording and interrupted-take recovery |
| MIDI | `tests/midi/MidiTests.cpp` | message payloads, running status, quantise strength, sort/range, event windows, recorded-note baking, routing + panic, instrument lifecycle, voice stealing |
| Project | `tests/project/ProjectTests.cpp` | `.aura` round trip, save-as collision, newer-format refusal, `.bak` recovery, media relink, autosave |
| Commands | `tests/commands/CommandsTests.cpp` | registry and ids, engine-action hook, search ranking, undo/redo of clip state, transaction semantics, redo-branch invalidation, bounded history |
| Automation | `tests/automation/AutomationTests.cpp` | lane interpolation and curve shapes, `sampleInto`, recorder thinning, target ranges (linear/log) |
| Timeline | `tests/timeline/TimelineTests.cpp` | every snap mode including markers and zero crossings, tempo-aware grid, view-state zoom limits, marker navigation |
| Plugin | `tests/plugin/PluginTests.cpp` | database persistence/search/favourites/blacklist, chain order/bypass/latency, failed-instance skip, state blobs, and the honest "not compiled in" contract for formats this build does not host |
| Plugin (VST 3) | `tests/plugin/Vst3HostingTests.cpp`, `tests/plugin/vst3/AuraTestPlugin.cpp` | only in a `-DAURA_ENABLE_VST3=ON` build: a real bundle is scanned, loaded, processed (parameters reach `ProcessData`, the audio obeys the reported latency), notes are delivered, state round-trips, and a bundle that hangs or crashes is quarantined by the child-process scanner |
| Child process | `tests/plugin/ChildProcessTests.cpp` | the supervisor the scanner stands on: timeouts, crash reporting, missing executables, captured output |

### 2. Integration tests

Question answered: *do the components work together through the real engine?*

`tests/engine/EngineTests.cpp` drives the whole chain with a deterministic
`TestDevice` (no threads, no timing) and once for real with
`MockAudioDevice` (a real thread with real timing):

* project audio renders through the graph and reaches the device buffers,
* track fader, pan and mute reach the graph (ramped, so measurements wait for the
  10 ms ramp),
* a stopped transport actually falls silent after the limiter's look-ahead drains,
* reported latency = device latency + insert chain + master limiter,
* the **offline renderer reuses the live graph** (the explicit product
  requirement) and produces a valid 24-bit WAV whose peak matches the live path,
* stems render one file per track.

### 3. Regression tests

Question answered: *is the bug we fixed last week still fixed?*

Each of these exists because it actually failed:

| Test | Bug it pins down |
|------|------------------|
| "Disk streams read a real file from a worker thread" | the reader thread rewound to frame 0 after every chunk, so disk streaming never produced audio |
| "The engine renders project audio through the graph" | a stopped transport kept replaying the last block (stale source buffers) |
| "A failed plug-in is skipped instead of being called again" | a failed instance was called anyway, repeatedly |
| "WAV files survive a full write/read round trip" (chunked) | `readFileToPlanar` passed base pointers per chunk and overwrote earlier chunks |
| "Level meter reports peak and RMS" | the RMS one-pole advanced once per block instead of once per sample |
| "Snapping to markers picks the nearest marker" | marker ticks were converted at 60 BPM regardless of tempo |
| "Zero-crossing snapping finds a click-free edit point" | a sample at ~1e-16 read as "not a crossing", so the search walked past the point |
| "Delay produces the requested delay time" | smoothers ramped in from zero and swallowed the first echoes |
| "A node keeps every destination it is connected to (fan-out)" | `GraphBuilder::connect()` *replaced* a node's destination list, so attaching a send deleted the track's own output: the dry signal left the mix (measured: master peak 0.0001 where 0.5 was expected) |
| "A track with a send still plays through its own output" | the same bug seen from the mix: the track went silent once a send was dialled in |
| "The fader is applied after the inserts, so it cannot re-voice them" | the fader ran *before* the insert chain, so halving it did not halve the output (measured 0.0397 where 0.0223 was expected) because the compressor saw a quieter signal |
| "A pre-fader edge reads the tap, not the fader" | a pre-fader send has to read the signal after the inserts but before the fader; both wrong tap points are caught by exact sums (0.1875 correct, 0.5625 pre-insert, 0.125 post-fader) |
| "A pre-fader send ignores the fader and the mute" | the cue-mix property, measured on the engine: post-fader follows the fader and dies with a mute, pre-fader is untouched by either |
| "A note held across a window boundary gets exactly one note-off" | `collectEvents()` had two conditions emitting the same note-off, so a held note was turned off twice — and the stale off collides with a re-trigger of that pitch |
| "A replaced plan outlives every block that could still use it" | the retired-plan grace window asked whether the engine had processed two blocks *in its lifetime*, so a rebuild freed the plan the audio thread was still rendering with (use-after-free on a device change or plug-in scan) |
| "Offline render is sample-identical to the live path" | export could quietly grow its own half of the signal chain; peaks agreed while samples did not (the test compares 10 240 samples of a session with a reverb, a send and the master limiter: worst difference 0) |
| "A send plays at the level it is set to" | `Send::level` was stored, documented and never applied — every send played at unity |
| Real-time-safety audit | `std::stable_sort` (allocates) on the MIDI path, and a dead `std::vector` in the stream read path |

### 4. Audio-quality tests

Question answered: *is the signal right, not merely the arithmetic?*

These are the DSP assertions that measure a produced signal rather than an
intermediate value: EQ magnitude vs. the analytic response of the same designer
(≤ 0.5 dB), limiter peak ≤ ceiling with ≤ 0.05 dB overshoot, delay impulse at the
requested sample ±1, distortion second harmonic below −60 dB, reverb tail finite
and decaying, loudness within 1.2 LU of the BS.1770 expectation, RMS of a 0.5 sine
= 0.3536.

Rule: if a property has a closed form, assert against the closed form; if it does
not, assert the property that matters (bounded, monotone, below a threshold) — and
write the reasoning into the test as a comment, because a future reader will
otherwise "fix" the tolerance.

### 5. Static gates and real-time safety

Question answered: *is the audio thread still allowed to be an audio thread, and
does the tree still build for the compiler that ships?*

Three mechanisms:

1. **Static, audio path** — `tools/rt_audit.py` (ctest
   `aura.static.realtime_audit`) scans the functions reachable from the callback
   for allocation, container growth, locks, file I/O, exceptions and logging.
   Heuristic by design; escapes require a written reason on the line
   (`// rt-audit: allow - <why>`) and are printed in the output, so an escape is
   visible.
2. **Dynamic** — `EngineTests` arms the allocation tracker
   (`rt::setTrackingEnabled` + `rt::resetRealtimeAllocationCount`) and renders a
   block through the engine, asserting `rt::realtimeAllocationCount() == 0`.
3. **Static, portability** — two gates. `tools/include_audit.py` (ctest
   `aura.static.include_audit`) checks that every file asks for the standard
   headers it uses, resolving the *transitive project* include closure first, and
   that no `#include` sits inside a namespace.
   `tools/win_crosscheck.sh` (CI job "Windows cross-check") compiles every
   Windows-only translation unit and every public header with MinGW-w64, so the
   MSVC-only code is type-checked on every push without a Windows runner. This
   exists because libstdc++ hands `std::mutex`, `std::thread`, `std::chrono` and
   friends out through unrelated headers, so a Linux build cannot tell you whether
   MSVC will accept the file — and MSVC rejected exactly that in the second CI run
   (`error C2039: 'mutex': is not a member of 'std'`). An escape carries a reason:
   `#include <vector>   // include-audit: allow <chrono> - arrives via Time.hpp`.
4. **Dynamic, on the shipping compiler** — CI job *Windows Debug-ASan*
   (`-DAURA_ENABLE_SANITIZERS=ON`, `windows-latest`). GCC/Clang are sanitized on
   Linux, but the MSVC code paths — WASAPI, the COM plumbing, `/Zc:preprocessor`
   behaviour — only ever execute on a Windows runner, so an address sanitizer that
   is never pointed at them is not evidence about them. Three configuration facts
   are load-bearing and each fails quietly if ignored: MSVC's ASan runtime is a
   DLL, so the build switches to the **dynamic** CRT (`MultiThreadedDebugDLL`) and
   drops `/RTC1`; the flags are **directory-scoped** rather than per-target,
   because the linker refuses to mix instrumented and uninstrumented objects
   (`error LNK2038: mismatch detected for 'annotate_string'`: value `0` doesn't
   match value `1` — Catch2 lands in the same binary, so it is built under the same
   rules); and the runtime DLL is not copied next to the test binary, so the job
   puts the compiler's bin directory on `PATH` before running ctest.
   `detect_leaks=0`: leak detection is the soak's job (measured over hours against a
   real session), and a report per test process would bury the memory errors this
   job exists to find.

### 6. Fuzzing

Question answered: *does the parser survive input that was written by an attacker,
a truncated download or a half-flushed disk?*

Three libFuzzer targets cover every byte range that arrives from outside the
process — the project JSON reader, the `.aura` manifest reader and the WAV reader
(`fuzz/fuzz_json.cpp`, `fuzz_manifest.cpp`, `fuzz_wav.cpp`). They call the **real**
entry points (`json::parse`, `Project::load`, `media::probeFile`,
`AudioFileReader::open`, `readFileToPlanar`); there is no second implementation to
drift. The invariants they assert are the ones a DAW actually cares about:
arbitrary bytes produce an error or a well-formed result whose claims match the
data, serialisation is deterministic, a round trip re-parses, and a reader that
promises N channels × M frames can hand over exactly that many without reading
outside the file.

Two halves, on purpose:

| Half | Where | Compiler | Runs |
|------|-------|----------|------|
| libFuzzer targets | `fuzz/`, CI job *Fuzzing* | Clang (`-fsanitize=fuzzer,address,undefined`) | bounded `-max_total_time=90` per target on every push |
| Portable property tests | `tests/core/FuzzSmokeTests.cpp`, tag `[fuzz]` | any (MSVC included) | every `ctest` run, from the same committed seeds |

The committed corpora (`fuzz/corpus/{json,manifest,wav}`) are the curated seed
minimum: every run starts from inputs that were already known to be interesting
instead of from noise. A longer campaign is a local command, not a CI step:

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DAURA_BUILD_FUZZERS=ON \
      -DAURA_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz -j
mkdir -p /tmp/corpus && cp -r fuzz/corpus/* /tmp/corpus/     # keep the tree clean:
                                                             # libFuzzer writes to the corpus
./build-fuzz/fuzz/aura_fuzz_wav /tmp/corpus/wav -max_total_time=3600 -artifact_prefix=/tmp/
```

### Campaign results (2026-10-01)

A local campaign of the three targets under `-fsanitize=fuzzer,address,undefined`
(Clang 19, 2-core runner), each seeded from the committed corpus:

| Target | Runs | Wall time | Findings |
|--------|-----:|----------:|---------:|
| `aura_fuzz_json` | 1 877 224 | 151 s | 0 |
| `aura_fuzz_manifest` | 1 222 820 | 151 s | 0 |
| `aura_fuzz_wav` | 2 954 294 | 241 s | 0 |

That is ~6 million executions with no crash, no timeout, no leak report and no
sanitizer diagnostic — the parsers hold up on input nobody wrote on purpose. It is
evidence, not a proof: the corpora grew by ~2 000 units during the run (those live
in the build tree, not in the repository), and a longer campaign on faster hardware
would explore more. The WAV seeds in `fuzz/corpus/wav` are the ones this run started
from — a valid file per format (8/16/24-bit PCM, 32-bit float), plus the shapes that
break naive chunk walking: an unknown chunk before `data`, a zero-length `data`
chunk, a size field that lies about the file length, a truncated header and a chunk
that claims to run past the end.

A finding arrives as `crash-<sha1>` / `timeout-<sha1>` / `leak-<sha1>` next to the
artifact prefix. Reproduce it with the same binary and the file as its argument,
then add the file to the corpus and a named test to `FuzzSmokeTests.cpp` — a crash
that is not turned into a deterministic test is a bug that will come back. The
first real finding was exactly that: a WAV whose header claimed ~5.7 GB per
channel from a 100-frame file, which made `readFileToPlanar()` size its buffers
from the claim alone (fixed in `src/audio/AudioFile.cpp`: the data chunk is clamped
to the file size, `AudioFileInfo::truncated` is set, and the sample rate must be
plausible; the regression test pins both).

### 7. Soak (memory growth)

Question answered: *does a session that runs for hours stay the same size?*

`bench/aura_soak` (issue #4) drives the **real** engine through the real device
callback — clips streaming, EQ + compressor per track, a reverb aux bus, periodic
`rebuildGraph()`, optional recording takes that open and close files — and samples
resident memory. The first `max(30 s, 10 %)` is warm-up and is thrown away; the
report compares the mean RSS of the first half of the run against the second half
and converts the difference to **MB/hour**, which is the only unit in which a slow
leak is visible (1 MB/hour is ~3 KB/s — invisible in any single-minute view).

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DAURA_BUILD_TESTS=OFF \
      -DAURA_BUILD_BENCHMARKS=ON
cmake --build build-bench -j 2 --target aura_soak
./build-bench/bench/aura_soak --minutes 480 --tracks 16 --record \
    --rebuild-every 120 --interval 60 --max-growth-mb-per-hour 2 --json soak.json
```

Exit codes: `0` pass, `1` the growth trend exceeded `--max-growth-mb-per-hour`,
`2` bad arguments — so it works as a gate, not only as a report. CI runs two
minutes of it in the Linux Debug job.

The numbers from a twenty-minute rehabilitation run (12 tracks, recording on,
inserts loaded) — `--minutes 20 --record --tracks 12`, exit code 0:

| Blocks | Audio processed | Graph rebuilds | Takes | RSS min/max/final | Growth (limit 8 MB/h) |
|--------|-----------------|----------------|-------|-------------------|-----------------------|
| 95 837 769 | ≈ 5.9 days | 2 395 | 60 × 30 s (494 MB written, 8.2 MB peak, then deleted) | 30 / 31 / 31 MB | **+0.00 MB/hour** |

The 8-hour acceptance campaign is still outstanding; [`PERFORMANCE.md`](PERFORMANCE.md)
carries the full table and labels that row as a plan rather than a measurement.

## Running the layers you care about

| Goal | Command |
|------|---------|
| Everything | `ctest --output-on-failure` |
| Fast feedback while editing DSP | `./tests/aura_tests "[dsp]"` |
| Anything touching the engine | `./tests/aura_tests "[engine]"` |
| RT safety only | `ctest -R realtime_audit` |
| Fuzzing, no Clang needed | `./tests/aura_tests "[fuzz]"` |
| Address sanitizer locally | `cmake -S . -B build-asan -DAURA_ENABLE_SANITIZERS=ON -DAURA_BUILD_TESTS=ON && ctest --test-dir build-asan` |
| Untrusted-input parsing under a real fuzzer | CI job *Fuzzing*, or the local campaign above |
| Investigate a failure | `./tests/aura_tests "case name" -s` |

## Conventions

* **Tags** describe layers/areas: `[dsp]`, `[engine]`, `[rt]`, `[project]`,
  `[regression]`, `[io]`, `[export]`. Add tags, don't add hierarchies.
* **Deterministic by default.** Fixed random seeds, no wall-clock dependence, no
  hardware. The one test that needs real threads (`MockAudioDevice`, the SPSC ring)
  uses generous timeouts and retries rather than sleeps.
* **Test writes go to `/tmp`** (or `%TEMP%` on Windows) with unique names, so tests
  can run in parallel without colliding.
* **A failing test is investigated before it is changed.** Twice this milestone the
  test was right and the library was wrong (RMS integrator, chunked file read).
  Only after proving the header's contract did the test get adjusted.
* **Tests never bend the library.** If a test asserts something the API cannot do,
  the test is wrong — or the API is missing a feature, in which case the *design* is
  discussed and the doc updated first.

## Not covered yet (honest gaps)

| Gap | Plan |
|-----|------|
| Stress: 500+ clips, an 8-hour recording in one take | M5 (clips scale is benchmarked; the single-take length is measured in `PERFORMANCE.md`) |
| Fuzzing | **done** — 3 libFuzzer targets in CI + portable `[fuzz]` tests |
| Memory growth over hours | **done** — `bench/aura_soak`, 2 minutes in CI, 8 hours on a workstation |
| MSVC-sanitizer run in CI | **done** — job *Windows Debug-ASan*; ASan on MSVC is x64-only, needs the VC++ runtime redistributable, and has no leak detector (the soak covers growth) |
| GUI/interaction tests (they do not exist because the shell does not exist) | M9 |
| Real audio hardware in CI (no sound card on runners) | never — mitigated by the mock device and by manual device-matrix testing |
| Accessibility automation (contrast, focus order, screen-reader smoke test) | M10, tracked in `research/ACCESSIBILITY_LOCALIZATION_RESEARCH.md` |
| Performance gates (reported, not enforced) | M5, needs a pinned machine |
