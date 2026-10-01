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

`ctest` runs **95 entries**: 94 Catch2 test cases (2687 assertions) plus one
static-analysis test. Current status: all passing.

## The five layers

### 1. Unit tests

Question answered: *does this component behave the way its header says?*

| Area | File | Highlights |
|------|------|-----------|
| Core | `tests/core/CoreTests.cpp` | `AuraResult` value/error paths, error-code catalogue, logging ring, JSON round trip, atomic file write, SPSC ring, two-thread producer/consumer, localisation fallback |
| DSP | `tests/dsp/DspTests.cpp` | biquad vs. analytic magnitude, EQ band isolation, compressor knee, limiter ceiling, delay time, reverb finiteness/decay, distortion spectrum, oscillator frequency, envelope stages, level meter peak/RMS, loudness vs. BS.1770, smoothing ramps |
| Time | `tests/time/TimeTests.cpp` | 960 PPQN, tempo → sample math, tempo change mid-song, BBT round trip, time signatures, SMPTE including 29.97 drop-frame |
| Graph | `tests/graph/GraphTests.cpp` | destination clearing, topological order, cycle reporting, fader ramping, insert chain + bypass, memory stream, disk stream, `readWithSilence` |
| Audio | `tests/audio/AudioTests.cpp` | WAV 16/24/32-bit and float round trips, clip windows, split/trim/slip/normalise, mixer solo rules, insert latency, peak pyramid, take recording and interrupted-take recovery |
| MIDI | `tests/midi/MidiTests.cpp` | message payloads, running status, quantise strength, sort/range, event windows, recorded-note baking, routing + panic, instrument lifecycle, voice stealing |
| Project | `tests/project/ProjectTests.cpp` | `.aura` round trip, save-as collision, newer-format refusal, `.bak` recovery, media relink, autosave |
| Commands | `tests/commands/CommandsTests.cpp` | registry and ids, engine-action hook, search ranking, undo/redo of clip state, transaction semantics, redo-branch invalidation, bounded history |
| Automation | `tests/automation/AutomationTests.cpp` | lane interpolation and curve shapes, `sampleInto`, recorder thinning, target ranges (linear/log) |
| Timeline | `tests/timeline/TimelineTests.cpp` | every snap mode including markers and zero crossings, tempo-aware grid, view-state zoom limits, marker navigation |
| Plugin | `tests/plugin/PluginTests.cpp` | database persistence/search/favourites/blacklist, chain order/bypass/latency, failed-instance skip, state blobs, VST3 → `NotImplemented` when unbuilt |

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

## Running the layers you care about

| Goal | Command |
|------|---------|
| Everything | `ctest --output-on-failure` |
| Fast feedback while editing DSP | `./tests/aura_tests "[dsp]"` |
| Anything touching the engine | `./tests/aura_tests "[engine]"` |
| RT safety only | `ctest -R realtime_audit` |
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
| Stress: long sessions, 500+ clips, 8-hour recordings, memory growth over hours | M5 |
| Fuzzing: manifest parser, WAV chunk parser | M5 |
| MSVC-sanitizer run in CI | M5 |
| GUI/interaction tests (they do not exist because the shell does not exist) | M9 |
| Real audio hardware in CI (no sound card on runners) | never — mitigated by the mock device and by manual device-matrix testing |
| Accessibility automation (contrast, focus order, screen-reader smoke test) | M10, tracked in `research/ACCESSIBILITY_LOCALIZATION_RESEARCH.md` |
| Performance gates (reported, not enforced) | M5, needs a pinned machine |
