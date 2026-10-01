// ============================================================================
// AURA DAW - midi/Midi.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI layer: messages, clips (note + controller events), quantisation and the
// input router that turns device input into per-track events.
//
// Time model: MIDI events are stored in TICKS (960 PPQN), not samples. That is
// what makes tempo changes, importing/exporting Standard MIDI Files and
// quantisation correct. Conversion to samples happens once per block on the
// audio thread using the tempo map it already has.
//
// Threading: MIDI input arrives on a driver thread (WinMM/ASIO callback). It is
// pushed into a lock-free queue and drained by the engine into the graph, so no
// driver thread ever touches the engine's structures.
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/SpscQueue.hpp"
#include "aura/time/Time.hpp"

namespace aura::midi {

/// A single MIDI event. Plain data (trivially copyable) so it can live in
/// lock-free queues and be copied per block without allocation.
struct Message {
    enum class Type : std::uint8_t {
        NoteOff = 0,
        NoteOn,
        PolyPressure,
        ControlChange,
        ProgramChange,
        ChannelPressure,
        PitchBend,
        /// Meta events are used inside MIDI clips (tempo, marker, text).
        Meta,
    };

    Type type = Type::NoteOn;
    std::uint8_t channel = 0;   ///< 0-based (MIDI channels are 1-16 on the wire)
    std::uint8_t data1 = 0;     ///< note number / controller number
    std::uint8_t data2 = 0;     ///< velocity / controller value
    std::uint8_t metaType = 0;
    std::int64_t timestampTicks = 0; ///< position in the clip or absolute (ticks)
    std::int64_t sampleOffset = 0;   ///< within-block offset, filled by the engine

    [[nodiscard]] static Message noteOn(int channel, int note, int velocity,
                                        std::int64_t ticks = 0) noexcept;
    [[nodiscard]] static Message noteOff(int channel, int note, std::int64_t ticks = 0) noexcept;
    [[nodiscard]] static Message controlChange(int channel, int controller, int value,
                                               std::int64_t ticks = 0) noexcept;
    [[nodiscard]] static Message pitchBend(int channel, int value14, std::int64_t ticks = 0) noexcept;
    [[nodiscard]] static Message programChange(int channel, int program,
                                               std::int64_t ticks = 0) noexcept;

    [[nodiscard]] bool isNoteOn() const noexcept {
        return type == Type::NoteOn && data2 > 0;
    }
    [[nodiscard]] bool isNoteOff() const noexcept {
        return type == Type::NoteOff || (type == Type::NoteOn && data2 == 0);
    }
    /// 14-bit pitch bend value 0..16383 (8192 = centre).
    [[nodiscard]] int bendValue() const noexcept {
        return static_cast<int>(data1) | (static_cast<int>(data2) << 7);
    }
    [[nodiscard]] float bendSemitones(int rangeSemitones = 2) const noexcept {
        return (static_cast<float>(bendValue()) - 8192.0f) / 8192.0f *
               static_cast<float>(rangeSemitones);
    }
    [[nodiscard]] std::string toString() const;

    /// Standard MIDI controller numbers used by AURA's defaults.
    static constexpr int kSustainController = 64;
    static constexpr int kAllNotesOffController = 123;
    static constexpr int kAllSoundOffController = 120;
};

/// A note inside a MIDI clip.
struct Note {
    int pitch = 60;              ///< MIDI note number
    int velocity = 100;          ///< 1..127
    std::int64_t startTicks = 0;
    std::int64_t lengthTicks = 960;
    std::int32_t channel = 0;
    int releaseVelocity = 64;
    bool selected = false;

