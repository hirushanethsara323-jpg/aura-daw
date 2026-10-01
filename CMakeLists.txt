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
option(AURA_ENABLE_VST3 "Host VST 3 plug-ins (fetches the Steinberg VST 3 SDK at a pinned tag; MIT licensed since 3.8)" OFF)
option(AURA_ENABLE_CLAP "Host CLAP plug-ins (reserved: the adapter is not written yet)" OFF)

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
    # Directory-scoped on purpose, not on the interface target: MSVC's linker treats
    # instrumented and uninstrumented objects as incompatible (LNK2038 on
    # annotate_string/annotate_vector/annotate_optional), so the third-party code that
    # ends up in the same binary - Catch2, fetched into tests/ - has to be built by the
    # same rules. These options apply to every target created from here on, including
    # the ones FetchContent adds later; GCC/Clang are happy either way, which is why the
    # Linux sanitizer job can stay on the per-target flags.
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address /INCREMENTAL:NO)
    message(STATUS "AURA: MSVC address sanitizer enabled (whole build, dynamic CRT, /INCREMENTAL:NO)")
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
  src/plugin/ChildProcess.cpp
  src/plugin/sandbox/SharedMemory.cpp
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
# Module: VST 3 hosting (optional, off by default)
#
# Off by default on purpose: fetching a third-party SDK is a decision, not a
# side effect of configuring the project. With it off, the host still builds,
# still scans (listing what it cannot host, with the reason) and still runs its
# full test suite - which is what keeps the default build honest and fast.
#
# The SDK is FETCHED AT A PINNED TAG, never vendored: this repository carries no
# Steinberg tree, the licence text stays where its authors put it, and the build
# is reproducible from the tag. Only the four submodules hosting actually needs
# are cloned (base, cmake, pluginterfaces, public.sdk) - VSTGUI, the tutorials and
# the plug-in examples are not part of a host build.
# ---------------------------------------------------------------------------
if(AURA_ENABLE_VST3)
  include(FetchContent)
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  # The SDK builds its libraries against the *dynamic* CRT (/MD[d]) while AURA ships
  # the static one, and MSVC refuses to link the two in one binary: LNK2038 in every
  # SDK object, then a wall of LNK2005/LNK2019 for the CRT symbols it cannot resolve.
  # Since the SDK's toolset module reads this option to choose /MT[d] or /MD[d], the
  # fix is to make it agree with how AURA builds - static normally, dynamic when the
  # sanitizer forces it (MSVC's ASan runtime exists only as a DLL).
  if(AURA_ENABLE_SANITIZERS)
    set(SMTG_USE_STATIC_CRT OFF CACHE BOOL "" FORCE)
  else()
    set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
  endif()
  # The pin is one variable so the status line, the docs and the fetch cannot drift
  # apart. VST 3.8.1 build 84 is MIT-licensed (VST 3.8 relicensed the SDK) and is
  # the version the adapter in src/plugin/vst3 is written against.
  set(AURA_VST3_SDK_TAG "v3.8.1_build_84")
  FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG ${AURA_VST3_SDK_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk)
  FetchContent_MakeAvailable(vst3sdk)

  target_sources(aura_engine PRIVATE
    src/plugin/vst3/Vst3Scan.cpp
    src/plugin/vst3/Vst3Instance.cpp
    $<TARGET_OBJECTS:aura_vst3_sdk_bridge>)

  # PlugProvider is the SDK's own component/controller wiring: it creates both
  # halves, connects them over ConnectionProxy and hands them back. It is NOT part
  # of the SDK's `sdk_hosting` library in 3.8.1 - the SDK compiles it per hosting
  # example - so a host that wants it names the file. Writing our own would mean
  # re-implementing the connection plumbing the format requires.
  #
  # It is compiled in its own object library so it does not inherit AURA's warning
  # gate: -Wshadow/-Wconversion/-Werror are a statement about our code, and a
  # third-party file that does not meet them is not a reason to weaken the gate.
  # The same is true of three more SDK sources a host needs and the SDK's libraries
  # do not carry: the platform module loader (dlopen/LoadLibrary) and the in-memory
  # IBStream used for plug-in state.
  add_library(aura_vst3_sdk_bridge OBJECT
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp)
  if(APPLE)
    target_sources(aura_vst3_sdk_bridge PRIVATE
      ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
  elseif(WIN32)
    target_sources(aura_vst3_sdk_bridge PRIVATE
      ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
  else()
    target_sources(aura_vst3_sdk_bridge PRIVATE
      ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
  endif()
  target_include_directories(aura_vst3_sdk_bridge SYSTEM PRIVATE ${vst3sdk_SOURCE_DIR})
  target_compile_features(aura_vst3_sdk_bridge PRIVATE cxx_std_20)
  if(MSVC)
    # The bridge is compiled with C++20 like the rest of the tree, but it does not
    # link aura_project_options (that would put third-party SDK code under AURA's
    # -Werror gate - see above), so it has to be told the one flag that matters to
    # the SDK: without /Zc:__cplusplus MSVC reports __cplusplus as 199711L, the
    # SDK's SMTG_CPP20 therefore evaluates to 0 while the compiler is in C++20 mode,
    # and its Windows module loader takes the pre-C++20 branch that passes a
    # std::u8string where a std::string is expected (C2664 on the machine that
    # ships). GCC/Clang get an honest __cplusplus for free.
    target_compile_options(aura_vst3_sdk_bridge PRIVATE /Zc:__cplusplus)
  endif()
  target_link_libraries(aura_vst3_sdk_bridge PRIVATE sdk_hosting sdk_common ${CMAKE_DL_LIBS})
  target_compile_definitions(aura_engine PUBLIC AURA_ENABLE_VST3=1)
  # sdk_hosting - module loading, PlugProvider, ProcessData, parameter changes.
  # sdk_common  - MemoryStream and the string conversions a host needs.
  # PUBLIC, not PRIVATE: a static library's consumers have to resolve the symbols
  # its objects reference, so the scanner helper and the tests linking Aura::Engine
  # need the SDK's libraries too. Hiding them here only moves the link error one
  # target further out.
  target_link_libraries(aura_engine PUBLIC sdk_hosting sdk_common)
  # SYSTEM on purpose: the SDK's headers are third-party code, and AURA builds with
  # -Werror/-W4. Gating our build on Steinberg's warning style would mean either
  # turning the gate off globally or patching a fetched tree - both worse than
  # saying "these are not our warnings".
  target_include_directories(aura_engine SYSTEM PRIVATE ${vst3sdk_SOURCE_DIR})
  message(STATUS "AURA: VST 3 hosting enabled (VST 3 SDK ${AURA_VST3_SDK_TAG})")
endif()

# ---------------------------------------------------------------------------
# Scanner helper process
#
# Built in every configuration, even one without any SDK: it is the supervisor's
# other half, and a build that cannot produce it would silently lose the only
# crash isolation the scanner has.
# ---------------------------------------------------------------------------
add_executable(aura_scan_host tools/scan_host/ScanHost.cpp)
# One predictable location for every generator and every configuration. A
# multi-config generator (Visual Studio) appends the configuration name to
# RUNTIME_OUTPUT_DIRECTORY unless the per-configuration properties are set too,
# which would leave the helper in tools/<Config>/ - while the host looks for it
# next to the executable, in ../tools and in ../../tools. The symptom was a
# Windows-only failure of "the scanner helper is built next to the tests" with
# every Linux job green, i.e. exactly the platform gap the Windows rows exist for.
set_target_properties(aura_scan_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
foreach(_aura_config IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${_aura_config}" _aura_config_upper)
  set_target_properties(aura_scan_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${_aura_config_upper} "${CMAKE_BINARY_DIR}/tools")
endforeach()
target_link_libraries(aura_scan_host PRIVATE Aura::Engine Aura::Core)
if(AURA_ENABLE_VST3)
  target_include_directories(aura_scan_host PRIVATE src/plugin/vst3)
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
message(STATUS "  VST 3 hosting : ${AURA_ENABLE_VST3}")
