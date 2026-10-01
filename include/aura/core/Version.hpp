// ============================================================================
// AURA DAW - core/Version.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Single source of truth for the version strings that end up in the UI title
// bar, the project file, the crash log and the About box.
//
// The CMake build overrides these through target_compile_definitions (see
// CMakeLists.txt: AURA_VERSION_STRING="<PROJECT_VERSION>"), so a packaged build
// reports the exact version it was built from. The fallbacks below exist so the
// headers stay usable in scratch builds, tests and tooling that do not go
// through CMake.
// ============================================================================
#pragma once

#ifndef AURA_VERSION_STRING
#    define AURA_VERSION_STRING "0.1.0-dev"
#endif

#ifndef AURA_VERSION_MAJOR
#    define AURA_VERSION_MAJOR 1
#endif

#ifndef AURA_VERSION_MINOR
#    define AURA_VERSION_MINOR 0
#endif

#ifndef AURA_VERSION_PATCH
#    define AURA_VERSION_PATCH 0
#endif

/// Human-readable product name (never localized: it is a brand, not UI text).
#define AURA_PRODUCT_NAME "AURA DAW"

/// Project file format version produced by this build. Loading a file with a
/// NEWER major version is refused (ErrorCode::ProjectVersionTooNew) rather than
/// being parsed optimistically and silently losing data.
#define AURA_PROJECT_FORMAT_MAJOR 1
#define AURA_PROJECT_FORMAT_MINOR 0

/// Version as separate integers for comparison logic.
namespace aura::core {

struct Version {
    int major = AURA_VERSION_MAJOR;
    int minor = AURA_VERSION_MINOR;
    int patch = AURA_VERSION_PATCH;
};

[[nodiscard]] inline const char* versionString() noexcept { return AURA_VERSION_STRING; }

/// True when `other` is newer than this build (used for the project format and
/// for the "this project was made with a newer AURA" warning).
[[nodiscard]] inline bool isNewerThanThisBuild(int major, int /*minor*/) noexcept {
    return major > AURA_PROJECT_FORMAT_MAJOR;
}

} // namespace aura::core
