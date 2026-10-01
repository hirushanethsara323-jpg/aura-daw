# SIMD_RESEARCH.md

**Status:** Decided (M5, issue #6) — *algorithmic first, intrinsics deferred* · **Supports:** ADR-0016 · **Code:** `aura::math` (block-state helpers), `dsp::LevelMeter`, `graph::GraphNode`, `bench/` (`graph` group)

## Question

The mixing path — fader, pan, summing every source into a bus, metering every track —
is the part of a DAW that runs for *every* track *every* block, whatever the session
contains. Should AURA vectorize it by hand with SIMD intrinsics (SSE2 / AVX2), or is
that work better spent elsewhere? The issue asks for a decision "from profile data",
so this document is mostly measurements.

## Step 1 — measure the ceiling, fairly

The first version of the probe compared hand-written AVX2 against *hand-written scalar*
loops and reported a 7× win on a gain multiply. That number was worthless: the scalar
loops were written without `__restrict`, so the compiler could not prove the output and
input pointers were distinct and refused to vectorize them (the classic aliasing
blocker — Intel's auto-vectorization guide spells it out). Giving the baseline the same
aliasing information makes the comparison honest:

| Kernel (256-frame blocks, stereo) | Baseline `-O2 -msse2`, auto-vectorizable | Hand AVX2 + FMA | Speed-up |
|---|---|---|---|
| gain multiply | 0.088 ns/frame | 0.044 | **2.0×** |
| pan (two outputs from one input) | 0.151 | 0.100 | **1.5×** |
| fan-in sum, one source | 0.113 | 0.111 | **1.0×** |
| fan-in sum, eight sources | 1.159 | 0.680 | **1.7×** |
| peak + sum of squares | 1.224 | 0.103 | 11.9×* |
| the meter's RMS recursion | 2.433 | 1.211 | 2.0×** |

\* The baseline figure is a scalar loop: GCC cannot vectorize a `max` reduction mixed
with an accumulating sum through a pointer it cannot disambiguate. Where the loop is
already vectorizable the win disappears — see the single-source sum, which is **1.0×**,
because at these block sizes the loop is limited by load/store bandwidth, not by
arithmetic width.

\*\* The AVX2 build is not vectorizing a recursion (it cannot); the 2× is the compiler
contracting `a*x + b` into a single FMA, and only because the test build allowed it.

**Read as a whole:** hand-written AVX2 buys 1.5–2× on the elementwise mixing loops and
*nothing* on the summing loop that dominates a large session.

## Step 2 — what the shipped build actually emits

```
libaura_dsp.a     934 movss   248 mulss    10 mulps    2 addps
libaura_engine.a  759 movss   108 mulss     6 mulps    2 addps
```

The per-sample loops in the shipping binary are scalar. That is not a missing flag: the
hottest ones cannot be vectorized as written, because they carry a loop-carried
dependency (a one-pole recursion) or write through pointers the compiler must assume
alias (`-ffp-contract=off` and `/fp:precise` are project policy, and neither changes
this).

## Step 3 — the loops that actually cost

`bench/` had a hole here: the graph's own work was never measured. The new `graph`
group fills it (256-frame blocks, stereo):

| Case | ns/frame | Reading |
|---|---|---|
| `graph/fan-in-8` | 2.2 | summing one source in costs ~0.2 ns/frame/channel |
| `graph/fan-in-16` | 5.1 | linear in sources: ~0.4 ns/frame per incoming connection |
| `graph/fan-in-48` | 19.9 | a 48-track master's summing, total |
| `graph/fan-in-64` | 29.4 | |
| `graph/fader-steady` | 0.46 | a fader that has landed: one multiply |
| `graph/fader-ramp` | 2.81 | a fader *in flight*: the recursion |

Put beside the session cases, the fan-in path is ~20 ns/frame of a 555 ns/frame
48-track session — **3 %**. Vectorizing it by 1.7× would move a session by ~1 %, at a
cost of two code paths and a CPU-feature dispatch. That is the end of the SIMD case for
the mixer.

## Step 4 — what the data said to do instead

Both of the expensive loops in that table are **first-order recursions** — the meter's
exponential RMS and a node's gain ramp. Each step needs the previous step's state, so
the FPU stalls on latency instead of running at throughput, and no vector width helps.

Because the coefficient is constant within a block, the recurrence is linear and
time-invariant, and a *group* of steps has a closed form:

```
s[n+3] = a⁴·s[n-1] + c·(a³·x[n]² + a²·x[n+1]² + a·x[n+2]² + x[n+3]²)
```

The four products are independent, so the dependency chain shrinks from four steps to
one. This is *the same recurrence*, re-associated — not an approximation. It is
implemented as two functions in `include/aura/core/Math.hpp`
(`advanceSquareRecursion`, `applyOnePoleRamp`) and used by `dsp::LevelMeter` and
`graph::GraphNode::applyGainSmoothing`.

Measured, suite-level, same machine, same session, before/after on the same commits
(1500 iterations per case, median of five passes):

| Case | Before | After | Delta |
|---|---|---|---|
| `dsp/levelmeter/128` | 6.87 ns/frame | 4.05 | **−41 %** |
| `dsp/levelmeter/512` | 7.02 | 4.34 | **−38 %** |
| `graph/fader-ramp/256` | 7.56 | 2.81 | **−63 %** |
| `graph/fader-ramp`, every block size 64…2048 | 7.4–7.7 | 2.8–4.2 | −44…−64 % |
| `graph/fader-steady/256` | 0.469 | 0.469 | unchanged (it is a multiply) |
| `engine/session-1x1`, every block size | 48.1–49.8 | 45.2–47.4 | **−5…−6 %** |
| `engine/session-16x8/256` | 173.4 | 167.8 | −3.2 % |
| `engine/session-48x8`, `…-fx` | 455–4800 | within ±1 % | noise floor of this machine |

The smaller the session, the larger the share the meter has — which is why the 1×1
session moves 6 % and a 48-track session with effects does not move measurably.

**Numerical contract:** a single block matches the per-sample form to ~1e-6 relative
(the rounding of one `a⁴` per group); over thousands of blocks the difference stays
bounded at ~1e-5 relative because the recursion is stable, and 1e-5 relative is
0.0001 dB — below the resolution of any meter we display. `tests/dsp/BlockStateTests.cpp`
pins both bounds, plus the property the meter's per-sample integration exists for: the
reading does not depend on how the audio is split into blocks.

## Step 5 — the denormal trap (why this document has a "measured with the right benchmark" section)

The first version of `graph/fader-ramp` ramped the fader down to 0.25 and back with
nothing ever restoring the material. Over a few hundred iterations the source buffer
decayed into the subnormal range, and on x86 every multiply with a subnormal operand
takes a microcode assist — Agner Fog measures ~124 cycles, and a WebRTC port hit the
identical problem in its recursive filter states ("on silent input, several recursive
filter states never decay to exact zero... a silent frame costing about 9× an active
one"). The benchmark reported **72 ns/frame** for a loop that actually costs **2.8**.

Two consequences, both now permanent:

* the case keeps the fader ride *around unity*, and its sanity check fails the case if
  the material leaves an amplitude band in either direction (drained to silence or run
  away), so the trap cannot come back silently;
* the ramp's closed form snaps its residual to the target once it is within 8 ULP.
  Without that snap the ramp never *lands*: the state is a float, so the moment the
  remaining difference falls under half an ULP the state parks on the neighbouring
  representable value, and — because each call recomputes the difference from that
  rounded state — it stays there forever while the difference keeps decaying into the
  subnormal range. The per-sample form has the same parking behaviour (one ULP short,
  forever on the slow path); the snap fixes both, and the node reaches exactly
  `current == target`, so the fader afterwards costs one plain multiply.

## Decision

1. **No hand-written SIMD kernels for the mixing path.** At measured 1.5–2×, on loops
   that are ~3 % of a session, for two code paths and a runtime dispatch, the
   end-to-end effect is below this project's measurement noise. Rejected on data, not
   on taste.
2. **Adopt the pattern, not the intrinsics:** block-state unrolling of first-order
   recursions. Implemented (−41 % meter, −63 % fader ramp, −6 % small session) and
   preferred over SIMD *because* it needs no CPU feature, no dispatch, no second code
   path, and no change to the FP environment.
3. **Keep the elementwise loops alias-friendly** so the compiler can still vectorize
   what is vectorizable (it does, when it can prove non-aliasing). No `-ffast-math`,
   no `/fp:fast`, no `-march=native`: reproducibility beats a few percent, and
   `-march=native` would produce binaries that are illegal on the machines older than
   the build machine (ADR-0004 decision on `/fp:precise`).
4. **No AVX-512.** It is absent from the "modest PC" reference profile, and the
   downclocking behaviour makes it a worse default than AVX2 even where it exists.
5. **Revisit when:** a real session profile (not a microbenchmark) shows an
   elementwise loop above ~10 % of the block budget on the reference profile. The
   order of attack is then: (a) check it is not a denormal problem, (b) fix aliasing /
   restructure the loop, (c) measure a hand-written kernel in isolation and require
   ≥2× there before it earns a home in the tree. **`std::simd` (C++26) is the one
   development that would change the cost side of this trade** — a portable,
   single-source formulation would remove the "two code paths" objection entirely; the
   compiler support (MSVC 19.x, GCC 14) is not there yet, so it is tracked as a
   follow-up rather than adopted now.

## Sources

* Intel, *A Guide to Vectorization with Intel C++ Compilers* — aliasing is the classic
  blocker ("if the compiler cannot exclude this possibility, it will not vectorize the
  loop"), and `restrict` is how you remove the doubt; also the reference for why
  data-dependent branches and loop-carried dependencies inhibit vectorization.
* Intel, *Use Automatic Vectorization* (DPC++ developer guide) — same conclusion from
  the current toolchain documentation: the compiler must conservatively assume
  overlapping memory regions can cause dependencies.
* Agner Fog, *Microarchitecture of Intel, AMD and VIA CPUs* (quoted and applied in the
  Stack Overflow thread *Why is icc generating weird assembly for a simple main?*):
  Haswell/Broadwell take a penalty of ~124 clock cycles when an operation on normal
  numbers produces a subnormal result, and for a multiplication between a normal and a
  subnormal operand regardless of the result; FTZ + DAZ avoid it.
* Stack Overflow, *Why denormalized floats are so much slower than other floats, from
  a hardware architecture viewpoint?* — the FP_ASSIST microcode flow, and the
  observation that this varies by microarchitecture (Sandy Bridge and later handle
  add/sub subnormals without an assist, Ryzen is far cheaper than old Intel parts).
* WebRTC (`dignifiedquire/sonora` PR #38, *flush denormals during stream processing*) —
  the same failure mode in an audio application: recursive filter states settling in
  the subnormal range, reproduced as a 9× cost on silent frames on Intel and much
  smaller on AMD.
* *Fading Audio is ROUGH on CPUs* (soundquality.org, Sep 2025) — a fade or a fade-out
  is the textbook way to generate subnormals in audio code, and why DAWs guard against
  them explicitly.
