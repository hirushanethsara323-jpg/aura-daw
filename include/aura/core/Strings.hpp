// ============================================================================
// AURA DAW - core/Strings.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small, dependency-free string helpers. No regex, no locale, no allocation
// surprises - real-time code must not use anything in here.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::strings {

[[nodiscard]] std::string toLower(std::string_view text);
[[nodiscard]] std::string toUpper(std::string_view text);

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept;

/// Does `haystack` contain `needle`, case-insensitively?
[[nodiscard]] bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept;

/// Fuzzy subsequence match used by the command palette / plug-in search.
/// Returns a score >= 0 (higher is better) and -1 when there is no match.
/// Consecutive matches and word-boundary matches score higher.
[[nodiscard]] int fuzzyScore(std::string_view haystack, std::string_view needle) noexcept;

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) noexcept;
[[nodiscard]] bool endsWith(std::string_view text, std::string_view suffix) noexcept;

[[nodiscard]] std::string trim(std::string_view text);
[[nodiscard]] std::string trimLeft(std::string_view text);
[[nodiscard]] std::string trimRight(std::string_view text);

[[nodiscard]] std::vector<std::string> split(std::string_view text, char delimiter);
[[nodiscard]] std::vector<std::string> splitLines(std::string_view text);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

[[nodiscard]] std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

/// Stable, platform-independent string hash (FNV-1a 64).
[[nodiscard]] std::uint64_t hash64(std::string_view text) noexcept;

/// Hex helpers used by colour parsing and project files.
[[nodiscard]] std::string toHex(std::uint32_t value, int width = 6);
[[nodiscard]] bool fromHex(std::string_view text, std::uint32_t& out) noexcept;

/// Parses "1.5", "-3", "1e-3" without locale dependency. Returns false on junk.
[[nodiscard]] bool toDouble(std::string_view text, double& out) noexcept;
[[nodiscard]] bool toInt(std::string_view text, long long& out) noexcept;

/// Formats a double without trailing zeros, always '.' as decimal separator.
[[nodiscard]] std::string fromDouble(double value, int maxDecimals = 6);

/// Left-pads with `fill` up to `width`.
[[nodiscard]] std::string padLeft(std::string_view text, std::size_t width, char fill = ' ');

/// Escapes/unescapes for the JSON+log safe transport of user text.
[[nodiscard]] std::string escapeJson(std::string_view text);

} // namespace aura::strings
