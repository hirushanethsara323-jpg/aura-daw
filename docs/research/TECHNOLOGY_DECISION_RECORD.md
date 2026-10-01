# TECHNOLOGY_DECISION_RECORD.md

**Status:** Accepted · **Last revised:** 2026-10-01

The single place where AURA's technology decisions are recorded. Each entry says
what was decided, what was rejected, and why. The detailed reasoning and sources
live in the per-topic documents in this folder; this file is the index of record.

---

## ADR-0001 — Language and standard: C++20

* **Accepted.** C++20 on MSVC (Visual Studio 2022, `/std:c++20`), GCC 13+/Clang 16+
  for development and CI on Linux.
* **Why:** concepts, `<bit>`, `std::atomic` improvements, designated initialisers,
  `constexpr` containers and three-way comparison all earn their keep in a
  real-time engine; C++23 adds little we need and reduces compiler support.
* **Consequences:** no `std::expected` (C++23) — AURA has its own `AuraResult<T>`
  in `include/aura/core/Errors.hpp`. Modules are **not** used: MSVC module support
  is still not dependable for a project this size, and CMake's handling of them
  would add risk for no gain.

## ADR-0002 — Build system: CMake ≥ 3.24 with presets

* **Accepted.** Single top-level `CMakeLists.txt`, explicit source lists (no
  globbing), `CMakePresets.json` for Debug/Release/RelWithDebInfo.
* **Why:** the de-facto standard for C++ projects, first-class MSVC generator
  support, and `FetchContent` covers the small number of dependencies. Ninja is the
  preferred generator on Windows (`-G "Visual Studio 17 2022"` also supported).
* **Consequences:** adding a source file requires editing a list — deliberate, it
  keeps the build deterministic and greppable. See `BUILD_SYSTEM_RESEARCH.md`.

## ADR-0003 — Project licence: GPL-3.0-or-later

* **Accepted.** Every file carries `SPDX-License-Identifier: GPL-3.0-or-later` and
  the project copyright line.
* **Why:** AURA wants to stay open — including the engine — while remaining
  compatible with the licences of the tools it links (ASIO's GPLv3 branch, and the
  MIT/BSL dependencies which are GPL-compatible). It also keeps the project out of
  the business of relicensing third-party code.
* **Consequences:** see ADR-0005/0006 for JUCE and ASIO. Any dependency that is not
  GPL-compatible cannot be linked, however convenient. The licence audit lives in
  `../LICENSES.md`.

## ADR-0004 — Test framework: Catch2 v3 (BSL-1.0)

* **Accepted.** Catch2 v3.7.1 via `FetchContent`, one test binary per area plus a
  shared support library.
* **Rejected:** GoogleTest (slower to compile, heavier, GPL-unfriendly only in the
  sense of bulk), doctest (faster, but Catch2's matchers, sections and generators
  are worth more to us than the compile-time difference at this size — the
  measured gap between doctest and Catch2-as-a-static-library is small).
* **Consequences:** requirement of a network fetch on first configure, with a
  documented offline path (`AURA_FETCH_DEPS=OFF` + a local Catch2 checkout).

## ADR-0005 — Audio device layer: first-party WASAPI + ASIO backends

* **Accepted.** `IAudioDevice` abstraction with WASAPI shared, WASAPI exclusive,
  ASIO and a mock backend. No third-party audio I/O dependency (no RtAudio, no
  PortAudio, no JUCE device layer).
* **Why:** the requirement is Windows-only output with per-device capability
  queries, exclusive-mode format negotiation, hot-plug and sample-rate-change
  recovery — exactly the places where a wrapper library hides the information we
  need. It also keeps the licence surface small.
* **Consequences:** we own the Win32 COM code and its failure modes (documented in
  `AUDIO_BACKEND_RESEARCH.md`); ASIO is opt-in (`AURA_ENABLE_ASIO=OFF` by default)
  because its SDK is not vendored.

