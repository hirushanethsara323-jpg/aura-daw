// ============================================================================
// AURA DAW - src/core/Json.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Strings.hpp"

namespace aura::json {
namespace {

const Value g_nullValue{};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void writeIndent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void writeNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null"; // JSON has no NaN/inf; document as null
        return;
    }
    // Integers print without a decimal point (nicer diffs, smaller files).
    if (value == std::floor(value) && std::abs(value) < 1.0e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        out += buffer;
        return;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out += buffer;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    AuraResult<Value> run() {
        skipWhitespace();
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3; // UTF-8 BOM
        }
        Value root;
        if (!parseValue(root))
            return failure(error_);
        skipWhitespace();
        if (pos_ != text_.size()) {
            setError("Unexpected trailing content");
            return failure(error_);
        }
        return success(std::move(root));
    }

private:
    void skipWhitespace() noexcept {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    void setError(const std::string& what) {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        error_ = makeError(ErrorCode::ParseError,
                           "JSON parse error at line " + std::to_string(line) + ", column " +
                               std::to_string(column) + ": " + what,
                           std::string(text_.substr(pos_, std::min<std::size_t>(24, text_.size() - pos_))));
    }

    bool consume(char expected) noexcept {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parseValue(Value& out) {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            setError("Unexpected end of input");
            return false;
        }
        const char c = text_[pos_];
        switch (c) {
        case '{': return parseObject(out);
        case '[': return parseArray(out);
        case '"': {
            std::string s;
            if (!parseString(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            if (text_.substr(pos_, 4) == "true") {
                pos_ += 4;
                out = Value(true);
                return true;
            }
            setError("Invalid literal");
            return false;
        case 'f':
            if (text_.substr(pos_, 5) == "false") {
                pos_ += 5;
                out = Value(false);
                return true;
            }
            setError("Invalid literal");
            return false;
        case 'n':
            if (text_.substr(pos_, 4) == "null") {
                pos_ += 4;
                out = Value(nullptr);
                return true;
            }
            setError("Invalid literal");
            return false;
        default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out) {
        ++pos_; // '{'
        out = Value::makeObject();
        skipWhitespace();
        if (consume('}'))
            return true;
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key))
                return false;
            skipWhitespace();
            if (!consume(':')) {
                setError("Expected ':' after object key");
                return false;
            }
            Value member;
            if (!parseValue(member))
                return false;
            out.set(std::move(key), std::move(member));
            skipWhitespace();
            if (consume(','))
                continue;
            if (consume('}'))
                return true;
            setError("Expected ',' or '}' in object");
            return false;
        }
    }

    bool parseArray(Value& out) {
        ++pos_; // '['
        out = Value::makeArray();
        skipWhitespace();
        if (consume(']'))
            return true;
        while (true) {
            Value element;
            if (!parseValue(element))
                return false;
            out.push(std::move(element));
            skipWhitespace();
            if (consume(','))
                continue;
            if (consume(']'))
                return true;
            setError("Expected ',' or ']' in array");
            return false;
        }
    }

    bool parseString(std::string& out) {
        if (!consume('"')) {
            setError("Expected string");
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                setError("Unterminated escape sequence");
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int code = 0;
                if (!parseHex4(code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    // High surrogate - expect a low surrogate to follow.
                    if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned int low = 0;
                        if (!parseHex4(low))
                            return false;
                        if (low >= 0xDC00 && low <= 0xDFFF)
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                appendUtf8(out, code);
                break;
            }
            default:
                setError("Invalid escape sequence");
                return false;
            }
        }
        setError("Unterminated string");
        return false;
    }

    bool parseHex4(unsigned int& code) {
        if (pos_ + 4 > text_.size()) {
            setError("Truncated \\u escape");
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            unsigned int digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned int>(c - 'A' + 10);
            else {
                setError("Invalid hex digit in \\u escape");
                return false;
            }
            code = (code << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool parseNumber(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
            ++pos_;
        bool any = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
            any = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
                any = true;
            }
        }
        if (any && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
                ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
                ++pos_;
        }
        if (!any) {
            setError("Invalid number");
            return false;
        }
        const std::string numberText(text_.substr(start, pos_ - start));
        double value = 0.0;
        if (!strings::toDouble(numberText, value)) {
            setError("Invalid number '" + numberText + "'");
            return false;
        }
        out = Value(value);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    AuraError error_{};
};

// ---------------------------------------------------------------------------
// UTF-8 aware writer for strings
// ---------------------------------------------------------------------------
void writeString(std::string& out, std::string_view text) {
    out += '"';
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
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                out += buffer;
            } else {
                out += c; // UTF-8 passes through unchanged
            }
        }
    }
    out += '"';
}

} // namespace

