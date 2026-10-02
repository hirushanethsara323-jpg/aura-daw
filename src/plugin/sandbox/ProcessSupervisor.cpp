// ============================================================================
// AURA DAW - src/plugin/sandbox/ProcessSupervisor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// POSIX and Windows implementations of the supervisor declared in
// ProcessSupervisor.hpp.
//
// Nothing is logged here. The supervisor knows that a process ended and how; it
// does not know which plug-in that process was holding, and a diagnostic that
// cannot name the plug-in is one the user has to go and correlate by hand. The
// session layer has both facts and does the logging.
// ============================================================================

#include "aura/plugin/sandbox/ProcessSupervisor.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aura::plugin::sandbox {
namespace {

/// How long to wait after the insistent signal before giving up on reaping. A
/// SIGKILL or a TerminateProcess is not refuseable, so this is a bound on a
/// scheduling delay rather than on a decision the child gets to make.
constexpr double kReapWaitSeconds = 2.0;

/// Interval between polls while waiting. Long enough that a helper being watched
/// for minutes costs nothing measurable, short enough that stop() answers within a
/// frame or two of being asked.
constexpr int kPollIntervalMs = 2;

#if defined(_WIN32)

/// UTF-8 to UTF-16, properly.
///
/// The scanner's helper widens a command line byte by byte, which is correct for
/// ASCII and wrong for anything else - and an executable path on Windows routinely
/// contains a user's name, which is not reliably ASCII. Doing it correctly here
/// costs one call and removes a whole class of "works until the user's name has an
/// accent in it" failure.
std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                          needed);
    return wide;
}

/// Quotes one argument for a Windows command line: wrap in quotes and double the
/// backslashes that precede a quote. The helper's arguments are AURA-generated and
/// contain no quotes, but an unquoted path with a space in it is the single most
/// common way a spawn silently runs the wrong program.
std::wstring quoteForCommandLine(const std::string& argument) {
    std::wstring wide = widen(argument);
    std::wstring quoted;
    quoted.reserve(wide.size() + 2);
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : wide) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            for (std::size_t i = 0; i < backslashes * 2 + 1; ++i)
                quoted.push_back(L'\\');
            backslashes = 0;
        } else {
            backslashes = 0;
        }
        quoted.push_back(character);
    }
    for (std::size_t i = 0; i < backslashes * 2; ++i)
        quoted.push_back(L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string describeLast(DWORD code) { return "Win32 error " + std::to_string(code); }

#else

std::vector<char*> buildArgv(const HelperLaunch& launch,
                             std::vector<std::string>& storage) {
    storage.clear();
    storage.reserve(launch.arguments.size() + 1);
    storage.push_back(launch.executable);
    for (const auto& argument : launch.arguments)
        storage.push_back(argument);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& item : storage)
        argv.push_back(item.data());
    argv.push_back(nullptr);
    return argv;
}

std::string describeErrno(int value) {
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : ("errno " + std::to_string(value));
}

#endif

/// Reads a file and removes it. Shared by both platforms; the redirection target is
/// always a plain file, so there is nothing platform-specific left to do.
std::string readAndRemove(const std::string& path) {
    if (path.empty())
        return {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return {};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    stream.close();
    std::remove(path.c_str());
    return buffer.str();
}

} // namespace

