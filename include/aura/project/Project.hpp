// ============================================================================
// AURA DAW - project/Project.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The .aura project format.
//
// A project is a DIRECTORY, not a single opaque blob:
//
//   MySong.aura/
//     project.json        - metadata, arrangement, tracks, mixer, automation,
//                           MIDI, markers, tempo map, plug-in *references*
//     plugin-states/      - one .json per plug-in instance (opaque to AURA)
//     media/              - audio referenced by the project (optional copy-in)
//     peaks/              - waveform caches (regenerable, safe to delete)
//
// Rationale (see docs/research/PROJECT_FORMAT_RESEARCH.md): a directory keeps
// large media out of the document, makes playlists/backups trivial (copy the
// folder), and keeps the human-readable part small enough to diff. The document
// itself is JSON: self-describing, forward compatible (unknown keys survive),
// and it can never be silently mis-parsed into a corrupt session.
//
// Guarantees implemented here:
//   * Atomic save: writes to project.json.tmp, fsyncs, renames. A crash mid-save
//     leaves either the old or the new file, never a half-written one.
//   * Version gates: a document from a newer major format version is refused
//     with ProjectVersionTooNew instead of being partially loaded.
//   * Autosave + recovery: a separate recovery file plus a crash marker, so a
//     crashed session can be restored (see Autosave).
//   * Never silently overwrite: Save As uses a unique path, backups keep a
//     .bak of the previous document.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/audio/AudioClip.hpp"
#include "aura/audio/Mixer.hpp"
#include "aura/core/Errors.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/time/Time.hpp"

namespace aura::project {

inline constexpr int kFormatVersionMajor = 1;
inline constexpr int kFormatVersionMinor = 0;
inline constexpr const char* kProjectExtension = ".aura";
inline constexpr const char* kDocumentFileName = "project.json";

/// Metadata shown in the UI and the file browser.
struct ProjectMetadata {
    std::string name = "Untitled";
    std::string artist;
    std::string comment;
    std::string createdAtIso;  ///< ISO-8601, local time
    std::string modifiedAtIso;
    std::int64_t sessionLengthSamples = 0;
    int versionMajor = kFormatVersionMajor;
    int versionMinor = kFormatVersionMinor;
};

/// A marker in the arrangement.
struct Marker {
    std::int64_t positionTicks = 0;
    std::string name;
    std::uint32_t colour = 0xFFFFB020u;
    bool isArrangementSection = false;
};

/// An entry in the media pool: one distinct source file.
struct MediaItem {
    std::uint64_t id = 0;
    std::string path;        ///< as stored in the project (relative when possible)
    std::string originalPath;///< absolute path at import time (for relinking)
    std::int64_t frames = 0;
    std::uint32_t sampleRate = 48000;
    std::uint16_t numChannels = 2;
    std::uint16_t bitsPerSample = 24;
    bool isRecording = false; ///< created by AURA rather than imported
    bool missing = false;     ///< resolved at load time
    std::int64_t fileSize = 0;
    float peakAmplitude = -1.0f;
};

/// Media pool: finds, relinks and reports on the audio a project references.
class MediaPool {
public:
    [[nodiscard]] const std::vector<MediaItem>& items() const noexcept { return items_; }
    [[nodiscard]] std::vector<MediaItem>& items() noexcept { return items_; }

    /// Adds a file (de-duplicating by canonical path). Returns the item id.
    std::uint64_t addFile(const std::string& path, bool isRecording);
    bool removeItem(std::uint64_t id);
    [[nodiscard]] MediaItem* find(std::uint64_t id) noexcept;
    [[nodiscard]] const MediaItem* find(std::uint64_t id) const noexcept;

    /// Resolves every item against `projectDirectory`; items that cannot be
    /// found are marked missing (never dropped - relinking must stay possible).
    void resolveAll(const std::string& projectDirectory);

    /// Relinks a missing item to a new path.
    Status relink(std::uint64_t id, const std::string& newPath);

    /// Removes items no clip references (the engine passes the referenced ids).
    std::size_t removeUnused(const std::vector<std::string>& referencedPaths);

    [[nodiscard]] std::vector<const MediaItem*> missingItems() const;

    /// Keeps the id counter ahead of ids restored from a project file.
    void ensureNextIdAbove(std::uint64_t id) noexcept {
        if (id + 1 > nextId_)
            nextId_ = id + 1;
    }

private:
    std::vector<MediaItem> items_;
    std::uint64_t nextId_ = 1;
};

/// The project document. Owns the mixer (and therefore every track) plus all
/// clips, MIDI clips, markers and tempo information.
class Project {
public:
    Project();
    ~Project();

    // --- lifecycle -----------------------------------------------------------
    /// Creates a new empty project (one master strip, one audio track).
    static std::unique_ptr<Project> createEmpty(const std::string& name);

    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
    [[nodiscard]] const std::string& documentPath() const noexcept { return documentPath_; }
    [[nodiscard]] bool isModified() const noexcept { return modified_; }
    void markModified() noexcept { modified_ = true; }
    void markSaved() noexcept { modified_ = false; }

    [[nodiscard]] ProjectMetadata& metadata() noexcept { return metadata_; }
    [[nodiscard]] const ProjectMetadata& metadata() const noexcept { return metadata_; }

