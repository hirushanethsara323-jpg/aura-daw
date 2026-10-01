// ============================================================================
// AURA DAW - tests/commands/CommandsTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include "aura/audio/AudioClip.hpp"
#include "aura/commands/Commands.hpp"
#include "aura/project/Project.hpp"
#include "support/TestSignals.hpp"

using namespace aura;

namespace {

std::shared_ptr<graph::SampleStream> stream(int frames) {
    std::vector<std::vector<float>> channels(2, std::vector<float>(static_cast<std::size_t>(frames)));
    test::fillSine(channels[0], 440.0, 48000.0, 0.5f);
    test::fillSine(channels[1], 440.0, 48000.0, 0.5f);
    return graph::MemoryStream::fromPlanar(std::move(channels), 48000.0);
}

} // namespace

TEST_CASE("Every built-in command is registered with stable ids", "[commands]") {
    commands::CommandRegistry registry;
    registry.registerBuiltinCommands();
    REQUIRE(registry.allCommands().size() >= 30);
    REQUIRE(registry.contains(commands::ids::kPlay));
    REQUIRE(registry.contains(commands::ids::kRecord));
    REQUIRE(registry.contains(commands::ids::kUndo));

    // Ids are the documented ones (never renumbered, never localized).
    REQUIRE(std::string(commands::ids::kPlay) == "AURA_Play");
    REQUIRE(std::string(commands::ids::kRecord) == "AURA_Record");

    // Shortcuts are declared for the keyboard-first workflow.
    const auto* play = registry.info(commands::ids::kPlay);
    REQUIRE(play != nullptr);
    REQUIRE(play->defaultShortcut == "Space");
    REQUIRE(play->category == "Transport");

    // Unknown commands fail with a diagnosable error instead of doing nothing.
    commands::CommandContext context;
    const auto result = registry.execute("AURA_NotACommand", context);
    REQUIRE_FALSE(result.status.hasValue());
    REQUIRE(result.status.error().code == ErrorCode::NotFound);
}

TEST_CASE("Commands reach the app through the engine action hook", "[commands]") {
    commands::CommandRegistry registry;
    registry.registerBuiltinCommands();

    std::vector<std::string> executed;
    commands::CommandContext context;
    context.engineAction = [&executed](const std::string& id) { executed.push_back(id); };

    REQUIRE(registry.execute(commands::ids::kPlay, context).status.hasValue());
    REQUIRE(registry.execute(commands::ids::kSave, context).status.hasValue());
    REQUIRE(executed.size() == 2);
    REQUIRE(executed[0] == commands::ids::kPlay);
    REQUIRE(executed[1] == commands::ids::kSave);

    // With no hook attached the command reports it rather than crashing.
    commands::CommandContext empty;
    REQUIRE_FALSE(registry.execute(commands::ids::kPlay, empty).status.hasValue());
}

TEST_CASE("The command palette ranks exact matches first", "[commands][ui]") {
    commands::CommandRegistry registry;
    registry.registerBuiltinCommands();
    const auto results = registry.search("record");
    REQUIRE_FALSE(results.empty());
    REQUIRE(results.front()->id == commands::ids::kRecord);
}

TEST_CASE("Undo and redo restore clip state exactly", "[commands][undo]") {
    auto project = project::Project::createEmpty("Undo Test");
    audio::Clip clip(1, audio::ClipType::Audio, "Take");
    clip.setStart(1000);
    clip.setLength(4000);
    clip.setStream(stream(8000));
    auto* stored = project->addClip(std::move(clip));
    REQUIRE(stored != nullptr);

    commands::UndoHistory history;
    REQUIRE_FALSE(history.canUndo());

    history.beginTransaction("Move clip");
    history.addEdit(std::make_unique<commands::MoveClipEdit>(stored->id(), 1000, 2500));
    // alreadyApplied = false: committing applies the edit (a menu command). A
    // direct-manipulation gesture passes true because the drag already moved it.
    history.commitTransaction(*project);
    REQUIRE(project->findClip(stored->id())->start() == 2500);

    REQUIRE(history.canUndo());
    REQUIRE(history.nextUndoDescription() == "Move clip");
    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(project->findClip(stored->id())->start() == 1000);

    REQUIRE(history.canRedo());
    REQUIRE(static_cast<bool>(history.redo(*project)));
    REQUIRE(project->findClip(stored->id())->start() == 2500);

    // Descriptions feed the UI's history list, newest first.
    const auto descriptions = history.historyDescriptions();
    REQUIRE(descriptions.size() == 1);
}

TEST_CASE("Adding a clip through an edit is undoable", "[commands][undo]") {
    auto project = project::Project::createEmpty("Add Clip");
    auto clip = std::make_unique<audio::Clip>(7, audio::ClipType::Audio, "New");
    clip->setStart(0);
    clip->setLength(1000);
    clip->setStream(stream(2000));
    const audio::ClipId id = clip->id();

    commands::UndoHistory history;
    history.beginTransaction("Add clip");
    history.addEdit(std::make_unique<commands::AddClipEdit>(std::move(clip)));
    history.commitTransaction(*project);

    // Undo before the edit was applied is a no-op, not a corruption.
    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(project->findClip(id) == nullptr);

    REQUIRE(static_cast<bool>(history.redo(*project)));
    REQUIRE(project->findClip(id) != nullptr);

    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(project->findClip(id) == nullptr);
}

