// ============================================================================
// AURA DAW - src/core/Errors.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Errors.hpp"

namespace aura {
namespace {

struct CodeInfo {
    ErrorCode code;
    const char* name;
    const char* key;
    const char* userText;
};

// Table drives logs, localization keys and the user-facing fallback text.
constexpr CodeInfo kCodeInfo[] = {
    {ErrorCode::Ok, "Ok", "err.ok", ""},
    {ErrorCode::Unknown, "Unknown", "err.unknown", "An unexpected problem occurred."},
    {ErrorCode::InvalidArgument, "InvalidArgument", "err.invalid_argument",
     "AURA received an invalid value for this operation."},
    {ErrorCode::NotFound, "NotFound", "err.not_found", "The requested item could not be found."},
    {ErrorCode::AlreadyExists, "AlreadyExists", "err.already_exists", "That item already exists."},
    {ErrorCode::IoError, "IoError", "err.io", "A file or disk operation failed."},
    {ErrorCode::ParseError, "ParseError", "err.parse", "A file could not be read because its contents are invalid."},
    {ErrorCode::UnsupportedFormat, "UnsupportedFormat", "err.unsupported_format",
     "This file format is not supported yet."},
    {ErrorCode::UnsupportedVersion, "UnsupportedVersion", "err.unsupported_version",
     "This file was created by a different version of AURA."},
    {ErrorCode::AudioDeviceNotFound, "AudioDeviceNotFound", "err.device_not_found",
     "The selected audio device is not available. It may have been disconnected."},
    {ErrorCode::AudioDeviceOpenFailed, "AudioDeviceOpenFailed", "err.device_open",
     "Could not start the selected audio device. The driver may be unavailable or already in use."},
    {ErrorCode::AudioDeviceInUse, "AudioDeviceInUse", "err.device_in_use",
     "The audio device is already in use by another application."},
    {ErrorCode::AudioDeviceLost, "AudioDeviceLost", "err.device_lost",
     "The audio device stopped responding and was reset. Playback has been paused."},
    {ErrorCode::SampleRateNotSupported, "SampleRateNotSupported", "err.sample_rate",
     "The audio device does not support the selected sample rate."},
    {ErrorCode::BufferSizeNotSupported, "BufferSizeNotSupported", "err.buffer_size",
     "The audio device does not support the selected buffer size."},
    {ErrorCode::PluginLoadFailed, "PluginLoadFailed", "err.plugin_load",
     "A plug-in could not be loaded. It has been disabled for this project."},
    {ErrorCode::PluginScanFailed, "PluginScanFailed", "err.plugin_scan",
     "A plug-in failed while being scanned. It has been added to the block list."},
    {ErrorCode::PluginCrashed, "PluginCrashed", "err.plugin_crash",
     "A plug-in stopped responding. It has been bypassed to protect your session."},
    {ErrorCode::DiskFull, "DiskFull", "err.disk_full",
     "There is not enough free space on the target drive."},
    {ErrorCode::PermissionDenied, "PermissionDenied", "err.permission",
     "AURA does not have permission to access this location."},
    {ErrorCode::OutOfMemory, "OutOfMemory", "err.memory", "Not enough memory is available."},
    {ErrorCode::RealtimeViolation, "RealtimeViolation", "err.realtime",
     "An internal real-time safety problem occurred. Details were written to the diagnostic log."},
    {ErrorCode::ProjectCorrupt, "ProjectCorrupt", "err.project_corrupt",
     "This project file is damaged and could not be opened. A backup may be available."},
    {ErrorCode::ProjectVersionTooNew, "ProjectVersionTooNew", "err.project_version",
     "This project was saved by a newer version of AURA. Please update AURA to open it."},
    {ErrorCode::RecordingFailed, "RecordingFailed", "err.recording",
     "Recording stopped because audio could not be written to disk. Already recorded audio was preserved."},
    {ErrorCode::Cancelled, "Cancelled", "err.cancelled", "The operation was cancelled."},
    {ErrorCode::NotImplemented, "NotImplemented", "err.not_implemented",
     "This feature is not available in this version of AURA."},
};

const CodeInfo* findInfo(ErrorCode code) noexcept {
    for (const auto& info : kCodeInfo) {
        if (info.code == code)
            return &info;
    }
    return nullptr;
}

} // namespace

const char* errorCodeName(ErrorCode code) noexcept {
    const CodeInfo* info = findInfo(code);
    return info ? info->name : "Unknown";
}

const char* errorCodeMessageKey(ErrorCode code) noexcept {
    const CodeInfo* info = findInfo(code);
    return info ? info->key : "err.unknown";
}

AuraError makeError(ErrorCode code, std::string message, std::string detail) {
    const CodeInfo* info = findInfo(code);
    return AuraError{code, std::move(message), info ? info->userText : std::string{}, std::move(detail)};
}

std::string AuraError::toString() const {
    std::string out = errorCodeName(code);
    out += ": ";
    out += message;
    if (!detail.empty()) {
        out += " [";
        out += detail;
        out += ']';
    }
    return out;
}

} // namespace aura
