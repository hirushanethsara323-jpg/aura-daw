// ============================================================================
// AURA DAW - audio/Track.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Track model. A track owns:
//   * identity & presentation (name, colour, height, icon)
//   * mixer state (volume, pan, mute, solo, phase, input monitoring)
//   * the insert chain (built-in processors and hosted plug-ins)
//   * sends to aux/return tracks and a routing destination (bus or master)
//   * automation lanes
//   * an arrangement of clips (audio or MIDI)
//
// Threading: the *model* (this class) is owned by the control/UI thread. What
// the audio thread sees is the compiled GraphPlan plus a small snapshot of
// mixer values (atomics), never the std::vector members of Track. Structure
// changes rebuild the plan on the control thread and are swapped in through
// the engine's command queue.
// ============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/graph/AudioGraph.hpp"
#include "aura/time/Time.hpp"

namespace aura::midi {
class Instrument; // forward declaration keeps the audio model free of MIDI types
}

namespace aura::audio {

using TrackId = std::uint32_t;
inline constexpr TrackId kInvalidTrackId = 0;
/// Insert slots per track. 16 is enough for real sessions (the mixer UI shows
/// 8 at a time) and keeps the strip layout allocation-free.
inline constexpr std::size_t kMaxInserts = 16;

enum class TrackKind : std::int32_t { Audio = 0, Midi, Instrument, Bus, Aux, Master };

const char* trackKindName(TrackKind kind) noexcept;

/// Automation modes per parameter lane (per the brief: Read/Touch/Write/Latch).
enum class AutomationMode : std::int32_t {
    Off = 0,
    Read,
    Touch,
    Write,
    Latch,
};

const char* automationModeName(AutomationMode mode) noexcept;

/// A send from a track to an aux/return track.
struct Send {
    TrackId destination = kInvalidTrackId;
    float level = 0.0f;        ///< linear
    bool preFader = false;
    bool enabled = true;
};

/// A routing destination for a track's output.
struct Routing {
    TrackId destination = kInvalidTrackId; ///< bus/master/aux; 0 = master implicitly
    bool enabled = true;
    int sourceChannel = -1;  ///< -1 = stereo source; >=0 = read channel N
    int destinationChannel = -1;
};

/// An insert slot: either a built-in processor or (later) a hosted plug-in.
/// The track does not own the processor; the engine's ProcessorPool does, which
/// keeps ownership explicit and makes undo/redo of insert changes trivial.
struct InsertSlot {
    std::string name;
    dsp::Processor* processor = nullptr;
    bool bypassed = false;
    /// Plug-in identification (empty for built-ins).
    std::string pluginId;
    /// Wet/dry for parallel processing (1.0 = fully processed).
    float mix = 1.0f;
};

/// One track in the arrangement.
class Track {
public:
    Track(TrackId id, TrackKind kind, std::string name);

    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;

