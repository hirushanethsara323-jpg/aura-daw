// ============================================================================
// AURA DAW - src/audio/Mixer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audio/Mixer.hpp"

#include <algorithm>

#include "aura/core/Log.hpp"

namespace aura::audio {
namespace {
constexpr const char* kCategory = "Audio.Mixer";
}

Mixer::Mixer() {
    master_ = std::make_unique<Track>(0, TrackKind::Master, "Master");
    master_->setGraphNodeId(graph::kInvalidNodeId);
}

TrackId Mixer::createTrack(TrackKind kind, std::string name) {
    const TrackId id = nextTrackId_++;
    auto track = std::make_unique<Track>(id, kind, std::move(name));

    // A new bus/aux track prefers the master as its output; audio/instrument
    // tracks default to the master too until the user routes them.
    track->setOutput(master_->id());
    ownedTracks_.push_back(std::move(track));
    Track* raw = ownedTracks_.back().get();
    tracks_.push_back(raw);

    meters_.push_back({id, std::make_unique<dsp::LevelMeter>()});
    meters_.back().second->prepare(sampleRate_);
    meters_.back().second->setPeakHoldSeconds(1.0f);

    AURA_LOG_DEBUG(kCategory, "Created %s track '%s' (id %u) - total %zu tracks",
                   trackKindName(kind), raw->name().c_str(), id, tracks_.size());
    (void)resolveSoloState();
    return id;
}

bool Mixer::removeTrack(TrackId id) {
    if (id == master_->id())
        return false; // the master strip is not removable

    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [id](const Track* track) { return track && track->id() == id; });
    if (it == tracks_.end())
        return false;
    tracks_.erase(it);

    // Re-link references: any send or routing pointing at the removed track
    // falls back to the master rather than dangling.
    for (Track* track : tracks_) {
        auto& sends = track->sends();
        sends.erase(std::remove_if(sends.begin(), sends.end(),
                                   [id](const Send& send) { return send.destination == id; }),
                    sends.end());
        if (track->routing().destination == id)
            track->routing().destination = master_->id();
    }

    ownedTracks_.erase(std::remove_if(ownedTracks_.begin(), ownedTracks_.end(),
                                      [id](const std::unique_ptr<Track>& track) {
                                          return track && track->id() == id;
                                      }),
                       ownedTracks_.end());
    meters_.erase(std::remove_if(meters_.begin(), meters_.end(),
                                 [id](const auto& entry) { return entry.first == id; }),
                  meters_.end());

    (void)resolveSoloState();
    return true;
}

bool Mixer::moveTrack(TrackId id, std::size_t newIndex) {
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [id](const Track* track) { return track && track->id() == id; });
    if (it == tracks_.end())
        return false;
    Track* track = *it;
    tracks_.erase(it);
    newIndex = std::min(newIndex, tracks_.size());
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(newIndex), track);
    return true;
}

Track* Mixer::findTrack(TrackId id) noexcept {
    if (id == master_->id())
        return master_.get();
    for (Track* track : tracks_) {
        if (track && track->id() == id)
            return track;
    }
    return nullptr;
}

const Track* Mixer::findTrack(TrackId id) const noexcept {
    return const_cast<Mixer*>(this)->findTrack(id);
}

Track* Mixer::trackAt(std::size_t index) noexcept {
    return index < tracks_.size() ? tracks_[index] : nullptr;
}

bool Mixer::anySoloed() const noexcept {
    for (const Track* track : tracks_) {
        if (track && track->isSoloed())
            return true;
    }
    return false;
}

