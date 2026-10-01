// ============================================================================
// AURA DAW - plugin/sandbox/HelperProcess.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "HelperProcess.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace aura::plugin::sandbox {
namespace {

/// The helper reads its channel from the command line. On POSIX that is a file
/// descriptor number, on Windows a handle value - both are small integers the child
/// inherits and interprets with the same code that created them. (A duplicate under a
/// fixed descriptor number would be an alternative; it buys nothing here, because the
/// child is our own binary and can simply be told which number to look at.)
constexpr const char* kControlArgument = "--control-fd";
#if defined(_WIN32)
constexpr const char* kControlWriteArgument = "--control-write";
#endif

} // namespace

HelperProcess::~HelperProcess() {
#if defined(_WIN32)
    if (processHandle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(processHandle_));
#else
    // A helper that outlives its host is the one outcome a DAW must never allow: it
    // would keep a plug-in loaded against a session that is gone. The destructor
    // therefore kills rather than leaks, and terminate() is the explicit path.
    if (id_ > 0 && !exited_) {
        ::kill(static_cast<pid_t>(id_), SIGKILL);
        int status = 0;
        ::waitpid(static_cast<pid_t>(id_), &status, 0);
    }
#endif
}

HelperProcess::HelperProcess(HelperProcess&& other) noexcept { *this = std::move(other); }

HelperProcess& HelperProcess::operator=(HelperProcess&& other) noexcept {
    if (this == &other)
        return *this;
    release();
#if defined(_WIN32)
    processHandle_ = other.processHandle_;
    other.processHandle_ = nullptr;
#endif
    id_ = other.id_;
    exited_ = other.exited_;
    crashed_ = other.crashed_;
    exitCode_ = other.exitCode_;
    other.id_ = 0;
    other.exited_ = true; // the moved-from object must not try to kill anything
    other.exitCode_ = -1;
    return *this;
}

void HelperProcess::release() noexcept {
#if defined(_WIN32)
    if (processHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(processHandle_));
        processHandle_ = nullptr;
    }
#else
    if (id_ > 0 && !exited_) {
        ::kill(static_cast<pid_t>(id_), SIGKILL);
        int status = 0;
        ::waitpid(static_cast<pid_t>(id_), &status, 0);
    }
#endif
    id_ = 0;
    exited_ = true;
    exitCode_ = -1;
}

bool HelperProcess::running() const noexcept {
#if defined(_WIN32)
    if (processHandle_ == nullptr)
        return false;
    return WaitForSingleObject(static_cast<HANDLE>(processHandle_), 0) == WAIT_TIMEOUT;
#else
    return id_ > 0 && !exited_;
#endif
}

#if defined(_WIN32)

HelperProcess HelperProcess::spawn(const HelperLaunch& launch, ControlChannel& channel,
                                   std::string& error) {
    HelperProcess helper;
    if (launch.executable.empty()) {
        error = "no helper executable to start";
        return helper;
    }
    if (!channel.valid()) {
        error = "the control channel was not created";
        return helper;
    }

    std::string commandLine = launch.executable;
    for (const std::string& argument : launch.arguments)
        commandLine += " " + argument;
    commandLine += " " + std::string(kControlArgument) + " " +
                   std::to_string(channel.childHandle());
    commandLine += " " + std::string(kControlWriteArgument) + " " +
                   std::to_string(channel.childWriteHandle());

    // CreateProcessW takes a mutable buffer and may write to it, so the string is
    // converted once here rather than cast in place.
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // CREATE_NO_WINDOW: a DAW that flashes a console window every time it sandboxes a
    // plug-in has not sandboxed anything a user wants. Both child ends of the channel
    // are inheritable (ControlChannel::createPair keeps the host's ends private),
    // which is what makes the handshake work without a name for the pipe.
    const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (ok == FALSE) {
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ") for " +
                launch.executable;
        return helper;
    }

    CloseHandle(process.hThread);
    helper.processHandle_ = process.hProcess;
    helper.id_ = static_cast<std::uint32_t>(process.dwProcessId);
    channel.closeChildEnd();
    return helper;
}

bool HelperProcess::terminate(std::string& error) {
    if (processHandle_ == nullptr || exited_)
        return true;
    if (TerminateProcess(static_cast<HANDLE>(processHandle_), 3) == FALSE) {
        const DWORD code = GetLastError();
        if (code != ERROR_ACCESS_DENIED) { // ACCESS_DENIED here means "already gone"
            error = "TerminateProcess failed (" + std::to_string(code) + ")";
            return false;
        }
    }
    return true;
}

