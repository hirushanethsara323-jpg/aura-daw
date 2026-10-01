// ============================================================================
// AURA DAW - engine/AudioEngine.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio engine: the only place that owns the real-time thread and decides
// what happens per block.
//
// Per-block sequence (all of it must stay allocation/lock/IO free):
//   1. drain commands that are safe to apply on the audio thread
//   2. render clip audio into track nodes (non-destructive, from SampleStreams)
//   3. run the graph plan: inserts -> sends -> buses -> master
//   4. master strip: limiter, meter, loudness
//   5. advance the transport (loop wrap / punch, sample accurate)
//   6. publish meters and playhead for the UI
//
// Structure changes (adding a track, reordering inserts) are compiled into a new
// GraphPlan on the control thread and swapped in through an atomic shared_ptr;
// the audio thread never sees a partially built plan.
// ============================================================================
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "aura/audio/Mixer.hpp"
#include "aura/audio/RecordingEngine.hpp"
#include "aura/dsp/Loudness.hpp"
#include "aura/engine/EngineCommandQueue.hpp"
#include "aura/graph/AudioGraph.hpp"
#include "aura/midi/Midi.hpp"
#include "aura/transport/Transport.hpp"

namespace aura::project {
class Project; // the engine references the project but never owns it
}

namespace aura::engine {

/// Audio device contract. WASAPI/ASIO implement this on Windows; the mock and
/// offline implementations live in audioio/ and engine/OfflineRenderer.
class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    /// Called by the device for every block. MUST be real-time safe.
    using Callback = std::function<void(float* const* outputs, const float* const* inputs,
                                        int numChannels, int numInputChannels, int numFrames)>;

    virtual Status start(Callback callback) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual double sampleRate() const noexcept = 0;
    [[nodiscard]] virtual int bufferSize() const noexcept = 0;
    [[nodiscard]] virtual int numOutputChannels() const noexcept = 0;
    [[nodiscard]] virtual int numInputChannels() const noexcept = 0;
    /// Round-trip latency in samples (input + output buffers + driver).
    [[nodiscard]] virtual int latencySamples() const noexcept = 0;
};

struct EngineSettings {
    double sampleRate = 48000.0;
    int bufferSize = 256;
    int numOutputChannels = 2;
    int numInputChannels = 2;
    bool inputMonitoringEnabled = true;
    bool midiInputEnabled = true;
    /// Delay compensation: shift every path to the maximum insert latency.
    bool delayCompensation = true;
};

/// CPU load estimate published to the UI (fraction of the block budget used).
struct EngineStatistics {
    std::atomic<float> cpuLoad{0.0f};
    std::atomic<float> peakCpuLoad{0.0f};
    std::atomic<std::uint64_t> underrunCount{0};
    std::atomic<std::uint64_t> xrunFrames{0};
    std::atomic<float> averageBlockMicros{0.0f};
    /// True while the disk writer is dropping frames (recording problem).
    std::atomic<bool> diskOverloaded{false};
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // --- lifecycle (control thread) ------------------------------------------
    /// Attaches the device and starts streaming. The engine takes ownership of
    /// the device pointer's lifetime decisions; it does NOT delete it.
    Status initialise(IAudioDevice* device, const EngineSettings& settings);
    void shutdown();

    /// Attaches the project whose tracks/clips are played. The engine never owns
    /// the project.
    void setProject(project::Project* project);
    [[nodiscard]] project::Project* project() noexcept { return project_; }

    /// Rebuilds the graph plan from the project (call after structural changes:
    /// adding tracks, changing routing, adding inserts). Control thread.
    Status rebuildGraph();

    /// Publishes a plan built by the caller (used by rebuildGraph(), by tests
    /// and by the offline renderer to render an arbitrary routing).
    ///
    /// LIFETIME CONTRACT (important): the audio thread reads the plan through a
    /// bare atomic pointer, so it never performs a shared_ptr refcount update on
    /// the real-time thread. Plans are retired only after two more blocks have
    /// been processed, which is strictly longer than any in-flight block can
    /// hold the old pointer. Call this only from the control thread.
    void publishPlan(std::shared_ptr<graph::GraphPlan> plan);

