// ============================================================================
// AURA DAW - engine/OfflineRenderer.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline (faster-than-real-time) rendering for export, stem bouncing and
// consolidation.
//
// THE CENTRAL RULE: export uses the SAME graph, the same processors and the same
// transport advance path as real-time playback. There is no second DSP
// implementation to drift out of sync - see docs/AUDIO_ENGINE.md, "one signal
// path". The only differences are that the block loop runs as fast as the CPU
// allows, and the result goes to a file instead of a device.
//
// Stems: the same plan is rendered once per requested bus, soloing that node
// and its upstream chain, which is exactly how the mixer would sound if only
// that bus were audible.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aura/audio/Mixer.hpp"
#include "aura/core/Errors.hpp"
#include "aura/core/Json.hpp"
#include "aura/engine/AudioEngine.hpp"
#include "aura/media/AudioFile.hpp"
#include "aura/time/Time.hpp"

namespace aura::engine {

/// Export settings. Bit depth, dither and tail follow the brief's export
/// requirements; more options (per-stem format, marker splitting) hang off the
/// same struct as the export UI grows.
struct RenderSettings {
    double sampleRate = 48000.0;
    int blockSize = 512;            ///< processing block (does not affect the result)
    std::uint16_t bitsPerSample = 24;
    media::SampleFormat format = media::SampleFormat::PcmInt24;
    /// Adds padding so reverb/delay tails are not cut off.
    double tailSeconds = 2.0;
    /// Triangular dither at 1 LSB before quantising to integer formats.
    bool dither = true;
    bool normaliseTarget = false;
    float normaliseTargetDb = -1.0f;
    /// Render the whole mix, or one file per selected bus/track ("stems").
    bool renderStems = false;
    /// Track/bus ids to render when `renderStems` is true (empty = every audio
    /// producing track).
    std::vector<audio::TrackId> stemIds;
    /// When true the master limiter is bypassed (useful for stem exports that
    /// must not be individually limited).
    bool bypassMasterLimiter = false;
};

struct RenderProgress {
    std::atomic<double> fraction{0.0};
    std::atomic<std::int64_t> framesRendered{0};
    std::atomic<bool> cancelled{false};
};

struct RenderResult {
    std::vector<std::string> outputFiles;
    std::int64_t framesRendered = 0;
    double peakDbfs = -144.0;
    double elapsedSeconds = 0.0;
    /// Real-time factor: > 1 means faster than real time.
    double speedFactor = 0.0;
};

class OfflineRenderer {
public:
    explicit OfflineRenderer(AudioEngine& engine);

    /// Renders [startPosition, endPosition) of the arrangement to `outputPath`.
    /// When `endPosition` is 0, the end of the last clip is used.
    ///
    /// `progress` is called from the rendering thread and must be thread-safe.
    /// `cancel` is polled between blocks; a cancelled render still writes a
    /// valid (shorter) file rather than a corrupt one.
    AuraResult<RenderResult> renderRange(const std::string& outputPath,
                                         time::SamplePos startPosition,
                                         time::SamplePos endPosition,
                                         const RenderSettings& settings,
                                         std::function<void(double)> progress = {},
                                         RenderProgress* cancellable = nullptr);

    /// Renders each stem to "<baseName> - <track>.wav" next to `outputPath`.
    AuraResult<RenderResult> renderStems(const std::string& basePath, time::SamplePos start,
                                         time::SamplePos end, const RenderSettings& settings,
                                         std::function<void(double)> progress = {});

    /// Convenience: the whole arrangement.
    AuraResult<RenderResult> renderAll(const std::string& outputPath,
                                       const RenderSettings& settings,
                                       std::function<void(double)> progress = {});

    /// Last render's per-block statistics (underruns cannot happen offline; the
    /// block timings are still reported because they are a useful performance
    /// measurement of the same code that runs live).
    [[nodiscard]] const EngineStatistics& statistics() const noexcept { return statistics_; }

private:
    /// Renders `frames` of audio through the live plan, exactly like the audio
    /// device callback does. Returns false when the engine has no plan.
    bool renderBlock(float* const* outputs, int numFrames, time::SamplePos position);

    AudioEngine& engine_;
    EngineStatistics statistics_{};
    std::vector<midi::Message> midiScratch_;
    /// Tempo map copy for the transport advance: rendering must not mutate the
    /// project's map, and the renderer works on an explicit range.
    std::int64_t renderedFrames_ = 0;
};

} // namespace aura::engine
