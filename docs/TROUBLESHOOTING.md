# TROUBLESHOOTING.md

Symptoms, causes, and what to actually do. Each entry is written from a failure the
project hit, not from imagination.

## Building

**`CMake Generate step failed. Build files cannot be regenerated correctly.`**
The tree changed shape while a configure was failing — usually a file listed in a
`CMakeLists.txt` that does not exist (the error above it names it), or a preset
directory holding a half-written cache.
→ Read the first `CMake Error` (not the last), fix the cause, then delete the binary
directory (`rm -rf build` / `Remove-Item -Recurse build`) and reconfigure. Presets
use their own binary directories so this stays contained.

**`Cannot find source file: …` / `No SOURCES given to target: …`**
A source list references a file that is not on disk. Create the file or remove the
entry — do *not* weaken the list to make the error go away; the list is the build's
contract.

**Catch2 fetch fails (no network)**
```bash
git clone --branch v3.7.1 --depth 1 https://github.com/catchorg/Catch2.git /opt/catch2
cmake -S . -B build -DAURA_BUILD_TESTS=ON -DAURA_FETCH_DEPS=OFF -DAURA_CATCH2_DIR=/opt/catch2
```

**`aura.static.realtime_audit` says `BAD_COMMAND`**
Python 3 was not found at configure time. Install Python 3 or configure with
`-DPython3_EXECUTABLE=/path/to/python3`; the audit is a ctest entry
(`python3 tools/rt_audit.py .`) and can also be run by hand.

**Link errors mentioning `aura::dsp`/`aura::engine` symbols**
A new `.cpp` was not added to the corresponding explicit source list in
`CMakeLists.txt` (see `DEVELOPMENT.md`).

## Tests

**A test fails only on the second run** — a test wrote state outside its own
temporary directory, or two tests share a `/tmp` path. Give each test a unique file
name; the suite is written to be order-independent.

**"Disk streams read a real file from a worker thread" fails** — a *real* regression
signal: the reader thread repositions only on a published request, and a bad change
there makes sequential playback rewind. Check `DiskStream::readerLoop` before
touching the test.

**Meter or loudness numbers look wrong after a change to block handling** — peak and
RMS are instantaneous/decaying values; measuring them after one very long block
reports a decayed peak. Feed blocks (100 ms is a good size) and read the snapshot
right after the block you care about.

**A DSP assertion is off by a few dB** — check the *measurement* first: an amplitude
of 0.5 in a magnitude measurement reads as −6.02 dB against a unity-gain analytic
response. The EQ test uses unit amplitude for exactly this reason.

## Running

**No sound** *(when the shell lands)* — check, in order: device selected and open;
the track is not muted/soloed-out; the master limiter is not bypassed while the
signal is above the ceiling; the transport is actually rolling (`isPlaying`); the
graph has a plan (`rebuildGraph()` after structural changes).

**Crackles/pops** — buffer size too small for the plug-in load (raise it), CPU load
near 100 % (`peakCpuLoad`), or a disk stream underrunning (`underrunCount()`), or
another application holding the device in exclusive mode.

**A stopped transport still makes noise** — it should be silence after the master
limiter's look-ahead drains (≈1.5 ms). If it isn't, the source-node clearing or the
`hasTail()` logic is wrong; there is a named regression test for this.

**ASIO device missing** — the backend is compiled out by default:
`-DAURA_ENABLE_ASIO=ON -DAURA_ASIO_SDK_DIR=<path>`. Without it, every ASIO entry
point returns `NotImplemented` with that exact instruction (by design).

**A plug-in does not appear** — the VST3 adapter is not compiled in, the plug-in is
blacklisted (check the reason recorded when it was scanned), or it lives in a
directory that is not being scanned.

**Project will not open: "written by a newer version of AURA"** — working as
intended: a newer `formatVersion` is refused rather than half-loaded. Use the newer
build, or open `backups/project.json.bak` with the older one.

**A take disappeared after a crash** — it should not: takes are valid WAV files from
the first second. Look in the recording directory for files whose header was never
patched; `recoverIncompleteTakes()` finds them on next launch, and a repair can also
be done by any WAV editor that rewrites the header. If you are reporting this, keep
the file — the recovery path is designed around it.

## Reporting a problem

Include: AURA version (About box), build configuration, enabled backends, the device
and the sample rate/buffer size the driver **granted**, the project's `formatVersion`,
and the log ring contents (diagnostics panel). For crashes: the minidump path shown
in the dialog. AURA never uploads anything on its own.
