// ============================================================================
// AURA DAW - core/Json.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Purpose-built JSON reader/writer for project, settings and plug-in cache
// files. Written in-house (no third-party dependency) with three goals:
//   1. Deterministic, human-diffable output (stable key order as inserted).
//   2. Clear AuraError diagnostics with line/column on malformed input.
//   3. Zero dependency on a large external library for a format we control.
//
// Documented limitation: this is a *document* model, not a streaming parser.
// Project files are JSON + a binary media payload, so documents stay small
// (text only); audio is never stored inside JSON.
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::json {

class Value;
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

/// A JSON value with an insertion-ordered object representation.
class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n) : type_(Type::Number), number_(n) {}
    Value(int n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(std::int64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(std::uint64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(const char* s) : type_(Type::String), string_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}

    static Value makeArray() {
        Value v;
        v.type_ = Type::Array;
        return v;
    }
    static Value makeObject() {
        Value v;
        v.type_ = Type::Object;
        return v;
    }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    // --- typed accessors (never throw; return fallbacks) ---------------------
    [[nodiscard]] bool asBool(bool fallback = false) const noexcept;
    [[nodiscard]] double asDouble(double fallback = 0.0) const noexcept;
    [[nodiscard]] float asFloat(float fallback = 0.0f) const noexcept;
    [[nodiscard]] int asInt(int fallback = 0) const noexcept;
    [[nodiscard]] std::int64_t asInt64(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] std::string asString(std::string_view fallback = {}) const;

    // --- container access ----------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const Value& operator[](std::size_t index) const; // array (empty value if OOB)
    [[nodiscard]] Value& operator[](std::size_t index);
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    [[nodiscard]] Value& operator[](std::string_view key);
    [[nodiscard]] bool has(std::string_view key) const noexcept;
    [[nodiscard]] bool erase(std::string_view key);
    [[nodiscard]] const Object& members() const noexcept { return object_; }
    [[nodiscard]] const Array& elements() const noexcept { return array_; }
    [[nodiscard]] Array& elements() noexcept { return array_; }

    void push(Value v);
    void set(std::string key, Value v);
    /// Appends to an array, creating it if necessary.
    Value& append(Value v);

    // --- convenience mutators that keep code short in serializers ------------
    void set(std::string key, bool v) { set(std::move(key), Value(v)); }
    void set(std::string key, double v) { set(std::move(key), Value(v)); }
    void set(std::string key, int v) { set(std::move(key), Value(v)); }
    /// 32-bit ids (TrackId, ClipId halves, ...) need their own overloads: without
    /// them the call is ambiguous between int/int64/uint64/bool.
    void set(std::string key, std::uint32_t v) {
        set(std::move(key), Value(static_cast<std::uint64_t>(v)));
    }
    void set(std::string key, std::int64_t v) { set(std::move(key), Value(v)); }
    void set(std::string key, std::uint64_t v) { set(std::move(key), Value(v)); }
    void set(std::string key, const char* v) { set(std::move(key), Value(v)); }
    void set(std::string key, std::string_view v) { set(std::move(key), Value(v)); }
    void set(std::string key, const std::string& v) { set(std::move(key), Value(v)); }

    // --- serialization -------------------------------------------------------
    /// Pretty (indent = 2 spaces) or compact output. Throws nothing.
    [[nodiscard]] std::string dump(bool pretty = true) const;

private:
    void dumpTo(std::string& out, bool pretty, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_; // insertion ordered, linear lookup (objects are small)
};

/// Parses JSON text. On failure returns an AuraError with line/column info.
[[nodiscard]] AuraResult<Value> parse(std::string_view text);

/// Reads and parses a file (UTF-8, BOM tolerated).
[[nodiscard]] AuraResult<Value> parseFile(const std::string& path);

/// Writes JSON to a file atomically (write to .tmp, fsync, rename).
[[nodiscard]] Status writeFile(const std::string& path, const Value& value, bool pretty = true);

/// Escape helper shared with the string utilities.
[[nodiscard]] std::string escape(std::string_view text);

} // namespace aura::json
