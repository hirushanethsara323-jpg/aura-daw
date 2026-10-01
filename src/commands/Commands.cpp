// ============================================================================
// AURA DAW - src/commands/Commands.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/commands/Commands.hpp"

#include <algorithm>

#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

namespace aura::commands {
namespace {
constexpr const char* kCategory = "Commands";

/// Built-in command table. Shortcuts follow documented AURA defaults (see
/// docs/DEFAULT_SHORTCUTS.md); they are strings so the shortcut manager can
/// re-map them without the command layer knowing about key codes.
struct BuiltinCommand {
    const char* id;
    const char* category;
    const char* nameKey;
    const char* shortcut;
    bool toggle;
};

const BuiltinCommand kBuiltins[] = {
    {ids::kPlay, "Transport", "transport.play", "Space", false},
    {ids::kStop, "Transport", "transport.stop", "Numpad0", false},
    {ids::kPause, "Transport", "transport.pause", "", false},
    {ids::kRecord, "Transport", "transport.record", "Ctrl+R", true},
    {ids::kToggleLoop, "Transport", "transport.loop", "L", true},
    {ids::kReturnToStart, "Transport", "transport.returnToStart", "Return", false},
    {ids::kGoToEnd, "Transport", "transport.goToEnd", "End", false},
    {ids::kPreviousMarker, "Transport", "transport.previousMarker", "Ctrl+Left", false},
    {ids::kNextMarker, "Transport", "transport.nextMarker", "Ctrl+Right", false},
    {ids::kToggleMetronome, "Transport", "transport.metronome", "K", true},

    {ids::kSave, "Project", "project.save", "Ctrl+S", false},
    {ids::kSaveAs, "Project", "project.saveAs", "Ctrl+Shift+S", false},
    {ids::kNewProject, "Project", "project.new", "Ctrl+N", false},
    {ids::kOpenProject, "Project", "project.open", "Ctrl+O", false},
    {ids::kCloseProject, "Project", "project.close", "Ctrl+W", false},

    {ids::kUndo, "Edit", "edit.undo", "Ctrl+Z", false},
    {ids::kRedo, "Edit", "edit.redo", "Ctrl+Shift+Z", false},
    {ids::kSplit, "Edit", "edit.split", "S", false},
    {ids::kDelete, "Edit", "edit.delete", "Delete", false},
    {ids::kDuplicate, "Edit", "edit.duplicate", "Ctrl+D", false},
    {ids::kCopy, "Edit", "edit.copy", "Ctrl+C", false},
    {ids::kPaste, "Edit", "edit.paste", "Ctrl+V", false},
    {ids::kSelectAll, "Edit", "edit.selectAll", "Ctrl+A", false},
    {ids::kQuantise, "Edit", "midi.quantise", "Q", false},
    {ids::kNormalise, "Edit", "edit.normalize", "Ctrl+Shift+N", false},
    {ids::kReverseClip, "Edit", "edit.reverse", "", false},
    {ids::kConsolidate, "Edit", "edit.consolidate", "", false},
    {ids::kCrossfade, "Edit", "edit.crossfade", "X", false},

    {ids::kZoomIn, "View", "timeline.zoomIn", "+", false},
    {ids::kZoomOut, "View", "timeline.zoomOut", "-", false},
    {ids::kZoomToFit, "View", "timeline.zoomToFit", "Shift+F", false},
    {ids::kToggleSnap, "View", "timeline.snap", "B", true},

    {ids::kNewTrack, "Track", "track.new", "Ctrl+T", false},
    {ids::kNewMidiTrack, "Track", "track.newMidi", "Ctrl+Shift+T", false},
    {ids::kDeleteTrack, "Track", "track.delete", "", false},
    {ids::kAddMarker, "Timeline", "timeline.addMarker", "M", false},

    {ids::kExportAudio, "File", "export.title", "Ctrl+E", false},
    {ids::kPreferences, "File", "settings.title", "Ctrl+,", false},
};

} // namespace

// ---------------------------------------------------------------------------
// CommandRegistry
// ---------------------------------------------------------------------------
void CommandRegistry::registerCommand(CommandInfo info, CommandFunction function) {
    Entry entry{std::move(info), std::move(function)};
    const std::string id = entry.info.id;
    const auto it = index_.find(id);
    if (it != index_.end()) {
        // Re-registration replaces the action but keeps the metadata slot, which
        // is what the app layer wants when re-binding commands after a project
        // switch.
        entries_[it->second] = std::move(entry);
        return;
    }
    index_[id] = entries_.size();
    entries_.push_back(std::move(entry));
}

bool CommandRegistry::contains(const std::string& id) const noexcept {
    return index_.find(id) != index_.end();
}

const CommandInfo* CommandRegistry::info(const std::string& id) const noexcept {
    const auto it = index_.find(id);
    if (it == index_.end())
        return nullptr;
    return &entries_[it->second].info;
}

CommandResult CommandRegistry::execute(const std::string& id, CommandContext& context) const {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return CommandResult::failed(
            makeError(ErrorCode::NotFound, "Unknown command: " + id, id));
    }
    const Entry& entry = entries_[it->second];
    if (!entry.function)
        return CommandResult::failed(makeError(ErrorCode::NotImplemented,
                                               "Command has no implementation: " + id, id));
    return entry.function(context);
}

