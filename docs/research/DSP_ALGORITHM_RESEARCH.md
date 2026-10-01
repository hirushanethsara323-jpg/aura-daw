# DSP_ALGORITHM_RESEARCH.md

**Status:** Accepted · **Code:** `aura::dsp`, tested by `tests/dsp/DspTests.cpp`

## Question

Which algorithm does each processor use, why that one, and how do we know it is
correct?

## The processors

| Processor | Algorithm | Why this one | Test that proves it |
|-----------|-----------|--------------|---------------------|
| **Gain** | ramped linear gain, dB interface | trivial, but must never click | ramp measured across blocks (`gain_probe`: 1.00000 / 0.50120 / 0.25120) |
| **Pan** | **stereo balance law** (±1 moves one channel, centre untouched) | an equal-power law adds +3 dB to already-stereo material at centre, which is wrong for a balance control | centre leaves both channels at unity |
| **Equalizer** | 5 bands (HP, low shelf, bell, high shelf, LP), RBJ cookbook biquads | closed-form coefficients, well documented, cheap, and the analogue prototypes are what engineers expect | measured magnitude vs. analytic response of the same designer |
| **Biquad core** | direct-form I with `double` coefficients, per-channel state | DF-I tolerates time-varying coefficients better than DF-II for the modulation AURA does; coefficient math in `double` avoids the low-frequency warping at 44.1 kHz | low-pass response vs. `BiquadDesign::magnitudeDb()` |
| **Compressor** | peak/RMS detector, soft knee, attack/release in ms, look-ahead-free | standard feed-forward design; the sidechain is a *block* input (`setSidechainInput`) rather than a separate bus for now | gain reduction > 0 above threshold, unity below |
| **Limiter** | look-ahead sliding-maximum over the window + release, ceiling in dBFS | look-ahead is the only way to guarantee the ceiling without distorting transients; sliding max costs O(1) per sample with a monotonic deque | peak ≤ ceiling + 0.05 dB, output finite |
| **Delay** | per-channel delay lines with linear interpolation, feedback, damping one-pole in the loop, cross-feed for ping-pong | interpolation keeps delay *time* continuous (sync, modulation); damping in the loop is what makes long feedback musical | impulse appears at the requested sample (±1) |
| **Reverb** | Schroeder/Moorer-style: pre-delay, damped comb banks, all-pass diffusion, stereo spread, low/high cut | algorithmic, no impulse-response assets (so no licensed material), tunable, and small enough to run on any machine | tail decays monotonically and stays finite |
| **Distortion** | waveshaping with oversampling | aliasing from hard nonlinearity is audible; oversampling is the standard mitigation | 2nd-harmonic content of a sine stays below ‑60 dB |
| **Oscillator** | PolyBLEP-free naive + band-limited modes, phase-continuous | simple and predictable in tests | waveform shape and frequency |
| **Noise** | xorshift/PCG-style generator, fixed seed option | deterministic tests, no library dependency | statistical sanity, determinism |
| **Envelope** | ADSR with selectable linear/exponential curve | both curves are used in practice; exponential release reaches ‑80 dB asymptotically, which the tests encode | stage timing, sustain level, idle after release |
| **LevelMeter** | peak with hold+decay, RMS as a per-*sample* one-pole (300 ms) | block-rate integration would make the reading depend on buffer size | RMS of a 0.5 sine = 0.3536 |
| **Loudness** | ITU-R BS.1770-4: K-weighting (shelving + high-pass), 400 ms momentary, 3 s short-term, gated integrated | it is the broadcast standard; anything else would disagree with every other tool the user owns | −20.7 LUFS for two −20 dBFS tones (sum +3.01, K-weighting +0.4) |

## Cross-cutting rules

1. **Denormals.** Every recursive structure flushes via `math::flushDenormal`
   (add/subtract a tiny constant, or `_MM_SET_FLUSH_ZERO_MODE` where we control the
   thread). A decaying reverb tail that reaches 1e-38 costs 10–100× a normal
   sample otherwise; this is the classic DAW performance trap.
2. **No allocation, no locks, no I/O** anywhere on the audio path — enforced by
   `tools/rt_audit.py` (static) and by the allocation counter in
   `tests/engine/EngineTests.cpp` (dynamic).
3. **Tail honesty.** `Processor::isActive()` answers "is this switched on", while
   `Processor::hasTail()` answers "does skipping this now drop audio". The graph
   skips silent, tail-less nodes. Conflating the two is a real bug we had: a
   delay with `mix > 0` kept every node alive forever and a stopped transport
   replayed its last block. The fix is the split, and the regression test is
   "The engine renders project audio through the graph".
4. **Statistical vs. analytic tests.** Anything with a closed form (biquads, gain,
   limiter ceiling) is asserted against the formula; anything without one
   (reverb tail, distortion spectrum) is asserted for the property that matters
   (monotone decay, bounded energy, harmonic level below a threshold).
5. **Smoothing defaults.** 10–20 ms for gains and mix parameters; delay time uses
   a slower ramp because a fast delay-time change sounds like tape flutter.
   Smoothers **snap** on the first block after `prepare()`/`setSettings()`, so a
   freshly loaded effect does not ramp in from silence and swallow the first echo.

## Numbers we hold ourselves to

| Item | Target | Where it is checked |
|------|--------|---------------------|
| Master limiter ceiling | −0.3 dBFS default, ≤ 0.05 dB overshoot | DspTests limiter case |
| Limiter look-ahead | 1.5 ms default, reported as latency | EngineTests latency case |
| EQ magnitude error | ≤ 0.5 dB vs. analytic | DspTests biquad case |
| Delay time accuracy | ±1 sample | DspTests delay case |
| Reverb tail | decays to silence, all samples finite | DspTests reverb case |
| Loudness accuracy | ±1.2 LU vs. BS.1770 expectation | DspTests loudness case |
| Denormal cost | no measurable slowdown after 30 s of tail | `bench/` (M5) |

## Open questions

* Oversampling factor for Distortion: 4× fixed today; 8× with a better
  anti-imaging filter may be worth the CPU on slow machines (measure first).
* Reverb: the per-sample LFOs are cheap but not free; a block-rate LFO is on the
  profiling list.
* Compressor: an RMS detector with a proper programme-dependent release is the
  next refinement, not a rewrite.

## Sources

* R. Bristow-Johnson, *Cookbook formulae for audio EQ biquad filter coefficients* —
  the RBJ formulas used in `BiquadDesign`.
* ITU-R BS.1770-4, *Algorithms to measure audio programme loudness and true-peak
  audio level* — K-weighting, gating, loudness range.
* M. Schroeder / J. Moorer, reverberator topology (comb + all-pass networks) —
  the structure `dsp::Reverb` implements.
* AURA's own `/tmp/model.cpp` reference harness (37 checks) and
  `tools/rt_audit.py`.
