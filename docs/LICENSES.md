# LICENSES.md

AURA DAW is **GPL-3.0-or-later**. This file is the dependency and licence audit
required by the project's own rules (ADR-0011): every dependency states its purpose,
version, licence, maintenance status, build impact and runtime impact — or it does
not get added.

## The project

| Item | Value |
|------|-------|
| Licence | `GPL-3.0-or-later` (`SPDX-License-Identifier: GPL-3.0-or-later`) |
| Copyright | `Copyright (c) 2026 The AURA DAW Project` |
| Full text | [`../LICENSE`](../LICENSE) (GNU GPL v3, 29 June 2007) |
| Source offer | the repository itself; release tags ship with a matching source tarball |

## Production dependencies

| Dependency | Version | Licence | Purpose | Maintenance | Build impact | Runtime impact |
|------------|---------|---------|---------|-------------|--------------|----------------|
| **C++ standard library** | toolchain | permissive (runtime exception for GCC/Clang; MSVC redistributable) | everything | n/a | none beyond the compiler | linked runtime |
| **Windows SDK / Win32 APIs** | 10.0.19041+ | Microsoft SDK terms (redistributable components) | WASAPI, ASIO host side, file I/O, Direct2D/DirectWrite (M9) | n/a | none beyond the OS SDK | OS components |

That is the entire production list. AURA's engine has no third-party runtime
dependency — a deliberate consequence of ADR-0005 (own device layer) and ADR-0006
(first-party UI).

## Development-only dependencies

| Dependency | Version | Licence | Purpose | Maintenance | Build impact | Runtime impact |
|------------|---------|---------|---------|-------------|--------------|----------------|
| **Catch2** | 3.7.1 | BSL-1.0 | unit/integration/regression tests | active (catchorg) | fetched by `FetchContent`, compiled as a static library (moderate) | test binary only, never shipped |
| **MinGW-w64** (`g++-mingw-w64-x86-64`) | 12.0.0 headers / GCC 14 | GPL-3.0-or-later with the GCC runtime exception, plus public-domain-ish Windows headers | cross-checks the Windows-only code (`tools/win_crosscheck.sh`, CI job "Windows cross-check") on Linux | active (mingw-w64 project) | not part of any AURA build; installed only in that CI job and optionally on a developer machine | none — nothing it produces is linked or shipped |

## Optional dependencies (off by default, never vendored)

| Dependency | Licence | Enabled by | Notes |
|------------|---------|-----------|-------|
| **VST 3 SDK** | **MIT** since VST 3.8 (29 Oct 2025); AURA pins `v3.8.1_build_84` | `AURA_ENABLE_VST3=ON` (fetched by `FetchContent` at the pinned tag, shallow, four submodules) or `-DFETCHCONTENT_SOURCE_DIR_VST3SDK=<dir>` for a local checkout | MIT permits binary distribution with the notice retained; the SDK is *not* committed to the repository. Four SDK sources a host needs but the SDK's libraries do not contain (`plugprovider.cpp`, `memorystream.cpp`, the platform module loader) are compiled into `aura_vst3_sdk_bridge`; see [`PLUGIN_HOST.md`](PLUGIN_HOST.md) |
| **CLAP** | **MIT** | (M7) | pure C ABI, no platform dependency |
| **ASIO SDK** | **GPLv3 or proprietary** since Oct 2025 | `AURA_ENABLE_ASIO=ON` + `AURA_ASIO_SDK_DIR` | GPLv3 is compatible with this project's licence; before Oct 2025 this required a signed agreement, which is why older advice says it is impossible |
| **JUCE** | **AGPLv3 or commercial** | `AURA_WITH_JUCE=ON` (not default) | AGPLv3 is *stronger* than GPL-3.0-or-later: builds that enable it must state that they include AGPLv3 code. The default build and CI prove the tree works with JUCE absent (ADR-0006) |

## Deliberately **not** used

| Thing | Why not |
|-------|---------|
| JUCE as the default framework | AGPLv3 obligations + 117 MB + a UI identity that would look like every other JUCE DAW (ADR-0006) |
| Qt | LGPL/commercial, large runtime, licence churn |
| RtAudio / PortAudio / miniaudio | they hide exactly the per-device capability detail AURA needs (ADR-0005) |
| Bundled third-party instruments or sample content | not our copyright to ship |
| MP3/AAC/FLAC encoders | licence review + dependency cost for no engine benefit |
| RIFF/format libraries (libsndfile etc.) | the WAV subset AURA needs is ~800 lines and already tested |

## Attribution rules

1. Third-party notices required by a licence (MIT text for VST3, BSL-1.0 for Catch2)
   are collected in the installer's `licenses/` directory and listed in the About
   box. A build with `AURA_ENABLE_VST3=ON` links Steinberg's MIT-licensed SDK, so its
   notice ships with any binary built that way - the source is fetched at build time,
   which keeps this repository clean but does not remove the notice requirement.
2. No proprietary code is copied, and no commercial DAW is reverse-engineered; UI
   layout, icons, themes and the built-in instrument are original work.
3. Copyleft compatibility is checked *before* a dependency is added, not after a
   release.
4. The dependency table above is updated in the same commit that adds, removes or
   upgrades a dependency.

## A note on the ASIO/VST3 relicensing

Before October 2025 the common advice was that ASIO was GPL-incompatible and VST3
was GPLv3-or-proprietary — both true at the time, both changed by Steinberg's
relicensing (VST 3.8 → MIT; ASIO → GPLv3-or-proprietary with the commercial option
retained). This file records the current position; any document that contradicts it
is older than the change and should be corrected rather than followed.
