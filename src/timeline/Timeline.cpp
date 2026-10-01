// ============================================================================
// AURA DAW - src/timeline/Timeline.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "aura/core/Math.hpp"

namespace aura::timeline {
namespace {

time::SamplePos snapToTicks(time::SamplePos position, std::int64_t gridTicks, double sampleRate,
                            double bpm, bool roundToNearest) {
    if (gridTicks <= 0 || sampleRate <= 0.0)
        return position;
    // Ticks -> samples: a beat lasts 60/bpm seconds, so one tick is
    // 60/(bpm * PPQN) seconds. (A tempo *map* is still not consulted - a single
    // tempo covers the whole timeline, which is what the snap context models.)
    const double secondsPerTick = 60.0 / (std::max(1.0, bpm) * static_cast<double>(time::kTicksPerBeat));
    const double samplesPerTick = sampleRate * secondsPerTick;
    const double gridSamples = static_cast<double>(gridTicks) * samplesPerTick;
    if (gridSamples <= 0.0)
        return position;
    const double positionInGrid = static_cast<double>(position) / gridSamples;
    const double target =
        roundToNearest ? std::round(positionInGrid) : std::floor(positionInGrid) + 1.0;
    return static_cast<time::SamplePos>(std::llround(target * gridSamples));
}

} // namespace

const char* snapModeName(SnapMode mode) noexcept {
    switch (mode) {
    case SnapMode::Off: return "Off";
    case SnapMode::Bar: return "Bar";
    case SnapMode::Beat: return "Beat";
    case SnapMode::Sixteenth: return "1/16";
    case SnapMode::ThirtySecond: return "1/32";
    case SnapMode::Frame: return "Frame";
    case SnapMode::Seconds: return "Seconds";
    case SnapMode::Markers: return "Markers";
    case SnapMode::Clips: return "Clips";
    case SnapMode::ZeroCrossing: return "Zero crossing";
    }
    return "Off";
}

time::SamplePos nearestZeroCrossing(const float* const* channels, int numChannels,
                                    std::int64_t frames, time::SamplePos positionSamples,
                                    int radiusSamples) noexcept {
    if (!channels || numChannels <= 0 || frames <= 0)
        return positionSamples;
    positionSamples = std::clamp<time::SamplePos>(positionSamples, 0, std::max<std::int64_t>(0, frames - 1));
    const int radius = std::max(1, radiusSamples);
    const auto limit = static_cast<std::int64_t>(radius);

    // Search outwards so the nearest crossing wins regardless of direction. A
    // zero crossing is where the summed signal changes sign; when the sample is
    // already exactly zero we are done.
    const auto signAt = [&](std::int64_t index) noexcept {
        double sum = 0.0;
        for (int channel = 0; channel < numChannels; ++channel) {
            if (channels[channel])
                sum += static_cast<double>(channels[channel][index]);
        }
        return sum;
    };

    // A sample that is "on zero" within numerical noise counts as a crossing: a
    // sine sampled exactly at a period boundary lands on +/-1e-16, not on 0.0,
    // and treating that as "no crossing here" made the search walk one sample
    // past the point the user clicked.
    constexpr double kZeroEpsilon = 1.0e-9;
    const auto isZero = [](double value) { return std::abs(value) <= kZeroEpsilon; };
    if (isZero(signAt(positionSamples)))
        return positionSamples;

    for (std::int64_t offset = 1; offset <= limit; ++offset) {
        const std::int64_t forward = positionSamples + offset;
        if (forward > 0 && forward < frames) {
            const double a = signAt(forward - 1);
            const double b = signAt(forward);
            if (isZero(a) || isZero(b) || (a < 0.0) != (b < 0.0))
                return forward;
        }
        const std::int64_t backward = positionSamples - offset;
        if (backward > 0 && backward < frames) {
            const double a = signAt(backward - 1);
            const double b = signAt(backward);
            if (isZero(a) || isZero(b) || (a < 0.0) != (b < 0.0))
                return backward;
        }
    }
    // No crossing within the search radius: return the original position so the
    // edit still happens (and never silently moves the user's edit far away).
    return positionSamples;
}

time::SamplePos snap(time::SamplePos positionSamples, const SnapContext& context,
                     time::SamplePos toleranceSamples) {
    if (context.mode == SnapMode::Off)
        return positionSamples;

    time::SamplePos snapped = positionSamples;
    switch (context.mode) {
    case SnapMode::Bar:
        snapped = snapToTicks(positionSamples, time::kTicksPerBar4_4, context.sampleRate, context.bpm, true);
        break;
    case SnapMode::Beat:
        snapped = snapToTicks(positionSamples, time::kTicksPerBeat, context.sampleRate, context.bpm, true);
        break;
    case SnapMode::Sixteenth:
        snapped = snapToTicks(positionSamples, time::kTicksPerBeat / 4, context.sampleRate, context.bpm, true);
        break;
    case SnapMode::ThirtySecond:
        snapped = snapToTicks(positionSamples, time::kTicksPerBeat / 8, context.sampleRate, context.bpm, true);
        break;
    case SnapMode::Frame: {
        // 25 fps is the default timeline frame rate; the project can override it
        // and the caller passes the resulting spacing through gridTicks == 0 and
        // sampleRate: frames are snapped in samples directly.
        const double frameSamples = context.sampleRate / 25.0;
        snapped = static_cast<time::SamplePos>(
            std::llround(std::round(static_cast<double>(positionSamples) / frameSamples) *
                         frameSamples));
        break;
    }
    case SnapMode::Seconds:
        snapped = static_cast<time::SamplePos>(
            std::llround(std::round(static_cast<double>(positionSamples) / context.sampleRate) *
                         context.sampleRate));
        break;
    case SnapMode::Markers: {
        if (!context.markerTicks || context.markerTicks->empty())
            return positionSamples;
        time::SamplePos best = positionSamples;
        time::SamplePos bestDistance = std::numeric_limits<time::SamplePos>::max();
        for (const std::int64_t markerTick : *context.markerTicks) {
            const double secondsPerTick =
                60.0 / (std::max(1.0, context.bpm) * static_cast<double>(time::kTicksPerBeat));
            const time::SamplePos markerSamples = static_cast<time::SamplePos>(
                std::llround(static_cast<double>(markerTick) * secondsPerTick * context.sampleRate));
            const time::SamplePos distance =
                markerSamples > positionSamples ? markerSamples - positionSamples
                                                : positionSamples - markerSamples;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = markerSamples;
            }
        }
        snapped = best;
        break;
    }
    case SnapMode::Clips: {
        if (!context.clipEdges || context.clipEdges->empty())
            return positionSamples;
        time::SamplePos best = positionSamples;
        time::SamplePos bestDistance = std::numeric_limits<time::SamplePos>::max();
        for (const time::SamplePos edge : *context.clipEdges) {
            const time::SamplePos distance =
                edge > positionSamples ? edge - positionSamples : positionSamples - edge;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = edge;
            }
        }
        snapped = best;
        break;
    }
    case SnapMode::ZeroCrossing:
        snapped = nearestZeroCrossing(context.sampleData, context.numChannels,
                                      context.sampleDataFrames, positionSamples);
        break;
    case SnapMode::Off:
    default: break;
    }

    // Tolerance: when the grid point is further away than the tolerance the user
    // asked for, keep the raw position (this is what makes "snap within N ms"
    // work in every DAW).
    if (toleranceSamples > 0) {
        const time::SamplePos distance =
            snapped > positionSamples ? snapped - positionSamples : positionSamples - snapped;
        if (distance > toleranceSamples)
            return positionSamples;
    }
    return snapped;
}

