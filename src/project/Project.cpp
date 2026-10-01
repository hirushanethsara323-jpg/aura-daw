// ============================================================================
// AURA DAW - src/project/Project.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Serialization for the .aura document. See Project.hpp for the format
// rationale. Everything here is control-thread code.
// ============================================================================
#include "aura/project/Project.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"
#include "aura/core/Version.hpp"
#include "aura/media/AudioFile.hpp"

namespace aura::project {
namespace {
constexpr const char* kCategory = "Project";

std::string nowIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buffer);
}

json::Value serializeFade(const audio::Fade& fade) {
    json::Value value = json::Value::makeObject();
    value.set("length", static_cast<std::int64_t>(fade.length));
    value.set("shape", static_cast<int>(fade.shape));
    return value;
}

audio::Fade deserializeFade(const json::Value& value) {
    audio::Fade fade;
    fade.length = static_cast<time::SamplePos>(value["length"].asInt64());
    fade.shape = static_cast<audio::FadeShape>(value["shape"].asInt(0));
    return fade;
}

} // namespace

// ---------------------------------------------------------------------------
// MediaPool
// ---------------------------------------------------------------------------
std::uint64_t MediaPool::addFile(const std::string& path, bool isRecording) {
    // De-duplicate by portable path so the same file imported twice stays one
    // media item (and one waveform cache).
    const std::string portable = filesystem::toPortablePath(filesystem::fs::path(path));
    for (const auto& item : items_) {
        if (item.path == portable)
            return item.id;
    }

    MediaItem item;
    item.id = nextId_++;
    item.path = portable;
    item.originalPath = path;
    item.isRecording = isRecording;
    item.fileSize = static_cast<std::int64_t>(filesystem::fileSize(path));
    if (auto info = media::probeFile(path)) {
        item.frames = info.value().lengthFrames;
        item.sampleRate = info.value().sampleRate;
        item.numChannels = info.value().numChannels;
        item.bitsPerSample = info.value().bitsPerSample;
        item.missing = false;
    } else {
        item.missing = true;
    }
    items_.push_back(item);
    return item.id;
}

bool MediaPool::removeItem(std::uint64_t id) {
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [id](const MediaItem& item) { return item.id == id; });
    if (it == items_.end())
        return false;
    items_.erase(it);
    return true;
}

MediaItem* MediaPool::find(std::uint64_t id) noexcept {
    for (auto& item : items_) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

const MediaItem* MediaPool::find(std::uint64_t id) const noexcept {
    return const_cast<MediaPool*>(this)->find(id);
}

void MediaPool::resolveAll(const std::string& projectDirectory) {
    const auto base = filesystem::fs::path(projectDirectory);
    for (auto& item : items_) {
        auto resolved = filesystem::fs::path(item.path);
        if (resolved.is_relative())
            resolved = base / resolved;
        if (!filesystem::isRegularFile(resolved) && !item.originalPath.empty()) {
            // Try the original absolute path before declaring it missing: for
            // moved projects this usually finds the file.
            const auto original = filesystem::fs::path(item.originalPath);
            if (filesystem::isRegularFile(original))
                resolved = original;
        }
        item.missing = !filesystem::isRegularFile(resolved);
        if (!item.missing && !item.originalPath.empty())
            item.originalPath = resolved.string();
    }
}

Status MediaPool::relink(std::uint64_t id, const std::string& newPath) {
    MediaItem* item = find(id);
    if (!item)
        return failure(makeError(ErrorCode::NotFound, "Media item not found"));
    if (!filesystem::isRegularFile(newPath))
        return failure(makeError(ErrorCode::NotFound, "File does not exist: " + newPath));
    item->path = filesystem::toPortablePath(filesystem::fs::path(newPath));
    item->originalPath = newPath;
    item->missing = false;
    if (auto info = media::probeFile(newPath)) {
        item->frames = info.value().lengthFrames;
        item->sampleRate = info.value().sampleRate;
        item->numChannels = info.value().numChannels;
    }
    return success();
}

std::size_t MediaPool::removeUnused(const std::vector<std::string>& referencedPaths) {
    const std::size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&referencedPaths](const MediaItem& item) {
                                    return std::find(referencedPaths.begin(),
                                                     referencedPaths.end(),
                                                     item.path) == referencedPaths.end();
                                }),
                 items_.end());
    return before - items_.size();
}

