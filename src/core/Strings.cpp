// ============================================================================
// AURA DAW - src/core/Strings.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Strings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aura::strings {
namespace {

inline char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline char upperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

inline bool isWordBoundary(char c) noexcept {
    return isSpace(c) || c == '_' || c == '-' || c == '/' || c == '\\' || c == '.' || c == '(' ||
           c == ')' || c == ':' || c == ',';
}

} // namespace

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lowerAscii);
    return out;
}

std::string toUpper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), upperAscii);
    return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i]))
            return false;
    }
    return true;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lowerAscii(haystack[i + j]) != lowerAscii(needle[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

int fuzzyScore(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty())
        return 0;
    int score = 0;
    std::size_t h = 0;
    std::size_t lastMatch = std::string_view::npos;
    for (std::size_t n = 0; n < needle.size(); ++n) {
        const char target = lowerAscii(needle[n]);
        bool found = false;
        while (h < haystack.size()) {
            const char c = lowerAscii(haystack[h]);
            if (c == target) {
                found = true;
                // Consecutive match bonus.
                if (lastMatch != std::string_view::npos && lastMatch + 1 == h)
                    score += 8;
                // Word-boundary bonus (matching the first letter of a word).
                if (h == 0 || isWordBoundary(haystack[h - 1]))
                    score += 5;
                ++score;
                lastMatch = h;
                ++h;
                break;
            }
            ++h;
        }
        if (!found)
            return -1;
    }
    // Prefer shorter haystacks (more specific names rank higher).
    score -= static_cast<int>(haystack.size() / 8);
    return score;
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string trimLeft(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && isSpace(text[i]))
        ++i;
    return std::string(text.substr(i));
}

std::string trimRight(std::string_view text) {
    std::size_t n = text.size();
    while (n > 0 && isSpace(text[n - 1]))
        --n;
    return std::string(text.substr(0, n));
}

std::string trim(std::string_view text) {
    std::size_t i = 0;
    std::size_t n = text.size();
    while (i < n && isSpace(text[i]))
        ++i;
    while (n > i && isSpace(text[n - 1]))
        --n;
    return std::string(text.substr(i, n - i));
}

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        std::size_t lineEnd = end;
        if (lineEnd > start && text[lineEnd - 1] == '\r')
            --lineEnd;
        lines.emplace_back(text.substr(start, lineEnd - start));
        start = end + 1;
    }
    return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += separator;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to) {
    if (from.empty())
        return std::string(text);
    std::string out;
    out.reserve(text.size());
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(from, start);
        if (pos == std::string_view::npos) {
            out.append(text.substr(start));
            break;
        }
        out.append(text.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    return out;
}

std::uint64_t hash64(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ull; // FNV offset basis
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull; // FNV prime
    }
    return hash;
}

std::string toHex(std::uint32_t value, int width) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%0*x", width, static_cast<unsigned int>(value));
    return std::string(buffer);
}

bool fromHex(std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty() || text.size() > 8)
        return false;
    std::uint32_t value = 0;
    for (const char c : text) {
        std::uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

bool toDouble(std::string_view text, double& out) noexcept {
    const std::string trimmed = trim(text);
    if (trimmed.empty())
        return false;
    // std::from_chars for floating point is available in GCC 11+/MSVC 2019 16.4+.
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out,
                                        std::chars_format::general);
    if (result.ec != std::errc{})
        return false;
    return result.ptr == trimmed.data() + trimmed.size();
}

bool toInt(std::string_view text, long long& out) noexcept {
    const std::string trimmed = trim(text);
    if (trimmed.empty())
        return false;
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out);
    if (result.ec != std::errc{})
        return false;
    return result.ptr == trimmed.data() + trimmed.size();
}

std::string fromDouble(double value, int maxDecimals) {
    if (!std::isfinite(value))
        return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", maxDecimals, value);
    std::string out(buffer);
    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0')
            out.pop_back();
        if (!out.empty() && out.back() == '.')
            out.pop_back();
    }
    if (out == "-0")
        out = "0";
    return out;
}

std::string padLeft(std::string_view text, std::size_t width, char fill) {
    if (text.size() >= width)
        return std::string(text);
    return std::string(width - text.size(), fill) + std::string(text);
}

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace aura::strings
