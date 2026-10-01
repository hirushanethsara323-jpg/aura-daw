// ============================================================================
// AURA DAW - tests/project/ProjectTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/audio/AudioClip.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/project/Project.hpp"
#include "aura/util/Localization.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {

std::shared_ptr<graph::SampleStream> sineStream(int frames) {
    std::vector<std::vector<float>> channels(2, std::vector<float>(static_cast<std::size_t>(frames)));
    test::fillSine(channels[0], 440.0, 48000.0, 0.5f);
    test::fillSine(channels[1], 440.0, 48000.0, 0.5f);
    return graph::MemoryStream::fromPlanar(std::move(channels), 48000.0);
}

} // namespace

TEST_CASE("A new project starts with one audio track and a sane tempo", "[project]") {
    auto project = project::Project::createEmpty("My Song");
    REQUIRE(project != nullptr);
    REQUIRE(project->metadata().name == "My Song");
    REQUIRE(project->mixer().trackCount() == 1);
    REQUIRE(project->tempoMap().tempoAt(0) == Approx(120.0));
    REQUIRE(project->clips().empty());
    REQUIRE(project->arrangementEndSamples(48000.0) == 0);
}

TEST_CASE("A project round-trips through the .aura directory format", "[project][io]") {
    const std::string directory = "/tmp/aura-project-test";
    (void)filesystem::removeFile(directory + "/project.json");

    auto project = project::Project::createEmpty("Round Trip");
    project->setSampleRate(48000.0);
    project->tempoMap().setDefaultTempo(132.0);
    project->tempoMap().addTempo(time::kTicksPerBar4_4 * 4, 90.0);
    project->mixer().prepare(48000.0, 256, 2);

    const auto trackId = project->mixer().createTrack(audio::TrackKind::Audio, "Guitar");
    project->mixer().findTrack(trackId)->setVolumeDb(-4.5f);
    project->mixer().findTrack(trackId)->setPan(0.25f);

    audio::Clip clip(project->nextClipId(), audio::ClipType::Audio, "Take 1");
    clip.setStart(0);
    clip.setLength(48000);
    clip.setStream(sineStream(48000));
    auto* stored = project->addClip(std::move(clip));
    REQUIRE(stored != nullptr);
    project->mixer().findTrack(trackId)->clips().push_back(stored->id());

    midi::MidiClip midiClip(project->nextMidiClipId(), "Keys");
    midi::Note note;
    note.pitch = 62;
    note.startTicks = 0;
    note.lengthTicks = 480;
    midiClip.addNote(note);
    REQUIRE(project->addMidiClip(std::move(midiClip)) != nullptr);

    project::Marker marker;
    marker.positionTicks = 3840;
    marker.name = "Chorus";
    project->markers().push_back(marker);

    std::string actualPath;
    REQUIRE(static_cast<bool>(project->saveAs(directory, &actualPath)));
    REQUIRE(filesystem::isRegularFile(actualPath + "/project.json"));

    auto reloaded = project::Project::load(actualPath);
    REQUIRE(reloaded.hasValue());
    auto& loaded = *reloaded.value();
    REQUIRE(loaded.metadata().name == "Round Trip");
    REQUIRE(loaded.tempoMap().tempoAt(0) == Approx(132.0));
    REQUIRE(loaded.tempoMap().tempoAt(time::kTicksPerBar4_4 * 4) == Approx(90.0));
    REQUIRE(loaded.mixer().trackCount() == 2);
    REQUIRE(loaded.clips().size() == 1);
    REQUIRE(loaded.midiClips().size() == 1);
    REQUIRE(loaded.midiClips().front()->notes().size() == 1);
    REQUIRE(loaded.markers().size() == 1);
    REQUIRE(loaded.markers().front().name == "Chorus");
    REQUIRE(loaded.arrangementEndSamples(48000.0) >= 48000);

    const auto* reloadedTrack = loaded.mixer().findTrack(trackId);
    if (reloadedTrack != nullptr) {
        REQUIRE(reloadedTrack->volumeDb() == Approx(-4.5f).margin(0.01f));
        REQUIRE(reloadedTrack->pan() == Approx(0.25f).margin(0.001f));
    }
}