const char* toString(HelperLiveness liveness) noexcept {
    switch (liveness) {
    case HelperLiveness::NotStarted: return "not started";
    case HelperLiveness::Running:    return "running";
    case HelperLiveness::Exited:     return "exited";
    case HelperLiveness::Killed:     return "killed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

ProcessSupervisor::~ProcessSupervisor() {
    // A supervisor destroyed while its helper runs would leave a process holding
    // the shared mapping open, and the next session's create() would then fail with
    // AlreadyExists - a plug-in that outlives the session that hosted it is the one
    // failure the sandbox exists to prevent.
    if (liveness_ == HelperLiveness::Running)
        kill();
    detachHandles();
}

ProcessSupervisor::ProcessSupervisor(ProcessSupervisor&& other) noexcept
    : liveness_(other.liveness_), exitCode_(other.exitCode_), processId_(other.processId_),
      outputPath_(std::move(other.outputPath_)), weTerminated_(other.weTerminated_)
#if defined(_WIN32)
      ,
      processHandle_(other.processHandle_), threadHandle_(other.threadHandle_)
#else
      ,
      pid_(other.pid_)
#endif
{
    other.liveness_ = HelperLiveness::NotStarted;
    other.exitCode_ = -1;
    other.processId_ = 0;
    other.weTerminated_ = false;
#if defined(_WIN32)
    other.processHandle_ = nullptr;
    other.threadHandle_ = nullptr;
#else
    other.pid_ = -1;
#endif
}

ProcessSupervisor& ProcessSupervisor::operator=(ProcessSupervisor&& other) noexcept {
    if (this != &other) {
        if (liveness_ == HelperLiveness::Running)
            kill();
        detachHandles();

        liveness_ = other.liveness_;
        exitCode_ = other.exitCode_;
        processId_ = other.processId_;
        outputPath_ = std::move(other.outputPath_);
        weTerminated_ = other.weTerminated_;
#if defined(_WIN32)
        processHandle_ = other.processHandle_;
        threadHandle_ = other.threadHandle_;
        other.processHandle_ = nullptr;
        other.threadHandle_ = nullptr;
#else
        pid_ = other.pid_;
        other.pid_ = -1;
#endif
        other.liveness_ = HelperLiveness::NotStarted;
        other.exitCode_ = -1;
        other.processId_ = 0;
        other.weTerminated_ = false;
    }
    return *this;
}

void ProcessSupervisor::detachHandles() noexcept {
#if defined(_WIN32)
    if (threadHandle_ != nullptr) {
        ::CloseHandle(threadHandle_);
        threadHandle_ = nullptr;
    }
    if (processHandle_ != nullptr) {
        ::CloseHandle(processHandle_);
        processHandle_ = nullptr;
    }
#endif
    // POSIX needs nothing: the child is reaped by poll()/kill(), and a closed pid is
    // not a handle. Leaving pid_ set is deliberate - takeOutput() and exitCode() are
    // still meaningful after the process is gone.
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------

Status ProcessSupervisor::start(const HelperLaunch& launch) {
    if (launch.executable.empty())
        return failure(ErrorCode::InvalidArgument, "no helper executable was given");
    if (liveness_ == HelperLiveness::Running)
        return failure(ErrorCode::InvalidArgument,
                       "this supervisor is already running a helper",
                       "starting a second one would lose track of the first, and a helper "
                       "nobody is watching is a helper nobody will kill");

    outputPath_ = launch.outputPath;
    weTerminated_ = false;
    exitCode_ = -1;

#if defined(_WIN32)
    // ------------------------------------------------------------------ Windows
    std::wstring commandLine = quoteForCommandLine(launch.executable);
    for (const auto& argument : launch.arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteForCommandLine(argument);
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE; // the child needs the redirected file handle

    HANDLE output = nullptr;
    if (!launch.outputPath.empty()) {
        const std::wstring wideOutput = widen(launch.outputPath);
        output = ::CreateFileW(wideOutput.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
            return failure(ErrorCode::IoError, "could not create the helper's output file",
                           launch.outputPath + ": " + describeLast(::GetLastError()));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (output != nullptr) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION process{};
    // CREATE_NO_WINDOW: a helper is not a console program, and flashing a console
    // per plug-in is the sort of thing a user notices once and never forgives.
    //
    // A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE belongs here and is M7
    // phase 6: it is what makes a helper that spawns children of its own die with
    // the host, and it needs its own tests rather than a flag added in passing.
    const BOOL ok = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
                                     output != nullptr ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &startup, &process);
    if (output != nullptr)
        ::CloseHandle(output);
    if (!ok)
        return failure(ErrorCode::PluginLoadFailed, "could not start the plug-in helper",
                       launch.executable + ": " + describeLast(::GetLastError()));

    processHandle_ = process.hProcess;
    threadHandle_ = process.hThread;
    processId_ = static_cast<std::uint64_t>(process.dwProcessId);
    liveness_ = HelperLiveness::Running;
    return success();
#else
    // -------------------------------------------------------------------- POSIX
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!launch.outputPath.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, launch.outputPath.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, launch.outputPath.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }

    // Its own process group, so that stopping a helper stops whatever it started.
    // A plug-in that spawns a licence server or a bridge of its own is not a
    // hypothetical; killing one process at a time is how a session ends up with
    // orphans holding the shared mapping.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    std::vector<std::string> storage;
    std::vector<char*> argv = buildArgv(launch, storage);

    pid_t pid = -1;
    const int spawnResult = posix_spawn(&pid, launch.executable.c_str(), &actions, &attributes,
                                        argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawnResult != 0)
        return failure(ErrorCode::PluginLoadFailed, "could not start the plug-in helper",
                       launch.executable + ": " + describeErrno(spawnResult));

    pid_ = pid;
    processId_ = static_cast<std::uint64_t>(pid);
    liveness_ = HelperLiveness::Running;
    return success();
#endif
}

// ---------------------------------------------------------------------------
// observation
// ---------------------------------------------------------------------------

HelperLiveness ProcessSupervisor::poll() noexcept {
    if (liveness_ != HelperLiveness::Running)
        return liveness_;

#if defined(_WIN32)
    const DWORD waited = ::WaitForSingleObject(processHandle_, 0);
    if (waited == WAIT_TIMEOUT)
        return liveness_;
    DWORD code = 0;
    if (waited == WAIT_OBJECT_0 && ::GetExitCodeProcess(processHandle_, &code))
        exitCode_ = static_cast<int>(code);
    liveness_ = weTerminated_ ? HelperLiveness::Killed : HelperLiveness::Exited;
    return liveness_;
#else
    int status = 0;
    // The specific pid, never -1: this process may also be running the scanner's
    // one-shot helper through plugin::runChildProcess(), and a general wait would
    // steal that child's status and leave it unreaped.
    const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
    if (waited == 0)
        return liveness_; // still running
    if (waited < 0) {
        // ECHILD means it was already reaped, which for a supervisor that only ever
        // waits on its own pid cannot happen unless the platform took it away. Either
        // way the process is gone, and reporting Running would hang the caller.
        liveness_ = weTerminated_ ? HelperLiveness::Killed : HelperLiveness::Exited;
        return liveness_;
    }
    if (WIFEXITED(status))
        exitCode_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exitCode_ = 128 + WTERMSIG(status); // 128+N, the shell convention
    liveness_ = weTerminated_ ? HelperLiveness::Killed : HelperLiveness::Exited;
    return liveness_;
#endif
}

HelperLiveness ProcessSupervisor::waitForExit(double timeoutSeconds) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(timeoutSeconds > 0.0 ? timeoutSeconds
                                                                                 : 0.0));
    for (;;) {
        const HelperLiveness now = poll();
        if (now != HelperLiveness::Running)
            return now;
        if (std::chrono::steady_clock::now() >= deadline)
            return now;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

std::string ProcessSupervisor::takeOutput() {
    std::string path;
    path.swap(outputPath_);
    return readAndRemove(path);
}

// ---------------------------------------------------------------------------
// stopping
// ---------------------------------------------------------------------------

void ProcessSupervisor::stop(double graceSeconds) noexcept {
    if (liveness_ != HelperLiveness::Running)
        return;
    weTerminated_ = true;

#if defined(_WIN32)
    // There is no polite verb on Windows: TerminateProcess is the only one. The
    // graceful path is the arena's request::kShutdown bit, which SandboxSession sets
    // before calling this, and a helper that honours it is already gone by now.
    ::TerminateProcess(processHandle_, 0xFFFFFFFFu);
    // Assigned rather than discarded: waitForExit() is what reaps the process and
    // records its exit code, and it reports the same liveness it just stored, so
    // taking the value keeps this function honest about having waited.
    liveness_ = waitForExit(graceSeconds > 0.0 ? graceSeconds : kReapWaitSeconds);
#else
    // The group first, then the process: if setpgid failed for any reason the group
    // kill returns ESRCH and the direct signal still stops the helper itself.
    if (::kill(-pid_, SIGTERM) != 0)
        ::kill(pid_, SIGTERM);
    if (waitForExit(graceSeconds) != HelperLiveness::Running)
        return;
    // It ignored SIGTERM. A plug-in that ignores SIGTERM is exactly the case the
    // sandbox exists for, so this is not an escalation to apologise for.
    if (::kill(-pid_, SIGKILL) != 0)
        ::kill(pid_, SIGKILL);
    liveness_ = waitForExit(kReapWaitSeconds);
#endif
    liveness_ = poll();
}

void ProcessSupervisor::kill() noexcept {
    if (liveness_ != HelperLiveness::Running)
        return;
    weTerminated_ = true;

#if defined(_WIN32)
    ::TerminateProcess(processHandle_, 0xFFFFFFFFu);
    liveness_ = waitForExit(kReapWaitSeconds);
#else
    if (::kill(-pid_, SIGKILL) != 0)
        ::kill(pid_, SIGKILL);
    liveness_ = waitForExit(kReapWaitSeconds);
#endif
    // poll() after a kill is what turns "we signalled it" into "it is gone, and here
    // is the number to report": it reaps the child and records 128+N on POSIX, which
    // is how a segfault arrives as 139 rather than as an exit code the plug-in chose.
    liveness_ = poll();
}

} // namespace aura::plugin::sandbox