int HelperProcess::waitForExit(double timeoutSeconds, bool& timedOut, std::string& error) {
    timedOut = false;
    if (processHandle_ == nullptr) {
        error = "no helper process";
        return -1;
    }
    const DWORD milliseconds =
        timeoutSeconds < 0.0 ? INFINITE : static_cast<DWORD>(timeoutSeconds * 1000.0);
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(processHandle_), milliseconds);
    if (wait == WAIT_TIMEOUT) {
        timedOut = true;
        return -1;
    }
    if (wait != WAIT_OBJECT_0) {
        error = "WaitForSingleObject failed (" + std::to_string(GetLastError()) + ")";
        return -1;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &code) == FALSE) {
        error = "GetExitCodeProcess failed (" + std::to_string(GetLastError()) + ")";
        return -1;
    }
    exited_ = true;
    exitCode_ = static_cast<int>(code);
    // Exit 0 is "asked to stop"; anything else is abnormal for a helper. The caller
    // maps the codes it knows (the helper's own usage error is 2) and treats the rest
    // as a crash, which is exactly the distinction the sandbox needs: a dry plug-in
    // and a line in the log, not a session that ends with it.
    crashed_ = code != 0;
    return exitCode_;
}

#else // POSIX

HelperProcess HelperProcess::spawn(const HelperLaunch& launch, ControlChannel& channel,
                                   std::string& error) {
    HelperProcess helper;
    if (launch.executable.empty()) {
        error = "no helper executable to start";
        return helper;
    }
    if (!channel.valid()) {
        error = "the control channel was not created";
        return helper;
    }

    std::vector<std::string> arguments;
    arguments.reserve(launch.arguments.size() + 3);
    arguments.push_back(launch.executable); // argv[0]
    for (const std::string& argument : launch.arguments)
        arguments.push_back(argument);
    arguments.push_back(kControlArgument);
    arguments.push_back(std::to_string(static_cast<long>(channel.childHandle())));

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    // posix_spawn, not fork: the host is a multithreaded audio application, and fork
    // in a process with a running audio thread is a well-known way to inherit a lock
    // the child can never release.
    //
    // No file actions are needed for the channel: the child's descriptor is not
    // close-on-exec (only the host's end is - see ControlChannel::createPair), so it
    // survives into the helper by default and the number on the command line still
    // refers to it.
    pid_t pid = -1;
    const int result =
        ::posix_spawn(&pid, launch.executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (result != 0) {
        error = std::string("posix_spawn failed: ") + std::strerror(result);
        return helper;
    }
    helper.id_ = static_cast<std::uint32_t>(pid);
    channel.closeChildEnd();
    return helper;
}

bool HelperProcess::terminate(std::string& error) {
    if (id_ == 0 || exited_)
        return true;
    // SIGKILL, not SIGTERM: this is the path taken when a plug-in has already ignored
    // a polite request, and a helper that does not respond must still be guaranteed to
    // die. Anything the helper needs to do first (flush state, tell the host why)
    // happens on the graceful path, which is the control protocol's "quit" message.
    if (::kill(static_cast<pid_t>(id_), SIGKILL) != 0 && errno != ESRCH) {
        error = std::string("kill failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

int HelperProcess::waitForExit(double timeoutSeconds, bool& timedOut, std::string& error) {
    timedOut = false;
    if (id_ == 0) {
        error = "no helper process";
        return -1;
    }
    if (exited_)
        return exitCode_;

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(id_), &status, WNOHANG);
        if (result == static_cast<pid_t>(id_)) {
            exited_ = true;
            if (WIFEXITED(status)) {
                exitCode_ = WEXITSTATUS(status);
                crashed_ = exitCode_ != 0;
            } else {
                // 128 + signal, so a signal death is visible as one and cannot be
                // mistaken for a legitimate exit code.
                exitCode_ = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                crashed_ = true;
            }
            return exitCode_;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            error = std::string("waitpid failed: ") + std::strerror(errno);
            return -1;
        }
        if (timeoutSeconds >= 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
                timeoutSeconds) {
            timedOut = true;
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // a poll, not a spin
    }
}

#endif

} // namespace aura::plugin::sandbox