## ADR-0006 — UI framework: Win32 + Direct2D first-party shell; JUCE optional, off by default

* **Provisional (M9/M10 decides the final shape).**
* **Why:** JUCE is dual-licensed AGPLv3/commercial. AGPLv3 is *stronger* than our
  GPL-3.0-or-later: GPLv3 §13 permits linking the two, but the combined work then
  carries the AGPL network clause, and the project could no longer describe itself
  as "GPL-3.0-or-later" alone. Combined with a 117 MB dependency and the
  requirement for an original AURA visual identity, a first-party Win32 +
  Direct2D/DirectWrite shell is the better default. JUCE remains available as a
  **build option** for the plugin-editor hosting adapter if that proves necessary.
* **Rejected:** Qt (LGPL + commercial, large runtime, licence churn), Dear ImGui
  (immediate mode redraws the whole window every frame — wrong for a DAW that must
  stay cool on modest PCs, and skinning it into an original identity is hard).
* **Consequences:** the shell and widget toolkit are ours to write and test; the
  engine must stay framework-free (it already is). See `UI_FRAMEWORK_RESEARCH.md`.

## ADR-0007 — Plugin formats: VST3 first, CLAP optional; MIT-safe path

* **Accepted.** VST3 (SDK MIT since 3.8, Oct 2025) is the primary format; CLAP
  (MIT) is the secondary. Hosting is behind `plugin::PluginHost` so the SDK is an
  optional build input, never vendored.
* **Why:** VST3 has the ecosystem and now has a permissive licence; CLAP is
  technically cleaner (C ABI, more metadata, thread-safe by design) but has a
  smaller installed base.
* **Consequences:** scanning and out-of-process isolation are designed for, not
  yet built; the limitation is documented rather than hidden. See
  `PLUGIN_FORMAT_RESEARCH.md`.

## ADR-0008 — Project format: a directory with JSON manifest and sidecar caches

* **Accepted.** `<name>.aura/` with `project.json`, `media/`, `peaks/`, `cache/`,
  `backups/`, and an atomic write + `.bak` rotation.
* **Why:** media stays where the user put it, the manifest is diffable and
  recoverable by hand, and cache directories can be deleted without data loss.
  A single opaque blob would be easier to write and much worse to live with.
* **Consequences:** path handling (relative vs absolute, relinking) becomes a real
  feature with real edge cases. See `PROJECT_FORMAT_RESEARCH.md`.

## ADR-0009 — Internal processing format: 32-bit float, planar, non-interleaved

* **Accepted.** Recording is 24-bit PCM WAV; everything inside the engine is
  `float` planar at the device sample rate.
* **Why:** float has the headroom for gain staging and the denormal handling is
  well understood; planar is what the DSP kernels and the graph want. Conversion
  happens once, at the file boundary (`aura::media`).
* **Consequences:** no internal sample-rate conversion yet — a mismatch is refused
  with an explicit error instead of silently resampling (documented limitation).

## ADR-0010 — Real-time rules are static-checked *and* unit-tested

* **Accepted.** Allocation, locks, file I/O and logging are banned on the audio
  thread; `tools/rt_audit.py` enforces this statically (run as the ctest
  `aura.static.realtime_audit`), and `tests/engine/EngineTests.cpp` renders a block
  with the allocation counter armed.
* **Why:** a rule that is only written down gets broken. Two independent
  mechanisms — one static, one dynamic — turn "we are careful" into a build gate.
* **Consequences:** the static audit is heuristic (it says so), so findings can be
  escaped with a justified `// rt-audit: allow - <reason>` annotation, and the two
  documented escape hatches are visible in the audit output. See
  `THREADING_RESEARCH.md`.

## ADR-0011 — Dependencies: minimal, documented, vendored by fetch not by copy

* **Accepted.** Production dependencies: the C++ standard library and the Windows
  SDK. Development-only: Catch2 v3 (BSL-1.0). Optional: VST3 SDK (MIT), CLAP (MIT),
  ASIO SDK (GPLv3 / commercial, never vendored), JUCE (AGPLv3 / commercial, off by
  default).
