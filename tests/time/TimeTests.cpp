// ============================================================================
// AURA DAW - tests/time/TimeTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/time/Time.hpp"

using namespace aura;
using Catch::Approx;

TEST_CASE("The tick resolution is the documented one", "[time]") {
    // 960 PPQN is AURA's canonical musical resolution: it divides binary and
    // ternary subdivisions exactly (960 = 2^6 * 3 * 5), which is why it is used
    // instead of 480 or 1000.
    REQUIRE(time::kTicksPerQuarterNote == 960);
    REQUIRE(time::kTicksPerBeat == 960);
    REQUIRE(time::kTicksPerBar4_4 == 3840);
}

TEST_CASE("Tempo map converts ticks to samples exactly", "[time][tempo]") {
    time::TempoMap map;
    map.setDefaultTempo(120.0);

    // At 120 BPM a quarter note is 0.5 s = 24000 samples at 48 kHz.
    REQUIRE(map.ticksToSamples(960, 48000.0) == 24000);
    REQUIRE(map.ticksToSamples(3840, 48000.0) == 96000);

    const auto samples = map.ticksToSamples(1234, 44100.0);
    REQUIRE(std::abs(map.samplesToTicks(samples, 44100.0) - 1234) <= 1);

    REQUIRE(map.ticksToSeconds(960) == Approx(0.5).margin(1e-9));
    REQUIRE(map.secondsToTicks(1.0) == 1920);
}

TEST_CASE("Tempo changes apply from their own position onwards", "[time][tempo]") {
    time::TempoMap map;
    map.setDefaultTempo(120.0);
    map.addTempo(time::kTicksPerBar4_4, 60.0); // bar 2 onwards: half speed

    const auto bar1 = map.ticksToSamples(0, 48000.0);
    const auto bar2 = map.ticksToSamples(time::kTicksPerBar4_4, 48000.0);
    const auto bar3 = map.ticksToSamples(time::kTicksPerBar4_4 * 2, 48000.0);

    REQUIRE(bar1 == 0);
    REQUIRE(bar2 - bar1 == 96000);  // one bar at 120 BPM
    REQUIRE(bar3 - bar2 == 192000); // one bar at 60 BPM
    REQUIRE(map.tempoAt(0) == Approx(120.0));
    REQUIRE(map.tempoAt(time::kTicksPerBar4_4) == Approx(60.0));
    REQUIRE(map.tempoAt(time::kTicksPerBar4_4 * 4) == Approx(60.0));
}

TEST_CASE("BBT conversion handles bars, beats and ticks", "[time][bbt]") {
    time::TempoMap map;
    map.setDefaultTempo(120.0);
    map.setDefaultTimeSignature({4, 4});

    const auto start = map.ticksToBbt(0);
    REQUIRE(start.bar == 1);
    REQUIRE(start.beat == 1);
    REQUIRE(start.tick == 0);

    const auto midBar = map.ticksToBbt(960 * 2 + 40);
    REQUIRE(midBar.bar == 1);
    REQUIRE(midBar.beat == 3);
    REQUIRE(midBar.tick == 40);

    const auto roundTrip = map.bbtToTicks({3, 2, 100});
    REQUIRE(roundTrip == time::kTicksPerBar4_4 * 2 + 960 + 100);
}

TEST_CASE("Time signature changes affect bar length", "[time][bbt]") {
    time::TempoMap map;
    map.setDefaultTempo(120.0);
    map.setDefaultTimeSignature({4, 4});
    map.addTimeSignature(time::kTicksPerBar4_4, {3, 4});

    REQUIRE(map.ticksPerBar(0) == 3840);
    REQUIRE(map.ticksPerBar(time::kTicksPerBar4_4) == 2880);

    const auto bar2 = map.ticksToBbt(3840);
    REQUIRE(bar2.bar == 2);
    const auto bar3 = map.ticksToBbt(3840 + 2880);
    REQUIRE(bar3.bar == 3);
}

TEST_CASE("SMPTE stays consistent, including 29.97 drop-frame", "[time][smpte]") {
    REQUIRE(time::frameRateValue(time::SmpteFrameRate::Fps25) == Approx(25.0));
    REQUIRE(time::frameRateValue(time::SmpteFrameRate::Fps29_97Drop) == Approx(29.97).margin(0.01));

    // 25 fps: two seconds is exactly 50 frames, 0 subframes.
    const auto smpte = time::secondsToSmpte(2.0, time::SmpteFrameRate::Fps25);
    REQUIRE(smpte.seconds == 2);
    REQUIRE(smpte.frames == 0);

    // Round trip through the conversion.
    REQUIRE(time::smpteToSeconds(smpte, time::SmpteFrameRate::Fps25) == Approx(2.0).margin(0.05));

    // Drop-frame exists so that one hour of *real* time reads 01:00:00;00
    // (non-drop at 29.97 drifts ~3.6 s per hour).
    const auto oneHour = time::secondsToSmpte(3600.0, time::SmpteFrameRate::Fps29_97Drop, true);
    INFO("one hour: " << time::formatSmpte(3600.0, time::SmpteFrameRate::Fps29_97Drop, true));
    REQUIRE(oneHour.hours == 1);
    REQUIRE(oneHour.minutes == 0);
    REQUIRE(oneHour.seconds == 0);
    REQUIRE(oneHour.frames == 0);

    // 29.97 fps counts 1798.2 frames in 60 s and the first minute is not dropped
    // from, so 60 s of real time is still inside minute 0 - not yet 00:01:00.
    const auto dropFrame = time::secondsToSmpte(60.0, time::SmpteFrameRate::Fps29_97Drop, true);
    REQUIRE(dropFrame.minutes == 0);
    REQUIRE(dropFrame.seconds == 59);
    REQUIRE(dropFrame.frames == 28); // 1798 frames into the minute

    const std::string text = time::formatSmpte(3661.5, time::SmpteFrameRate::Fps25);
    REQUIRE(text.find("01:01:01") != std::string::npos);
}
