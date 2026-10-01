// ============================================================================
// AURA DAW - commands/Commands.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Command system. Every user-visible action is a Command with a stable string
// id, so the same action can be triggered from a menu, a key binding, a MIDI
// controller mapping, the command palette or a test - and so undo/redo is
// uniform instead of being reinvented per widget.
//
//   AURA_Play, AURA_Stop, AURA_Record, AURA_Save, AURA_SaveAs, AURA_Undo,
//   AURA_Redo, AURA_Split, AURA_Delete, AURA_Duplicate, AURA_ZoomIn,
//   AURA_ZoomOut, AURA_NewTrack ...
//
// Undo model (see UndoHistory): each command may push an UndoTransaction that
// captures *what changed* as a small set of typed edits, not a whole-project
// snapshot. Snapshots are only used for operations that are genuinely global.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "aura/audio/AudioClip.hpp"
#include "aura/core/Errors.hpp"
#include "aura/project/Project.hpp"

namespace aura::commands {

/// Stable command identifiers (never localized, never renumbered).
namespace ids {
inline constexpr const char* kPlay = "AURA_Play";
inline constexpr const char* kStop = "AURA_Stop";
inline constexpr const char* kPause = "AURA_Pause";
inline constexpr const char* kRecord = "AURA_Record";
inline constexpr const char* kToggleLoop = "AURA_ToggleLoop";
inline constexpr const char* kReturnToStart = "AURA_ReturnToStart";
inline constexpr const char* kGoToEnd = "AURA_GoToEnd";
inline constexpr const char* kPreviousMarker = "AURA_PreviousMarker";
inline constexpr const char* kNextMarker = "AURA_NextMarker";
inline constexpr const char* kSave = "AURA_Save";
inline constexpr const char* kSaveAs = "AURA_SaveAs";
inline constexpr const char* kNewProject = "AURA_NewProject";
inline constexpr const char* kOpenProject = "AURA_OpenProject";
inline constexpr const char* kCloseProject = "AURA_CloseProject";
inline constexpr const char* kUndo = "AURA_Undo";
inline constexpr const char* kRedo = "AURA_Redo";
inline constexpr const char* kSplit = "AURA_Split";
inline constexpr const char* kDelete = "AURA_Delete";
inline constexpr const char* kDuplicate = "AURA_Duplicate";
inline constexpr const char* kCopy = "AURA_Copy";
inline constexpr const char* kPaste = "AURA_Paste";
inline constexpr const char* kSelectAll = "AURA_SelectAll";
inline constexpr const char* kZoomIn = "AURA_ZoomIn";
inline constexpr const char* kZoomOut = "AURA_ZoomOut";
inline constexpr const char* kZoomToFit = "AURA_ZoomToFit";
inline constexpr const char* kNewTrack = "AURA_NewTrack";
inline constexpr const char* kNewMidiTrack = "AURA_NewMidiTrack";
inline constexpr const char* kDeleteTrack = "AURA_DeleteTrack";
inline constexpr const char* kToggleMetronome = "AURA_ToggleMetronome";
inline constexpr const char* kToggleSnap = "AURA_ToggleSnap";
inline constexpr const char* kQuantise = "AURA_Quantise";
inline constexpr const char* kNormalise = "AURA_Normalise";
inline constexpr const char* kReverseClip = "AURA_ReverseClip";
inline constexpr const char* kConsolidate = "AURA_Consolidate";
inline constexpr const char* kCrossfade = "AURA_Crossfade";
inline constexpr const char* kAddMarker = "AURA_AddMarker";
inline constexpr const char* kExportAudio = "AURA_ExportAudio";
inline constexpr const char* kPreferences = "AURA_Preferences";
} // namespace ids

/// Context a command executes against. Passed by reference so commands never
/// capture engine internals: the app layer supplies whatever it has.
struct CommandContext {
    project::Project* project = nullptr;
    /// Opaque hook to the engine (transport, graph rebuild request, render).
    /// Declared as std::function so the command layer does not depend on the
    /// engine header (dependency inversion: the app wires it up).
    std::function<void(const std::string& commandId)> engineAction;
    std::function<void(const std::string& message)> reportMessage;
    /// Current selection (clip ids, note indices...) - kept generic on purpose.
    std::vector<std::uint64_t> selection;
};

/// Result of a command: a user-facing status plus what to report.
struct CommandResult {
    Status status;
    bool requiresRedraw = true;
    bool requiresAutosave = false;

    static CommandResult ok(bool redraw = true) {
        return CommandResult{success(), redraw, false};
    }
    static CommandResult failed(AuraError error) {
        return CommandResult{failure(std::move(error)), false, false};
    }
};

using CommandFunction = std::function<CommandResult(CommandContext&)>;

/// Metadata for UI, keyboard mapping and the command palette.
struct CommandInfo {
    std::string id;
    std::string category;
    /// Localization keys: the UI resolves these, the command layer stays textless.
    std::string nameKey;
    std::string descriptionKey;
    /// Default shortcut, e.g. "Space", "Ctrl+Z", "Ctrl+Shift+Z".
    std::string defaultShortcut;
    bool isToggle = false;
};

class CommandRegistry {
public:
    void registerCommand(CommandInfo info, CommandFunction function);
    [[nodiscard]] bool contains(const std::string& id) const noexcept;
    [[nodiscard]] const CommandInfo* info(const std::string& id) const noexcept;
    [[nodiscard]] CommandResult execute(const std::string& id, CommandContext& context) const;

