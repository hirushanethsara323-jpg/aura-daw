// ============================================================================
// AURA DAW - src/timeline/Time.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/time/Time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "aura/core/Strings.hpp"

namespace aura::time {
namespace {

/// Integer-correct tick -> seconds accumulator walking the tempo segments.
/// Kept double based: at 200 BPM the beat length is 0.3 s, and doubles carry
/// enough precision for one second of music (the sample-rate conversion below
/// rounds to the nearest sample, so long sessions do not drift).
double ticksToSecondsImpl(const std::vector<TempoEvent>& tempos, double defaultTempo,
                          std::int64_t positionTicks) {
    if (positionTicks <= 0)
        return static_cast<double>(positionTicks) / static_cast<double>(kTicksPerBeat) *
               (60.0 / defaultTempo);

    double seconds = 0.0;
    double tempo = defaultTempo;
    std::int64_t cursor = 0;
    for (const auto& event : tempos) {
        if (event.positionTicks >= positionTicks)
            break;
        if (event.positionTicks > cursor) {
            const std::int64_t span = event.positionTicks - cursor;
            seconds += static_cast<double>(span) / static_cast<double>(kTicksPerBeat) *
                       (60.0 / tempo);
            cursor = event.positionTicks;
        }
        tempo = event.bpm > 0.0 ? event.bpm : tempo;
    }
    if (positionTicks > cursor) {
        const std::int64_t span = positionTicks - cursor;
        seconds += static_cast<double>(span) / static_cast<double>(kTicksPerBeat) * (60.0 / tempo);
    }
    return seconds;
}

/// Inverse of ticksToSecondsImpl: converts seconds to ticks walking segments.
std::int64_t secondsToTicksImpl(const std::vector<TempoEvent>& tempos, double defaultTempo,
                                double seconds) {
    if (seconds == 0.0)
        return 0;
    if (seconds < 0.0)
        return -secondsToTicksImpl(tempos, defaultTempo, -seconds);

    double remaining = seconds;
    double tempo = defaultTempo;
    std::int64_t cursorTicks = 0;
    std::size_t index = 0;
    while (true) {
        const std::int64_t nextTicks =
            (index < tempos.size()) ? tempos[index].positionTicks : std::int64_t{1} << 62;
        const double secondsPerTick = (60.0 / tempo) / static_cast<double>(kTicksPerBeat);
        const double segmentSeconds = static_cast<double>(nextTicks - cursorTicks) * secondsPerTick;
        if (std::isfinite(segmentSeconds) && remaining >= segmentSeconds) {
            remaining -= segmentSeconds;
            cursorTicks = nextTicks;
            if (index < tempos.size()) {
                tempo = tempos[index].bpm > 0.0 ? tempos[index].bpm : tempo;
                ++index;
                continue;
            }
            // Past the last event: constant tempo tail.
            cursorTicks += static_cast<std::int64_t>(std::llround(remaining / secondsPerTick));
            return cursorTicks;
        }
        cursorTicks += static_cast<std::int64_t>(std::llround(remaining / secondsPerTick));
        return cursorTicks;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Small value types
// ---------------------------------------------------------------------------
std::string Bbt::toString(bool showTicks) const {
    char buffer[48];
    if (showTicks)
        std::snprintf(buffer, sizeof(buffer), "%d.%d.%03d", bar, beat, tick);
    else
        std::snprintf(buffer, sizeof(buffer), "%d.%d", bar, beat);
    return std::string(buffer);
}

std::string Smpte::toString() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    return std::string(buffer);
}

double frameRateValue(SmpteFrameRate rate) noexcept {
    switch (rate) {
    case SmpteFrameRate::Fps24: return 24.0;
    case SmpteFrameRate::Fps25: return 25.0;
    case SmpteFrameRate::Fps29_97Drop: return 30000.0 / 1001.0;
    case SmpteFrameRate::Fps30: return 30.0;
    case SmpteFrameRate::Fps60: return 60.0;
    }
    return 30.0;
}

const char* frameRateName(SmpteFrameRate rate) noexcept {
    switch (rate) {
    case SmpteFrameRate::Fps24: return "24 fps";
    case SmpteFrameRate::Fps25: return "25 fps";
    case SmpteFrameRate::Fps29_97Drop: return "29.97 DF";
    case SmpteFrameRate::Fps30: return "30 fps";
    case SmpteFrameRate::Fps60: return "60 fps";
    }
    return "30 fps";
}

// ---------------------------------------------------------------------------
// TempoMap
// ---------------------------------------------------------------------------
TempoMap::TempoMap() {
    clear();
}

void TempoMap::clear() {
    defaultTempo_ = 120.0;
    defaultSignature_ = TimeSignature{4, 4};
    tempos_.clear();
    signatures_.clear();
}

void TempoMap::setDefaultTempo(double bpm) {
    defaultTempo_ = std::clamp(bpm, 5.0, 999.0);
}

void TempoMap::setDefaultTimeSignature(TimeSignature signature) {
    if (signature.isValid())
        defaultSignature_ = signature;
}

void TempoMap::addTempo(std::int64_t positionTicks, double bpm) {
    bpm = std::clamp(bpm, 5.0, 999.0);
    if (positionTicks <= 0) {
        defaultTempo_ = bpm;
        return;
    }
    for (auto& event : tempos_) {
        if (event.positionTicks == positionTicks) {
            event.bpm = bpm;
            return;
        }
    }
    tempos_.push_back(TempoEvent{positionTicks, bpm});
    sortEvents();
}

void TempoMap::removeTempoAt(std::int64_t positionTicks) {
    tempos_.erase(std::remove_if(tempos_.begin(), tempos_.end(),
                                 [positionTicks](const TempoEvent& e) {
                                     return e.positionTicks == positionTicks;
                                 }),
                  tempos_.end());
}

void TempoMap::addTimeSignature(std::int64_t positionTicks, TimeSignature signature) {
    if (!signature.isValid())
        return;
    if (positionTicks <= 0) {
        defaultSignature_ = signature;
        return;
    }
    for (auto& event : signatures_) {
        if (event.positionTicks == positionTicks) {
            event.signature = signature;
            return;
        }
    }
    signatures_.push_back(TimeSignatureEvent{positionTicks, signature});
    sortEvents();
}

void TempoMap::removeTimeSignatureAt(std::int64_t positionTicks) {
    signatures_.erase(std::remove_if(signatures_.begin(), signatures_.end(),
                                     [positionTicks](const TimeSignatureEvent& e) {
                                         return e.positionTicks == positionTicks;
                                     }),
                      signatures_.end());
}

void TempoMap::sortEvents() {
    std::stable_sort(tempos_.begin(), tempos_.end(),
                     [](const TempoEvent& a, const TempoEvent& b) {
                         return a.positionTicks < b.positionTicks;
                     });
    std::stable_sort(signatures_.begin(), signatures_.end(),
                     [](const TimeSignatureEvent& a, const TimeSignatureEvent& b) {
                         return a.positionTicks < b.positionTicks;
                     });
}

double TempoMap::tempoAt(std::int64_t positionTicks) const {
    double tempo = defaultTempo_;
    for (const auto& event : tempos_) {
        if (event.positionTicks <= positionTicks)
            tempo = event.bpm;
        else
            break;
    }
    return tempo;
}

TimeSignature TempoMap::signatureAt(std::int64_t positionTicks) const {
    TimeSignature signature = defaultSignature_;
    for (const auto& event : signatures_) {
        if (event.positionTicks <= positionTicks)
            signature = event.signature;
        else
            break;
    }
    return signature;
}

std::int64_t TempoMap::ticksToSamples(std::int64_t positionTicks, double sampleRate) const noexcept {
    const double seconds = ticksToSeconds(positionTicks);
    return static_cast<std::int64_t>(std::llround(seconds * sampleRate));
}

std::int64_t TempoMap::samplesToTicks(SamplePos position, double sampleRate) const noexcept {
    if (sampleRate <= 0.0 || position == 0)
        return 0;
    const double seconds = static_cast<double>(position) / sampleRate;
    return secondsToTicks(seconds);
}

double TempoMap::ticksToSeconds(std::int64_t positionTicks) const noexcept {
    return ticksToSecondsImpl(tempos_, defaultTempo_, positionTicks);
}

std::int64_t TempoMap::secondsToTicks(double seconds) const noexcept {
    return secondsToTicksImpl(tempos_, defaultTempo_, seconds);
}

double TempoMap::samplesToSeconds(SamplePos position, double sampleRate) const noexcept {
    return sampleRate > 0.0 ? static_cast<double>(position) / sampleRate : 0.0;
}

SamplePos TempoMap::secondsToSamples(double seconds, double sampleRate) const noexcept {
    return static_cast<SamplePos>(std::llround(seconds * sampleRate));
}

std::int64_t TempoMap::ticksPerBar(std::int64_t positionTicks) const noexcept {
    const TimeSignature signature = signatureAt(positionTicks);
    // A "beat" in our model is a quarter note; a bar of 6/8 therefore spans
    // 6 * (4/8) = 3 quarter-note beats' worth of ticks.
    const double quarterNotesPerBar = static_cast<double>(signature.numerator) * 4.0 /
                                      static_cast<double>(signature.denominator);
    return static_cast<std::int64_t>(std::llround(quarterNotesPerBar *
                                                  static_cast<double>(kTicksPerBeat)));
}

Bbt TempoMap::ticksToBbt(std::int64_t positionTicks) const noexcept {
    Bbt bbt{};
    std::int64_t remaining = positionTicks;
    std::int64_t barStart = 0;
    std::int64_t bar = 1;

    // Walk meter segments accumulating whole bars. Meter changes are rare, so a
    // linear scan is fine and keeps the maths exact.
    std::size_t index = 0;
    while (true) {
        const std::int64_t segmentEnd =
            (index < signatures_.size()) ? signatures_[index].positionTicks : std::int64_t{1} << 62;
        const TimeSignature signature = signatureAt(barStart);
        const std::int64_t ticksPerBarHere = ticksPerBar(barStart);
        if (ticksPerBarHere <= 0)
            break;
        if (remaining < segmentEnd - barStart || segmentEnd == (std::int64_t{1} << 62)) {
            const std::int64_t bars = remaining / ticksPerBarHere;
            bar += bars;
            remaining -= bars * ticksPerBarHere;
            const double quarterNotesPerBeat =
                4.0 / static_cast<double>(signature.denominator);
            const std::int64_t ticksPerBeatHere = static_cast<std::int64_t>(
                std::llround(quarterNotesPerBeat * static_cast<double>(kTicksPerBeat)));
            std::int32_t beat = 1;
            if (ticksPerBeatHere > 0) {
                beat += static_cast<std::int32_t>(remaining / ticksPerBeatHere);
                remaining %= ticksPerBeatHere;
            }
            bbt.bar = static_cast<std::int32_t>(bar);
            bbt.beat = beat;
            bbt.tick = static_cast<std::int32_t>(std::abs(remaining));
            return bbt;
        }
        // Advance across this meter segment.
        const std::int64_t span = segmentEnd - barStart;
        const std::int64_t wholeBars = span / ticksPerBarHere;
        bar += wholeBars;
        barStart += wholeBars * ticksPerBarHere;
        remaining = positionTicks - barStart;
        ++index;
        if (index > signatures_.size())
            break;
    }
    bbt.bar = static_cast<std::int32_t>(bar);
    bbt.beat = 1;
    bbt.tick = static_cast<std::int32_t>(remaining);
    return bbt;
}

std::int64_t TempoMap::bbtToTicks(const Bbt& bbt) const noexcept {
    if (bbt.bar < 1)
        return 0;
    std::int64_t ticks = 0;
    std::int32_t currentBar = 1;
    std::int64_t barStart = 0;
    std::size_t index = 0;
    while (currentBar < bbt.bar) {
        const std::int64_t segmentEnd =
            (index < signatures_.size()) ? signatures_[index].positionTicks : std::int64_t{1} << 62;
        const std::int64_t ticksPerBarHere = ticksPerBar(barStart);
        if (ticksPerBarHere <= 0)
            return 0;
        if (segmentEnd == (std::int64_t{1} << 62)) {
            const std::int64_t barsToGo = bbt.bar - currentBar;
            ticks = barStart + barsToGo * ticksPerBarHere;
            currentBar = bbt.bar;
            break;
        }
        const std::int64_t barsInSegment = (segmentEnd - barStart) / ticksPerBarHere;
        const std::int64_t barsNeeded = bbt.bar - currentBar;
        if (barsNeeded <= barsInSegment) {
            ticks = barStart + barsNeeded * ticksPerBarHere;
            currentBar = bbt.bar;
            break;
        }
        currentBar += static_cast<std::int32_t>(barsInSegment);
        barStart += barsInSegment * ticksPerBarHere;
        ticks = barStart;
        ++index;
    }
    const TimeSignature signature = signatureAt(ticks);
    const std::int64_t ticksPerBeatHere = static_cast<std::int64_t>(
        std::llround(4.0 / static_cast<double>(signature.denominator) *
                     static_cast<double>(kTicksPerBeat)));
    ticks += static_cast<std::int64_t>(bbt.beat - 1) * ticksPerBeatHere;
    ticks += bbt.tick;
    return ticks < 0 ? 0 : ticks;
}

Bbt TempoMap::samplesToBbt(SamplePos position, double sampleRate) const noexcept {
    return ticksToBbt(samplesToTicks(position, sampleRate));
}

SamplePos TempoMap::bbtToSamples(const Bbt& bbt, double sampleRate) const noexcept {
    return ticksToSamples(bbtToTicks(bbt), sampleRate);
}

SamplePos TempoMap::roundToBar(SamplePos position, double sampleRate, bool roundUp) const noexcept {
    const std::int64_t ticksPerBarHere = ticksPerBar(samplesToTicks(position, sampleRate));
    if (ticksPerBarHere <= 0)
        return position;
    const std::int64_t barIndex =
        samplesToTicks(position, sampleRate) / ticksPerBarHere;
    std::int64_t targetTicks = barIndex * ticksPerBarHere;
    if (roundUp)
        targetTicks += ticksPerBarHere;
    return ticksToSamples(targetTicks, sampleRate);
}

double TempoMap::secondsPerBeatAt(std::int64_t positionTicks) const noexcept {
    return 60.0 / tempoAt(positionTicks);
}

std::string TempoMap::describe() const {
    std::string out = "TempoMap{default=" + strings::fromDouble(defaultTempo_, 3) + "bpm " +
                      timeSignatureToString(defaultSignature_) + ", tempos=" +
                      std::to_string(tempos_.size()) + ", meters=" +
                      std::to_string(signatures_.size()) + "}";
    return out;
}

bool TempoMap::isDefault() const noexcept {
    return tempos_.empty() && signatures_.empty() && std::abs(defaultTempo_ - 120.0) < 1e-9 &&
           defaultSignature_ == TimeSignature{4, 4};
}

// ---------------------------------------------------------------------------
// SMPTE + formatting
// ---------------------------------------------------------------------------
Smpte secondsToSmpte(double seconds, SmpteFrameRate rate, bool dropFrame) {
    Smpte smpte{};
    const double fps = frameRateValue(rate);
    if (seconds < 0.0)
        seconds = 0.0;

    if (dropFrame && rate == SmpteFrameRate::Fps29_97Drop) {
        // Drop-frame: skip frame numbers 0 and 1 of every minute except every
        // 10th minute, using the standard 29.97 counting scheme.
        const double totalFramesExact = seconds * fps;
        std::int64_t frameNumber = static_cast<std::int64_t>(totalFramesExact);
        const std::int64_t framesPer10Min = 17982; // 30*60*10 - 18
        const std::int64_t framesPerMin = 1798;    // 30*60 - 2
        const std::int64_t tenMinutes = frameNumber / framesPer10Min;
        std::int64_t remainder = frameNumber % framesPer10Min;
        std::int64_t droppedFrames = 18 * tenMinutes;
        if (remainder >= 2) {
            droppedFrames += 2 * ((remainder - 2) / framesPerMin);
        }
        frameNumber += droppedFrames;
        const std::int64_t frames = frameNumber % 30;
        const std::int64_t totalSeconds = frameNumber / 30;
        smpte.hours = static_cast<int>(totalSeconds / 3600);
        smpte.minutes = static_cast<int>((totalSeconds / 60) % 60);
        smpte.seconds = static_cast<int>(totalSeconds % 60);
        smpte.frames = static_cast<int>(frames);
        smpte.subframes = totalFramesExact - std::floor(totalFramesExact);
        return smpte;
    }

    const double totalFramesExact = seconds * fps;
    const std::int64_t frameNumber = static_cast<std::int64_t>(totalFramesExact);
    const std::int64_t framesPerSecond = static_cast<std::int64_t>(std::llround(fps));
    smpte.frames = static_cast<int>(frameNumber % framesPerSecond);
    const std::int64_t totalSeconds = frameNumber / framesPerSecond;
    smpte.hours = static_cast<int>(totalSeconds / 3600);
    smpte.minutes = static_cast<int>((totalSeconds / 60) % 60);
    smpte.seconds = static_cast<int>(totalSeconds % 60);
    smpte.subframes = totalFramesExact - std::floor(totalFramesExact);
    return smpte;
}

double smpteToSeconds(const Smpte& smpte, SmpteFrameRate rate, bool dropFrame) {
    const double fps = frameRateValue(rate);
    if (dropFrame && rate == SmpteFrameRate::Fps29_97Drop) {
        const std::int64_t totalMinutes = static_cast<std::int64_t>(smpte.hours) * 60 + smpte.minutes;
        const std::int64_t frameNumber = ((totalMinutes * 60 + smpte.seconds) * 30 + smpte.frames);
        const std::int64_t droppedFrames = 2 * (totalMinutes - totalMinutes / 10);
        return static_cast<double>(frameNumber + droppedFrames) / fps;
    }
    const std::int64_t framesPerSecond = static_cast<std::int64_t>(std::llround(fps));
    const std::int64_t totalFrames =
        ((static_cast<std::int64_t>(smpte.hours) * 3600 + smpte.minutes * 60 + smpte.seconds) *
             framesPerSecond +
         smpte.frames);
    return static_cast<double>(totalFrames) / fps;
}

std::string formatSmpte(double seconds, SmpteFrameRate rate, bool dropFrame) {
    const Smpte smpte = secondsToSmpte(seconds, rate, dropFrame);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d%c%02d", smpte.hours, smpte.minutes,
                  smpte.seconds, dropFrame ? ';' : ':', smpte.frames);
    return std::string(buffer);
}

std::string formatSeconds(double seconds, int decimals) {
    const bool negative = seconds < 0.0;
    seconds = std::abs(seconds);
    const int minutes = static_cast<int>(seconds / 60.0);
    const double remainder = seconds - minutes * 60.0;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s%d:%0*.*f", negative ? "-" : "", minutes,
                  3 + (decimals > 0 ? decimals + 1 : 0), decimals, remainder);
    return std::string(buffer);
}

std::string formatSamples(SamplePos samples) {
    return std::to_string(samples);
}

std::string timeSignatureToString(const TimeSignature& signature) {
    return std::to_string(signature.numerator) + "/" + std::to_string(signature.denominator);
}

bool timeSignatureFromString(const std::string& text, TimeSignature& out) noexcept {
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos)
        return false;
    long long numerator = 0;
    long long denominator = 0;
    if (!strings::toInt(text.substr(0, slash), numerator))
        return false;
    if (!strings::toInt(text.substr(slash + 1), denominator))
        return false;
    TimeSignature signature{static_cast<std::int32_t>(numerator),
                            static_cast<std::int32_t>(denominator)};
    if (!signature.isValid())
        return false;
    out = signature;
    return true;
}

} // namespace aura::time