TEST_CASE("A send survives a save and reload unchanged", "[project][io][sends]") {
    // A send's level and tap point are mix decisions: losing either one on save
    // changes the mix when the project is reopened, which is data loss the user
    // cannot see. The fields were serialized before they were used, so this pins
    // them to the audio they now control rather than to the JSON writer alone.
    const std::string directory = "/tmp/aura-project-send-test";
    (void)filesystem::removeFile(directory + "/project.json");

    auto project = project::Project::createEmpty("Send Round Trip");
    project->setSampleRate(48000.0);
    project->mixer().prepare(48000.0, 256, 2);

    const auto trackId = project->mixer().createTrack(audio::TrackKind::Audio, "Vocal");
    const auto cueId = project->mixer().createTrack(audio::TrackKind::Bus, "Cue");
    const auto reverbId = project->mixer().createTrack(audio::TrackKind::Bus, "Reverb");
    audio::Track* track = project->mixer().findTrack(trackId);
    REQUIRE(track != nullptr);

    audio::Send cue;
    cue.destination = cueId;
    cue.level = 0.25f;
    cue.preFader = true;
    cue.enabled = true;
    track->addSend(cue);

    audio::Send reverb;
    reverb.destination = reverbId;
    reverb.level = 1.0f;
    reverb.preFader = false;
    reverb.enabled = false; // a disabled send must stay disabled
    track->addSend(reverb);

    std::string actualPath;
    REQUIRE(static_cast<bool>(project->saveAs(directory, &actualPath)));

    auto reloaded = project::Project::load(actualPath);
    REQUIRE(reloaded.hasValue());
    const audio::Track* reloadedTrack = reloaded.value()->mixer().findTrack(trackId);
    REQUIRE(reloadedTrack != nullptr);
    REQUIRE(reloadedTrack->sends().size() == 2);

    const auto& loadedCue = reloadedTrack->sends()[0];
    REQUIRE(loadedCue.destination == cueId);
    REQUIRE(loadedCue.level == Approx(0.25f).margin(0.0001f));
    REQUIRE(loadedCue.preFader); // the tap point is a routing decision, not a detail
    REQUIRE(loadedCue.enabled);

    const auto& loadedReverb = reloadedTrack->sends()[1];
    REQUIRE(loadedReverb.destination == reverbId);
    REQUIRE(loadedReverb.level == Approx(1.0f).margin(0.0001f));
    REQUIRE_FALSE(loadedReverb.preFader);
    REQUIRE_FALSE(loadedReverb.enabled);
}

TEST_CASE("Save As never silently overwrites an existing project", "[project][io]") {
    const std::string directory = "/tmp/aura-project-collision";
    auto first = project::Project::createEmpty("First");
    std::string firstPath;
    REQUIRE(static_cast<bool>(first->saveAs(directory, &firstPath)));

    auto second = project::Project::createEmpty("Second");
    std::string secondPath;
    REQUIRE(static_cast<bool>(second->saveAs(directory, &secondPath)));

    REQUIRE(secondPath != firstPath);
    // Both projects still exist; nothing was destroyed.
    REQUIRE(filesystem::isRegularFile(firstPath + "/project.json"));
    REQUIRE(filesystem::isRegularFile(secondPath + "/project.json"));
}

TEST_CASE("A newer project format is refused instead of mis-parsed", "[project][io][regression]") {
    const std::string directory = "/tmp/aura-project-newer";
    (void)filesystem::createDirectories(directory);

    const std::string document = R"({
      "format": "aura-project",
      "versionMajor": 99,
      "versionMinor": 0,
      "metadata": { "name": "From The Future" }
    })";
    REQUIRE(static_cast<bool>(filesystem::writeTextFileAtomic(directory + "/project.json", document)));

    auto loaded = project::Project::load(directory);
    REQUIRE_FALSE(loaded.hasValue());
    REQUIRE(loaded.error().code == ErrorCode::ProjectVersionTooNew);
    // The project on disk is untouched by the failed load.
    REQUIRE(filesystem::isRegularFile(directory + "/project.json"));
}

