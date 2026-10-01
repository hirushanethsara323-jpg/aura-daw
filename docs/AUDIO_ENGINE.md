# AUDIO_ENGINE.md

The real-time engine: what happens inside one callback, and the rules that keep it
real-time. Threading research and the bugs that shaped these rules are in
[`research/THREADING_RESEARCH.md`](research/THREADING_RESEARCH.md).

## Entry point

```cpp
void AudioEngine::processBlock(float* const* outputs,
                               const float* const* inputs,
                               int numFrames) noexcept;
```

Called by the device (WASAPI event callback, ASIO buffer switch, mock thread).
`noexcept`, allocation-free, lock-free, non-blocking.

## The ten steps

| # | Step | Notes |
|---|------|-------|
| 1 | Drain `EngineCommandQueue` | bounded to 32 commands per block; POD payloads only |
| 2 | Build `ProcessContext` | sample rate, position (samples + ticks), tempo, `isPlaying` |
| 3 | `renderClips()` | each track's clips render into that node's buffers; the node's "input active" flag is set from what was actually written |
| 4 | `renderInstruments()` | MIDI clips → events (sample-offset clamped into the block) → instruments |
| 5 | `renderRecording()` | input monitoring / take capture (no hardware capture yet — see AUDIO_BACKEND) |
| 6 | `GraphPlan::process()` | clear accumulators → fader ramp → inserts → fan-in → next node |
| 7 | Master strip | limiter (look-ahead, default ceiling −0.3 dBFS) → meter → loudness → `sanitizeBlock()` |
| 8 | Copy to outputs | channel-count aware; silence when there is no plan |
| 9 | `transport_.advance(frames)` | loop wrap, punch, count-in, record start, position publish |
| 10 | Publish statistics | CPU load, peak load, xruns, meter snapshots (atomics) |

If the transport is **stopped**, source nodes are cleared and marked inactive (so a
reverb/delay tail still decays, but the last block is never replayed), the master
still runs its limiter and meters, and the transport does not advance.

## Latency accounting

```
totalLatencySamples() = device latency
                      + longest insert path in the graph (plan latency)
                      + master strip latency (the limiter's look-ahead)
```

Rules: the number is what the engine can *justify*, not what sounds good in a
comparison table. The master limiter is part of the graph's output path even though
the engine runs it after the plan, so it is reported explicitly.

**Compensation delays are deliberately not in that sum.** Delay compensation does
not make the session shorter — it delays the paths that are *early* so that parallel
paths meet sample-aligned. The plan's `latencySamples()` is already the longest path
in the graph, so adding `compensationSamples()` on top would report the same samples
twice. The figures are reported separately, which is also how a mixing engineer reads
them: "the session is 64 samples late" and "one connection was delayed 64 samples to
line up" are two different statements.

```
plan.latencySamples()          longest path through the graph
plan.compensationSamples()     total delay inserted to align parallel paths
plan.compensatedEdges()        how many connections that delay is spread over
```

`AudioEngine::delayCompensationSamples()` and
`compensatedConnectionCount()` publish the same numbers from the current plan, and
`EngineSettings::delayCompensation` (a preference, default on) turns the mechanism
off without removing the reported latencies.

## Delay compensation

Two paths that carry the same audio remeet after one of them passed through a
look-ahead limiter, an oversampled distortion or a linear-phase EQ, and the copies
arrive at different times. They then comb-filter — the hollow, phasey sound that is
worse than the latency it came from. The graph fixes that by delaying the *shorter*
path.

```
pathLatency[node] = max(pathLatency[inputs]) + node's own insert latency
edge delay        = (the node's latest input) − pathLatency[source]
plan latency      = the longest path in the graph
```

* The compensation is stored **per edge** (`graph::InputEdge`), not per node: one
  node can feed several destinations whose path latencies differ, and a single delay
  cannot serve both.
* Each edge owns a `dsp::DelayLine`, sized in `prepare()` and never resized
  afterwards, so `sumFrom()` allocates nothing on the audio thread. An edge with a
  zero delay takes the plain summing path.
* A bypassed processor contributes no latency (it does not run), so compensating for
  it would *create* the misalignment — `InsertChain::totalLatencySamples()` skips
  bypassed processors, exactly as `process()` does. Toggling bypass is a graph change.
* The master strip is not part of the plan: the limiter runs after
  `GraphPlan::process()` (step 7 above), so it is reported in the round-trip figure
  and not compensated for — there is nothing after it to align it with.
