# PROJECT_FORMAT.md

The `.aura` format as shipped. Research, alternatives and the reasoning:
[`research/PROJECT_FORMAT_RESEARCH.md`](research/PROJECT_FORMAT_RESEARCH.md).

**Format version:** 1 (`AURA_PROJECT_FORMAT_VERSION` in
`include/aura/core/Version.hpp`) — independent of the application version.

## Layout

```
MySong.aura/
  project.json          the manifest (see below)
  media/                recordings AURA created (takes)
  peaks/                *.aurapeaks sidecars - regenerable, safe to delete
  cache/                waveform/scan/thumbnail caches - regenerable
  backups/              project.json.bak + older generations
  crash.flag            present while a session is open; removed on clean exit
```

* Imported media is **referenced, not copied**; only AURA's own recordings live in
  `media/`.
* `peaks/` and `cache/` may be deleted at any time; the next open rebuilds them.
* `media/` is data. Never delete it by hand and never let a tool write into it.

## Manifest structure

```jsonc
{
  "header": {
    "formatVersion": 1,
    "appVersion": "0.1.0",
    "projectId": "…",
    "created": "2026-10-01T09:00:00Z",
    "modified": "2026-10-01T09:12:31Z",
    "sampleRate": 48000,
    "tempoMap":    [ { "positionTicks": 0, "bpm": 120 } ],
    "timeSignatures": [ { "positionTicks": 0, "numerator": 4, "denominator": 4 } ]
  },
  "tracks": [
    { "id": 1, "kind": "audio", "name": "Guitar", "colour": "#c8a24a",
      "volume": 0.8, "pan": -0.2, "mute": false, "solo": false,
      "destination": 0, "sends": [], "inserts": [
        { "name": "EQ", "processor": "aura.eq", "state": "…", "bypassed": false } ] }
  ],
  "clips": [
    { "id": 12, "type": "audio", "track": 1, "start": 0, "length": 48000,
      "sourceOffset": 0, "gain": 1.0, "fadeIn": 0, "fadeOut": 0,
      "stretchRatio": 1.0, "pitchSemitones": 0, "media": 3 }
  ],
  "media": [
    { "id": 3, "path": "media/Take 1.wav", "originalPath": "D:/rec/Take 1.wav",
      "frames": 48000, "sampleRate": 48000, "channels": 2, "bits": 24,
      "isRecording": true }
  ],
  "automation": [ { "target": "track.1.volume", "points": [], "shape": "linear",
                    "mode": "read" } ],
  "markers": [ { "positionTicks": 3840, "name": "Chorus", "colour": "#5fa8d3" } ],
  "mixer": { "masterGain": 1.0, "limiter": { "ceilingDb": -0.3, "lookaheadMs": 1.5 } },
  "view": { "samplesPerPixel": 512, "scrollSamples": 0, "trackHeights": [] }
}
```

* `view` is cosmetic and may be lost without harm.
* `stretchRatio` and `pitchSemitones` are stored but **do not affect DSP yet** —
  they are reserved so a future build does not need a format change. This is stated
  here so nobody assumes time-stretching exists.
* Unknown keys are tolerated on load and preserved; an older build opening a newer
  file does not destroy fields it does not understand.

## Save safety

1. Write `project.json.tmp`, flush, close.
2. Rotate the current manifest into `backups/` (`project.json.bak`, then `.1`, `.2`).
3. Replace atomically (`MoveFileEx(..., MOVEFILE_REPLACE_EXISTING |
   MOVEFILE_WRITE_THROUGH)`).

Consequences: a power cut leaves the previous manifest valid; recovering is a file
copy, not an archaeology project. Save-as onto an existing path asks for
confirmation; opening a project never modifies it.

## Autosave and recovery

* A background timer (configurable) writes to `cache/`, never over the user's file.
* `crash.flag` distinguishes "AURA died" from "AURA was closed".
* On open, if the flag is present **and** an autosave is newer, the user is offered
  the recovery copy, with both timestamps shown. AURA never silently replaces their
  file.
* Recordings are separately recoverable: takes are valid WAV files from the first
  second, and `recoverIncompleteTakes()` offers them after a crash.

## Compatibility

* A manifest with a **higher** `formatVersion` is refused with a message naming both
  versions. A half-loaded session is worse than an honest refusal.
* Migrations are one function per version step, each with a checked-in fixture and a
  test.
* `formatVersion` is bumped for any change an older build could misinterpret:
  adding an optional field is not a bump; removing or repurposing one is.
