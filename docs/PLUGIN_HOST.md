# PLUGIN_HOST.md

How AURA hosts plug-ins, what it verifies, and what it deliberately does not do
yet. The format research and the licensing analysis behind the choices are in
[`research/PLUGIN_FORMAT_RESEARCH.md`](research/PLUGIN_FORMAT_RESEARCH.md) and
[`LICENSES.md`](LICENSES.md).

`plugin::PluginHost` (`include/aura/plugin/PluginHost.hpp`) is the **only** header in
the tree that knows a plug-in format exists: no SDK type appears in the engine, the
mixer or the UI. Everything a plug-in is, from the rest of AURA's point of view, is
one of the types in the table below.

## What this build can host

| Format | State | How it is decided |
|--------|-------|-------------------|
| `Builtin` | always | AURA's own processors and `aura.basic`; no third-party code involved |
| `VST 3` | **implemented** behind `-DAURA_ENABLE_VST3=ON` (off by default) | `hostSupports(PluginFormat::Vst3)` is the only thing the UI asks |
| `CLAP` | **not written** — the option exists, the adapter does not | `hostSupports` returns false; a scan still *lists* the bundle with the reason |

A build that cannot host a format says so instead of failing obscurely: a scan
records the bundle as `incompatible` with the text "hosting is not enabled in this
build", and `instantiatePlugin()` returns `ErrorCode::NotImplemented` with a message
that names the format and the CMake option. `tests/plugin/PluginTests.cpp` asserts
both directions of that contract, so the honest answer cannot rot.

## Enabling VST 3

```bash
cmake -S . -B build-vst3 -DAURA_ENABLE_VST3=ON -DAURA_BUILD_TESTS=ON
cmake --build build-vst3 -j
cd build-vst3 && ctest            # 143 tests, including the six VST 3 hosting cases
```

What that does, and why:

* The Steinberg VST 3 SDK is **fetched, never vendored**, pinned to
  `AURA_VST3_SDK_TAG` = `v3.8.1_build_84` (shallow clone, only the `base`, `cmake`,
  `pluginterfaces` and `public.sdk` submodules - 38 MB instead of 247 MB). This
  repository stays free of third-party licensed trees, and the build is reproducible
  from the tag. VST 3.8 is MIT-licensed; 3.7 and earlier were GPLv3-or-proprietary.
* To build against a checkout you already have (no network, or a patched SDK), point
  CMake at it: `-DFETCHCONTENT_SOURCE_DIR_VST3SDK=/path/to/vst3sdk`. CI uses this to
  keep a job fast; the default path is exercised by the ordinary fetch.
* VSTGUI and the SDK's own examples are switched off. AURA draws its own UI and does
  not need the SDK's widget toolkit; the sample plug-ins cannot even be built without
  it, which is one more reason the test fixture is ours.
* Two SDK libraries are linked: `sdk_hosting` (module loading, `ProcessData`,
  parameter queues) and `sdk_common` (state streams, string conversion). They are
  `PUBLIC` on the static engine library because a static library's consumers have to
  resolve the symbols its objects reference.
* Four SDK sources a host needs are **not** in any SDK library and are compiled into
  the object library `aura_vst3_sdk_bridge`: `plugprovider.cpp` (component/controller
  creation and connection), `common/memorystream.cpp` (the in-memory `IBStream` state
  goes through), and the platform module loader (`module_linux.cpp`,
  `module_win32.cpp` or `module_mac.mm`). SDK headers are included as `SYSTEM`, and
  the bridge does not inherit `-Werror`: the warning gate is a statement about AURA's
  code, and gating the build on third-party warning style would either weaken it
  globally or mean patching a fetched tree - both worse.

## The surface

| Type | Role |
|------|------|
| `PluginDescriptor` | what a scan found: stable id, name, vendor, version, format, is-instrument, category (+ best-effort `categoryId`), path, channel counts, editor, `incompatible` + reason |
| `PluginUserData` | what the *user* decided: favourite, custom name, category override, last used, blacklist + reason |
| `PluginDatabase` | JSON cache of descriptors + user data; `search()`, `recentlyUsed()`, `blacklistCount()`, `availableCategories()`. A corrupt cache degrades to "rescan", never to a broken app |
| `PluginScanner` | discovery + scanning, in a worker thread; `startScan(database, progress)`, `cancel()`, `waitForCompletion()`, `lastErrors()` |
| `PluginInstance` | a loaded plug-in behind a format-free interface: `prepare/reset/process`, parameters (plain **and** normalised), `latencySamples()`, state, editor handle, failure marker |
| `PluginChain` | ordered instances with per-slot bypass; skips bypassed **and failed** slots; `totalLatencySamples()`; `saveStates()`/`restoreStates()` |
| `instantiatePlugin()` / `hostSupports()` | the honest entry points: load, or explain why not |