    /// Control-thread view of the live plan (may be null before initialise()).
    [[nodiscard]] std::shared_ptr<graph::GraphPlan> currentPlan() const noexcept {
        return activePlan_;
    }

    [[nodiscard]] transport::Transport& transport() noexcept { return transport_; }
    [[nodiscard]] const transport::Transport& transport() const noexcept { return transport_; }
    [[nodiscard]] audio::RecordingEngine& recorder() noexcept { return recorder_; }
    [[nodiscard]] EngineStatistics& statistics() noexcept { return statistics_; }

    [[nodiscard]] EngineSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const EngineSettings& settings() const noexcept { return settings_; }
    void setSettings(const EngineSettings& settings);

    /// Command queue: the UI posts commands, the audio thread drains them at a
    /// block boundary (never mid-block, so behaviour is deterministic).
    [[nodiscard]] EngineCommandQueue& commands() noexcept { return commands_; }

    /// Renders exactly one block. Called by the device callback and by the
    /// offline renderer; the only function that may be called from the audio
    /// thread. `inputs` may be null.
    void processBlock(float* const* outputs, const float* const* inputs, int numFrames) noexcept;

    /// True when the engine is currently streaming.
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Publishes the current playhead for the UI (lock-free read).
    [[nodiscard]] time::SamplePos playheadPosition() const noexcept {
        return transport_.position();
    }

    /// Total reported latency: device + graph + master limiter.
    [[nodiscard]] int totalLatencySamples() const noexcept;
    /// Delay compensation diagnostics (control thread).
    ///
    /// `delayCompensationSamples()` is how much delay the current plan inserts to
    /// align parallel paths, summed over every compensated connection; it is what
    /// the UI shows next to the round-trip latency so a user can see *why* a track
    /// with a look-ahead limiter made the session later. It is not part of
    /// `totalLatencySamples()` (see the comment there).
    [[nodiscard]] int delayCompensationSamples() const noexcept;
    [[nodiscard]] int compensatedConnectionCount() const noexcept;

private:
    /// Copies track fader/pan/mute/solo state into the graph nodes. Called once
    /// per block on the audio thread: every value read here is an atomic the
    /// control thread wrote, and writing it to the node only stores floats.
    void applyMixerStateToGraph() noexcept;

    void renderClips(int numFrames, const dsp::ProcessContext& context) noexcept;
    /// Collects MIDI clip events for every MIDI/instrument track and renders the
    /// track's instrument into its graph node.
    void renderInstruments(int numFrames, const dsp::ProcessContext& context) noexcept;
    void renderRecording(int numFrames, const float* const* inputs, int numInputChannels) noexcept;
    void updateStatistics(std::uint64_t blockStartMicros, int numFrames) noexcept;

    IAudioDevice* device_ = nullptr;
    EngineSettings settings_{};
    project::Project* project_ = nullptr;
    transport::Transport transport_;
    audio::RecordingEngine recorder_;
    EngineCommandQueue commands_;
    EngineStatistics statistics_{};

    /// Live plan: written by the control thread, read once per block by the
    /// audio thread. The owning shared_ptrs live in activePlan_/retiredPlans_.
    std::atomic<graph::GraphPlan*> livePlan_{nullptr};
    std::shared_ptr<graph::GraphPlan> activePlan_;
    std::vector<std::shared_ptr<graph::GraphPlan>> retiredPlans_;
    std::atomic<std::uint64_t> blocksProcessedAtomic_{0};

    /// Frees plans that can no longer be in use (called from publishPlan()).
    void pruneRetiredPlans();

    [[nodiscard]] graph::GraphPlan* livePlan() const noexcept {
        return livePlan_.load(std::memory_order_acquire);
    }
    std::atomic<bool> running_{false};

    /// Scratch for MIDI events per block (preallocated: the audio thread must
    /// not allocate).
    std::vector<midi::Message> midiScratch_;

    std::uint64_t blocksProcessed_ = 0;
    double cumulativeBlockMicros_ = 0.0;
};

} // namespace aura::engine