// ---------------------------------------------------------------------------
// Re-entrancy guard: an edit (or a UI callback hanging off one) that reaches the
// history while undo()/redo() is running must not be recorded. Before this guard
// existed the `applying_` flag was declared and never read, which clang's
// -Wunused-private-field reported -- a warning that pointed at a real hole: the
// intruding transaction was spliced into the middle of the stack being unwound and
// cleared the redo branch the user still needed.
// ---------------------------------------------------------------------------
namespace {

/// An edit whose revert() performs *another* history-visible action, which is what
/// a callback wired to a parameter change looks like from inside the history.
class ReentrantEdit final : public commands::UndoEdit {
public:
    ReentrantEdit(std::string description, commands::UndoHistory& history, int& intrusions)
        : description_(std::move(description)), history_(history), intrusions_(intrusions) {}

    [[nodiscard]] std::string description() const override { return description_; }

    aura::Status apply(aura::project::Project&) override { return aura::success(); }

    aura::Status revert(aura::project::Project&) override {
        ++intrusions_;
        history_.beginTransaction("Intruder");
        history_.addEdit(std::make_unique<commands::MoveClipEdit>(1, 0, 1));
        history_.commitTransaction(*currentProject_);
        return aura::success();
    }

    void setProject(aura::project::Project* project) { currentProject_ = project; }

private:
    std::string description_;
    commands::UndoHistory& history_;
    int& intrusions_;
    aura::project::Project* currentProject_ = nullptr;
};

} // namespace

TEST_CASE("Undo ignores history recorded from inside its own callbacks",
          "[commands][undo][regression]") {
    auto project = project::Project::createEmpty("Re-entrancy Test");
    audio::Clip clip(1, audio::ClipType::Audio, "Take");
    clip.setStart(1000);
    clip.setLength(4000);
    clip.setStream(stream(8000));
    auto* stored = project->addClip(std::move(clip));
    REQUIRE(stored != nullptr);

    commands::UndoHistory history;
    int intrusions = 0;
    auto first = std::make_unique<commands::MoveClipEdit>(stored->id(), 1000, 3000);
    auto second = std::make_unique<commands::MoveClipEdit>(stored->id(), 3000, 5000);

    history.beginTransaction("First");
    history.addEdit(std::move(first));
    history.commitTransaction(*project);

    // Two user actions, so there is a redo branch worth protecting.
    history.beginTransaction("Second");
    history.addEdit(std::move(second));
    history.commitTransaction(*project);
    REQUIRE(stored->start() == 5000);

    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(stored->start() == 3000);

    // Now undo again, but this time the edit tries to record an action of its own.
    auto reentrant = std::make_unique<ReentrantEdit>("Third", history, intrusions);
    reentrant->setProject(project.get());
    // Push it directly: the point is what happens *during* the undo below.
    history.beginTransaction("Third");
    history.addEdit(std::make_unique<commands::MoveClipEdit>(stored->id(), 3000, 1000));
    history.commitTransaction(*project);
    REQUIRE(stored->start() == 1000);

    // Rebuild the stack so the re-entrant edit itself is the newest entry.
    history.clear();
    history.beginTransaction("Third");
    history.addEdit(std::move(reentrant));
    history.commitTransaction(*project);
    REQUIRE(stored->start() == 1000);

    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(intrusions == 1); // the callback really did run

    // The redo branch must still hold exactly the one real action, and undoing it
    // must put the clip back where the user left it.
    REQUIRE(history.canRedo());
    REQUIRE(history.nextRedoDescription() == "Third");
    REQUIRE_FALSE(static_cast<bool>(history.undo(*project))); // stack is exhausted
    REQUIRE(static_cast<bool>(history.redo(*project)));
    REQUIRE(stored->start() == 1000);

    // And the intruder never became an undo step of its own: after the two undos
    // and one redo above, the history holds at most the single real entry.
    CHECK(history.historyDescriptions().size() <= 1);
    CHECK(history.canRedo() == (history.historyDescriptions().size() == 0));
}

TEST_CASE("A new action clears the redo branch", "[commands][undo]") {
    auto project = project::Project::createEmpty("Redo Branch");
    commands::UndoHistory history;
    history.beginTransaction("First");
    history.addEdit(std::make_unique<commands::TrackMixEdit>(1, commands::TrackMixEdit::State{},
                                                             commands::TrackMixEdit::State{}));
    history.commitTransaction(*project);
    REQUIRE(static_cast<bool>(history.undo(*project)));
    REQUIRE(history.canRedo());

    history.beginTransaction("Second");
    history.addEdit(std::make_unique<commands::TrackMixEdit>(1, commands::TrackMixEdit::State{},
                                                             commands::TrackMixEdit::State{}));
    history.commitTransaction(*project);
    REQUIRE_FALSE(history.canRedo());
}

TEST_CASE("Undo history is bounded so memory cannot grow without limit", "[commands][undo]") {
    auto project = project::Project::createEmpty("Bounded");
    commands::UndoHistory history(4);
    for (int i = 0; i < 20; ++i) {
        history.beginTransaction("Edit " + std::to_string(i));
        history.addEdit(std::make_unique<commands::MoveClipEdit>(1, 0, i + 1));
        history.commitTransaction(*project);
    }
    REQUIRE(history.historyDescriptions().size() == 4);
    REQUIRE(history.historyDescriptions().front() == "Edit 19");
}