## Status

| Capability | State |
|------------|-------|
| Descriptor model, database, search, favourites, blacklist, recently used | implemented + tested |
| Chain ordering, bypass, latency sum, state save/restore, failed-slot skip | implemented + tested |
| **VST 3 scanning in a child process, with timeouts and quarantine** | **implemented + tested** |
| **VST 3 loading: parameters, latency, audio, events, state round trip** | **implemented + tested against a real bundle** |
| CLAP instantiation | not written |
| Editor *window* embedding | not built; `createEditor()` returns the plug-in's own view handle and the shell owns it (M9/M10). A generic parameter list is the fallback |
| Out-of-process **audio** sandbox | not built - see the honest limitation below |

## Scanning

1. **Discover.** Every search path is walked for `.vst3` / `.clap` bundles
   (`findBundleCandidates`). A search path that *is* a bundle counts too: "scan the
   plug-in I just downloaded" is a reasonable request. Defaults come from
   `PluginScanner::defaultSearchPaths()`:

   | Platform | Defaults |
   |----------|----------|
   | Windows | `%ProgramFiles%\Common Files\VST3`, `…\CLAP`, `%ProgramFiles(x86)%\Common Files\VST3` |
   | macOS | `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3` |
   | Linux | `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.vst3` |

2. **Scan in a child process.** `tools/scan_host` loads the bundle, asks its factory
   what it contains and writes one JSON object; the parent kills it if it exceeds
   `timeoutSecondsPerPlugin` (default 10 s) and moves on. Loading an untrusted binary
   runs its static initialisers as a side effect - that is the part that must not
   share an address space with the session.

   ```bash
   aura_scan_host --scan <bundle> --out <file>     # 0 scanned, 1 bad args, 2 built without VST 3, 3 cannot write
   aura_scan_host --version
   ```

   The helper's output is a documented, format-free schema
   (`"schema": "aura.plugin.scan.v1"`): `path`, `ok`, `error`, and a `classes` array
   of `id`, `name`, `vendor`, `version`, `category`, `categoryId`, `format`,
   `isInstrument`, `numInputs`, `numOutputs`, `hasEditor`. Nothing in the parent
   parses SDK types, so a hostile or crashed helper can only produce bad *data*.

3. **Quarantine instead of silence.** A bundle that hangs, crashes or loads without
   an audio class becomes a descriptor with `incompatible = true` and a reason
   ("the scanner did not finish within 10s …", "the scanner crashed while loading
   this bundle (…)", "No VST 3 audio class found in …"). The browser can then show
   the user *why* a plug-in is missing; it does not just vanish. The reason also lands
   in `PluginScanner::lastErrors()`.

4. **Merge, never clobber.** Results are merged with `PluginDatabase::upsert`, keyed
   by descriptor id, and user data (favourites, category overrides, blacklist) lives
   in a separate map keyed the same way. A rescan cannot lose a favourite. A blacklisted
   bundle is skipped (`Options::skipBlacklisted`) *unless* the user asks for it.
   Classes that used to exist in a bundle and no longer do are removed.

5. **Progress and cancel.** `progress(current, total, path)` is called from the worker
   thread; `cancel()` is honoured between bundles. Scanning is asynchronous by design:
   a slow plug-in must not freeze the UI.

If the helper executable is missing (a stripped install, or a build that did not
produce it), `childProcessScanningAvailable()` is false and the scanner falls back to
the **in-process** scan with a warning. Faster, and the honest trade - but it means
the bundle's initialisers run inside AURA, so it is a fallback, not the plan.

## The host contract as implemented

**Lifecycle.** `instantiatePlugin(descriptor, sampleRate, maxBlockSize, numChannels)`
parses the descriptor id (`VST3:<class uid>@<bundle path>` - the id has to be readable
without the SDK), loads the module, creates component + controller through
`PlugProvider`, then: `initialize(hostContext)` → `setupProcessing(kRealtime,
kSample32, maxSamplesPerBlock, sampleRate)` → prepares the state/parameter holders →
`setActive(true)` → `setProcessing(true)`. `reset()` is deactivate + reactivate, which
is the format's way to clear delay lines and reverb tails without rebuilding.

**Audio buffers.** The engine owns the audio, so `ProcessData::prepare()` is called
with a buffer size of **0**: a non-zero value makes `ProcessData` allocate its own
channels and then *silently refuse* `setChannelBuffers()`, which is how a host ends up
feeding a plug-in silence and hearing nothing back. Bus 0 is the track: input and
output share the engine's pointers (in-place processing, which the format allows and
an insert wants). Every other channel - a plug-in's extra channels on bus 0, and every
channel of a sidechain or aux bus - points at its own preallocated, zeroed slice,
allocated in `prepare()`. A bus channel holding a null pointer is a crash waiting for
a plug-in that processes all of them, so there are none. If the wiring is ever
refused, the slot is marked failed and bypassed rather than left half-connected.

