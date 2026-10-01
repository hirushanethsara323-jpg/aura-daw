# CRASH_RECOVERY_RESEARCH.md

**Status:** Accepted (autosave shipped; dump capture lands with the app) · **Code:** `aura::project::Autosave`, `aura::project::recoveryAvailable`, `crash.flag`

## The three separate problems

Crash safety is often described as one thing. It is three, with different
solutions:

1. **Don't lose work** — autosave, atomic writes, backup rotation.
2. **Don't lose a recording** — the take is a file on disk that must stay valid
   even if the process dies mid-take.
3. **Find out what happened** — a crash dump and a readable report that says
   which module and which thread.

## 1. Don't lose work

| Mechanism | Detail |
|-----------|--------|
| Atomic manifest write | write `.tmp` → flush → replace (`MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH`); a power cut cannot leave a half-written manifest |
| Backup rotation | previous manifest → `backups/project.json.bak`, oldest generations dropped |
| Autosave | independent background timer writing to `cache/`, never over the user's file; `Autosave::setIntervalSeconds()` is configurable and `saveNow()` is callable |
| Crash flag | `crash.flag` in the project directory while a session is open; a clean exit removes it. On open, a lingering flag + a newer autosave is what makes `recoveryAvailable()` true |
| Recovery presentation | the user is *offered* the recovery copy with both timestamps ("autosave 3 min newer than project.json"); AURA never silently replaces their file |

Rationale for the flag rather than a timestamp heuristic alone: the file system can
lie about mtimes (network shares, clock changes, restores). A flag that only a
clean shutdown clears is unambiguous.

## 2. Don't lose a recording

Design rule: **a take is written as a real WAV from the first second**, header
sizes patched on close. Consequences:

* The file is playable by any tool even after a crash and a forced reboot — the
  only loss is the header length fields.
* `RecordingEngine::droppedFrames()` counts what the writer could not keep up with
  (a disk stall must be *reported*, not hidden), and `recoverIncompleteTakes()`
  scans the recording directory on next launch, offers the takes, and repairs the
  headers it can. This is implemented and tested.
* Recording never blocks the audio thread: the writer's buffers are preallocated,
  and a full buffer increments the dropped-frame counter rather than waiting.

## 3. Find out what happened

| Option | Licence | What it gives | Verdict |
|--------|---------|---------------|---------|
| **`SetUnhandledExceptionFilter` + `MiniDumpWriteDump`** (DbgHelp) | Windows SDK | a minidump plus our own context file; no dependency, ~40 lines | **Accepted for the app** |
| Crashpad | Apache-2.0 | out-of-process handler, upload, symbol management | Rejected for v1 — a meaningful dependency and an upload endpoint we do not have; revisit if telemetry is ever wanted |
| Windows Error Reporting / LocalDumps registry | OS | dumps with no app code, but requires registry setup | Kept as a documented fallback for support ("enable LocalDumps and send us the file") |
| Breakpad | BSD-3 | cross-platform ancestor of Crashpad | Rejected — same dependency argument |
| CrashRpt | BSD-ish | full report UI + SMTP/HTTP upload | Rejected — old, upload-centric |

Dump policy we can defend: **write a minidump (normal + thread info), plus a plain
text context file**, into `%LOCALAPPDATA%\AURA\crashes\`, keep the last N (default
5), never upload anything automatically, and tell the user exactly what the file
contains and where it is. A DAW may hold unreleased music; silent telemetry is not
acceptable.

The structured log (`core::Log`, TRACE→FATAL) keeps an in-memory ring (for the
diagnostics panel) and a rotating file. The crash context file records: app
version, build config, enabled backends (ASIO/VST3/JUCE), device name + granted
sample rate/buffer size, project path, engine state (block count, xruns), and the
last 200 log lines.

## What we explicitly do not promise

* **No plug-in sandbox yet** (M7). A plug-in that segfaults takes the process with
  it; recovery then means "reopen and you get the autosave", which is honest but
  not the same as isolation. `PLUGIN_FORMAT_RESEARCH.md` states this plainly.
* **No cloud backup, no automatic upload, no "anonymous usage statistics".**
* **No promise that a repaired take is sample-perfect** — a take whose header was
  truncated has a length we infer from the file size; the engine says so in the
  recovery dialog rather than pretending.

## Test coverage today

* Interrupted take → recovered and readable (`tests/audio/AudioTests.cpp`).
* `.bak` recovery when `project.json` is corrupt (`tests/project/ProjectTests.cpp`).
* Autosave writes and reports availability; recovery is not offered after a clean
  save.
* Newer project format refused rather than half-loaded.

## Sources

* Microsoft, *MiniDumpWriteDump*, *SetUnhandledExceptionFilter*, *Collecting user-mode
  dumps* (WER `LocalDumps` registry configuration).
* jake-shadle.github.io, *Crash reporting* — Breakpad/Crashpad/minidumper design
  notes: why minidumps, why the handler must do as little as possible in the
  crashing process, and Windows-specific details (`_set_invalid_parameter_handler`,
  `_purecall`, `__fastfail`).
* CrashCatch project documentation — the header-only Windows approach
  (`SetUnhandledExceptionFilter` + `MiniDumpWriteDump` + `StackWalk64`) that matches
  what we plan to implement first-party.
* CrashRpt project documentation — the upload-oriented model we deliberately reject.
