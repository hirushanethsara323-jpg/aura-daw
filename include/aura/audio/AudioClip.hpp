// ============================================================================
// AURA DAW - audio/AudioClip.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Non-destructive audio clips plus the arrangement operations the editor needs.
//
// A clip is a *window* onto a source stream:
//
//   source file:  [..............................................]
//   clip:                    offset
//                            |<---- length ---->|
//   timeline:      start ->
//
// The audio thread resolves `start` -> `offset` and reads sequentially from the
// clip's SampleStream; nothing is ever destructively modified. Splitting a clip
// creates two windows; trimming changes start/length/offset; fades are applied
// by the clip's own envelope. All of it serialises as numbers.
//
// Documented limitation (V1): time-stretching and pitch-shifting are modelled in
// the clip (stretchRatio/pitchSemitones) and serialised, but the DSP for them is
// NOT implemented - the engine renders at ratio 1 and reports this honestly
// through Clip::isStretchActive() so the UI can mark the clip as unsupported.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/graph/SampleStream.hpp"
#include "aura/time/Time.hpp"

namespace aura::audio {

using ClipId = std::uint64_t;

enum class ClipType : std::int32_t { Audio = 0, Midi };

enum class FadeShape : std::int32_t {
    Linear = 0,
    EqualPower,
    Exponential,
    Logarithmic,
};

struct Fade {
    time::SamplePos length = 0; ///< fade length in samples
    FadeShape shape = FadeShape::EqualPower;
};

/// The widest clip `Clip::render()` will fill, in channels.
///
/// `render()` runs on the audio thread and needs an array of per-channel write
/// pointers. Allocating one per block is exactly what the audio thread must not do,
/// so the array is on the stack and this constant is its length. 64 is the same
/// channel budget the M7 sandbox transport carries (`plugin::sandbox::kMaxChannels`),
/// so a clip can never be wider than the path that would have to move it. A clip
/// asking for more renders as silence and trips an RT assert rather than overflowing
/// the scratch - see `Clip::render()`.
inline constexpr int kMaxClipChannels = 64;

/// Non-destructive audio clip.
class Clip {
public:
    Clip(ClipId id, ClipType type, std::string name);

