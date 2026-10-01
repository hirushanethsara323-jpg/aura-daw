// ============================================================================
// AURA DAW - tests/timeline/TimelineTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/timeline/Timeline.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

TEST_CASE("Snapping lands on the musical grid", "[timeline][snap]") {
    timeline::SnapContext context;
    context.sampleRate = 48000.0;
    context.mode = timeline::SnapMode::Beat;

    // At 120 BPM a beat is 24000 samples at 48 kHz (the context uses the
    // documented default grid: one beat).
    const auto snapped = timeline::snap(24500, context);
    INFO("snapped to " << snapped);
    REQUIRE((snapped % 24000) == 0);

    // Tolerance: a position far from any grid point is left alone.
    const auto untouched = timeline::snap(24500, context, 100);
    REQUIRE(untouched == 24500);

    // Off means off.
    context.mode = timeline::SnapMode::Off;
    REQUIRE(timeline::snap(24500, context) == 24500);
}

TEST_CASE("Snapping to markers picks the nearest marker", "[timeline][snap]") {
    const std::vector<std::int64_t> markers{0, 960, 1920}; // ticks (0, beat 1, beat 2)
    timeline::SnapContext context;
    context.sampleRate = 48000.0;
    context.bpm = 120.0;
    context.mode = timeline::SnapMode::Markers;
    context.markerTicks = &markers;

    const auto snapped = timeline::snap(25000, context);
    INFO("snapped to " << snapped);
    // 960 ticks = one beat at 960 PPQN = 0.5 s at 120 BPM = 24000 samples, and
    // that is the nearest marker to 25000.
    REQUIRE(snapped == 24000);
    // A position closer to the next marker snaps forward.
    REQUIRE(timeline::snap(47000, context) == 48000);
}

TEST_CASE("Zero-crossing snapping finds a click-free edit point", "[timeline][snap]") {
    // A 1 kHz sine at 48 kHz crosses zero every 24 samples.
    std::vector<float> samples(4800);
    test::fillSine(samples, 1000.0, 48000.0, 0.5f);
    const float* channels[1] = {samples.data()};

    const time::SamplePos crossed =
        timeline::nearestZeroCrossing(channels, 1, 4800, 1003, 64);
    INFO("crossing at " << crossed);
    REQUIRE(crossed != 1003);
    // The sample just before and after must have opposite signs (or be zero).
    const double before = samples[static_cast<std::size_t>(crossed - 1)];
    const double at = samples[static_cast<std::size_t>(crossed)];
    const bool signChange = (before <= 0.0) != (at <= 0.0);
    REQUIRE((signChange || at == 0.0));

    // A position that is already a crossing is returned unchanged.
    REQUIRE(timeline::nearestZeroCrossing(channels, 1, 4800, 24, 64) == 24);

    // With no data the position is returned unchanged rather than moved.
    REQUIRE(timeline::nearestZeroCrossing(nullptr, 0, 0, 1000, 64) == 1000);
}

TEST_CASE("View state clamps zoom and converts between pixels and samples", "[timeline][view]") {
    timeline::TimelineViewState view;
    view.setZoom(0.0); // nonsense input
    REQUIRE(view.samplesPerPixel == timeline::TimelineViewState::kMinSamplesPerPixel);
    view.setZoom(1.0e12);
    REQUIRE(view.samplesPerPixel == timeline::TimelineViewState::kMaxSamplesPerPixel);

    view.setZoom(100.0);
    view.scrollSamples = 1000;
    REQUIRE(view.positionToSamples(10.0) == 2000);
    REQUIRE(view.samplesToPosition(2000) == Approx(10.0));
    REQUIRE(view.samplesToPixels(500) == Approx(5.0));

    view.zoomToFit(0, 48000, 960);
    REQUIRE(view.samplesPerPixel == Approx(50.0));
    REQUIRE(view.scrollSamples == 0);

    view.zoomIn();
    REQUIRE(view.samplesPerPixel < 50.0);
    view.zoomOut();
    REQUIRE(view.samplesPerPixel == Approx(50.0).margin(0.01));
}

TEST_CASE("Marker navigation steps forwards and backwards", "[timeline][markers]") {
    std::vector<timeline::TimelineMarker> markers;
    markers.push_back({0, "Intro", 0, false});
    markers.push_back({3840, "Verse", 0, false});
    markers.push_back({7680, "Chorus", 0, true});

    REQUIRE(timeline::nextMarkerAfter(markers, 0) == 3840);
    REQUIRE(timeline::nextMarkerAfter(markers, 3841) == 7680);
    REQUIRE(timeline::nextMarkerAfter(markers, 10000) == 10000); // nothing ahead
    REQUIRE(timeline::previousMarkerBefore(markers, 7680) == 3840);
    REQUIRE(timeline::previousMarkerBefore(markers, 10) == 0);
    REQUIRE(timeline::previousMarkerBefore(markers, 0) == 0); // nothing behind
}

TEST_CASE("Ruler formatting is stable", "[timeline][format]") {
    time::Bbt position;
    position.bar = 3;
    position.beat = 2;
    position.tick = 480;
    REQUIRE(timeline::formatMusical(position) == "3.2.480");

    time::Smpte smpte;
    smpte.hours = 1;
    smpte.minutes = 2;
    smpte.seconds = 3;
    smpte.frames = 4;
    REQUIRE(timeline::formatSmpte(smpte, time::SmpteFrameRate::Fps25) == "01:02:03:04");

    REQUIRE(timeline::formatSeconds(61.5).find("1:01.500") != std::string::npos);
    REQUIRE(timeline::formatSeconds(-61.5).front() == '-');
}

TEST_CASE("Snap mode names cover every mode", "[timeline][snap]") {
    REQUIRE(std::string(timeline::snapModeName(timeline::SnapMode::Off)) == "Off");
    REQUIRE(std::string(timeline::snapModeName(timeline::SnapMode::ZeroCrossing)) ==
            "Zero crossing");
    REQUIRE(std::string(timeline::snapModeName(timeline::SnapMode::Frame)) == "Frame");
}
