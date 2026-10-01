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
set(AURA_CATCH2_DIR "" CACHE PATH
    "Local Catch2 v3 *source* checkout used when AURA_FETCH_DEPS=OFF")

if(AURA_FETCH_DEPS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG ${AURA_CATCH2_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
elseif(AURA_CATCH2_DIR)
  # A relative value is resolved against the repository root (so CI can pass the
  # plain, shell-independent "../catch2" for a sibling checkout). Doing the
  # resolution here - rather than with `cd ... && pwd` in the workflow - avoids the
  # MSYS-vs-CMake path mismatch that Git Bash introduces on Windows runners, where
  # a shell-computed path can arrive as /d/a/... and be read as a path on C:.
  if(NOT IS_ABSOLUTE "${AURA_CATCH2_DIR}")
    set(AURA_CATCH2_DIR "${CMAKE_SOURCE_DIR}/${AURA_CATCH2_DIR}")
  endif()
  get_filename_component(AURA_CATCH2_DIR "${AURA_CATCH2_DIR}" ABSOLUTE)
  if(NOT EXISTS "${AURA_CATCH2_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "AURA_CATCH2_DIR does not look like a Catch2 checkout (no CMakeLists.txt): "
      "${AURA_CATCH2_DIR}\n"
      "Clone it with:\n"
      "  git clone --branch v3.7.1 --depth 1 https://github.com/catchorg/Catch2.git ../catch2")
  endif()
  message(STATUS "Offline build: Catch2 source at ${AURA_CATCH2_DIR}")
  # Offline / air-gapped build: docs/BUILDING.md tells the user to clone a pinned
  # tag and point this variable at it. find_package() cannot consume a source
  # tree (it wants catch2-config.cmake), which is why this branch builds Catch2 as
  # a subproject instead - the mistakes this avoids are a configure failure that
  # looks like a missing package on a machine that has the source right there.
  add_subdirectory(${AURA_CATCH2_DIR} ${CMAKE_BINARY_DIR}/catch2 EXCLUDE_FROM_ALL)
  list(APPEND CMAKE_MODULE_PATH ${AURA_CATCH2_DIR}/extras)
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
  core/FuzzSmokeTests.cpp
  time/TimeTests.cpp
  dsp/DspTests.cpp
  dsp/BlockStateTests.cpp
  graph/GraphTests.cpp
  graph/DelayCompensationTests.cpp
  audio/AudioTests.cpp
  midi/MidiTests.cpp
  project/ProjectTests.cpp
  engine/EngineTests.cpp
  api/DeviceLifetimeTests.cpp
  commands/CommandsTests.cpp
  automation/AutomationTests.cpp
  timeline/TimelineTests.cpp
  plugin/PluginTests.cpp
  plugin/ChildProcessTests.cpp
)
target_link_libraries(aura_tests PRIVATE Aura::Engine Aura::Dsp Aura::Core aura_test_support)

# ---------------------------------------------------------------------------
# VST 3 hosting tests + the real plug-in they load.
#
# Only when AURA_ENABLE_VST3 is on: without the SDK there is no adapter to test,
# and the rest of the suite must still build and pass (which is the whole point of
# the option being off by default). The plug-in is built here rather than taken
# from the SDK's samples so the test subject is ours - if the host is wrong, the
# fixture does not bend to hide it.
# ---------------------------------------------------------------------------
if(AURA_ENABLE_VST3)
  target_sources(aura_tests PRIVATE plugin/Vst3HostingTests.cpp)
  target_compile_definitions(aura_tests PRIVATE
    AURA_TEST_VST3_BUNDLE="${CMAKE_CURRENT_BINARY_DIR}/vst3/AuraTestPlugin.vst3")

  add_library(aura_test_vst3_plugin MODULE
    plugin/vst3/AuraTestPlugin.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
  if(UNIX AND NOT APPLE)
    # The format's Linux entry points (ModuleEntry/ModuleExit) come from the SDK's
    # linuxmain.cpp; without them the loader refuses the bundle before it ever
    # reaches GetPluginFactory.
    target_sources(aura_test_vst3_plugin PRIVATE
      ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/linuxmain.cpp)
  endif()
  target_include_directories(aura_test_vst3_plugin SYSTEM PRIVATE ${vst3sdk_SOURCE_DIR})
  target_link_libraries(aura_test_vst3_plugin PRIVATE sdk sdk_common)

  # Both layouts the format defines: a bundle is <name>.vst3/Contents/<platform>/
  # <name>.<ext>, the loader refuses anything else, and the inner file must be named
  # after the bundle folder. Leaving a bare library where the format wants a bundle
  # produces a "plug-in" that cannot be loaded by the format it claims to be.
  if(WIN32)
    # Windows: the binary itself carries the .vst3 extension, and the platform
    # directory is x86_64-win (x86-win on a 32-bit build).
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(AURA_VST3_BUNDLE_LIBDIR "x86_64-win")
    else()
      set(AURA_VST3_BUNDLE_LIBDIR "x86-win")
    endif()
    set(AURA_VST3_BUNDLE_LIBNAME "AuraTestPlugin.vst3")
    set_target_properties(aura_test_vst3_plugin PROPERTIES
      PREFIX "" SUFFIX ".vst3" OUTPUT_NAME "AuraTestPlugin")
  elseif(UNIX AND NOT APPLE)
    set(AURA_VST3_BUNDLE_LIBDIR "${CMAKE_SYSTEM_PROCESSOR}-linux")
    set(AURA_VST3_BUNDLE_LIBNAME "AuraTestPlugin.so")
    set_target_properties(aura_test_vst3_plugin PROPERTIES
      PREFIX "" SUFFIX ".so" OUTPUT_NAME "AuraTestPlugin")
  else()
    message(WARNING
      "AURA_ENABLE_VST3=ON but the test bundle layout is only implemented for "
      "Linux and Windows; the VST 3 tests will not be able to load the fixture on "
      "this platform.")
  endif()

  if(DEFINED AURA_VST3_BUNDLE_LIBDIR)
    set(AURA_VST3_BUNDLE_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/vst3/AuraTestPlugin.vst3/Contents/${AURA_VST3_BUNDLE_LIBDIR}/${AURA_VST3_BUNDLE_LIBNAME}")
    add_custom_command(TARGET aura_test_vst3_plugin POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/vst3/AuraTestPlugin.vst3/Contents/${AURA_VST3_BUNDLE_LIBDIR}"
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:aura_test_vst3_plugin>
              "${AURA_VST3_BUNDLE_FILE}"
      COMMENT "Assembling the AuraTestPlugin VST 3 bundle")
    # The tests load a real bundle from disk; building the tests must not race it.
    add_dependencies(aura_tests aura_test_vst3_plugin)
    add_dependencies(aura_scan_host aura_test_vst3_plugin)
  endif()
endif()

# The portable fuzz smoke test reuses the seeds and the temporary-file helper from
# fuzz/FuzzSupport.hpp so the deterministic test and the libFuzzer targets start
# from the same inputs (a bug found by one reproduces in the other).
target_include_directories(aura_tests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/plugin)
if(AURA_ENABLE_VST3)
  # ChildProcessTests drives the real scanner helper, which is a separate binary.
  target_include_directories(aura_scan_host PRIVATE ${CMAKE_SOURCE_DIR}/src/plugin)
  add_dependencies(aura_tests aura_scan_host)
endif()

include(Catch)
catch_discover_tests(aura_tests
  TEST_PREFIX "aura."
  PROPERTIES LABELS "unit")

# The static audit tools run as tests so CI cannot forget them.
if(AURA_BUILD_TESTS)
  add_test(NAME aura.static.realtime_audit
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rt_audit.py
                   ${CMAKE_SOURCE_DIR})
  add_test(NAME aura.static.include_audit
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/include_audit.py
                   ${CMAKE_SOURCE_DIR})
endif()
