# AURA DAW — Research Index

Every non-trivial technology, format and algorithm choice in AURA is written down
here *before* it is implemented. Each document records the alternatives that were
actually considered, the primary sources they were checked against, the decision,
and the consequences we accept by making it.

The rule for this folder: **no undocumented dependency, no undocumented format.**
If code depends on it, a document explains why it is the right choice and what it
costs us.

## Documents

| # | Document | Decides |
|---|----------|---------|
| 0 | [TECHNOLOGY_DECISION_RECORD.md](TECHNOLOGY_DECISION_RECORD.md) | The master list: language, compiler, build, licence, all accepted ADRs |
| 1 | [AUDIO_BACKEND_RESEARCH.md](AUDIO_BACKEND_RESEARCH.md) | WASAPI shared/exclusive, ASIO, device model, latency |
| 2 | [UI_FRAMEWORK_RESEARCH.md](UI_FRAMEWORK_RESEARCH.md) | JUCE vs Qt vs Dear ImGui vs first-party Win32/Direct2D |
| 3 | [PLUGIN_FORMAT_RESEARCH.md](PLUGIN_FORMAT_RESEARCH.md) | VST3 (MIT), CLAP (MIT), hosting and crash isolation |
| 4 | [DSP_ALGORITHM_RESEARCH.md](DSP_ALGORITHM_RESEARCH.md) | Filters, dynamics, delay, reverb, metering, loudness |
| 5 | [AUDIO_FILE_FORMAT_RESEARCH.md](AUDIO_FILE_FORMAT_RESEARCH.md) | Recording and export formats, dither, RF64 |
| 6 | [PROJECT_FORMAT_RESEARCH.md](PROJECT_FORMAT_RESEARCH.md) | The `.aura` container, atomic save, autosave, recovery |
| 7 | [WAVEFORM_CACHE_RESEARCH.md](WAVEFORM_CACHE_RESEARCH.md) | Multi-resolution peak pyramid (AUPZ v2) |
| 8 | [MIDI_RESEARCH.md](MIDI_RESEARCH.md) | MIDI 1.0/2.0, SMF, the clip data model, piano-roll timing |
| 9 | [THREADING_RESEARCH.md](THREADING_RESEARCH.md) | Thread ownership, lock-free comms, MMCSS, RT rules |
| 10 | [TESTING_RESEARCH.md](TESTING_RESEARCH.md) | Catch2 v3, test layers, how DSP correctness is judged |
| 11 | [BUILD_SYSTEM_RESEARCH.md](BUILD_SYSTEM_RESEARCH.md) | CMake presets, MSVC flags, options, dependency policy |
| 12 | [CI_RESEARCH.md](CI_RESEARCH.md) | GitHub Actions matrix, caching, static analysis |
| 13 | [PACKAGING_RESEARCH.md](PACKAGING_RESEARCH.md) | Inno Setup vs WiX vs MSIX, signing, install layout |
| 14 | [CRASH_RECOVERY_RESEARCH.md](CRASH_RECOVERY_RESEARCH.md) | Minidumps, autosave, recording preservation |
| 15 | [ACCESSIBILITY_LOCALIZATION_RESEARCH.md](ACCESSIBILITY_LOCALIZATION_RESEARCH.md) | UI Automation, keyboard-first, i18n catalogues |
| 16 | [PERFORMANCE_RESEARCH.md](PERFORMANCE_RESEARCH.md) | Latency budgets, CPU measurement, SIMD, denormals |
| 17 | [LATENCY_COMPENSATION_RESEARCH.md](LATENCY_COMPENSATION_RESEARCH.md) | Delay compensation: per-edge alignment, read-ahead vs. delay, what cannot be fixed |
| 18 | [SIMD_RESEARCH.md](SIMD_RESEARCH.md) | Applying SIMD to the mixing path: measured ceiling, what actually costs, block-state unrolling instead |

## Status vocabulary

* **Accepted** — implemented or being implemented; changing it needs a new entry.
* **Provisional** — decided, but a milestone still has to prove it (the doc says
  which one and by when).
* **Open** — genuinely undecided; the doc lists what would settle it.

## Related documentation

* [../ARCHITECTURE.md](../ARCHITECTURE.md) — module map and data flow
* [../AUDIO_ENGINE.md](../AUDIO_ENGINE.md) — the real-time engine, in detail
* [../TESTING.md](../TESTING.md) — the five test layers and how to run them
* [../PROJECT_FORMAT.md](../PROJECT_FORMAT.md) — the file format, as shipped
