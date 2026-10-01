# ============================================================================
# AURA DAW - bench/CMakeLists.txt
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# Benchmarks are built only with AURA_BUILD_BENCHMARKS=ON and are never part of
# ctest: a shared CI machine would decide the numbers and a regression gate built
# on them would be a flakiness generator. `--json` output is what goes into
# docs/PERFORMANCE.md (issue #7).
# ============================================================================
add_executable(aura_bench
  main.cpp
  Benchmarks.cpp
  DspBenchmarks.cpp
  EngineBenchmarks.cpp
  GraphBenchmarks.cpp
)

target_include_directories(aura_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Aura::Options carries the shared warning/sanitizer settings; linking it on the
# executable itself (and not only through the libraries) keeps -fsanitize=address
# on the final link line, which is what stops the "ASan runtime does not come
# first in initial library list" failure when a sanitizer build is run.
target_link_libraries(aura_bench PRIVATE Aura::Engine Aura::Options)

# Optimised measurements: a benchmark built with -O0 measures the build, not the
# code, and that is the one mistake that makes benchmark numbers worthless.
target_compile_options(aura_bench PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/O2>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>)

# ---------------------------------------------------------------------------
# The memory-growth soak (issue #4). A separate binary from the benchmarks: a
# benchmark answers "how fast", a soak answers "does it stay the same", and they
# have different run lengths, different failure modes and different audiences.
# ---------------------------------------------------------------------------
add_executable(aura_soak SoakMain.cpp)
target_include_directories(aura_soak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_soak PRIVATE Aura::Engine Aura::Options)

target_compile_options(aura_soak PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/O2>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>)

if(WIN32)
  # GetProcessMemoryInfo (psapi) is how Windows reports resident memory.
  target_link_libraries(aura_soak PRIVATE psapi)
endif()
