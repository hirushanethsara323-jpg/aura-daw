# PERFORMANCE_RESEARCH.md

**Status:** Accepted (targets; measurement lands with `bench/` in M5) · **Code:** `bench/`, `aura::engine::EngineStatistics`

## Question

A DAW's performance is not "fast"; it is a set of budgets that must hold on the
worst machine the user owns, in the worst moment of a session. Which budgets does
AURA adopt, and how does it measure them?

## The numbers

| Budget | Target | Rationale |
|--------|--------|-----------|
| **Round-trip latency** (reported, not hoped) | device latency + limiter look-ahead, exposed as `AudioEngine::totalLatencySamples()` | A DAW that under-reports latency makes every recording late |
| Audio callback budget | ≤ 50 % of the block period at 64 voices on a 2-core laptop CPU | 50 % leaves headroom for the OS, the GPU-less UI and plug-ins |
| CPU load reporting | a percentage of the block budget, per block, with peak-hold | Users need to see *why* it crackles |
| Xrun (underrun) reporting | counter + "since last reset" | A hidden dropout is a bug report we can never reproduce |
| UI frame time | ≤ 2 ms at 1080p with a 24-track arrangement; ≤ 8 ms with 8 tracks visible and meters running | 60 Hz needs 16.6 ms; the UI thread must never be the reason the audio thread misses |
| Idle cost | < 2 % CPU with a 24-track session open and stopped | The brief: no idle burn |
| Memory | < 400 MB with a 24-track, 2-hour project (excluding decoded media, which streams) | Reasonable on an 8 GB machine |
| Disk streaming | 2-hour stereo 24-bit stream with zero xruns at 512-sample buffers on a 5400 rpm disk | Streaming must work on a spinning disk, not just NVMe |
| Peak cache build | a 1-hour stereo file at 48 kHz in < 20 s on one core, cancellable, progressive | Background work must not feel like a freeze |
| Export speed | ≥ 20× real time for a 24-track session with the stock processors | Users export constantly |
| Project load | < 300 ms for a 24-track project (without media decode) | Perceived as instant |

## How each one is kept

1. **Measure before optimising.** `bench/` holds micro-benchmarks (biquad, delay,
   reverb, meter, graph traversal, clip render) and a session-scale benchmark
   (N tracks × M clips). CI *reports* them; gates require a pinned machine.
2. **The graph skips what it can.** Nodes with no input and no tail are cleared and
   skipped entirely — which is why `Processor::hasTail()` exists. A 24-track
   session with 4 tracks playing costs roughly 4 tracks, not 24.
3. **No allocation on the audio path** (measured, `EngineTests`): allocation is not
   just slow, it takes the allocator lock and can page-fault — the worst-case time
   is what matters.
4. **Denormal flushing** in every recursive structure; an unfushed reverb tail can
   cost 10–100× a normal sample on x86.
5. **Planar, contiguous, cache-friendly DSP.** `float*` per channel, block-based,
   in-place where possible; no per-sample virtual dispatch beyond the processor
   boundary.
6. **One conversion per boundary.** Float internal, integer at the file edge (see
   `AUDIO_FILE_FORMAT_RESEARCH.md`) — resampling or converting in the inner loop is
   the classic "why is my mixer at 40 % CPU" answer.
7. **Metering at UI rate.** Peak/RMS/LUFS are computed on the audio thread but
   *published* through atomics; the UI samples them at 30 Hz. Nothing blocks.
8. **`/fp:precise` (MSVC) and `-ffp-contract=off` (GCC/Clang)** — deliberately *not*
   `/fp:fast`: reassociation changes DSP results, breaks analytic tests, and buys
   little at our scale.

## Where SIMD would go (and where it would not)

| Candidate | Worth it? |
|-----------|-----------|
| Gain/pan/mix/sanitize/level metering loops | **Yes** — trivially vectorisable, 2–4× on these, and they run on every block |
| Biquad (IIR) | Mostly no — the recursion is sequential; vectorise across *channels* or *bands* instead |
| Reverb combs/all-passes | No — same recursion problem, and it is not the hot spot |
| Convolution | n/a — AURA uses algorithmic reverb, no FFT engine yet |
| Resampling | n/a — not implemented |

Decision: measure first; if the profile says gain/mix metering is > 20 % of the
callback, add SSE2 (baseline on x64, so no runtime dispatch needed) for those
loops only. AVX2 dispatch is a later, opt-in refinement because it needs
`__cpuid` gating and a fallback path.

## Measurement plan

* `bench/` micro-benchmarks with deterministic inputs and repeated runs
  (median + 95th percentile, not mean).
* A session-scale benchmark that builds N tracks with synthetic clips and reports
  callback time distribution at 64/128/256/512 buffers.
* `EngineStatistics::cpuLoad` / `peakCpuLoad` / `xruns` surfaced in the diagnostics
  panel, so a user's report comes with numbers.
* On a "modest PC" profile (2 cores, 8 GB, integrated GPU, 5400 rpm disk) before
  each milestone is called done.

## Accepted costs

* `/fp:precise` costs some floating-point performance in exchange for reproducible,
  testable results. We take that trade deliberately.
* Skipping silent nodes means the graph has to be *correct* about activation
  (`hasTail`, `inputActive`) — a bug there is a silent-audio bug, so the rules are
  unit-tested.
* Reporting latency honestly (including the limiter) makes our number *look* worse
  than DAWs that report only the device latency. It also makes recorded audio line
  up.
