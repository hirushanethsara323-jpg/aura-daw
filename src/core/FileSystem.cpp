// ============================================================================
// AURA DAW - src/core/FileSystem.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/FileSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include "aura/core/Strings.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace aura::filesystem {
namespace {

AuraError ioError(const std::string& what, const fs::path& path) {
    return makeError(ErrorCode::IoError, what + ": " + path.string(), path.string());
}

} // namespace

bool exists(const fs::path& path) noexcept {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool isDirectory(const fs::path& path) noexcept {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

bool isRegularFile(const fs::path& path) noexcept {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

std::uint64_t fileSize(const fs::path& path) noexcept {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

std::int64_t fileModifiedTimeMs(const fs::path& path) noexcept {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
        return 0;
    const auto since = time.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

std::uint64_t freeSpaceBytes(const fs::path& path) noexcept {
#if defined(_WIN32)
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(path.wstring().c_str(), &freeBytes, nullptr, nullptr))
        return freeBytes.QuadPart;
    return 0;
#else
    struct statvfs info{};
    if (::statvfs(path.string().c_str(), &info) != 0)
        return 0;
    return static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_frsize);
#endif
}

Status createDirectories(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return success();
    fs::create_directories(path, ec);
    if (ec && !fs::is_directory(path))
        return failure(ioError("Could not create directory", path));
    return success();
}

Status removeFile(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
        return failure(ioError("Could not remove file", path));
    return success();
}

Status copyFile(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code ec;
    fs::copy_file(from, to, overwrite ? fs::copy_options::overwrite_existing
                                      : fs::copy_options::none,
                  ec);
    if (ec)
        return failure(ioError("Could not copy file", from));
    return success();
}

Status renameFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        // Cross-device rename falls back to copy+delete.
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return failure(ioError("Could not move file", from));
        fs::remove(from, ec);
    }
    return success();
}

fs::path makeUniquePath(const fs::path& desired) {
    if (!aura::filesystem::exists(desired))
        return desired;
    const fs::path parent = desired.parent_path();
    const std::string stem = desired.stem().string();
    const std::string ext = desired.extension().string();
    for (int counter = 2; counter < 10000; ++counter) {
        const fs::path candidate = parent / (stem + " (" + std::to_string(counter) + ")" + ext);
        if (!aura::filesystem::exists(candidate))
            return candidate;
    }
    return desired;
}

std::string sanitizeFileName(const std::string& name, std::size_t maxLength) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool bad = static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' ||
                         c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
        out += bad ? '_' : c;
    }
    out = strings::trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    if (out.empty())
        out = "Untitled";
    if (out.size() > maxLength)
        out.resize(maxLength);
    // Windows reserved device names.
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                     "LPT1", "LPT2", "LPT3", "LPT4"};
    const std::string upper = strings::toUpper(out);
    for (const char* r : reserved) {
        if (upper == r) {
            out = "_" + out;
            break;
        }
    }
    return out;
}

bool hasExtension(const fs::path& path, const char* extension) noexcept {
    return strings::equalsIgnoreCase(path.extension().string(), extension);
}

fs::path ensureExtension(const fs::path& path, const char* extension) {
    if (hasExtension(path, extension))
        return path;
    return fs::path(path.string() + extension);
}

std::string extensionLower(const fs::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty() && ext[0] == '.')
        ext.erase(0, 1);
    return strings::toLower(ext);
}

AuraResult<std::string> readTextFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return failure(ioError("Could not open file for reading", path));
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad())
        return failure(ioError("Error while reading file", path));
    return success(std::move(text));
}

namespace {

Status writeFileWithRename(const fs::path& path, const void* data, std::size_t size,
                           const char* mode) {
    (void)mode;
    const fs::path temp = fs::path(path.string() + ".tmp");
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream)
            return failure(ioError("Could not open file for writing", temp));
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        stream.flush();
        if (!stream)
            return failure(ioError("Could not write file", temp));
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
        if (ec)
            return failure(ioError("Could not finalize file", path));
    }
    return success();
}

} // namespace

Status writeTextFileAtomic(const fs::path& path, const std::string& text) {
    if (!path.parent_path().empty()) {
        const Status dirs = createDirectories(path.parent_path());
        if (!dirs)
            return dirs;
    }
    return writeFileWithRename(path, text.data(), text.size(), "wb");
}

AuraResult<std::vector<std::uint8_t>> readBinaryFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return failure(ioError("Could not open file for reading", path));
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(std::max<std::streamoff>(size, 0)));
    if (!data.empty())
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (stream.bad())
        return failure(ioError("Error while reading file", path));
    return success(std::move(data));
}

Status writeBinaryFileAtomic(const fs::path& path, const std::vector<std::uint8_t>& data) {
    if (!path.parent_path().empty()) {
        const Status dirs = createDirectories(path.parent_path());
        if (!dirs)
            return dirs;
    }
    return writeFileWithRename(path, data.data(), data.size(), "wb");
}

fs::path appDataDirectory(const std::string& appName) {
#if defined(_WIN32)
    PWSTR wide = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wide))) {
        fs::path base(wide);
        CoTaskMemFree(wide);
        return base / appName;
    }
    if (const char* appData = std::getenv("APPDATA"))
        return fs::path(appData) / appName;
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return fs::path(xdg) / appName;
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".local" / "share" / appName;
#endif
    return fs::temp_directory_path() / appName;
}

fs::path appDataSubdirectory(const std::string& subdir) {
    const fs::path path = appDataDirectory() / subdir;
    (void)createDirectories(path);
    return path;
}

fs::path defaultProjectDirectory() {
#if defined(_WIN32)
    PWSTR wide = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &wide))) {
        fs::path base(wide);
        CoTaskMemFree(wide);
        return base / "AURA Projects";
    }
#endif
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "AURA Projects";
    return appDataDirectory() / "Projects";
}

fs::path defaultRecordingDirectory() {
    return appDataSubdirectory("Recordings");
}

fs::path defaultSettingsDirectory() {
    return appDataSubdirectory("Settings");
}

fs::path defaultCacheDirectory() {
    return appDataSubdirectory("Cache");
}

std::vector<fs::path> listFiles(const fs::path& directory, const std::vector<std::string>& extensions,
                                bool recursive) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return files;

    const auto matches = [&extensions](const fs::path& path) {
        if (extensions.empty())
            return true;
        const std::string ext = extensionLower(path);
        for (const auto& wanted : extensions) {
            if (ext == strings::toLower(wanted))
                return true;
        }
        return false;
    };

    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(
                 directory, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            if (it->is_regular_file(ec) && matches(it->path()))
                files.push_back(it->path());
        }
    } else {
        for (auto it = fs::directory_iterator(directory, fs::directory_options::skip_permission_denied,
                                              ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            if (it->is_regular_file(ec) && matches(it->path()))
                files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path relativeTo(const fs::path& base, const fs::path& target) {
    std::error_code ec;
    const fs::path rel = fs::relative(target, base, ec);
    if (ec || rel.empty())
        return target;
    // Refuse to return paths that climb out of the base directory; a project
    // that references ../.. would break portability expectations.
    const std::string text = rel.generic_string();
    if (strings::startsWith(text, ".."))
        return target;
    return rel;
}

std::string toPortablePath(const fs::path& path) {
    return path.generic_string();
}

fs::path fromPortablePath(const std::string& text) {
    return fs::path(text);
}

fs::path executableDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0)
        return fs::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return self.parent_path();
#endif
    return fs::current_path();
}

} // namespace aura::filesystem