    [[nodiscard]] std::int64_t endTicks() const noexcept { return startTicks + lengthTicks; }
    [[nodiscard]] bool isActiveAt(std::int64_t ticks) const noexcept {
        return ticks >= startTicks && ticks < endTicks();
    }
};

/// One controller lane (e.g. CC1 modulation, CC64 sustain, pitch bend #128).
struct ControllerEvent {
    std::int64_t positionTicks = 0;
    int controller = 1; ///< 0..127, or 128 for pitch bend, 129 for channel pressure
    int value = 0;      ///< 0..127 (or 0..16383 for pitch bend)
};

inline constexpr int kPitchBendController = 128;
inline constexpr int kChannelPressureController = 129;

/// Quantisation grid.
enum class QuantiseGrid : std::int32_t {
    Off = 0,
    Quarter,
    Eighth,
    EighthTriplet,
    Sixteenth,
    SixteenthTriplet,
    ThirtySecond,
};

[[nodiscard]] std::int64_t gridToTicks(QuantiseGrid grid) noexcept;
const char* quantiseGridName(QuantiseGrid grid) noexcept;

/// Quantisation settings shared by the piano roll and clip commands.
struct QuantiseSettings {
    QuantiseGrid grid = QuantiseGrid::Sixteenth;
    /// Strength 0..1 (1 = snap exactly).
    float strength = 1.0f;
    /// Also quantise note ends (lengths) when enabled.
    bool quantiseEnds = false;
    float swing = 0.0f; ///< -0.5 .. +0.5, delays every second grid line
};

/// Turns note positions into themselves (returns true when anything moved).
bool quantiseNotes(std::vector<Note>& notes, const QuantiseSettings& settings);

/// Non-destructive-ish note edits used by the piano roll and the command layer.
void transposeNotes(std::vector<Note>& notes, int semitones);
void scaleVelocity(std::vector<Note>& notes, float factor, int minimumVelocity = 1,
                   int maximumVelocity = 127);
void setVelocity(std::vector<Note>& notes, int velocity);

/// A MIDI clip: notes plus controller lanes.
class MidiClip {
public:
    MidiClip(std::uint64_t id, std::string name);

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    void setStart(time::SamplePos start) noexcept { start_ = start < 0 ? 0 : start; }
    [[nodiscard]] time::SamplePos start() const noexcept { return start_; }
    void setLengthTicks(std::int64_t ticks) noexcept { lengthTicks_ = ticks < 1 ? 1 : ticks; }
    [[nodiscard]] std::int64_t lengthTicks() const noexcept { return lengthTicks_; }
    [[nodiscard]] std::int64_t endTicks() const noexcept { return startTicks_ + lengthTicks_; }
    void setStartTicks(std::int64_t ticks) noexcept { startTicks_ = ticks < 0 ? 0 : ticks; }
    [[nodiscard]] std::int64_t startTicks() const noexcept { return startTicks_; }
    void setLoopEnabled(bool loop) noexcept { loop_ = loop; }
    [[nodiscard]] bool isLoopEnabled() const noexcept { return loop_; }
    void setMuted(bool muted) noexcept { muted_ = muted; }
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setColour(std::uint32_t argb) noexcept { colour_ = argb; }
    [[nodiscard]] std::uint32_t colour() const noexcept { return colour_; }

    [[nodiscard]] std::vector<Note>& notes() noexcept { return notes_; }
    [[nodiscard]] const std::vector<Note>& notes() const noexcept { return notes_; }
    [[nodiscard]] std::vector<ControllerEvent>& controllers() noexcept { return controllers_; }
    [[nodiscard]] const std::vector<ControllerEvent>& controllers() const noexcept {
        return controllers_;
    }

    /// Adds a note, keeping the note list sorted by start time (the renderer and
    /// the piano roll both rely on this ordering).
    void addNote(Note note);
    bool removeNoteAt(std::size_t index);
    [[nodiscard]] std::size_t findNote(int pitch, std::int64_t ticks) const noexcept;

    /// Highest/lowest pitch present (used to frame the piano roll).
    [[nodiscard]] std::pair<int, int> pitchRange() const noexcept;

    /// Events captured during recording, merged on stop.
    void appendRecorded(const Message& message);
    /// Bakes performed notes into the clip (called when recording stops).
    void finishRecording();

