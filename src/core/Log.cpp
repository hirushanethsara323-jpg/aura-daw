// ============================================================================
// AURA DAW - src/core/Log.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace aura {
namespace {

std::uint64_t g_startMs = 0;
std::once_flag g_startFlag;

std::uint64_t steadyNowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

LogRecord makeRecord(LogLevel level, std::string_view category, std::string_view message,
                     bool realtime) {
    LogRecord record;
    record.level = level;
    record.timestampMs = Log::nowMs();
    record.threadId = Log::currentThreadId();
    record.realtime = realtime;
    const auto copy = [](auto& dest, std::string_view src) {
        const std::size_t n = std::min(src.size(), dest.size() - 1);
        std::memcpy(dest.data(), src.data(), n);
        dest[n] = '\0';
    };
    copy(record.category, category);
    copy(record.message, message);
    return record;
}

const char* levelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "\x1b[90m";
    case LogLevel::Debug: return "\x1b[36m";
    case LogLevel::Info: return "\x1b[32m";
    case LogLevel::Warning: return "\x1b[33m";
    case LogLevel::Error: return "\x1b[31m";
    case LogLevel::Fatal: return "\x1b[35m";
    }
    return "";
}

} // namespace

const char* logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "?";
}

bool logLevelFromName(std::string_view name, LogLevel& out) noexcept {
    for (int i = 0; i <= static_cast<int>(LogLevel::Fatal); ++i) {
        const auto level = static_cast<LogLevel>(i);
        const char* n = logLevelName(level);
        if (name.size() == std::strlen(n)) {
            bool match = true;
            for (std::size_t c = 0; c < name.size(); ++c) {
                const char a = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(name[c])));
                if (a != n[c]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out = level;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------
void ConsoleSink::write(const LogRecord& record) {
    std::FILE* stream = (record.level >= LogLevel::Error) ? stderr : stdout;
    if (colors_) {
        std::fprintf(stream, "%s[%s] %-8s %-10s %s\x1b[0m\n", levelColor(record.level),
                     record.realtime ? "RT" : "  ", logLevelName(record.level),
                     record.category.data(), record.message.data());
    } else {
        std::fprintf(stream, "[%s] %-8s %-10s %s\n", record.realtime ? "RT" : "  ",
                     logLevelName(record.level), record.category.data(), record.message.data());
    }
}

FileSink::~FileSink() {
    close();
}

bool FileSink::open(const std::string& path, std::uintmax_t maxBytes, int maxFiles) {
    close();
    path_ = path;
    maxBytes_ = maxBytes;
    maxFiles_ = maxFiles;
    file_ = std::fopen(path.c_str(), "ab");
    if (!file_)
        return false;
    if (std::fseek(static_cast<std::FILE*>(file_), 0, SEEK_END) == 0)
        written_ = static_cast<std::uintmax_t>(std::ftell(static_cast<std::FILE*>(file_)));
    return true;
}

void FileSink::close() {
    if (file_) {
        std::fclose(static_cast<std::FILE*>(file_));
        file_ = nullptr;
    }
}

void FileSink::rotateIfNeeded() {
    if (written_ < maxBytes_)
        return;
    close();
    for (int i = maxFiles_ - 1; i >= 1; --i) {
        const std::string from = path_ + "." + std::to_string(i);
        const std::string to = path_ + "." + std::to_string(i + 1);
        std::remove(to.c_str());
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(path_.c_str(), (path_ + ".1").c_str());
    file_ = std::fopen(path_.c_str(), "ab");
    written_ = 0;
}

void FileSink::write(const LogRecord& record) {
    if (!file_)
        return;
    rotateIfNeeded();
    if (!file_)
        return;
    const int written = std::fprintf(static_cast<std::FILE*>(file_),
                                     "%10llu %-8s %-10s %s%s\n",
                                     static_cast<unsigned long long>(record.timestampMs),
                                     logLevelName(record.level), record.category.data(),
                                     record.realtime ? "[rt] " : "", record.message.data());
    if (written > 0)
        written_ += static_cast<std::uintmax_t>(written);
}

void FileSink::flush() {
    if (file_)
        std::fflush(static_cast<std::FILE*>(file_));
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
Log& Log::instance() {
    static Log logger;
    return logger;
}

std::uint64_t Log::nowMs() noexcept {
    std::call_once(g_startFlag, [] { g_startMs = steadyNowMs(); });
    return steadyNowMs() - g_startMs;
}

std::uint32_t Log::currentThreadId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentThreadId());
#else
    static thread_local const std::uint32_t id = [] {
        return static_cast<std::uint32_t>(::getpid()) ^
               static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }();
    return id;
#endif
}

void Log::addSink(std::shared_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Log::clearSinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void Log::dispatch(const LogRecord& record) {
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        history_[historyWrite_] = record;
        historyWrite_ = (historyWrite_ + 1) % kHistoryCapacity;
        historyCount_ = std::min(historyCount_ + 1, kHistoryCapacity);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_)
        sink->write(record);
}

void Log::log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < level_.load(std::memory_order_relaxed))
        return;
    LogRecord record = makeRecord(level, category, message, false);
    record.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    dispatch(record);
}

void Log::logRealtime(LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (level < rtLevel_.load(std::memory_order_relaxed))
        return;
    LogRecord record = makeRecord(level, category, message, true);
    record.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!rtQueue_.tryPush(record))
        droppedRealtime_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t Log::pumpRealtimeQueue(std::size_t maxRecords) {
    std::size_t pumped = 0;
    while (pumped < maxRecords) {
        auto record = rtQueue_.tryPop();
        if (!record.has_value())
            break;
        dispatch(*record);
        ++pumped;
    }
    return pumped;
}

std::vector<LogRecord> Log::recentRecords(std::size_t maxCount) const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    const std::size_t count = std::min(maxCount, historyCount_);
    std::vector<LogRecord> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Start at the oldest retained record.
        const std::size_t index = (historyWrite_ + kHistoryCapacity - historyCount_ + i) % kHistoryCapacity;
        out.push_back(history_[index]);
    }
    if (out.size() > maxCount)
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(maxCount));
    return out;
}

