// ============================================================================
// AURA DAW - src/audio/Track.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audio/Track.hpp"

#include <algorithm>
#include <cmath>

#include "aura/core/Log.hpp"

namespace aura::audio {
namespace {
constexpr const char* kCategory = "Audio.Track";
} // namespace

const char* trackKindName(TrackKind kind) noexcept {
    switch (kind) {
    case TrackKind::Audio: return "Audio";
    case TrackKind::Midi: return "MIDI";
    case TrackKind::Instrument: return "Instrument";
    case TrackKind::Bus: return "Bus";
    case TrackKind::Aux: return "Aux";
    case TrackKind::Master: return "Master";
    }
    return "Track";
}

const char* automationModeName(AutomationMode mode) noexcept {
    switch (mode) {
    case AutomationMode::Off: return "Off";
    case AutomationMode::Read: return "Read";
    case AutomationMode::Touch: return "Touch";
    case AutomationMode::Write: return "Write";
    case AutomationMode::Latch: return "Latch";
    }
    return "Read";
}

Track::Track(TrackId id, TrackKind kind, std::string name)
    : id_(id), kind_(kind), name_(std::move(name)) {
    // Sensible defaults by kind: buses start silent-ish so a new bus never
    // doubles the level of something already routed to the master.
    switch (kind) {
    case TrackKind::Bus:
    case TrackKind::Aux:
    case TrackKind::Master:
        colour_.store(0xFF64748Bu);
        height_ = 60;
        break;
    case TrackKind::Midi:
    case TrackKind::Instrument:
        colour_.store(0xFFA855F7u);
        break;
    case TrackKind::Audio:
    default:
        break;
    }
}

void Track::setVolume(float linear) noexcept {
    volume_.store(math::clamp(linear, 0.0f, 4.0f), std::memory_order_relaxed);
}

void Track::setVolumeDb(float db) noexcept {
    setVolume(db <= -96.0f ? 0.0f : math::dbToGain(db));
}

float Track::volumeDb() const noexcept {
    const float linear = volume();
    return linear <= 0.0f ? -96.0f : math::gainToDb(linear);
}

void Track::setInputMonitoring(bool monitoring) noexcept {
    inputMonitoring_.store(monitoring, std::memory_order_relaxed);
}

bool Track::addInsert(InsertSlot slot) {
    if (inserts_.size() >= kMaxInserts) {
        AURA_LOG_WARN(kCategory, "Track '%s' already has the maximum of %zu inserts",
                      name_.c_str(), kMaxInserts);
        return false;
    }
    // Live reordering: the index identifies the slot, so gaps in `processor`
    // are allowed (a slot can be empty while a plug-in loads).
    if (slot.name.empty() && slot.pluginId.empty())
        slot.name = "Empty";
    inserts_.push_back(std::move(slot));
    return true;
}

bool Track::removeInsertAt(std::size_t index) {
    if (index >= inserts_.size())
        return false;
    inserts_.erase(inserts_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool Track::moveInsert(std::size_t from, std::size_t to) {
    if (from >= inserts_.size() || to >= inserts_.size() || from == to)
        return false;
    InsertSlot slot = std::move(inserts_[from]);
    inserts_.erase(inserts_.begin() + static_cast<std::ptrdiff_t>(from));
    inserts_.insert(inserts_.begin() + static_cast<std::ptrdiff_t>(to), std::move(slot));
    return true;
}

int Track::totalInsertLatencySamples() const noexcept {
    int total = 0;
    for (const auto& insert : inserts_) {
        if (insert.processor && !insert.bypassed)
            total += insert.processor->latencySamples();
    }
    return total;
}

void Track::setSendLevel(TrackId destination, float level) {
    for (auto& send : sends_) {
        if (send.destination == destination) {
            send.level = math::clamp(level, 0.0f, 2.0f);
            return;
        }
    }
}

} // namespace aura::audio
