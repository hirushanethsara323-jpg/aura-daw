// ============================================================================
// AURA DAW - audioio/AudioDeviceManager.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio device layer.
//
// AURA deliberately owns this layer instead of delegating it to a GUI toolkit:
// the brief requires WASAPI shared/exclusive AND ASIO behind one abstraction,
// with explicit device-capability checks and hot-plug/sample-rate-change
// recovery. That is driver-level behaviour, so it lives in AURA's own code.
//
// Backends:
//   WasapiDevice  - Windows. Shared mode (event driven, WASAPI resamples) and
//                   exclusive mode (AURA negotiates the exact format, lowest
//                   latency). Milestone M3.
//   AsioDevice    - Windows. ASIO SDK is GPLv3-or-proprietary since Oct 2025,
//                   which is compatible with AURA's GPL-3.0-or-later licence.
//                   Scheduled for M8; until then the backend reports
//                   ErrorCode::NotImplemented instead of pretending.
//   MockAudioDevice - every platform. Deterministic, injectable failures: the
//                   test suite and CI use it to exercise the engine, hot-plug
//                   and error recovery without any hardware.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/engine/AudioEngine.hpp"

namespace aura::audioio {

enum class DriverType : std::int32_t { WasapiShared = 0, WasapiExclusive, Asio, Mock };

const char* driverTypeName(DriverType type) noexcept;

/// What a device can do, as reported by the driver (never guessed).
struct AudioDeviceInfo {
    std::string id;              ///< stable string used by preferences
    std::string name;            ///< user-facing device name
    DriverType driver = DriverType::WasapiShared;
    int maxInputChannels = 0;
    int maxOutputChannels = 0;
    double defaultSampleRate = 48000.0;
    std::vector<double> supportedSampleRates; ///< empty = driver does not say
    int defaultBufferSize = 512;
    int minimumBufferSize = 64;
    int maximumBufferSize = 2048;
    bool isDefault = false;
    bool isExclusiveCapable = false;
    /// Round-trip latency reported by the driver, in samples (0 = unknown).
    int reportedLatencySamples = 0;
};

/// The buffer sizes and sample rates AURA offers. Anything the device does not
/// support is filtered out before the UI shows it, so the user can never pick a
/// combination the driver will reject.
struct AudioFormatSupport {
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;

    [[nodiscard]] bool supportsSampleRate(double sampleRate) const noexcept;
    [[nodiscard]] bool supportsBufferSize(int frames) const noexcept;
    /// Closest supported values (used when a project's rate is unavailable).
    [[nodiscard]] double nearestSampleRate(double sampleRate) const noexcept;
    [[nodiscard]] int nearestBufferSize(int frames) const noexcept;
};

class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    /// Enumerates available devices. Never throws; on failure the error is
    /// logged and the list is left unchanged (a missing driver must not stop
    /// AURA from starting - it falls back to the mock device so the user can
    /// still work on the project).
    Status refresh();

    [[nodiscard]] const std::vector<AudioDeviceInfo>& devices() const noexcept { return devices_; }
    [[nodiscard]] const AudioDeviceInfo* findDevice(const std::string& id) const noexcept;
    [[nodiscard]] const AudioDeviceInfo* defaultOutputDevice() const noexcept;

    /// Queries the formats a device supports without opening it for streaming.
    [[nodiscard]] AudioFormatSupport formatSupport(const std::string& id) const;

    /// Opens a device for streaming and attaches it to the engine.
    Status open(const std::string& id, const engine::EngineSettings& settings);
    /// Opens the previously used device (from preferences), or the system
    /// default when nothing is remembered.
    Status openRememberedOr(const engine::EngineSettings& settings);
    void close();

    [[nodiscard]] const std::string& currentDeviceId() const noexcept { return currentId_; }
    [[nodiscard]] bool isOpen() const noexcept { return device_ != nullptr; }
    [[nodiscard]] const AudioDeviceInfo* currentDeviceInfo() const noexcept {
        return findDevice(currentId_);
    }