* **Live input monitoring is outside the compensation.** You cannot play a musician
  before they play, so the monitor path carries the device's own latency and is not
  an `InputEdge`. This is a documented limitation, not an oversight.
* A plug-in that reports a wrong latency cannot be corrected by the host — the host
  can only be consistent with what it is told. Per-processor latency is displayed so
  the figure can be checked, and late latency reports (`restartComponent(kLatencyChanged)`
  in VST3) mean "rebuild the plan", which the engine already supports.

Design notes, alternatives and sources: `research/LATENCY_COMPENSATION_RESEARCH.md`.
Verification (impulse-based, end-to-end): `tests/graph/DelayCompensationTests.cpp`.

## Windows system libraries

AURA links four Windows import libraries and nothing else from the platform. They
are part of the Windows SDK, so they add no third-party dependency, but each one
has a reason and the list is closed on purpose:

| Library | Used for |
|---------|----------|
| `ole32` | COM: `CoInitializeEx`/`CoUninitialize`, `CoCreateInstance`, `CoTaskMemFree` (including the strings `SHGetKnownFolderPath` returns) |
| `shell32` | `SHGetKnownFolderPath` — the per-user AppData and Music folders |
| `avrt` | MMCSS: promoting the render thread to "Pro Audio" (`AvSetMmThreadCharacteristicsW`) and reverting it on stop |
| `winmm` | timer resolution (`timeBeginPeriod`) and the legacy output path used as a compatibility fallback |

`uuid` is deliberately absent: every interface ID is resolved at compile time
through `__uuidof`, so there is no link-time GUID symbol to satisfy. When the
crash handler lands (M7) it will add `dbghelp` for `MiniDumpWriteDump`, and that
will be recorded here and in `LICENSES.md` at the same time.

## Device management

* `AudioDeviceManager` enumerates, opens, closes and switches devices and owns the
  mock backend; it never runs on the audio thread.
* The engine asks for a sample rate and buffer size and always reads back what the
  device **granted** (drivers refuse politely and silently; sizing from the request
  is how buffer overruns are born).
* Device loss (`deviceLost()`) is a state, not a crash: the stream is torn down, the
  transport keeps its position, and the UI offers retry/switch.
* Sample-rate or buffer-size change: the graph is rebuilt (`rebuildGraph()` →
  `prepare()` everywhere) and the plan is republished.

## Recording path

* Preallocated interleaved write buffer per take; the audio thread appends, the
  writer drains. A full buffer increments `droppedFrames()` rather than blocking.
* 24-bit WAV from the first second, header sizes patched on close, so an
  interrupted take is still a readable file.
* `recoverIncompleteTakes()` finds and repairs takes after a crash.

## Offline render

`OfflineRenderer` drives `AudioEngine::processBlock` — the same code path — with a
scratch block, then writes the result through `media::AudioFileWriter` (with
optional TPDF dither at 1 LSB for integer depths, and clamping before quantising).

Deliberate constraints:

* the render block is clamped to the engine's prepared block size (otherwise the
  render would write past buffers that were prepared for something smaller),
* the result is **bit-identical** to what the device callback produces from the same
  start position — not by construction, but as an assertion: a test renders 10 240
  frames of a session containing a stateful reverb, a send with a level, an insert and
  the master limiter both ways and compares every sample (worst difference: 0),
* a sample-rate mismatch is **refused** with a clear message rather than silently
  producing audio at the wrong speed (there is no sample-rate conversion yet),
* stems solo one track at a time so bus routing and inserts behave as in the
  session — a stem is a bounce, not a re-mix,
* the master limiter can be bypassed for stem exports that must not be limited
  individually.

## Statistics and diagnostics

| Value | Meaning |
|-------|---------|
| `cpuLoad`, `peakCpuLoad` | fraction of the block budget used, peak-held |
| `xruns` | blocks where the callback missed its deadline (device-reported where available) |
| meter snapshots | peak (with hold/decay), RMS, clip flags per channel |
| `masterLoudness().snapshot()` | momentary, short-term, integrated LUFS, LRA |
| `underrunCount()` | per disk stream — a stream that cannot keep up says so |

## Budgets

See [`PERFORMANCE.md`](PERFORMANCE.md). The short version: ≤ 50 % of the block
period at 64 voices on a 2-core laptop, < 2 % idle CPU with a 24-track session open,
and no allocation anywhere on the callback path (measured, not assumed).