* **Why:** every dependency is a licence obligation, an upgrade chore and a build
  risk; the engine needs remarkably few.
* **Consequences:** anything new must be justified in `../LICENSES.md` (purpose,
  version, licence, maintenance status, build and runtime impact) before it lands.

## ADR-0012 — Versioning, branching, releases

* **Accepted.** Semantic version in `include/aura/core/Version.hpp`; milestones
  M0–M12 as GitHub milestones; feature branches, no force-push to `main`, tests
  green before merge.
* **Why:** the master plan asks for a professional history, and a version number
  that other code can read.
* **Consequences:** the project format version (`AURA_PROJECT_FORMAT_VERSION`) is
  versioned separately from the application, and opening a newer project in an
  older build is refused with a clear message rather than half-loaded.

## ADR-0013 — One warning contract for both toolchains

* **Accepted.** The MSVC and GCC flag sets are kept aligned, warnings are errors in
  CI (`/WX`, `-Werror`), and the only suppression in the tree is a scoped
  `#pragma warning(disable : 4324)` around the two cache-line-padded lock-free
  classes (`SpscQueue`, `AudioRingBuffer`).
* **Why:** the first CI run was green on the "Repo hygiene" job and red on every
  build job — with failures that could not appear locally: GCC 13's `-Wshadow`
  rejecting `using Array = …` next to a `Type::Array` enumerator (GCC 14 accepts
  it), MSVC C4324 padding, and `GetCurrentThreadId` without `<windows.h>`. Two
  hours of CI were spent proving that "builds on my machine" is not evidence.
* **Research:** Microsoft's own pages define the levels we now design for — C4324
  is level 4 and fires on any `alignas`/`__declspec(align)` that pads a class
  ([C4324](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4324)),
  C4127 does *not* fire for trivial literals such as `while (false)` since VS2015
  update 3, which keeps the `do { … } while (false)` macro idiom safe
  ([C4127](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4127)),
  and C4100 (unreferenced formal parameter) is reported at `/W4`, so
  `-Wunused-parameter` stays **on** for GCC — a warning Linux forgives is a build
  break on Windows.
* **Rejected:** a global `/wd4324` or `/wd4100` in `CMakeLists.txt` (hides the next
  real instance in unrelated code), lowering `/W4`, and turning warnings-as-errors
  off in CI. Also rejected: naming locals after members and silencing `-Wshadow`
  instead of renaming the local.
* **Consequences:** a new suppression must be a scoped `pragma push/pop` with a
  comment saying why the warning is expected, plus an entry here. The rule has a
  second, mechanical half: `tools/include_audit.py` (ctest
  `aura.static.include_audit`) fails on a file that uses a standard type without
  including its header, because libstdc++ supplies `std::mutex`, `std::thread`,
  `std::chrono` and others transitively and MSVC's STL does not — the exact way the
  *second* Windows CI run failed after the first fix landed. Both toolchains
  are in the matrix, so an asymmetry is caught by the first push rather than by a
  user. `docs/DEVELOPMENT.md` documents the contract for contributors.

---

## Open decisions

| # | Question | Needed by | What settles it |
|---|----------|-----------|-----------------|
| O-1 | Out-of-process plug-in sandbox: child process vs. JUCE-style plugin host process | M7 | measured crash rate + IPC latency budget |
| O-2 | VST3 SDK: statically linked and vendored into the build, or loaded through an optional adapter module | M6 | whether the MIT text can be redistributed in binary releases as we intend |
| O-3 | MSIX in addition to an Inno Setup EXE | M11 | whether the Store/sideload story is worth the signing cost |
| O-4 | SIMD path (SSE2/AVX2) for the graph and EQ, or leave it to the compiler | M5 | benchmark results in `bench/` on a modest machine |
| O-5 | Whether to ship an internal sample-rate converter | after M5 | user reports; until then a mismatch is an explicit error |
