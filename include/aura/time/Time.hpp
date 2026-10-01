// ============================================================================
// AURA DAW - time/Time.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Musical time model: samples, seconds, beats and bars:beats:ticks (BBT),
// plus SMPTE timecode. Tick resolution is 960 PPQN (a superset-friendly value
// that divides 4/4, 3/4, 6/8 and triplet grids exactly).
//
// The TempoMap is a sorted event list. Lookup is O(log n) binary search which
// is fast enough for both the audio thread (once per block) and the UI (once
// per frame) without any locking, because the audio engine keeps an immutable
// snapshot that is swapped atomically (see engine/TempoMapSnapshot).
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"

namespace aura::time {

inline constexpr std::int64_t kTicksPerQuarterNote = 960;
inline constexpr std::int64_t kTicksPerBeat = kTicksPerQuarterNote; // "beat" == quarter note
inline constexpr std::int64_t kTicksPerBar4_4 = kTicksPerBeat * 4;

/// Transport position expressed in samples at the engine sample rate.
using SamplePos = std::int64_t;

struct Seconds {
    double value = 0.0;
};
struct Beats {
    double value = 0.0;
};
struct Ticks {
    std::int64_t value = 0;
};

/// Bars:beats:ticks position (1-based bars and beats, as musicians see them).
struct Bbt {
    std::int32_t bar = 1;
    std::int32_t beat = 1;
    std::int32_t tick = 0;

    [[nodiscard]] bool operator==(const Bbt& other) const noexcept {
        return bar == other.bar && beat == other.beat && tick == other.tick;
    }
    [[nodiscard]] std::string toString(bool showTicks = true) const;
};

/// SMPTE / timecode.
struct Smpte {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    double subframes = 0.0;

    [[nodiscard]] std::string toString() const;
};

/// Frame rates are enumerated with distinct values (29.97 DF and 30 both count
/// 30 frames in integer terms, which would collide as duplicate case labels).
enum class SmpteFrameRate : std::int32_t {
    Fps24 = 0,
    Fps25 = 1,
    Fps29_97Drop = 2, ///< 30000/1001, drop-frame counting
    Fps30 = 3,
    Fps60 = 4,
};

[[nodiscard]] double frameRateValue(SmpteFrameRate rate) noexcept;
[[nodiscard]] const char* frameRateName(SmpteFrameRate rate) noexcept;

struct TimeSignature {
    std::int32_t numerator = 4;
    std::int32_t denominator = 4;

    [[nodiscard]] bool operator==(const TimeSignature& other) const noexcept {
        return numerator == other.numerator && denominator == other.denominator;
    }
    [[nodiscard]] bool isValid() const noexcept {
        return numerator > 0 && numerator <= 32 && denominator > 0 && denominator <= 32;
    }
};

/// One tempo-map entry. `position` is expressed in ticks so that it survives
/// tempo edits and sample-rate changes (ticks are the canonical time base for
/// the arrangement; samples are derived).
struct TempoEvent {
    std::int64_t positionTicks = 0;
    double bpm = 120.0;
};

struct TimeSignatureEvent {
    std::int64_t positionTicks = 0;
    TimeSignature signature{};
};

/// Piecewise-constant tempo/time-signature map with (documented) limitation:
/// tempo changes are steps, not ramps. Ramps are a planned extension; the
/// lookup API takes the tick position of the *block start*, so adding ramps
/// later only touches this class.
class TempoMap {
public:
    TempoMap();

    void clear();
    void setDefaultTempo(double bpm);
    void setDefaultTimeSignature(TimeSignature signature);

    /// Inserts or replaces the tempo event at exactly `positionTicks`.
    void addTempo(std::int64_t positionTicks, double bpm);
    void removeTempoAt(std::int64_t positionTicks);

    void addTimeSignature(std::int64_t positionTicks, TimeSignature signature);
    void removeTimeSignatureAt(std::int64_t positionTicks);

    [[nodiscard]] double tempoAt(std::int64_t positionTicks) const;
    [[nodiscard]] TimeSignature signatureAt(std::int64_t positionTicks) const;
    [[nodiscard]] const std::vector<TempoEvent>& tempos() const noexcept { return tempos_; }
    [[nodiscard]] const std::vector<TimeSignatureEvent>& timeSignatures() const noexcept {
        return signatures_;
    }

    /// Tempo conversion. All of these are pure functions of the event list.
    [[nodiscard]] std::int64_t ticksToSamples(std::int64_t positionTicks,
                                              double sampleRate) const noexcept;
    [[nodiscard]] std::int64_t samplesToTicks(SamplePos position, double sampleRate) const noexcept;
    [[nodiscard]] double ticksToSeconds(std::int64_t positionTicks) const noexcept;
    [[nodiscard]] std::int64_t secondsToTicks(double seconds) const noexcept;
    [[nodiscard]] double samplesToSeconds(SamplePos position, double sampleRate) const noexcept;
    [[nodiscard]] SamplePos secondsToSamples(double seconds, double sampleRate) const noexcept;

    /// BBT conversion (bar/beat are 1-based; ticks within the beat).
    [[nodiscard]] Bbt ticksToBbt(std::int64_t positionTicks) const noexcept;
    [[nodiscard]] std::int64_t bbtToTicks(const Bbt& bbt) const noexcept;
    [[nodiscard]] Bbt samplesToBbt(SamplePos position, double sampleRate) const noexcept;
    [[nodiscard]] SamplePos bbtToSamples(const Bbt& bbt, double sampleRate) const noexcept;

    /// Number of ticks in a bar at the given position (respects meter changes).
    [[nodiscard]] std::int64_t ticksPerBar(std::int64_t positionTicks) const noexcept;

    /// Musical helpers.
    [[nodiscard]] SamplePos roundToBar(SamplePos position, double sampleRate, bool roundUp = false) const noexcept;
    [[nodiscard]] double secondsPerBeatAt(std::int64_t positionTicks) const noexcept;

    /// Serialization helpers (project format uses these).
    [[nodiscard]] std::string describe() const;

    /// True when the map is the default 120 BPM 4/4 with no user events.
    [[nodiscard]] bool isDefault() const noexcept;

private:
    void sortEvents();

    double defaultTempo_ = 120.0;
    TimeSignature defaultSignature_{4, 4};
    std::vector<TempoEvent> tempos_;
    std::vector<TimeSignatureEvent> signatures_;
};

/// Timecode formatting at a given frame rate; `dropFrame` uses ';' separator.
[[nodiscard]] Smpte secondsToSmpte(double seconds, SmpteFrameRate rate, bool dropFrame = false);
[[nodiscard]] double smpteToSeconds(const Smpte& smpte, SmpteFrameRate rate, bool dropFrame = false);
[[nodiscard]] std::string formatSmpte(double seconds, SmpteFrameRate rate, bool dropFrame = false);

/// Human-readable helpers used by the UI.
[[nodiscard]] std::string formatSeconds(double seconds, int decimals = 3);
[[nodiscard]] std::string formatSamples(SamplePos samples);

/// Time-signature parsing/formatting for project files ("4/4").
[[nodiscard]] std::string timeSignatureToString(const TimeSignature& signature);
[[nodiscard]] bool timeSignatureFromString(const std::string& text, TimeSignature& out) noexcept;

} // namespace aura::time
