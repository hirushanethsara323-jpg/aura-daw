// ============================================================================
// AURA DAW - core/Realtime.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-time discipline utilities:
//   * ScopedNoDenormals  - FTZ/DAZ enable for the current block (x86 SSE2+)
//   * thread role tagging + AURA_RT_ASSERT
//   * allocation tracking used by the RT-safety test suite to prove that the
//     audio callback performs zero heap allocations
//
// Allocation tracking is compiled in only when AURA_ENABLE_ALLOC_TRACKING is
// defined (the `aura_rt_tracking` CMake target) so release builds pay nothing.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aura::rt {

/// Marks the calling thread as a real-time (audio) thread. Do this once at the
/// top of the audio callback thread; it makes AURA_RT_ASSERT meaningful and
/// lets the logger/doc-checks flag misuse.
void enterRealtimeThread() noexcept;
void exitRealtimeThread() noexcept;
[[nodiscard]] bool isRealtimeThread() noexcept;

/// Runtime counter of allocations observed while the RT flag was active.
[[nodiscard]] std::uint64_t realtimeAllocationCount() noexcept;
void resetRealtimeAllocationCount() noexcept;

/// Enabled/disabled independently of thread role: the test harness turns this
/// on so it can assert "this whole block did no allocation".
void setTrackingEnabled(bool enabled) noexcept;
[[nodiscard]] bool trackingEnabled() noexcept;

/// Called by the global operator new/delete replacement when tracking is on.
void noteAllocation(std::size_t bytes) noexcept;
void noteDeallocation(std::size_t bytes) noexcept;
[[nodiscard]] std::uint64_t totalAllocationCount() noexcept;
[[nodiscard]] std::int64_t outstandingAllocationCount() noexcept;

/// True when a real-time thread is running with tracking enabled.
[[nodiscard]] bool shouldFlagAllocations() noexcept;

/// Sets and restores the floating-point environment for the scope: flush-to-zero
/// and denormals-are-zero on x86, so feedback paths cannot fall into subnormal
/// arithmetic (a classic 10x CPU spike in reverbs/delays).
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept;
    ~ScopedNoDenormals() noexcept;
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    unsigned int savedState_ = 0;
#if defined(_MSC_VER)
    unsigned int savedMxcsr_ = 0;
#endif
};

} // namespace aura::rt

/// Assertion that also trips in release builds when a violation is detected in
/// a real-time thread (logged once, counted, never throws).
#if defined(AURA_ENABLE_RT_ASSERTS)
#define AURA_RT_ASSERT(cond, what)                                                                 \
    do {                                                                                           \
        if (!(cond))                                                                               \
            ::aura::rt::reportViolation(__FILE__, __LINE__, what);                                 \
    } while (false)
#else
#define AURA_RT_ASSERT(cond, what) ((void)0)
#endif

namespace aura::rt {
void reportViolation(const char* file, int line, const char* what) noexcept;
[[nodiscard]] std::uint64_t violationCount() noexcept;
} // namespace aura::rt
