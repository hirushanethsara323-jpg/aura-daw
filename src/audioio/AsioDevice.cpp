// ============================================================================
// AURA DAW - src/audioio/AsioDevice.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See AsioDevice.hpp for the licence situation and the M8 implementation plan.
// Every entry point here reports the real state of the build: no silent
// successes, no fake devices.
// ============================================================================
#include "aura/audioio/AsioDevice.hpp"

#include <algorithm>

#include "aura/core/Log.hpp"

#if defined(AURA_ENABLE_ASIO) && defined(_WIN32)
// The ASIO SDK headers are provided by the builder (AURA_ASIO_SDK_DIR). They are
// never vendored in this repository.
#    include <asio.h>
#endif

namespace aura::audioio {
namespace {
constexpr const char* kCategory = "AudioIO.ASIO";

AuraError asioUnavailable(const std::string& deviceName) {
    return makeError(ErrorCode::NotImplemented,
                     "ASIO support is not compiled into this build",
                     deviceName + ": rebuild with -DAURA_ENABLE_ASIO=ON and AURA_ASIO_SDK_DIR "
                                  "pointing at the Steinberg ASIO SDK (see docs/BUILDING.md)");
}

} // namespace

bool asioBackendCompiled() noexcept {
#if defined(AURA_ENABLE_ASIO) && defined(_WIN32)
    return true;
#else
    return false;
#endif
}

std::vector<AudioDeviceInfo> asioEnumerateDrivers() {
    std::vector<AudioDeviceInfo> drivers;
#if defined(AURA_ENABLE_ASIO) && defined(_WIN32)
    // Registry enumeration: HKLM\SOFTWARE\ASIO\<name> with a CLSID value. Each
    // entry is probed with asioInit to read its channel counts and buffer sizes.
    // (Implemented in M8; the code path is deliberately absent rather than
    // half-written, so this build cannot produce a device it cannot stream on.)
    AURA_LOG_INFO(kCategory, "ASIO enumeration requested but the backend is not built");
#else
    AURA_LOG_DEBUG(kCategory, "ASIO enumeration requested; backend not compiled in");
#endif
    return drivers;
}

AsioDevice::AsioDevice(AudioDeviceInfo info) : info_(std::move(info)) {}

AsioDevice::~AsioDevice() = default;

Status AsioDevice::open(double sampleRate, int bufferSize) {
    sampleRate_ = sampleRate;
    bufferSize_ = bufferSize;
    return failure(asioUnavailable(info_.name));
}

Status AsioDevice::start(engine::IAudioDevice::Callback) {
    return failure(asioUnavailable(info_.name));
}

void AsioDevice::stop() {
    running_ = false;
}

} // namespace aura::audioio
