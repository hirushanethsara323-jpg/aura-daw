# PLUGIN_HOST.md

How AURA hosts plug-ins today, what it will do next, and what it deliberately does
not do. Research and licensing analysis:
[`research/PLUGIN_FORMAT_RESEARCH.md`](research/PLUGIN_FORMAT_RESEARCH.md).

## The surface

`plugin::PluginHost` is the **only** header in the tree that knows a plug-in format
exists. Nothing else includes a vendor SDK type.

| Type | Role |
|------|------|
| `PluginDescriptor` | what a scan found: id, name, vendor, version, format, is-instrument, category, path |
| `PluginUserData` | what the *user* decided: favourite, custom name, last used, tags, blacklisted + reason |
| `PluginDatabase` | cache of descriptors + user data, JSON-backed, rebuildable; `search(query, instruments, effects, favoritesOnly, includeBlacklisted)`, `recentlyUsed(n)`, `blacklistCount()` |
| `PluginChain` | ordered instances on a track/bus: `add/remove/move`, bypass, summed latency, `saveStates()`/`restoreStates()`, failed-instance skipping |
| `instantiatePlugin(descriptor)` | creates an instance; returns `NotImplemented` with an actionable message when the SDK is not compiled in |

## Status today

| Capability | State |
|------------|-------|
| Descriptor model, database, search, favourites, blacklist, recently used | **implemented + tested** |
| Chain ordering, bypass, latency reporting, state save/restore | **implemented + tested** |
| Failed-instance skip (a plug-in that failed is never called again in that session) | **implemented + tested** |
| VST3 instantiation | **declared, returns `NotImplemented`** until `AURA_ENABLE_VST3=ON` with `AURA_VST3_SDK_DIR` |
| CLAP instantiation | planned (M7) |
| Scanning in a separate process | designed, not built (M6/M7) |
| Out-of-process audio sandbox | designed, not built (M7) — the current host is in-process and the docs say so |
| Editor windows | not built (needs the shell, M9/M10); a generic parameter list is the fallback |

## What a project stores

Each chain instance serialises: the plug-in's unique id, vendor, version, its
bypass state and an opaque state blob. Opening a project whose plug-in is missing
produces a **placeholder** entry that keeps the chain position and the state blob,
so reinstalling the plug-in restores the sound rather than losing the settings.

## Scanning design (M6)

1. Enumerate the standard vendor directories plus user-specified paths.
2. For each binary: read metadata *without executing plug-in code* where the format
   allows it (CLAP can; VST3 needs a load).
3. Loads happen in a **separate process** with a hard timeout (5 s per binary) and a
   result file; a hang or crash is recorded as a blacklist entry *with a reason*
   instead of taking the host down.
4. Results merge into the database; user data (favourites, names, tags) is never
   touched by a rescan.
5. Progress is reported and cancellable; the whole library scan targets < 60 s cold,
   < 5 s warm.

## Sandbox design (M7) — and the honest limitation

```
control thread            audio thread                 sandbox process
  gateway      ──lock-free command queue──▶  proxy node ──▶ plug-in instance
  state I/O    ◀──lock-free audio ring────   proxy node ◀── plug-in DSP
```

* The proxy appears to the graph as an ordinary `Processor` whose latency is the
  round-trip plus the plug-in's own reported latency.
* Until that exists, **a crashing plug-in crashes AURA**. The mitigation is
  autosave + crash recovery, and this paragraph. A DAW that claims sandboxing it
  does not have is worse than one that admits the gap.

## Automation and parameters

Plug-in parameters are exposed as AURA automation targets (id + index), so a lane
can drive a plug-in parameter exactly like a fader. VST3 gives stable parameter ids;
CLAP gives id + module/port/index. Where a plug-in reports latency *after* its first
process call, the chain re-reports and the graph republishes.

## Built-in instruments

AURA ships `aura.basic` (`midi::Instrument`) — an original polyphonic subtractive
synth: oscillator, filter, ADSR, voice stealing, capacity-bounded sustain stack.
**No third-party instrument is bundled**: it would be a licensing problem and it
would make AURA's default sound someone else's.

## Testing

`tests/plugin/PluginTests.cpp` covers the database, chain behaviour, state blobs and
the "no SDK → `NotImplemented`" contract with a fake plug-in implementing the host
interface. A real VST3 is not part of the test suite: no SDK or binaries are vendored
or downloaded by the build.