std::vector<const MediaItem*> MediaPool::missingItems() const {
    std::vector<const MediaItem*> missing;
    for (const auto& item : items_) {
        if (item.missing)
            missing.push_back(&item);
    }
    return missing;
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
Project::Project() : mixer_(std::make_unique<audio::Mixer>()) {
    metadata_.createdAtIso = nowIso();
    metadata_.modifiedAtIso = metadata_.createdAtIso;
}

Project::~Project() = default;

std::unique_ptr<Project> Project::createEmpty(const std::string& name) {
    auto project = std::make_unique<Project>();
    project->metadata_.name = name.empty() ? "Untitled" : name;
    project->tempoMap_.setDefaultTempo(120.0);
    project->tempoMap_.setDefaultTimeSignature({4, 4});
    // A new project starts with one audio track and a master strip, which is
    // what a user expects the moment the window appears.
    project->mixer_->createTrack(audio::TrackKind::Audio, "Audio 1");
    project->mixer_->prepare(48000.0, 512, 2);
    return project;
}

audio::Clip* Project::addClip(audio::Clip clip) {
    clips_.push_back(std::make_unique<audio::Clip>(std::move(clip)));
    modified_ = true;
    return clips_.back().get();
}

midi::MidiClip* Project::addMidiClip(midi::MidiClip clip) {
    midiClips_.push_back(std::make_unique<midi::MidiClip>(std::move(clip)));
    modified_ = true;
    return midiClips_.back().get();
}

bool Project::removeClip(audio::ClipId id) {
    const auto it = std::find_if(clips_.begin(), clips_.end(),
                                 [id](const std::unique_ptr<audio::Clip>& clip) {
                                     return clip && clip->id() == id;
                                 });
    if (it == clips_.end())
        return false;
    clips_.erase(it);
    modified_ = true;
    return true;
}

audio::Clip* Project::findClip(audio::ClipId id) noexcept {
    for (auto& clip : clips_) {
        if (clip && clip->id() == id)
            return clip.get();
    }
    return nullptr;
}

midi::MidiClip* Project::findMidiClip(std::uint64_t id) noexcept {
    for (auto& clip : midiClips_) {
        if (clip && clip->id() == id)
            return clip.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
time::SamplePos Project::arrangementEndSamples(double sampleRate) const noexcept {
    time::SamplePos end = 0;
    for (const auto& clip : clips_) {
        if (clip)
            end = std::max(end, clip->end());
    }
    for (const auto& clip : midiClips_) {
        if (!clip)
            continue;
        // MIDI clips are authoritative in ticks, so convert through the tempo map
        // rather than assuming a constant tempo.
        end = std::max(end, tempoMap_.ticksToSamples(clip->endTicks(), sampleRate));
    }
    for (const auto& marker : markers_) {
        if (marker.positionTicks <= 0)
            continue;
        end = std::max(end, tempoMap_.ticksToSamples(marker.positionTicks, sampleRate));
    }
    return end;
}

std::string Project::serializeDocument() const {
    json::Value root = json::Value::makeObject();
    root.set("format", "aura-project");
    root.set("versionMajor", metadata_.versionMajor);
    root.set("versionMinor", metadata_.versionMinor);
    root.set("application", "AURA DAW " AURA_VERSION_STRING);

    json::Value meta = json::Value::makeObject();
    meta.set("name", metadata_.name);
    meta.set("artist", metadata_.artist);
    meta.set("comment", metadata_.comment);
    meta.set("createdAt", metadata_.createdAtIso);
    meta.set("modifiedAt", metadata_.modifiedAtIso);
    meta.set("sampleRate", sampleRate_);
    meta.set("sessionLengthSamples", static_cast<std::int64_t>(metadata_.sessionLengthSamples));
    root.set("metadata", std::move(meta));

    // --- tempo map -----------------------------------------------------------
    json::Value tempo = json::Value::makeObject();
    tempo.set("defaultBpm", tempoMap_.tempoAt(0));
    const auto& signatures = tempoMap_.timeSignatures();
    tempo.set("defaultTimeSignature",
              time::timeSignatureToString(tempoMap_.signatureAt(0)));
    json::Value tempoEvents = json::Value::makeArray();
    for (const auto& event : tempoMap_.tempos()) {
        json::Value entry = json::Value::makeObject();
        entry.set("positionTicks", event.positionTicks);
        entry.set("bpm", event.bpm);
        tempoEvents.push(std::move(entry));
    }
    tempo.set("events", std::move(tempoEvents));
    json::Value signatureEvents = json::Value::makeArray();
    for (const auto& event : signatures) {
        json::Value entry = json::Value::makeObject();
        entry.set("positionTicks", event.positionTicks);
        entry.set("signature", time::timeSignatureToString(event.signature));
        signatureEvents.push(std::move(entry));
    }
    tempo.set("signatureEvents", std::move(signatureEvents));
    root.set("tempoMap", std::move(tempo));

    // --- markers -------------------------------------------------------------
    json::Value markers = json::Value::makeArray();
    for (const auto& marker : markers_) {
        json::Value entry = json::Value::makeObject();
        entry.set("positionTicks", marker.positionTicks);
        entry.set("name", marker.name);
        entry.set("colour", static_cast<std::int64_t>(marker.colour));
        entry.set("section", marker.isArrangementSection);
        markers.push(std::move(entry));
    }
    root.set("markers", std::move(markers));

    // --- tracks --------------------------------------------------------------
    json::Value tracks = json::Value::makeArray();
    for (audio::Track* track : mixer_->allTracksIncludingMaster()) {
        if (!track)
            continue;
        json::Value entry = json::Value::makeObject();
        entry.set("id", track->id());
        entry.set("kind", static_cast<int>(track->kind()));
        entry.set("name", track->name());
        entry.set("colour", static_cast<std::int64_t>(track->colour()));
        entry.set("height", track->height());
        entry.set("icon", track->icon());

        json::Value mixer = json::Value::makeObject();
        mixer.set("volume", track->volume());
        mixer.set("pan", track->pan());
        mixer.set("mute", track->isMuted());
        mixer.set("solo", track->isSoloed());
        mixer.set("phaseInverted", track->isPhaseInverted());
        mixer.set("recordArmed", track->isRecordArmed());
        mixer.set("inputMonitoring", track->isInputMonitoring());
        mixer.set("automationMode", static_cast<int>(track->automationMode()));
        entry.set("mixer", std::move(mixer));

        json::Value inserts = json::Value::makeArray();
        for (const auto& insert : track->inserts()) {
            json::Value slot = json::Value::makeObject();
            slot.set("name", insert.name);
            slot.set("bypassed", insert.bypassed);
            slot.set("mix", insert.mix);
            // Plug-in state is stored separately and referenced by id; built-in
            // processors are described by name + parameters.
            if (!insert.pluginId.empty())
                slot.set("pluginId", insert.pluginId);
            inserts.push(std::move(slot));
        }
        entry.set("inserts", std::move(inserts));

        json::Value sends = json::Value::makeArray();
        for (const auto& send : track->sends()) {
            json::Value value = json::Value::makeObject();
            value.set("destination", send.destination);
            value.set("level", send.level);
            value.set("preFader", send.preFader);
            value.set("enabled", send.enabled);
            sends.push(std::move(value));
        }
        entry.set("sends", std::move(sends));

        json::Value routing = json::Value::makeObject();
        routing.set("destination", track->routing().destination);
        routing.set("enabled", track->routing().enabled);
        entry.set("routing", std::move(routing));

        json::Value clipIds = json::Value::makeArray();
        for (const std::uint64_t clipId : track->clips())
            clipIds.push(clipId);
        entry.set("clips", std::move(clipIds));
        tracks.push(std::move(entry));
    }
    root.set("tracks", std::move(tracks));

    // --- clips ---------------------------------------------------------------
    json::Value clips = json::Value::makeArray();
    for (const auto& clip : clips_) {
        if (!clip)
            continue;
        json::Value entry = json::Value::makeObject();
        entry.set("id", clip->id());
        entry.set("name", clip->name());
        entry.set("source", clip->sourceFile());
        entry.set("start", static_cast<std::int64_t>(clip->start()));
        entry.set("length", static_cast<std::int64_t>(clip->length()));
        entry.set("sourceOffset", static_cast<std::int64_t>(clip->sourceOffset()));
        entry.set("gain", clip->gain());
        entry.set("muted", clip->isMuted());
        entry.set("locked", clip->isLocked());
        entry.set("loop", clip->isLoopEnabled());
        entry.set("reversed", clip->isReversed());
        entry.set("stretchRatio", clip->stretchRatio());
        entry.set("pitchSemitones", clip->pitchSemitones());
        entry.set("colour", static_cast<std::int64_t>(clip->colour()));
        entry.set("fadeIn", serializeFade(clip->fadeIn()));
        entry.set("fadeOut", serializeFade(clip->fadeOut()));
        // stretchRatio/pitch are written even though the DSP is not implemented
        // yet: the values must survive a save/load round trip so clips are not
        // silently "fixed" when the feature lands.
        clips.push(std::move(entry));
    }
    root.set("clips", std::move(clips));

    // --- MIDI clips ----------------------------------------------------------
    json::Value midiClips = json::Value::makeArray();
    for (const auto& clip : midiClips_) {
        if (!clip)
            continue;
        json::Value entry = json::Value::makeObject();
        entry.set("id", clip->id());
        entry.set("name", clip->name());
        entry.set("startTicks", clip->startTicks());
        entry.set("lengthTicks", clip->lengthTicks());
        entry.set("loop", clip->isLoopEnabled());
        json::Value notes = json::Value::makeArray();
        for (const auto& note : clip->notes()) {
            json::Value value = json::Value::makeObject();
            value.set("pitch", note.pitch);
            value.set("velocity", note.velocity);
            value.set("start", note.startTicks);
            value.set("length", note.lengthTicks);
            value.set("channel", note.channel);
            notes.push(std::move(value));
        }
        entry.set("notes", std::move(notes));
        json::Value controllers = json::Value::makeArray();
        for (const auto& controller : clip->controllers()) {
            json::Value value = json::Value::makeObject();
            value.set("position", controller.positionTicks);
            value.set("controller", controller.controller);
            value.set("value", controller.value);
            controllers.push(std::move(value));
        }
        entry.set("controllers", std::move(controllers));
        midiClips.push(std::move(entry));
    }
    root.set("midiClips", std::move(midiClips));

    // --- media pool ----------------------------------------------------------
    json::Value media = json::Value::makeArray();
    for (const auto& item : mediaPool_.items()) {
        json::Value entry = json::Value::makeObject();
        entry.set("id", item.id);
        entry.set("path", item.path);
        entry.set("originalPath", item.originalPath);
        entry.set("frames", static_cast<std::int64_t>(item.frames));
        entry.set("sampleRate", item.sampleRate);
        entry.set("channels", item.numChannels);
        entry.set("bits", item.bitsPerSample);
        entry.set("recording", item.isRecording);
        entry.set("fileSize", static_cast<std::int64_t>(item.fileSize));
        entry.set("peak", item.peakAmplitude);
        media.push(std::move(entry));
    }
    root.set("media", std::move(media));

    return root.dump(true);
}

Status Project::beginSession() {
    const std::string marker = crashMarkerPath();
    const std::string contents = nowIso();
    return filesystem::writeTextFileAtomic(marker, contents);
}

Status Project::endSession() {
    const std::string marker = crashMarkerPath();
    if (!filesystem::exists(marker))
        return success();
    return filesystem::removeFile(marker);
}

std::string Project::crashMarkerPath() const {
    if (directory_.empty())
        return {};
    return (filesystem::fs::path(directory_) / "session.lock").string();
}

Status Project::save() {
    if (directory_.empty())
        return failure(makeError(ErrorCode::InvalidArgument, "Project has no directory (use Save As)"));
    const auto dir = filesystem::fs::path(directory_);
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(dir));
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(dir / "media"));
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(dir / "peaks"));
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(dir / "plugin-states"));

    metadata_.modifiedAtIso = nowIso();

    // Keep one backup of the previous document: a corrupt save must never cost
    // the user their session.
    const auto document = dir / kDocumentFileName;
    if (filesystem::exists(document)) {
        const auto backup = dir / "project.json.bak";
        (void)filesystem::copyFile(document, backup, true);
    }

    const std::string text = serializeDocument();
    AURA_RETURN_ON_ERROR(filesystem::writeTextFileAtomic(document, text));

    documentPath_ = document.string();
    markSaved();
    AURA_LOG_INFO(kCategory, "Saved project '%s' (%zu tracks, %zu clips, %zu media items)",
                  metadata_.name.c_str(), mixer_->trackCount(), clips_.size(),
                  mediaPool_.items().size());
    return success();
}

