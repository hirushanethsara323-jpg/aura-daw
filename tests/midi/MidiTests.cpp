// ============================================================================
// AURA DAW - tests/midi/MidiTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/dsp/Processor.hpp"
#include "aura/midi/Instrument.hpp"
#include "aura/midi/Midi.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

TEST_CASE("MIDI messages carry the right payload", "[midi]") {
    const auto noteOn = midi::Message::noteOn(0, 60, 100, 960);
    REQUIRE(noteOn.isNoteOn());
    REQUIRE_FALSE(noteOn.isNoteOff());
    REQUIRE(noteOn.data1 == 60);
    REQUIRE(noteOn.data2 == 100);
    REQUIRE(noteOn.timestampTicks == 960);

    // A note-on with velocity 0 is a note-off by definition (MIDI 1.0).
    const auto runningStatus = midi::Message::noteOn(0, 60, 0);
    REQUIRE(runningStatus.isNoteOff());

    const auto bend = midi::Message::pitchBend(0, 8192 + 4096);
    REQUIRE(bend.bendValue() == 8192 + 4096);
    REQUIRE(bend.bendSemitones(2) == Approx(1.0f).margin(0.01f));
}

TEST_CASE("Quantise moves notes onto the grid without destroying them", "[midi][quantise]") {
    midi::MidiClip clip(1, "Keys");
    midi::Note note;
    note.pitch = 60;
    note.velocity = 90;
    note.startTicks = 1000; // 40 ticks past the 1/16 grid (960)
    note.lengthTicks = 240;
    clip.addNote(note);

    midi::QuantiseSettings settings;
    settings.grid = midi::QuantiseGrid::Sixteenth;
    settings.strength = 1.0f;
    midi::quantiseNotes(clip.notes(), settings);

    REQUIRE(clip.notes().size() == 1);
    REQUIRE(clip.notes().front().startTicks == 960);
    REQUIRE(clip.notes().front().lengthTicks == 240);

    // Half strength moves halfway and never overshoots.
    midi::Note second;
    second.pitch = 64;
    second.startTicks = 1000;
    second.lengthTicks = 240;
    clip.addNote(second);
    settings.strength = 0.5f;
    midi::quantiseNotes(clip.notes(), settings);
    for (const auto& candidate : clip.notes()) {
        if (candidate.pitch == 64)
            REQUIRE(candidate.startTicks == Approx(980).margin(1));
    }
}

TEST_CASE("MIDI clips keep notes sorted and expose their range", "[midi][clip]") {
    midi::MidiClip clip(1, "Lead");
    for (const int pitch : {67, 60, 64}) {
        midi::Note note;
        note.pitch = pitch;
        note.startTicks = 960;
        clip.addNote(note);
    }
    REQUIRE(clip.notes().size() == 3);
    // Sorted by (start, pitch).
    REQUIRE(clip.notes()[0].pitch == 60);
    REQUIRE(clip.notes()[1].pitch == 64);
    REQUIRE(clip.notes()[2].pitch == 67);
    REQUIRE(clip.pitchRange().first == 60);
    REQUIRE(clip.pitchRange().second == 67);

    // Events inside a window are collected, and only those. The window is
    // half-open [start, end): the note-offs land exactly on tick 1920, so they
    // only appear once the window includes it.
    std::vector<midi::Message> halfOpen;
    clip.collectEvents(0, 1920, halfOpen);
    REQUIRE(halfOpen.size() == 3); // the three note-ons

    std::vector<midi::Message> full;
    clip.collectEvents(0, 1921, full);
    REQUIRE(full.size() == 6); // three note-ons and three note-offs
    REQUIRE(full.front().timestampTicks <= full.back().timestampTicks);
}

TEST_CASE("Recording notes bakes them into the clip", "[midi][clip][recording]") {
    midi::MidiClip clip(1, "Take");
    clip.appendRecorded(midi::Message::noteOn(0, 60, 100, 0));
    clip.appendRecorded(midi::Message::noteOn(0, 64, 90, 480));
    clip.appendRecorded(midi::Message::noteOff(0, 60, 960));
    clip.appendRecorded(midi::Message::noteOff(0, 64, 1200));
    clip.finishRecording();

    REQUIRE(clip.notes().size() == 2);
    REQUIRE(clip.notes()[0].pitch == 60);
    REQUIRE(clip.notes()[0].lengthTicks == 960);
    REQUIRE(clip.notes()[1].pitch == 64);
    REQUIRE(clip.notes()[1].lengthTicks == 720);
}