// ---------------------------------------------------------------------------
// Formatting + free functions
// ---------------------------------------------------------------------------
std::string formatStringV(const char* fmt, std::va_list args) {
    std::va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (needed <= 0)
        return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    std::vsnprintf(out.data(), static_cast<std::size_t>(needed) + 1, fmt, args);
    return out;
}

std::string formatString(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::string out = formatStringV(fmt, args);
    va_end(args);
    return out;
}

namespace {
template <typename... Args>
void logFormatted(LogLevel level, const char* category, const char* fmt, Args... args) {
    const std::string message = formatString(fmt, args...);
    Log::instance().log(level, category, message);
}
} // namespace

void logTrace(const char* category, const char* fmt, ...) {
    if (Log::instance().level() > LogLevel::Trace)
        return;
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Trace, category, message);
}

void logDebug(const char* category, const char* fmt, ...) {
    if (Log::instance().level() > LogLevel::Debug)
        return;
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Debug, category, message);
}

void logInfo(const char* category, const char* fmt, ...) {
    if (Log::instance().level() > LogLevel::Info)
        return;
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Info, category, message);
}

void logWarning(const char* category, const char* fmt, ...) {
    if (Log::instance().level() > LogLevel::Warning)
        return;
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Warning, category, message);
}

void logError(const char* category, const char* fmt, ...) {
    if (Log::instance().level() > LogLevel::Error)
        return;
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Error, category, message);
}

void logFatal(const char* category, const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    const std::string message = formatStringV(fmt, args);
    va_end(args);
    Log::instance().log(LogLevel::Fatal, category, message);
    Log::instance().pumpRealtimeQueue();
}

void logRealtime(LogLevel level, const char* category, std::string_view message) noexcept {
    Log::instance().logRealtime(level, category, message);
}

} // namespace aura
