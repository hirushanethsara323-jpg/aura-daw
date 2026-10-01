// ============================================================================
// AURA DAW - src/core/Localization.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/util/Localization.hpp"

#include <algorithm>

#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

namespace aura::loc {
namespace {

constexpr const char* kCategory = "Localization";

} // namespace

Translator::Translator() = default;

Translator& Translator::instance() {
    static Translator translator;
    return translator;
}

const Catalogue* Translator::find(const std::string& code) const {
    for (const auto& catalogue : catalogues_) {
        if (strings::equalsIgnoreCase(catalogue.languageCode, code))
            return &catalogue;
    }
    return nullptr;
}

void Translator::setCatalogue(Catalogue catalogue) {
    for (auto& existing : catalogues_) {
        if (strings::equalsIgnoreCase(existing.languageCode, catalogue.languageCode)) {
            existing = std::move(catalogue);
            return;
        }
    }
    catalogues_.push_back(std::move(catalogue));
}

Status Translator::loadFromFile(const std::string& path) {
    auto document = json::parseFile(path);
    if (!document)
        return failure(document.error());

    const json::Value& root = document.value();
    Catalogue catalogue;
    catalogue.languageCode = root["language"].asString("en");
    catalogue.displayName = root["displayName"].asString(catalogue.languageCode);
    catalogue.fallbackCode = root["fallback"].asString("");

    const json::Value& strings = root["strings"];
    if (strings.isObject()) {
        for (const auto& [key, value] : strings.members()) {
            if (value.isString())
                catalogue.entries[key] = value.asString();
        }
    }
    AURA_LOG_INFO(kCategory, "Loaded language pack '%s' (%zu strings) from %s",
                  catalogue.languageCode.c_str(), catalogue.entries.size(), path.c_str());
    setCatalogue(std::move(catalogue));
    return success();
}

Status Translator::setLanguage(const std::string& languageCode) {
    if (find(languageCode)) {
        activeLanguage_ = languageCode;
        return success();
    }
    const Catalogue* fallback = find(kFallbackLanguage);
    if (!fallback)
        return failure(makeError(ErrorCode::NotFound,
                                 "Language pack not available: " + languageCode));
    activeLanguage_ = kFallbackLanguage;
    return failure(makeError(ErrorCode::NotFound,
                             "Language pack not available, continuing in English: " + languageCode));
}

std::string Translator::translate(std::string_view key) const {
    const std::string keyString(key);
    if (const Catalogue* active = find(activeLanguage_)) {
        if (const auto it = active->entries.find(keyString); it != active->entries.end())
            return it->second;
        if (!active->fallbackCode.empty()) {
            if (const Catalogue* chain = find(active->fallbackCode)) {
                if (const auto it2 = chain->entries.find(keyString); it2 != chain->entries.end())
                    return it2->second;
            }
        }
    }
    if (const Catalogue* english = find(kFallbackLanguage)) {
        if (const auto it = english->entries.find(keyString); it != english->entries.end())
            return it->second;
    }
    // Last resort: return the key so the UI still renders something readable.
    if (std::find(missingKeys_.begin(), missingKeys_.end(), keyString) == missingKeys_.end())
        missingKeys_.push_back(keyString);
    return keyString;
}

std::string Translator::translate(std::string_view key,
                                  const std::vector<std::string>& arguments) const {
    std::string text = translate(key);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string placeholder = "{" + std::to_string(i) + "}";
        text = strings::replaceAll(text, placeholder, arguments[i]);
    }
    return text;
}

std::string Translator::translateError(const AuraError& error) const {
    const std::string key = errorCodeMessageKey(error.code);
    const std::string localized = translate(key);
    if (localized != key)
        return localized;
    if (!error.userMessage.empty())
        return error.userMessage;
    return translate("err.unknown");
}

std::vector<std::string> Translator::missingKeys() const {
    return missingKeys_;
}

void Translator::clearMissingKeyLog() {
    missingKeys_.clear();
}

std::vector<std::string> Translator::availableLanguages() const {
    std::vector<std::string> codes;
    codes.reserve(catalogues_.size());
    for (const auto& catalogue : catalogues_)
        codes.push_back(catalogue.languageCode);
    return codes;
}

std::string tr(std::string_view key) {
    return Translator::instance().translate(key);
}

std::string trf(std::string_view key, const std::vector<std::string>& arguments) {
    return Translator::instance().translate(key, arguments);
}

