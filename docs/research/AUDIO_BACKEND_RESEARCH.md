# AUDIO_BACKEND_RESEARCH.md

**Status:** Accepted · **Supports:** ADR-0005 · **Code:** `aura::audioio`, `aura::engine::IAudioDevice`

## Question

How does AURA talk to audio hardware on Windows 10/11 x64 with the lowest
defensible latency, correct device enumeration, and predictable recovery when the
device changes underneath us?

## Options considered

| Option | Licence | Latency | Enumeration/capability | Verdict |
|--------|---------|---------|------------------------|---------|
| **WASAPI shared** (WASAPI/direct) | OS API | ~10–30 ms | full: format support, mix format, period | **Accepted** — default, mixes with other apps |
| **WASAPI exclusive** | OS API | ~3–10 ms | full: `IsFormatSupported` negotiation per format | **Accepted** — opt-in per device |
| **ASIO** | SDK: GPLv3 or proprietary (Oct 2025) | ~1–5 ms | per-driver (`ASIOGetChannels`, buffer sizes) | **Accepted, opt-in** — `AURA_ENABLE_ASIO` |
| DirectSound | legacy | poor | poor | Rejected — deprecated, no capability queries |
| WaveOut / MME | legacy | very poor | poor | Rejected — kept only inside other tools |
| XAudio2 | OS API (MIT-ish runtime) | good for games | limited for pro audio | Rejected — designed around pre-mixed game audio |
| RtAudio / PortAudio / miniaudio | permissive | wraps the above | hides per-device detail | Rejected — we need the raw capability data |

## Findings that shaped the design

1. **WASAPI shared mode always resamples** to the device mix format. That is fine
   for a default and wrong for mastering; it is why the device is described to the
   user as "Shared (system mixer)" rather than silently labelled by its sample rate.
2. **WASAPI exclusive requires format negotiation.** The application must ask the
   driver whether the combination of sample rate, bit depth and channel count is
   accepted (`IAudioClient::IsFormatSupported`) and then own that format. AURA
   returns the refused combination to the UI as a diagnosable error, never as a
   silent fallback.
3. **`E_BUFFER_SIZE_NOT_ALIGNED` after `Initialize` is normal** in exclusive mode:
   the driver reports the alignment it actually needs and the client must be
   recreated with it (`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` → recompute
   `hnsPeriodicity` from `GetBufferSize()` and initialise again). AURA implements
   exactly one retry with the driver's value.
4. **Event-driven, not polling.** `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` plus
   `SetEventHandle` gives the audio thread a real event to wait on, which removes
   the spin-wait that push-mode clients need and is the single biggest reliability
   win on Windows.
5. **Thread priority matters.** The callback thread is raised with the MMCSS
   "Pro Audio" task (`AvSetMmThreadCharacteristicsW(L"Pro Audio", ...)`) and
   `AvRevertMmThreadCharacteristics` on shutdown; without it, a DAW competes with
   compositor and network threads for a core.
6. **Devices disappear.** USB interfaces are unplugged, Bluetooth headsets are
   switched, sample rates change from the Windows sound panel. AURA registers
   `IMMNotificationClient` for `OnDeviceStateChanged`,
   `OnDefaultDeviceChanged` and `OnPropertyValueChanged`, and the engine treats a
   lost device as a recoverable state (`deviceLost()`), not a crash: the transport
   keeps its position, the stream is torn down, and the UI offers to retry or pick
   another endpoint.
7. **Loopback capture is a separate mode** (`AUDCLNT_STREAMFLAGS_LOOPBACK`) and is
   *not* implemented yet; AURA currently opens render endpoints only. Capture is
   tracked as an explicit gap, not an accident: `WasapiDevice` never claims input.
8. **ASIO's licence changed in October 2025** from "GPL-incompatible, contract
   required" to GPLv3-or-proprietary. That makes it usable by AURA (which is
   GPL-3.0-or-later) without a signed agreement, but the SDK is still *not*
   vendored: the build expects headers at `AURA_ASIO_SDK_DIR` and every entry point
   returns `NotImplemented` with a clear message when the option is off.

## Decision

`IAudioDevice` is the only interface the engine knows:

```cpp
struct IAudioDevice {
    virtual Status start(Callback) = 0;
    virtual void stop() = 0;
    virtual std::string name() const = 0;
    virtual double sampleRate() const = 0;   // what the driver GRANTED
    virtual int bufferSize() const = 0;      // what the driver GRANTED
    virtual int latencySamples() const = 0;  // both directions, driver-reported
};
```

Backends: `WasapiDevice` (shared/exclusive), `AsioDevice` (opt-in), `MockAudioDevice`
(real thread, deterministic timing — used by tests and by the CI job that has no
sound card at all).

## Consequences and accepted costs

* We own COM lifetime, HRESULT mapping and the retry path; there is no upstream to
  blame when a driver misbehaves.
* AURA asks for `bufferSize` but always reports back what the device **granted**
  (a driver may refuse 2048 and hand back 4096) — the engine sizes from the granted
  value, never from the request.
* Exclusive mode is a real user-visible mode with real user-visible failures
  ("another application is using this device"); the UI must explain it rather than
  hide it.
* Until capture lands, the input path exists in the engine (monitoring, recording
  buffers) but has no hardware source.

## Open questions

* Should AURA offer WASAPI **push** mode as a fallback for devices with broken event
  handling? (Deferred; no report of a device that needs it yet.)
* Should the exclusive-mode format list include 32-bit float? (Currently: yes if the
  driver accepts it, because it avoids a conversion.)

## Sources

* Microsoft, *Core Audio APIs* — WASAPI client model, shared/exclusive modes,
  `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`, event-driven buffering.
* Microsoft, *IMMNotificationClient* — device change notifications.
* Microsoft, *AvSetMmThreadCharacteristics* — MMCSS "Pro Audio" task.
* Steinberg, ASIO SDK relicensing (October 2025) — GPLv3-or-proprietary.
* Kodi Wiki, *Windows audio* — practical differences between shared and exclusive
  modes, and why exclusive mode bypasses the OS mixer and resampler.
* donyaquick.com, *Minimizing Audio Latency on Windows 10 with WASAPI* — the
  user-visible exclusive-mode settings and their failure modes.
