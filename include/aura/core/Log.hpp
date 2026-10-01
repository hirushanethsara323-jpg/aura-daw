// ============================================================================
// AURA DAW - core/Log.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structured logging.
//   * Control threads: logTrace/logDebug/... format and dispatch to sinks.
//   * Audio thread:   logRealtime() NEVER blocks, never allocates and never
//                     touches a file. It formats into a fixed stack buffer and
//                     tries to push into a lock-free ring that a background
//                     drainer (Log::pumpRealtimeQueue) writes out.
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "aura/core/SpscQueue.hpp"

namespace aura {

enum class LogLevel : std::int32_t { Trace = 0, Debug, Info, Warning, Error, Fatal };

const char* logLevelName(LogLevel level) noexcept;
bool logLevelFromName(std::string_view name, LogLevel& out) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::uint64_t timestampMs = 0; ///< steady clock, ms since process start
    std::uint64_t sequence = 0;
    std::uint32_t threadId = 0;
    bool realtime = false; ///< came from an audio/RT thread
    std::array<char, 48> category{};
    std::array<char, 512> message{};
};

/// Sink interface. Implementations must be thread-safe; the logger serialises
/// calls with a spin-free mutex on control threads only.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

class ConsoleSink final : public LogSink {
public:
    explicit ConsoleSink(bool useColors = true) : colors_(useColors) {}
    void write(const LogRecord& record) override;

private:
    bool colors_;
};

/// Rotating file sink. Writes synchronously but is only ever called from
/// control threads (the RT path goes through the queue drainer).
class FileSink final : public LogSink {
public:
    FileSink() = default;
    ~FileSink() override;

    /// Opens `path`; rotates to `path.1`, `path.2` ... keeping maxFiles-1 archives.
    bool open(const std::string& path, std::uintmax_t maxBytes = 4u * 1024u * 1024u,
              int maxFiles = 4);
    void close();
    void write(const LogRecord& record) override;
    void flush() override;

private:
    void rotateIfNeeded();

    std::string path_;
    void* file_ = nullptr; // std::FILE*
    std::uintmax_t maxBytes_ = 4u * 1024u * 1024u;
    int maxFiles_ = 4;
    std::uintmax_t written_ = 0;
};

/// Optional callback sink (used by the GUI to mirror logs into a diagnostics pane).
class CallbackSink final : public LogSink {
public:
    using Callback = std::function<void(const LogRecord&)>;
    explicit CallbackSink(Callback cb) : cb_(std::move(cb)) {}
    void write(const LogRecord& record) override {
        if (cb_)
            cb_(record);
    }

private:
    Callback cb_;
};

class Log {
public:
    static Log& instance();

    void addSink(std::shared_ptr<LogSink> sink);
    void clearSinks();
    void setLevel(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
    void setRealtimeLevel(LogLevel level) noexcept {
        rtLevel_.store(level, std::memory_order_relaxed);
    }

    /// Control-thread logging. Never call from the audio thread.
    void log(LogLevel level, std::string_view category, std::string_view message);

    /// Real-time safe: formats into a stack buffer, tries to enqueue.
    /// Dropped records are counted (see droppedRealtimeRecords()).
    void logRealtime(LogLevel level, std::string_view category, std::string_view message) noexcept;

    /// Drain queued RT records into the sinks. Call from a timer/worker thread.
    std::size_t pumpRealtimeQueue(std::size_t maxRecords = 256);

    [[nodiscard]] std::uint64_t droppedRealtimeRecords() const noexcept {
        return droppedRealtime_.load(std::memory_order_relaxed);
    }

    /// Last N records (ring of formatted strings), for diagnostics reports.
    [[nodiscard]] std::vector<LogRecord> recentRecords(std::size_t maxCount = 200) const;

    static std::uint64_t nowMs() noexcept;
    static std::uint32_t currentThreadId() noexcept;

private:
    Log() = default;
    void dispatch(const LogRecord& record);

    std::mutex mutex_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<LogLevel> rtLevel_{LogLevel::Warning};
    std::atomic<std::uint64_t> droppedRealtime_{0};
    std::atomic<std::uint64_t> sequence_{0};

    static constexpr std::size_t kRtQueueCapacity = 512;
    SpscQueue<LogRecord, kRtQueueCapacity> rtQueue_;

    static constexpr std::size_t kHistoryCapacity = 256;
    mutable std::mutex historyMutex_;
    std::array<LogRecord, kHistoryCapacity> history_{};
    std::size_t historyWrite_ = 0;
    std::size_t historyCount_ = 0;
};

/// printf-style formatting into a std::string (bounded, allocation only on the
/// calling thread - never use from the audio thread).
std::string formatString(const char* fmt, ...);
std::string formatStringV(const char* fmt, std::va_list args);

/// ---------------------------------------------------------------------------
/// Free functions + macros.
/// ---------------------------------------------------------------------------
void logTrace(const char* category, const char* fmt, ...);
void logDebug(const char* category, const char* fmt, ...);
void logInfo(const char* category, const char* fmt, ...);
void logWarning(const char* category, const char* fmt, ...);
void logError(const char* category, const char* fmt, ...);
void logFatal(const char* category, const char* fmt, ...);

/// Real-time safe variant (accepts a single already-built string_view).
void logRealtime(LogLevel level, const char* category, std::string_view message) noexcept;

#define AURA_LOG_TRACE(cat, ...) ::aura::logTrace(cat, __VA_ARGS__)
#define AURA_LOG_DEBUG(cat, ...) ::aura::logDebug(cat, __VA_ARGS__)
#define AURA_LOG_INFO(cat, ...) ::aura::logInfo(cat, __VA_ARGS__)
#define AURA_LOG_WARN(cat, ...) ::aura::logWarning(cat, __VA_ARGS__)
#define AURA_LOG_ERROR(cat, ...) ::aura::logError(cat, __VA_ARGS__)
#define AURA_LOG_FATAL(cat, ...) ::aura::logFatal(cat, __VA_ARGS__)

} // namespace aura
