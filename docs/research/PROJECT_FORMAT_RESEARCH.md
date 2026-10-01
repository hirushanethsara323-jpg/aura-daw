# PROJECT_FORMAT_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0008 · **Code:** `aura::project`, format version in `core/Version.hpp`

## Question

How is a session stored so that it is safe against crashes, honest about media,
diffable by hand when something goes wrong, and forward-compatible enough to
survive a few years of feature work?

## Options

| Option | Crash safety | Media handling | Tooling | Verdict |
|--------|--------------|----------------|---------|---------|
| **Directory with JSON manifest + sidecars** | atomic write + `.bak` + autosave | media referenced, never copied implicitly | `jq`, text editors, git | **Accepted** |
| Single binary blob (e.g. SQLite/proprietary) | good with WAL | opaque | needs our tools to inspect | Rejected — worse failure mode, harder to support |
| Single JSON *file* with embedded media (base64) | bad (large file rewritten per save) | terrible for 500 MB of audio | readable but impractical | Rejected |
| XML manifest | fine | same as JSON | heavier, no benefit | Rejected |
| One file per track + a manifest (Reaper-ish) | fine | fine | decent | Rejected — more files to keep in sync; JSON + media is simpler |

## Layout

```
MySong.aura/
  project.json         the manifest: tempo map, tracks, clips, mixer, markers, automation
  media/               recordings AURA created (takes), copied in only when the user asks
  peaks/               one .aurapeaks sidecar per media file (regenerable)
  cache/               waveform/thumbnail/scan caches (regenerable, safe to delete)
  backups/             rotated copies: project.json.bak, project.json.1, .2, ...
  crash.flag           written while a session is open, removed on clean exit
```

Key rule: **`media/` and `cache/` are never the only copy of anything a user
recorded elsewhere.** Imported files stay where they are and are referenced; only
AURA's own recordings land in `media/`.

## Save discipline (the part that matters)

1. **Write to a temporary file, then rename.** `FileSystem::writeTextFileAtomic`
   writes `project.json.tmp`, flushes, closes, and only then replaces the target.
   On Windows the replace is `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING |
   MOVEFILE_WRITE_THROUGH)`; a power cut mid-save leaves the previous manifest
   intact.
2. **Rotate a backup before replacing.** The previous manifest becomes
   `backups/project.json.bak` (and two older generations are kept). Recovering is
   then a copy, not an archaeology project.
3. **Refuse silent overwrite.** Save-as onto an existing path asks; opening a
   project never mutates it until the first explicit save.
4. **Refuse to half-open a future project.** A manifest with
   `formatVersion > AURA_PROJECT_FORMAT_VERSION` is refused with a clear message
   naming both versions; a *newer minor* within the same major may load with a
   warning if and only if the parsing rules allow it. (Tested: "a newer project
   format is refused rather than half-loaded".)
5. **Autosave is independent of the user's save.** A background timer
   (`Autosave::setIntervalSeconds`) writes to the cache directory, never over the
   user's file; if the app dies, `Autosave::recoveryAvailable()` reports true and
   the user is offered the recovery copy. The crash flag is what distinguishes
   "AURA died" from "AURA was closed"; a clean exit removes it.
6. **Media is verified, not assumed.** The media pool stores the path, the frame
   count, the sample rate, the channel count and the bit depth it saw at import.
   If `probeFile` reports something different on open, the clip is flagged as
   *relinkable* rather than silently misaligned — and `PluginChain`-style state for
   audio clips (offset/length) stays valid because the clip stores sample values,
   not seconds.

## Manifest contents (v1)

| Section | Contains |
|---------|----------|
| `header` | format version, app version, project id, created/modified timestamps, sample rate, tempo map, time-signature map |
| `tracks` | id, kind (audio/instrument/midi/bus/aux/master), name, colour, volume, pan, mute, solo, routing destination, sends, inserts (name + processor id + state blob) |
| `clips` | id, type (audio/midi), track, start, length, source offset, gain, fades, stretch ratio, pitch semitones, media id or MIDI payload |
| `media` | pool entries: id, path (relative when possible), original absolute path, frames, sample rate, channels, bits, `isRecording` |
| `automation` | lanes: target id, points, curve shape, recording mode |
| `markers` | position (ticks or samples), name, colour |
| `mixer` | master gain, limiter settings, meter/preset flags |
| `view` | zoom, scroll, track heights, selection — cosmetic, safe to lose |

Stretch ratio and pitch are stored by design and **do not yet affect DSP**; the
document says so in `../PROJECT_FORMAT.md` too, so a user reading a file does not
have to guess.

## Compatibility policy

* `formatVersion` is a single integer, bumped on any change that an older build
  could misinterpret.
* Additive fields are tolerated (unknown keys are preserved on load and written
  back only by the build that understands them — i.e. an old build that opens a
  new file leaves the unknown keys alone).
* Removing or repurposing a field requires a version bump; the loader keeps a
  migration function per version step, and every migration has a test with a
  checked-in fixture.

## Accepted costs

* A directory is easier to corrupt by hand (users *will* delete `peaks/`); every
  regenerable directory is therefore labelled as such in the manifest and in
  `../PROJECT_FORMAT.md`.
* JSON is bigger than a binary blob and slower to parse for very large sessions;
  measured acceptable (< 30 ms for a 24-track project), and it buys hand-recovery
  and diffability.
* Long-term we may add a binary `cache/session.bin` for peak/scan state, but the
  manifest stays JSON — that is the point of the split.

## Sources

* Microsoft, *MoveFileEx* — atomic replace semantics on Windows; the reason the
  atomic-write helper uses `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.
* RFC 8259, *JSON* — the manifest's interchange format, chosen over XML for size
  and tooling.
* AURA's implementation and tests: `tests/project/ProjectTests.cpp` (round trip,
  collision refusal, newer-format refusal, `.bak` recovery, media relink,
  autosave, localisation of user-facing strings).
