// ============================================================================
// AURA DAW - src/plugin/ChildProcess.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// POSIX and Windows implementations of the process supervisor declared in
// ChildProcess.hpp. Both do the same three things in the same order: redirect the
// child's output to a temporary file, wait with a deadline, and kill if the
// deadline passes.
//
// Notes from writing it:
//   * The output goes to a file rather than a pipe on purpose. A pipe needs the
//     parent to read while the child writes; a child that writes more than the
//     pipe buffer holds would block forever, and the whole point of this code is
//     to survive a child that blocks forever.
//   * On POSIX the child is put in its own process group so a plug-in that spawns
//     helpers of its own can be killed with the group, not one process at a time.
//   * On Windows there is no such thing as a process group for TerminateProcess,
//     which is why the SDK-side plan (docs/PLUGIN_HOST.md) uses a job object when
//     the sandboxed host lands; for scanning, a single process is enough.
// ============================================================================

#include "ChildProcess.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <memory>
#else
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>
#endif

namespace aura::plugin {
namespace {

constexpr const char* kCategory = "plugin";
constexpr double kPollIntervalSeconds = 0.01;

/// A unique temporary file per invocation: two scans running at once must not
/// write into each other's output.
std::string temporaryOutputPath() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error);
    const std::string name = "aura-child-" + std::to_string(now) + "-" +
                             std::to_string(serial) + ".log";
    return (error ? std::filesystem::path(".") : directory).string() + "/" + name;
}

std::string readAndRemove(const std::string& path) {
    std::string contents;
    {
        std::ifstream stream(path, std::ios::binary);
        if (stream) {
            contents.assign(std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>());
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return contents;
}

#if !defined(_WIN32)
/// argv for posix_spawn: the executable as argv[0] plus the caller's arguments.
std::vector<char*> buildArgv(const ChildProcessRequest& request,
                             std::vector<std::string>& storage) {
    storage.clear();
    storage.push_back(request.executable);
    for (const auto& argument : request.arguments)
        storage.push_back(argument);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& entry : storage)
        argv.push_back(entry.data());
    argv.push_back(nullptr);
    return argv;
}
#endif

} // namespace

namespace {
/// Where a helper binary is looked for, most specific first. A build tree puts the
/// scanner in `tools/` while the application and the tests live elsewhere in the
/// same tree, so "next to the executable" alone would fail exactly where the
/// isolation matters most; an installed layout keeps them together and matches on
/// the first try.
std::string firstExisting(const std::vector<std::string>& candidates) {
    std::error_code error;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return candidates.empty() ? std::string{} : candidates.front();
}
} // namespace

std::string helperExecutablePath(const std::string& name) {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            break;
        buffer.resize(buffer.size() * 2);
    }
    const std::filesystem::path directory = std::filesystem::path(buffer.data()).parent_path();
    const std::string executable = name + ".exe";
#elif defined(__APPLE__)
    std::filesystem::path directory;
    {
        char raw[4096];
        std::uint32_t size = sizeof(raw);
        extern int _NSGetExecutablePath(char*, std::uint32_t*);
        if (_NSGetExecutablePath(raw, &size) == 0)
            directory = std::filesystem::path(raw).parent_path();
    }
    if (directory.empty())
        return {};
    const std::string executable = name;
#else
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error)
        return {};
    const std::filesystem::path directory = self.parent_path();
    const std::string executable = name;
#endif

    // The last two cover a build tree: the tests live in tests/ while the helper
    // is built into tools/. CMake pins that to one directory for every
    // configuration (see CMakeLists.txt), but a tree configured before that - or by
    // an IDE that put the configuration name back in - is still worth finding, by
    // reusing the executable's own configuration directory name.
    const std::string configName = directory.filename().string();
    const std::vector<std::string> candidates = {
        (directory / executable).string(),
        (directory / ".." / "tools" / executable).string(),
        (directory / ".." / ".." / "tools" / executable).string(),
        (directory / ".." / "tools" / configName / executable).string(),
        (directory / ".." / ".." / "tools" / configName / executable).string(),
    };
    return firstExisting(candidates);
}

ChildProcessResult runChildProcess(const ChildProcessRequest& request) {
    ChildProcessResult result;
    const std::string outputPath = temporaryOutputPath();
    const auto started = std::chrono::steady_clock::now();

    if (request.executable.empty()) {
        result.error = "no executable given";
        return result;
    }

#if defined(_WIN32)
    // ------------------------------------------------------------------ Windows
    std::string commandLine = "\"" + request.executable + "\"";
    for (const auto& argument : request.arguments)
        commandLine += " \"" + argument + "\"";

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    std::wstring wideOutput(outputPath.begin(), outputPath.end());
    const HANDLE output = CreateFileW(wideOutput.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        result.error = "could not create the child's output file";
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring wideCommand(commandLine.begin(), commandLine.end());
    const BOOL ok = CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(output);
    if (!ok) {
        result.error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return result;
    }
    result.started = true;

    const DWORD timeoutMs = static_cast<DWORD>(std::max(1.0, request.timeoutSeconds) * 1000.0);
    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(process.hProcess, 0xFFFFFFFFu);
        WaitForSingleObject(process.hProcess, 2000);
    } else if (wait == WAIT_FAILED) {
        result.error = "WaitForSingleObject failed";
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.hProcess, &exitCode))
        result.exitCode = static_cast<int>(exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    // -------------------------------------------------------------------- POSIX
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, outputPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, outputPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
    // A plug-in that spawns its own helpers should die with them.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    std::vector<std::string> argvStorage;
    std::vector<char*> argv = buildArgv(request, argvStorage);

    pid_t pid = -1;
    const int spawnResult = posix_spawn(&pid, request.executable.c_str(), &actions, &attributes,
                                        argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawnResult != 0) {
        result.error = "posix_spawn failed: " + std::string(std::strerror(spawnResult));
        readAndRemove(outputPath);
        return result;
    }
    result.started = true;

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(std::max(0.01, request.timeoutSeconds)));
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid)
            break;
        if (waited < 0) {
            result.error = "waitpid failed";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            // The whole group: a plug-in that forked during scanning dies with it.
            if (kill(-pid, SIGKILL) != 0)
                kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(kPollIntervalSeconds));
    }

    if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status); // 128+N, the shell convention
#endif

    const auto finished = std::chrono::steady_clock::now();
    result.elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();
    result.capturedOutput = readAndRemove(outputPath);

    if (result.timedOut && result.error.empty()) {
        result.error = "the process did not finish within " +
                       strings::fromDouble(request.timeoutSeconds, 1) + "s and was killed";
        AURA_LOG_WARN(kCategory, "%s timed out after %.1fs", request.executable.c_str(),
                      request.timeoutSeconds);
    } else if (result.crashed() && result.error.empty()) {
        result.error = "the process terminated abnormally (exit code " +
                       std::to_string(result.exitCode) + ")";
    }
    return result;
}

} // namespace aura::plugin