Status Project::saveAs(const std::string& directory, std::string* outActualPath) {
    const auto requested = filesystem::fs::path(directory);
    // Never silently overwrite: if the target already contains a document, make
    // a unique sibling instead.
    auto target = requested;
    if (filesystem::exists(requested / kDocumentFileName)) {
        target = filesystem::makeUniquePath(requested);
        AURA_LOG_WARN(kCategory, "Target already contains a project; saving to '%s' instead",
                      target.string().c_str());
    }
    directory_ = target.string();
    documentPath_ = (target / kDocumentFileName).string();
    if (outActualPath)
        *outActualPath = directory_;
    return save();
}

AuraResult<std::unique_ptr<Project>> Project::load(const std::string& directory) {
    const auto dir = filesystem::fs::path(directory);
    const auto document = dir / kDocumentFileName;
    if (!filesystem::isRegularFile(document))
        return failure(makeError(ErrorCode::NotFound, "No project.json in " + directory));

    auto text = filesystem::readTextFile(document);
    if (!text)
        return failure(text.error());

    auto result = deserializeDocument(dir.string(), text.value());
    if (!result) {
        // Fall back to the backup before giving up: this is the difference
        // between "your session is gone" and "AURA recovered your session".
        const auto backup = dir / "project.json.bak";
        if (filesystem::isRegularFile(backup)) {
            if (auto backupText = filesystem::readTextFile(backup)) {
                AURA_LOG_WARN(kCategory, "project.json failed to parse; recovered from backup");
                return deserializeDocument(dir.string(), backupText.value());
            }
        }
        return failure(result.error());
    }
    return result;
}