// ---------------------------------------------------------------------------
// Value accessors
// ---------------------------------------------------------------------------
bool Value::asBool(bool fallback) const noexcept {
    if (type_ == Type::Bool)
        return bool_;
    if (type_ == Type::Number)
        return number_ != 0.0;
    return fallback;
}

double Value::asDouble(double fallback) const noexcept {
    if (type_ == Type::Number)
        return number_;
    if (type_ == Type::Bool)
        return bool_ ? 1.0 : 0.0;
    return fallback;
}

float Value::asFloat(float fallback) const noexcept {
    return static_cast<float>(asDouble(static_cast<double>(fallback)));
}

int Value::asInt(int fallback) const noexcept {
    if (type_ == Type::Number)
        return static_cast<int>(number_);
    if (type_ == Type::Bool)
        return bool_ ? 1 : 0;
    return fallback;
}

std::int64_t Value::asInt64(std::int64_t fallback) const noexcept {
    if (type_ == Type::Number)
        return static_cast<std::int64_t>(number_);
    return fallback;
}

std::string Value::asString(std::string_view fallback) const {
    if (type_ == Type::String)
        return string_;
    if (type_ == Type::Number)
        return strings::fromDouble(number_);
    if (type_ == Type::Bool)
        return bool_ ? "true" : "false";
    return std::string(fallback);
}

std::size_t Value::size() const noexcept {
    if (type_ == Type::Array)
        return array_.size();
    if (type_ == Type::Object)
        return object_.size();
    return 0;
}

const Value& Value::operator[](std::size_t index) const {
    if (type_ == Type::Array && index < array_.size())
        return array_[index];
    return g_nullValue;
}

Value& Value::operator[](std::size_t index) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    if (index >= array_.size())
        array_.resize(index + 1);
    return array_[index];
}

const Value& Value::operator[](std::string_view key) const {
    if (type_ == Type::Object) {
        for (const auto& [k, v] : object_) {
            if (k == key)
                return v;
        }
    }
    return g_nullValue;
}

Value& Value::operator[](std::string_view key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& [k, v] : object_) {
        if (k == key)
            return v;
    }
    object_.emplace_back(std::string(key), Value{});
    return object_.back().second;
}

bool Value::has(std::string_view key) const noexcept {
    if (type_ != Type::Object)
        return false;
    for (const auto& [k, v] : object_) {
        if (k == key)
            return true;
    }
    return false;
}

bool Value::erase(std::string_view key) {
    if (type_ != Type::Object)
        return false;
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

void Value::push(Value v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(v));
}

void Value::set(std::string key, Value v) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& [k, existing] : object_) {
        if (k == key) {
            existing = std::move(v);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(v));
}

Value& Value::append(Value v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(v));
    return array_.back();
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void Value::dumpTo(std::string& out, bool pretty, int indent, int depth) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: writeNumber(out, number_); break;
    case Type::String: writeString(out, string_); break;
    case Type::Array: {
        if (array_.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < array_.size(); ++i) {
            if (i > 0)
                out += ',';
            if (pretty) {
                out += '\n';
                writeIndent(out, depth + indent);
            }
            array_[i].dumpTo(out, pretty, indent, depth + indent);
        }
        if (pretty) {
            out += '\n';
            writeIndent(out, depth);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        if (object_.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < object_.size(); ++i) {
            if (i > 0)
                out += ',';
            if (pretty) {
                out += '\n';
                writeIndent(out, depth + indent);
            }
            writeString(out, object_[i].first);
            out += ':';
            if (pretty)
                out += ' ';
            object_[i].second.dumpTo(out, pretty, indent, depth + indent);
        }
        if (pretty) {
            out += '\n';
            writeIndent(out, depth);
        }
        out += '}';
        break;
    }
    }
}

std::string Value::dump(bool pretty) const {
    std::string out;
    out.reserve(4096);
    dumpTo(out, pretty, 2, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
AuraResult<Value> parse(std::string_view text) {
    Parser parser(text);
    return parser.run();
}

AuraResult<Value> parseFile(const std::string& path) {
    auto text = filesystem::readTextFile(path);
    if (!text)
        return failure(text.error());
    return parse(text.value());
}

Status writeFile(const std::string& path, const Value& value, bool pretty) {
    return filesystem::writeTextFileAtomic(path, value.dump(pretty));
}

std::string escape(std::string_view text) {
    std::string out;
    writeString(out, text); // includes surrounding quotes
    return out;
}

} // namespace aura::json
