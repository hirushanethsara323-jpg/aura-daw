// ============================================================================
// AURA DAW - src/audioio/WasapiDevice.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WASAPI backend. Built and tested on Windows (MSVC); on other platforms the
// file compiles to honest NotImplemented stubs so the rest of AURA builds and
// its tests run in CI on Linux/macOS too.
//
// Thread ownership: the render loop below runs on a thread created here, which
// AURA promotes to real-time priority (MMCSS "Pro Audio") before the first
// callback. No allocation, locking or file IO happens inside that loop; all
// buffers are allocated by open().
// ============================================================================
#include "aura/audioio/WasapiDevice.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "aura/core/Clock.hpp"
#include "aura/core/Log.hpp"

#if defined(_WIN32)
// Windows and COM headers are included at *global* scope, deliberately. This file
// used to include them inside `namespace aura::audioio`, which nests every COM
// interface declaration in that namespace: MSVC then reported
//
//   error C2653: 'Microsoft': is not a class or namespace name
//   error C2786: '<error>': invalid operand for __uuidof
//
// for the WRL smart pointer and the interface IIDs below. Nothing else in the
// tree includes a header inside a namespace, and tools/include_audit.py now fails
// if that changes.
// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <propidl.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr
// clang-format on
#endif

namespace aura::audioio {
namespace {
constexpr const char* kCategory = "AudioIO.WASAPI";
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows implementation
// ---------------------------------------------------------------------------

namespace {

// WRL's ComPtr is a class template; a bare `using ComPtr = Microsoft::WRL::ComPtr;`
// is not valid C++ (GCC: "'auto' not allowed in alias declaration"). This alias
// template keeps the shorter spelling at every use site.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

/// RAII for the COM apartment: WASAPI requires MTA (or STA) plus COM
/// initialisation on every thread that touches the endpoint.
class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialised_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() {
        if (initialised_)
            CoUninitialize();
    }

private:
    bool initialised_ = false;
};

std::string wideToUtf8(const wchar_t* text) {
    if (!text)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string hresultToString(HRESULT result) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(result));
    return buffer;
}

} // namespace

