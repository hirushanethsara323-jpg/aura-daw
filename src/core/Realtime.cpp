// ============================================================================
// AURA DAW - src/core/Realtime.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Realtime.hpp"

#include <cstdio>

#if defined(_MSC_VER)
#include <float.h>
#include <intrin.h>
#elif defined(__SSE2__) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace aura::rt {
namespace {

thread_local bool g_isRealtimeThread = false;
thread_local std::uint64_t g_rtAllocations = 0;

std::atomic<bool> g_trackingEnabled{false};
std::atomic<std::uint64_t> g_totalAllocations{0};
std::atomic<std::int64_t> g_outstandingAllocations{0};
std::atomic<std::uint64_t> g_violations{0};
std::atomic<bool> g_violationReported{false};

} // namespace

void enterRealtimeThread() noexcept {
    g_isRealtimeThread = true;
}
void exitRealtimeThread() noexcept {
    g_isRealtimeThread = false;
}
bool isRealtimeThread() noexcept {
    return g_isRealtimeThread;
}

void setTrackingEnabled(bool enabled) noexcept {
    g_trackingEnabled.store(enabled, std::memory_order_relaxed);
}
bool trackingEnabled() noexcept {
    return g_trackingEnabled.load(std::memory_order_relaxed);
}

void noteAllocation(std::size_t bytes) noexcept {
    (void)bytes;
    g_totalAllocations.fetch_add(1, std::memory_order_relaxed);
    g_outstandingAllocations.fetch_add(1, std::memory_order_relaxed);
    if (g_isRealtimeThread)
        ++g_rtAllocations;
}

void noteDeallocation(std::size_t bytes) noexcept {
    (void)bytes;
    g_outstandingAllocations.fetch_sub(1, std::memory_order_relaxed);
}

bool shouldFlagAllocations() noexcept {
    return g_trackingEnabled.load(std::memory_order_relaxed) && g_isRealtimeThread;
}

std::uint64_t realtimeAllocationCount() noexcept {
    return g_rtAllocations;
}
void resetRealtimeAllocationCount() noexcept {
    g_rtAllocations = 0;
    g_totalAllocations.store(0, std::memory_order_relaxed);
}
std::uint64_t totalAllocationCount() noexcept {
    return g_totalAllocations.load(std::memory_order_relaxed);
}
std::int64_t outstandingAllocationCount() noexcept {
    return g_outstandingAllocations.load(std::memory_order_relaxed);
}

void reportViolation(const char* file, int line, const char* what) noexcept {
    g_violations.fetch_add(1, std::memory_order_relaxed);
    // Log at most once per process to avoid a log flood from the audio thread.
    bool expected = false;
    if (g_violationReported.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "RT violation: %s (%s:%d)", what ? what : "unknown",
                      file ? file : "?", line);
        std::fputs(buffer, stderr);
        std::fputc('\n', stderr);
    }
}

std::uint64_t violationCount() noexcept {
    return g_violations.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// ScopedNoDenormals
// ---------------------------------------------------------------------------
ScopedNoDenormals::ScopedNoDenormals() noexcept {
#if defined(_MSC_VER)
    savedMxcsr_ = _mm_getcsr();
    // FTZ (bit 15) + DAZ (bit 6)
    _mm_setcsr(savedMxcsr_ | 0x8040u);
#elif defined(__SSE2__) || defined(__x86_64__)
    savedState_ = _mm_getcsr();
    _mm_setcsr(savedState_ | 0x8040u);
#endif
}

ScopedNoDenormals::~ScopedNoDenormals() noexcept {
#if defined(_MSC_VER)
    _mm_setcsr(savedMxcsr_);
#elif defined(__SSE2__) || defined(__x86_64__)
    if (savedState_ != 0)
        _mm_setcsr(savedState_);
#endif
}

} // namespace aura::rt