    /// Collects the messages that fall inside [startTick, endTick) into `out`.
    /// Used by the engine per block; allocation-free when `out` is reserved.
    void collectEvents(std::int64_t startTick, std::int64_t endTick,
                       std::vector<Message>& out) const;

private:
    std::uint64_t id_ = 0;
    std::string name_;
    time::SamplePos start_ = 0;
    std::int64_t startTicks_ = 0;
    std::int64_t lengthTicks_ = time::kTicksPerBar4_4;
    bool loop_ = false;
    bool muted_ = false;
    std::uint32_t colour_ = 0xFFA855F7u;
    std::vector<Note> notes_;
    std::vector<ControllerEvent> controllers_;

    /// Notes currently being performed (recording).
    struct HeldNote {
        int pitch = 60;
        int velocity = 100;
        std::int64_t startTicks = 0;
    };
    std::vector<HeldNote> heldNotes_;
};

/// Lock-free MIDI input queue: driver thread -> engine.
class MidiInputQueue {
public:
    static constexpr std::size_t kCapacity = 512;

    void push(const Message& message) noexcept {
        if (!queue_.tryPush(message))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool pop(Message& out) noexcept {
        if (auto value = queue_.tryPop()) {
            out = *value;
            return true;
        }
        return false;
    }
    [[nodiscard]] std::uint64_t droppedMessages() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    SpscQueue<Message, kCapacity> queue_;
    std::atomic<std::uint64_t> dropped_{0};
};

/// Per-track MIDI routing: which device/channel feeds which track, and where
/// the track's own MIDI output goes.
struct MidiRoute {
    std::string inputDevice;   ///< empty = "all inputs"
    int inputChannel = -1;     ///< -1 = omni
    std::string outputDevice;  ///< empty = internal instrument only
    int outputChannel = 0;
    bool enabled = true;
    bool thru = false;         ///< echo input to output (for hardware synths)
};

/// The router owns the device list and dispatches incoming messages to tracks.
class MidiInputRouter {
public:
    struct DeviceInfo {
        std::string id;
        std::string name;
        bool isInput = true;
        bool enabled = true;
    };

    /// Installs the current device list (from the audio/MIDI device manager).
    void setDevices(std::vector<DeviceInfo> devices);

    /// Registers a track's routing. Returns a token used to unregister.
    std::uint32_t addRoute(std::string trackName, MidiRoute route);
    void removeRoute(std::uint32_t token);
    void setRouteEnabled(std::uint32_t token, bool enabled);

    /// Called from the MIDI driver thread. Copies the message into every
    /// matching route's queue. Lock-free.
    void handleIncomingMessage(const Message& message, const std::string& deviceId = {}) noexcept;

    /// Called by the engine on the audio thread: drains `token`'s queue.
    [[nodiscard]] bool popForRoute(std::uint32_t token, Message& out) noexcept;

    /// Diagnostics.
    [[nodiscard]] std::uint64_t totalDroppedMessages() const noexcept;
    [[nodiscard]] const std::vector<DeviceInfo>& devices() const noexcept { return devices_; }

    /// Sends an all-notes-off to every route (used when stopping/panicking).
    void panic() noexcept;

private:
    struct RouteEntry {
        std::uint32_t token = 0;
        std::string trackName;
        MidiRoute route;
        std::unique_ptr<MidiInputQueue> queue;
    };

    mutable std::mutex mutex_; ///< control-thread only; the audio thread reads
                               ///< the atomic `enabled` flag and the queue.
    std::vector<DeviceInfo> devices_;
    std::vector<RouteEntry> routes_;
    std::uint32_t nextToken_ = 1;
};

/// Standard MIDI File support (used by import/export milestones).
/// Returns a clear NotImplemented error until the SMF reader lands; the
/// signature exists so callers can be written against it today.
AuraResult<std::vector<MidiClip>> importStandardMidiFile(const std::string& path);
Status exportStandardMidiFile(const std::string& path, const std::vector<MidiClip>& clips,
                              double tempoBpm, time::TimeSignature signature);

} // namespace aura::midi