// ---------------------------------------------------------------------------
// Built-in English catalogue.
// Keys are grouped by surface. The same keys are what translators receive.
// ---------------------------------------------------------------------------
void installBuiltinEnglishCatalogue() {
    Catalogue english;
    english.languageCode = "en";
    english.displayName = "English";
    english.entries = {
        // --- Application -------------------------------------------------
        {"app.name", "AURA DAW"},
        {"app.fullName", "Advanced Unified Recording & Audio"},
        {"app.untitled", "Untitled Project"},
        {"app.ready", "Ready"},

        // --- Transport ---------------------------------------------------
        {"transport.play", "Play"},
        {"transport.stop", "Stop"},
        {"transport.pause", "Pause"},
        {"transport.record", "Record"},
        {"transport.loop", "Loop"},
        {"transport.returnToStart", "Return to Start"},
        {"transport.goToEnd", "Go to End"},
        {"transport.previousMarker", "Previous Marker"},
        {"transport.nextMarker", "Next Marker"},
        {"transport.metronome", "Metronome"},
        {"transport.countIn", "Count-in"},
        {"transport.state.stopped", "Stopped"},
        {"transport.state.playing", "Playing"},
        {"transport.state.paused", "Paused"},
        {"transport.state.recording", "Recording"},

        // --- Tracks ------------------------------------------------------
        {"track.audio", "Audio Track"},
        {"track.midi", "MIDI Track"},
        {"track.instrument", "Instrument Track"},
        {"track.bus", "Bus"},
        {"track.aux", "Aux Return"},
        {"track.master", "Master"},
        {"track.new", "New Track"},
        {"track.delete", "Delete Track"},
        {"track.duplicate", "Duplicate Track"},
        {"track.rename", "Rename Track"},
        {"track.arm", "Arm for Recording"},
        {"track.mute", "Mute"},
        {"track.solo", "Solo"},
        {"track.inputMonitor", "Input Monitoring"},
        {"track.height", "Track Height"},

        // --- Mixer -------------------------------------------------------
        {"mixer.volume", "Volume"},
        {"mixer.pan", "Pan"},
        {"mixer.gain", "Gain"},
        {"mixer.phase", "Invert Phase"},
        {"mixer.sends", "Sends"},
        {"mixer.inserts", "Inserts"},
        {"mixer.routing", "Routing"},
        {"mixer.meter", "Level Meter"},
        {"mixer.clip", "Clip"},
        {"mixer.master", "Master"},

        // --- Editing -----------------------------------------------------
        {"edit.split", "Split"},
        {"edit.trim", "Trim"},
        {"edit.move", "Move"},
        {"edit.duplicate", "Duplicate"},
        {"edit.delete", "Delete"},
        {"edit.slip", "Slip Edit"},
        {"edit.fadeIn", "Fade In"},
        {"edit.fadeOut", "Fade Out"},
        {"edit.crossfade", "Crossfade"},
        {"edit.consolidate", "Consolidate"},
        {"edit.normalize", "Normalize"},
        {"edit.reverse", "Reverse"},
        {"edit.undo", "Undo"},
        {"edit.redo", "Redo"},
        {"edit.cut", "Cut"},
        {"edit.copy", "Copy"},
        {"edit.paste", "Paste"},
        {"edit.selectAll", "Select All"},

        // --- Timeline ----------------------------------------------------
        {"timeline.barsBeats", "Bars & Beats"},
        {"timeline.seconds", "Seconds"},
        {"timeline.timecode", "Timecode"},
        {"timeline.samples", "Samples"},
        {"timeline.snap", "Snap"},
        {"timeline.snap.grid", "Snap to Grid"},
        {"timeline.snap.bar", "Snap to Bar"},
        {"timeline.snap.beat", "Snap to Beat"},
        {"timeline.snap.clip", "Snap to Clip"},
        {"timeline.snap.marker", "Snap to Marker"},
        {"timeline.snap.zeroCrossing", "Snap to Zero Crossing"},
        {"timeline.zoomIn", "Zoom In"},
        {"timeline.zoomOut", "Zoom Out"},
        {"timeline.zoomToFit", "Zoom to Fit"},
        {"timeline.addMarker", "Add Marker"},
        {"timeline.loopRange", "Loop Range"},

        // --- Plug-ins ----------------------------------------------------
        {"plugin.browser", "Plug-in Browser"},
        {"plugin.scanning", "Scanning plug-ins..."},
        {"plugin.scanComplete", "Plug-in scan complete: {0} found, {1} failed"},
        {"plugin.instruments", "Instruments"},
        {"plugin.effects", "Effects"},
        {"plugin.dynamics", "Dynamics"},
        {"plugin.eq", "EQ"},
        {"plugin.reverb", "Reverb"},
        {"plugin.delay", "Delay"},
        {"plugin.utility", "Utility"},
        {"plugin.favorites", "Favorites"},
        {"plugin.recent", "Recently Used"},
        {"plugin.bypass", "Bypass"},
        {"plugin.editor", "Plug-in Editor"},
        {"plugin.blocklisted", "This plug-in is blocked because it failed to load."},

        // --- Project -----------------------------------------------------
        {"project.new", "New Project"},
        {"project.open", "Open Project"},
        {"project.save", "Save Project"},
        {"project.saveAs", "Save Project As"},
        {"project.close", "Close Project"},
        {"project.recent", "Recent Projects"},
        {"project.settings", "Project Settings"},
        {"project.saved", "Project saved"},
        {"project.unsavedChanges", "This project has unsaved changes."},
        {"project.autosaved", "Autosave"},
        {"project.recoveryFound", "AURA found work from a session that did not close normally."},
        {"project.recoveryRestore", "Recover Session"},
        {"project.recoveryDiscard", "Discard"},

        // --- Export ------------------------------------------------------
        {"export.title", "Export Audio"},
        {"export.start", "Export"},
        {"export.range", "Range"},
        {"export.entireProject", "Entire Project"},
        {"export.selection", "Selection"},
        {"export.stems", "Stems"},
        {"export.sampleRate", "Sample rate"},
        {"export.bitDepth", "Bit depth"},
        {"export.channels", "Channels"},
        {"export.normalize", "Normalize"},
        {"export.dither", "Dither"},
        {"export.progress", "Exporting {0}%"},
        {"export.complete", "Export complete: {0}"},
        {"export.cancelled", "Export cancelled"},

        // --- Settings ----------------------------------------------------
        {"settings.title", "Preferences"},
        {"settings.audio", "Audio"},
        {"settings.midi", "MIDI"},
        {"settings.plugins", "Plug-ins"},
        {"settings.general", "General"},
        {"settings.audio.driver", "Driver"},
        {"settings.audio.inputDevice", "Input device"},
        {"settings.audio.outputDevice", "Output device"},
        {"settings.audio.sampleRate", "Sample rate"},
        {"settings.audio.bufferSize", "Buffer size"},
        {"settings.audio.latency", "Latency"},
        {"settings.audio.activeChannels", "Active inputs / outputs"},
        {"settings.general.theme", "Theme"},
        {"settings.general.language", "Language"},
        {"settings.general.autosave", "Autosave interval"},
        {"settings.general.projectFolder", "Default project folder"},
        {"settings.general.recordingFolder", "Recording folder"},
        {"settings.plugins.folders", "Plug-in folders"},
        {"settings.plugins.rescan", "Rescan plug-ins"},

        // --- Errors (must mirror errorCodeMessageKey) ---------------------
        {"err.ok", ""},
        {"err.unknown", "An unexpected problem occurred."},
        {"err.invalid_argument", "AURA received an invalid value for this operation."},
        {"err.not_found", "The requested item could not be found."},
        {"err.already_exists", "That item already exists."},
        {"err.io", "A file or disk operation failed."},
        {"err.parse", "A file could not be read because its contents are invalid."},
        {"err.unsupported_format", "This file format is not supported yet."},
        {"err.unsupported_version", "This file was created by a different version of AURA."},
        {"err.device_not_found",
         "The selected audio device is not available. It may have been disconnected."},
        {"err.device_open",
         "Could not start the selected audio device. The driver may be unavailable or already in use."},
        {"err.device_in_use", "The audio device is already in use by another application."},
        {"err.device_lost",
         "The audio device stopped responding and was reset. Playback has been paused."},
        {"err.sample_rate", "The audio device does not support the selected sample rate."},
        {"err.buffer_size", "The audio device does not support the selected buffer size."},
        {"err.plugin_load", "A plug-in could not be loaded. It has been disabled for this project."},
        {"err.plugin_scan", "A plug-in failed while being scanned. It has been added to the block list."},
        {"err.plugin_crash",
         "A plug-in stopped responding. It has been bypassed to protect your session."},
        {"err.disk_full", "There is not enough free space on the target drive."},
        {"err.permission", "AURA does not have permission to access this location."},
        {"err.memory", "Not enough memory is available."},
        {"err.realtime",
         "An internal real-time safety problem occurred. Details were written to the diagnostic log."},
        {"err.project_corrupt",
         "This project file is damaged and could not be opened. A backup may be available."},
        {"err.project_version",
         "This project was saved by a newer version of AURA. Please update AURA to open it."},
        {"err.recording",
         "Recording stopped because audio could not be written to disk. Already recorded audio was preserved."},
        {"err.cancelled", "The operation was cancelled."},
        {"err.not_implemented", "This feature is not available in this version of AURA."},
    };
    Translator::instance().setCatalogue(std::move(english));
}

} // namespace aura::loc
