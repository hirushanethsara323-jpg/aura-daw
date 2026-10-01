// ============================================================================
// AURA DAW - audioio/AsioDevice.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ASIO backend - DECLARED, NOT IMPLEMENTED (milestone M8).
//
// Why it is here at all: the brief requires ASIO with real driver enumeration,
// and the honest way to deliver "not yet" is a typed, documented backend that
// returns ErrorCode::NotImplemented with a clear reason, rather than an #ifdef
// that silently pretends the driver does not exist.
//
// LICENCE (researched October 2025 - see docs/research/ASIO_RESEARCH.md):
//   The ASIO SDK was relicensed by Steinberg to GPL-3.0-or-later (with a
//   proprietary dual-licence option retained). AURA is GPL-3.0-or-later, so the
//   SDK is compatible - but the SDK must still be obtained by the builder, which
//   is why AURA_ENABLE_ASIO defaults to OFF and the headers are never vendored
//   in this repository.
//
// IMPLEMENTATION PLAN (M8), so the work is specified rather than vague:
//   1. Load the driver COM server (CoCreateInstance with the driver CLSID from
//      the registry key HKLM\SOFTWARE\ASIO\<driver>).
//   2. asioInit / asioGetChannels / asioGetBufferSize / asioGetSampleRate.
//   3. Register the buffer-switch callback with asioCreateBuffers and drive
//      AURA's IAudioDevice callback from it.
//   4. Report driver-reported input/output latency through latencySamples().
//   5. Surface ASIO's own error strings (ASIOGetErrorString) in AuraError.detail.
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aura/audioio/AudioDeviceManager.hpp"
#include "aura/core/Errors.hpp"
#include "aura/engine/AudioEngine.hpp"

namespace aura::audioio {

/// True when this build was compiled with AURA_ENABLE_ASIO and the SDK headers
/// were found. Always false in the default configuration.
[[nodiscard]] bool asioBackendCompiled() noexcept;

/// Enumerates installed ASIO drivers from the registry. Returns an empty list
/// when the backend is not compiled in (and logs why).
std::vector<AudioDeviceInfo> asioEnumerateDrivers();

/// ASIO device. Present so the device manager can be written once; every method
/// reports its status through AuraError rather than failing silently.
class AsioDevice final : public engine::IAudioDevice {
public:
    explicit AsioDevice(AudioDeviceInfo info);
    ~AsioDevice() override;

    Status open(double sampleRate, int bufferSize);
    Status start(Callback callback) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const noexcept override { return running_; }
    [[nodiscard]] std::string name() const override { return info_.name; }
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }
    [[nodiscard]] int bufferSize() const noexcept override { return bufferSize_; }
    [[nodiscard]] int numOutputChannels() const noexcept override { return info_.maxOutputChannels; }
    [[nodiscard]] int numInputChannels() const noexcept override { return info_.maxInputChannels; }
    [[nodiscard]] int latencySamples() const noexcept override { return reportedLatency_; }

private:
    AudioDeviceInfo info_;
    double sampleRate_ = 48000.0;
    int bufferSize_ = 256;
    int reportedLatency_ = 0;
    bool running_ = false;
};

} // namespace aura::audioio