    [[nodiscard]] ClipId id() const noexcept { return id_; }
    [[nodiscard]] ClipType type() const noexcept { return type_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    // --- arrangement ---------------------------------------------------------
    void setStart(time::SamplePos start) noexcept { start_ = start < 0 ? 0 : start; }
    [[nodiscard]] time::SamplePos start() const noexcept { return start_; }
    void setLength(time::SamplePos length) noexcept { length_ = length < 1 ? 1 : length; }
    [[nodiscard]] time::SamplePos length() const noexcept { return length_; }
    [[nodiscard]] time::SamplePos end() const noexcept { return start_ + length_; }

    /// Offset into the source file (in source frames).
    void setSourceOffset(time::SamplePos offset) noexcept { sourceOffset_ = offset < 0 ? 0 : offset; }
    [[nodiscard]] time::SamplePos sourceOffset() const noexcept { return sourceOffset_; }

    void setSourceFile(std::string path) { sourcePath_ = std::move(path); }
    [[nodiscard]] const std::string& sourceFile() const noexcept { return sourcePath_; }

    // --- mixer / editing state ----------------------------------------------
    void setGain(float linear) noexcept { gain_ = linear; }
    [[nodiscard]] float gain() const noexcept { return gain_; }
    void setGainDb(float db) noexcept { gain_ = db <= -96.0f ? 0.0f : math::dbToGain(db); }
    [[nodiscard]] float gainDb() const noexcept { return math::gainToDb(gain_); }
    void setMuted(bool muted) noexcept { muted_ = muted; }
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setLocked(bool locked) noexcept { locked_ = locked; }
    [[nodiscard]] bool isLocked() const noexcept { return locked_; }
    void setSelected(bool selected) noexcept { selected_ = selected; }
    [[nodiscard]] bool isSelected() const noexcept { return selected_; }
    void setColour(std::uint32_t argb) noexcept { colour_ = argb; }
    [[nodiscard]] std::uint32_t colour() const noexcept { return colour_; }
    void setLoopEnabled(bool loop) noexcept { loop_ = loop; }
    [[nodiscard]] bool isLoopEnabled() const noexcept { return loop_; }
    /// Playback rate for the clip (1.0 = original). Values != 1 require the
    /// (unimplemented) stretch engine; the engine reports them as unsupported.
    void setStretchRatio(float ratio) noexcept { stretchRatio_ = math::clamp(ratio, 0.25f, 4.0f); }
    [[nodiscard]] float stretchRatio() const noexcept { return stretchRatio_; }
    void setPitchSemitones(float semitones) noexcept { pitchSemitones_ = math::clamp(semitones, -24.0f, 24.0f); }
    [[nodiscard]] float pitchSemitones() const noexcept { return pitchSemitones_; }
    void setReversed(bool reversed) noexcept { reversed_ = reversed; }
    [[nodiscard]] bool isReversed() const noexcept { return reversed_; }

    [[nodiscard]] bool isStretchActive() const noexcept {
        return std::abs(stretchRatio_ - 1.0f) > 1.0e-4f || std::abs(pitchSemitones_) > 1.0e-4f;
    }

    // --- fades ---------------------------------------------------------------
    void setFadeIn(Fade fade) noexcept { fadeIn_ = fade; }
    void setFadeOut(Fade fade) noexcept { fadeOut_ = fade; }
    [[nodiscard]] const Fade& fadeIn() const noexcept { return fadeIn_; }
    [[nodiscard]] const Fade& fadeOut() const noexcept { return fadeOut_; }

    /// Fade gain for a position relative to the clip start, in samples.
    [[nodiscard]] float fadeGainAt(time::SamplePos positionInClip) const noexcept;

    // --- playback ------------------------------------------------------------
    /// Binds the stream that supplies samples (owned elsewhere: the media pool).
    void setStream(std::shared_ptr<graph::SampleStream> stream) { stream_ = std::move(stream); }
    [[nodiscard]] const std::shared_ptr<graph::SampleStream>& stream() const noexcept {
        return stream_;
    }
    [[nodiscard]] bool isReady() const noexcept { return stream_ != nullptr; }

    /// Renders `frames` of this clip into planar destinations, honouring gain,
    /// mute, fades and loop. `timelinePosition` is the absolute transport
    /// position of the first frame. Real-time safe.
    /// Returns frames actually written (0 when the playhead is outside the clip).
    int render(time::SamplePos timelinePosition, int frames, float* const* destinations,
               int channels) noexcept;

    /// Peak level of the clip's *source* (for normalisation / UI display).
    /// Filled by the waveform cache; -1 means "not measured yet".
    void setSourcePeak(float peak) noexcept { sourcePeak_ = peak; }
    [[nodiscard]] float sourcePeak() const noexcept { return sourcePeak_; }

private:
    ClipId id_ = 0;
    ClipType type_ = ClipType::Audio;
    std::string name_;
    std::string sourcePath_;

    time::SamplePos start_ = 0;
    time::SamplePos length_ = 48000;
    time::SamplePos sourceOffset_ = 0;

    float gain_ = 1.0f;
    float sourcePeak_ = -1.0f;
    float stretchRatio_ = 1.0f;
    float pitchSemitones_ = 0.0f;
    bool muted_ = false;
    bool locked_ = false;
    bool selected_ = false;
    bool loop_ = false;
    bool reversed_ = false;
    std::uint32_t colour_ = 0xFF2563EBu;

    Fade fadeIn_{};
    Fade fadeOut_{};
    std::shared_ptr<graph::SampleStream> stream_;
};

/// Arrangement-level clip operations. Each returns the resulting clips so the
/// command layer can build an undo record from them (see commands/UndoHistory).
namespace clipops {

/// Splits `clip` at an absolute timeline position. Returns false when the
/// position is outside the clip body (a split exactly on a boundary is a no-op).
[[nodiscard]] bool split(Clip& clip, time::SamplePos atPosition, Clip& outSecondHalf,
                         ClipId secondHalfId);

/// Trims the clip's left edge to `newStart` (adjusting source offset).
[[nodiscard]] bool trimStart(Clip& clip, time::SamplePos newStart);

/// Trims the clip's right edge to `newEnd`.
[[nodiscard]] bool trimEnd(Clip& clip, time::SamplePos newEnd);

/// Moves the clip in time (source offset unchanged).
void move(Clip& clip, time::SamplePos newStart);

/// Slip edit: moves the source window inside the clip without moving the clip.
[[nodiscard]] bool slip(Clip& clip, time::SamplePos deltaFrames);

/// Duplicates the clip, offset in time.
[[nodiscard]] bool duplicate(const Clip& source, time::SamplePos offset, ClipId newId,
                             Clip& outClip);

/// Applies a gain change (used by Normalize).
void applyGain(Clip& clip, float linearGain);

/// Normalise the clip's gain so its source peak reaches `targetDb`.
/// Requires a measured source peak (waveform cache).
[[nodiscard]] bool normalise(Clip& clip, float targetDb);

} // namespace clipops

} // namespace aura::audio
