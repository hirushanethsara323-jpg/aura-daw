// ============================================================================
// AURA DAW - util/Localization.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Localization architecture. UI code must never hardcode user-visible English:
// it calls translate("key") or the AURA_TR / AURA_TRF macros and ships a
// `lang/en.json` catalogue. Language packs are plain JSON so translators do
// not need a toolchain, and missing keys fall back to English, then to the key
// itself (which keeps the UI usable and makes gaps obvious in testing).
//
// Initial catalogue: English. Adding Sinhala / Tamil / German / Japanese later
// is a data-only change (see docs/DEVELOPMENT.md#localization).
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::loc {

inline constexpr const char* kFallbackLanguage = "en";

/// A loaded language pack.
struct Catalogue {
    std::string languageCode = kFallbackLanguage;
    std::string displayName = "English";
    /// BCP-47-ish code, plus optional parent fallback ("de-AT" -> "de").
    std::string fallbackCode;
    std::unordered_map<std::string, std::string> entries;

    [[nodiscard]] bool has(std::string_view key) const {
        return entries.find(std::string(key)) != entries.end();
    }
};

class Translator {
public:
    static Translator& instance();

    /// Loads a catalogue from a JSON file: {"language": {...}, "strings": {...}}
    Status loadFromFile(const std::string& path);

    /// Registers a catalogue in memory (used by tests and embedded defaults).
    void setCatalogue(Catalogue catalogue);

    /// Activates a previously loaded language. Falls back to English when the
    /// requested language is unavailable; enqueues a warning (never throws).
    Status setLanguage(const std::string& languageCode);
    [[nodiscard]] const std::string& language() const noexcept { return activeLanguage_; }

    /// Look up `key`. Order: active catalogue -> fallback catalogue -> key.
    [[nodiscard]] std::string translate(std::string_view key) const;

    /// Look up a key and substitute {0}, {1}, ... placeholders.
    [[nodiscard]] std::string translate(std::string_view key,
                                        const std::vector<std::string>& arguments) const;

    /// Translate an AuraError for display: prefers the localized message for
    /// the error code, falls back to the plain-English text stored in the error.
    [[nodiscard]] std::string translateError(const AuraError& error) const;

    /// Diagnostics: keys requested but missing (surfaced by the test suite).
    [[nodiscard]] std::vector<std::string> missingKeys() const;
    void clearMissingKeyLog();

    /// Available language codes (embedded defaults + loaded packs).
    [[nodiscard]] std::vector<std::string> availableLanguages() const;

private:
    Translator();
    const Catalogue* find(const std::string& code) const;

    std::string activeLanguage_ = kFallbackLanguage;
    std::vector<Catalogue> catalogues_;
    mutable std::vector<std::string> missingKeys_;
};

/// Convenience: translate + format "{0}" placeholders.
std::string tr(std::string_view key);
std::string trf(std::string_view key, const std::vector<std::string>& arguments);

/// Installs the built-in English catalogue (called by App at startup; also by
/// unit tests so they exercise the same code path).
void installBuiltinEnglishCatalogue();

} // namespace aura::loc