bool Mixer::resolveSoloState() {
    const bool soloed = anySoloed();
    bool changed = false;

    if (!soloed) {
        for (Track* track : tracks_) {
            if (track && track->isSoloedOut()) {
                track->setSoloedOut(false);
                changed = true;
            }
        }
        if (master_->isSoloedOut()) {
            master_->setSoloedOut(false);
            changed = true;
        }
        return changed;
    }

    // A track is audible when it (or something feeding it) is soloed. Any track
    // is "solo safe" when it is an ancestor of a soloed track: soloing a vocal
    // must not silence the reverb aux it sends to, nor the bus it feeds.
    auto feedsSoloedPath = [this](TrackId id) {
        // Walk forward: does `id` reach any soloed track or the master?
        std::vector<TrackId> stack{id};
        std::vector<TrackId> visited;
        while (!stack.empty()) {
            const TrackId current = stack.back();
            stack.pop_back();
            if (std::find(visited.begin(), visited.end(), current) != visited.end())
                continue;
            visited.push_back(current);
            if (const Track* track = findTrack(current)) {
                if (track->isSoloed())
                    return true;
                for (const auto& send : track->sends()) {
                    if (send.enabled)
                        stack.push_back(send.destination);
                }
                if (track->routing().destination != kInvalidTrackId)
                    stack.push_back(track->routing().destination);
            }
        }
        return false;
    };

    for (Track* track : tracks_) {
        if (!track)
            continue;
        // Tracks that are soloed stay on; buses/aux that carry a soloed child
        // stay on (solo-safe); everything else is muted by the solo logic.
        const bool keep = track->isSoloed() || track->kind() == TrackKind::Bus ||
                          track->kind() == TrackKind::Aux || feedsSoloedPath(track->id());
        const bool soloedOut = !keep;
        if (track->isSoloedOut() != soloedOut) {
            track->setSoloedOut(soloedOut);
            changed = true;
        }
    }
    return changed;
}

dsp::LevelMeter& Mixer::meterForTrack(TrackId id) {
    for (auto& entry : meters_) {
        if (entry.first == id)
            return *entry.second;
    }
    // The master meter doubles as the fallback so the UI always has something
    // valid to draw (never a dangling reference).
    return masterMeter_;
}

int Mixer::maximumInsertLatencySamples() const noexcept {
    int maximum = 0;
    for (const Track* track : tracks_) {
        if (track)
            maximum = std::max(maximum, track->totalInsertLatencySamples());
    }
    maximum = std::max(maximum, master_->totalInsertLatencySamples());
    return maximum;
}

void Mixer::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_ = numChannels;
    masterMeter_.prepare(sampleRate);
    masterMeter_.setPeakHoldSeconds(1.0f);
    masterLoudness_.prepare(sampleRate, numChannels);
    masterLimiter_.prepare(sampleRate, maxBlockSize, numChannels);
    for (auto& entry : meters_) {
        entry.second->prepare(sampleRate);
        entry.second->setPeakHoldSeconds(1.0f);
    }
    // Master strip defaults: limiter engaged at -0.3 dBFS, the safe default the
    // brief asks for ("master: stereo meter, peak, RMS, LUFS, limiter, gain").
    dsp::LimiterSettings limiterSettings;
    limiterSettings.ceilingDb = -0.3f;
    limiterSettings.lookaheadMs = 1.5f;
    masterLimiter_.setSettings(limiterSettings);
    // prepare() runs many times in a session (project load, device change,
    // sample-rate change, rebuildGraph). The strip insert must be registered
    // exactly once: duplicates would stack the limiter's latency and process the
    // master five times over.
    bool limiterInserted = false;
    for (const auto& insert : master_->inserts()) {
        if (insert.processor == &masterLimiter_)
            limiterInserted = true;
    }
    if (!limiterInserted) {
        // The safety limiter is the last line of defence against a clipped master,
        // so if the chain is somehow already full the failure must be visible
        // rather than silently dropped.
        if (!master_->addInsert({"Master Limiter", &masterLimiter_, false, "", 1.0f}))
            AURA_LOG_ERROR(kCategory, "Master insert chain is full; safety limiter not inserted");
    }
}

void Mixer::reset() noexcept {
    masterMeter_.reset();
    masterLoudness_.reset();
    masterLimiter_.reset();
    for (auto& entry : meters_)
        entry.second->reset();
}

std::vector<Track*> Mixer::allTracksIncludingMaster() noexcept {
    std::vector<Track*> all = tracks_;
    all.push_back(master_.get());
    return all;
}

} // namespace aura::audio
