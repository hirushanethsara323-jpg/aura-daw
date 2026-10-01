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
)

target_include_directories(aura_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_bench PRIVATE Aura::Engine)

# Optimised measurements: a benchmark built with -O0 measures the build, not the
# code, and that is the one mistake that makes benchmark numbers worthless.
target_compile_options(aura_bench PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/O2>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>)
