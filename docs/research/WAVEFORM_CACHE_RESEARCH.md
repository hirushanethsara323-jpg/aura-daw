# WAVEFORM_CACHE_RESEARCH.md

**Status:** Accepted · **Code:** `aura::audio::WaveformCache` (`*.aurapeaks`, AUPZ v2)

## Question

How do we draw a 3-hour recording at 1 sample per pixel without touching the audio
file, and without a visible stall when the file is first opened?

## Approach

A **multi-resolution peak pyramid**, built asynchronously, stored in a sidecar file
next to the media, and discarded without loss whenever it is invalid.

| Level | Frames per bucket | Zoom it serves | Buckets for 1 h @ 48 kHz |
|-------|-------------------|----------------|---------------------------|
| 0 | 256 | sample-level detail | 675 000 |
| 1 | 512 (×2) | … | 337 500 |
| 2 | 1024 | … | 168 750 |
| n | 256 · 2ⁿ | capped at `kMaxLevels` | … |

* **Per-bucket payload:** min and max per channel (the two values a waveform
  actually needs) plus a coarse sum-of-squares for an optional RMS overlay.
  Storing peaks *and* RMS is what makes the drawing honest: peaks alone look
  identical for a quiet and a loud passage at low zoom.
* **`std::atomic<uint32_t> bucketsReady`** lets the UI draw a partially built
  waveform immediately — progressive display, never a blocking wait.
* The builder runs on a worker thread, is cancellable, and is *never* on the audio
  thread (asserted by `tools/rt_audit.py` and by the "no allocation on the audio
  path" regression test).
* **Deleting the sidecar is always safe.** The cache is keyed by file path, size,
  modification time, sample rate, channel count and format; any mismatch rebuilds
  it. Nothing in a project depends on the presence of `peaks/`.

## Why not store the peak data in the project manifest

A 3-hour stereo recording needs roughly 675 k × 2 channels × 3 floats per level,
tens of megabytes across levels. In JSON that would be an order of magnitude
worse and would have to be rewritten on every save. Sidecars keep the manifest
small and the cache disposable.

## Format (AUPZ v2)

```
magic "AUPZ" | version 2 | sampleRate | channels | frames | levelCount
per level: framesPerBucket | bucketCount | (min,max,rms per channel per bucket, float32)
```

* Little-endian, fixed-size records: memory-mappable and trivially readable.
* A version bump invalidates older caches cleanly (they are deleted and rebuilt).
* The reader is written to tolerate a truncated tail: a partially written cache
  after a crash is rebuilt rather than rejected with an error the user sees.

## Alternatives considered

| Option | Verdict |
|--------|---------|
| Recompute peaks from the audio on every open | Rejected — seconds of stall on a 3-hour file, and it reads the whole file into page cache for nothing |
| Two levels only (fine/coarse) | Rejected — zooming then rescans the coarse level every frame |
| Store the audio file's PCM in a cache and draw from that | Rejected — duplicates the media, no benefit over the pyramid |
| GPU-side mipmaps of a rendered image | Rejected — needs a full-image render first, which is the thing we are avoiding |

## Measured targets

| Property | Target | Test |
|----------|--------|------|
| Level 0 bucket size | exactly 256 frames | WaveformCache tests |
| Level n bucket size | 256 · 2ⁿ | "cache builds a pyramid" |
| Progressive availability | `bucketsReady` > 0 before the build finishes | tests + UI |
| Cache reuse | second open does not rebuild | mtime/size key check |
| Audio-thread safety | no allocation, no file I/O | `tools/rt_audit.py` |

## Accepted costs

* Disk: roughly 1.5–2 % of the audio file size per level; acceptable and always
  deletable.
* A file modified *without* changing its size or mtime (rare, but possible with
  some tools) keeps a stale cache. Combined with the media-pool metadata check
  (frames/rate/channels), this is a documented residual risk rather than a hidden
  one.