    /// Reported round-trip latency in samples (device + engine graph).
    [[nodiscard]] int totalLatencySamples() const;

    /// Called on the UI thread when the device set changes (hot-plug). The
    /// manager also re-opens automatically when the running device disappears
    /// and reports it through `onDeviceChanged`.
    using DeviceChangeCallback = std::function<void(const std::string& message, bool recovered)>;
    void setDeviceChangeCallback(DeviceChangeCallback callback);

    /// Hands the manager the engine it should attach devices to.
    void attachEngine(engine::AudioEngine* engine) noexcept { engine_ = engine; }

    /// Starts watching for hot-plug events (default: every 2 seconds, which is
    /// the interval Windows itself caches endpoint enumeration for). Pass 0 to
    /// disable: AURA never polls a filesystem, and a user who dislikes periodic
    /// enumeration can turn it off entirely in preferences.
    void startWatching(std::uint32_t intervalMs = 2000);
    void stopWatching();
    [[nodiscard]] bool isWatching() const noexcept { return watching_.load(); }

    /// Creates a device object for testing (mock backend). Tests inject
    /// behaviour through the returned object.
    static std::unique_ptr<engine::IAudioDevice> createMockDevice(const AudioDeviceInfo& info);

    /// True when the platform backend for `driver` is compiled into this build.
    [[nodiscard]] static bool backendAvailable(DriverType driver) noexcept;

private:
    Status openInternal(const AudioDeviceInfo& info, const engine::EngineSettings& settings);

    /// Detaches the device from the engine, stops it and destroys it. Safe to
    /// call with no device open. NEVER destroy the device without going through
    /// here: the engine keeps a raw pointer to it.
    void releaseDevice() noexcept;

    std::vector<AudioDeviceInfo> devices_;
    std::unique_ptr<engine::IAudioDevice> device_;
    std::string currentId_;
    engine::AudioEngine* engine_ = nullptr;
    DeviceChangeCallback deviceChangeCallback_;
    std::atomic<bool> watching_{false};
    std::thread watcherThread_;
    /// Signature of the device set at the last successful refresh, used to detect
    /// hot-plug changes without polling the filesystem.
    std::uint64_t deviceSignature_ = 0;
};

/// Mock device used by tests, CI and the "no audio hardware" fallback.
///
/// It is a REAL device implementation (it produces and consumes audio on a
/// worker thread at the requested rate) with deterministic behaviour and hooks
/// for the failure modes the engine must survive.
class MockAudioDevice final : public engine::IAudioDevice {
public:
    explicit MockAudioDevice(AudioDeviceInfo info);
    ~MockAudioDevice() override;

    Status start(Callback callback) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const noexcept override { return running_.load(); }
    [[nodiscard]] std::string name() const override { return info_.name; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] int bufferSize() const noexcept override { return bufferSize_; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return info_.maxOutputChannels; }
    [[nodiscard]] int numInputChannels() const noexcept override { return info_.maxInputChannels; }
    [[nodiscard]] int latencySamples() const noexcept override { return info_.reportedLatencySamples; }

    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    void setBufferSize(int frames) noexcept { bufferSize_ = frames; }
    /// When true the worker thread stops calling the callback, as if the device
    /// had been unplugged mid-stream.
    void simulateDeviceLoss() noexcept { lost_.store(true); }
    /// Injects a constant tone on the input channels (for input-path tests).
    void setInputTone(double hz, float amplitude) noexcept;

    [[nodiscard]] std::uint64_t callbackCount() const noexcept { return callbacks_.load(); }
    [[nodiscard]] float lastOutputPeak() const noexcept { return lastPeak_.load(); }

private:
    void workerLoop();

    AudioDeviceInfo info_;
    Callback callback_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> lost_{false};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<float> lastPeak_{0.0f};
    double sampleRate_ = 48000.0;
    int bufferSize_ = 256;
    double toneHz_ = 0.0;
    float toneAmplitude_ = 0.0f;
    double tonePhase_ = 0.0;
    std::vector<float> outputStorage_[2];
    std::vector<float> inputStorage_[2];
};

} // namespace aura::audioio