// ---------------------------------------------------------------------------
// TimelineViewState
// ---------------------------------------------------------------------------
void TimelineViewState::setZoom(double value) noexcept {
    // The parameter is not named after the member it assigns (-Wshadow would flag
    // it, and `this->` at the call site would be the only way to tell them apart).
    samplesPerPixel = math::clamp(value, kMinSamplesPerPixel, kMaxSamplesPerPixel);
}

void TimelineViewState::zoomIn(double factor) noexcept {
    setZoom(samplesPerPixel / std::max(1.01, factor));
}

void TimelineViewState::zoomOut(double factor) noexcept {
    setZoom(samplesPerPixel * std::max(1.01, factor));
}

void TimelineViewState::zoomToFit(time::SamplePos start, time::SamplePos end, int widthPixels) noexcept {
    if (widthPixels <= 0 || end <= start)
        return;
    scrollSamples = start;
    setZoom(static_cast<double>(end - start) / static_cast<double>(widthPixels));
}

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------
std::int64_t nextMarkerAfter(const std::vector<TimelineMarker>& markers,
                             std::int64_t positionTicks) noexcept {
    std::int64_t best = positionTicks;
    bool found = false;
    for (const auto& marker : markers) {
        if (marker.positionTicks > positionTicks &&
            (!found || marker.positionTicks < best)) {
            best = marker.positionTicks;
            found = true;
        }
    }
    return found ? best : positionTicks;
}

std::int64_t previousMarkerBefore(const std::vector<TimelineMarker>& markers,
                                  std::int64_t positionTicks) noexcept {
    std::int64_t best = positionTicks;
    bool found = false;
    for (const auto& marker : markers) {
        if (marker.positionTicks < positionTicks &&
            (!found || marker.positionTicks > best)) {
            best = marker.positionTicks;
            found = true;
        }
    }
    return found ? best : positionTicks;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
std::string formatMusical(const time::Bbt& position) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%d.%d.%03d", position.bar, position.beat,
                  static_cast<int>(position.tick));
    return buffer;
}

std::string formatSmpte(const time::Smpte& position, time::SmpteFrameRate rate) {
    (void)rate;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", position.hours, position.minutes,
                  position.seconds, position.frames);
    return buffer;
}

std::string formatSeconds(double seconds, bool showMilliseconds) {
    const double absolute = std::abs(seconds);
    const int minutes = static_cast<int>(absolute / 60.0);
    const double remainder = absolute - minutes * 60.0;
    char buffer[64];
    if (showMilliseconds)
        std::snprintf(buffer, sizeof(buffer), "%s%d:%06.3f", seconds < 0 ? "-" : "", minutes,
                      remainder);
    else
        std::snprintf(buffer, sizeof(buffer), "%s%d:%02d", seconds < 0 ? "-" : "", minutes,
                      static_cast<int>(remainder));
    return buffer;
}

} // namespace aura::timeline