    /// Every registered command (UI menus, palette, shortcut editor).
    [[nodiscard]] std::vector<const CommandInfo*> allCommands() const;
    /// Fuzzy search over names/ids - the palette uses this; must be instant, so
    /// it is a linear scan over a few hundred entries with a cheap scorer.
    [[nodiscard]] std::vector<const CommandInfo*> search(const std::string& query) const;

    /// Registers the built-in AURA command set. The app layer supplies the
    /// actions it can perform through CommandContext::engineAction.
    void registerBuiltinCommands();

private:
    struct Entry {
        CommandInfo info;
        CommandFunction function;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

/// A single reversible edit. Transactions group several edits so one user
/// action (e.g. "split clip") undoes atomically.
class UndoEdit {
public:
    virtual ~UndoEdit() = default;
    [[nodiscard]] virtual std::string description() const = 0;
    virtual Status apply(project::Project& project) = 0;   ///< redo
    virtual Status revert(project::Project& project) = 0;  ///< undo
};

/// Concrete edit: move a clip to a new start position.
class MoveClipEdit final : public UndoEdit {
public:
    MoveClipEdit(audio::ClipId clipId, time::SamplePos oldStart, time::SamplePos newStart)
        : clipId_(clipId), oldStart_(oldStart), newStart_(newStart) {}
    [[nodiscard]] std::string description() const override { return "Move clip"; }
    Status apply(project::Project& project) override;
    Status revert(project::Project& project) override;

private:
    audio::ClipId clipId_;
    time::SamplePos oldStart_;
    time::SamplePos newStart_;
};

/// Concrete edit: create a clip (revert removes it again).
class AddClipEdit final : public UndoEdit {
public:
    explicit AddClipEdit(std::unique_ptr<audio::Clip> clip) : clip_(std::move(clip)) {}
    [[nodiscard]] std::string description() const override { return "Add clip"; }
    Status apply(project::Project& project) override;
    Status revert(project::Project& project) override;

private:
    std::unique_ptr<audio::Clip> clip_;
    audio::ClipId clipId_ = 0;
    bool applied_ = false;
};

/// Concrete edit: change a track's volume/pan/mute/solo as one unit.
class TrackMixEdit final : public UndoEdit {
public:
    struct State {
        float volume = 1.0f;
        float pan = 0.0f;
        bool muted = false;
        bool soloed = false;
    };
    TrackMixEdit(audio::TrackId track, State before, State after)
        : track_(track), before_(before), after_(after) {}
    [[nodiscard]] std::string description() const override { return "Change track mixer"; }
    Status apply(project::Project& project) override;
    Status revert(project::Project& project) override;

private:
    audio::TrackId track_;
    State before_;
    State after_;
};

/// A group of edits performed/undone together.
class UndoTransaction {
public:
    void setDescription(std::string description) { description_ = std::move(description); }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    void add(std::unique_ptr<UndoEdit> edit) { edits_.push_back(std::move(edit)); }
    [[nodiscard]] std::size_t size() const noexcept { return edits_.size(); }
    [[nodiscard]] bool empty() const noexcept { return edits_.empty(); }
    [[nodiscard]] const std::vector<std::unique_ptr<UndoEdit>>& edits() const noexcept {
        return edits_;
    }

private:
    std::string description_;
    std::vector<std::unique_ptr<UndoEdit>> edits_;
};

/// Undo stack with a bounded history, transaction grouping and coalescing of
/// rapid parameter drags into one entry.
class UndoHistory {
public:
    explicit UndoHistory(std::size_t maximumEntries = 256);

    /// Starts a transaction. Nested calls are not supported on purpose: the UI
    /// must open exactly one per user action (a documented rule that keeps
    /// "one Ctrl+Z = one visible change" true).
    void beginTransaction(std::string description);
    void addEdit(std::unique_ptr<UndoEdit> edit);
    /// Commits the open transaction (no-op when empty).
    void commitTransaction(project::Project& project, bool alreadyApplied = false);
    void abortTransaction();

    Status undo(project::Project& project);
    Status redo(project::Project& project);

    [[nodiscard]] bool canUndo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redoStack_.empty(); }
    [[nodiscard]] std::string nextUndoDescription() const;
    [[nodiscard]] std::string nextRedoDescription() const;

    /// Undo history view (titles, newest first) for the UI.
    [[nodiscard]] std::vector<std::string> historyDescriptions() const;

    void clear();

private:
    std::size_t maximumEntries_;
    std::vector<std::unique_ptr<UndoTransaction>> undoStack_;
    std::vector<std::unique_ptr<UndoTransaction>> redoStack_;
    std::unique_ptr<UndoTransaction> openTransaction_;
    bool applying_ = false;
};

} // namespace aura::commands
