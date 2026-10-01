# DSP.md

The processors in `aura_dsp`, their parameters and their guarantees. The reasoning
behind each algorithm (and the alternatives rejected) is in
[`research/DSP_ALGORITHM_RESEARCH.md`](research/DSP_ALGORITHM_RESEARCH.md).

## Shared contract

```cpp
class Processor {
    virtual void prepare(double sampleRate, int maxBlockSize, int numChannels);
    virtual void reset() noexcept;
    virtual void process(AudioBlockView& block, const ProcessContext& context) noexcept;
    virtual std::string_view typeName() const noexcept;
    virtual bool isActive() const noexcept;   // "switched on / not bypassed"
    virtual bool hasTail() const noexcept;    // "would skipping this drop audio?"
    virtual int  latencySamples() const noexcept;
};
```

* `prepare()` allocates and configures; nothing else may.
* `process()` is RT-safe: no allocation, no locks, no I/O (`tools/rt_audit.py`).
* `hasTail()` is what lets the graph skip silent work without truncating a delay or
  reverb tail. Conflating it with `isActive()` is a bug we already paid for.
* Parameter changes are smoothed (10–20 ms for gains/mix; delay time is slower) and
  **snap** on the first block after `prepare()`/`setSettings()` so a freshly loaded
  effect does not fade in.
* Denormals are flushed in every recursive structure.

## Processors

| Processor | Key parameters | Latency | Notes |
|-----------|----------------|---------|-------|
| `GainProcessor` | gain (dB or linear), bypass | 0 | ramped, click-free (verified 1.00000 / 0.50120 / 0.25120) |
| `PanProcessor` | pan −1…+1, law | 0 | **balance** law: centre leaves both channels at unity |
| `Equalizer` | 5 bands (HP, low shelf, bell, high shelf, LP) × enable/freq/Q/gain | 0 | RBJ biquads, `double` coefficients, DF-I |
| `Compressor` | threshold, ratio, attack, release, knee, makeup, auto-makeup, sidechain | 0 | feed-forward; `setSidechainInput(block)` |
| `Limiter` | ceiling (dBFS), look-ahead (ms), release, bypass | look-ahead samples | sliding-max window + gain ramp; reports its latency; default −0.3 dBFS / 1.5 ms |
| `Delay` | time L/R (ms) **or** tempo sync (beats), feedback, cross-feed, mix, damping | 0 | linear-interpolated taps, ping-pong at `crossFeed = 1` |
| `Reverb` | size, decay, damping, pre-delay, mix, low/high cut, modulation, freeze | 0 | damped comb banks + all-passes, stereo spread, no IR assets |
| `Distortion` | drive, tone, mix, oversampling, bypass | oversampling latency | waveshaping with anti-aliasing |
| `Oscillator` | waveform, frequency, phase, level | 0 | phase-continuous, deterministic |
| `NoiseGenerator` | enabled, level, colour, seed | 0 | deterministic when seeded (tests rely on it) |
| `AdsrEnvelope` | attack/decay/sustain/release, curve (linear/exponential) | 0 | 1 V/oct style gate semantics for instruments |
| `LevelMeter` | — | 0 | peak with hold+decay (≈20 dB/s), RMS as a per-**sample** 300 ms one-pole, clip flags |
| `LoudnessMeter` | — | 0 | ITU-R BS.1770-4: K-weighting, 400 ms momentary, 3 s short-term, gated integrated + LRA |
| `LinearSmoothedValue` / `OnePoleSmoother` | ramp/target/snap | 0 | the primitives every parameter uses |
| `Biquad<MaxChannels>` | coefficients from `BiquadDesign` | 0 | `lowPass`, `highPass`, shelf, bell, `magnitudeDb()` for analytic comparison |

## What the tests assert (a sample)

| Property | Value |
|----------|-------|
| EQ magnitude vs. analytic | ≤ 0.5 dB |
| Limiter output peak | ≤ ceiling × 1.006 |
| Delay impulse position | requested sample ± 1 |
| Distortion 2nd harmonic | < −60 dB |
| Reverb tail | finite, decaying, no runaway feedback |
| Loudness of two −20 dBFS tones | −20.3 LUFS ± 1.2 (K-weighting + channel sum) |
| Envelope after release | reaches idle within 1 s (exponential curve) |
| RMS of a 0.5 sine | 0.3536 ± 0.02 |

## Measurement helpers in the tests

`tests/support/TestSignals.{hpp,cpp}` provides `fillSine`, `fillConstant`, `peak`,
`peakFrom`, `allFinite`, `magnitudeDbAtFrequency` (Goertzel-style single-frequency
magnitude) and a `Block` wrapper around `dsp::AudioBlockView`. Use them rather than
rolling a new measurement per test — the measurement being wrong is a classic way to
"prove" an incorrect DSP result.

## Adding a processor

See [`DEVELOPMENT.md`](DEVELOPMENT.md#adding-a-dsp-processor). In short: header,
source, CMake list entry, test written first, RT audit run, documented here.

## Known refinements

* Distortion oversampling factor is fixed at 4×; 8× is a measured decision, not a
  guess.
* Reverb modulation uses per-sample LFOs (cheap but not free) — block-rate LFOs are
  on the profiling list.
* The limiter's sliding-max window is evaluated per block; a fully sample-accurate
  window across block boundaries is tracked as a refinement.
