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
| `AURA_ENABLE_VST3` | OFF | compile the VST 3 adapter; the SDK is fetched at a pinned tag (see below) |
| `AURA_WITH_JUCE` | OFF | JUCE-based plug-in editor adapter (**AGPLv3** — read ADR-0006 first) |
| `AURA_WARNINGS_AS_ERRORS` | OFF | `/W4 /WX` on MSVC, `-Wall -Wextra -Werror` elsewhere |
| `AURA_ENABLE_SANITIZERS` | OFF | ASan + UBSan on GCC/Clang; ASan on MSVC (which switches to the dynamic CRT and drops `/RTC1`, both of which the sanitizer requires) |
| `AURA_ENABLE_ALLOC_TRACKING` | ON in Debug/tests | global allocation counter used by the RT regression test |
| `AURA_ENABLE_RT_ASSERTS` | ON in Debug/tests | fails loudly when an RT rule is broken |

Changing a source list: the libraries use **explicit file lists** in
`CMakeLists.txt` (no globbing). Adding `src/foo/Bar.cpp` means adding it to the
right list; a missing entry shows up as an undefined symbol at link time.

## Building with VST 3 hosting

```bash
cmake -S . -B build-vst3 -DAURA_ENABLE_VST3=ON -DAURA_BUILD_TESTS=ON \
      -DAURA_WARNINGS_AS_ERRORS=ON
cmake --build build-vst3 -j
cd build-vst3 && ctest --output-on-failure        # 143 tests, six of them VST 3
```

* The SDK is **fetched, not vendored**: `FetchContent` clones
  `steinbergmedia/vst3sdk` at the pinned tag (`AURA_VST3_SDK_TAG` in
  `CMakeLists.txt`) with `GIT_SHALLOW` and four submodules (`base`, `cmake`,
  `pluginterfaces`, `public.sdk`) — about 38 MB instead of the full 247 MB.
* Air-gapped or patched-SDK builds point CMake at an existing checkout instead:
  `-DFETCHCONTENT_SOURCE_DIR_VST3SDK=/path/to/vst3sdk`. Nothing else changes.
* VSTGUI and the SDK's examples are forced off; AURA does not use them, and the SDK's
  sample plug-ins cannot be built without VSTGUI.
* Only `public.sdk/source/vst/vstsinglecomponenteffect.cpp` and the platform module
  loader are compiled from the SDK into the host bridge (see
  [`PLUGIN_HOST.md`](PLUGIN_HOST.md)); the SDK's headers are included as `SYSTEM`
  because AURA builds with `-Werror` and the gate is a statement about AURA's code.
* The test suite loads a **real** bundle that this build produces
  (`AuraTestPlugin.vst3`, built from `tests/plugin/vst3/`), so an enabled build
  verifies the adapter against the format rather than against a mock. On platforms
  where the fixture's bundle layout is not implemented yet, configure warns.

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

## Checking the Windows code from Linux

The Windows-only code (`WASAPI`, COM, the Win32 file system paths) is not compiled
by a GCC/Linux build, so a mistake there costs a Windows CI round to find. A
MinGW-w64 cross compiler can type-check all of it locally in under a minute:

```bash
sudo apt-get install -y g++-mingw-w64-x86-64     # development tool only
bash tools/win_crosscheck.sh --headers
```

It compiles every `src/**/*.cpp` and every public header standalone with the same
warning set as the native build, and exits non-zero on the first failure. MinGW is
**not** a supported AURA target - MSVC is - and the check does not link or run
anything; it exists because three Windows CI rounds were lost to problems it finds
instantly (see ADR-0014). `--require-toolchain` makes a missing cross compiler an
error, which is how the CI job uses it.

## Benchmarks

Benchmarks are off by default and are never part of `ctest` (a shared CI machine
would decide the numbers):

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DAURA_BUILD_BENCHMARKS=ON
cmake --build build-bench -j 2
./build-bench/bench/aura_bench --list                 # what is measured
./build-bench/bench/aura_bench --iterations 400 --json bench.json
python3 tools/bench_to_markdown.py bench.json --budgets
```

Build them **Release**: an `-O0` benchmark measures the build, not the code (the
target forces `-O2` itself, for exactly that reason). Results and the published
table live in `PERFORMANCE.md`.

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
