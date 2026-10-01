# MIDI.md

The MIDI subsystem as shipped. Research (including why MIDI 2.0 is deliberately not
implemented yet): [`research/MIDI_RESEARCH.md`](research/MIDI_RESEARCH.md).

## Timing: ticks, everywhere

* **960 PPQN** (`time::kTicksPerBeat`) for every musical position: notes, clips,
  automation, markers, tempo map. One unit, one place to get it wrong.
* Sample conversion happens **once per block**, in the engine: a block's
  `[startTicks, endTicks)` window is converted through the tempo map, and each event
  gets a `sampleOffset` clamped into the block. Note starts are therefore
  sample-accurate inside the block.
* **Windows are half-open** `[start, end)`: a note-off exactly on the boundary
  belongs to the next block. The header states this because the alternative produces
  a stuck note per loop.

## Data model

```cpp
struct Note { int pitch, velocity, releaseVelocity; int32 channel;
              int64 startTicks, lengthTicks; bool selected; };
struct Controller { int controller; int value; int64 positionTicks; };
class MidiClip { std::vector<Note> notes_; std::vector<Controller> controllers_; };
```

* Notes are stored; the event stream is derived (`collectEvents(startTick, endTick,
  out)`), which emits note-on/off pairs plus CC and pitch-bend, **and a note-off for
  a note that started before the window and is still sounding** — the standard stuck
  note fix.
* Sorting is `core::stableSortSmall` (allocation-free insertion sort): events sharing
  a timestamp must keep their order, and `std::stable_sort` allocates, which is not
  allowed on this path.
* Operations: `addNote`, `removeNote`, transpose, velocity scaling, length changes,
  split at a tick, range queries (`pitchRange()`), quantise.

## Quantise

`QuantiseSettings { grid, strength }`. Strength is a real percentage: 50 % moves a
note halfway to the grid. Grid values come from the timeline's musical grid so the
piano roll and the arrangement agree.

## Recording

`clip.appendRecorded(message)` bakes an incoming stream into notes; the clip is
sorted on access, not on insert. Panic (`allNotesOff` + CC 123 on every route) is a
command, not a workaround: a stuck note is the most annoying failure a MIDI DAW has.

## Devices and routing

`MidiInputRouter` owns the device list and dispatches to tracks:

```cpp
struct MidiRoute { std::string inputDevice; int inputChannel = -1;   // -1 = omni
                   std::string outputDevice; int outputChannel = 0;
                   bool enabled = true; bool thru = false; };
```

Windows input uses WinMM (`midiIn`) — no dependency, works on every device. Sending
to external hardware is routed through the same table. Virtual ports and MIDI
Services (UMP/MIDI 2.0) are future work, documented as such.

## Instruments

`midi::Instrument` is the internal API (`prepare`, `process`, `isProducingSound`,
`allNotesOff`, `panic`). The built-in is `aura.basic`: oscillator → filter → ADSR
poly synth with oldest-first voice stealing and a **capacity-bounded** sustain-pedal
stack (reserved in `prepare()`, guarded push), so a pedal gesture can never allocate
on the audio thread.

Third-party plug-in instruments go through `plugin::PluginHost` and appear as the
same `Instrument` interface to the engine — the engine does not know the difference.

## Not implemented (stated, not implied)

| Feature | Status |
|---------|--------|
| SMF (`.mid`) import/export | declared, returns `NotImplemented` |
| MIDI 2.0 / UMP | not implemented; the message struct is wide enough to adopt it without a clip-format change |
| MPE / per-note expression | data model allows it (per-channel bend); not surfaced in the UI |
| Virtual MIDI ports | out of scope for v1 |
| MIDI recording latency compensation | uses the input timestamp as-is; needs device latency from the audio backend |

## Tests

`tests/midi/MidiTests.cpp`: message payloads and running status, quantise strength,
sort by (start, pitch), range, half-open collection windows, recorded-note baking,
router dispatch + panic, instrument lifecycle, voice stealing.