AuraResult<std::unique_ptr<Project>> Project::deserializeDocument(const std::string& directory,
                                                                 const std::string& jsonText) {
    auto parsed = json::parse(jsonText);
    if (!parsed)
        return failure(parsed.error());

    const json::Value& root = parsed.value();
    const int major = root["versionMajor"].asInt(0);
    if (major > kFormatVersionMajor) {
        return failure(makeError(ErrorCode::ProjectVersionTooNew,
                                 "Project format version " + std::to_string(major) +
                                     " is newer than this build supports",
                                 directory));
    }

    auto project = std::make_unique<Project>();
    project->directory_ = directory;
    project->documentPath_ = (filesystem::fs::path(directory) / kDocumentFileName).string();
    project->metadata_.name = root["metadata"]["name"].asString("Untitled");
    project->metadata_.artist = root["metadata"]["artist"].asString("");
    project->metadata_.comment = root["metadata"]["comment"].asString("");
    project->metadata_.createdAtIso = root["metadata"]["createdAt"].asString("");
    project->metadata_.modifiedAtIso = root["metadata"]["modifiedAt"].asString("");
    project->metadata_.versionMajor = root["versionMajor"].asInt(kFormatVersionMajor);
    project->metadata_.versionMinor = root["versionMinor"].asInt(0);
    project->sampleRate_ = root["metadata"]["sampleRate"].asDouble(48000.0);
    project->metadata_.sessionLengthSamples =
        root["metadata"]["sessionLengthSamples"].asInt64();

    // --- tempo map -----------------------------------------------------------
    const json::Value& tempo = root["tempoMap"];
    project->tempoMap_.clear();
    project->tempoMap_.setDefaultTempo(tempo["defaultBpm"].asDouble(120.0));
    time::TimeSignature signature{4, 4};
    if (!time::timeSignatureFromString(tempo["defaultTimeSignature"].asString("4/4"), signature)) {
        // Repair rather than refuse: the rest of the document is still valid and
        // the default is documented in PROJECT_FORMAT.md. The user is told.
        AURA_LOG_WARN(kCategory, "Unreadable default time signature in project.json; using 4/4");
        signature = time::TimeSignature{4, 4};
    }
    project->tempoMap_.setDefaultTimeSignature(signature);
    for (const auto& event : tempo["events"].elements()) {
        project->tempoMap_.addTempo(event["positionTicks"].asInt64(), event["bpm"].asDouble(120.0));
    }
    for (const auto& event : tempo["signatureEvents"].elements()) {
        time::TimeSignature eventSignature{4, 4};
        if (!time::timeSignatureFromString(event["signature"].asString("4/4"), eventSignature)) {
            AURA_LOG_WARN(kCategory, "Dropped a time-signature event with an unreadable value");
            continue;
        }
        project->tempoMap_.addTimeSignature(event["positionTicks"].asInt64(), eventSignature);
    }

    // --- markers -------------------------------------------------------------
    for (const auto& entry : root["markers"].elements()) {
        Marker marker;
        marker.positionTicks = entry["positionTicks"].asInt64();
        marker.name = entry["name"].asString();
        marker.colour = static_cast<std::uint32_t>(entry["colour"].asInt64(0xFFFFB020u));
        marker.isArrangementSection = entry["section"].asBool(false);
        project->markers_.push_back(std::move(marker));
    }

    // --- tracks --------------------------------------------------------------
    int droppedInserts = 0;
    for (const auto& entry : root["tracks"].elements()) {
        const auto kind = static_cast<audio::TrackKind>(entry["kind"].asInt(0));
        const audio::TrackId id = static_cast<audio::TrackId>(entry["id"].asInt(0));
        if (kind == audio::TrackKind::Master) {
            audio::Track& master = project->mixer_->master();
            master.setName(entry["name"].asString("Master"));
            master.setVolume(entry["mixer"]["volume"].asFloat(1.0f));
            master.setPan(entry["mixer"]["pan"].asFloat(0.0f));
            master.setMuted(entry["mixer"]["mute"].asBool(false));
            continue;
        }
        const audio::TrackId created =
            project->mixer_->createTrack(kind, entry["name"].asString("Track"));
        if (audio::Track* track = project->mixer_->findTrack(created)) {
            track->setColour(static_cast<std::uint32_t>(entry["colour"].asInt64(0xFF3B82F6u)));
            track->setHeight(entry["height"].asInt(74));
            track->setIcon(entry["icon"].asString(""));
            track->setVolume(entry["mixer"]["volume"].asFloat(1.0f));
            track->setPan(entry["mixer"]["pan"].asFloat(0.0f));
            track->setMuted(entry["mixer"]["mute"].asBool(false));
            track->setSoloed(entry["mixer"]["solo"].asBool(false));
            track->setPhaseInverted(entry["mixer"]["phaseInverted"].asBool(false));
            track->setRecordArmed(entry["mixer"]["recordArmed"].asBool(false));
            track->setInputMonitoring(entry["mixer"]["inputMonitoring"].asBool(false));
            track->setAutomationMode(
                static_cast<audio::AutomationMode>(entry["mixer"]["automationMode"].asInt(1)));
            for (const auto& slot : entry["inserts"].elements()) {
                audio::InsertSlot insert;
                insert.name = slot["name"].asString();
                insert.bypassed = slot["bypassed"].asBool(false);
                insert.mix = slot["mix"].asFloat(1.0f);
                insert.pluginId = slot["pluginId"].asString("");
                // false means the chain is already at kMaxInserts (addInsert logs
                // the track and the limit itself); the rest of the project still
                // loads, and the total is reported once below.
                if (!track->addInsert(std::move(insert)))
                    ++droppedInserts;
            }
            for (const auto& send : entry["sends"].elements()) {
                audio::Send value;
                value.destination = static_cast<audio::TrackId>(send["destination"].asInt(0));
                value.level = send["level"].asFloat(0.0f);
                value.preFader = send["preFader"].asBool(false);
                value.enabled = send["enabled"].asBool(true);
                track->addSend(value);
            }
            track->setOutput(static_cast<audio::TrackId>(entry["routing"]["destination"].asInt(0)));
            for (const auto& clipId : entry["clips"].elements())
                track->clips().push_back(static_cast<std::uint64_t>(clipId.asInt64()));
            (void)id;
        }
    }
    if (droppedInserts > 0) {
        AURA_LOG_WARN(kCategory, "Dropped %d insert(s) while loading (limit is %zu per track)",
                      droppedInserts, audio::kMaxInserts);
    }
    (void)project->mixer_->resolveSoloState();

    // --- clips ---------------------------------------------------------------
    for (const auto& entry : root["clips"].elements()) {
        audio::Clip clip(static_cast<audio::ClipId>(entry["id"].asInt64()), audio::ClipType::Audio,
                         entry["name"].asString("Clip"));
        clip.setSourceFile(entry["source"].asString());
        clip.setStart(entry["start"].asInt64());
        clip.setLength(entry["length"].asInt64(48000));
        clip.setSourceOffset(entry["sourceOffset"].asInt64());
        clip.setGain(entry["gain"].asFloat(1.0f));
        clip.setMuted(entry["muted"].asBool(false));
        clip.setLocked(entry["locked"].asBool(false));
        clip.setLoopEnabled(entry["loop"].asBool(false));
        clip.setReversed(entry["reversed"].asBool(false));
        clip.setStretchRatio(entry["stretchRatio"].asFloat(1.0f));
        clip.setPitchSemitones(entry["pitchSemitones"].asFloat(0.0f));
        clip.setColour(static_cast<std::uint32_t>(entry["colour"].asInt64(0xFF2563EBu)));
        clip.setFadeIn(deserializeFade(entry["fadeIn"]));
        clip.setFadeOut(deserializeFade(entry["fadeOut"]));
        project->nextClipId_ = std::max(project->nextClipId_, clip.id() + 1);
        project->clips_.push_back(std::make_unique<audio::Clip>(std::move(clip)));
    }

    // --- MIDI clips ----------------------------------------------------------
    for (const auto& entry : root["midiClips"].elements()) {
        midi::MidiClip clip(static_cast<std::uint64_t>(entry["id"].asInt64()),
                            entry["name"].asString("MIDI Clip"));
        clip.setStartTicks(entry["startTicks"].asInt64());
        clip.setLengthTicks(entry["lengthTicks"].asInt64(time::kTicksPerBar4_4));
        clip.setLoopEnabled(entry["loop"].asBool(false));
        for (const auto& note : entry["notes"].elements()) {
            midi::Note value;
            value.pitch = note["pitch"].asInt(60);
            value.velocity = note["velocity"].asInt(100);
            value.startTicks = note["start"].asInt64();
            value.lengthTicks = note["length"].asInt64(240);
            value.channel = note["channel"].asInt(0);
            clip.addNote(value);
        }
        for (const auto& controller : entry["controllers"].elements()) {
            midi::ControllerEvent value;
            value.positionTicks = controller["position"].asInt64();
            value.controller = controller["controller"].asInt(1);
            value.value = controller["value"].asInt(0);
            clip.controllers().push_back(value);
        }
        project->nextMidiClipId_ = std::max(project->nextMidiClipId_, clip.id() + 1);
        project->midiClips_.push_back(std::make_unique<midi::MidiClip>(std::move(clip)));
    }

    // --- media pool ----------------------------------------------------------
    for (const auto& entry : root["media"].elements()) {
        MediaItem item;
        item.id = static_cast<std::uint64_t>(entry["id"].asInt64());
        item.path = entry["path"].asString();
        item.originalPath = entry["originalPath"].asString();
        item.frames = entry["frames"].asInt64();
        item.sampleRate = static_cast<std::uint32_t>(entry["sampleRate"].asInt(48000));
        item.numChannels = static_cast<std::uint16_t>(entry["channels"].asInt(2));
        item.bitsPerSample = static_cast<std::uint16_t>(entry["bits"].asInt(24));
        item.isRecording = entry["recording"].asBool(false);
        item.fileSize = entry["fileSize"].asInt64();
        item.peakAmplitude = entry["peak"].asFloat(-1.0f);
        project->mediaPool_.items().push_back(item);
        project->mediaPool_.ensureNextIdAbove(item.id);
    }
    project->mediaPool_.resolveAll(directory);
    project->markSaved();

    AURA_LOG_INFO(kCategory, "Loaded project '%s' from %s (%zu tracks, %zu clips)",
                  project->metadata_.name.c_str(), directory.c_str(),
                  project->mixer_->trackCount(), project->clips_.size());
    return success(std::move(project));
}

