// ============================================================================
// AURA DAW - src/transport/Transport.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/transport/Transport.hpp"

#include <algorithm>

namespace aura::transport {

const char* transportStateName(TransportState state) noexcept {
    switch (state) {
    case TransportState::Stopped: return "Stopped";
    case TransportState::Playing: return "Playing";
    case TransportState::Paused: return "Paused";
    case TransportState::Recording: return "Recording";
    case TransportState::CountingIn: return "Count-in";
    }
    return "?";
}

void Transport::play() noexcept {
    const auto current = state();
    if (current == TransportState::Playing || current == TransportState::Recording)
        return;
    if (settings_.countInEnabled && current != TransportState::Paused) {
        countInStartPosition_ = position_.load(std::memory_order_relaxed);
        countInBeatsRemaining_.store(std::max(1, settings_.metronomeBeatsBeforeStart),
                                     std::memory_order_relaxed);
        state_.store(static_cast<std::int32_t>(TransportState::CountingIn), std::memory_order_release);
        return;
    }
    state_.store(static_cast<std::int32_t>(TransportState::Playing), std::memory_order_release);
}

void Transport::stop() noexcept {
    state_.store(static_cast<std::int32_t>(TransportState::Stopped), std::memory_order_release);
    position_.store(startPosition_.load(std::memory_order_relaxed), std::memory_order_release);
    countInBeatsRemaining_.store(0, std::memory_order_relaxed);
    pendingRecordStart_ = false;
}

void Transport::pause() noexcept {
    const auto current = state();
    if (current == TransportState::Stopped)
        return;
    // Recording does not survive a pause: the take is finalised (the engine
    // closes the writer) and the transport drops into Paused.
    state_.store(static_cast<std::int32_t>(TransportState::Paused), std::memory_order_release);
    countInBeatsRemaining_.store(0, std::memory_order_relaxed);
}

void Transport::togglePlayStop() noexcept {
    if (isPlaying())
        stop();
    else
        play();
}

bool Transport::record() noexcept {
    const auto current = state();
    if (current == TransportState::Recording)
        return false;
    pendingRecordStart_ = true;
    if (settings_.countInEnabled) {
        countInStartPosition_ = position_.load(std::memory_order_relaxed);
        countInBeatsRemaining_.store(std::max(1, settings_.metronomeBeatsBeforeStart),
                                     std::memory_order_relaxed);
        state_.store(static_cast<std::int32_t>(TransportState::CountingIn), std::memory_order_release);
    } else {
        state_.store(static_cast<std::int32_t>(TransportState::Recording), std::memory_order_release);
    }
    return true;
}

void Transport::seek(time::SamplePos position) noexcept {
    position_.store(position < 0 ? 0 : position, std::memory_order_release);
}

void Transport::seekTicks(std::int64_t ticks, const time::TempoMap& tempo) noexcept {
    seek(tempo.ticksToSamples(ticks, sampleRate_));
}

void Transport::setLoopRange(time::SamplePos start, time::SamplePos end) noexcept {
    if (start < 0)
        start = 0;
    if (end <= start) {
        clearLoopRange();
        return;
    }
    loopStart_.store(start, std::memory_order_relaxed);
    loopEnd_.store(end, std::memory_order_relaxed);
    if (loopMode_.load(std::memory_order_relaxed) == LoopMode::Off)
        loopMode_.store(LoopMode::Loop, std::memory_order_relaxed);
}

void Transport::clearLoopRange() noexcept {
    loopStart_.store(0, std::memory_order_relaxed);
    loopEnd_.store(0, std::memory_order_relaxed);
    loopMode_.store(LoopMode::Off, std::memory_order_relaxed);
}

bool Transport::isInsidePunchWindow() const noexcept {
    if (!settings_.punchEnabled)
        return true; // no punch window configured: record everywhere
    const time::SamplePos position = position_.load(std::memory_order_relaxed);
    return position >= settings_.punchInPosition && position < settings_.punchOutPosition;
}

Transport::AdvanceResult Transport::advance(int frames, const time::TempoMap& tempo) noexcept {
    AdvanceResult result;
    if (frames <= 0)
        return result;

    const auto current = state();
    if (current == TransportState::Stopped || current == TransportState::Paused)
        return result;

    time::SamplePos position = position_.load(std::memory_order_relaxed);

    // --- count-in -----------------------------------------------------------
    if (current == TransportState::CountingIn) {
        // Count-in advances the playhead musically so the metronome is in time
        // and the recording starts exactly on the downbeat.
        const double secondsPerBeat = tempo.secondsPerBeatAt(tempo.samplesToTicks(position, sampleRate_));
        const auto framesPerBeat = static_cast<time::SamplePos>(secondsPerBeat * sampleRate_);
        const time::SamplePos countInEnd = countInStartPosition_ + framesPerBeat *
                                                                      std::max(1, settings_.metronomeBeatsBeforeStart);
        position += frames;
        if (position >= countInEnd) {
            // Count-in finished inside this block: report the exact jump so the
            // engine can render the tail of the block in the new state.
            result.framesUntilJump = static_cast<int>(countInEnd - (position - frames));
            result.jumpPosition = countInEnd;
            position = countInEnd;
            countInBeatsRemaining_.store(0, std::memory_order_relaxed);
            state_.store(static_cast<std::int32_t>(
                             pendingRecordStart_ ? TransportState::Recording : TransportState::Playing),
                         std::memory_order_release);
            result.startedRecording = pendingRecordStart_;
            position_.store(position, std::memory_order_release);
            return result;
        }
        const auto remainingFrames = countInEnd - position;
        const int remainingBeats = static_cast<int>((remainingFrames + framesPerBeat - 1) / std::max<time::SamplePos>(1, framesPerBeat));
        countInBeatsRemaining_.store(remainingBeats, std::memory_order_relaxed);
        position_.store(position, std::memory_order_release);
        return result;
    }

    // --- punch-out ----------------------------------------------------------
    if (current == TransportState::Recording && settings_.punchEnabled) {
        const time::SamplePos punchOut = settings_.punchOutPosition;
        if (position < punchOut && position + frames > punchOut) {
            result.framesUntilJump = static_cast<int>(punchOut - position);
            result.jumpPosition = punchOut;
            position = punchOut;
        }
    }

    // --- loop wrap ----------------------------------------------------------
    const LoopMode loopMode = loopMode_.load(std::memory_order_relaxed);
    if (loopMode != LoopMode::Off) {
        const time::SamplePos loopStart = loopStart_.load(std::memory_order_relaxed);
        const time::SamplePos loopEnd = loopEnd_.load(std::memory_order_relaxed);
        if (loopEnd > loopStart) {
            const time::SamplePos newPosition = position + frames;
            if (newPosition >= loopEnd) {
                const int framesBeforeWrap = static_cast<int>(loopEnd - position);
                if (result.framesUntilJump == 0 || framesBeforeWrap < result.framesUntilJump) {
                    result.framesUntilJump = framesBeforeWrap;
                    result.jumpPosition = loopStart;
                }
                position = loopStart + (newPosition - loopEnd);
                position_.store(position, std::memory_order_release);
                return result;
            }
        }
    }

    position_.store(position + frames, std::memory_order_release);
    return result;
}

void Transport::notifyRecordingStarted() noexcept {
    state_.store(static_cast<std::int32_t>(TransportState::Recording), std::memory_order_release);
    pendingRecordStart_ = false;
}

void Transport::notifyRecordingStopped() noexcept {
    pendingRecordStart_ = false;
    if (state() == TransportState::Recording)
        state_.store(static_cast<std::int32_t>(TransportState::Playing), std::memory_order_release);
}

} // namespace aura::transport
