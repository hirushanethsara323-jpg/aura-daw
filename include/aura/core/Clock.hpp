// ============================================================================
// AURA DAW - core/Clock.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Monotonic high-resolution clock for measuring audio callbacks, UI frame time
// and benchmark durations.
//
// Why not std::chrono::steady_clock directly: on Windows, steady_clock is
// implemented with QueryPerformanceCounter but its reported period is 100 ns
// while the real resolution is typically ~0.1-1 us and it is NOT synchronized
// across cores on older systems. For audio callback timing we want the most
// accurate counter the platform offers, which on Windows is QPC (used by
// ASIO/WASAPI hosts) and on POSIX CLOCK_MONOTONIC (or CLOCK_MONOTONIC_RAW where
// available). This header wraps both behind one call, and is deliberately
// header-only and allocation-free so it is safe from the audio thread.
//
// Research record: docs/research/TECHNOLOGY_DECISION_RECORD.md (timing section).
// ============================================================================
#pragma once

#include <cstdint>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <time.h>
#endif

namespace aura::core {

/// Monotonic counter in nanoseconds. Never goes backwards; the epoch is
/// arbitrary (use differences, never absolute values).
inline std::uint64_t monotonicNanos() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0)
        return 0;
    // 128-bit intermediate avoided on purpose: counter.QuadPart * 1e9 can
    // overflow 64 bits after ~7 minutes of uptime at 10 MHz. Splitting into
    // seconds + remainder keeps full precision for years of uptime.
    const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
    const std::uint64_t perSecond = static_cast<std::uint64_t>(frequency.QuadPart);
    const std::uint64_t seconds = ticks / perSecond;
    const std::uint64_t remainder = ticks % perSecond;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / perSecond;
#else
#    if defined(CLOCK_MONOTONIC_RAW)
    constexpr clockid_t clockId = CLOCK_MONOTONIC_RAW;
#    else
    constexpr clockid_t clockId = CLOCK_MONOTONIC;
#    endif
    timespec ts{};
    if (clock_gettime(clockId, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

/// Scoped stopwatch. `nanos()` is meaningful while the scope is alive, and the
/// value is frozen by stop().
class Stopwatch {
public:
    Stopwatch() noexcept : start_(monotonicNanos()) {}

    void restart() noexcept {
        start_ = monotonicNanos();
        stopped_ = false;
    }
    void stop() noexcept {
        if (!stopped_) {
            elapsed_ = monotonicNanos() - start_;
            stopped_ = true;
        }
    }
    [[nodiscard]] std::uint64_t nanos() const noexcept {
        return stopped_ ? elapsed_ : monotonicNanos() - start_;
    }
    [[nodiscard]] double millis() const noexcept {
        return static_cast<double>(nanos()) / 1.0e6;
    }
    [[nodiscard]] double seconds() const noexcept {
        return static_cast<double>(nanos()) / 1.0e9;
    }

private:
    std::uint64_t start_ = 0;
    std::uint64_t elapsed_ = 0;
    bool stopped_ = false;
};

} // namespace aura::core
