# PERFORMANCE.md

Budgets, how they are measured, and what has been done to meet them. The research
behind the numbers (and the SIMD decision) is in
[`research/PERFORMANCE_RESEARCH.md`](research/PERFORMANCE_RESEARCH.md).

## Budgets

| Budget | Target | Status |
|--------|--------|--------|
| Audio callback time | ≤ 50 % of the block period at 64 voices on a 2-core laptop CPU | **measured 9.2 %** of a 64-frame block period (`dsp/instrument-64-voices/64`); the engine also reports `cpuLoad`/`peakCpuLoad` live |
| Idle cost | < 2 % CPU with a 24-track session open and stopped | by design: silent, tail-less nodes are skipped and the UI is event-driven |
| UI frame | ≤ 2 ms at 1080p (24-track arrangement), ≤ 8 ms with meters running | M9 (shell) |
| Memory | < 400 MB with a 24-track 2-hour project, media streaming | **measured during streaming**: 20 MB resident (8 tracks) and 37 MB (16 tracks, recording on) in the soak; media is streamed through a fixed window, so project *length* does not add resident memory. The explicit 24-track/2-hour run is still pending |
| Disk streaming | 2-hour stereo 24-bit, zero xruns at 512 frames on a 5400 rpm disk | streaming implemented; measurement M5 |
| Peak-cache build | 1-hour stereo 48 kHz in < 20 s, cancellable, progressive | implemented progressively (`bucketsReady`) |
| Session cost | a 16-track session with EQ + compressor on every track ≤ 50 % of the block period | **measured 7.8 %**; a 48-track session with the same inserts uses **23.1 %** (`bench/`) |
| Export speed | ≥ 20× real time for a 24-track session with stock processors | offline render reuses the same graph; the `engine/session-*` cases show 4.3×–129× realtime of *graph* cost, and export adds only the WAV writer |
| Project load | < 300 ms for 24 tracks (no media decode) | M5 measurement |
| RT safety | zero allocations per rendered block | **enforced today** (test + static audit) |

## Measured numbers (2026-10-01)

Machine: 2 vCPU Intel Xeon @ 2.60 GHz, 2 GB RAM, Linux 6.1 (cloud VM — a shared
machine, so treat the absolute figures as indicative and the ratios as the
signal). Compiler: GCC 14.2.0, `-O2`, Release (`NDEBUG`). 400 timed iterations
plus warm-up per case, median reported (`p95/median` shows how noisy the run was;
values near 1.0 mean the machine was quiet).

Reproduce:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DAURA_BUILD_TESTS=OFF \
      -DAURA_BUILD_BENCHMARKS=ON