std::vector<const CommandInfo*> CommandRegistry::allCommands() const {
    std::vector<const CommandInfo*> all;
    all.reserve(entries_.size());
    for (const auto& entry : entries_)
        all.push_back(&entry.info);
    return all;
}

std::vector<const CommandInfo*> CommandRegistry::search(const std::string& query) const {
    std::vector<std::pair<int, const CommandInfo*>> scored;
    for (const auto& entry : entries_) {
        const int score = strings::fuzzyScore(entry.info.id, query);
        if (score >= 0)
            scored.emplace_back(score, &entry.info);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const CommandInfo*> results;
    results.reserve(scored.size());
    for (const auto& pair : scored)
        results.push_back(pair.second);
    return results;
}

void CommandRegistry::registerBuiltinCommands() {
    for (const auto& builtin : kBuiltins) {
        CommandInfo info;
        info.id = builtin.id;
        info.category = builtin.category;
        info.nameKey = builtin.nameKey;
        info.descriptionKey = std::string(builtin.nameKey) + ".description";
        info.defaultShortcut = builtin.shortcut;
        info.isToggle = builtin.toggle;

        // The action forwards to the app-supplied hook. This keeps the command
        // layer free of engine dependencies (and testable with a stub hook).
        registerCommand(info, [id = std::string(builtin.id)](CommandContext& context) {
            if (!context.engineAction) {
                return CommandResult::failed(makeError(ErrorCode::NotImplemented,
                                                       "No engine action handler is attached"));
            }
            context.engineAction(id);
            return CommandResult::ok();
        });
    }
    AURA_LOG_DEBUG(kCategory, "Registered %zu built-in commands", entries_.size());
}

// ---------------------------------------------------------------------------
// Concrete edits
// ---------------------------------------------------------------------------
Status MoveClipEdit::apply(project::Project& project) {
    audio::Clip* clip = project.findClip(clipId_);
    if (!clip)
        return failure(makeError(ErrorCode::NotFound, "Clip not found for move"));
    clip->setStart(newStart_);
    return success();
}

Status MoveClipEdit::revert(project::Project& project) {
    audio::Clip* clip = project.findClip(clipId_);
    if (!clip)
        return failure(makeError(ErrorCode::NotFound, "Clip not found for move"));
    clip->setStart(oldStart_);
    return success();
}

Status AddClipEdit::apply(project::Project& project) {
    if (applied_)
        return success();
    if (!clip_)
        return failure(makeError(ErrorCode::NotFound, "AddClipEdit lost its clip"));
    clipId_ = clip_->id();
    project.addClip(std::move(*clip_)); // the project now owns a copy
    clip_.reset();
    applied_ = true;
    return success();
}

Status AddClipEdit::revert(project::Project& project) {
    if (!applied_)
        return success();
    const auto found = std::find_if(project.clips().begin(), project.clips().end(),
                                    [this](const std::unique_ptr<audio::Clip>& clip) {
                                        return clip && clip->id() == clipId_;
                                    });
    if (found != project.clips().end()) {
        // Take ownership back so a redo can re-add the exact same clip.
        clip_ = std::make_unique<audio::Clip>(**found);
        project.clips().erase(found);
    } else {
        return failure(makeError(ErrorCode::NotFound, "Clip to undo was not found"));
    }
    applied_ = false;
    return success();
}

Status TrackMixEdit::apply(project::Project& project) {
    audio::Track* track = project.mixer().findTrack(track_);
    if (!track)
        return failure(makeError(ErrorCode::NotFound, "Track not found"));
    track->setVolume(after_.volume);
    track->setPan(after_.pan);
    track->setMuted(after_.muted);
    track->setSoloed(after_.soloed);
    (void)project.mixer().resolveSoloState();
    return success();
}

Status TrackMixEdit::revert(project::Project& project) {
    audio::Track* track = project.mixer().findTrack(track_);
    if (!track)
        return failure(makeError(ErrorCode::NotFound, "Track not found"));
    track->setVolume(before_.volume);
    track->setPan(before_.pan);
    track->setMuted(before_.muted);
    track->setSoloed(before_.soloed);
    (void)project.mixer().resolveSoloState();
    return success();
}

// ---------------------------------------------------------------------------
// UndoHistory
// ---------------------------------------------------------------------------
UndoHistory::UndoHistory(std::size_t maximumEntries) : maximumEntries_(maximumEntries) {}

void UndoHistory::beginTransaction(std::string description) {
    if (openTransaction_ && !openTransaction_->empty()) {
        // A transaction was left open (a UI bug): keep the user's edits instead
        // of discarding them, but make the mistake visible in the log.
        AURA_LOG_WARN(kCategory,
                      "Undo transaction '%s' was already open; committing it implicitly",
                      openTransaction_->description().c_str());
        redoStack_.clear();
        undoStack_.push_back(std::move(openTransaction_));
        while (undoStack_.size() > maximumEntries_)
            undoStack_.erase(undoStack_.begin());
    }
    openTransaction_ = std::make_unique<UndoTransaction>();
    openTransaction_->setDescription(std::move(description));
}

void UndoHistory::addEdit(std::unique_ptr<UndoEdit> edit) {
    if (!edit)
        return;
    if (!openTransaction_)
        beginTransaction(edit->description());
    openTransaction_->add(std::move(edit));
}

void UndoHistory::abortTransaction() {
    openTransaction_.reset();
}

void UndoHistory::commitTransaction(project::Project& project, bool alreadyApplied) {
    if (!openTransaction_)
        return;
    // A direct-manipulation gesture (dragging a clip, turning a fader) has
    // already changed the project, so the caller passes alreadyApplied = true.
    // Everything else - a menu command, a script, a macro - is applied here.
    if (!alreadyApplied) {
        for (const auto& edit : openTransaction_->edits())
            (void)edit->apply(project);
    }
    if (openTransaction_->empty()) {
        openTransaction_.reset();
        return;
    }
    // Committing a new action invalidates the redo branch: that is what users
    // expect from every editor.
    redoStack_.clear();
    undoStack_.push_back(std::move(openTransaction_));
    while (undoStack_.size() > maximumEntries_)
        undoStack_.erase(undoStack_.begin());
}

Status UndoHistory::undo(project::Project& project) {
    if (undoStack_.empty())
        return failure(makeError(ErrorCode::NotFound, "Nothing to undo"));
    auto transaction = std::move(undoStack_.back());
    undoStack_.pop_back();

    // Revert in reverse order so dependent edits unwind correctly.
    for (auto it = transaction->edits().rbegin(); it != transaction->edits().rend(); ++it) {
        const Status status = (*it)->revert(project);
        if (!status)
            return status;
    }
    redoStack_.push_back(std::move(transaction));
    return success();
}

Status UndoHistory::redo(project::Project& project) {
    if (redoStack_.empty())
        return failure(makeError(ErrorCode::NotFound, "Nothing to redo"));
    auto transaction = std::move(redoStack_.back());
    redoStack_.pop_back();
    for (const auto& edit : transaction->edits()) {
        const Status status = edit->apply(project);
        if (!status)
            return status;
    }
    undoStack_.push_back(std::move(transaction));
    return success();
}

std::string UndoHistory::nextUndoDescription() const {
    return undoStack_.empty() ? std::string() : undoStack_.back()->description();
}

std::string UndoHistory::nextRedoDescription() const {
    return redoStack_.empty() ? std::string() : redoStack_.back()->description();
}

std::vector<std::string> UndoHistory::historyDescriptions() const {
    std::vector<std::string> descriptions;
    descriptions.reserve(undoStack_.size());
    for (auto it = undoStack_.rbegin(); it != undoStack_.rend(); ++it)
        descriptions.push_back((*it)->description());
    return descriptions;
}

void UndoHistory::clear() {
    undoStack_.clear();
    redoStack_.clear();
    openTransaction_.reset();
}

} // namespace aura::commands
