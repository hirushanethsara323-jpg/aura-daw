// ============================================================================
// AURA DAW - src/automation/Automation.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/automation/Automation.hpp"

#include <algorithm>
#include <cmath>

#include "aura/core/Log.hpp"
#include "aura/core/Math.hpp"
#include "aura/core/Strings.hpp"

namespace aura::automation {
namespace {
constexpr const char* kCategory = "Automation";

/// Evaluates the segment between two points.
float interpolate(const Point& a, const Point& b, double t) noexcept {
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    switch (a.shape) {
    case CurveShape::Hold: return a.value;
    case CurveShape::Exponential: return a.value + (b.value - a.value) * static_cast<float>(t * t);
    case CurveShape::Logarithmic: return a.value + (b.value - a.value) * static_cast<float>(std::sqrt(t));
    case CurveShape::SCurve: {
        const double smooth = t * t * (3.0 - 2.0 * t);
        return a.value + (b.value - a.value) * static_cast<float>(smooth);
    }
    case CurveShape::Linear:
    default: return a.value + (b.value - a.value) * static_cast<float>(t);
    }
}

} // namespace

const char* curveShapeName(CurveShape shape) noexcept {
    switch (shape) {
    case CurveShape::Linear: return "linear";
    case CurveShape::Hold: return "hold";
    case CurveShape::Exponential: return "exponential";
    case CurveShape::Logarithmic: return "logarithmic";
    case CurveShape::SCurve: return "s-curve";
    }
    return "linear";
}

const char* automationModeName(AutomationMode mode) noexcept {
    switch (mode) {
    case AutomationMode::Read: return "Read";
    case AutomationMode::Touch: return "Touch";
    case AutomationMode::Write: return "Write";
    case AutomationMode::Latch: return "Latch";
    }
    return "Read";
}

// ---------------------------------------------------------------------------
// Lane
// ---------------------------------------------------------------------------
Lane::Lane(std::string targetId, float defaultValue)
    : targetId_(std::move(targetId)), defaultValue_(defaultValue) {}

void Lane::setPoint(std::int64_t positionTicks, float value, CurveShape shape) {
    const Point point{positionTicks, value, shape};
    const auto it = std::lower_bound(points_.begin(), points_.end(), positionTicks,
                                     [](const Point& candidate, std::int64_t position) {
                                         return candidate.positionTicks < position;
                                     });
    if (it != points_.end() && it->positionTicks == positionTicks)
        *it = point;
    else
        points_.insert(it, point);
}

void Lane::removePointAt(std::int64_t positionTicks) {
    const auto it = std::find_if(points_.begin(), points_.end(), [positionTicks](const Point& p) {
        return p.positionTicks == positionTicks;
    });
    if (it != points_.end())
        points_.erase(it);
}

void Lane::clearRange(std::int64_t startTicks, std::int64_t endTicks) {
    points_.erase(std::remove_if(points_.begin(), points_.end(),
                                 [startTicks, endTicks](const Point& p) {
                                     return p.positionTicks >= startTicks &&
                                            p.positionTicks < endTicks;
                                 }),
                  points_.end());
}

float Lane::valueAt(std::int64_t positionTicks) const noexcept {
    if (points_.empty())
        return defaultValue_;
    if (positionTicks <= points_.front().positionTicks)
        return points_.front().value;
    if (positionTicks >= points_.back().positionTicks)
        return points_.back().value;

    // Binary search for the segment containing the position.
    const auto upper = std::upper_bound(points_.begin(), points_.end(), positionTicks,
                                        [](std::int64_t position, const Point& point) {
                                            return position < point.positionTicks;
                                        });
    const Point& b = *upper;
    const Point& a = *(upper - 1);
    const auto span = static_cast<double>(b.positionTicks - a.positionTicks);
    if (span <= 0.0)
        return b.value;
    const double t = static_cast<double>(positionTicks - a.positionTicks) / span;
    return interpolate(a, b, t);
}

void Lane::sampleInto(std::int64_t startTicks, std::int64_t endTicks, std::int64_t stepTicks,
                      std::vector<float>& out) const {
    out.clear();
    if (stepTicks <= 0 || endTicks <= startTicks)
        return;
    const auto count = static_cast<std::size_t>((endTicks - startTicks) / stepTicks);
    out.reserve(count);
    for (std::int64_t position = startTicks; position < endTicks; position += stepTicks)
        out.push_back(valueAt(position));
}

// ---------------------------------------------------------------------------
// AutomationRecorder
// ---------------------------------------------------------------------------
void AutomationRecorder::begin(Lane& lane, std::int64_t startTicks, float value,
                               float thinningThreshold) {
    lane_ = &lane;
    lastPositionTicks_ = startTicks;
    lastValue_ = value;
    threshold_ = thinningThreshold > 0.0f ? thinningThreshold : 0.01f;
    written_ = 0;
    lane.setPoint(startTicks, value);
    ++written_;
}

void AutomationRecorder::update(std::int64_t positionTicks, float value) {
    if (!lane_)
        return;
    lastValue_ = value; // the gesture's current value, stored or not
    if (std::abs(value - lastWrittenValue_) < threshold_ &&
        positionTicks - lastPositionTicks_ < 480)
        return; // below the thinning threshold: nothing worth storing yet
    lane_->setPoint(positionTicks, value);
    lastPositionTicks_ = positionTicks;
    lastWrittenValue_ = value;
    ++written_;
}

void AutomationRecorder::end(std::int64_t positionTicks, bool latch) {
    (void)latch;
    if (!lane_)
        return;
    lane_->setPoint(positionTicks, lastValue_);
    ++written_;
    AURA_LOG_DEBUG(kCategory, "Automation recorded on '%s': %zu points",
                   lane_->targetId().c_str(), written_);
    lane_ = nullptr;
}

// ---------------------------------------------------------------------------
// TargetRange
// ---------------------------------------------------------------------------
float TargetRange::denormalise(float normalised) const noexcept {
    const float clamped = math::clamp01(normalised);
    if (!isLogarithmic)
        return minimum + clamped * (maximum - minimum);
    // Logarithmic mapping for frequency/gain targets: 0 -> minimum, 1 -> maximum.
    const float low = std::max(1.0e-6f, minimum);
    const float high = std::max(low * 1.000001f, maximum);
    return low * std::pow(high / low, clamped);
}

float TargetRange::normalise(float value) const noexcept {
    if (!isLogarithmic)
        return maximum > minimum ? math::clamp01((value - minimum) / (maximum - minimum)) : 0.0f;
    const float low = std::max(1.0e-6f, minimum);
    const float high = std::max(low * 1.000001f, maximum);
    const float clamped = std::max(low, std::min(high, value));
    return math::clamp01(std::log(clamped / low) / std::log(high / low));
}

// ---------------------------------------------------------------------------
// Target ids
// ---------------------------------------------------------------------------
namespace targets {
std::string trackVolume(std::uint32_t trackId) {
    return "track." + std::to_string(trackId) + ".volume";
}
std::string trackPan(std::uint32_t trackId) {
    return "track." + std::to_string(trackId) + ".pan";
}
std::string trackMute(std::uint32_t trackId) {
    return "track." + std::to_string(trackId) + ".mute";
}
std::string insertParameter(std::uint32_t trackId, std::size_t insertIndex,
                            std::size_t parameterIndex) {
    return "track." + std::to_string(trackId) + ".insert." + std::to_string(insertIndex) +
           ".param." + std::to_string(parameterIndex);
}
std::string pluginParameter(std::uint32_t trackId, std::size_t insertIndex,
                            std::size_t parameterIndex) {
    // Plug-in parameters get their own prefix so a future "show all plug-in
    // automation" view can filter on it.
    return "plugin.track." + std::to_string(trackId) + ".insert." +
           std::to_string(insertIndex) + ".param." + std::to_string(parameterIndex);
}
std::string masterVolume() { return "master.volume"; }
std::string masterLimiterCeiling() { return "master.limiter.ceiling"; }
} // namespace targets

} // namespace aura::automation