cmake --build build-bench -j 2
./build-bench/bench/aura_bench --iterations 400 --json bench.json
python3 tools/bench_to_markdown.py bench.json --budgets     # budget verdicts
python3 tools/bench_to_markdown.py bench.json --group dsp   # any group as Markdown
```

### Budget check

| case | block | share of one core | budget | verdict |
|---|---|---|---|---|
| `dsp/instrument-64-voices/64` | 64 | 9.2 % | 50 % — 64 voices | **PASS** |
| `engine/session-16x8-fx/512` | 512 | 7.8 % | 50 % — one core | **PASS** |
| `engine/session-48x8-fx/256` | 256 | 23.1 % | 100 % — one core | **PASS** |

"Share of one core" is `ns/frame × blockSize ÷ block period`: the number the
audio thread's deadline actually constrains.

### DSP primitives (128-frame blocks)

| processor | ns/frame | x-realtime |
|---|---|---|
| Gain | 0.2 | 102 564× |
| Pan | 0.9 | 23 188× |
| Compressor (4:1, auto makeup) | 25.9 | 804× |
| Limiter (1.5 ms look-ahead) | 21.5 | 967× |
| Delay (tempo-synced, feedback) | 20.9 | 998× |
| Reverb (1.8 s decay) | 115.2 | 181× |
| Distortion (2× oversampled, 31-tap) | 292.3 | 71× |
| 5-band EQ (5 bands active) | 73.9 | 282× |

| case | block | ch | ns/frame | p95/median | x-realtime |
|---|---|---|---|---|---|
| `dsp/levelmeter/128` | 128 | 2 | 6.9 | 1.00 | 3027x |
| `dsp/levelmeter/512` | 512 | 2 | 7.0 | 1.00 | 2965x |
| `dsp/loudness/128` | 128 | 2 | 11.5 | 1.22 | 1815x |
| `dsp/loudness/512` | 512 | 2 | 13.0 | 1.11 | 1607x |

Interpretation: the EQ is the most expensive *per-sample* stock processor and the
distortion is the most expensive overall (the 2× oversampling filter costs ~4×
the waveshaper itself — measured, not guessed, and the reason its taps were
reduced from an idealised design to 31). Metering is nearly free: LUFS at
11.5 ns/frame means a master meter can stay on always.

### Strips and sessions

| case | block | ch | ns/frame | p95/median | x-realtime |
|---|---|---|---|---|---|
| `chain/master-strip/256` | 256 | 2 | 34.4 | 1.15 | 605x |
| `chain/track-strip/256` | 256 | 2 | 77.1 | 1.21 | 270x |

| case | block | ch | ns/frame | p95/median | x-realtime |
|---|---|---|---|---|---|
| `engine/session-16x8-fx/1024` | 1024 | 2 | 1551.5 | 1.05 | 13x |
| `engine/session-16x8-fx/128` | 128 | 2 | 1402.6 | 1.31 | 15x |
| `engine/session-16x8-fx/2048` | 2048 | 2 | 1498.7 | 1.04 | 14x |
| `engine/session-16x8-fx/256` | 256 | 2 | 1464.1 | 1.26 | 14x |
| `engine/session-16x8-fx/512` | 512 | 2 | 1623.8 | 1.13 | 13x |
| `engine/session-16x8-fx/64` | 64 | 2 | 1347.0 | 1.22 | 15x |
| `engine/session-16x8/1024` | 1024 | 2 | 161.1 | 1.16 | 129x |
| `engine/session-16x8/128` | 128 | 2 | 207.7 | 1.12 | 100x |
| `engine/session-16x8/2048` | 2048 | 2 | 158.7 | 1.14 | 131x |
| `engine/session-16x8/256` | 256 | 2 | 180.2 | 1.39 | 116x |
| `engine/session-16x8/512` | 512 | 2 | 169.4 | 1.70 | 123x |
| `engine/session-16x8/64` | 64 | 2 | 269.0 | 1.12 | 77x |
| `engine/session-1x1/1024` | 1024 | 2 | 48.6 | 1.28 | 429x |
| `engine/session-1x1/128` | 128 | 2 | 50.8 | 1.04 | 410x |
| `engine/session-1x1/2048` | 2048 | 2 | 48.7 | 1.23 | 428x |
| `engine/session-1x1/256` | 256 | 2 | 48.7 | 1.09 | 428x |
| `engine/session-1x1/512` | 512 | 2 | 48.5 | 1.12 | 429x |
| `engine/session-1x1/64` | 64 | 2 | 49.7 | 1.06 | 420x |
| `engine/session-48x8-fx/256` | 256 | 2 | 4814.6 | 1.04 | 4x |
| `engine/session-48x8/1024` | 1024 | 2 | 427.4 | 1.09 | 49x |
| `engine/session-48x8/128` | 128 | 2 | 831.0 | 1.20 | 25x |
| `engine/session-48x8/2048` | 2048 | 2 | 408.5 | 1.05 | 51x |
| `engine/session-48x8/256` | 256 | 2 | 586.9 | 1.17 | 35x |
| `engine/session-48x8/512` | 512 | 2 | 473.2 | 1.13 | 44x |
| `engine/session-48x8/64` | 64 | 2 | 1338.0 | 1.18 | 16x |

### Instruments (polyphony budget)

| case | block | ch | ns/frame | p95/median | x-realtime |
|---|---|---|---|---|---|
| `dsp/instrument-64-voices/128` | 128 | 2 | 1815.3 | 1.12 | 11x |
| `dsp/instrument-64-voices/256` | 256 | 2 | 1798.8 | 2.51 | 12x |
| `dsp/instrument-64-voices/64` | 64 | 2 | 1909.9 | 1.18 | 11x |

64 voices cost 9.2 % of a 64-frame block period, so the documented ≤ 50 % budget
holds with a factor of 5 in hand on this machine — the budget is stated for a
2-core laptop CPU, which is slower per core than this VM, so the margin is what
makes the budget honest rather than optimistic.

## What makes it fast (and why)

1. **Skip silent work.** A node with no input and no tail is cleared and skipped;
   `Processor::hasTail()` is the switch. A 24-track session with four playing tracks
   costs about four tracks.
2. **No allocation on the audio path.** Allocation takes the allocator lock and can
   fault; worst-case time is what matters, not the average. Enforced statically and
   dynamically.
3. **Denormal flushing** in every recursive structure — an unfushed reverb tail can
   cost 10–100× a normal sample. The same trap bit the gain ramp's closed form, which
   is why it snaps its residual to the target at 8 ULP instead of creeping through the
   subnormal range (SIMD_RESEARCH.md, Step 5).
4. **Planar float buffers, block processing, in-place where possible**; no
   per-sample virtual dispatch beyond the processor boundary.
5. **One conversion per boundary** — float internally, integer at the file edge; no
   resampling in the inner loop.
6. **Metering published through atomics**, sampled by the UI at ~30 Hz.
7. **`/fp:precise` and `-ffp-contract=off`**: reproducible DSP beats a few percent of
   floating-point throughput, and it keeps the analytic tests meaningful.

### Delay compensation costs nothing measurable (2026-10-01)

Delay compensation (per-edge delays on the paths that are early, ADR-0015) was
measured the way a regression should be: the same 66-case suite, before and after,
on the same machine, with the session cases compared pairwise.

| Case | Before (ns/frame) | After | Delta |
|------|-------------------|-------|-------|
| `session-1x1` | 48.7 | 48.8 | +0.2 % |
| `session-16x8` (256) | 180.2 | 180.1 | −0.1 % |
| `session-16x8-fx` (256) | 1464.1 | 1421.3 | −2.9 % |
| `session-48x8` (512) | 473.2 | 476.1 | +0.6 % |
| `session-48x8-fx` (256) | 4814.6 | 4802.8 | −0.2 % |

Every session case lands inside ±3 %, which is run-to-run noise on this machine
(the `session-1x1/256` case showed +60 % in the first after-run and 48.8 ns on
three immediate re-runs — a scheduling artefact of a shared 2-vCPU runner, not a
regression; the 48-track session, which has 48 compensated-capable edges, is
unchanged). The reason is structural: a session with no latent processor computes a
delay of 0 for every edge, which takes the plain summing path with one extra
`nodeId` comparison, and the per-edge lookup is a binary search over a table sorted
once in `prepare()` rather than a scan.

## Memory growth under load: the soak

A benchmark answers "how fast"; a soak answers "does it stay the same". They need
different harnesses, so `bench/aura_soak` is separate from `bench/aura_bench`.
It drives a real 16-track session through the real engine (clips streaming, EQ +
compressor on every track, a reverb on an aux bus fed by a send, the graph rebuilt
every two minutes, recording opening and closing takes) and samples resident memory
once a minute. Warm-up is excluded, the steady samples are split in half, and the
difference is reported as **MB/hour** — the unit in which a slow leak is visible.

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DAURA_BUILD_TESTS=OFF \
      -DAURA_BUILD_BENCHMARKS=ON
cmake --build build-bench -j 2 --target aura_soak
./build-bench/bench/aura_soak --minutes 480 --tracks 16 --record \
    --rebuild-every 120 --interval 60 --json bench/results/soak-8h.json
```

