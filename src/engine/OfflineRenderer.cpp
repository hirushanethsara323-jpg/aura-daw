// ============================================================================
// AURA DAW - src/engine/OfflineRenderer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/engine/OfflineRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include "aura/audio/AudioClip.hpp"
#include "aura/core/Clock.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"
#include "aura/project/Project.hpp"

namespace aura::engine {
namespace {
constexpr const char* kCategory = "Export";

/// Triangular (TPDF) dither at one LSB for the target bit depth. Generated with
/// a fixed-seed engine so two renders of the same project are reproducible -
/// which matters for tests and for "did my export change?" comparisons.
class Dither {
public:
    void prepare(int bits) noexcept {
        if (bits <= 0 || bits > 32)
            bits = 24;
        const double lsb = 1.0 / static_cast<double>(1ull << (bits - 1));
        amplitude_ = static_cast<float>(lsb);
    }
    [[nodiscard]] float next() noexcept {
        const float a = uniform_(rng_);
        const float b = uniform_(rng_);
        return (a - b) * amplitude_; // TPDF: difference of two uniform values
    }

private:
    std::uniform_real_distribution<float> uniform_{-1.0f, 1.0f};
    std::mt19937 rng_{0x41555241u}; // "AURA"
    float amplitude_ = 0.0f;
};

/// Applies dither + a hard clip guard before the WAV writer quantises.
void prepareForExport(float* buffer, int frames, bool dither, Dither& generator) {
    for (int i = 0; i < frames; ++i) {
        float value = buffer[i];
        if (dither)
            value += generator.next();
        // Integer formats cannot represent beyond +/-1.0: clamp here instead of
        // letting the writer wrap around (which would be an audible defect).
        buffer[i] = std::clamp(value, -1.0f, 1.0f);
    }
}

} // namespace

OfflineRenderer::OfflineRenderer(AudioEngine& engine) : engine_(engine) {
    midiScratch_.reserve(2048);
}

bool OfflineRenderer::renderBlock(float* const* outputs, int numFrames, time::SamplePos position) {
    // The renderer drives the engine's own block path; the engine never touches
    // a device here, so the same DSP code that runs live produces the file.
    engine_.transport().seek(position);
    engine_.processBlock(outputs, nullptr, numFrames);
    return true;
}

AuraResult<RenderResult> OfflineRenderer::renderRange(const std::string& outputPath,
                                                      time::SamplePos startPosition,
                                                      time::SamplePos endPosition,
                                                      const RenderSettings& settings,
                                                      std::function<void(double)> progress,
                                                      RenderProgress* cancellable) {
    if (!engine_.project())
        return failure(makeError(ErrorCode::NotFound, "No project is loaded"));
    if (!engine_.currentPlan())
        return failure(makeError(ErrorCode::NotFound,
                                 "The audio graph has not been built yet (call rebuildGraph())"));

    // The engine preallocates every graph and insert buffer for its configured
    // block size. Rendering with a bigger block would write past those buffers,
    // so the render block is clamped to what the graph was prepared for (and a
    // sample-rate mismatch is refused rather than silently producing
    // wrong-speed audio: there is no sample-rate conversion in the engine yet).
    if (settings.sampleRate != engine_.settings().sampleRate)
        return failure(makeError(
            ErrorCode::InvalidArgument,
            "Offline render sample rate must match the engine sample rate",
            "engine expects " + std::to_string(engine_.settings().sampleRate) +
                " Hz, the render asked for " + std::to_string(settings.sampleRate) +
                " Hz - set the device/project sample rate first (sample-rate conversion is not "
                "implemented yet)"));
    const int engineBlockSize = std::max(1, engine_.settings().bufferSize);

    project::Project& project = *engine_.project();
    const double sampleRate = settings.sampleRate;
    const auto startNanos = core::monotonicNanos();

    if (endPosition <= startPosition)
        endPosition = project.arrangementEndSamples(sampleRate);
    if (endPosition <= startPosition)
        return failure(makeError(ErrorCode::InvalidArgument,
                                 "Nothing to render: the arrangement is empty"));

    const std::int64_t tailFrames = static_cast<std::int64_t>(settings.tailSeconds * sampleRate);
    const std::int64_t totalFrames = (endPosition - startPosition) + tailFrames;

    media::AudioFileWriter::Settings writerSettings;
    writerSettings.sampleRate = static_cast<std::uint32_t>(sampleRate);
    writerSettings.numChannels = 2;
    writerSettings.bitsPerSample = settings.bitsPerSample;
    writerSettings.format = settings.format;

    const auto outputFile = filesystem::fs::path(outputPath);
    AURA_RETURN_ON_ERROR(filesystem::createDirectories(outputFile.parent_path()));

    media::AudioFileWriter writer;
    auto created = writer.create(outputPath, writerSettings);
    if (!created)
        return failure(created.error());

    Dither dither;
    dither.prepare(settings.bitsPerSample);

    const int blockSize = std::clamp(settings.blockSize, 16, engineBlockSize);
    std::vector<float> left(static_cast<std::size_t>(blockSize));
    std::vector<float> right(static_cast<std::size_t>(blockSize));
    std::vector<float> interleaved(static_cast<std::size_t>(blockSize) * 2);
    float* outputs[2] = {left.data(), right.data()};

    const bool limiterWasBypassed = project.mixer().masterLimiter().isBypassed();
    if (settings.bypassMasterLimiter)
        project.mixer().masterLimiter().setBypassed(true);

    // The transport is put in a clean playing state for the render: offline
    // rendering must not inherit the user's playhead or loop range.
    engine_.transport().stop();
    engine_.transport().clearLoopRange();
    engine_.transport().play();

    RenderResult result;
    std::int64_t rendered = 0;
    double peak = 0.0;
    bool cancelled = false;

    while (rendered < totalFrames) {
        if (cancellable && cancellable->cancelled.load(std::memory_order_relaxed)) {
            cancelled = true;
            break;
        }
        const int frames = static_cast<int>(
            std::min<std::int64_t>(blockSize, totalFrames - rendered));
        // The transport advances by the engine's own advance() so tempo, loop
        // and punch behaviour is identical to live playback.
        renderBlock(outputs, frames, startPosition + rendered);

        for (int i = 0; i < frames; ++i) {
            peak = std::max(peak, std::max(std::abs(static_cast<double>(left[i])),
                                           std::abs(static_cast<double>(right[i]))));
            interleaved[static_cast<std::size_t>(i) * 2] = left[static_cast<std::size_t>(i)];
            interleaved[static_cast<std::size_t>(i) * 2 + 1] = right[static_cast<std::size_t>(i)];
            prepareForExport(&interleaved[static_cast<std::size_t>(i) * 2], 2,
                             settings.dither, dither);
        }

        auto written = writer.writeInterleaved(interleaved.data(), frames);
        if (!written) {
            if (settings.bypassMasterLimiter)
                project.mixer().masterLimiter().setBypassed(limiterWasBypassed);
            (void)writer.close();
            return failure(written.error());
        }
        rendered += frames;

        if (progress)
            progress(static_cast<double>(rendered) / static_cast<double>(totalFrames));
        if (cancellable)
            cancellable->fraction.store(static_cast<double>(rendered) /
                                           static_cast<double>(totalFrames),
                                       std::memory_order_relaxed);
    }

    engine_.transport().stop();
    if (settings.bypassMasterLimiter)
        project.mixer().masterLimiter().setBypassed(limiterWasBypassed);

    const Status closed = writer.close();
    if (!closed)
        return failure(closed.error());

    result.outputFiles.push_back(outputPath);
    result.framesRendered = rendered;
    result.peakDbfs = peak > 0.0 ? math::gainToDb(static_cast<float>(peak)) : -144.0;
    const double elapsed =
        static_cast<double>(core::monotonicNanos() - startNanos) / 1.0e9;
    result.elapsedSeconds = elapsed;
    const double audioSeconds = static_cast<double>(rendered) / sampleRate;
    result.speedFactor = elapsed > 0.0 ? audioSeconds / elapsed : 0.0;

    AURA_LOG_INFO(kCategory,
                  "Rendered %lld frames to %s in %.2f s (%.1fx real time, peak %.2f dBFS%s)",
                  static_cast<long long>(rendered), outputPath.c_str(), elapsed,
                  result.speedFactor, result.peakDbfs, cancelled ? ", CANCELLED" : "");
    return success(std::move(result));
}

AuraResult<RenderResult> OfflineRenderer::renderStems(const std::string& basePath,
                                                      time::SamplePos start,
                                                      time::SamplePos end,
                                                      const RenderSettings& settings,
                                                      std::function<void(double)> progress) {
    if (!engine_.project())
        return failure(makeError(ErrorCode::NotFound, "No project is loaded"));
    project::Project& project = *engine_.project();

    std::vector<audio::TrackId> stemIds = settings.stemIds;
    if (stemIds.empty()) {
        for (audio::Track* track : project.mixer().tracks()) {
            if (track && track->kind() != audio::TrackKind::Bus &&
                track->kind() != audio::TrackKind::Aux)
                stemIds.push_back(track->id());
        }
    }
    if (stemIds.empty())
        return failure(makeError(ErrorCode::NotFound, "No tracks to render as stems"));

    const auto base = filesystem::fs::path(basePath);
    const std::string stemDirectory = base.parent_path().string();
    const std::string baseName = base.stem().string();

    RenderResult combined;
    const auto startNanos = core::monotonicNanos();

    // Solo one track at a time: the graph's solo rules make the bus path and its
    // inserts run exactly as they would in the session, so a stem is a faithful
    // bounce rather than a re-mix.
    std::vector<std::pair<audio::TrackId, bool>> previousSolo;
    for (audio::Track* track : project.mixer().allTracksIncludingMaster())
        previousSolo.emplace_back(track->id(), track->isSoloed());

    int index = 0;
    for (const audio::TrackId stemId : stemIds) {
        audio::Track* track = project.mixer().findTrack(stemId);
        if (!track)
            continue;

        for (audio::Track* other : project.mixer().tracks())
            other->setSoloed(other->id() == stemId);
        (void)project.mixer().resolveSoloState();

        const auto stemPath = filesystem::fs::path(stemDirectory) /
                              (baseName + " - " + filesystem::sanitizeFileName(track->name()) + ".wav");
        RenderSettings stemSettings = settings;
        stemSettings.renderStems = false;
        // Individual stems keep the master limiter bypassed: limiting each stem
        // separately would change the sum, and the user can limit the master.
        stemSettings.bypassMasterLimiter = true;

        auto stemResult = renderRange(stemPath.string(), start, end, stemSettings,
                                      [&](double fraction) {
                                          if (progress) {
                                              const double spans = static_cast<double>(stemIds.size());
                                              progress((static_cast<double>(index) + fraction) / spans);
                                          }
                                      });
        if (!stemResult)
            return failure(stemResult.error());

        combined.outputFiles.push_back(stemPath.string());
        combined.framesRendered = std::max(combined.framesRendered, stemResult.value().framesRendered);
        combined.peakDbfs = std::max(combined.peakDbfs, stemResult.value().peakDbfs);
        ++index;
    }

    for (const auto& [id, soloed] : previousSolo) {
        if (audio::Track* track = project.mixer().findTrack(id))
            track->setSoloed(soloed);
    }
    (void)project.mixer().resolveSoloState();

    const double elapsed = static_cast<double>(core::monotonicNanos() - startNanos) / 1.0e9;
    combined.elapsedSeconds = elapsed;
    const double audioSeconds =
        static_cast<double>(combined.framesRendered) / std::max(1.0, settings.sampleRate);
    combined.speedFactor = elapsed > 0.0 ? audioSeconds / elapsed : 0.0;
    AURA_LOG_INFO(kCategory, "Rendered %zu stems in %.2f s", combined.outputFiles.size(), elapsed);
    return success(std::move(combined));
}

AuraResult<RenderResult> OfflineRenderer::renderAll(const std::string& outputPath,
                                                    const RenderSettings& settings,
                                                    std::function<void(double)> progress) {
    if (!engine_.project())
        return failure(makeError(ErrorCode::NotFound, "No project is loaded"));
    const time::SamplePos end = engine_.project()->arrangementEndSamples(settings.sampleRate);
    if (settings.renderStems)
        return renderStems(outputPath, 0, end, settings, std::move(progress));
    return renderRange(outputPath, 0, end, settings, std::move(progress));
}

} // namespace aura::engine