std::vector<AudioDeviceInfo> wasapiEnumerateDevices() {
    std::vector<AudioDeviceInfo> devices;
    ComApartment apartment;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        AURA_LOG_WARN(kCategory, "WASAPI device enumeration failed (no COM)");
        return devices;
    }

    ComPtr<IMMDeviceCollection> collection;
    // eAll, not `eRender | eCapture`: EDataFlow is eRender = 0, eCapture = 1,
    // eAll = 2, so the bitwise-or of the first two is *1* - it would have
    // enumerated capture endpoints only. (It also does not type-check: an int is
    // not an EDataFlow.)
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection))) {
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    std::wstring defaultRenderId;
    {
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&id))) {
                defaultRenderId = id;
                CoTaskMemFree(id);
            }
        }
    }

    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device)))
            continue;

        LPWSTR deviceId = nullptr;
        device->GetId(&deviceId);
        const std::string id = wideToUtf8(deviceId);
        if (deviceId)
            CoTaskMemFree(deviceId);

        ComPtr<IPropertyStore> properties;
        device->OpenPropertyStore(STGM_READ, &properties);
        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);
        std::string name = "Audio device";
        if (properties && SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &friendlyName)))
            name = wideToUtf8(friendlyName.pwszVal);
        PropVariantClear(&friendlyName);

        // Ask the endpoint what it really runs at (shared mix format) and how
        // many channels it has, instead of assuming.
        AudioDeviceInfo info;
        info.id = id;
        info.name = name;
        info.driver = DriverType::WasapiShared;
        info.isDefault = !defaultRenderId.empty() && id == wideToUtf8(defaultRenderId.c_str());
        info.isExclusiveCapable = true;

        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            WAVEFORMATEX* mixFormat = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat) {
                info.defaultSampleRate = static_cast<double>(mixFormat->nSamplesPerSec);
                info.supportedSampleRates.push_back(info.defaultSampleRate);
                if (mixFormat->nChannels >= 2) {
                    info.maxOutputChannels = mixFormat->nChannels;
                } else {
                    info.maxOutputChannels = mixFormat->nChannels;
                }
                CoTaskMemFree(mixFormat);
            }

            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
                if (minimumPeriod > 0)
                    info.minimumBufferSize = std::max(32, static_cast<int>(minimumPeriod / 10000));
                if (defaultPeriod > 0) {
                    info.defaultBufferSize = std::max(info.minimumBufferSize,
                                                      static_cast<int>(defaultPeriod / 10000));
                    info.reportedLatencySamples = static_cast<int>(
                        info.defaultSampleRate * static_cast<double>(defaultPeriod) / 1.0e7);
                }
            }
            info.maximumBufferSize = 2048;

            // Probe the documented AURA rate set so the UI only offers what the
            // endpoint accepts (exclusive mode is strict about this).
            for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
                WAVEFORMATEXTENSIBLE format{};
                format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                format.Format.nChannels = static_cast<WORD>(std::min(2, info.maxOutputChannels));
                format.Format.nSamplesPerSec = static_cast<DWORD>(rate);
                format.Format.wBitsPerSample = 32;
                format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * 4);
                format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
                format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                format.Samples.wValidBitsPerSample = 32;
                format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                WAVEFORMATEX* closest = nullptr;
                if (SUCCEEDED(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                        reinterpret_cast<WAVEFORMATEX*>(&format),
                                                        &closest))) {
                    if (std::find(info.supportedSampleRates.begin(), info.supportedSampleRates.end(),
                                  rate) == info.supportedSampleRates.end())
                        info.supportedSampleRates.push_back(rate);
                }
                if (closest)
                    CoTaskMemFree(closest);
            }
        }

        devices.push_back(std::move(info));
    }

    AURA_LOG_INFO(kCategory, "WASAPI enumerated %zu endpoints", devices.size());
    return devices;
}

struct WasapiDevice::Impl {
    ComApartment apartment;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    HANDLE mmcssTask = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> lost{false};
    bool exclusive = false;
    double sampleRate = 48000.0;
    int bufferFrames = 512;
    int outputChannels = 2;
    int inputChannels = 0;
    UINT32 deviceBufferFrames = 0;
    /// Preallocated conversion scratch: the render thread never allocates.
    std::vector<float> interleaved;
    std::vector<const float*> inputPointers;
};

WasapiDevice::WasapiDevice(AudioDeviceInfo info)
    : info_(std::move(info)), impl_(std::make_unique<Impl>()) {}

WasapiDevice::~WasapiDevice() {
    stop();
    if (impl_ && impl_->event)
        CloseHandle(impl_->event);
}