The harness drives a device that never sleeps, so it processes roughly **400× real
time** at 16 tracks: a one-minute run puts about six hours of audio through the
graph, and the twenty-minute run below put 5.9 days through it. (The factor scales
with track count and whether the insert chains are loaded, which is why the tables
below report wall time and audio time side by side rather than quoting a single
number.) That is deliberate — the paths that are allowed to allocate (graph rebuild,
plan publication, media open/close, take write) get exercised hundreds of thousands
of times instead of a handful — with one consequence worth knowing about: a take
bounded by wall-clock time would be gigabytes long, so takes are bounded by
**frames written** (`--take-seconds`, default 30 s of audio) with a hard
`--max-take-megabytes` guard and the files deleted after closing. The first
unattended run of this harness wrote 16 GB in five minutes and taught us that;
the fix is in the harness, and the guard is there so the mistake cannot recur
quietly.

| Run | Wall | Block | Tracks | Blocks | Audio processed | RSS (first half → second half) | Growth | Verdict |
|---|---|---|---|---|---|---|---|
| CI step (2 min) | 120 s | 256 | 8, no recording | 10 583 907 | 15.7 h | 20 → 20 MB | +0.00 MB/hour | **PASS** |
| Local soak, recording on | 60 s | 256 | 16 | 4 348 485 | 6.4 h | 37 → 37 MB | +0.00 MB/hour | **PASS** |
| ASAN build | 9 s | 256 | 4 | 13 281 | 0.02 h | 56 → 56 MB | +0.00 MB/hour | **PASS** (no ASAN/UBSAN finding) |
| Rehabilitation run, 2026-10-01 (`--record --tracks 12`) | 1 200 s | 256 | 12 | 95 837 769 | 5.9 days | 31 → 31 MB (min 30, max 31, final 31) | +0.00 MB/hour | **PASS** |
| Acceptance-flag run, 2026-10-01 (`--record --tracks 16 --rebuild-every 120`) | 2 280 s | 256 | 16 | 157 369 420 | 9.7 days | 37 → 37 MB (min 36, max 37, final 37) | +0.00 MB/hour | **PASS, interrupted at 38 of 480 minutes** (`"complete": false`) — flagged, counted, and 114 takes plus 1.3 M graph rebuilds into the run |
| **Acceptance-flag run, 2026-10-01, complete** | 3 600 s | 256 | 16 | 231 524 947 | 14.3 days | 37 → 37 MB (min 36, max 37, final 37) | **+0.00 MB/hour** | **PASS** (`"complete": true`) — 180 takes written and deleted, 1 929 374 graph rebuilds, per-take size guard never hit |
| 8-hour campaign | automated by `.github/workflows/soak.yml` | 256 | 16 | — | — | — | — | the longest run a hosted runner allows is 5 hours (6-hour job cap); the 8-hour figure is that same command, run longer |

