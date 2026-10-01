# AUDIO_FILE_FORMAT_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0009 · **Code:** `aura::media` (`src/audio/AudioFile.cpp`)

## Question

What do we record, what do we export, and what do we claim to support?

## Decision summary

| Purpose | Format | Why |
|---------|--------|-----|
| Recording (takes) | **WAV, 24-bit PCM**, engine sample rate, project channel count | The brief's requirement; universally readable; 24-bit is the professional default for tracking. |
| Project media (imported) | read as-is: PCM 16/24/32, float 32/64 | Never convert on import: the project stores a *reference*, and the stream converts while reading. |
| Internal processing | **32-bit float, planar** | Headroom for gain staging, no clipping inside the graph, and it is what the DSP wants. |
| Export (master/stems) | WAV with user-selectable 16/24/32-bit PCM or 32-bit float, **TPDF dither at 1 LSB for integer depths** | Dither is part of a correct export, not a plugin. |
| Peak cache | sidecar `*.aurapeaks` (AUPZ v2) | see `WAVEFORM_CACHE_RESEARCH.md` |
| Project manifest | JSON | see `PROJECT_FORMAT_RESEARCH.md` |
| Compressed delivery formats (MP3/AAC/FLAC) | **not shipped** | Would require an encoder dependency and a licence review for no engine benefit. Documented gap. |

## Why WAV and not something else

* **WAV/RIFF is the only container a Windows DAW can write that every other tool
  reads without an argument.** BWF (`bext` chunk) is a WAV extension and stays
  compatible — the recording path writes a standard RIFF header, and the writer's
  `patchHeaderOnClose` flag exists so a crashed take can still be repaired by
  patching the sizes into the header afterwards.
* **RF64 / WAVE64** (for files > 4 GB) is planned, not implemented: 24-bit stereo
  at 48 kHz hits 4 GB after ~2.5 hours, which is longer than a session take in
  practice but not than a live recording. The format reader rejects what it cannot
  describe rather than mis-reporting a length.
* **Float WAV** is supported for import and export, and is the right choice for
  bounces that will be mastered elsewhere; the recording path stays integer
  because that is what the brief specifies and what users expect from a "24-bit
  recording".

## Conventions we follow

1. **Header first, sizes patched on close.** A recording that is interrupted is
   still a valid file (see `CRASH_RECOVERY_RESEARCH.md`), because the header is
   written with placeholder sizes and corrected when the take ends. A file whose
   header was never patched is recognisable, and the media pool can offer to
   repair it instead of discarding it.
2. **One conversion per boundary.** `AudioFileReader::readPlanar` converts once,
   into the float planar form the engine uses; nothing downstream re-converts.
   A test asserts bit-exactness for float files and LSB-bounded error for integer
   ones.
3. **Chunk reading is offset-correct.** `readFileToPlanar` advances the per-channel
   pointers per chunk. (This was a real bug: passing the base pointers every chunk
   overwrote earlier chunks and silently produced silence for longer files. The
   fix and its comment are in `src/audio/AudioFile.cpp`.)
4. **Non-finite input is refused, not propagated.** A float WAV containing NaN or
   Inf reads as 0.0 for that sample — a corrupt file must never be able to inject
   NaN into the graph or the master limiter.
5. **Bit depths are honest.** `floatToPcm24` etc. clamp to the representable range
   rather than wrapping; the export path clamps *before* quantising so no sample
   can wrap into the opposite polarity (an audible click that is very hard to
   diagnose later).
6. **Dither is deterministic by design.** The exporter uses a fixed-seeded TPDF
   generator, so rendering the same project twice produces the same file — which
   is what makes "did my export change?" a meaningful question and keeps export
   tests reproducible.

## Measured behaviour (what the tests assert)

| Property | Assertion |
|----------|-----------|
| 16/24/32-bit round trip | quantisation error ≤ 1 LSB of the chosen depth |
| 32-bit float round trip | bit-exact |
| Channel count, sample rate, length | reported correctly by `probeFile` after write |
| Chunked read of a 144 000-frame file | all three chunks intact (regression for the pointer bug) |
| Export of a 0.5 sine at 24-bit | peak within 0.02 of 0.5, no wraps |
| Interrupted take | file still openable, `recoverIncompleteTakes()` finds it |

## Accepted costs and gaps

* No RF64/WAVE64, no MP3/AAC/FLAC, no CAF/AIFF write (AIFF read is not
  implemented either — the reader is WAV-only and says so).
* No automatic sample-rate conversion on import: a 44.1 kHz file in a 48 kHz
  project plays at the wrong speed unless the clip is resampled, and AURA does not
  resample yet. The UI must state the mismatch; the engine refuses to pretend.
* Metadata written is minimal (no `bext` for BWF timestamps/channel maps yet).

## Sources

* Microsoft / IBM, *Multimedia Programming Interface and Data Specifications 1.0*
  (RIFF/WAVE) — the canonical WAV layout, `fmt ` and `data` chunks.
* EBU Tech 3285, *Specification of the Broadcast Wave Format (BWF)* — `bext`
  chunk, used for the planned BWF support (not yet implemented).
* ITU-R BS.1770 / EBU R128 literature on dither and noise shaping — TPDF at 1 LSB
  is the conservative default; noise shaping is deliberately *not* implemented
  (it is audible on further processing and would be dishonest to apply silently).