// ---------------------------------------------------------------------------
// Autosave
// ---------------------------------------------------------------------------
Autosave::Autosave(Project* project) : project_(project) {}
Autosave::~Autosave() {
    stop();
}

void Autosave::setIntervalSeconds(double seconds) {
    intervalSeconds_ = math::clamp(seconds, 5.0, 3600.0);
}

void Autosave::start() {
    if (running_.exchange(true))
        return;
    worker_ = std::thread([this] { workerLoop(); });
}

void Autosave::stop() {
    if (!running_.exchange(false))
        return;
    if (worker_.joinable())
        worker_.join();
}

std::string Autosave::autosavePath() const {
    if (!project_ || project_->directory().empty())
        return {};
    return (filesystem::fs::path(project_->directory()) / "autosave" / "project.autosave.json")
        .string();
}

Status Autosave::saveNow() {
    if (!project_ || project_->directory().empty())
        return failure(makeError(ErrorCode::InvalidArgument, "Project has no directory yet"));
    const auto path = filesystem::fs::path(autosavePath());
    const Status dirs = filesystem::createDirectories(path.parent_path());
    if (!dirs)
        return dirs;
    // The autosave document is the same JSON as the project document: recovery
    // therefore restores exactly what the user had, without a second format.
    return filesystem::writeTextFileAtomic(path, project_->serializeDocument());
}