**Parameters.** The controller is the plug-in's own: values are written with
`setParamNormalized()` and read back with `getParamNormalized()`, and plain↔normalised
conversion goes through the plug-in's `plainParamToNormalized()` /
`normalizedParamToPlain()`, never through an assumed 0..1 range. Changes also go into
the SDK's lock-free `ParameterChangeTransfer` ring, which the audio thread drains into
`ProcessData::inputParameterChanges`, so the DSP sees them on the next block. A
plug-in that reports `kParamValuesChanged` / `kParamTitlesChanged` gets its parameter
list re-read.

**Latency.** `getLatencySamples()` is read at activation, when the plug-in asks for a
restart with `kLatencyChanged`, after a state restore (state can carry a
latency-setting value), and at the end of a block if a change arrived during
processing. `latencySamples()` answers from a cached atomic and re-reads first if a
notification is pending, because the graph asks for it *between* blocks when it works
out delay compensation - one block late means one block out of alignment.

**State.** `saveState()` returns the host's own framing, not the plug-in's raw stream:
`[u32 component length][component bytes][u32 controller length][controller bytes]`,
so a plug-in that writes nothing, or a controller that has no state, is still
unambiguous. Restoring writes the component first, hands the same bytes to the
controller via `setComponentState()` (the controller's display must agree with the
DSP), then restores the controller's own state. A single-component plug-in - the
common case for effects - is one object serving both interfaces, and the code path is
the same one `PlugProvider` hands back. `PluginChain::saveStates()` wraps this per
slot, hex-encoded, for the project file (see [`PROJECT_FORMAT.md`](PROJECT_FORMAT.md)).

**Events.** MIDI note on/off and poly pressure are converted to VST 3 events with
sample offsets, and note-on with velocity 0 is sent as note-off (both are legal, and
plug-ins differ in which they expect). Control changes and pitch bend are **not**
translated: VST 3 replaced them with parameter changes and note expressions, and a
converter that guesses would be wrong in a way the user hears. They are delivered to
instruments through the MIDI path in M7. Note *output* events (arpeggiators feeding
other tracks) are read by the chain today and consumed in M7.

**Failure handling.** A plug-in that returns anything other than `kResultOk` from
`process()` is marked failed and skipped for the rest of the session - one bad
plug-in must not be able to silence a session, and calling it again every block would
turn a glitch into a stutter. The chain skips bypassed and failed slots, and
`totalLatencySamples()` counts only the slots that will actually process.

## Known limits

* **A crashing plug-in crashes AURA.** Isolation covers *scanning*, not *playback*.
  A loaded plug-in runs in the audio engine's address space; the mitigation today is
  autosave + crash recovery, and this paragraph. `PluginInstance` is the boundary that
  makes the fix mechanical rather than structural - moving a loaded plug-in into a
  helper process is a matter of replacing the transport (shared memory + IPC) instead
  of rewriting the mixer. A DAW that claims sandboxing it does not have is worse than
  one that admits the gap.
* **Only bus 0 of the plug-in is the track.** Sidechain and aux buses are wired to
  silence, not routed: the graph has no sidechain send yet (M7/M8). A plug-in with a
  sidechain input will therefore hold its detector at silence instead of opening.
* **A mono track feeding a stereo plug-in leaves the second channel silent** rather
  than duplicating it. That is a decision, not an oversight; a *warning* is logged at
  `prepare()` so it is visible. Channel-count adaptation upstream of the insert
  (mono→stereo by duplication) belongs to the graph, not the adapter.
* **Parameter changes are block-aligned.** They are applied at the start of the next
  block (offset 0), which the SDK's transfer ring supports; sample-accurate parameter
  ramps inside a block are not implemented.
* **No note expressions, no CC mapping, no note output events** (M7).
* **CLAP is not implemented.** The option is reserved and the enum exists so the
  database can list CLAP bundles with the reason; there is no adapter behind it.
* **The test fixture's bundle layout is implemented for Linux and Windows**
  (`Contents/<arch>-linux/<name>.so`, `Contents/x86_64-win/<name>.vst3`); macOS
  warns and skips, since nothing builds the fixture there. The adapter itself is
  platform-neutral.
