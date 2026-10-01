// ============================================================================
// AURA DAW - tests/automation/AutomationTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/automation/Automation.hpp"

using namespace aura;
using Catch::Approx;

TEST_CASE("Automation lanes interpolate between breakpoints", "[automation]") {
    automation::Lane lane(automation::targets::trackVolume(3), 0.0f);
    REQUIRE(lane.targetId() == "track.3.volume");
    REQUIRE(lane.empty());
    // An empty lane holds its default (no implicit ramp that would change the mix).
    REQUIRE(lane.valueAt(0) == Approx(0.0f));

    lane.setPoint(0, 0.0f);
    lane.setPoint(960, 1.0f);

    REQUIRE(lane.valueAt(0) == Approx(0.0f));
    REQUIRE(lane.valueAt(960) == Approx(1.0f));
    REQUIRE(lane.valueAt(480) == Approx(0.5f).margin(0.001f));
    // Outside the range the curve holds.
    REQUIRE(lane.valueAt(-500) == Approx(0.0f));
    REQUIRE(lane.valueAt(99999) == Approx(1.0f));
}

TEST_CASE("Every curve shape is monotonic and hits its end points", "[automation]") {
    for (const auto shape : {automation::CurveShape::Linear, automation::CurveShape::Hold,
                             automation::CurveShape::Exponential,
                             automation::CurveShape::Logarithmic,
                             automation::CurveShape::SCurve}) {
        automation::Lane lane("test", 0.0f);
        lane.setPoint(0, 0.0f, shape);
        lane.setPoint(1000, 1.0f);
        INFO("shape " << automation::curveShapeName(shape));
        REQUIRE(lane.valueAt(0) == Approx(0.0f));
        REQUIRE(lane.valueAt(1000) == Approx(1.0f));
        REQUIRE(lane.valueAt(500) >= 0.0f);
        REQUIRE(lane.valueAt(500) <= 1.0f);
        if (shape == automation::CurveShape::Hold)
            REQUIRE(lane.valueAt(999) == Approx(0.0f));
    }
}

TEST_CASE("Points stay sorted and can be removed", "[automation]") {
    automation::Lane lane("test", 0.0f);
    lane.setPoint(1000, 0.3f);
    lane.setPoint(0, 0.1f);
    lane.setPoint(500, 0.2f);
    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.points()[0].positionTicks == 0);
    REQUIRE(lane.points()[1].positionTicks == 500);
    REQUIRE(lane.points()[2].positionTicks == 1000);

    // Setting the same position replaces rather than duplicates.
    lane.setPoint(500, 0.9f);
    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.points()[1].value == Approx(0.9f));

    lane.removePointAt(500);
    REQUIRE(lane.points().size() == 2);

    lane.clearRange(0, 1000);
    REQUIRE(lane.points().size() == 1); // the 1000 point survives an exclusive range
}

TEST_CASE("Recording thins a gesture into sparse points", "[automation][recording]") {
    automation::Lane lane("test", 0.0f);
    automation::AutomationRecorder recorder;
    recorder.begin(lane, 0, 0.0f, 0.05f);

    // A slow 1000-tick gesture with tiny movements must not create 1000 points.
    for (int tick = 0; tick <= 1000; ++tick)
        recorder.update(tick, static_cast<float>(tick) / 1000.0f);
    recorder.end(1000, false);

    REQUIRE(recorder.writtenPoints() > 1);
    REQUIRE(lane.points().size() < 40);
    REQUIRE(lane.valueAt(0) == Approx(0.0f));
    REQUIRE(lane.valueAt(1000) == Approx(1.0f).margin(0.01f));
}

TEST_CASE("Target ranges map normalised values correctly", "[automation]") {
    automation::TargetRange linear;
    linear.minimum = -1.0f;
    linear.maximum = 1.0f;
    REQUIRE(linear.denormalise(0.5f) == Approx(0.0f));
    REQUIRE(linear.denormalise(1.0f) == Approx(1.0f));
    REQUIRE(linear.normalise(0.0f) == Approx(0.5f));
    // Out-of-range input is clamped, never extrapolated.
    REQUIRE(linear.denormalise(2.0f) == Approx(1.0f));

    automation::TargetRange logRange;
    logRange.minimum = 20.0f;
    logRange.maximum = 20000.0f;
    logRange.isLogarithmic = true;
    // The midpoint of a logarithmic range is the geometric mean (632 Hz).
    REQUIRE(logRange.denormalise(0.5f) == Approx(632.0f).margin(5.0f));
    REQUIRE(logRange.normalise(632.0f) == Approx(0.5f).margin(0.01f));
}

TEST_CASE("Automation mode names are stable and user-visible", "[automation]") {
    REQUIRE(std::string(automation::automationModeName(automation::AutomationMode::Read)) == "Read");
    REQUIRE(std::string(automation::automationModeName(automation::AutomationMode::Touch)) == "Touch");
    REQUIRE(std::string(automation::automationModeName(automation::AutomationMode::Write)) == "Write");
    REQUIRE(std::string(automation::automationModeName(automation::AutomationMode::Latch)) == "Latch");
}

TEST_CASE("Lanes can be sampled into a buffer for the UI and the renderer", "[automation]") {
    automation::Lane lane("test", 0.0f);
    lane.setPoint(0, 0.0f);
    lane.setPoint(1000, 1.0f);
    std::vector<float> samples;
    lane.sampleInto(0, 1000, 100, samples);
    REQUIRE(samples.size() == 10);
    REQUIRE(samples.front() == Approx(0.0f));
    REQUIRE(samples[5] == Approx(0.5f).margin(0.01f));
}
