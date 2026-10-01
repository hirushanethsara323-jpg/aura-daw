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
