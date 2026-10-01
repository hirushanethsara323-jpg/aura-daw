# MIDI_RESEARCH.md

**Status:** Accepted (MIDI 2.0 deferred) · **Supports:** the piano roll and instrument API · **Code:** `aura::midi`

## Question

What does AURA implement of MIDI, how is it timed, and what does a MIDI clip
actually contain?

## Scope decision

| Feature | Status | Why |
|---------|--------|-----|
| MIDI 1.0 messages over Windows WinMM (`midiIn`) | implemented (`MidiInputRouter`) | it is what every device on the market speaks today, and WinMM is the OS API with no dependency |
| MIDI 1.0 file (SMF) read/write | declared, `NotImplemented` | honest gap: the API exists and returns a clear error rather than half-parsing a file |
| MIDI 2.0 / UMP | **not implemented, deliberately** | UMP is the future but Windows MIDI Services is still rolling out and no user-visible feature would be delivered now. The data model avoids a rewrite: messages are `aura::midi::Message` structs with 32-bit payload fields, so 16-bit-per-note MIDI 2.0 values fit without changing the clip format |
| MPE / per-note expression | prepared for (per-note channel, pitch bend per channel) | pitch bend is stored per note-channel and applied by the instrument |
| Virtual ports / loopback | out of scope for v1 | useful, not required, and needs a driver |

## Timing model

* **Everything musical is in ticks at 960 PPQN** (`time::kTicksPerBeat`), matching
  the timeline, the tempo map and the automation lanes. One number, one unit,
  everywhere — the worst class of bug in a DAW is a 960-vs-480 mismatch that only
  shows up on one machine.
* **Sample conversion happens once, at the block boundary.** The engine converts a
  block's `[startTicks, endTicks)` window to sample offsets via the tempo map and
  clamps them into the block, so notes start sample-accurately inside the block
  (`sampleOffset` on the message). No sub-block jitter, no separate MIDI clock.
* **Windows are half-open `[start, end)`.** A note-off exactly on the block
  boundary belongs to the next block. This is stated in the header because getting
  it wrong produces the classic "one stuck note per loop".

## The clip data model

```cpp
struct Note { int pitch, velocity, releaseVelocity; int64 startTicks, lengthTicks;
              int32 channel; bool selected; };
class MidiClip { std::vector<Note> notes_; std::vector<Controller> controllers_; };
```

* **Notes are stored, events are derived.** `collectEvents()` produces the note-on/
  note-off stream (plus CC and pitch bend) for a window; it also emits a note-off
  for a note that started *before* the window and is still sounding — the fix for
  stuck notes when the playhead enters a clip mid-note.
* **Collection order is allocation-free and stable**: `core::stableSortSmall`
  (insertion sort) replaces `std::stable_sort` on this path, because
  `std::stable_sort` allocates and stability is required for events that share a
  timestamp.
* **Quantisation** is strength-based (`QuantiseSettings{grid, strength}`), so 50 %
  moves a note halfway to the grid — the behaviour users expect from other DAWs
  rather than a hard snap.
* **Recording** uses `appendRecorded(Message)`, baking the incoming stream into
  notes; the clip is sorted on access, not on every insert.

## Instruments

* `midi::Instrument` is the internal API: `prepare`, `process(block, messages…,
  count, context)`, `isProducingSound()`, `allNotesOff`, `panic`.
* The built-in instrument is `aura.basic` — an original polyphonic subtractive
  synth (oscillator + filter + ADSR + voice stealing). No third-party instrument is
  bundled, ever: it would be a licensing problem and it would make AURA's default
  sound someone else's.
* **Voice stealing:** oldest-first, with a hard cap on voices; the oldest voice is
  re-used when the cap is reached. The sustain-pedal note stack is *capacity
  bounded* (`reserve(32)` in `prepare()`, guarded push) so releasing notes under a
  held pedal can never allocate on the audio thread — an explicit
  `// rt-audit: allow` annotation documents it, and the audit output lists it.

## Device routing

`MidiInputRouter` owns the device list and dispatches to tracks: a route is
`{inputDevice, inputChannel, outputDevice, outputChannel, enabled, thru}`.
`-1` means omni / internal instrument only. Panic (`allNotesOff` + CC 123 on every
route) is a first-class command because a stuck note is the single most annoying
failure a MIDI DAW has.

## Open questions

* SMF import/export (needed for interchange; the writer is straightforward, the
  reader needs tempo-map handling for SMPTE-based files).
* Windows MIDI Services (the modern MIDI 2.0 stack) once it is present on the
  minimum supported OS build.
* Recording latency compensation (MIDI timestamp to project position) — currently
  the input timestamp is used as-is; needs device latency from the audio backend.

## Sources

* MIDI Association, *MIDI 1.0 Detailed Specification* — message format, running
  status, channel messages, system exclusive.
* MIDI Association, *MIDI 2.0 / UMP specification* — why the message struct is
  deliberately wider than 7-bit-per-field.
* Microsoft, *MIDI Input/Output* (WinMM) — `midiInOpen`, `midiInStart`, timestamps.
* AURA tests: `tests/midi/MidiTests.cpp` (payloads, running status, quantise,
  sort/range, collect window, recording bake, routing, instrument lifecycle, voice
  stealing).
