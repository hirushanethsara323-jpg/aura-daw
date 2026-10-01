// ============================================================================
// AURA DAW - audioio/WasapiDevice.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows Audio Session API backend: shared mode (event driven) and exclusive
// mode (AURA owns the endpoint and its format).
//
// Research: docs/research/WASAPI_RESEARCH.md and
// docs/research/LOW_LATENCY_AUDIO_RESEARCH.md.
//
// Notes that shape the implementation:
//   * Shared mode always runs at the endpoint's mix format and resamples
//     anything else in the audio engine (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM is
//     NOT used: AURA would rather resample itself and know the latency).
//   * Exclusive mode must be given a format the endpoint accepts; AURA probes
//     with IsFormatSupported() and picks the best match rather than guessing.
//   * Event-driven streaming (AUDCLNT_STREAMFLAGS_EVENTCALLBACK) is required to
//     hit small buffers: a polling loop cannot reliably sustain 64 frames.
//   * The render thread is a real-time thread: it is created by AURA (not by
//     WASAPI), so thread ownership stays explicit and auditable
//     (docs/ARCHITECTURE.md "thread ownership").
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aura/audioio/AudioDeviceManager.hpp"
#include "aura/core/Errors.hpp"
#include "aura/engine/AudioEngine.hpp"

namespace aura::audioio {

/// Enumerates WASAPI render/capture endpoints (shared mode). Returns an empty
/// list on non-Windows builds.
std::vector<AudioDeviceInfo> wasapiEnumerateDevices();

/// WASAPI endpoint implementation. On non-Windows builds every method returns
/// ErrorCode::NotImplemented so the type can still be referenced by the device
/// manager without a platform #ifdef at the call site.
class WasapiDevice final : public engine::IAudioDevice {
public:
    explicit WasapiDevice(AudioDeviceInfo info);
    ~WasapiDevice() override;

    /// Opens the endpoint. Must be called before start(). `exclusive` selects
    /// AUDCLNT_SHAREMODE_EXCLUSIVE and negotiates the format.
    Status open(double sampleRate, int bufferSize, bool exclusive);

    Status start(Callback callback) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::string name() const override { return info_.name; }
    [[nodiscard]] double sampleRate() const noexcept override;
    [[nodiscard]] int bufferSize() const noexcept override;
    [[nodiscard]] int numOutputChannels() const noexcept override;
    [[nodiscard]] int numInputChannels() const noexcept override;
    [[nodiscard]] int latencySamples() const noexcept override;

    /// True when the endpoint disappeared while streaming (unplugged, disabled,
    /// driver reset). The device manager polls this to recover automatically.
    [[nodiscard]] bool deviceLost() const noexcept;

    /// True while exclusive mode is active (reported in the UI so users can see
    /// which mode is really running).
    [[nodiscard]] bool isExclusive() const noexcept;

    struct Impl;

private:
    AudioDeviceInfo info_;
    std::unique_ptr<Impl> impl_;
};

} // namespace aura::audioio
