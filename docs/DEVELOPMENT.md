# DEVELOPMENT.md

Conventions and workflows for working on AURA. Contribution process and code of
conduct: [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## The loop

```bash
cmake -S . -B build -DAURA_BUILD_TESTS=ON && cmake --build build -j 2
cd build && ctest --output-on-failure      # must be green before a commit
python3 ../tools/rt_audit.py ..            # must be clean
```

Then, in the repository:

```bash
git switch -c feature/midi-import
git add -A && git commit -m "midi: SMF type-1 reader"
git switch main && git merge --no-ff feature/midi-import   # tests green first
```

House rules from the project brief, kept verbatim because they are easy to forget
in a hurry:

* feature branches; never rewrite published history;
* **no force-push to `main`**, ever;
* never delete someone else's code without understanding what it does first;
* inspect the code before changing architecture;
* tests green before commit, build green before merge.

## Code conventions

* **C++20**, 4-space indent, 100-column soft limit, `#pragma once`.
* Every file starts with the banner + `SPDX-License-Identifier: GPL-3.0-or-later`
  + `Copyright (c) 2026 The AURA DAW Project`.
* Naming: `PascalCase` types, `camelCase` methods, `snake_case_` members, `kName`
  constants, `namespace aura::module`.
* Headers document the thread contract on the class (`Control thread only.`,
  `RT-safe.`, `Called by the audio thread.`). A wrong comment here is a bug.
* Comments explain **why**, not what. If a line needed a comment to be understood,
  the comment says what would go wrong without it — including the bug that caused
  it (`// This was a bug: …`).
* Errors are values: return `AuraResult<T>`/`Status`, never throw across a module
  boundary, never fail silently.
* No `using namespace` in headers; no C-style casts; no `malloc`/`free`;
  `std::string_view` for read-only parameters.

## Real-time rules (non-negotiable on the audio path)

No allocation, no locks, no file or console I/O, no exceptions, no unbounded loops,
no logging. Everything needed at run time is preallocated in `prepare()`.
`tools/rt_audit.py` enforces this statically; if you believe a finding is safe,
annotate the statement with a reason:

```cpp
sustainedNotes_.push_back(note);  // rt-audit: allow - reserved in prepare(), capacity-guarded
```

The audit prints every escape hatch, so an annotation is a visible decision.

## Adding a DSP processor

1. Add `include/aura/dsp/Thing.hpp` (declare `prepare`, `reset`, `process`,
   `typeName`, and either `hasTail()` if it rings or `isActive()` if it can be
   bypassed).
2. Add `src/dsp/Thing.cpp` and **add it to the `aura_dsp` source list** in
   `CMakeLists.txt`.
3. Write the test first in `tests/dsp/DspTests.cpp`: an analytic assertion if the
   processor has a closed form, otherwise the property that matters (bounded,
   monotone, below a threshold) — with a comment explaining why.
4. Run the RT audit. A new processor on the audio path must not allocate; buffers
   belong in `prepare()`.
5. Document it: a row in `DSP.md` and, if it introduces an algorithm or a trade-off,
   a paragraph in `research/DSP_ALGORITHM_RESEARCH.md`.
6. Edit anything?

## Adding a command

1. Add an id in `aura::commands::ids` and a `CommandInfo` (name, category,
   description, default shortcut).
2. Implement the action (an `UndoEdit` if it changes the project).
3. Add a test in `tests/commands/CommandsTests.cpp` — including the undo path.
4. If it needs to reach the audio thread, it goes through `EngineCommandQueue`
   (POD payload only) — never by touching the graph directly.

## Debugging tools in the tree

| Tool | Use |
|------|-----|
| `tools/rt_audit.py` | RT-safety findings with file:line and why |
| `tests/aura_tests "case" -s` | full expression output for a failing assertion |
| `ctest --output-on-failure` | the same, for everything |
| `AURA_ENABLE_ALLOC_TRACKING` | counts allocations; armed inside engine tests |
| `AURA_LOG_*` + the log ring | structured logs, readable from the diagnostics panel |
| `/tmp/model.cpp`, `/tmp/gain_probe.cpp` | historical reference harnesses (37 checks; gain ramp verification) |

## Documentation rule

Any change to behaviour, format, options, dependencies or performance targets
updates the matching document **in the same commit** as the code. A document that
describes a previous version is worse than no document: it makes a wrong assumption
look verified.

## Commit messages

`area: imperative summary` — e.g. `engine: keep a stopped transport silent`,
`docs: research record for the UI framework`, `tests: cover chunked file reads`.
The body explains what was wrong, what changed and what proves it (test names).
