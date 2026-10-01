# ============================================================================
# AURA DAW - Advanced Unified Recording & Audio
# Copyright (c) 2026 The AURA DAW Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Top level build. Target platform: Windows 10/11 x64 (MSVC 2022).
# The core engine builds and tests on any platform with a C++20 toolchain,
# which is deliberate: it keeps CI cheap, the DSP layer testable everywhere,
# and the Windows audio backends isolated behind aura/audioio.
#
# Layout:
#   aura_core    - errors, logging, RT primitives, JSON, filesystem, time model
#   aura_dsp     - filters, dynamics, modulation, metering, analysis
#   aura_engine  - audio graph, tracks, transport, project, commands, plugins
#   aura_app     - JUCE-based GUI + Windows audio backends (option AURA_BUILD_APP)
# ============================================================================
cmake_minimum_required(VERSION 3.24)

project(AuraDAW
        VERSION 0.1.0
        DESCRIPTION "AURA - Advanced Unified Recording & Audio"
        HOMEPAGE_URL "https://github.com/aura-daw/aura-daw"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep Debug builds honest but usable; audio code needs symbols + no inlining
# surprises for the RT-safety tooling.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AURA_BUILD_TESTS "Build the AURA test suite" ON)
option(AURA_BUILD_BENCHMARKS "Build micro-benchmarks" OFF)
option(AURA_BUILD_APP "Build the desktop application (requires JUCE; Windows)" OFF)
option(AURA_ENABLE_ALLOC_TRACKING "Track heap allocations for RT-safety tests" OFF)
option(AURA_ENABLE_RT_ASSERTS "Enable real-time thread assertions in all builds" OFF)
option(AURA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AURA_ENABLE_SANITIZERS "Enable ASan (and UBSan on GCC/Clang) where supported" OFF)
option(AURA_BUILD_FUZZERS "Build the libFuzzer targets in fuzz/ (requires Clang)" OFF)

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------
add_library(aura_project_options INTERFACE)
add_library(Aura::Options ALIAS aura_project_options)

target_compile_features(aura_project_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(aura_project_options INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr
    /MP /utf-8 /EHsc
    $<$<CONFIG:Release>:/Oi /Ot>
    $<$<CONFIG:RelWithDebInfo>:/Zi /O2>
    $<$<CONFIG:Debug>:/Od>
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${AURA_ENABLE_SANITIZERS}>>>:/RTC1>)
  target_compile_definitions(aura_project_options INTERFACE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  # Windows 10 is the floor (AURA ships for 10/11): pinning it means the SDK does
  # not quietly hand out APIs that only exist on a newer OS, and the MinGW
  # cross-check (tools/win_crosscheck.sh) uses the same value.
  _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  # Static CRT by default: a DAW ships as a self-contained install and must not
  # depend on a specific VC++ redistributable being present.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  if(AURA_ENABLE_SANITIZERS)
    # MSVC's address sanitizer ships as a runtime *DLL* and its allocator hooks only
    # exist in the dynamic CRT, so the static CRT has to give way for this
    # configuration. Everything else about the build stays identical, which is what
    # makes the run worth doing: it exercises the same code the Release build ships.
    # Spell it out for configure-time validation: CMake accepts exactly
    # MultiThreaded[Debug][DLL], so "DLL" trails the optional "Debug".
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(aura_project_options INTERFACE /fsanitize=address /Zi)
    target_link_options(aura_project_options INTERFACE
      /fsanitize=address /INCREMENTAL:NO)
    message(STATUS "AURA: MSVC address sanitizer enabled (dynamic CRT, /INCREMENTAL:NO)")
  endif()
else()
  # The list mirrors the MSVC /W4 set: -Wunused-parameter stays ON because MSVC
  # reports C4100 at /W4 (and /WX turns it into an error), so an unused parameter
  # that GCC forgives would still break the Windows build. Keeping the two flag
  # sets aligned means "green on Linux" really does imply "green on CI Windows".
  target_compile_options(aura_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wunused-parameter -Wno-missing-field-initializers)
  if(AURA_ENABLE_SANITIZERS)
    target_compile_options(aura_project_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(aura_project_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(AURA_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(aura_project_options INTERFACE /WX)
  else()
    target_compile_options(aura_project_options INTERFACE -Werror)
  endif()
endif()

if(AURA_ENABLE_ALLOC_TRACKING)
  target_compile_definitions(aura_project_options INTERFACE AURA_ENABLE_ALLOC_TRACKING=1)
endif()
if(AURA_ENABLE_RT_ASSERTS)
  target_compile_definitions(aura_project_options INTERFACE AURA_ENABLE_RT_ASSERTS=1)
endif()

# ---------------------------------------------------------------------------
# Module: aura_core
# ---------------------------------------------------------------------------
add_library(aura_core STATIC
  src/core/Errors.cpp
  src/core/Log.cpp
  src/core/Strings.cpp
  src/core/Json.cpp
  src/core/FileSystem.cpp
  src/core/Localization.cpp
  src/core/Realtime.cpp
  src/time/Time.cpp
)
add_library(Aura::Core ALIAS aura_core)
target_include_directories(aura_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(aura_core PUBLIC Aura::Options)
if(WIN32)
  # core/FileSystem.cpp resolves the per-user AppData and Music folders through
  # SHGetKnownFolderPath (shell32) and frees the returned wide string with
  # CoTaskMemFree (ole32). A static library propagates these to whatever links it.
  target_link_libraries(aura_core PRIVATE shell32 ole32)
endif()
target_compile_definitions(aura_core PUBLIC AURA_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(aura_core PROPERTIES OUTPUT_NAME aura_core)

# ---------------------------------------------------------------------------
# Module: aura_dsp   (depends on core only - usable headless for rendering)
# ---------------------------------------------------------------------------
add_library(aura_dsp STATIC
  src/dsp/Processor.cpp
  src/dsp/ParameterSmoothing.cpp
  src/dsp/Biquad.cpp
  src/dsp/GainProcessor.cpp
  src/dsp/PanProcessor.cpp
  src/dsp/Equalizer.cpp
  src/dsp/Compressor.cpp
  src/dsp/Limiter.cpp
  src/dsp/Delay.cpp
  src/dsp/Reverb.cpp
  src/dsp/Distortion.cpp
  src/dsp/Oscillator.cpp
  src/dsp/NoiseGenerator.cpp
  src/dsp/Envelope.cpp
  src/dsp/Loudness.cpp)
add_library(Aura::Dsp ALIAS aura_dsp)
target_include_directories(aura_dsp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(aura_dsp PUBLIC Aura::Core)

# ---------------------------------------------------------------------------
# Module: aura_engine   (graph, transport, project, commands, plugin host API)
# The audio engine is deliberately independent of any GUI toolkit so it can be
# driven by the offline renderer, the test harness and, later, a CLI tool.
# ---------------------------------------------------------------------------
# NOTE ON FILE LAYOUT: each module is one translation unit per cohesive concern
# rather than one file per class. When a file grows past ~800 lines it is split
# (the project serializer and the mixer UI are the next candidates).
add_library(aura_engine STATIC
  src/graph/GraphNode.cpp
  src/graph/SampleStream.cpp
  src/audio/AudioFile.cpp
  src/audio/AudioClip.cpp
  src/audio/Track.cpp
  src/audio/Mixer.cpp
  src/audio/RecordingEngine.cpp
  src/audio/WaveformCache.cpp
  src/transport/Transport.cpp
  src/midi/Midi.cpp
  src/midi/Instrument.cpp
  src/project/Project.cpp
  src/commands/Commands.cpp
  src/plugin/PluginHost.cpp
  src/automation/Automation.cpp
  src/timeline/Timeline.cpp
  src/engine/AudioEngine.cpp
  src/engine/OfflineRenderer.cpp
  src/audioio/AudioDeviceManager.cpp
  src/audioio/WasapiDevice.cpp
  src/audioio/AsioDevice.cpp
)
add_library(Aura::Engine ALIAS aura_engine)
target_include_directories(aura_engine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(aura_engine PUBLIC Aura::Core Aura::Dsp)
if(WIN32)
  # ole32 - COM (WASAPI devices, CoInitializeEx/CoCreateInstance/CoTaskMemFree)
  # avrt  - MMCSS "Pro Audio" thread promotion for the render loop
  # winmm - waveOut-style fallback and the timer resolution calls in the clock
  # (uuid is not needed: every IID is resolved at compile time through __uuidof.)
  target_link_libraries(aura_engine PRIVATE ole32 avrt winmm)
endif()

# ---------------------------------------------------------------------------
# Application (JUCE) - Windows only, optional so the engine can be built/tested
# on CI runners and developer machines without a GUI toolchain.
# ---------------------------------------------------------------------------
if(AURA_BUILD_APP)
  if(NOT WIN32)
    message(WARNING "AURA_BUILD_APP=ON but this is not Windows; the desktop app targets Windows 10/11 x64.")
  endif()
  add_subdirectory(app)
endif()

# ---------------------------------------------------------------------------
# Tests / benchmarks
# ---------------------------------------------------------------------------
if(AURA_BUILD_TESTS)
  enable_testing()
  # The real-time-safety audit registered by tests/CMakeLists.txt is a Python
  # script, so the interpreter is located here (top level, before the
  # subdirectory) rather than adding build-system plumbing to the test file.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(NOT Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "python3" CACHE FILEPATH "Python 3 interpreter (fallback)" FORCE)
    message(STATUS "Python 3 not found via CMake: falling back to 'python3' for the RT audit")
  else()
    message(STATUS "Python 3 for the RT audit: ${Python3_EXECUTABLE}")
  endif()
  add_subdirectory(tests)
endif()
if(AURA_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
if(AURA_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Status summary
# ---------------------------------------------------------------------------
message(STATUS "AURA DAW ${PROJECT_VERSION}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  tests         : ${AURA_BUILD_TESTS}")
message(STATUS "  app (JUCE)    : ${AURA_BUILD_APP}")
message(STATUS "  alloc tracking: ${AURA_ENABLE_ALLOC_TRACKING}")
message(STATUS "  Werror        : ${AURA_WARNINGS_AS_ERRORS}")
message(STATUS "  fuzzers       : ${AURA_BUILD_FUZZERS}")
