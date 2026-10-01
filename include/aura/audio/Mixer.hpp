// ============================================================================
// AURA DAW - audio/Mixer.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mixer owns the track collection and implements the console rules that
// must not live in the UI:
//   * solo resolution (including "solo safe" for buses that carry a soloed
//     child, so soloing a track does not silence the bus it feeds)
//   * fader/pan/mute application order
//   * master strip with limiter and metering
//   * latency reporting for delay compensation
//
// Everything here is control-thread work that produces (a) atomics the audio
// thread reads and (b) a GraphPlan it processes. The audio thread never walks
// this class.
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "aura/audio/Track.hpp"
#include "aura/dsp/LevelMeter.hpp"
#include "aura/dsp/Limiter.hpp"
#include "aura/dsp/Loudness.hpp"

namespace aura::audio {

class Mixer {
public:
    Mixer();

    /// Creates a track and returns its id. Track order = mixer channel order.
    TrackId createTrack(TrackKind kind, std::string name);
    /// Removes a track and re-links any sends/routing that referenced it.
    bool removeTrack(TrackId id);
    /// Reorders a track within the mixer (UI drag of a channel).
    bool moveTrack(TrackId id, std::size_t newIndex);

    [[nodiscard]] Track* findTrack(TrackId id) noexcept;
    [[nodiscard]] const Track* findTrack(TrackId id) const noexcept;
    [[nodiscard]] Track* trackAt(std::size_t index) noexcept;
    [[nodiscard]] std::vector<Track*>& tracks() noexcept { return tracks_; }
    [[nodiscard]] const std::vector<Track*>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] std::size_t trackCount() const noexcept { return tracks_.size(); }

    [[nodiscard]] Track& master() noexcept { return *master_; }
    [[nodiscard]] const Track& master() const noexcept { return *master_; }
    [[nodiscard]] TrackId masterId() const noexcept { return master_->id(); }

    /// Recomputes `soloedOut` flags for every track. Must be called after any
    /// solo/mute/routing change. Returns true when anything changed (so the
    /// engine knows it needs to publish a new snapshot).
    bool resolveSoloState();

    /// True when at least one track is soloed.
    [[nodiscard]] bool anySoloed() const noexcept;

    /// Metering: the engine feeds these from the audio thread; the UI reads
    /// snapshots (lock-free, see dsp::LevelMeter).
    [[nodiscard]] dsp::LevelMeter& meterForTrack(TrackId id);
    [[nodiscard]] dsp::LevelMeter& masterMeter() noexcept { return masterMeter_; }
    [[nodiscard]] dsp::LoudnessMeter& masterLoudness() noexcept { return masterLoudness_; }
    [[nodiscard]] dsp::Limiter& masterLimiter() noexcept { return masterLimiter_; }

    /// Total latency (samples) the mixer introduces on the master path, used
    /// for delay compensation and for the reported round-trip latency.
    [[nodiscard]] int masterLatencySamples() const noexcept {
        return masterLimiter_.latencySamples();
    }

    /// Maximum insert latency across all tracks: the engine compensates every
    /// path up to this value so parallel paths stay phase aligned.
    [[nodiscard]] int maximumInsertLatencySamples() const noexcept;

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset() noexcept;

    /// Serialization support: stable iteration order over all tracks including
    /// the master (master last).
    [[nodiscard]] std::vector<Track*> allTracksIncludingMaster() noexcept;

private:
    std::vector<Track*> tracks_; ///< owned via `ownedTracks_`
    std::vector<std::unique_ptr<Track>> ownedTracks_;
    std::unique_ptr<Track> master_;
    TrackId nextTrackId_ = 1;

    std::vector<std::pair<TrackId, std::unique_ptr<dsp::LevelMeter>>> meters_;
    dsp::LevelMeter masterMeter_;
    dsp::LoudnessMeter masterLoudness_;
    dsp::Limiter masterLimiter_;
    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    int numChannels_ = 2;
};

} // namespace aura::audio
