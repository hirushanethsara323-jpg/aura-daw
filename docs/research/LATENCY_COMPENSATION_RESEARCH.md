# LATENCY_COMPENSATION_RESEARCH.md

**Status:** Implemented (M5, issue #5) · **Supports:** ADR-0015 · **Code:** `aura::graph` (`GraphBuilder`, `GraphNode::sumFrom`), `aura::dsp::DelayLine`, `aura::engine::AudioEngine`

## Question

Every processor in a session that has to look ahead — a limiter, an oversampled
distortion, a linear-phase EQ, a plug-in's internal buffer — delays the signal that
passes through it. When two paths that carry the same audio meet again further down
the graph (a dry track against a processed one, a direct out against a send bus),
the shorter path arrives first. The two copies no longer sum; they *comb filter*,
which is audible as a hollow, phasey tone and is worse than the latency it came
from. So: where does the alignment live in AURA, how much delay is correct, what is
`latencySamples()` on the plan supposed to mean, and what can't be fixed?

## The arithmetic

Latency is additive along a path and comparable only at a meeting point:

```
pathLatency[node] = max(pathLatency[source] over inputs) + node's own insert latency
edge delay        = pathLatency[node]'s latest input − pathLatency[source]
plan latency      = the longest path in the graph
```

Worked example, the case the tests pin (`tests/graph/DelayCompensationTests.cpp`):

| Node | Inserts | Path latency | Incoming edge delay |
|------|---------|--------------|---------------------|
| Dry track | — | 0 | **64** (delayed to meet the latent path) |
| Latent track | pad, 64 | 64 | 0 |
| Master | — | 64 | — |

Compensation therefore **adds delay to the paths that are early**; it never pulls
the late ones earlier. That is the whole design, and it is why the plan's
`latencySamples()` does not change when compensation is switched on: the session
gets *less* jagged, not shorter.

## Approaches compared

| Approach | How it works | Why we chose / rejected it |
|----------|--------------|----------------------------|
| **Delay the shorter paths** (chosen) | Each edge that is early gets an integer-sample delay of the difference | Needs no future audio, no transport changes, works with a live input in the same graph, and is exact at sample granularity. Costs one ring buffer per compensated edge, sized once in `prepare()` |
| **Read-ahead** (Ardour's default for playback) | The host plays the session slightly in the future and reads material *ahead* of the playhead, so the longest path's output is what reaches the outputs | Better for the transport (the audio you hear matches the timecode without extra delay), but it requires material to exist before the session start, so it cannot be applied to live input, and it changes what "play position" means for every consumer (recording, MIDI, metronome). We keep the option open — the delay lines live in the graph, not in the transport |
| **Per-node delay** | Give each node one delay equal to `longestPath − itsPathLatency` | **Wrong, and the obvious implementation.** A node can feed several destinations whose path latencies differ; one delay cannot serve both. Compensation is a property of the *connection*, so it is stored per edge (`InputEdge`) |
| **Report only** | Show latency in the mixer and let the user nudge tracks manually | Rejected as the default: it is a manual step that silently produces comb filtering when forgotten. Manual per-processor latency override is still a planned UI affordance for plug-ins that lie |
| **Compensate the master strip inside the plan** | Include the master limiter's look-ahead in the plan's latency | Rejected: the master limiter runs *after* `GraphPlan::process()` (engine step 5), so an edge delay computed against it would delay every track by the limiter's look-ahead for no benefit — the master output is the last thing in the chain, and there is nothing left to align it with |

## What is implemented

* `dsp::DelayLine` — fixed-capacity, integer-sample ring (`capacity + 1` slots), one
  write and one read per sample, no allocation after `prepare()`, no branching on
  signal values, delay clamped to capacity so a bad number cannot read out of bounds.
* `graph::InputEdge { NodeId source; int compensationSamples; }`, owned by the
  destination node. `GraphNode::prepare()` allocates one `DelayLine` per edge sized
  to that edge's compensation.
* `GraphBuilder::build()` runs a single pass over the topological order (the same
  order the plan processes in) and fills in the delays; `setDelayCompensationEnabled(false)`
  zeroes them all, which is what the preferences toggle does. `GraphPlan` publishes
  `compensationSamples()` and `compensatedEdges()` for the UI.
* `GraphNode::sumFrom()` reads the edge's delay from the node's edge table (a linear
  scan over a handful of edges — no map, no allocation) and either sums through the
  delay line or takes the plain sum path when the delay is zero.
* `AudioEngine::rebuildGraph()` attaches track inserts to the builder's nodes
  **before** `build()`, because the builder has to see the chains to compute their
  latency. `AudioEngine::delayCompensationSamples()` and
  `compensatedConnectionCount()` expose the published figures; `totalLatencySamples()`
  is device latency + master strip only, because compensation is alignment, not
  round-trip latency (adding it would report the same samples twice).
* Bypass is handled where it is honest: `InsertChain::totalLatencySamples()` skips
  bypassed processors because `process()` skips them too. A bypassed look-ahead
  limiter adds no delay, so compensating for it would *create* the misalignment.
  Toggling bypass is a graph change — the engine rebuilds the plan.
* A real gap the tests found and this work closed: `GraphNode::prepare()` now calls
  `prepare()` on every processor in its insert chain (`tests/graph/DelayCompensationTests.cpp`,
  "The graph prepares the processors in its insert chains"). Before that, an insert
  added to a track processed with unprepared internal state — silent for a delay
  line, garbage for a filter.

## What cannot be fixed, and how we say so

* **Live input monitoring is outside the plan.** You cannot play a musician before
  they play, so the monitored signal cannot be aligned with a compensated playback
  path. The monitor path is deliberately not an `InputEdge`, and the round-trip
  figure the user sees is the device's own latency plus the master strip. This is
  documented rather than hidden (`docs/AUDIO_ENGINE.md`).
* **A processor that misreports its latency cannot be corrected by the host.**
  Paul Davis is blunt about this on the Ardour forum: if a plug-in says 0 when it
  delays by 23 ms, no amount of host arithmetic recovers the alignment. What the
  host can do — and AURA does — is display the reported figure per processor and
  keep the arithmetic consistent with it. A manual override is on the roadmap.
* **Latency that appears after the first `process()` call** (some plug-ins report it
  late, VST3 signals it with `restartComponent(kLatencyChanged)`) invalidates the
  computed delays. The graph is built to re-learn: the engine rebuilds the plan and
  the compensation follows. The VST3 host path (M6/M7) must call this out to the
  engine when the notification arrives; until then the built-in processors report
  their latency at prepare time.

## Budgets

* Per compensated edge: `(compensation + 1) * 4` bytes, allocated once in
  `prepare()`. A 512-sample compensation across 48 tracks is 98 KB — noise next to
  the clip buffers it aligns.
* Audio-thread cost: one extra read + write per sample per compensated edge, and
  nothing at all when no path is compensated (the delay is 0, and `sumFrom` takes
  the plain sum).
* Build cost: one pass over the topological order with a linear scan of the edge
  list per node. It runs on the control thread during a rebuild and is invisible at
  session sizes (measured: the 48-track benchmark builds and rebuilds in
  microseconds — see `docs/PERFORMANCE.md`).

## How it is verified

Measured, not asserted structurally: every test places impulses and checks where the
audio *lands*, because a test that only checks "the plan says 64" would pass even if
nothing delayed anything.

| Test | What it pins |
|------|--------------|
| "Compensation aligns a short path with a latent one" | Two tracks, one with a 64-sample pad: with compensation the master shows one summed impulse at twice the amplitude; without it, two impulses 64 samples apart |
| "Without compensation the dry path really is early" | The negative control for the case above |
| "A send is compensated against the direct out" | Track → master directly *and* through a bus with a latent insert: the **direct** edge is the one that needs the delay |
| "Compensation follows the longest path, not the first one found" | Two latent paths of different lengths: only the shorter edge is delayed, the plan reports the longest |
| "A bypassed insert adds no latency and no compensation" | Bypass removes the delay it would otherwise justify |
| "Compensation keeps its bounded output over a long run" | 200 blocks of impulses: output stays finite and ≤ the sum of the inputs, so a drifting ring index cannot hide |
| "The graph prepares the processors in its insert chains" | Inserts are prepared with the real sample rate / block size / channel count, twice in a row |
| "The engine aligns a track that carries a real latent insert" | End-to-end: real project, real track insert, real `AudioEngine`, outputs captured and the arrivals compared |

## Sources

* Steinberg, *VST 3 Developer Portal — Processing FAQ* and `IAudioProcessor`
  reference: `getLatencySamples()` is the group delay the plug-in must report, and
  a plug-in that changes it calls `IComponentHandler::restartComponent(kLatencyChanged)`
  so the host can re-query and "adapt its latency compensation if supported" —
  i.e. **compensation is the host's job, and re-learning latency is an expected
  event, not an error**.
* Ardour manual, *Latency and Latency-Compensation*: latency is additive (converters,
  I/O buffers, plug-ins), and the two host strategies are read-ahead (play early) and
  write-behind; read-ahead avoids transport/timecode complications but needs material
  before the session start.
* Ardour Discourse, *Latency compensation: effective management of plug-in latencies
  in complex mix environments* — Paul Davis describing the algorithm AURA implements:
  compute the latency of every routing path, then delay the *less* latent ones so the
  whole mix is sample-aligned; and that a plug-in which misreports latency cannot be
  fixed by the host.
* Ardour Discourse, *Edit → plugin delay: what is solved?* — the user-visible
  consequence: PDC adds delay to the other tracks as well, otherwise tracks phase
  against each other; per-plug-in latency display and manual override are the
  accompanying affordances.