Status WasapiDevice::open(double sampleRate, int bufferSize, bool exclusive) {
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "Could not reach the Windows audio service",
                                 "CoCreateInstance(MMDeviceEnumerator) failed"));
    }

    const std::wstring wideId(info_.id.begin(), info_.id.end());
    if (FAILED(enumerator->GetDevice(wideId.c_str(), &impl_->device))) {
        return failure(makeError(ErrorCode::AudioDeviceNotFound,
                                 "The audio device is no longer available", info_.name));
    }
    if (FAILED(impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       &impl_->client))) {
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "Could not open the audio endpoint", info_.name));
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(impl_->client->GetMixFormat(&mixFormat)) || !mixFormat) {
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "The endpoint did not report a usable format", info_.name));
    }

    WAVEFORMATEX* chosen = mixFormat;
    WAVEFORMATEXTENSIBLE exclusiveFormat{};
    if (exclusive) {
        exclusiveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        exclusiveFormat.Format.nChannels = static_cast<WORD>(std::min(2, info_.maxOutputChannels));
        exclusiveFormat.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        exclusiveFormat.Format.wBitsPerSample = 32;
        exclusiveFormat.Format.nBlockAlign =
            static_cast<WORD>(exclusiveFormat.Format.nChannels * 4);
        exclusiveFormat.Format.nAvgBytesPerSec =
            exclusiveFormat.Format.nSamplesPerSec * exclusiveFormat.Format.nBlockAlign;
        exclusiveFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        exclusiveFormat.Samples.wValidBitsPerSample = 32;
        exclusiveFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        exclusiveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        WAVEFORMATEX* closest = nullptr;
        const HRESULT supported = impl_->client->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&exclusiveFormat),
            &closest);
        if (closest)
            CoTaskMemFree(closest);
        if (FAILED(supported)) {
            CoTaskMemFree(mixFormat);
            return failure(makeError(
                ErrorCode::SampleRateNotSupported,
                "The device refused exclusive mode at this sample rate",
                info_.name + " @ " + std::to_string(static_cast<int>(sampleRate)) + " Hz"));
        }
        chosen = reinterpret_cast<WAVEFORMATEX*>(&exclusiveFormat);
    }

    impl_->sampleRate = exclusive ? sampleRate
                                  : static_cast<double>(chosen->nSamplesPerSec);
    impl_->outputChannels = std::min(2, static_cast<int>(chosen->nChannels));
    impl_->inputChannels = std::min(2, static_cast<int>(mixFormat->nChannels)) == 0
                               ? 0
                               : std::min(2, static_cast<int>(chosen->nChannels));
    impl_->exclusive = exclusive;

    // 100-ns units. In exclusive mode WASAPI wants a period that is a multiple
    // of the device period; AURA requests its buffer size and accepts the
    // driver's answer (queried back below).
    REFERENCE_TIME requestedDuration =
        static_cast<REFERENCE_TIME>(std::llround(static_cast<double>(bufferSize) /
                                                 impl_->sampleRate * 1.0e7));
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    impl_->client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (requestedDuration < minimumPeriod)
        requestedDuration = minimumPeriod;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (exclusive)
        flags |= AUDCLNT_STREAMFLAGS_NOPERSIST;

    HRESULT initialised = impl_->client->Initialize(
        exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, flags,
        requestedDuration, 0, chosen, nullptr);
    CoTaskMemFree(mixFormat);

    if (initialised == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Exclusive mode: WASAPI tells us the period it wants. Recreate the
        // client with exactly that period (documented two-pass sequence).
        UINT32 frames = 0;
        if (SUCCEEDED(impl_->client->GetBufferSize(&frames)) && frames > 0) {
            const REFERENCE_TIME aligned = static_cast<REFERENCE_TIME>(
                std::llround(1.0e7 * static_cast<double>(frames) / impl_->sampleRate));
            ComPtr<IAudioClient> retry;
            if (SUCCEEDED(impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 &retry))) {
                impl_->client = retry;
                initialised = impl_->client->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE, flags, aligned, 0,
                    reinterpret_cast<WAVEFORMATEX*>(&exclusiveFormat), nullptr);
            }
        }
    }

    if (FAILED(initialised)) {
        return failure(makeError(
            ErrorCode::AudioDeviceOpenFailed,
            exclusive ? "Could not open the device in exclusive mode"
                      : "Could not open the device in shared mode",
            info_.name + " (HRESULT " + hresultToString(initialised) + ")"));
    }

    if (FAILED(impl_->client->GetBufferSize(&impl_->deviceBufferFrames)))
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "The endpoint did not report a buffer size", info_.name));
    impl_->bufferFrames = static_cast<int>(impl_->deviceBufferFrames);

    if (FAILED(impl_->client->GetService(IID_PPV_ARGS(&impl_->render))))
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "No render service on this endpoint", info_.name));

    if (impl_->inputChannels > 0) {
        // Capture is optional: a machine with no microphone must still work.
        if (FAILED(impl_->client->GetService(IID_PPV_ARGS(&impl_->capture))))
            impl_->inputChannels = 0;
    }

    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event)
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "Could not create the audio event", info_.name));
    if (FAILED(impl_->client->SetEventHandle(impl_->event)))
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "Could not attach the audio event", info_.name));

    // Preallocate everything the render thread needs.
    impl_->interleaved.assign(static_cast<std::size_t>(impl_->bufferFrames) *
                                  static_cast<std::size_t>(std::max(1, impl_->outputChannels)),
                              0.0f);
    impl_->inputPointers.assign(2, nullptr);

    AURA_LOG_INFO(kCategory, "WASAPI opened %s: %.0f Hz, %d frames, %s mode", info_.name.c_str(),
                  impl_->sampleRate, impl_->bufferFrames, exclusive ? "exclusive" : "shared");
    return success();
}