bool Autosave::recoveryAvailable() const {
    const std::string path = autosavePath();
    if (path.empty() || !filesystem::isRegularFile(path))
        return false;
    // Recovery is offered when the session marker survived (i.e. the last
    // session did not close cleanly).
    if (project_ && !filesystem::exists(project_->crashMarkerPath()))
        return false;
    return true;
}

AuraResult<std::unique_ptr<Project>> Autosave::loadRecovery() {
    const std::string path = autosavePath();
    if (path.empty() || !filesystem::isRegularFile(path))
        return failure(makeError(ErrorCode::NotFound, "No autosave available"));
    auto text = filesystem::readTextFile(path);
    if (!text)
        return failure(text.error());
    const std::string directory =
        project_ ? project_->directory() : filesystem::fs::path(path).parent_path().string();
    return Project::deserializeDocument(directory, text.value());
}

void Autosave::workerLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        // Sleep in small slices so stop() is responsive.
        const int slices = static_cast<int>(intervalSeconds_ * 10.0);
        for (int i = 0; i < slices && running_.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!running_.load(std::memory_order_relaxed))
            break;
        if (!project_ || !project_->isModified())
            continue;
        const Status status = saveNow();
        if (status) {
            AURA_LOG_DEBUG(kCategory, "Autosave written (%s)", autosavePath().c_str());
        } else {
            AURA_LOG_WARN(kCategory, "Autosave failed: %s", status.error().toString().c_str());
        }
    }
}

} // namespace aura::project
