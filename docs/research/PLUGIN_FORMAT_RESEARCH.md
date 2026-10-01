# PLUGIN_FORMAT_RESEARCH.md

**Status:** Accepted (hosting lands in M6/M7) · **Supports:** ADR-0007 · **Code:** `aura::plugin`

## Question

How does AURA host third-party instruments and effects — which format, with what
licence, with what crash story, and with what it exposes to the rest of the app?

## The October 2025 licensing change

This is the single most important fact in this document, and it inverted the
usual answer:

* **VST 3.8 (29 Oct 2025) moved the SDK from GPLv3+/proprietary to MIT.** Keeping
  the copyright and licence notice is the obligation; otherwise it can be used in
  any project, including closed ones.
* **ASIO moved to GPLv3-or-proprietary** in the same announcement. GPLv3 is
  compatible with AURA's GPL-3.0-or-later, so an ASIO backend is now possible
  without a signed agreement — but the SDK is still not something we redistribute.
* Before that date, VST3 hosting from a GPL application was a licensing maze and
  ASIO was outright GPL-incompatible; any research older than late 2025 that says
  otherwise is out of date.

## Format comparison

| Format | Licence | Ecosystem | Technical notes | AURA's use |
|--------|---------|-----------|-----------------|------------|
| **VST3** | MIT (since 3.8) | de-facto standard on Windows | COM-style interfaces, `IComponent`/`IEditController`, host-provided `IHostApplication`, parameter queues, sample-accurate automation with `ProcessData` | **Primary** |
| **CLAP** | MIT | growing, smaller base | pure C ABI, no platform dependencies, metadata readable *without* loading the binary (fast scan), audio-rate modulation, per-voice expressions, designed for thread-safety and multicore | **Secondary (M7)** |
| VST2 | proprietary, no new licences | legacy | cannot be legally distributed | Rejected |
| AAX / AU / LV2 | proprietary / permissive / permissive | other platforms | not needed on Windows | Rejected |
| Internal AURA instrument API | ours | n/a | `midi::Instrument` — the built-in poly synth and future devices | **Built-in path** |

## Decision

1. **`plugin::PluginHost` is the only surface the rest of AURA sees.** Scan,
   instantiate, chain, bypass, save state — nothing else in the tree includes a
   vendor header. When no SDK is configured, every entry point returns
   `ErrorCode::NotImplemented` with a message naming the build option that enables
   it (`-DAURA_ENABLE_VST3=ON`, SDK fetched at the pinned tag), never a crash and never a
   silent no-op. This is implemented and tested today.
2. **The SDK is an optional build input, never vendored into the repository.**
   Binary releases may link it (MIT permits it); the source tree keeps it out so
   that the licence text we ship is unambiguous.
3. **Scanning is metadata-first.** CLAP can be read without loading the module;
   VST3 needs a load, which is exactly why the scanner runs in a *separate
   process* with a timeout and a per-plugin result file. A plug-in that hangs or
   crashes the scanner is recorded as `blacklisted` with the reason, and stays
   visible in the browser (the user can re-enable it) — never silently vanishes.
4. **The database is a cache, not a source of truth.** `PluginDatabase` persists
   descriptors plus user data (favourite, custom name, last used, blacklist,
   tags) as JSON next to the user's other AURA state, and can always be rebuilt by
   rescanning. Search, favourites and blacklisting are already implemented and
   covered by tests (`tests/plugin/PluginTests.cpp`).
5. **Chains are explicit.** `PluginChain` owns instances, guarantees
   order-preserving processing, reports the summed latency, honours bypass, and
   skips instances the host has marked failed instead of calling into them again
   (tested: "a failed plug-in is skipped instead of being called again").
6. **State save/restore is versioned and opaque.** A chain serialises each
   instance's state to a base64/hex blob inside the project JSON, tagged with the
   plug-in's unique id and version, so a project that references a missing plug-in
   opens with a placeholder rather than failing.

## Crash isolation: what is designed, what is not built

The architecture assumes the plug-in sandbox is a **separate process**:

```
gateway (control thread)      audio thread                sandbox process
   scan/instantiate   →   lock-free command queue   →   plug-in instance
   state save/restore  ←  lock-free audio pipes    ←   DSP + message queues
```

* Not yet built (M7). The honest current status: an in-process host with a
  documented limitation — a plug-in that crashes the host takes AURA down with it,
  which is mitigated by autosave and crash recovery (`CRASH_RECOVERY_RESEARCH.md`)
  but not solved.
* What *is* built now: the scanner's process-isolation contract, the blacklist,
  the failed-instance skip path, and the fact that `PluginHost` never exposes
  vendor types — so swapping the transport underneath is a contained change.

## Budgets

* Scan: ≤ 200 ms per plug-in directory entry, hard timeout 5 s per binary, whole
  library under 60 s on a cold cache (background, cancellable, progress-reported).
* Instantiate: off the audio thread, always.
* Editor: opened on the UI thread; if the plug-in has no editor, AURA shows its
  own generic parameter list driven by `IPlugView`/`IAudioProcessor` parameters.
* Automation: parameters are addressed by id (VST3) or id + index (CLAP) and
  surfaced to the automation lanes as ordinary AURA targets.

## Consequences and accepted costs

* We implement parameter discovery, latency reporting and state blobs ourselves,
  including the cases where plug-ins report latency late (after the first
  `process` call) — the graph must be able to re-learn latency and re-publish.
* Two formats means two adapters; the shared surface is deliberately narrow.
* No sandbox yet means the crash story leans on recovery, and the docs must say so
  plainly rather than implying isolation exists.

## Sources

* librearts.org, *Steinberg relicenses VST3 and ASIO* (Nov 2025) — VST3 → MIT,
  ASIO → GPLv3-or-proprietary.
* KVR Audio news, *Steinberg moves the VST 3 SDK to MIT* — VST 3.8, 29 Oct 2025.
* heise.de, *Steinberg releases ASIO and VST under open source licences* —
  confirms the MIT terms and the retained proprietary option.
* Steinberg forum, *ASIO licence and open source software* — historical context
  (pre-2025: GPL-incompatible, signed agreement required) that the relicensing
  superseded.
* producergrid.com, *CLAP plugin format* — MIT, no fees/NDAs, per-voice note
  expressions, thread-safe multicore design.
* multitrackstudio.com, *CLAP plugins* — MIT confirmation and per-platform
  plug-in directories.
