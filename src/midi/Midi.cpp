// ============================================================================
// AURA DAW - src/midi/Midi.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/core/Sort.hpp"
#include "aura/midi/Midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "aura/core/Log.hpp"
#include "aura/core/Math.hpp"

namespace aura::midi {
namespace {
constexpr const char* kCategory = "MIDI";
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
Message Message::noteOn(int channel, int note, int velocity, std::int64_t ticks) noexcept {
    Message message;
    message.type = Type::NoteOn;
    message.channel = static_cast<std::uint8_t>(math::clamp(channel, 0, 15));
    message.data1 = static_cast<std::uint8_t>(math::clamp(note, 0, 127));
    message.data2 = static_cast<std::uint8_t>(math::clamp(velocity, 0, 127));
    message.timestampTicks = ticks;
    return message;
}

Message Message::noteOff(int channel, int note, std::int64_t ticks) noexcept {
    Message message;
    message.type = Type::NoteOff;
    message.channel = static_cast<std::uint8_t>(math::clamp(channel, 0, 15));
    message.data1 = static_cast<std::uint8_t>(math::clamp(note, 0, 127));
    message.timestampTicks = ticks;
    return message;
}

Message Message::controlChange(int channel, int controller, int value, std::int64_t ticks) noexcept {
    Message message;
    message.type = Type::ControlChange;
    message.channel = static_cast<std::uint8_t>(math::clamp(channel, 0, 15));
    message.data1 = static_cast<std::uint8_t>(math::clamp(controller, 0, 127));
    message.data2 = static_cast<std::uint8_t>(math::clamp(value, 0, 127));
    message.timestampTicks = ticks;
    return message;
}

Message Message::pitchBend(int channel, int value14, std::int64_t ticks) noexcept {
    Message message;
    message.type = Type::PitchBend;
    message.channel = static_cast<std::uint8_t>(math::clamp(channel, 0, 15));
    const int value = math::clamp(value14, 0, 16383);
    message.data1 = static_cast<std::uint8_t>(value & 0x7F);
    message.data2 = static_cast<std::uint8_t>((value >> 7) & 0x7F);
    message.timestampTicks = ticks;
    return message;
}

Message Message::programChange(int channel, int program, std::int64_t ticks) noexcept {
    Message message;
    message.type = Type::ProgramChange;
    message.channel = static_cast<std::uint8_t>(math::clamp(channel, 0, 15));
    message.data1 = static_cast<std::uint8_t>(math::clamp(program, 0, 127));
    message.timestampTicks = ticks;
    return message;
}

std::string Message::toString() const {
    char buffer[64];
    switch (type) {
    case Type::NoteOn:
        std::snprintf(buffer, sizeof(buffer), "NoteOn ch%d note%d vel%d", channel + 1, data1, data2);
        break;
    case Type::NoteOff:
        std::snprintf(buffer, sizeof(buffer), "NoteOff ch%d note%d", channel + 1, data1);
        break;
    case Type::ControlChange:
        std::snprintf(buffer, sizeof(buffer), "CC ch%d #%d = %d", channel + 1, data1, data2);
        break;
    case Type::PitchBend:
        std::snprintf(buffer, sizeof(buffer), "Bend ch%d = %d", channel + 1, bendValue());
        break;
    case Type::ProgramChange:
        std::snprintf(buffer, sizeof(buffer), "Program ch%d = %d", channel + 1, data1);
        break;
    case Type::PolyPressure:
        std::snprintf(buffer, sizeof(buffer), "PolyPressure ch%d note%d = %d", channel + 1, data1, data2);
        break;
    case Type::ChannelPressure:
        std::snprintf(buffer, sizeof(buffer), "ChannelPressure ch%d = %d", channel + 1, data1);
        break;
    case Type::Meta:
        std::snprintf(buffer, sizeof(buffer), "Meta type%d", metaType);
        break;
    }
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Quantisation
// ---------------------------------------------------------------------------
std::int64_t gridToTicks(QuantiseGrid grid) noexcept {
    switch (grid) {
    case QuantiseGrid::Off: return 0;
    case QuantiseGrid::Quarter: return time::kTicksPerQuarterNote;
    case QuantiseGrid::Eighth: return time::kTicksPerQuarterNote / 2;
    case QuantiseGrid::EighthTriplet: return time::kTicksPerQuarterNote / 3;
    case QuantiseGrid::Sixteenth: return time::kTicksPerQuarterNote / 4;
    case QuantiseGrid::SixteenthTriplet: return time::kTicksPerQuarterNote / 6;
    case QuantiseGrid::ThirtySecond: return time::kTicksPerQuarterNote / 8;
    }
    return 0;
}

const char* quantiseGridName(QuantiseGrid grid) noexcept {
    switch (grid) {
    case QuantiseGrid::Off: return "Off";
    case QuantiseGrid::Quarter: return "1/4";
    case QuantiseGrid::Eighth: return "1/8";
    case QuantiseGrid::EighthTriplet: return "1/8T";
    case QuantiseGrid::Sixteenth: return "1/16";
    case QuantiseGrid::SixteenthTriplet: return "1/16T";
    case QuantiseGrid::ThirtySecond: return "1/32";
    }
    return "Off";
}

bool quantiseNotes(std::vector<Note>& notes, const QuantiseSettings& settings) {
    const std::int64_t grid = gridToTicks(settings.grid);
    if (grid <= 0)
        return false;

    // Swing delays every second grid line by up to half a grid step.
    const double swingOffset = static_cast<double>(grid) * 0.5 * static_cast<double>(settings.swing);
    const float strength = math::clamp01(settings.strength);
    bool changed = false;

    // One helper for both ends of the note so the two quantise paths cannot drift
    // apart, and so the long long -> double narrowing std::llround() forces is
    // explicit rather than an implicit -Wconversion hit.
    const auto quantiseTicks = [grid](std::int64_t ticks) noexcept -> std::int64_t {
        const double gridValue = static_cast<double>(grid);
        // Round to whole grid units first, then scale back to ticks.
        const double units = static_cast<double>(std::llround(static_cast<double>(ticks) / gridValue));
        return static_cast<std::int64_t>(units * gridValue);
    };

    for (auto& note : notes) {
        const auto positions = static_cast<std::int64_t>(std::llround(static_cast<double>(note.startTicks) / static_cast<double>(grid)));
        const double swung =
            (positions % 2 == 0) ? 0.0 : swingOffset * (static_cast<double>(grid) / static_cast<double>(grid));
        const auto snapped = static_cast<std::int64_t>(
            std::llround(static_cast<double>(positions) * static_cast<double>(grid) + swung));
        const auto newStart = static_cast<std::int64_t>(
            std::llround(static_cast<double>(note.startTicks) +
                         static_cast<double>(snapped - note.startTicks) * static_cast<double>(strength)));
        if (newStart != note.startTicks) {
            note.startTicks = std::max<std::int64_t>(0, newStart);
            changed = true;
        }
        if (settings.quantiseEnds) {
            const auto endQuantised = quantiseTicks(note.endTicks());
            const auto newEnd = static_cast<std::int64_t>(
                std::llround(static_cast<double>(note.endTicks()) +
                             static_cast<double>(endQuantised - note.endTicks()) * static_cast<double>(strength)));
            const std::int64_t newLength = std::max<std::int64_t>(grid / 4, newEnd - note.startTicks);
            if (newLength != note.lengthTicks) {
                note.lengthTicks = newLength;
                changed = true;
            }
        }
    }
    return changed;
}

void transposeNotes(std::vector<Note>& notes, int semitones) {
    for (auto& note : notes)
        note.pitch = math::clamp(note.pitch + semitones, 0, 127);
}

void scaleVelocity(std::vector<Note>& notes, float factor, int minimumVelocity, int maximumVelocity) {
    for (auto& note : notes) {
        note.velocity = math::clamp(static_cast<int>(std::lround(static_cast<float>(note.velocity) * factor)),
                                    minimumVelocity, maximumVelocity);
    }
}

void setVelocity(std::vector<Note>& notes, int velocity) {
    for (auto& note : notes)
        note.velocity = math::clamp(velocity, 1, 127);
}

// ---------------------------------------------------------------------------
// MidiClip
// ---------------------------------------------------------------------------
MidiClip::MidiClip(std::uint64_t id, std::string name) : id_(id), name_(std::move(name)) {}

void MidiClip::addNote(Note note) {
    note.velocity = math::clamp(note.velocity, 1, 127);
    if (note.lengthTicks < 1)
        note.lengthTicks = 1;
    // Insert sorted by start time, then pitch: the renderer walks notes in order
    // and the piano roll draws them in order, so keeping this invariant here
    // avoids sorting in both places.
    const auto position = std::lower_bound(
        notes_.begin(), notes_.end(), note, [](const Note& a, const Note& b) {
            if (a.startTicks != b.startTicks)
                return a.startTicks < b.startTicks;
            return a.pitch < b.pitch;
        });
    notes_.insert(position, note);
}

bool MidiClip::removeNoteAt(std::size_t index) {
    if (index >= notes_.size())
        return false;
    notes_.erase(notes_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::size_t MidiClip::findNote(int pitch, std::int64_t ticks) const noexcept {
    for (std::size_t i = 0; i < notes_.size(); ++i) {
        if (notes_[i].pitch == pitch && notes_[i].isActiveAt(ticks))
            return i;
    }
    return notes_.size();
}

std::pair<int, int> MidiClip::pitchRange() const noexcept {
    if (notes_.empty())
        return {60, 72};
    int lowest = 127;
    int highest = 0;
    for (const auto& note : notes_) {
        lowest = std::min(lowest, note.pitch);
        highest = std::max(highest, note.pitch);
    }
    return {lowest, highest};
}

void MidiClip::appendRecorded(const Message& message) {
    if (message.isNoteOn()) {
        heldNotes_.push_back({message.data1, message.data2, message.timestampTicks});
        return;
    }
    if (message.isNoteOff()) {
        for (auto it = heldNotes_.begin(); it != heldNotes_.end(); ++it) {
            if (it->pitch == message.data1) {
                const std::int64_t length = std::max<std::int64_t>(1, message.timestampTicks - it->startTicks);
                addNote(Note{it->pitch, it->velocity, it->startTicks, length, message.channel, 64, false});
                heldNotes_.erase(it);
                return;
            }
        }
        return;
    }
    if (message.type == Message::Type::ControlChange || message.type == Message::Type::PitchBend) {
        ControllerEvent event;
        event.positionTicks = message.timestampTicks;
        if (message.type == Message::Type::PitchBend) {
            event.controller = kPitchBendController;
            event.value = message.bendValue();
        } else {
            event.controller = message.data1;
            event.value = message.data2;
        }
        controllers_.push_back(event);
    }
}

void MidiClip::finishRecording() {
    // Any note still held when recording stops gets a sensible length so the
    // take is never silently lost.
    for (const auto& held : heldNotes_) {
        addNote(Note{held.pitch, held.velocity, held.startTicks, time::kTicksPerQuarterNote / 4, 0, 64, false});
    }
    heldNotes_.clear();
    std::stable_sort(controllers_.begin(), controllers_.end(),
                     [](const ControllerEvent& a, const ControllerEvent& b) {
                         return a.positionTicks < b.positionTicks;
                     });
}

void MidiClip::collectEvents(std::int64_t startTick, std::int64_t endTick,
                             std::vector<Message>& out) const {
    if (muted_ || endTick <= startTick)
        return;
    for (const auto& note : notes_) {
        if (note.startTicks >= startTick && note.startTicks < endTick)
            out.push_back(Message::noteOn(note.channel, note.pitch, note.velocity, note.startTicks));
        // One note-off per note, emitted in the window that contains its end - a
        // note that started before the window and is still sounding ends here too.
        // (There used to be a second condition for "started earlier, still
        // sounding", which was a strict subset of this one: the note was turned off
        // twice, and a synth that counts active voices per pitch could end the
        // *next* note of that pitch with the stale off.)
        if (note.endTicks() >= startTick && note.endTicks() < endTick)
            out.push_back(Message::noteOff(note.channel, note.pitch, note.endTicks()));
    }
    for (const auto& controller : controllers_) {
        if (controller.positionTicks >= startTick && controller.positionTicks < endTick) {
            if (controller.controller == kPitchBendController)
                out.push_back(Message::pitchBend(0, controller.value, controller.positionTicks));
            else if (controller.controller < 128)
                out.push_back(Message::controlChange(0, controller.controller, controller.value,
                                                     controller.positionTicks));
        }
    }
    // Allocation-free stable sort: this runs from the audio thread (the engine
    // collects the events for each block), and std::stable_sort would allocate.
    core::stableSortSmall(out.begin(), out.end(), [](const Message& a, const Message& b) {
        return a.timestampTicks < b.timestampTicks;
    });
}

// ---------------------------------------------------------------------------
// MidiInputRouter
// ---------------------------------------------------------------------------
void MidiInputRouter::setDevices(std::vector<DeviceInfo> devices) {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_ = std::move(devices);
}

std::uint32_t MidiInputRouter::addRoute(std::string trackName, MidiRoute route) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteEntry entry;
    entry.token = nextToken_++;
    entry.trackName = std::move(trackName);
    entry.route = std::move(route);
    entry.queue = std::make_unique<MidiInputQueue>();
    routes_.push_back(std::move(entry));
    return routes_.back().token;
}

void MidiInputRouter::removeRoute(std::uint32_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    routes_.erase(std::remove_if(routes_.begin(), routes_.end(),
                                 [token](const RouteEntry& entry) { return entry.token == token; }),
                  routes_.end());
}

void MidiInputRouter::setRouteEnabled(std::uint32_t token, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : routes_) {
        if (entry.token == token) {
            entry.route.enabled = enabled;
            return;
        }
    }
}

void MidiInputRouter::handleIncomingMessage(const Message& message,
                                            const std::string& deviceId) noexcept {
    // Called from the MIDI driver thread. The route list is only mutated on the
    // control thread; we read it under the mutex - a short, bounded critical
    // section that never blocks the audio thread (which uses popForRoute).
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : routes_) {
        if (!entry.route.enabled || !entry.queue)
            continue;
        if (!entry.route.inputDevice.empty() && !deviceId.empty() &&
            entry.route.inputDevice != deviceId)
            continue;
        if (entry.route.inputChannel >= 0 && message.channel != entry.route.inputChannel)
            continue;
        entry.queue->push(message);
    }
}

bool MidiInputRouter::popForRoute(std::uint32_t token, Message& out) noexcept {
    for (auto& entry : routes_) {
        if (entry.token == token && entry.queue)
            return entry.queue->pop(out);
    }
    return false;
}

std::uint64_t MidiInputRouter::totalDroppedMessages() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t total = 0;
    for (const auto& entry : routes_) {
        if (entry.queue)
            total += entry.queue->droppedMessages();
    }
    return total;
}

void MidiInputRouter::panic() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : routes_) {
        if (!entry.queue)
            continue;
        for (int channel = 0; channel < 16; ++channel)
            entry.queue->push(Message::controlChange(channel, Message::kAllNotesOffController, 0));
    }
}

// ---------------------------------------------------------------------------
// Standard MIDI File (documented as not implemented yet, with a clear error)
// ---------------------------------------------------------------------------
AuraResult<std::vector<MidiClip>> importStandardMidiFile(const std::string& path) {
    AURA_LOG_WARN(kCategory, "SMF import requested for %s but not implemented in this milestone",
                  path.c_str());
    return failure(makeError(ErrorCode::NotImplemented,
                             "Standard MIDI File import is scheduled for milestone M7",
                             path));
}

Status exportStandardMidiFile(const std::string& path, const std::vector<MidiClip>& clips,
                              double tempoBpm, time::TimeSignature signature) {
    (void)clips;
    (void)tempoBpm;
    (void)signature;
    return failure(makeError(ErrorCode::NotImplemented,
                             "Standard MIDI File export is scheduled for milestone M7", path));
}

} // namespace aura::midi
