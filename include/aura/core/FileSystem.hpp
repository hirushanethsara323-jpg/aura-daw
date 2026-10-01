// ============================================================================
// AURA DAW - core/FileSystem.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-platform file/path utilities (std::filesystem based). Everything here
// is for control/worker threads only - the audio thread never touches it.
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::filesystem {

namespace fs = std::filesystem;

[[nodiscard]] bool exists(const fs::path& path) noexcept;
[[nodiscard]] bool isDirectory(const fs::path& path) noexcept;
[[nodiscard]] bool isRegularFile(const fs::path& path) noexcept;

[[nodiscard]] std::uint64_t fileSize(const fs::path& path) noexcept;

/// Cross-platform "modified" time in ms since unix epoch (0 when unavailable).
[[nodiscard]] std::int64_t fileModifiedTimeMs(const fs::path& path) noexcept;

/// Free space in bytes on the volume containing `path` (0 when unknown).
[[nodiscard]] std::uint64_t freeSpaceBytes(const fs::path& path) noexcept;

[[nodiscard]] Status createDirectories(const fs::path& path);
[[nodiscard]] Status removeFile(const fs::path& path);
[[nodiscard]] Status copyFile(const fs::path& from, const fs::path& to, bool overwrite = true);
[[nodiscard]] Status renameFile(const fs::path& from, const fs::path& to);

/// Makes the path unique by appending " (2)", " (3)", ... before the extension.
[[nodiscard]] fs::path makeUniquePath(const fs::path& desired);

/// Sanitizes a string for use as a file name (no path separators, no reserved
/// Windows device names, bounded length).
[[nodiscard]] std::string sanitizeFileName(const std::string& name, std::size_t maxLength = 120);

/// Ensures an extension (with leading dot), case-insensitive.
[[nodiscard]] bool hasExtension(const fs::path& path, const char* extension) noexcept;
[[nodiscard]] fs::path ensureExtension(const fs::path& path, const char* extension);
[[nodiscard]] std::string extensionLower(const fs::path& path);

/// Text IO (UTF-8). Atomic write uses tmp file + rename so a crash mid-write
/// can never truncate an existing project/settings file.
[[nodiscard]] AuraResult<std::string> readTextFile(const fs::path& path);
[[nodiscard]] Status writeTextFileAtomic(const fs::path& path, const std::string& text);

/// Binary IO.
[[nodiscard]] AuraResult<std::vector<std::uint8_t>> readBinaryFile(const fs::path& path);
[[nodiscard]] Status writeBinaryFileAtomic(const fs::path& path,
                                           const std::vector<std::uint8_t>& data);

/// Per-user application data directories (Windows: %APPDATA%\AURA; Linux: XDG).
[[nodiscard]] fs::path appDataDirectory(
    const std::string& appName =
#if defined(_WIN32)
        "AURA"
#else
        "aura"
#endif
);

/// <appData>/<subdir>, created on demand.
[[nodiscard]] fs::path appDataSubdirectory(const std::string& subdir);

/// User's home/Music folder fallbacks for default project & recording locations.
[[nodiscard]] fs::path defaultProjectDirectory();
[[nodiscard]] fs::path defaultRecordingDirectory();
[[nodiscard]] fs::path defaultSettingsDirectory();
[[nodiscard]] fs::path defaultCacheDirectory();

/// Lists files in a directory (non-recursive) optionally filtered by extension.
[[nodiscard]] std::vector<fs::path> listFiles(const fs::path& directory,
                                              const std::vector<std::string>& extensions = {},
                                              bool recursive = false);

/// Relative path from `base` to `target` when possible, else absolute target.
[[nodiscard]] fs::path relativeTo(const fs::path& base, const fs::path& target);

/// Portable string form (UTF-8, forward slashes) for storing in project files.
[[nodiscard]] std::string toPortablePath(const fs::path& path);
[[nodiscard]] fs::path fromPortablePath(const std::string& text);

/// Executable directory (for locating bundled assets).
[[nodiscard]] fs::path executableDirectory();

} // namespace aura::filesystem