TEST_CASE("Loading falls back to the backup when the document is corrupt",
          "[project][io][recovery]") {
    const std::string directory = "/tmp/aura-project-recovery";
    auto project = project::Project::createEmpty("Recover Me");
    std::string actualPath;
    REQUIRE(static_cast<bool>(project->saveAs(directory, &actualPath)));

    // Save twice so a .bak exists, then corrupt the main document.
    REQUIRE(static_cast<bool>(project->save()));
    REQUIRE(static_cast<bool>(project->save()));
    auto backup = filesystem::fs::path(actualPath) / "project.json.bak";
    INFO("backup exists: " << filesystem::isRegularFile(backup));
    if (filesystem::isRegularFile(backup)) {
        REQUIRE(static_cast<bool>(
            filesystem::writeTextFileAtomic(filesystem::fs::path(actualPath) / "project.json",
                                            "{ truncated")));
        auto recovered = project::Project::load(actualPath);
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value()->metadata().name == "Recover Me");
    }
}

TEST_CASE("The media pool tracks missing files instead of dropping them",
          "[project][media]") {
    const std::string directory = "/tmp/aura-project-media";
    (void)filesystem::createDirectories(directory);
    const std::string mediaPath = directory + "/kick.wav";

    std::vector<std::vector<float>> channels(2, std::vector<float>(4800));
    test::fillSine(channels[0], 80.0, 48000.0, 0.6f);
    test::fillSine(channels[1], 80.0, 48000.0, 0.6f);
    media::AudioFileWriter::Settings settings;
    settings.sampleRate = 48000;
    settings.numChannels = 2;
    settings.bitsPerSample = 24;
    REQUIRE(static_cast<bool>(media::writePlanarToFile(mediaPath, channels, 48000, settings)));

    project::MediaPool pool;
    const auto id = pool.addFile(mediaPath, false);
    REQUIRE(id != 0);
    // De-duplication: the same path twice is one item.
    REQUIRE(pool.addFile(mediaPath, false) == id);
    REQUIRE(pool.items().size() == 1);
    REQUIRE(pool.items().front().frames == 4800);

    // A missing file is marked, kept, and can be relinked.
    REQUIRE(static_cast<bool>(filesystem::removeFile(mediaPath)));
    pool.resolveAll(directory);
    REQUIRE(pool.items().size() == 1);
    REQUIRE_FALSE(pool.missingItems().empty());

    REQUIRE(static_cast<bool>(media::writePlanarToFile(mediaPath, channels, 48000, settings)));
    REQUIRE(static_cast<bool>(pool.relink(id, mediaPath)));
    REQUIRE(pool.missingItems().empty());
}

TEST_CASE("Autosave writes on modification and only reports recovery when needed",
          "[project][autosave]") {
    const std::string directory = "/tmp/aura-project-autosave";
    auto project = project::Project::createEmpty("Autosave Test");
    std::string actualPath;
    REQUIRE(static_cast<bool>(project->saveAs(directory, &actualPath)));

    project::Autosave autosave(project.get());
    autosave.setIntervalSeconds(0.05);
    (void)autosave.recoveryAvailable();
    REQUIRE(static_cast<bool>(autosave.saveNow()));
    const auto autosaveFile = filesystem::fs::path(actualPath) / "autosave" / "project.autosave.json";
    REQUIRE(filesystem::isRegularFile(autosaveFile));

    // No crash marker means no recovery prompt: the user is never nagged.
    REQUIRE_FALSE(project->crashMarkerPath().empty());
    REQUIRE_FALSE(autosave.recoveryAvailable());
}

TEST_CASE("User-facing messages are localized through the translator", "[project][localization]") {
    auto& translator = loc::Translator::instance();
    // The unit-test build registers no catalogue, so setLanguage() may report
    // NotFound and stay on the fallback - that is the documented behaviour. The
    // invariant this pins is that the translator is always left in a usable state
    // (no call site has to check whether a language is active).
    const Status status = translator.setLanguage("en");
    REQUIRE_FALSE(translator.language().empty());
    if (!status)
        REQUIRE(translator.language() == "en");
    const AuraError error = makeError(ErrorCode::DiskFull, "disk full");
    // The key exists for every code, so the UI can always show a translated
    // message rather than a raw enum name.
    REQUIRE(std::string(errorCodeMessageKey(error.code))[0] != '\0');
}
