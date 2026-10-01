# BUILD_SYSTEM_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0002 · **Code:** `CMakeLists.txt`, `CMakePresets.json`

## Question

How is the tree configured and built so that a Windows developer, a CI runner and
a Linux contributor all get the same result, and so that the engine can be built
and tested without the GUI toolchain?

## Shape of the build

```
aura_core    (always)   errors, math, json, log, fs, realtime, strings, localization
aura_dsp     (always)   15 processors + biquad/smoothing primitives
aura_engine  (always)   graph, streams, media, transport, audio, midi, project,
                        commands, plugin host surface, automation, timeline,
                        device manager (WASAPI/ASIO/mock), engine, offline render
aura_app     (optional) Win32/Direct2D shell - Windows only
aura_tests   (optional) one Catch2 binary + tests/support
aura_bench   (optional) micro-benchmarks
```

`aura_engine` is a single library on purpose: the graph, the device manager and the
engine have a genuine three-way dependency (the engine drives the graph, the device
drives the engine, the graph must know the device's block size), and splitting it
would mean either circular libraries or an interface layer that exists only to
satisfy the build. The *module boundaries* are enforced by directory structure and
include rules, not by link boundaries.

## Options (all default to a build a contributor can run without extra downloads)

| Option | Default | Meaning |
|--------|---------|---------|
| `AURA_BUILD_TESTS` | OFF | build the Catch2 suite and register ctest entries |
| `AURA_BUILD_APP` | OFF | build the Windows shell (warns and skips on non-Windows) |
| `AURA_BUILD_BENCHMARKS` | OFF | micro-benchmarks (`bench/`) |
| `AURA_FETCH_DEPS` | ON | fetch Catch2 with `FetchContent`; OFF uses a local checkout |
| `AURA_ENABLE_ASIO` | OFF | compile the ASIO backend (needs `AURA_ASIO_SDK_DIR`) |
| `AURA_ENABLE_VST3` | OFF | compile the VST3 adapter (needs `AURA_VST3_SDK_DIR`) |
| `AURA_WITH_JUCE` | OFF | optional JUCE-based plug-in editor adapter (AGPLv3!) |
| `AURA_ENABLE_ALLOC_TRACKING` | per config | global `operator new` counter used by the RT regression test |
| `AURA_ENABLE_RT_ASSERTS` | per config | asserts that fail when an RT rule is broken |
| `AURA_WARNINGS_AS_ERRORS` | ON for CI | `/W4 /WX`, `-Wall -Wextra -Werror` |
| `AURA_ENABLE_SANITIZERS` | OFF | ASAN + UBSAN (Clang/GCC; MSVC ASAN separately) |

Defaults are chosen so that `cmake -S . -B build && cmake --build build` produces
the three libraries with no network access and no SDKs.

## Compiler settings worth explaining

* **MSVC:** `/std:c++20 /permissive- /W4 /MP /Zc:__cplusplus /EHsc`, `/fp:precise`
  (not `/fp:fast`) — audio DSP must not be re-associated by the optimiser; the
  tests compare against analytic values and `/fp:fast` would make those comparisons
  lie. `/GL` + `/LTCG` in Release only.
* **GCC/Clang:** `-Wall -Wextra -Wshadow -Wconversion`-lite (full `-Wconversion`
  is too noisy around `float`/`double` in DSP code, so the specific conversions we
  care about are covered by tests), `-ffp-contract=off` for the same
  reproducibility reason as `/fp:precise`.
* **`NDEBUG`** is *not* used to remove the RT asserts: `AURA_ENABLE_RT_ASSERTS` is
  independent, so a Release build can still catch a violation in a test run.
* **Explicit source lists, no globbing.** A `file(GLOB)` build silently changes
  when a file is added or renamed; a missing file then produces a link error that
  is hard to attribute. The lists are long and boring on purpose. (CMake also
  warns that globbing cannot detect additions without re-running configure.)

## Presets

`CMakePresets.json` carries `debug`, `release`, `relwithdebinfo`, `ci-linux` and
`ci-windows`, each setting generator, binary directory and the options above, so
`cmake --preset release && cmake --build --preset release` reproduces CI locally.

Presets also solve the case that previously broke configuration: a Wedged
`build/` cache from a failed generate. The rule is documented in
`../BUILDING.md`: presets use their own binary directories, and a stale directory
can always be deleted without losing source changes (`rm -rf build` /
`Remove-Item -Recurse build`).

## Dependency policy (ADR-0011 in practice)

Any new dependency must be added to `../LICENSES.md` with: purpose, exact version
and upstream URL, licence, maintenance status (last release, activity), build
impact (compile time, artefact size), runtime impact (memory, threads, disk), and
the reason a first-party implementation was not chosen. The current production
dependency list is deliberately short: **the C++ standard library and the Windows
SDK**. Everything else is development-only (Catch2) or optional and off by default
(VST3, CLAP, ASIO, JUCE).

## Accepted costs

* Long explicit source lists are annoying to maintain by hand; the payoff is a
  build that cannot silently change shape.
* Requiring an option (`AURA_ENABLE_ASIO`) for a backend means a user's build can
  differ from ours; the About box and `--version` therefore report the enabled
  backends and plugin formats.
* `FetchContent` for Catch2 means the first configure needs network access; the
  offline fallback is documented and exercised by the CI "deps off" job.