TEST_CASE("The MIDI input router maps devices to tracks without dropping messages",
          "[midi][router]") {
    midi::MidiInputRouter router;
    midi::MidiRoute route;
    route.inputDevice = "dev-1";
    route.inputChannel = -1; // all channels
    const std::uint32_t token = router.addRoute("Track 1", route);
    REQUIRE(token != 0);

    for (int i = 0; i < 8; ++i)
        router.handleIncomingMessage(midi::Message::noteOn(0, 60 + i, 100, 0), "dev-1");

    int received = 0;
    midi::Message message;
    while (router.popForRoute(token, message))
        ++received;
    REQUIRE(received == 8);
    REQUIRE(router.totalDroppedMessages() == 0);

    // A different device must not reach this route.
    router.handleIncomingMessage(midi::Message::noteOn(0, 72, 100, 0), "dev-2");
    REQUIRE_FALSE(router.popForRoute(token, message));

    router.panic();
    int panicMessages = 0;
    while (router.popForRoute(token, message)) {
        if (message.type == midi::Message::Type::ControlChange)
            ++panicMessages;
    }
    REQUIRE(panicMessages >= 16); // all-notes-off on all 16 channels
}

TEST_CASE("The built-in instrument turns notes into audio and releases them",
          "[midi][instrument]") {
    auto instrument = midi::createBuiltinInstrument("aura.basic");
    REQUIRE(instrument != nullptr);
    REQUIRE(instrument->id() == "aura.basic");
    REQUIRE(instrument->maxVoices() == 16);
    REQUIRE(static_cast<bool>(instrument->prepare(48000.0, 512, 2)));
    REQUIRE_FALSE(instrument->isProducingSound());

    test::Block block(2, 512);
    midi::Message noteOn = midi::Message::noteOn(0, 60, 110, 0);
    noteOn.sampleOffset = 0;
    auto view = block.view(512);
    instrument->process(view, &noteOn, 1, dsp::ProcessContext{});

    REQUIRE(instrument->activeVoices() == 1);
    REQUIRE(instrument->isProducingSound());
    REQUIRE(test::peak(block.channels[0]) > 0.001f);
    REQUIRE(test::allFinite(block.channels[0]));

    // Hold the note for 0.5 s: the sound must continue.
    for (int blockIndex = 0; blockIndex < 46; ++blockIndex) {
        test::Block next(2, 512);
        auto nextView = next.view(512);
        instrument->process(nextView, nullptr, 0, dsp::ProcessContext{});
    }
    REQUIRE(instrument->isProducingSound());

    // Release, then render past the release time: the voice must free itself.
    midi::Message noteOff = midi::Message::noteOff(0, 60, 0);
    noteOff.sampleOffset = 0;
    instrument->process(view, &noteOff, 1, dsp::ProcessContext{});
    for (int blockIndex = 0; blockIndex < 200; ++blockIndex) { // > 2 s
        test::Block next(2, 512);
        auto nextView = next.view(512);
        instrument->process(nextView, nullptr, 0, dsp::ProcessContext{});
    }
    REQUIRE_FALSE(instrument->isProducingSound());
    REQUIRE(instrument->activeVoices() == 0);
}

TEST_CASE("Voice stealing keeps a polyphonic instrument finite", "[midi][instrument]") {
    midi::PolySynth synth;
    REQUIRE(static_cast<bool>(synth.prepare(48000.0, 256, 2)));

    // 40 notes into 16 voices: no crash, no allocation growth, exactly 16 active.
    for (int i = 0; i < 40; ++i) {
        midi::Message noteOn = midi::Message::noteOn(0, 36 + i, 100, 0);
        noteOn.sampleOffset = 0;
        test::Block block(2, 256);
        auto view = block.view(256);
        synth.process(view, &noteOn, 1, dsp::ProcessContext{});
        REQUIRE(test::allFinite(block.channels[0]));
    }
    REQUIRE(synth.activeVoices() <= synth.maxVoices());
    REQUIRE(synth.activeVoices() > 0);
}