* **Windows child processes have no job object yet**: a hung scanner is killed by
  handle, but a plug-in that spawns its own children is not cleaned up. POSIX gets a
  process group; the Windows path documents the gap in `src/plugin/ChildProcess.cpp`.
* **A crash on Windows is not always quick.** A crashing process can sit in Windows
  Error Reporting for seconds before it finally dies, so a bundle that crashes during
  a scan may be reported as a *timeout* rather than a crash. Both outcomes quarantine
  the bundle with a reason and neither touches the session, which is why the timeout
  is the defence that matters - and why the scanner's own crash test hook uses a
  fail-fast exception instead of `abort()`: a test that waits for the crash reporter
  is a test of the crash reporter.

## Troubleshooting

| Symptom | Cause | What to do |
|---------|-------|------------|
| A plug-in is missing from the browser | It was quarantined | Read `incompatibleReason` (browser tooltip) or `PluginScanner::lastErrors()`; the text says whether it timed out, crashed or has no audio class |
| A plug-in is in the browser but will not load | Quarantined, or its format is not compiled in | The instantiate error names the reason and the CMake option |
| Scanning takes a long time | A plug-in waiting for a licence dialog or opening its own UI | The per-plug-in timeout ends it (default 10 s); shorten `Options::timeoutSecondsPerPlugin` if you prefer quarantining sooner |
| The scanner "hangs" a machine | Rare, but it is why the scan is a child process | Kill the helper process; the parent records a timeout and carries on |
| Every plug-in vanished from the browser | The cache is unreadable or the search path is wrong | The cache degrades to "rescan" by design; check `defaultSearchPaths()` against where your plug-ins are |
| Tests hang under `[plugin]` | `AURA_SCAN_BEHAVIOUR=hang` / `=crash` set in the environment | Those hooks exist for the scanner-survival test; unset them |

## Testing

```bash
# The VST 3 suite, against a real bundle built from this repository:
cd build-vst3 && ctest -R "vst3|scanner|plug" --output-on-failure
./tests/aura_tests "[vst3]" -s        # every assertion, with expansions
```

CI runs it in two configurations: `Linux VST 3 hosting` (the row that fetches the
SDK, builds and runs everything, then scans the bundle with the helper and reads the
JSON back) and the Windows `Debug-VST3` matrix row, which is the same code under MSVC
against a bundle in the Windows layout.

The subject is `tests/plugin/vst3/AuraTestPlugin.cpp`, an original plug-in built as a
proper `.vst3` bundle (not an SDK sample: samples cannot even be built without
VSTGUI). It has two parameters with a **real** plain range (gain 0..2, delay
0..512 samples) so conversion is exercised rather than assumed, a delay line whose
length is exactly what `getLatencySamples()` reports, a note-event counter that
survives a state round trip (which is how "MIDI actually reached the plug-in" becomes
an assertion), state save/restore, a `kLatencyChanged` notification when the delay
parameter moves, and no editor. The tests assert, among other things:

* the descriptor the scanner produces (name, vendor, version, category, id form);
* that 1.5× gain is normalised 0.75 and comes out of `process()` as 1.5× on the first
  block - i.e. the parameter reached the DSP through `ProcessData`, not through a
  private back channel;
* that reported latency is the plug-in's latency *and* that the audio obeys it (an
  impulse comes out exactly 8 samples late), and that the chain reports 8 for delay
  compensation;
* that state saved after setting gain 0.25 / delay 4 restores into a fresh instance;
* that three MIDI notes reach the plug-in (read out of its own state blob);
* that the child-process scanner survives a bundle that hangs or crashes, and that a
  bad bundle becomes a quarantined descriptor instead of an exception.

The suite earns its keep: writing it found three real host bugs, all fixed - the
`ProcessData` buffer-ownership trap (`prepare()` with a non-zero block size silently
disables `setChannelBuffers()`, so the plug-in processed silence), a state-restore
offset that read the length prefix as plug-in data (so no project would ever have
reloaded its plug-in state), and a latency that was not re-read after a restart
notification. Each is now covered by an assertion, which is the point of testing
against the format instead of against a mock that agrees with us.

## Licensing

| Component | Licence | How it reaches the build |
|-----------|---------|--------------------------|
| VST 3 SDK 3.8.1 build 84 | MIT | fetched at the pinned tag by CMake; not in this repository |
| `aura_scan_host`, the adapter, the fixture | GPL-3.0-or-later | part of AURA |

Attribution and the full dependency table: [`LICENSES.md`](LICENSES.md).