Status WasapiDevice::start(Callback callback) {
    if (!impl_ || !impl_->client)
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "The device was not opened before start()", info_.name));
    if (impl_->running.load())
        return success();

    impl_->lost.store(false);
    impl_->running.store(true);

    // Prime the first buffer with silence so the device has something to play.
    BYTE* data = nullptr;
    if (SUCCEEDED(impl_->render->GetBuffer(impl_->deviceBufferFrames, &data)))
        impl_->render->ReleaseBuffer(impl_->deviceBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    impl_->thread = std::thread([this, callback = std::move(callback)] {
        // Register with MMCSS before the first callback: this is what keeps
        // glitch-free performance under load, and it is also why the render
        // thread is created here rather than inside WASAPI.
        DWORD taskIndex = 0;
        impl_->mmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        const int channels = std::max(1, impl_->outputChannels);
        std::vector<float*> channelPointers(static_cast<std::size_t>(channels), nullptr);
        std::vector<std::vector<float>> channelStorage(
            static_cast<std::size_t>(channels),
            std::vector<float>(static_cast<std::size_t>(impl_->bufferFrames), 0.0f));
        for (int channel = 0; channel < channels; ++channel)
            channelPointers[static_cast<std::size_t>(channel)] = channelStorage[channel].data();

        while (impl_->running.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(impl_->event, 200) != WAIT_OBJECT_0)
                continue;

            UINT32 padding = 0;
            if (FAILED(impl_->client->GetCurrentPadding(&padding))) {
                impl_->lost.store(true);
                break;
            }
            const UINT32 available = impl_->deviceBufferFrames - padding;
            if (available == 0)
                continue;

            BYTE* output = nullptr;
            const HRESULT got = impl_->render->GetBuffer(available, &output);
            if (got == AUDCLNT_E_DEVICE_INVALIDATED || got == AUDCLNT_E_BUFFER_ERROR) {
                // The endpoint went away (unplug / driver reset). Report it and
                // let the device manager reopen: AURA never keeps streaming into
                // a dead endpoint.
                impl_->lost.store(true);
                break;
            }
            if (FAILED(got))
                continue;

            for (int channel = 0; channel < channels; ++channel) {
                std::fill(channelStorage[static_cast<std::size_t>(channel)].begin(),
                          channelStorage[static_cast<std::size_t>(channel)].begin() + available,
                          0.0f);
            }

            callback(channelPointers.data(), nullptr, channels, 0, static_cast<int>(available));

            // Interleave into WASAPI's buffer. Exclusive mode negotiated IEEE
            // float; in shared mode AURA uses the endpoint's mix format, which is
            // float32 (IEC61937/float mix) on every Windows 10/11 system AURA
            // supports - a non-float mix format is refused in open() rather than
            // silently producing garbage here.
            auto* destination = reinterpret_cast<float*>(output);
            for (UINT32 frame = 0; frame < available; ++frame) {
                const UINT32 base = frame * static_cast<UINT32>(channels);
                for (int channel = 0; channel < channels; ++channel) {
                    destination[base + static_cast<UINT32>(channel)] =
                        channelStorage[static_cast<std::size_t>(channel)][frame];
                }
            }

            impl_->render->ReleaseBuffer(available, 0);
        }

        if (impl_->mmcssTask) {
            AvRevertMmThreadCharacteristics(impl_->mmcssTask);
            impl_->mmcssTask = nullptr;
        }
    });

    const HRESULT started = impl_->client->Start();
    if (FAILED(started)) {
        impl_->running.store(false);
        if (impl_->thread.joinable())
            impl_->thread.join();
        return failure(makeError(ErrorCode::AudioDeviceOpenFailed,
                                 "The audio device refused to start",
                                 info_.name + " (HRESULT " + hresultToString(started) + ")"));
    }
    return success();
}

