# CI_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0012 · **Code:** `.github/workflows/`

## Question

What does continuous integration have to prove about a DAW, and on which runners,
without burning the free budget?

## What CI must prove (in priority order)

1. **The Windows build works.** MSVC Debug and Release, x64, `/W4 /WX`. This is the
   platform that ships; a green Linux build alone proves nothing about the code
   paths that matter (COM, WASAPI, MME, `MoveFileEx`, `MiniDumpWriteDump`).
2. **Tests pass.** `ctest --output-on-failure` on both Windows configurations,
   including the static RT audit.
3. **The engine builds and tests *without* the optional dependencies.**
   A job that configures with `AURA_FETCH_DEPS=OFF` + a pre-fetched Catch2, or with
   JUCE/VST3/ASIO all disabled, proves an "optional" dependency has not become
   mandatory. This is the job that keeps the licence story true.
4. **Static analysis is clean-ish.** `clang-tidy` with a curated check set on the
   Linux job (MSVC's `/analyze` is slow and noisy; clang-tidy covers the same
   ground faster).
5. **Sanitizers.** ASAN + UBSAN on the Linux job; they catch the buffer and
   lifetime bugs that no assertion will.

## Runners and cost

* `windows-latest` (currently Windows Server 2022) for the Windows matrix;
  `ubuntu-latest` for the fast job. Two vCPUs per runner — the build is designed
  for that (`-j 2`, explicit source lists, no templates metaprogramming storms).
* **Caching:** `actions/cache` on the FetchContent `_deps` directory keyed by
  `CMakeLists.txt` + compiler + configuration, plus `ccache` on Linux. Caching is
  what makes a 40-file C++ build tolerable on a 2-core runner; without it the
  Windows jobs spend most of their budget recompiling Catch2 and the engine.
* Concurrency groups cancel superseded runs on the same branch.
* Timeouts are explicit (30 min) so a hung test cannot eat the budget.

## The matrix (as configured)

| Job | Runner | Config | What it runs |
|-----|--------|--------|--------------|
| `linux` | ubuntu | Debug, `AURA_BUILD_TESTS=ON` | build, `ctest`, RT audit, clang-tidy, ASAN/UBSAN variant |
| `windows-debug` | windows | Debug | build, `ctest` |
| `windows-release` | windows | Release, `AURA_WARNINGS_AS_ERRORS=ON` | build, `ctest`, artefact upload |
| `windows-no-deps` | windows | Release, `AURA_FETCH_DEPS=OFF` with a pre-fetched Catch2 | proves optional dependencies stay optional |

Notes that came from experience, not theory:

* **`fail-fast: false`** on the matrix — a Windows-only failure must not cancel the
  Linux job that would have told us *why*.
* **Logs are uploaded on failure** (CMake, `Testing/Temporary/LastTest.log`, the
  Catch2 output). Half of the value of CI is the diagnostics.
* **The Windows jobs build with the same preset names a developer uses** so a
  failure reproduces locally with one command (`cmake --preset ci-windows`).

## What CI deliberately does *not* do

* **No audio hardware testing** — runners have no sound card. The `MockAudioDevice`
  (a real thread with deterministic timing) is what CI exercises; the WASAPI and
  ASIO paths are compiled and unit-tested for their *logic*, never claimed to be
  hardware-verified.
* **No GUI tests** until the shell exists (M9).
* **No release signing** — signing needs a certificate and a secrets story that
  belongs to the release job (M11), not to every push.
* **No benchmarks as gates** — benchmarks are reported, not enforced, because a
  shared runner's timings are not stable enough to fail a build on. Performance
  gates require a pinned machine.

## Open questions

* Whether to add a `windows-11-arm64` job (MSVC ARM64 works; the audio backends
  would need an ARM path through ASIO — probably not worth it before v1).
* Whether MSVC ASAN (`/fsanitize=address`) is stable enough for a Windows job; it is
  slower and has historically had false positives with COM.
* Whether to move to `cmake --workflow` (CMake 3.25+) to describe the whole
  configure/build/test sequence in `CMakePresets.json` — it would remove a few lines
  of YAML and make local reproduction literal.

## Sources

* GitHub Actions documentation — Windows runners, `actions/cache`, concurrency,
  matrix `fail-fast`.
* lukka, `run-vcpkg` / `CppCMakeVcpkgTemplate` — the caching pattern (binary cache
  keyed by content) that informs our `_deps` caching.
* Cristian Adam, *Speeding up C++ GitHub Actions using ccache* — cache key design
  (`<config>-ccache-<timestamp>` + `restore-keys` prefix) and the
  `CMAKE_*_COMPILER_LAUNCHER` wiring.
* r/cpp thread on vcpkg + Actions — the failure mode where a partial dependency
  build caches nothing, which is why our cache key is the dependency input, not the
  job result.
