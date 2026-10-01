// ============================================================================
// AURA DAW - transport/Transport.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Centralised transport state: the single source of truth for play/stop/record
// state, playhead position, loop range, metronome and time signature handling.
//
// Threading contract:
//   * Control threads call the command methods (play/stop/seek/setLoopRange...).
//     These only touch atomics - no locks, no allocation - so the UI can fire
//     them at frame rate.
//   * The audio thread calls advance() exactly once per block and reads the
//     resulting state. It is the ONLY writer of position_ while playing.
//   * Position is published as an atomic so the UI can draw the playhead
//     without synchronising with the audio thread.
//
// Recording state machine:
//   Stopped -> CountingIn -> Recording -> (Stopped | Playing)
//   with punch-in/punch-out windows evaluated per block on the audio thread.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

#include "aura/core/Errors.hpp"
#include "aura/time/Time.hpp"

namespace aura::transport {

enum class TransportState : std::int32_t { Stopped = 0, Playing, Paused, Recording, CountingIn };

const char* transportStateName(TransportState state) noexcept;

/// What the transport should do when the loop end is reached.
enum class LoopMode : std::int32_t {
    Off = 0,
    Loop,        ///< jump back to loop start and keep playing
    LoopRecording ///< jump back and keep recording (loop recording takes)
};

struct TransportSettings {
    bool metronomeEnabled = false;
    int metronomeBeatsBeforeStart = 2; ///< count-in bars/beats (in beats)
    bool countInEnabled = false;
    bool punchEnabled = false;
    time::SamplePos punchInPosition = 0;
    time::SamplePos punchOutPosition = 0;
    bool recordArmAutoMonitor = true;
};

class Transport {
public:
    Transport() = default;

    void prepare(double sampleRate) noexcept { sampleRate_ = sampleRate; }

    // --- control-thread commands (lock free) ---------------------------------
    void play() noexcept;
    void stop() noexcept;
    void pause() noexcept;
    void togglePlayStop() noexcept;
    /// Starts playback *and* recording. Returns false when already recording.
    bool record() noexcept;
    void seek(time::SamplePos position) noexcept;
    void seekTicks(std::int64_t ticks, const time::TempoMap& tempo) noexcept;
    void returnToStart() noexcept { seek(startPosition_.load(std::memory_order_relaxed)); }
    void setStartPosition(time::SamplePos position) noexcept {
        startPosition_.store(position < 0 ? 0 : position, std::memory_order_relaxed);
    }

    void setLoopRange(time::SamplePos start, time::SamplePos end) noexcept;
    void clearLoopRange() noexcept;
    void setLoopMode(LoopMode mode) noexcept { loopMode_.store(mode, std::memory_order_relaxed); }
    [[nodiscard]] LoopMode loopMode() const noexcept { return loopMode_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isLooping() const noexcept {
        return loopMode_.load(std::memory_order_relaxed) != LoopMode::Off;
    }

    void setSettings(const TransportSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const TransportSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] TransportSettings& mutableSettings() noexcept { return settings_; }

    // --- audio-thread use ----------------------------------------------------
    /// Advances the playhead by `frames` and resolves loop/punch behaviour.
    /// This is the only place transport state changes on the audio thread.
    /// Returns the number of frames to render before a position jump occurs,
    /// so the engine can render exactly up to the jump point (sample accurate).
    struct AdvanceResult {
        int framesUntilJump = 0;       ///< 0 when no jump inside this block
        time::SamplePos jumpPosition = 0;
        bool startedRecording = false;
    };
    AdvanceResult advance(int frames, const time::TempoMap& tempo) noexcept;

    /// Called by the engine when recording actually begins/ends.
    void notifyRecordingStarted() noexcept;
    void notifyRecordingStopped() noexcept;

    [[nodiscard]] TransportState state() const noexcept {
        return static_cast<TransportState>(state_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool isPlaying() const noexcept {
        const TransportState current = state();
        return current == TransportState::Playing || current == TransportState::Recording ||
               current == TransportState::CountingIn;
    }
    [[nodiscard]] bool isRecording() const noexcept {
        return state() == TransportState::Recording;
    }
    [[nodiscard]] bool isCountingIn() const noexcept {
        return state() == TransportState::CountingIn;
    }
    [[nodiscard]] time::SamplePos position() const noexcept {
        return position_.load(std::memory_order_acquire);
    }
    [[nodiscard]] time::SamplePos loopStart() const noexcept {
        return loopStart_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] time::SamplePos loopEnd() const noexcept {
        return loopEnd_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

    /// Musical position helpers for the UI.
    [[nodiscard]] time::Bbt positionBbt(const time::TempoMap& tempo) const noexcept {
        return tempo.samplesToBbt(position(), sampleRate_);
    }
    [[nodiscard]] std::int64_t positionTicks(const time::TempoMap& tempo) const noexcept {
        return tempo.samplesToTicks(position(), sampleRate_);
    }

    /// True when the transport is inside the punch window.
    [[nodiscard]] bool isInsidePunchWindow() const noexcept;

    /// Count-in state (beats remaining) for the UI.
    [[nodiscard]] int countInBeatsRemaining() const noexcept {
        return countInBeatsRemaining_.load(std::memory_order_relaxed);
    }

private:
    double sampleRate_ = 48000.0;
    TransportSettings settings_{};

    std::atomic<std::int32_t> state_{static_cast<std::int32_t>(TransportState::Stopped)};
    std::atomic<time::SamplePos> position_{0};
    std::atomic<time::SamplePos> startPosition_{0};
    std::atomic<time::SamplePos> loopStart_{0};
    std::atomic<time::SamplePos> loopEnd_{0};
    std::atomic<LoopMode> loopMode_{LoopMode::Off};
    std::atomic<int> countInBeatsRemaining_{0};

    /// Audio-thread only counters (no atomics needed: single writer/reader is
    /// the audio thread itself; the UI reads the published atomics above).
    time::SamplePos countInStartPosition_ = 0;
    bool pendingRecordStart_ = false;
};

} // namespace aura::transport