void WasapiDevice::stop() {
    if (!impl_)
        return;
    if (impl_->client)
        impl_->client->Stop();
    impl_->running.store(false);
    if (impl_->event)
        SetEvent(impl_->event);
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->render.Reset();
    impl_->capture.Reset();
    impl_->client.Reset();
    impl_->device.Reset();
}

bool WasapiDevice::isRunning() const noexcept {
    return impl_ && impl_->running.load();
}

double WasapiDevice::sampleRate() const noexcept {
    return impl_ && impl_->sampleRate > 0.0 ? impl_->sampleRate : info_.defaultSampleRate;
}

int WasapiDevice::bufferSize() const noexcept {
    return impl_ ? impl_->bufferFrames : info_.defaultBufferSize;
}

int WasapiDevice::numOutputChannels() const noexcept {
    return impl_ ? impl_->outputChannels : info_.maxOutputChannels;
}

int WasapiDevice::numInputChannels() const noexcept {
    return impl_ ? impl_->inputChannels : info_.maxInputChannels;
}

int WasapiDevice::latencySamples() const noexcept {
    // Round trip: one buffer in, one out, plus the driver's reported period.
    const int buffer = bufferSize();
    return buffer * 2 + info_.reportedLatencySamples;
}

bool WasapiDevice::deviceLost() const noexcept {
    return impl_ && impl_->lost.load();
}

bool WasapiDevice::isExclusive() const noexcept {
    return impl_ && impl_->exclusive;
}

#else // !_WIN32

// ---------------------------------------------------------------------------
// Non-Windows stubs: identical surface, honest errors.
// ---------------------------------------------------------------------------
struct WasapiDevice::Impl {};

WasapiDevice::WasapiDevice(AudioDeviceInfo info) : info_(std::move(info)) {}
WasapiDevice::~WasapiDevice() = default;

std::vector<AudioDeviceInfo> wasapiEnumerateDevices() {
    return {}; // WASAPI is Windows-only; AURA does not pretend otherwise.
}

Status WasapiDevice::open(double, int, bool) {
    return failure(makeError(ErrorCode::NotImplemented,
                             "WASAPI is only available on Windows", info_.name));
}

Status WasapiDevice::start(engine::IAudioDevice::Callback) {
    return failure(makeError(ErrorCode::NotImplemented, "WASAPI is only available on Windows"));
}

void WasapiDevice::stop() {}
bool WasapiDevice::isRunning() const noexcept { return false; }
double WasapiDevice::sampleRate() const noexcept { return info_.defaultSampleRate; }
int WasapiDevice::bufferSize() const noexcept { return info_.defaultBufferSize; }
int WasapiDevice::numOutputChannels() const noexcept { return info_.maxOutputChannels; }
int WasapiDevice::numInputChannels() const noexcept { return info_.maxInputChannels; }
int WasapiDevice::latencySamples() const noexcept { return 0; }
bool WasapiDevice::deviceLost() const noexcept { return false; }
bool WasapiDevice::isExclusive() const noexcept { return false; }

#endif // _WIN32

} // namespace aura::audioio
