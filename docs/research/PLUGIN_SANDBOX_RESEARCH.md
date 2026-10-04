# PLUGIN_SANDBOX_RESEARCH.md

M7's question: **how do we run a plug-in so that its crash is survivable, without
making the mixer block, allocate or copy in a way a DAW cannot afford?** This is the
research that shapes `src/plugin/sandbox/`; the code decisions that follow from it are
summarised in [`../PLUGIN_HOST.md`](../PLUGIN_HOST.md).

A plugin *scanner* already runs out of process (M6). A sandbox is the harder half: the
plug-in has to process the session's audio, every block, with the transport running.

## What the alternatives actually cost

| Approach | Isolation | Cost | Verdict |
|----------|-----------|------|---------|
| Same thread, same process (today) | none — a crash is AURA's crash | cheapest | keep as the *default for trusted plug-ins* only; it is what shipping DAWs still do |
| Same process, different thread | none: threads share an address space, so a crash or heap corruption still takes the host down (KVR's sandboxing thread makes this point plainly) | none | rejected — it buys scheduling, not safety |
| **Separate process, shared-memory audio** | a crash costs that process; the session survives | one buffer of added latency + IPC per block; measured overhead is "3–5 % at 128 frames, negligible at 256+" in a real-world Bitwig test | **chosen** |
| Separate machine (ReaRoute-style) | total | network latency, unusable for monitoring | out of scope |

Prior art for the *policy* rather than the mechanism is Bitwig's, and it is worth
copying because it is a UX answer as much as a technical one: run plug-ins in the
host process, one shared sandbox process for all plug-ins, one process per plug-in
vendor, one per plug-in, or one per instance. Each step down the list buys isolation
and pays for it in processes, memory (each copy of a plug-in has its own sample
libraries loaded) and IPC.

## Decision 1 — policy is per plug-in, and the default is *one shared sandbox*

```
enum class SandboxPolicy { InProcess, Shared, PerInstance };
```

* **`Shared`** (default for third-party plug-ins): one helper process hosts every
  sandboxed plug-in. This is the mode that protects the session from the common case
  (one buggy plug-in) at the lowest cost, and it keeps "modest PCs" in the picture:
  one extra process, not a dozen.
* **`PerInstance`**: for the plug-in that keeps misbehaving. Stored per plug-in id in
  `PluginUserData` (it is user data, exactly like a favourite), so it survives a
  rescan and a project reload.
* **`InProcess`**: the escape hatch, and the default for *AURA's own* processors
  (which are our code and cannot be sandboxed against themselves usefully).

The plugin *database* gains a sandbox override column; the UI exposes it as a
per-plug-in choice with the cost in words ("uses another process; adds one buffer of
latency"), not as an unexplained toggle.

## Decision 2 — the transport is shared memory, and the audio thread never blocks

The audio thread must not wait for another process. Any design where the mixer writes
a block and *waits* for the result has a deadline it can miss — and when it misses,
the user hears it.

So the sandbox is a **one-block-deferred pipeline**:

```
audio thread (host)                       helper process
  n:    write block into shared slot A ──▶ (publish) ──▶ read A, process, write D
  n+1:  read the block the helper finished (C) ◀── publish ◀── read B, process, write C
```

* Two shared audio slots in flight; the host writes the *current* block and reads the
  *previously processed* one. The host thread does a bounded amount of work (a copy in,
  a copy out) and never waits on the helper.
* The price is **exactly one block of latency**, which is *measurable* and therefore
  *compensable*: the proxy reports it through `latencySamples()` and the graph's
  existing delay compensation aligns it with the rest of the session (M5's
  `DelayCompensationTests.cpp` is the proof that this machinery works). A sandboxed
  plug-in costs one buffer of PDC, not a mystery offset.
* Honest consequence: the plug-in's own latency reporting is no longer lined up
  sample-exactly with the host's block clock in a live monitoring path. The design
  records it; the `InProcess` mode remains available for a plug-in that must be
  monitored at the lowest possible latency.

The shared region holds one *arena* per sandbox:

| Region | Contents | Direction |
|--------|----------|-----------|
| Audio slots | `maxBlockSize × maxChannels` floats ×2 (double-buffered) | host → helper → host |
| Note queue | fixed-size records, SPSC ring | both |
| Parameter queue | `paramId + normalised value + sampleOffset` recs, SPSC ring | host → helper |
| Latency/telemetry mailbox | atomics: reported latency, tail, CPU estimate, CPU spikes, xrun count | helper → host |
| Flags | transport snapshot (playing, tempo, position, sample rate, block size), "please reset", "please save state" | both, as versioned atomics |

Everything is POD and fixed-size, sized at `prepare()` from the sample rate, block
size and channel count. Publishing uses release/acquire on a per-slot sequence
counter — the same pattern as AURA's existing lock-free queues
(`research/THREADING_RESEARCH.md`), just in shared memory instead of process memory.

Non-real-time traffic (load, unload, state blobs, parameter lists, GUI) goes over a
control channel (pipe on Windows, socketpair/pipe on POSIX) where blocking is
allowed, because none of it happens on the audio thread.

## Decision 3 — crash protocol: the session keeps playing

1. The supervisor holds the helper's process handle and a wait primitive.
2. Helper dies → the supervisor notices independently of the audio thread, and marks
   the slot failed. The proxy returns the input block unchanged (dry) for that block
   and every block after it, so the graph keeps running and the user hears the
   plug-in disappear rather than the session freeze.
3. The **last state blob is the recovery point**: the host keeps the most recent
   `saveState()` bytes (it must anyway, for the project file), so a restarted helper
   can be restored to where the plug-in was. Autosave is not the recovery mechanism
   here — the running session is.
4. The user is told once, with the plug-in's name and the exit code, and offered
   *reload* / *keep bypassed* / *disable this plug-in*. A plug-in that crashes twice
   in a session is not restarted automatically a third time.
5. A helper that **hangs** (no progress for N blocks) is killed at a deadline —
   the same discipline the scanner already uses.

## Decision 4 — Windows gets a job object, and that is a *stability* boundary

`CreateJobObject` + `AssignProcessToJobObject` with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` gives what the scanner's POSIX process group
already gives: when the helper is killed, everything it spawned dies with it. This is
the M6 carry-over, and it is cheap.

What this is **not**: a security sandbox. A plug-in still runs with the user's
privileges and can read and write their files. Saying otherwise would be a lie that
costs someone their data. The boundary AURA buys here is *stability* (a crash, a hang
and a memory leak are contained), and the documentation says exactly that. A
restricted-token / AppContainer design is recorded as future work with its costs
(plug-ins that write their own settings files or talk to licence servers break, which
is a support burden and a compatibility wall).

## Decision 5 — the editor window stays in the helper

A plug-in's GUI belongs to the process that owns the plug-in. Two ways to show it:

* **Reparent the helper's window into the host's window tree** (Windows `SetParent` on
  an `HWND` created by the helper). Cross-process window parenting is supported and
  is what several hosts do; input and painting cross a process boundary, so keyboard
  focus and modal dialogs need explicit handling.
* **Render offscreen in the helper and blit** into the host's window. Fully
  controlled, but a plug-in that renders at 60 fps over IPC costs bandwidth and
  latency, and some plug-ins' painting is not offscreen-friendly.

Decision: **reparenting** on Windows, with the offscreen path as the fallback if a
plug-in misbehaves. Inside the helper, the editor runs on the helper's UI thread;
AURA's shell only supplies a parent window handle. That also means the *shell* does
not have to know whether a plug-in is sandboxed. The M9/M10 shell is what makes this
concrete; until then the generic parameter list is the UI, and the proxy already
exposes parameters and state over the control channel.

## Decision 6 — CLAP arrives through the same door

`AURA_ENABLE_CLAP=ON` fetches the CLAP headers (MIT, headers-only, pinned) and adds
an adapter with the same shape as the VST 3 one: entry point → factory → plugin
instance → `params`, `state`, `latency`, `audio-ports`, `note-ports`, `render`,
`tail`. CLAP is a C ABI with a single exported symbol (`clap_entry`), a documented
thread model per function, and extensions discovered by id — which makes the adapter
smaller than the VST 3 one and, importantly, **sandbox-friendly by construction**: C
structs with function pointers are what an out-of-process host wants, because there is
no C++ ABI to bridge.

The host-side extensions AURA implements first: `log`, `thread-check`, `params`,
`state`, `latency`, `audio-ports`, `note-ports`, `gui` (stub until the shell),
`timer-support` (no), `thread-pool` (no, until it is measured as needed).

## Phases (each one lands with tests or it does not land)

1. **Transport first, in one process** — `SharedAudioRing` + tests: publish/consume
   ordering, overrun/underrun behaviour, sequence-counter correctness, no allocation
   after `prepare()`.
   — **Landed.** `include/aura/plugin/sandbox/{SandboxArena,SharedAudioRing}.hpp` and
   `src/plugin/sandbox/*.cpp`; 15 tests tagged `[sandbox]` in
   `tests/plugin/sandbox/SandboxTransportTests.cpp`, including a two-thread run of
   60 000 blocks that asserts every published block came back processed exactly once,
   and a zero-allocation claim measured against `AURA_ENABLE_ALLOC_TRACKING` rather
   than asserted from reading the code. Three corrections to this design were needed
   while writing it (ADR-0017): the seqlock version word is `memory_order_seq_cst`,
   because a release/acquire seqlock is reorderable and can hand a reader torn data
   under a plausible version; a slot's block number travels in a producer-declared
   `origin` field, because after any dropped block the ring index no longer identifies
   it; and a consumer must finish verifying a slot before it releases it, because
   releasing first lets the producer legitimately restamp the slot and fabricates a
   torn read. The same work found two bugs outside the sandbox — a per-block
   `std::vector` in `Clip::render()` on the audio thread, and a function extractor in
   `tools/rt_audit.py` that silently skipped every function after the first one-line
   definition in a file.
2. **Helper process** — `aura_plugin_host` loads one bundle, runs the RT loop against
   the arena, and reports state over the control channel; started by the supervisor.
   — **Landed, with one deliberate deviation.** `tools/plugin_host/PluginHostMain.cpp`,
   `plugin/sandbox/{ProcessSupervisor,SandboxProcessor,SharedMemory,HelperProtocol}`;
   12 tests in `tests/plugin/sandbox/SandboxHelperTests.cpp` (`[shm]`, `[helper]`), the
   end-to-end one asserting 200 blocks cross a real process boundary and come back
   bit-exact, in order, once each.
   *It does not load a bundle yet*: the format adapters are phases 6-7, so the helper
   drives `SandboxProcessor` and ships a deterministic reference processor (gain on
   parameter 0, notes echoed). That substitution is what makes this phase testable at
   all — a cross-process audio path that can only be exercised with a real third-party
   bundle is a path no CI runner ever runs.
   Three things this phase settled that the design had left open:
   - **The audio path runs under back-pressure.** The helper holds one processed block
     until the host has room, instead of dropping it when the output ring is full. The
     dropping version lost 2-5 blocks in 200 depending on scheduling; a bounded retry
     was tried and was worse, because any budget short of "wait for the host" still
     drops under load. Holding costs nothing: the rings are two deep and the host pops
     before it pushes, which is why `exchange()` is ordered that way. After the change
     the same test delivers 200 of 200 with zero lost and zero torn.
   - **`rt_audit.py` scans the helper's loop** (`tools/plugin_host/*.cpp`,
     `^runHelperLoop$`). The helper's thread is real-time in every sense the engine's
     callback is, with two deliberate differences: it may *sleep* when idle, because
     nothing waits on it and spinning would steal the core the host's callback needs;
     and it may *exit* when the host goes quiet, because a dead host cannot ask it to.
     The new scope was verified by injecting a `std::vector` into the loop and
     confirming the audit fails.
   - **`kSaveState` is acknowledged and refused.** The arena has nowhere to put a blob
     and this thread must not touch the disk, so wiring state save is a `kLayoutVersion`
     bump that belongs with phase 4, where the blob is the recovery point and therefore
     has to be right. The helper clears the request bit without setting `kStateReady`,
     which is the honest answer.
   - **A session's shared-memory name must be unique to that session.** POSIX has
     `shm_unlink`, so a creator's release frees the name at once even while a helper
     still has the object mapped; Windows has no unlink, so a named section lives
     until its last handle closes and `create()` on a name a live peer holds reports
     `AlreadyExists` — correctly, since the alternative is handing back that peer's
     section. A fixed name would therefore collide on Windows exactly when a previous
     helper is still alive, which is when a new session is most likely to start. This
     was not found by reading the platform documentation: a test asserted the POSIX
     rule unconditionally, passed on Linux for a whole increment, and CI's Windows
     jobs disagreed with it. The rule is now written down for both platforms in
     `SharedMemory.hpp` and tested on both. Phase 3's session owns name generation.
   Also added: `ArenaView::attach(bytes, size)`, which reads the spec from the peer's
   own header — a helper has no independent copy to compare against, and putting the
   spec on the command line would create a second source of truth that a stale launch
   could disagree with the mapping. It is safe because the header is then proved
   against the bytes (magic, version, a spec that normalises, offsets that match what
   the spec implies, a mapping big enough), and four tests lie about each of those.
3. **Proxy** — `SandboxedPluginInstance` implements `PluginInstance`; the chain and
   therefore the whole engine see no difference. Tests: audio equals the in-process
   result within the compensated block, parameters and notes cross, state round-trips.
   — **Landed.** `plugin/sandbox/{SandboxSession,SandboxedPluginInstance}`; 8 tests in
   `tests/plugin/sandbox/SandboxProxyTests.cpp` (`[proxy]`), each spawning a real
   helper, including one that puts the proxy in a real `PluginChain` and checks latency
   summing, state round trip and bypass through the chain's own API.
   Three things this phase settled:
   - **An in-place caller reverses the exchange order.** `PluginInstance::process()`
     hands over one buffer that is both input and output, so the proxy pushes and then
     pops: the push copies the input into the slot, after which the buffer may receive
     what comes back. `SharedAudioRing::exchange()` pops first, deliberately, to give
     the helper the longest run at the previous block - and with an in-place caller
     that order would overwrite the input before publishing it. Both orders are
     correct; they answer to different callers.
   - **A refused push must drain, or the pipeline deadlocks.** The helper runs under
     back-pressure (phase 2), so it holds a processed block until the output ring has
     room and takes no new input while it does. A host that returns early on a refused
     push - leaving the audio dry, which looks like the safe answer - waits for the
     helper to consume, and the helper waits for the host to pop: a deadlock that
     presents exactly as a hung plug-in, found by a cross-process test and not by
     reading either side. The proxy now drains one output into scratch (allocated in
     `prepare()`, never in `process()`) and retries the push once.
   - **Tests have to pace themselves.** A real host calls `process()` once per device
     callback. Calling it back to back is not a harsher test but a different one: the
     host outruns a peer in another process that has not been scheduled yet, and what
     gets measured is the test runner's loop. One block per millisecond is inside every
     block period AURA supports and outside the helper's 200 µs idle reaction, which
     came down from 1 ms for that reason - the sleep is the helper's worst-case
     reaction time, and 1 ms against a 2.7 ms block is a third of the budget. Pacing
     alone was not enough on the Windows runners, so the cases also retry a block until
     the proxy has published it: a dropped input shifts the deferral by one for the
     rest of the run, and every later comparison is then against the wrong index. Each
     returned block is *classified* rather than assumed - dry, delay n, silent from a
     discarded torn read, or corrupt - so a scheduling fact is never reported as a
     transport bug and a transport bug is never excused as scheduling. Two of the four
     Windows failures this caused were the test's own off-by-one: the priming loop
     breaks on the block it just fed, so measuring from that index feeds it twice and
     the second call returns its own output, which reads as a delay of 0 where 1 was
     expected. It was found by printing the samples, not by reading the loop.
   Still deferred, deliberately: the proxy reports no editor (it stays in the helper
   and needs the shell, M9/M10), `isActive()` is always false (whether a plug-in is
   still producing a tail is something only the plug-in knows, and `Telemetry` has a
   `tailSamples` field nothing publishes yet), and `saveState()` saves the parameters
   the proxy last sent rather than the plug-in's own blob - which crosses with the
   crash protocol in phase 4, where it is the recovery point and has to be right.
4. **Crash and hang** — the fixture grows a deliberate crash/hang hook (launched from
   a *child* process, unlike M6's scanner hooks, because it runs the RT loop);
   tests assert the session survives, the slot goes dry, the watchdog kills a hung
   helper at the deadline, and the restart restores state.
5. **Policy plumbing** — `PluginUserData::sandbox`, the UI-facing API, and the
   database round trip.
6. **Windows job object** + the platform-specific process plumbing, cross-checked by
   `tools/win_crosscheck.sh` and the Windows CI rows.
7. **CLAP adapter** (its own increment, after the VST 3 host has a sandbox).

## What could make this the wrong design

* **Latency-sensitive monitoring.** One block of PDC is fine for mixing; it is not
  fine for a guitarist tracking through a plug-in amp sim at 64 frames. `InProcess`
  stays, and the UI has to say "sandboxed: +1 buffer" in a way the user can act on.
* **Plug-ins that share state between instances** (one vendor's ecosystem talking to
  itself). `Shared` keeps that working; `PerInstance` breaks it, which is why it is
  opt-in.
* **CPU cost on a 2-core machine.** The measured overhead is small at 256 frames, but
  it is not zero, and a sandboxed session has one more process to schedule. The
  benchmark suite grows a sandboxed case so this is measured rather than argued.

## Sources

* KVR Audio, *What is the most CPU efficient plugin sandboxing setting?* — Bitwig's
  policies (in-process / shared / by manufacturer / by plug-in / per instance) and
  why threads are not isolation: <https://www.kvraudio.com/forum/viewtopic.php?t=591669>
* *How I solved the Cubase plugin crash problem…* (measured sandbox overhead: ~3–5 %
  at 128 frames, negligible at 256+, and the observation that REAPER and Tracktion
  Waveform sandbox too): <https://dredyson.com/how-i-solved-the-cubase-plugin-crash-problem-using-sandboxing-concepts-a-complete-step-by-step-guide-to-understanding-crash-protection-plugin-isolation-and-workarounds-that-actually-work-tested-with-bitwig-and-reaper-comparisons-w/>
* Bitwig Studio feature documentation and user reports on sandbox modes (per-vendor
  as the practical default): <https://www.audeobox.com/learn/bitwig/>
* Microsoft Learn, *Job Objects* — `CreateJobObject`,
  `AssignProcessToJobObject`, `SetInformationJobObject` limits (including
  kill-on-job-close) and nested jobs: <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>
* free-audio/clap — the format's own documentation: entry point, factory,
  `clap_host`/`clap_plugin`, extensions (`log`, `thread-check`, `audio-ports`,
  `note-ports`, `params`, `latency`, `render`, `tail`, `state`, `gui`), and the
  statement that every method has a documented thread specification:
  <https://github.com/free-audio/clap>
* CLAP tutorial, *part 1* (the entry/factory shapes a host must call):
  <https://nakst.gitlab.io/tutorial/clap-part-1.html>
* AURA-internal: [`LATENCY_COMPENSATION_RESEARCH.md`](LATENCY_COMPENSATION_RESEARCH.md)
  (delay compensation), [`THREADING_RESEARCH.md`](THREADING_RESEARCH.md) (lock-free
  ordering, publish/acquire discipline).
