// ============================================================================
// AURA DAW - src/audio/AudioClip.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audio/AudioClip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aura::audio {
namespace {

float shapeFade(float x, FadeShape shape) noexcept {
    x = math::clamp01(x);
    switch (shape) {
    case FadeShape::Linear: return x;
    case FadeShape::EqualPower: return std::sin(x * static_cast<float>(math::kHalfPi));
    case FadeShape::Exponential: return x * x;
    case FadeShape::Logarithmic: return std::sqrt(x);
    }
    return x;
}

} // namespace

Clip::Clip(ClipId id, ClipType type, std::string name)
    : id_(id), type_(type), name_(std::move(name)) {}

float Clip::fadeGainAt(time::SamplePos positionInClip) const noexcept {
    float gain = 1.0f;
    if (fadeIn_.length > 0 && positionInClip < fadeIn_.length) {
        gain *= shapeFade(static_cast<float>(positionInClip) / static_cast<float>(fadeIn_.length),
                          fadeIn_.shape);
    }
    if (fadeOut_.length > 0) {
        const time::SamplePos fadeStart = length_ - fadeOut_.length;
        if (positionInClip > fadeStart) {
            const float x = static_cast<float>(length_ - positionInClip) /
                            static_cast<float>(fadeOut_.length);
            gain *= shapeFade(x, fadeOut_.shape);
        }
    }
    return math::clamp01(gain);
}

int Clip::render(time::SamplePos timelinePosition, int frames, float* const* destinations,
                 int channels) noexcept {
    if (muted_ || frames <= 0 || !stream_)
        return 0;

    // Where does this block fall inside the clip?
    const time::SamplePos blockEnd = timelinePosition + frames;
    if (blockEnd <= start_ || timelinePosition >= end())
        return 0;

    const auto writeStart = static_cast<int>(std::max<time::SamplePos>(0, start_ - timelinePosition));
    const int writeFrames =
        static_cast<int>(std::min<time::SamplePos>(frames - writeStart, end() - std::max(start_, timelinePosition)));
    if (writeFrames <= 0)
        return 0;

    // Source position: the clip is a window; loop wraps inside the clip body.
    const time::SamplePos positionInClip = (timelinePosition + writeStart) - start_;
    time::SamplePos sourcePosition =
        sourceOffset_ + (reversed_ ? (length_ - positionInClip - writeFrames) : positionInClip);
    if (sourcePosition < 0)
        sourcePosition = 0;

    // Planar scratch is avoided: read straight into the destination channels.
    std::vector<float*> offsets(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
        offsets[static_cast<std::size_t>(channel)] = destinations[channel] + writeStart;

    stream_->seek(sourcePosition);
    const int read = stream_->read(offsets.data(), writeFrames);
    if (read < writeFrames) {
        // Past the end of the source: silence, never recycled audio.
        for (int channel = 0; channel < channels; ++channel) {
            std::memset(offsets[static_cast<std::size_t>(channel)] + read, 0,
                        static_cast<std::size_t>(writeFrames - read) * sizeof(float));
        }
    }

    // Gain, mute and fades are applied here so the audio thread needs no
    // per-sample state outside this call.
    const float gain = muted_ ? 0.0f : gain_;
    for (int channel = 0; channel < channels; ++channel) {
        float* data = offsets[static_cast<std::size_t>(channel)];
        for (int i = 0; i < writeFrames; ++i) {
            const time::SamplePos clipPosition = positionInClip + i;
            const float fade = fadeGainAt(clipPosition);
            data[i] = math::flushDenormal(data[i] * gain * fade);
        }
    }

    // Loop the clip body when enabled (the engine calls render again).
    if (loop_ && sourcePosition + writeFrames > sourceOffset_ + length_)
        (void)0; // documented: looping is driven by the engine re-rendering

    return writeFrames;
}

// ---------------------------------------------------------------------------
// Clip operations
// ---------------------------------------------------------------------------
namespace clipops {

bool split(Clip& clip, time::SamplePos atPosition, Clip& outSecondHalf, ClipId secondHalfId) {
    if (atPosition <= clip.start() || atPosition >= clip.end())
        return false;

    const time::SamplePos firstLength = atPosition - clip.start();
    const time::SamplePos secondLength = clip.length() - firstLength;

    outSecondHalf = Clip(secondHalfId, clip.type(), clip.name() + " (2)");
    outSecondHalf.setStart(atPosition);
    outSecondHalf.setLength(secondLength);
    outSecondHalf.setSourceOffset(clip.sourceOffset() + firstLength);
    outSecondHalf.setSourceFile(clip.sourceFile());
    outSecondHalf.setStream(clip.stream());
    outSecondHalf.setGain(clip.gain());
    outSecondHalf.setColour(clip.colour());
    // The right half keeps the outgoing fade, the left half the incoming one.
    outSecondHalf.setFadeOut(clip.fadeOut());
    outSecondHalf.setFadeIn(Fade{0, clip.fadeIn().shape});

    clip.setLength(firstLength);
    Fade fadeIn = clip.fadeIn();
    // Keep the fade-in on the first half only if it still fits.
    if (fadeIn.length > firstLength)
        fadeIn.length = firstLength;
    clip.setFadeIn(fadeIn);
    clip.setFadeOut(Fade{0, clip.fadeOut().shape});
    return true;
}

bool trimStart(Clip& clip, time::SamplePos newStart) {
    if (newStart < clip.start() || newStart >= clip.end())
        return false;
    const time::SamplePos delta = newStart - clip.start();
    clip.setSourceOffset(clip.sourceOffset() + delta);
    clip.setLength(clip.length() - delta);
    clip.setStart(newStart);
    return true;
}

bool trimEnd(Clip& clip, time::SamplePos newEnd) {
    if (newEnd <= clip.start() || newEnd > clip.end())
        return false;
    clip.setLength(newEnd - clip.start());
    return true;
}

void move(Clip& clip, time::SamplePos newStart) {
    clip.setStart(newStart);
}

bool slip(Clip& clip, time::SamplePos deltaFrames) {
    const time::SamplePos newOffset = clip.sourceOffset() + deltaFrames;
    if (newOffset < 0)
        return false;
    clip.setSourceOffset(newOffset);
    return true;
}

bool duplicate(const Clip& source, time::SamplePos offset, ClipId newId, Clip& outClip) {
    outClip = Clip(newId, source.type(), source.name() + " copy");
    outClip.setStart(source.start() + offset);
    outClip.setLength(source.length());
    outClip.setSourceOffset(source.sourceOffset());
    outClip.setSourceFile(source.sourceFile());
    outClip.setStream(source.stream());
    outClip.setGain(source.gain());
    outClip.setFadeIn(source.fadeIn());
    outClip.setFadeOut(source.fadeOut());
    outClip.setColour(source.colour());
    outClip.setPitchSemitones(source.pitchSemitones());
    outClip.setStretchRatio(source.stretchRatio());
    return true;
}

void applyGain(Clip& clip, float linearGain) {
    clip.setGain(clip.gain() * linearGain);
}

bool normalise(Clip& clip, float targetDb) {
    const float peak = clip.sourcePeak();
    if (peak <= 1.0e-6f)
        return false; // nothing measured yet, or a silent file
    const float target = math::dbToGain(targetDb);
    clip.setGain(target / peak);
    return true;
}

} // namespace clipops

} // namespace aura::audio
