# BUILDING.md

## Requirements

| Need | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.24 | 4.x works |
| C++ compiler | MSVC 19.3x (VS 2022), GCC 13+, Clang 16+ | C++20 required |
| Generator | Ninja (preferred) or Visual Studio 17 2022 | |
| Python 3 | optional | only for `tools/rt_audit.py` (the RT-safety ctest) |
| Windows SDK | 10.0.19041+ | for the device layer and the shell |
| Network | first configure only | fetches Catch2 when tests are enabled |

No SDK is required for a default build: VST3, CLAP, ASIO and JUCE are all optional
and off.

## The three commands

```bash
cmake -S . -B build -DAURA_BUILD_TESTS=ON
cmake --build build -j 2
cd build && ctest --output-on-failure
```

Windows (PowerShell), with presets:

```powershell
cmake --preset ci-windows
cmake --build --preset ci-windows
ctest --test-dir build-ci-windows --output-on-failure
```

## Options

| Option | Default | Effect |
|--------|---------|--------|
| `AURA_BUILD_TESTS` | OFF | build the Catch2 suite, register ctest entries (including the RT audit) |
| `AURA_BUILD_APP` | OFF | build the Windows shell (`app/`, M9); warns and skips on non-Windows |
| `AURA_BUILD_BENCHMARKS` | OFF | build `bench/` |
| `AURA_FETCH_DEPS` | ON | fetch Catch2 via `FetchContent`; OFF uses `AURA_CATCH2_DIR` |
| `AURA_CATCH2_DIR` | — | path to a local Catch2 v3 checkout (used when `AURA_FETCH_DEPS=OFF`) |
| `AURA_ENABLE_ASIO` | OFF | compile the ASIO backend; needs `AURA_ASIO_SDK_DIR` |
| `AURA_ENABLE_VST3` | OFF | compile the VST3 adapter; needs `AURA_VST3_SDK_DIR` |
| `AURA_WITH_JUCE` | OFF | JUCE-based plug-in editor adapter (**AGPLv3** — read ADR-0006 first) |
| `AURA_WARNINGS_AS_ERRORS` | OFF | `/W4 /WX` on MSVC, `-Wall -Wextra -Werror` elsewhere |
| `AURA_ENABLE_SANITIZERS` | OFF | ASAN + UBSAN (GCC/Clang) |
| `AURA_ENABLE_ALLOC_TRACKING` | ON in Debug/tests | global allocation counter used by the RT regression test |
| `AURA_ENABLE_RT_ASSERTS` | ON in Debug/tests | fails loudly when an RT rule is broken |

Changing a source list: the libraries use **explicit file lists** in
`CMakeLists.txt` (no globbing). Adding `src/foo/Bar.cpp` means adding it to the
right list; a missing entry shows up as an undefined symbol at link time.

## Offline / air-gapped builds

```bash
git clone --branch v3.7.1 --depth 1 https://github.com/catchorg/Catch2.git /opt/catch2
cmake -S . -B build -DAURA_BUILD_TESTS=ON \
      -DAURA_FETCH_DEPS=OFF -DAURA_CATCH2_DIR=/opt/catch2
```

`AURA_CATCH2_DIR` takes either an absolute path or one **relative to the repository
root** — CI uses `../catch2` for a sibling checkout, which keeps the path identical
on Linux and on a Windows runner. It must point at a Catch2 *source* checkout
(`add_subdirectory` is used, not `find_package`, because a source tree has no
`catch2-config.cmake`); if it does not look like one, configure stops with the
clone command in the message instead of a generic "package not found".

Catch2 is BSL-1.0; a local checkout is a legal and supported way to build.

## Configuration problems and their answers

* **`CMake Generate step failed. Build files cannot be regenerated correctly.`**
  Something in the tree changed shape while a configure was failing — usually a
  source file that does not exist (the message names it) or a half-written preset
  cache. Delete the binary directory and reconfigure; presets keep their own
  directories for exactly this reason.
* **`Cannot find source file: …`** — a file listed in a `CMakeLists.txt` is not on
  disk. Create it or remove the entry; do not "fix" the build by weakening the list.
* **Catch2 fetch fails** — no network or a proxy. Use the offline path above.
* **`Python3_EXECUTABLE` not found** — the RT audit test falls back to `python3`;
  if that is missing too, configure still succeeds and the audit test is skipped.
* **Warnings as errors** — `/W4` (MSVC) and `-Wall -Wextra -Wconversion
  -Wsign-conversion -Wshadow -Wunused-parameter` (GCC/Clang) are strict and CI
  turns them into errors. Fix the warning instead of disabling the flag; see
  "The warning contract" in `DEVELOPMENT.md` for the rules and the one scoped
  exception in the tree.

## Build outputs

```
build/libaura_core.a      core utilities
build/libaura_dsp.a       DSP processors
build/libaura_engine.a    graph, engine, devices, model
build/tests/aura_tests    the test binary
```

On Windows these are `.lib`/`.dll` depending on `BUILD_SHARED_LIBS` (static by
default for the engine, so a crash-free deployment has one fewer moving part).

## Sanitizers and analysis

```bash
cmake -S . -B build-asan -DAURA_BUILD_TESTS=ON -DAURA_ENABLE_SANITIZERS=ON \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j 2 && ctest --test-dir build-asan --output-on-failure
```

`tools/rt_audit.py .` can be run by hand; it is also the ctest
`aura.static.realtime_audit`.
