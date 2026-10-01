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

## The warning contract

Two compilers, one set of rules. A warning that Linux forgives but MSVC reports is
a red CI job, so the flag sets are kept deliberately equal (ADR-0013):

| what | MSVC | GCC / Clang |
|------|------|-------------|
| base level | `/W4` | `-Wall -Wextra -Wpedantic` |
| narrowing | C4244 / C4245 / C4267 | `-Wconversion -Wsign-conversion` |
| shadowing | C4456–C4459 | `-Wshadow` |
| unused parameters | C4100 | `-Wunused-parameter` |
| as errors | `/WX` | `-Werror` (both only with `AURA_WARNINGS_AS_ERRORS=ON`) |

* Check your work the way CI does:
  `cmake -S . -B build-werror -DAURA_WARNINGS_AS_ERRORS=ON && cmake --build build-werror -j 2`.
* **`-Wshadow` is why aliases may not repeat an enumerator name.** GCC 13 (CI)
  reports an enumerator that shares a name with a namespace-scope declaration;
  `json::Type::Array` next to `using Array = …` was exactly that. GCC 14 (many dev
  machines) accepts it — do not trust a green local build for that class of change.
* **Unused parameters stay an error**, because MSVC reports C4100 at `/W4` and an
  interface-imposed parameter cannot be dropped from the signature. Leave the
  parameter unnamed in the *definition* (`int /*numChannels*/`) and say why in a
  comment; `[[maybe_unused]]` is not needed.
* **Ask for what you use.** Never rely on a transitive include: libstdc++ pulls
  `std::mutex`, `std::thread` and `std::chrono` in through unrelated headers, MSVC
  does not. `python3 tools/include_audit.py .` (ctest `aura.static.include_audit`)
  fails the build if a file uses a standard type whose header is not in its include
  closure. In a header, include `<cstdint>`, `<vector>`, … directly.
* **Suppressions are rare, scoped and explained.** The tree's only one is
  `#pragma warning(disable : 4324)` around `SpscQueue` and `AudioRingBuffer`, where
  padding *is* the feature (one cache line per index). Wrap it in
  `pragma warning(push/pop)`, never add a global `/wd` or `-Wno-` flag, and record
  the reason in ADR-0013.
* `sizeof`-based `if` in a template is C4127 on MSVC — write `if constexpr`.

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

## Repository administration

```bash
GITHUB_TOKEN=<token> python3 scripts/github_setup.py owner/repo --issues --dry-run
GITHUB_TOKEN=<token> python3 scripts/github_setup.py owner/repo --issues
```

Creates/updates the label set, the M0–M12 milestones (mirroring `ROADMAP.md`) and the
starter issues; idempotent, so re-running after a roadmap change applies only the
difference. The token comes from the environment and is never written to a file. For a
long-lived setup prefer a **fine-grained** token limited to this repository with
`Issues: write` and `Metadata: read` over a broad classic token, and revoke it when the
setup work is done.

## Debugging tools in the tree

| Tool | Use |
|------|-----|
| `tools/rt_audit.py` | RT-safety findings with file:line and why |
| `tools/include_audit.py` | files that use a standard type without its header (MSVC will reject those) |
| `tests/aura_tests "case" -s` | full expression output for a failing assertion |
| `ctest --output-on-failure` | the same, for everything |
| `AURA_ENABLE_ALLOC_TRACKING` | counts allocations; armed inside engine tests |
| `AURA_LOG_*` + the log ring | structured logs, readable from the diagnostics panel |
| `-DAURA_ENABLE_ALLOC_TRACKING=ON` | builds in the counter used by the dynamic RT test |

## Documentation rule

Any change to behaviour, format, options, dependencies or performance targets
updates the matching document **in the same commit** as the code. A document that
describes a previous version is worse than no document: it makes a wrong assumption
look verified.

## Commit messages

`area: imperative summary` — e.g. `engine: keep a stopped transport silent`,
`docs: research record for the UI framework`, `tests: cover chunked file reads`.
The body explains what was wrong, what changed and what proves it (test names).
