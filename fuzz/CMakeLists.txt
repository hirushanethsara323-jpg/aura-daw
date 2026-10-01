# ============================================================================
# AURA DAW - fuzz/CMakeLists.txt
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# LibFuzzer targets. Off by default: they need Clang (libFuzzer is part of the
# compiler's runtime), they want a sanitizer to be useful, and they run for as
# long as you let them - none of which belongs in a normal build.
#
#   cmake -S . -B build-fuzz -DAURA_BUILD_FUZZERS=ON \
#         -DCMAKE_CXX_COMPILER=clang++ -DAURA_BUILD_TESTS=OFF
#   cmake --build build-fuzz -j
#   ./build-fuzz/fuzz/aura_fuzz_json fuzz/corpus/json -max_total_time=300
#
# CI runs each target for a bounded time as a smoke test (see .github/workflows/
# ci.yml) and fails on any finding. The portable half of the same coverage - the
# property that untrusted bytes yield an error rather than a crash - lives in
# tests/core/FuzzSmokeTests.cpp so that MSVC/Windows runs get fuzzed too, without
# Clang and without a time budget.
# ============================================================================
if(NOT AURA_BUILD_FUZZERS)
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "AURA_BUILD_FUZZERS=ON but the compiler is ${CMAKE_CXX_COMPILER_ID}, "
                  "not Clang: libFuzzer targets are skipped.")
  return()
endif()

set(AURA_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

function(aura_add_fuzzer name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE Aura::Engine)
  target_compile_options(${name} PRIVATE ${AURA_FUZZ_FLAGS})
  target_link_options(${name} PRIVATE ${AURA_FUZZ_FLAGS})
  # Fuzzing wants speed: an unoptimised build explores far less input per minute.
  target_compile_options(${name} PRIVATE -O1)
endfunction()

# Where the fuzzers may write scratch files. Inside the build tree so a session
# never scribbles outside it; CI overrides it with a temp directory.
target_compile_definitions(aura_project_options INTERFACE
  $<$<BOOL:${AURA_FUZZ_TMPDIR}>:AURA_FUZZ_TMPDIR_DEFAULT="${AURA_FUZZ_TMPDIR}">)

aura_add_fuzzer(aura_fuzz_json fuzz_json.cpp)
aura_add_fuzzer(aura_fuzz_manifest fuzz_manifest.cpp)
aura_add_fuzzer(aura_fuzz_wav fuzz_wav.cpp)

# Seed corpora are committed (fuzz/corpus/<target>) so every run starts from inputs
# that are known to be interesting rather than from random noise. libFuzzer grows
# them; the ones in the repository are the curated minimum.
set(AURA_FUZZ_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/corpus)

foreach(target aura_fuzz_json aura_fuzz_manifest aura_fuzz_wav)
  string(REPLACE "aura_fuzz_" "" corpus_name ${target})
  if(EXISTS ${AURA_FUZZ_CORPUS_DIR}/${corpus_name})
    add_custom_target(${target}_corpus
      COMMAND ${CMAKE_COMMAND} -E echo "corpus: ${AURA_FUZZ_CORPUS_DIR}/${corpus_name}")
  endif()
endforeach()

message(STATUS "Fuzz targets enabled: aura_fuzz_json, aura_fuzz_manifest, aura_fuzz_wav")
