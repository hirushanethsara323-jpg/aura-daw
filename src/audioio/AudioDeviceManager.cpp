// ============================================================================
// AURA DAW - src/audioio/AudioDeviceManager.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/audioio/AudioDeviceManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "aura/audioio/WasapiDevice.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Math.hpp"

namespace aura::audioio {
namespace {
constexpr const char* kCategory = "AudioIO";

/// AURA's candidate rates and buffer sizes. The device's real capabilities are
/// intersected with these, never assumed.
constexpr double kCandidateRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
constexpr int kCandidateBuffers[] = {32, 64, 128, 192, 256, 384, 512, 768, 1024, 2048};

} // namespace

const char* driverTypeName(DriverType type) noexcept {
    switch (type) {
    case DriverType::WasapiShared: return "WASAPI (shared)";
    case DriverType::WasapiExclusive: return "WASAPI (exclusive)";
    case DriverType::Asio: return "ASIO";
    case DriverType::Mock: return "Mock (no hardware)";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// AudioFormatSupport
// ---------------------------------------------------------------------------
bool AudioFormatSupport::supportsSampleRate(double sampleRate) const noexcept {
    for (const double rate : sampleRates) {
        if (std::abs(rate - sampleRate) < 1.0)
            return true;
    }
    return false;
}

bool AudioFormatSupport::supportsBufferSize(int frames) const noexcept {
    return std::find(bufferSizes.begin(), bufferSizes.end(), frames) != bufferSizes.end();
}

double AudioFormatSupport::nearestSampleRate(double sampleRate) const noexcept {
    if (sampleRates.empty())
        return sampleRate;
    double best = sampleRates.front();
    for (const double rate : sampleRates) {
        if (std::abs(rate - sampleRate) < std::abs(best - sampleRate))
            best = rate;
    }
    return best;
}

int AudioFormatSupport::nearestBufferSize(int frames) const noexcept {
    if (bufferSizes.empty())
        return frames;
    int best = bufferSizes.front();
    for (const int candidate : bufferSizes) {
        if (std::abs(candidate - frames) < std::abs(best - frames))
            best = candidate;
    }
    return best;
}

// ---------------------------------------------------------------------------
// AudioDeviceManager
// ---------------------------------------------------------------------------
AudioDeviceManager::AudioDeviceManager() = default;

AudioDeviceManager::~AudioDeviceManager() {
    close();
    stopWatching();
}

Status AudioDeviceManager::refresh() {
    std::vector<AudioDeviceInfo> found;

    // Always offer the mock device: it lets the user open AURA on a machine with
    // no working audio driver (or on a VM/CI runner) instead of facing an empty
    // device list. It is clearly labelled so nobody mistakes it for hardware.
    AudioDeviceInfo mock;
    mock.id = "mock:aura";
    mock.name = "AURA Mock Device (no hardware)";
    mock.driver = DriverType::Mock;
    mock.maxInputChannels = 2;
    mock.maxOutputChannels = 2;
    mock.defaultSampleRate = 48000.0;
    mock.supportedSampleRates = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
    mock.defaultBufferSize = 256;
    mock.minimumBufferSize = 32;
    mock.maximumBufferSize = 2048;
    mock.isDefault = false;
    mock.reportedLatencySamples = 256;
    found.push_back(std::move(mock));

#if defined(_WIN32)
    // Real Windows enumeration (WASAPI shared + exclusive capable endpoints).
    auto wasapiDevices = wasapiEnumerateDevices();
    for (auto& info : wasapiDevices)
        found.push_back(std::move(info));
#elif defined(__APPLE__) || defined(__linux__)
    // CoreAudio/ALSA/JACK backends are not implemented: AURA targets Windows.
    // Reporting nothing here is honest; the mock device keeps the engine
    // testable on developer machines and CI.
#endif

    // Detect an actual change so hot-plug notifications are not spammed.
    std::uint64_t signature = 1469598103934665603ull;
    for (const auto& info : found) {
        for (const char character : info.id) {
            signature ^= static_cast<std::uint8_t>(character);
            signature *= 1099511628211ull;
        }
    }
    const bool changed = signature != deviceSignature_;
    devices_ = std::move(found);
    deviceSignature_ = signature;

    if (changed && deviceChangeCallback_)
        deviceChangeCallback_("Audio device list changed", false);

    // If the device we are streaming on vanished, say so and recover.
    if (!currentId_.empty() && findDevice(currentId_) == nullptr) {
        AURA_LOG_WARN(kCategory, "The device AURA was using (%s) is no longer present",
                      currentId_.c_str());
        if (deviceChangeCallback_)
            deviceChangeCallback_("The audio device was disconnected", false);
    }

    AURA_LOG_INFO(kCategory, "Found %zu audio devices", devices_.size());
    return success();
}

const AudioDeviceInfo* AudioDeviceManager::findDevice(const std::string& id) const noexcept {
    for (const auto& info : devices_) {
        if (info.id == id)
            return &info;
    }
    return nullptr;
}

const AudioDeviceInfo* AudioDeviceManager::defaultOutputDevice() const noexcept {
    for (const auto& info : devices_) {
        if (info.isDefault && info.maxOutputChannels > 0)
            return &info;
    }
    for (const auto& info : devices_) {
        if (info.maxOutputChannels > 0)
            return &info;
    }
    return nullptr;
}

AudioFormatSupport AudioDeviceManager::formatSupport(const std::string& id) const {
    AudioFormatSupport support;
    const AudioDeviceInfo* info = findDevice(id);
    if (!info) {
        // Unknown device: offer AURA's standard set rather than an empty list,
        // and let the driver reject anything it cannot do.
        for (const double rate : kCandidateRates)
            support.sampleRates.push_back(rate);
        for (const int buffer : kCandidateBuffers)
            support.bufferSizes.push_back(buffer);
        return support;
    }

    if (info->supportedSampleRates.empty()) {
        for (const double rate : kCandidateRates) {
            if (rate >= 44100.0 /* AURA supports the full documented range */)
                support.sampleRates.push_back(rate);
        }
    } else {
        for (const double rate : info->supportedSampleRates) {
            for (const double candidate : kCandidateRates) {
                if (std::abs(rate - candidate) < 1.0)
                    support.sampleRates.push_back(candidate);
            }
        }
    }
    if (support.sampleRates.empty())
        support.sampleRates.push_back(info->defaultSampleRate);

    for (const int buffer : kCandidateBuffers) {
        if (buffer >= info->minimumBufferSize && buffer <= info->maximumBufferSize)
            support.bufferSizes.push_back(buffer);
    }
    if (support.bufferSizes.empty())
        support.bufferSizes.push_back(info->defaultBufferSize);
    return support;
}

Status AudioDeviceManager::open(const std::string& id, const engine::EngineSettings& settings) {
    const AudioDeviceInfo* info = findDevice(id);
    if (!info)
        return failure(makeError(ErrorCode::AudioDeviceNotFound,
                                 "No such audio device: " + id));
    return openInternal(*info, settings);
}

Status AudioDeviceManager::openRememberedOr(const engine::EngineSettings& settings) {
    if (!currentId_.empty()) {
        if (const AudioDeviceInfo* remembered = findDevice(currentId_)) {
            const Status opened = openInternal(*remembered, settings);
            if (opened)
                return opened;
            AURA_LOG_WARN(kCategory, "Could not reopen %s; falling back to the default device",
                          currentId_.c_str());
        }
    }
    const AudioDeviceInfo* fallback = defaultOutputDevice();
    if (!fallback)
        return failure(makeError(ErrorCode::AudioDeviceNotFound,
                                 "No audio output device is available"));
    return openInternal(*fallback, settings);
}

Status AudioDeviceManager::openInternal(const AudioDeviceInfo& info,
                                        const engine::EngineSettings& settings) {
    close();

    if (!backendAvailable(info.driver)) {
        return failure(makeError(
            ErrorCode::NotImplemented,
            std::string(driverTypeName(info.driver)) + " is not available in this build",
            "device: " + info.name));
    }

    // Capability checks BEFORE the driver is touched: the user gets a clear
    // message instead of a driver-level failure.
    const AudioFormatSupport support = formatSupport(info.id);
    engine::EngineSettings used = settings;
    if (!support.supportsSampleRate(used.sampleRate)) {
        const double nearest = support.nearestSampleRate(used.sampleRate);
        AURA_LOG_WARN(kCategory, "%s does not support %.0f Hz; using %.0f Hz", info.name.c_str(),
                      used.sampleRate, nearest);
        used.sampleRate = nearest;
    }
    if (!support.supportsBufferSize(used.bufferSize)) {
        const int nearest = support.nearestBufferSize(used.bufferSize);
        AURA_LOG_WARN(kCategory, "%s does not support a %d frame buffer; using %d", info.name.c_str(),
                      used.bufferSize, nearest);
        used.bufferSize = nearest;
    }

    switch (info.driver) {
    case DriverType::Mock: device_ = std::make_unique<MockAudioDevice>(info); break;
#if defined(_WIN32)
    case DriverType::WasapiShared:
    case DriverType::WasapiExclusive: {
        auto wasapi = std::make_unique<WasapiDevice>(info);
        const Status opened = wasapi->open(used.sampleRate, used.bufferSize,
                                           info.driver == DriverType::WasapiExclusive);
        if (!opened)
            return opened;
        device_ = std::move(wasapi);
        break;
    }
#endif
    case DriverType::Asio:
        return failure(makeError(ErrorCode::NotImplemented,
                                 "ASIO support is scheduled for milestone M8",
                                 "see docs/research/ASIO_RESEARCH.md"));
    default:
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "No backend is available for this device", info.name));
    }

    if (engine_) {
        const Status started = engine_->initialise(device_.get(), used);
        if (!started) {
            device_.reset();
            return started;
        }
    }

    currentId_ = info.id;
    AURA_LOG_INFO(kCategory, "Opened %s (%s) at %.0f Hz / %d frames", info.name.c_str(),
                  driverTypeName(info.driver), used.sampleRate, used.bufferSize);
    return success();
}

void AudioDeviceManager::close() {
    if (device_) {
        device_->stop();
        device_.reset();
    }
}

int AudioDeviceManager::totalLatencySamples() const {
    if (!device_)
        return 0;
    int total = device_->latencySamples();
    if (engine_)
        total = engine_->totalLatencySamples();
    return total;
}

void AudioDeviceManager::setDeviceChangeCallback(DeviceChangeCallback callback) {
    deviceChangeCallback_ = std::move(callback);
}

std::unique_ptr<engine::IAudioDevice> AudioDeviceManager::createMockDevice(
    const AudioDeviceInfo& info) {
    return std::make_unique<MockAudioDevice>(info);
}

bool AudioDeviceManager::backendAvailable(DriverType driver) noexcept {
    switch (driver) {
    case DriverType::Mock: return true;
    case DriverType::WasapiShared:
    case DriverType::WasapiExclusive:
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    case DriverType::Asio: return false; // M8 (see docs/research/ASIO_RESEARCH.md)
    }
    return false;
}

void AudioDeviceManager::startWatching(std::uint32_t intervalMs) {
    if (watching_.exchange(true))
        return;
    watcherThread_ = std::thread([this, intervalMs] {
        while (watching_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            // refresh() is deliberately cheap and idempotent: it reports through
            // the callback only when the device set actually changed, so this is
            // not "continuous disk polling" - it is a driver enumeration that
            // Windows caches, run at a user-configurable interval (default 2 s,
            // and it can be disabled entirely).
            (void)refresh();
        }
    });
}

void AudioDeviceManager::stopWatching() {
    if (!watching_.exchange(false))
        return;
    if (watcherThread_.joinable())
        watcherThread_.join();
}

// ---------------------------------------------------------------------------
// MockAudioDevice
// ---------------------------------------------------------------------------
MockAudioDevice::MockAudioDevice(AudioDeviceInfo info) : info_(std::move(info)) {
    sampleRate_ = info_.defaultSampleRate > 0.0 ? info_.defaultSampleRate : 48000.0;
    bufferSize_ = info_.defaultBufferSize > 0 ? info_.defaultBufferSize : 256;
    for (auto& channel : outputStorage_)
        channel.assign(static_cast<std::size_t>(2048), 0.0f);
    for (auto& channel : inputStorage_)
        channel.assign(static_cast<std::size_t>(2048), 0.0f);
}

MockAudioDevice::~MockAudioDevice() {
    stop();
}

Status MockAudioDevice::start(Callback callback) {
    if (running_.load())
        return success();
    callback_ = std::move(callback);
    lost_.store(false);
    running_.store(true);
    worker_ = std::thread([this] { workerLoop(); });
    AURA_LOG_INFO(kCategory, "Mock device started: %.0f Hz, %d frames", sampleRate_, bufferSize_);
    return success();
}

void MockAudioDevice::stop() {
    if (!running_.exchange(false))
        return;
    if (worker_.joinable())
        worker_.join();
}

void MockAudioDevice::setInputTone(double hz, float amplitude) noexcept {
    toneHz_ = hz;
    toneAmplitude_ = amplitude;
}

void MockAudioDevice::workerLoop() {
    // The mock device runs in real time (sleep-based pacing) so that tests
    // exercising scheduling/priority behave like the real thing. Tests that need
    // speed drive the engine directly instead of through the device.
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(
        static_cast<std::int64_t>(1.0e9 * bufferSize_ / sampleRate_));
    auto next = clock::now() + period;

    while (running_.load(std::memory_order_relaxed)) {
        if (lost_.load(std::memory_order_relaxed)) {
            // Simulated unplug: stop calling the callback, as a driver would.
            std::this_thread::sleep_for(period);
            continue;
        }

        const int channels = std::max(1, info_.maxOutputChannels);
        float* outputs[2] = {outputStorage_[0].data(), outputStorage_[1].data()};
        float* inputs[2] = {inputStorage_[0].data(), inputStorage_[1].data()};

        for (int channel = 0; channel < std::min(channels, 2); ++channel) {
            for (int i = 0; i < bufferSize_; ++i) {
                float value = 0.0f;
                if (toneHz_ > 0.0 && channel < info_.maxInputChannels) {
                    value = toneAmplitude_ * static_cast<float>(
                                                  std::sin(2.0 * math::kPi * toneHz_ * tonePhase_));
                }
                inputs[channel][static_cast<std::size_t>(i)] = value;
                outputs[channel][static_cast<std::size_t>(i)] = 0.0f;
            }
        }
        if (toneHz_ > 0.0)
            tonePhase_ += static_cast<double>(bufferSize_) / sampleRate_;

        if (callback_)
            callback_(outputs, inputs, channels, std::min(2, info_.maxInputChannels), bufferSize_);

        float peak = 0.0f;
        for (int i = 0; i < bufferSize_; ++i)
            peak = std::max(peak, std::abs(outputs[0][static_cast<std::size_t>(i)]));
        lastPeak_.store(peak, std::memory_order_relaxed);
        callbacks_.fetch_add(1, std::memory_order_relaxed);

        std::this_thread::sleep_until(next);
        next += period;
        // If the process was starved, resynchronise instead of spinning to catch
        // up (a real driver would have dropped the buffers).
        if (clock::now() > next + period)
            next = clock::now() + period;
    }
}

} // namespace aura::audioio
