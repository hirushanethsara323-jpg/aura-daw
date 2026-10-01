// ============================================================================
// AURA DAW - engine/EngineCommandQueue.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lock-free command channel from the UI/worker threads to the audio thread.
//
// Commands are small, trivially-copyable structs (a tagged union) so they can
// travel through an SPSC ring without allocation. Anything that needs to
// allocate (loading a plug-in, creating a track) is done on the control thread;
// the queue only carries the *result* (typically "swap in this new plan" or
// "apply these mixer values").
// ============================================================================
#pragma once

#include <cstdint>

#include "aura/core/SpscQueue.hpp"

namespace aura::engine {

enum class EngineCommandType : std::int32_t {
    None = 0,
    TransportPlay,
    TransportStop,
    TransportPause,
    TransportRecord,
    TransportSeek,
    SetLoopRange,
    SetClearLoop,
    SetTrackVolume,
    SetTrackPan,
    SetTrackMute,
    SetTrackSolo,
    SwapPlan,             ///< pointer payload (shared_ptr kept alive by the caller)
    StartRecording,
    StopRecording,
    PanicMidi,            ///< all-notes-off on every MIDI path
    SetMetronome,
};

struct EngineCommand {
    EngineCommandType type = EngineCommandType::None;
    std::int32_t index = -1;      ///< track index where relevant
    float value = 0.0f;
    std::int64_t value64 = 0;     ///< seek position / loop boundaries
    void* pointer = nullptr;      ///< plan payload for SwapPlan
};

class EngineCommandQueue {
public:
    static constexpr std::size_t kCapacity = 256;

    /// Control thread.
    bool post(const EngineCommand& command) noexcept { return queue_.tryPush(command); }

    /// Audio thread: pops at most `maxCommands` commands per block. Applying a
    /// bounded number per block keeps worst-case block time deterministic.
    template <typename Handler>
    void drain(int maxCommands, Handler&& handler) noexcept {
        int processed = 0;
        while (processed < maxCommands) {
            auto command = queue_.tryPop();
            if (!command.has_value())
                break;
            handler(*command);
            ++processed;
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
    [[nodiscard]] std::uint64_t droppedCommands() const noexcept { return dropped_; }
    void noteDropped() noexcept { ++dropped_; }

private:
    SpscQueue<EngineCommand, kCapacity> queue_;
    std::uint64_t dropped_ = 0;
};

} // namespace aura::engine
