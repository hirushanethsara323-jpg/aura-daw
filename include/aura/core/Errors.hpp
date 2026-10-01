// ============================================================================
// AURA DAW - Advanced Unified Recording & Audio
// Copyright (c) 2026 The AURA DAW Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Errors.hpp - error taxonomy and AuraResult<T>. No exceptions cross the
// public API boundary; technical detail is separated from the user-facing
// message so the UI never has to show raw HRESULT / errno text.
// ============================================================================
#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(AURA_ENABLE_RT_ASSERTS) || !defined(NDEBUG)
#define AURA_ASSERT_MSG(cond, msg) assert((cond) && (msg))
#else
#define AURA_ASSERT_MSG(cond, msg) ((void)0)
#endif

namespace aura {

enum class ErrorCode : std::int32_t {
    Ok = 0,
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    ParseError,
    UnsupportedFormat,
    UnsupportedVersion,
    AudioDeviceNotFound,
    AudioDeviceOpenFailed,
    AudioDeviceInUse,
    AudioDeviceLost,
    SampleRateNotSupported,
    BufferSizeNotSupported,
    PluginLoadFailed,
    PluginScanFailed,
    PluginCrashed,
    DiskFull,
    PermissionDenied,
    OutOfMemory,
    RealtimeViolation,
    ProjectCorrupt,
    ProjectVersionTooNew,
    RecordingFailed,
    Cancelled,
    NotImplemented,
};

/// Stable machine name of an error code (used in logs, JSON, tests).
const char* errorCodeName(ErrorCode code) noexcept;

/// Localization key for the user-facing message of a code.
/// The UI resolves it through aura::loc::Translator; when a key is missing the
/// plain-English text stored in AuraError::userMessage is used instead.
const char* errorCodeMessageKey(ErrorCode code) noexcept;

/// A structured, diagnosable error.
struct AuraError {
    ErrorCode code = ErrorCode::Unknown;
    /// Developer-facing description, safe for logs and diagnostics.
    std::string message;
    /// User-facing, plain language (also the fallback when no catalogue entry).
    std::string userMessage;
    /// Extra technical context (driver name, file, HRESULT, exception text).
    std::string detail;

    AuraError() = default;

    AuraError(ErrorCode c, std::string msg, std::string user = {}, std::string det = {})
        : code(c), message(std::move(msg)), userMessage(std::move(user)), detail(std::move(det)) {}

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }

    /// One-line diagnostic representation: "Code: message (detail)".
    [[nodiscard]] std::string toString() const;
};

/// Builds an error with the default user-facing message for the code.
AuraError makeError(ErrorCode code, std::string message, std::string detail = {});

// ---------------------------------------------------------------------------
// AuraResult
//
//   auto r = openDevice();          // AuraResult<DeviceHandle>
//   if (!r) { log(r.error().toString()); }
//   use(r.value());
//
// The `void` specialisation is declared before the primary template so that
// instantiating AuraResult<void> never reaches the variant-based definition.
// ---------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] AuraResult;

template <>
class [[nodiscard]] AuraResult<void> {
public:
    AuraResult() = default;
    AuraResult(AuraError error) : error_(std::move(error)), ok_(false) {}

    static AuraResult success() { return AuraResult{}; }

    [[nodiscard]] bool hasValue() const noexcept { return ok_; }
    [[nodiscard]] bool hasError() const noexcept { return !ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const AuraError& error() const& { return error_; }
    [[nodiscard]] AuraError& error() & { return error_; }

private:
    AuraError error_{};
    bool ok_ = true;
};

template <typename T>
class [[nodiscard]] AuraResult {
public:
    AuraResult(T value) : storage_(std::move(value)) {}
    AuraResult(AuraError error) : storage_(std::move(error)) {}

    /// Lets `return failure(err);` compile inside a function that returns
    /// AuraResult<T>, because failure(AuraError) yields a Status. Constructing
    /// from a *successful* Status is a programming error and trips the assert.
    AuraResult(const AuraResult<void>& status) : storage_(status.error()) {
        AURA_ASSERT_MSG(status.hasError(), "AuraResult<T> built from a successful Status");
    }

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool hasError() const noexcept { return storage_.index() == 1; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const AuraError& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] AuraError& error() & { return std::get<1>(storage_); }

    /// Value if present, otherwise the supplied fallback.
    [[nodiscard]] T valueOr(T fallback) const {
        return hasValue() ? std::get<0>(storage_) : std::move(fallback);
    }

    template <typename Fn>
    [[nodiscard]] auto map(Fn&& fn) const -> AuraResult<std::invoke_result_t<Fn, const T&>> {
        if (hasValue())
            return AuraResult<std::invoke_result_t<Fn, const T&>>(fn(std::get<0>(storage_)));
        return AuraResult<std::invoke_result_t<Fn, const T&>>(error());
    }

private:
    std::variant<T, AuraError> storage_;
};

using Status = AuraResult<void>;

/// Convenience constructors.
template <typename T>
inline AuraResult<std::decay_t<T>> success(T&& value) {
    return AuraResult<std::decay_t<T>>(std::forward<T>(value));
}
inline AuraResult<void> success() { return AuraResult<void>::success(); }
inline AuraResult<void> failure(AuraError e) { return AuraResult<void>(std::move(e)); }
template <typename T>
inline AuraResult<T> failureT(AuraError e) {
    return AuraResult<T>(std::move(e));
}
inline AuraResult<void> failure(ErrorCode code, std::string message, std::string detail = {}) {
    return failure(makeError(code, std::move(message), std::move(detail)));
}

/// Propagate an error from a nested Status (expression form, concise).
#define AURA_RETURN_ON_ERROR(expr)                                                                 \
    do {                                                                                           \
        ::aura::Status auraStatus_ = (expr);                                                       \
        if (!auraStatus_)                                                                          \
            return auraStatus_;                                                                    \
    } while (false)

/// Propagate an error from a nested Status.
#define AURA_TRY_STATUS(expr)                                                                      \
    do {                                                                                           \
        ::aura::Status auraTryStatus_ = (expr);                                                    \
        if (!auraTryStatus_)                                                                       \
            return ::aura::failure(auraTryStatus_.error());                                        \
    } while (false)

/// Propagate an error from a nested AuraResult<T>, binding the value.
#define AURA_TRY_VALUE(decl, expr)                                                                 \
    auto auraTry_##decl = (expr);                                                                  \
    if (!auraTry_##decl)                                                                           \
        return ::aura::failure(auraTry_##decl.error());                                            \
    auto& decl = auraTry_##decl.value()

} // namespace aura
