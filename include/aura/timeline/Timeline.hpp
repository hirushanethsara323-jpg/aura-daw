// ============================================================================
// AURA DAW - timeline/Timeline.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timeline concerns that are NOT the transport and NOT the arrangement model:
// the view state (zoom/scroll), the snapping engine, marker navigation and the
// zoom pyramid used by the arrangement view.
//
// Keeping this separate matters: the engine must never depend on view state, and
// the edit commands must snap the same way whether they come from the mouse, a
// keyboard nudge or a quantise command.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/time/Time.hpp"

namespace aura::timeline {

enum class SnapMode : std::int32_t {
    Off = 0,
    Bar,
    Beat,
    Sixteenth,
    ThirtySecond,
    Frame,          ///< SMPTE frame boundary (audio post workflows)
    Seconds,
    Markers,
    Clips,          ///< clip edges on the same track
    ZeroCrossing,   ///< nearest zero crossing - the click-free edit mode
};

const char* snapModeName(SnapMode mode) noexcept;

struct SnapContext {
    SnapMode mode = SnapMode::Beat;
    /// Grid resolution in ticks when the mode is a musical one.
    std::int64_t gridTicks = time::kTicksPerBeat;
    double sampleRate = 48000.0;
    /// Tempo used to convert ticks to samples. Musical snapping (bar/beat/1/16)
    /// and SnapMode::Markers (marker ticks) need it; without a tempo the grid
    /// would silently assume 60 BPM.
    double bpm = 120.0;
    /// Marker positions in ticks (used by SnapMode::Markers).
    const std::vector<std::int64_t>* markerTicks = nullptr;
    /// Nearby clip edges in samples (used by SnapMode::Clips).
    const std::vector<time::SamplePos>* clipEdges = nullptr;
    /// Sample data for zero-crossing snapping: planar pointers plus channel
    /// count. When null, zero-crossing snapping falls back to the grid.
    const float* const* sampleData = nullptr;
    int numChannels = 0;
    std::int64_t sampleDataFrames = 0;
};

/// Snaps `positionSamples` to the grid described by `context`.
/// `toleranceSamples` bounds how far the result may move (0 = always snap).
[[nodiscard]] time::SamplePos snap(time::SamplePos positionSamples,
                                   const SnapContext& context,
                                   time::SamplePos toleranceSamples = 0);

/// Finds the nearest zero crossing of the summed channels within +/- radius.
/// Returns `positionSamples` unchanged when nothing is found, so an edit that
/// cannot be made click-free still happens (and the UI can say so).
[[nodiscard]] time::SamplePos nearestZeroCrossing(const float* const* channels, int numChannels,
                                                  std::int64_t frames,
                                                  time::SamplePos positionSamples,
                                                  int radiusSamples = 64) noexcept;

/// Timeline view state (zoom + scroll + track heights). Pure data: the widgets
/// read it, the commands write it, tests construct it directly.
struct TimelineViewState {
    double samplesPerPixel = 512.0;   ///< zoom
    time::SamplePos scrollSamples = 0;
    int visibleTrackHeight = 0;       ///< 0 = automatic
    bool followPlayhead = true;

    /// Zoom limits: 1 sample per pixel (maximum detail) up to 5 minutes per
    /// pixel (whole-song overview). Both are clamped by the setter.
    static constexpr double kMinSamplesPerPixel = 1.0;
    static constexpr double kMaxSamplesPerPixel = 48000.0 * 300.0;

    void setZoom(double samplesPerPixel) noexcept;
    void zoomIn(double factor = 1.5) noexcept;
    void zoomOut(double factor = 1.5) noexcept;
    /// Fits [start, end] into `width` pixels.
    void zoomToFit(time::SamplePos start, time::SamplePos end, int widthPixels) noexcept;

    [[nodiscard]] time::SamplePos positionToSamples(double pixel) const noexcept {
        return scrollSamples + static_cast<time::SamplePos>(pixel * samplesPerPixel);
    }
    [[nodiscard]] double samplesToPosition(time::SamplePos position) const noexcept {
        return static_cast<double>(position - scrollSamples) / samplesPerPixel;
    }
    [[nodiscard]] double samplesToPixels(time::SamplePos span) const noexcept {
        return static_cast<double>(span) / samplesPerPixel;
    }
};

/// Marker with persistence-friendly fields. The arrangement model stores markers
/// in ticks; this type is the view/edit view of the same data.
struct TimelineMarker {
    std::int64_t positionTicks = 0;
    std::string name;
    std::uint32_t colour = 0xFFFFB020u;
    bool isSection = false;
};

/// Navigation helpers used by commands and key bindings.
[[nodiscard]] std::int64_t nextMarkerAfter(const std::vector<TimelineMarker>& markers,
                                           std::int64_t positionTicks) noexcept;
[[nodiscard]] std::int64_t previousMarkerBefore(const std::vector<TimelineMarker>& markers,
                                                std::int64_t positionTicks) noexcept;

/// Time ruler formatting. AURA shows bars:beats:ticks for musical work and
/// SMPTE when the project is in a post-production mode; both come from here so
/// the ruler and the transport readout never disagree.
[[nodiscard]] std::string formatMusical(const time::Bbt& position);
[[nodiscard]] std::string formatSmpte(const time::Smpte& position, time::SmpteFrameRate rate);
[[nodiscard]] std::string formatSeconds(double seconds, bool showMilliseconds = true);

} // namespace aura::timeline
