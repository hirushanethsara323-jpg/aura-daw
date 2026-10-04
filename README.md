# AURA DAW

**A**dvanced **U**nified **R**ecording & **A**udio — an original, production-quality
digital audio workstation for Windows 10/11 x64.

AURA is a real audio application: a real-time engine that renders a real graph of
tracks, inserts, sends and buses, with a real device layer (WASAPI shared/exclusive,
ASIO opt-in), non-destructive recording and editing, a plug-in host surface, and an
offline renderer that reuses the live DSP graph rather than reimplementing it.

> **Status:** engine milestone. The DSP, graph, engine, project, command and
> device layers are implemented and covered by 151 passing test cases (153 `ctest`
> entries including two static audits). The Windows shell (`app/`) is the next
> milestone — until it lands, AURA is a library and a test suite, not a window you
> can click. This is stated up front because the project's rule is to document what
> exists rather than what is planned.

## What works today

| Area | State |
|------|-------|
| DSP suite | Gain, Pan, 5-band EQ, Compressor (with sidechain input), Limiter (1.5 ms look-ahead), Delay (ping-pong), Reverb, Distortion, Oscillator, Noise, ADSR, Level metering, BS.1770 loudness — all unit-tested |
| Audio graph | tracks → inserts → sends → bus → master, ramped faders, solo/mute, cycle detection, latency reporting, plan swapping with a grace window |
| Engine | lock-free command queue from the control thread, sample-accurate clip rendering, instrument rendering, recording, metering, CPU/xrun statistics, offline render + stems through the same graph |
| Devices | `IAudioDevice` abstraction; WASAPI shared/exclusive (event-driven, MMCSS, format negotiation, device-loss recovery); ASIO (opt-in, `AURA_ENABLE_ASIO`); mock device for tests and CI |
| Timeline | 960 PPQN, tempo/timesignature map, 10 snap modes including zero-crossing, markers, SMPTE |
| Project | `.aura` directory format (JSON manifest + media/peaks/cache/backups), atomic save, `.bak` rotation, autosave, crash flag, recovery, media pool with relink |
| Editing model | non-destructive clips: split, trim, slip, fades/crossfades, consolidate, normalise, reverse |
| MIDI | messages, clips, quantise, recording bake, input routing with panic, original built-in poly synth, voice stealing |
| Automation | Read/Touch/Write/Latch, lanes with curve shapes, gesture recorder with thinning, target ranges |
| Commands | ~38 built-in commands with ids, categories and default shortcuts, undo/redo with transactions and bounded history |
| Plug-ins | **VST 3 hosting implemented** (`-DAURA_ENABLE_VST3=ON`, SDK fetched at a pinned tag and MIT-licensed): out-of-process scanning with timeouts and quarantine, parameters through the plug-in's own ranges, latency reporting, MIDI events, state round trip; database with search/favourites/blacklist, chains with order/bypass/latency. CLAP is not written; a build without the SDK says `NotImplemented` instead of failing obscurely. **M7 phases 1–3 landed:** `plugin/sandbox/` holds the shared-memory transport an out-of-process plug-in host speaks over, `aura_plugin_host` — the helper process that runs audio on the other side of it, tested end to end across a real process boundary with 200 blocks returned bit-exact — and `SandboxedPluginInstance`, a `PluginInstance` proxy a `PluginChain` cannot distinguish from an in-process slot. **Nothing chooses one yet:** `PluginHost` still creates in-process adapters, so a crashing plug-in still crashes AURA until the policy plumbing of phase 5 |
| Localisation | catalogue-based, English shipped, missing-key fallback, error-message translation |
| Diagnostics | TRACE→FATAL structured logging, in-memory ring, CPU/xrun reporting, static real-time audit |

## Build

```bash
cmake -S . -B build -DAURA_BUILD_TESTS=ON
cmake --build build -j 2
cd build && ctest --output-on-failure
```

Windows developers: `cmake --preset ci-windows && cmake --build --preset ci-windows`.
Details, options and troubleshooting: [`docs/BUILDING.md`](docs/BUILDING.md).

## Design rules (short version)

1. **The audio thread never allocates, locks, logs or touches a file.** Enforced
   statically (`tools/rt_audit.py`) and dynamically (allocation counter in the
   engine tests).
2. **One DSP implementation.** Export uses the same graph as playback.
3. **Non-destructive by default.** Nothing in the project mutates a media file.
4. **Every decision is written down** in [`docs/research/`](docs/research/) before
   it is implemented.
5. **No unnecessary dependencies** — the production dependency list is the C++
   standard library and the Windows SDK.

## Documentation

| Document | What it covers |
|----------|----------------|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | module map, ownership, data flow |
| [`docs/AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md) | the callback path in detail |
| [`docs/BUILDING.md`](docs/BUILDING.md) | building, options, offline builds |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | workflow, conventions, adding a processor |
| [`docs/TESTING.md`](docs/TESTING.md) | the five test layers |
| [`docs/PROJECT_FORMAT.md`](docs/PROJECT_FORMAT.md) | the `.aura` format as shipped |
| [`docs/DSP.md`](docs/DSP.md) / [`docs/MIDI.md`](docs/MIDI.md) / [`docs/PLUGIN_HOST.md`](docs/PLUGIN_HOST.md) | subsystem references |
| [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) | budgets and how they are measured |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | milestones M0–M12 |
| [`PROJECT_STATUS.md`](PROJECT_STATUS.md) | what works, what is partial, what is not built |
| [`CHANGELOG.md`](CHANGELOG.md) | every change, in the commit that made it |
| [`docs/research/`](docs/research/) | the research and decision record |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | common failures and fixes |
| [`docs/SECURITY.md`](docs/SECURITY.md) / [`docs/LICENSES.md`](docs/LICENSES.md) / [`CONTRIBUTING.md`](CONTRIBUTING.md) | policy |

## Licence

GPL-3.0-or-later — see [`LICENSE`](LICENSE). Third-party notices and the
dependency audit live in [`docs/LICENSES.md`](docs/LICENSES.md).
