// ============================================================================
// AURA DAW - automation/Automation.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation model: lanes of breakpoint curves that drive any parameter, plus
// the four recording modes the brief requires (Read / Touch / Write / Latch).
//
// Design decisions (see docs/research/AUTOMATION_RESEARCH.md):
//   * A lane addresses its target by a stable string id ("track.3.volume"),
//     not by a pointer. That keeps undo, copy/paste and the project file simple,
//     and means a lane survives a plug-in being reloaded.
//   * Evaluation happens once per audio block into a value that is then ramped
//     by the destination's own smoothing. AURA does not re-evaluate the curve
//     per sample: a breakpoint curve is linear between points by definition, so
//     evaluating at block boundaries and ramping is mathematically equivalent
//     for the linear case and avoids per-sample search costs on the audio thread.
//   * Writing (Touch/Latch/Write) records user moves as sparse points with a
//     configurable thinning threshold, so a fader gesture does not create
//     thousands of points.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/time/Time.hpp"

namespace aura::automation {

/// Shape of the segment that FOLLOWS a point.
enum class CurveShape : std::int32_t { Linear = 0, Hold, Exponential, Logarithmic, SCurve };

const char* curveShapeName(CurveShape shape) noexcept;

struct Point {
    std::int64_t positionTicks = 0;
    float value = 0.0f;             ///< normalised 0..1 (mapped by the target)
    CurveShape shape = CurveShape::Linear;
};

/// The four modes from the brief. `Off` is not a mode: a lane without automation
/// is simply empty or disabled.
enum class AutomationMode : std::int32_t { Read = 0, Touch, Write, Latch };

const char* automationModeName(AutomationMode mode) noexcept;

class Lane {
public:
    Lane() = default;
    Lane(std::string targetId, float defaultValue);

    [[nodiscard]] const std::string& targetId() const noexcept { return targetId_; }
    void setTargetId(std::string id) { targetId_ = std::move(id); }

    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    void setVisible(bool visible) noexcept { visible_ = visible; }

    [[nodiscard]] const std::vector<Point>& points() const noexcept { return points_; }
    [[nodiscard]] std::vector<Point>& points() noexcept { return points_; }

    [[nodiscard]] float defaultValue() const noexcept { return defaultValue_; }
    void setDefaultValue(float value) noexcept { defaultValue_ = value; }

    /// Inserts/replaces a point, keeping the list sorted by position.
    void setPoint(std::int64_t positionTicks, float value, CurveShape shape = CurveShape::Linear);
    void removePointAt(std::int64_t positionTicks);
    /// Removes points inside [start, end) - used by "delete range".
    void clearRange(std::int64_t startTicks, std::int64_t endTicks);
    void clear() { points_.clear(); }
    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

    /// Value at `positionTicks`. Outside the point range the curve holds the
    /// first/last value (the standard DAW behaviour: no implicit ramp to the
    /// default, which would silently change the mix).
    [[nodiscard]] float valueAt(std::int64_t positionTicks) const noexcept;

    /// Linear value in a range, sampled at `step` ticks (for the UI to draw and
    /// for the renderer to pre-compute automation into a buffer).
    void sampleInto(std::int64_t startTicks, std::int64_t endTicks, std::int64_t stepTicks,
                    std::vector<float>& out) const;

private:
    std::string targetId_;
    float defaultValue_ = 0.0f;
    bool enabled_ = true;
    bool visible_ = true;
    std::vector<Point> points_; ///< sorted by positionTicks
};

/// A recorder for one gesture: samples the incoming value and writes points when
/// the value has moved more than the thinning threshold.
class AutomationRecorder {
public:
    void begin(Lane& lane, std::int64_t startTicks, float value, float thinningThreshold = 0.01f);
    void update(std::int64_t positionTicks, float value);
    /// Latch keeps writing after the control is released; Touch stops.
    void end(std::int64_t positionTicks, bool latch);
    [[nodiscard]] bool isActive() const noexcept { return lane_ != nullptr; }
    [[nodiscard]] std::size_t writtenPoints() const noexcept { return written_; }

private:
    Lane* lane_ = nullptr;
    std::int64_t lastPositionTicks_ = 0;
    /// Last value that was actually written to the lane (thinning compares
    /// against this one).
    float lastWrittenValue_ = 0.0f;
    /// Latest value of the gesture, updated on every call so the point written
    /// when the gesture ends is where the user actually let go.
    float lastValue_ = 0.0f;
    float threshold_ = 0.01f;
    std::size_t written_ = 0;
};

/// Maps a normalised automation value onto a parameter range. Targets are
/// looked up by id by whoever owns the parameter (engine, mixer, plug-in host).
struct TargetRange {
    float minimum = 0.0f;
    float maximum = 1.0f;
    bool isLogarithmic = false; ///< frequency/gain style targets

    [[nodiscard]] float denormalise(float normalised) const noexcept;
    [[nodiscard]] float normalise(float value) const noexcept;
};

/// Standard target id builders so the engine, UI and project file always agree.
namespace targets {
[[nodiscard]] std::string trackVolume(std::uint32_t trackId);
[[nodiscard]] std::string trackPan(std::uint32_t trackId);
[[nodiscard]] std::string trackMute(std::uint32_t trackId);
[[nodiscard]] std::string insertParameter(std::uint32_t trackId, std::size_t insertIndex,
                                          std::size_t parameterIndex);
[[nodiscard]] std::string pluginParameter(std::uint32_t trackId, std::size_t insertIndex,
                                          std::size_t parameterIndex);
[[nodiscard]] std::string masterVolume();
[[nodiscard]] std::string masterLimiterCeiling();
} // namespace targets

} // namespace aura::automation