    [[nodiscard]] TrackId id() const noexcept { return id_; }
    [[nodiscard]] TrackKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    /// 0xAARRGGBB. The UI owns the palette; the model just stores the value.
    void setColour(std::uint32_t argb) noexcept { colour_.store(argb, std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t colour() const noexcept { return colour_.load(std::memory_order_relaxed); }

    void setHeight(int pixels) noexcept { height_ = pixels; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // --- mixer state (atomics: readable by the audio thread via snapshots) ---
    void setVolume(float linear) noexcept;
    [[nodiscard]] float volume() const noexcept { return volume_.load(std::memory_order_relaxed); }
    void setVolumeDb(float db) noexcept;
    [[nodiscard]] float volumeDb() const noexcept;
    void setPan(float pan) noexcept { pan_.store(pan, std::memory_order_relaxed); }
    [[nodiscard]] float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }
    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    [[nodiscard]] bool isMuted() const noexcept { return muted_.load(std::memory_order_relaxed); }
    void setSoloed(bool soloed) noexcept { soloed_.store(soloed, std::memory_order_relaxed); }
    [[nodiscard]] bool isSoloed() const noexcept { return soloed_.load(std::memory_order_relaxed); }
    void setPhaseInverted(bool inverted) noexcept { phaseInverted_.store(inverted, std::memory_order_relaxed); }
    [[nodiscard]] bool isPhaseInverted() const noexcept { return phaseInverted_.load(std::memory_order_relaxed); }
    void setRecordArmed(bool armed) noexcept { recordArmed_.store(armed, std::memory_order_relaxed); }
    [[nodiscard]] bool isRecordArmed() const noexcept { return recordArmed_.load(std::memory_order_relaxed); }
    void setInputMonitoring(bool monitoring) noexcept;
    [[nodiscard]] bool isInputMonitoring() const noexcept { return inputMonitoring_.load(std::memory_order_relaxed); }
    void setInputChannel(int channel) noexcept { inputChannel_.store(channel, std::memory_order_relaxed); }
    [[nodiscard]] int inputChannel() const noexcept { return inputChannel_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isAudible() const noexcept { return !isMuted() && !isSoloedOut(); }

    /// Solo logic: a track is "soloed out" when any track is soloed and this one
    /// is not (and it is not a master/bus that carries a soloed child - the
    /// engine resolves buses, see Mixer::rebuildSoloState).
    void setSoloedOut(bool soloedOut) noexcept { soloedOut_.store(soloedOut, std::memory_order_relaxed); }
    [[nodiscard]] bool isSoloedOut() const noexcept { return soloedOut_.load(std::memory_order_relaxed); }

    // --- inserts -------------------------------------------------------------
    [[nodiscard]] std::vector<InsertSlot>& inserts() noexcept { return inserts_; }
    [[nodiscard]] const std::vector<InsertSlot>& inserts() const noexcept { return inserts_; }
    [[nodiscard]] bool addInsert(InsertSlot slot);
    [[nodiscard]] bool removeInsertAt(std::size_t index);
    [[nodiscard]] bool moveInsert(std::size_t from, std::size_t to);
    [[nodiscard]] int totalInsertLatencySamples() const noexcept;

    // --- sends ---------------------------------------------------------------
    [[nodiscard]] std::vector<Send>& sends() noexcept { return sends_; }
    [[nodiscard]] const std::vector<Send>& sends() const noexcept { return sends_; }
    void addSend(Send send) { sends_.push_back(send); }
    void clearSends() { sends_.clear(); }
    void setSendLevel(TrackId destination, float level);

    // --- routing -------------------------------------------------------------
    [[nodiscard]] Routing& routing() noexcept { return routing_; }
    [[nodiscard]] const Routing& routing() const noexcept { return routing_; }
    void setOutput(TrackId destination) noexcept { routing_.destination = destination; }

    // --- automation ----------------------------------------------------------
    [[nodiscard]] AutomationMode automationMode() const noexcept {
        return static_cast<AutomationMode>(automationMode_.load(std::memory_order_relaxed));
    }
    void setAutomationMode(AutomationMode mode) noexcept {
        automationMode_.store(static_cast<std::int32_t>(mode), std::memory_order_relaxed);
    }

    // --- arrangement ---------------------------------------------------------
    /// Clip slots (audio clips, MIDI clips, and later automation lanes) are
    /// stored as opaque ids; the concrete clip objects live in the Project so
    /// serialization and undo have a single owner.
    [[nodiscard]] std::vector<std::uint64_t>& clips() noexcept { return clips_; }
    [[nodiscard]] const std::vector<std::uint64_t>& clips() const noexcept { return clips_; }

    /// Icons are referenced by name so the model stays free of UI types.
    void setIcon(std::string icon) { icon_ = std::move(icon); }
    [[nodiscard]] const std::string& icon() const noexcept { return icon_; }

    /// Explicit record input mapping (which device channels feed this track).
    void setInputChannels(std::vector<int> channels) { inputChannels_ = std::move(channels); }
    [[nodiscard]] const std::vector<int>& inputChannels() const noexcept { return inputChannels_; }

    /// Instrument feeding a MIDI/instrument track. Owned by the engine's
    /// instrument pool; the track only holds the reference (forward-declared to
    /// avoid the audio model depending on the MIDI module).
    void setInstrument(midi::Instrument* instrument) noexcept { instrument_ = instrument; }
    [[nodiscard]] midi::Instrument* instrument() const noexcept { return instrument_; }

    /// Graph node backing this track (set by the engine when the plan is built).
    void setGraphNodeId(graph::NodeId node) noexcept { graphNode_ = node; }
    [[nodiscard]] graph::NodeId graphNodeId() const noexcept { return graphNode_; }

private:

    TrackId id_ = kInvalidTrackId;
    TrackKind kind_ = TrackKind::Audio;
    std::string name_;
    std::string icon_;
    std::atomic<std::uint32_t> colour_{0xFF3B82F6u};
    int height_ = 74;

    std::atomic<float> volume_{1.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> soloed_{false};
    std::atomic<bool> soloedOut_{false};
    std::atomic<bool> phaseInverted_{false};
    std::atomic<bool> recordArmed_{false};
    std::atomic<bool> inputMonitoring_{false};
    std::atomic<int> inputChannel_{-1};
    std::atomic<std::int32_t> automationMode_{static_cast<std::int32_t>(AutomationMode::Read)};
    midi::Instrument* instrument_ = nullptr; ///< non-owning (engine instrument pool)

    std::vector<InsertSlot> inserts_;
    std::vector<Send> sends_;
    Routing routing_{};
    std::vector<std::uint64_t> clips_;
    std::vector<int> inputChannels_;
    graph::NodeId graphNode_ = graph::kInvalidNodeId;
};

} // namespace aura::audio
