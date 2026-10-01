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
                      + inserted compensation delays (0 until M5)
```

Rules: the number is what the engine can *justify*, not what sounds good in a
comparison table. The master limiter is part of the graph's output path even though
the engine runs it after the plan, so it is reported explicitly. A compensating
delay that does not exist yet is not reported as if it did.

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
