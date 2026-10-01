# ============================================================================
# AURA DAW - test suite
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Catch2 v3 is fetched at a pinned tag (BSL-1.0 licence: header + static libs).
# AURA_FETCH_DEPS=OFF uses an installed Catch2 instead (offline/distro builds).
#
# Test layers (see docs/TESTING.md):
#   unit        - one module, deterministic, no I/O
#   integration - several modules together (engine + project + files)
#   regression  - a specific bug that must never come back
#   audiomath   - measured signal properties against analytic expectations
#   realtime    - allocation/lock discipline on the audio path
# All of them run in one binary per layer group so CI can filter with tags.
# ============================================================================
include(FetchContent)

option(AURA_FETCH_DEPS "Download pinned third-party test dependencies" ON)
set(AURA_CATCH2_TAG v3.7.1)

if(AURA_FETCH_DEPS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG ${AURA_CATCH2_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
else()
  find_package(Catch2 3 REQUIRED)
  list(APPEND CMAKE_MODULE_PATH ${Catch2_ROOT}/extras)
endif()

# ---------------------------------------------------------------------------
# Shared test support (signal measurement helpers + project fixtures)
# ---------------------------------------------------------------------------
add_library(aura_test_support STATIC
  support/TestSignals.cpp
)
target_include_directories(aura_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_test_support PUBLIC Aura::Core Aura::Dsp Catch2::Catch2WithMain)

# ---------------------------------------------------------------------------
# Test executables, grouped by subsystem so a failure points at its owner.
# ---------------------------------------------------------------------------
add_executable(aura_tests
  core/CoreTests.cpp
  time/TimeTests.cpp
  dsp/DspTests.cpp
  graph/GraphTests.cpp
  audio/AudioTests.cpp
  midi/MidiTests.cpp
  project/ProjectTests.cpp
  engine/EngineTests.cpp
  commands/CommandsTests.cpp
  automation/AutomationTests.cpp
  timeline/TimelineTests.cpp
  plugin/PluginTests.cpp
)
target_link_libraries(aura_tests PRIVATE Aura::Engine Aura::Dsp Aura::Core aura_test_support)

include(Catch)
catch_discover_tests(aura_tests
  TEST_PREFIX "aura."
  PROPERTIES LABELS "unit")

# The realtime-safety audit tool is a build target so CI can run it as a test.
if(AURA_BUILD_TESTS)
  add_test(NAME aura.static.realtime_audit
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rt_audit.py
                   ${CMAKE_SOURCE_DIR})
endif()
