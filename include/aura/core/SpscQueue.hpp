// ============================================================================
// AURA DAW - core/SpscQueue.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixed-capacity single-producer / single-consumer queue. Wait-free for both
// sides (try_push / try_pop never spin, never lock, never allocate), which is
// exactly what the UI <-> audio-thread boundary needs.
//
// Ordering: producer publishes with release, consumer observes with acquire.
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace aura {

#if defined(_MSC_VER)
// C4324 ("structure was padded due to alignment specifier") is the whole point of
// this class: head and tail each get their own cache line so the producer and the
// consumer never write to the same one. MSVC warns at /W4 and the project builds
// with /WX, so the warning is silenced here, where it is understood - not globally.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue payloads must be trivially copyable to stay RT-safe");

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Producer side. Returns false when full (caller decides whether to drop).
    bool tryPush(const T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) % Capacity;
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        storage_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns nullopt when empty.
    [[nodiscard]] std::optional<T> tryPop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt; // empty
        T value = storage_[tail];
        tail_.store((tail + 1) % Capacity, std::memory_order_release);
        return value;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// Approximate size (safe to call from either side; used for diagnostics).
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity - tail + head);
    }

private:
    std::array<T, Capacity> storage_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aura
