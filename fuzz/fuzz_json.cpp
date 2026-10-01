// ============================================================================
// AURA DAW - fuzz/fuzz_json.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Fuzzes core::json::parse() - the parser every persisted artefact goes through:
// the project manifest, settings, the plug-in database, the plug-in cache. It is
// the one piece of AURA that eats arbitrary bytes from other people's machines.
//
// Invariants (a violation is a bug worth fixing, not a fuzzing artefact):
//   1. parse() returns a value or an error - never crashes, never asserts;
//   2. a parsed value can be quoted back out (dump) and re-parsed, and the second
//      parse succeeds - a serialiser that emits something its own parser rejects
//      would corrupt every save;
//   3. dumping is deterministic: the same value dumps to the same bytes.
//
// A hang counts as a finding too: libFuzzer's timeout turns it into a crash. The
// document model is depth- and size-limited by the parser itself, so a pathological
// input must be *rejected* rather than consume memory without bound.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aura/core/Json.hpp"
#include "fuzz/FuzzSupport.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    auto parsed = aura::json::parse(text);
    if (!parsed)
        return 0; // a rejection is a valid outcome; the error path is the point

    const aura::json::Value& value = parsed.value();
    const std::string first = value.dump(false);
    const std::string second = value.dump(false);
    if (first != second)
        __builtin_trap(); // non-deterministic serialisation

    // Round trip: whatever we wrote must be readable by our own parser.
    auto reparsed = aura::json::parse(first);
    if (!reparsed)
        __builtin_trap();
    if (reparsed.value().dump(false) != first)
        __builtin_trap(); // not idempotent: parse(dump(v)) must be stable

    return 0;
}
