# PERFORMANCE.md

Budgets, how they are measured, and what has been done to meet them. The research
behind the numbers (and the SIMD decision) is in
[`research/PERFORMANCE_RESEARCH.md`](research/PERFORMANCE_RESEARCH.md).

## Budgets

| Budget | Target | Status |
|--------|--------|--------|
| Audio callback time | ≤ 50 % of the block period at 64 voices on a 2-core laptop CPU | measured by `bench/` (M5); the engine reports `cpuLoad`/`peakCpuLoad` live |
| Idle cost | < 2 % CPU with a 24-track session open and stopped | by design: silent, tail-less nodes are skipped and the UI is event-driven |
| UI frame | ≤ 2 ms at 1080p (24-track arrangement), ≤ 8 ms with meters running | M9 (shell) |
| Memory | < 400 MB with a 24-track 2-hour project, media streaming | M5 measurement |
| Disk streaming | 2-hour stereo 24-bit, zero xruns at 512 frames on a 5400 rpm disk | streaming implemented; measurement M5 |
| Peak-cache build | 1-hour stereo 48 kHz in < 20 s, cancellable, progressive | implemented progressively (`bucketsReady`) |
| Export speed | ≥ 20× real time for a 24-track session with stock processors | measured on the reference harness (≈ 360× for a single-track tone) |
| Project load | < 300 ms for 24 tracks (no media decode) | M5 measurement |
| RT safety | zero allocations per rendered block | **enforced today** (test + static audit) |

## What makes it fast (and why)

1. **Skip silent work.** A node with no input and no tail is cleared and skipped;
   `Processor::hasTail()` is the switch. A 24-track session with four playing tracks
   costs about four tracks.
2. **No allocation on the audio path.** Allocation takes the allocator lock and can
   fault; worst-case time is what matters, not the average. Enforced statically and
   dynamically.
3. **Denormal flushing** in every recursive structure — an unfushed reverb tail can
   cost 10–100× a normal sample.
4. **Planar float buffers, block processing, in-place where possible**; no
   per-sample virtual dispatch beyond the processor boundary.
5. **One conversion per boundary** — float internally, integer at the file edge; no
   resampling in the inner loop.
6. **Metering published through atomics**, sampled by the UI at ~30 Hz.
7. **`/fp:precise` and `-ffp-contract=off`**: reproducible DSP beats a few percent of
   floating-point throughput, and it keeps the analytic tests meaningful.

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