The first two rows are the numbers the CI gate and a one-minute local run produce;
they are the *floor* of what is checked automatically.

The twenty-minute row is the run that followed the memory fixes below: 60 takes of
30 s each were written and deleted (494 MB in total, peak 8.2 MB per take, the
per-take size guard never triggered), the graph was rebuilt 2 395 times, and resident
memory moved by one megabyte across the whole run — the same megabyte the first half
saw. 95.8 M blocks is about six days of audio, which is the useful property of this
harness: a five-year-old laptop can put a multi-day session through the engine
faster than a coffee break.

The complete hour above is the run that the interrupted one became: the same flags, the
same 16-track session with recording on, restarted on a machine that stayed up. 231.5 M
blocks is 14.3 days of audio through the real engine - clips streamed, EQ and compressor
on every track, a reverb on an aux bus, 180 takes opened, written and closed, and the
graph rebuilt 1.9 M times - with resident memory starting and ending at 37 MB and never
leaving 36-37 MB. That is the criterion M5 asked for, measured rather than extrapolated.

The long campaign is the M5 acceptance run, and it is now **automated rather than
promised** (`.github/workflows/soak.yml`, monthly and on demand, Windows, the same
harness and the same flags). One honest difference is the clock: GitHub-hosted runners
cap a single job at six hours, so the scheduled campaign is five hours and the
eight-hour figure comes from running the identical command for longer — locally, or
on any machine that can be left alone for a day. What the shorter runs establish is
that a longer one is a scheduling question rather than a research question: the
acceptance-flag run above held a 37 MB footprint through 157.4 M blocks (9.7 days of
audio) with recording on and the graph rebuilt 1.3 M times, and it was stopped by its
environment, not by a failure. It is listed as interrupted, with `"complete": false`,
because that is what it is — and because an interrupted campaign that still leaves its
trend behind is the reason the JSON is written on every sample instead of at the end.

The campaign is the one measurement on this page that a contributor other than the
author can reproduce by pressing a button: **Actions → Soak → Run workflow**, with the
duration as an input. It also fails the job on trend, not on thresholds that only a
human reads: the harness exits non-zero above `--max-growth-mb-per-hour`, and the JSON
is uploaded as an artifact either way so a drift is visible before it crosses the
line.

The JSON is written with `"complete": false` after every sample and `"complete": true`
at the end, so an interrupted campaign still leaves the trend behind instead of leaving
nothing.

### What the soak found

Running it under AddressSanitizer (`-DAURA_ENABLE_SANITIZERS=ON`) immediately
produced a `heap-use-after-free` in `AudioEngine::shutdown() -> device_->stop()`:
the harness destroyed its device before the engine that was pointing at it. The
same shape existed in the application — `AudioDeviceManager::close()` did
`device_.reset()` and left the engine holding a dangling raw pointer, so an engine
shutdown or settings change after closing a device would have dereferenced freed
memory. The owners now call `AudioEngine::detachDevice()` before destroying, the
manager builds the new backend before retiring the old one, and
`tests/api/DeviceLifetimeTests.cpp` pins all of it. It is the clearest argument for
why M5 asked for a soak: no benchmark would ever have found it.

## Measuring it yourself

```bash
cmake -S . -B build -DAURA_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2 && ./build/bench/aura_bench
```

Report **median and 95th percentile**, never the mean: an audio thread fails on the
worst block, not on the average one. `bench/` covers biquad, delay, reverb, meter,
loudness, graph traversal (N nodes) and clip rendering.

Live numbers come from the engine: `EngineStatistics::cpuLoad`, `peakCpuLoad`,
`xruns`, plus per-stream `underrunCount()`. The diagnostics panel shows exactly these
values, so a user report arrives with data.

## Reference machine ("modest PC" profile)

2 cores, 8 GB RAM, integrated GPU, 5400 rpm disk, Windows 10 21H2. Every milestone is
checked against this profile before it is called done — a DAW that only performs on
the developer's machine is not shipping.

## Known costs we accept

* Honest latency reporting (device + insert chain + master limiter) makes AURA's
  number look worse than DAWs that report device latency only. Recorded audio lines
  up, which is the point.
* `/fp:precise` costs some throughput for reproducibility.
* Skipping silent nodes requires the activation flags to be *right*; the rules
  (`hasTail`, `inputActive`, per-block clear) are unit-tested because a bug there is
  a silent-audio bug.
* Reverb modulation uses per-sample LFOs and the limiter's sliding max is evaluated
  per block: both are on the profiling list, neither is a correctness problem.
