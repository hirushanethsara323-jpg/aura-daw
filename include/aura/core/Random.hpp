// ============================================================================
// AURA DAW - core/Random.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deterministic, allocation-free PRNGs. Used for noise generators, dither,
// reverb diffusion and reproducible test signals. Never use std::rand in
// audio code (locking, non-deterministic across platforms).
// ============================================================================
#pragma once

#include <cstdint>

namespace aura {

/// xoshiro128** style generator - tiny state, good quality, branch-free.
class XorShift128 {
public:
    explicit XorShift128(std::uint32_t seed = 1u) noexcept { seedFrom(seed); }

    void seedFrom(std::uint32_t seed) noexcept {
        // SplitMix32 seeding so small seeds still produce well-mixed state.
        std::uint32_t x = seed ? seed : 0x9E3779B9u;
        for (auto& s : state_) {
            x += 0x9E3779B9u;
            std::uint32_t z = x;
            z = (z ^ (z >> 16)) * 0x21F0AAADu;
            z = (z ^ (z >> 15)) * 0x735A2D97u;
            s = z ^ (z >> 15);
        }
        if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0)
            state_[0] = 0x1234567u;
    }

    [[nodiscard]] std::uint32_t nextU32() noexcept {
        const std::uint32_t result = rotl(state_[1] * 5u, 7) * 9u;
        const std::uint32_t t = state_[1] << 9;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 11);
        return result;
    }

    /// Uniform in [-1, 1).
    [[nodiscard]] float nextFloat() noexcept {
        // 24 mantissa bits from the top of the 32-bit output.
        const float unit = static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f);
        return unit * 2.0f - 1.0f;
    }

    /// Uniform in [0, 1).
    [[nodiscard]] float nextUnitFloat() noexcept { return (nextFloat() + 1.0f) * 0.5f; }

    [[nodiscard]] int nextInt(int exclusiveUpper) noexcept {
        if (exclusiveUpper <= 1)
            return 0;
        return static_cast<int>(nextU32() % static_cast<std::uint32_t>(exclusiveUpper));
    }

private:
    static std::uint32_t rotl(std::uint32_t x, int k) noexcept {
        return (x << k) | (x >> (32 - k));
    }

    std::uint32_t state_[4]{};
};

/// TPDF dither source (two independent uniform values summed).
class TpdfDither {
public:
    explicit TpdfDither(std::uint32_t seed = 0x5EEDu) noexcept : rng_(seed) {}

    [[nodiscard]] float nextSample(float amplitude) noexcept {
        return (rng_.nextFloat() + rng_.nextFloat()) * 0.5f * amplitude;
    }

private:
    XorShift128 rng_;
};

} // namespace aura
