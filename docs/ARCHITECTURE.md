# ARCHITECTURE.md

How AURA is put together: the modules, who owns what, and how a sample gets from a
file on disk to the speakers. Decisions behind this shape are in
[`research/TECHNOLOGY_DECISION_RECORD.md`](research/TECHNOLOGY_DECISION_RECORD.md).

## Layers

```
                 app/                (M9: Win32 + Direct2D shell, Windows only)
                  │  commands, view state, meter snapshots (atomics)
                  ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │                        aura_engine                               │
   │                                                                  │
   │  engine/      AudioEngine · EngineCommandQueue · OfflineRenderer │
   │  audioio/     IAudioDevice · WasapiDevice · AsioDevice · Mock    │
   │  graph/       GraphNode · GraphPlan · GraphBuilder · SampleStream│
   │  audio/       Track · Mixer · AudioClip · clipops · WaveformCache│
   │               RecordingEngine                                    │
   │  transport/   Transport (one owner of position/state/loop)        │
   │  time/        TempoMap · BBT · SMPTE · musical arithmetic        │
   │  midi/        Message · MidiClip · MidiInputRouter · Instrument  │
   │  automation/  Lane · AutomationRecorder · TargetRange            │
   │  timeline/    snap modes · markers · view state                  │
   │  project/     Project · MediaPool · Autosave · recovery          │
   │  commands/    ids · CommandRegistry · UndoHistory · edits        │
   │  plugin/      PluginHost surface · PluginDatabase · PluginChain  │
   └──────────────────────────────────────────────────────────────────┘
                  │  blocks of float, planar
                  ▼
              aura_dsp          Processor · Biquad · smoothing + 14 processors
                  │
                  ▼
              aura_core         Errors · Math · Json · Log · FileSystem ·
                                Realtime · Sort · Strings · Version ·
                                Localization
```

`aura_engine` is one library because the graph, the device and the engine have a
genuine three-way dependency (the device decides the block size, the engine drives
the graph, the graph must render exactly that block). Module boundaries are
enforced by directories and include rules, not by link boundaries.

## One block, end to end

```
WASAPI event / ASIO callback / mock timer
        │
        ▼
AudioEngine::processBlock(outputs, inputs, frames)
  1. drain EngineCommandQueue           (bounded: 32 commands per block)
  2. build the process context          (sample rate, position, tempo)
  3. render clip audio into source nodes
  4. render MIDI instruments
  5. render input monitoring / recording
  6. GraphPlan::process                  (clear → fader → inserts → sum)
  7. master strip: limiter → meter → loudness → sanitize
  8. copy to the device buffers
  9. transport_.advance(frames)          (loop, punch, count-in, record start)
 10. publish statistics (CPU, xruns, meters)
```

Rules that hold on every one of those lines: no allocation, no locks, no file I/O,
no exceptions, and no unbounded work. `tools/rt_audit.py` checks the static
properties; the engine tests check the dynamic ones.

## Ownership (who may touch what)

| Object | Written by | Read by |
|--------|-----------|---------|
| `Project` (tracks, clips, media, automation) | control/UI thread | control thread |
| `GraphPlan` | control thread (built, then published) | audio thread (const after publish) |
| `AudioEngine` transport/statistic atomics | audio thread | UI thread |
| `EngineCommandQueue` | UI thread (push) | audio thread (drain) |
| `WaveformCache` levels | worker thread (append + `bucketsReady`) | UI thread |
| `DiskStream` ring | reader thread (write) | audio thread (read) |
| `RecordingEngine` writer buffers | audio thread (append) | writer/control thread (drain) |
| `IAudioDevice` (WASAPI/mock/offline) | `AudioDeviceManager` (creates, stops, destroys) | engine (borrowed raw pointer, released with `detachDevice()`) |
| `UndoHistory` stacks | control thread (`beginTransaction`/`addEdit`/`commitTransaction`) | control thread (`undo`/`redo`); re-entrant edits from inside an undo are refused |

Each header states its thread contract in the class comment (`Control thread only.`,
`RT-safe.`, `Called by the audio thread.`). That is a maintenance rule: a comment
that is wrong is a bug.

## The graph

* `GraphBuilder` builds nodes and connections, detects cycles (Kahn's algorithm)
  and reports them as an error instead of producing a silent or infinite graph.
* `GraphPlan` is immutable once published: an ordered node list, the master node id
  and the total insert latency of the longest path.
* Publishing is a release-store of a raw pointer; the previous plan is retained for
  two processed blocks *after that retirement* (the grace window) so no in-flight block can reference freed
  memory.
* Per block, `GraphPlan::process` first clears every accumulator (nodes with
  incoming connections and the master), then processes in topological order. Source
  nodes are deliberately *not* cleared there — the engine has already filled them
  with this block's audio.
* Each node runs **inserts first, then its fader/pan/mute**, with the pre-fader tap
  captured in between. The fader is the last thing before the output, so riding it
  cannot change what the inserts do — otherwise the same fader move would re-voice
  every non-linear insert (drive a saturator harder, push a compressor over its
  threshold) and the mix would no longer be the mix that was saved.
* A node is skipped only when it has no input **and** no tail
  (`Processor::hasTail()`), which is what makes a 24-track session cheap without
  truncating a reverb tail.

## Sends, buses and solo

Tracks route to a destination (master by default, or a bus/aux return). A send is a
*connection with a level*, not a second copy of the channel: the level is carried on
the graph edge (`InputEdge::gain`) and applied while summing, so one source can feed
the master at unity and a reverb return at -12 dB without an extra node, buffer or
latency-accounting participant. A zero-level send is skipped entirely — it carries
nothing and must not touch a delay ring.

A send has two independent properties — its level (the edge gain) and its **tap
point**. A post-fader send reads the source's output; a pre-fader send reads the
source's *tap*: the signal after its inserts but before its fader, pan and mute. The
tap is a per-connection choice because the same source feeds the master post-fader and
a cue mix pre-fader — which is the entire point of a cue mix: the artist's headphones
must not go silent when the control room fader is pulled down.

The tap buffer exists only for nodes that actually feed a pre-fader edge (allocated in
`prepare()`, one planar buffer), so a session without pre-fader sends pays one bool
test per block and no memory.

Solo is resolved once per block (`Mixer::resolveSoloState`) and applied by the plan, so
a soloed bus keeps its sources audible and an unsoloed track is muted without the graph
being rebuilt.

## Error handling

`AuraResult<T>` carries either a value or an `AuraError` (code + technical message +
localised user message + detail). No exception crosses a module boundary, and the
engine never returns a silent failure: a refused device format, an absent SDK, a
missing plug-in and a corrupted manifest are all diagnosable values that the UI can
turn into a sentence.

## Where things will grow

| Area | Next step | Milestone |
|------|-----------|-----------|
| Multichannel | graph, insert and meter paths are already channel-count aware; the device layer is stereo-first | after M5 |
| Sample-rate conversion | import currently refuses a mismatch rather than resampling | after M5 |
| Plug-in sandbox | `PluginHost` is the only vendor-facing surface, so the transport can be replaced with an out-of-process one | M7 |
| UI | `app/` Win32 + Direct2D shell consuming commands and atomics only | M9–M10 |