    [[nodiscard]] audio::Mixer& mixer() noexcept { return *mixer_; }
    [[nodiscard]] const audio::Mixer& mixer() const noexcept { return *mixer_; }

    [[nodiscard]] time::TempoMap& tempoMap() noexcept { return tempoMap_; }
    [[nodiscard]] const time::TempoMap& tempoMap() const noexcept { return tempoMap_; }

    [[nodiscard]] std::vector<Marker>& markers() noexcept { return markers_; }
    [[nodiscard]] const std::vector<Marker>& markers() const noexcept { return markers_; }

    [[nodiscard]] MediaPool& mediaPool() noexcept { return mediaPool_; }
    [[nodiscard]] const MediaPool& mediaPool() const noexcept { return mediaPool_; }

    /// Clips are owned here (single owner: serialization, undo and the media
    /// pool all reference them by id).
    [[nodiscard]] std::vector<std::unique_ptr<audio::Clip>>& clips() noexcept { return clips_; }
    [[nodiscard]] const std::vector<std::unique_ptr<audio::Clip>>& clips() const noexcept {
        return clips_;
    }
    [[nodiscard]] std::vector<std::unique_ptr<midi::MidiClip>>& midiClips() noexcept {
        return midiClips_;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<midi::MidiClip>>& midiClips() const noexcept {
        return midiClips_;
    }

    audio::Clip* addClip(audio::Clip clip);
    midi::MidiClip* addMidiClip(midi::MidiClip clip);
    bool removeClip(audio::ClipId id);
    [[nodiscard]] audio::Clip* findClip(audio::ClipId id) noexcept;
    [[nodiscard]] midi::MidiClip* findMidiClip(std::uint64_t id) noexcept;

    /// End of the arrangement in samples at `sampleRate`: the furthest point any
    /// clip reaches. Used by export and by the timeline's "zoom to fit" so both
    /// agree on where the song ends.
    [[nodiscard]] time::SamplePos arrangementEndSamples(double sampleRate) const noexcept;

    [[nodiscard]] audio::ClipId nextClipId() noexcept { return nextClipId_++; }
    [[nodiscard]] std::uint64_t nextMidiClipId() noexcept { return nextMidiClipId_++; }

    // --- persistence ---------------------------------------------------------
    /// Saves the whole project (atomic). Creates the directory structure if it
    /// does not exist yet.
    Status save();
    /// Saves to a new directory (Save As). Refuses to overwrite silently: the
    /// caller gets a unique path back through `outActualPath`.
    Status saveAs(const std::string& directory, std::string* outActualPath = nullptr);
    /// Loads from a project directory (Open / recovery).
    static AuraResult<std::unique_ptr<Project>> load(const std::string& directory);
    /// Serializes the document to a JSON value (used by save, autosave and the
    /// test suite).
    [[nodiscard]] std::string serializeDocument() const;
    /// Applies a serialized document onto a fresh project.
    static AuraResult<std::unique_ptr<Project>> deserializeDocument(const std::string& directory,
                                                                   const std::string& jsonText);

    /// Session lifetime helpers.
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }

    /// Called when a project opens/closes to create/remove the crash marker.
    Status beginSession();
    Status endSession();

    /// Path of the write-protected "this session is open" marker; a leftover
    /// marker at startup means the last session crashed.
    [[nodiscard]] std::string crashMarkerPath() const;

private:
    std::string directory_;
    std::string documentPath_;
    ProjectMetadata metadata_{};
    std::unique_ptr<audio::Mixer> mixer_;
    time::TempoMap tempoMap_{};
    std::vector<Marker> markers_;
    MediaPool mediaPool_;
    std::vector<std::unique_ptr<audio::Clip>> clips_;
    std::vector<std::unique_ptr<midi::MidiClip>> midiClips_;
    double sampleRate_ = 48000.0;
    bool modified_ = false;
    audio::ClipId nextClipId_ = 1;
    std::uint64_t nextMidiClipId_ = 1;
};

/// Autosave + crash recovery.
///
/// Behaviour:
///   * The autosave worker writes `autosave/project.autosave.json` every N
///     seconds *and* on a timer only while the project is modified; it never
///     interrupts the audio thread (it runs on its own thread and only touches
///     the document model, which the audio thread never reads directly).
///   * On startup, a leftover crash marker plus an autosave newer than the last
///     manual save offers recovery.
class Autosave {
public:
    explicit Autosave(Project* project);
    ~Autosave();

    void setIntervalSeconds(double seconds);
    [[nodiscard]] double intervalSeconds() const noexcept { return intervalSeconds_; }

    void start();
    void stop();
    /// Writes an autosave immediately (used on crash-prone operations such as
    /// recording start and destructive commands).
    Status saveNow();

    [[nodiscard]] std::string autosavePath() const;
    [[nodiscard]] bool recoveryAvailable() const;

    /// Loads the autosave (called when the user accepts recovery).
    [[nodiscard]] AuraResult<std::unique_ptr<Project>> loadRecovery();

private:
    void workerLoop();

    Project* project_ = nullptr;
    double intervalSeconds_ = 60.0;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace aura::project
