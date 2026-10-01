# SECURITY.md

## Reporting a vulnerability

Open a **private** security advisory on the project's GitHub repository
(`Security` → `Report a vulnerability`). Please do not open a public issue for
anything that could be exploited before a fix ships.

Include: affected version (`AURA_VERSION_STRING`), build configuration, steps to
reproduce, and — for anything touching a project file — a minimal `.aura` sample or
the offending `project.json`.

**Response expectations:** acknowledgement within 7 days; a fix or a mitigation plan
within 30 days for issues that can lose data or execute code. AURA is a
volunteer-maintained project; these are goals, stated so they can be held to.

## Supported versions

| Version | Supported |
|---------|-----------|
| latest release | yes |
| `main` branch | yes (development) |
| older releases | no — fixes land in the next release |

## What a DAW's threat model actually contains

| Surface | Risk | Current position |
|---------|------|------------------|
| **Project files** (`.aura`, JSON, media paths) | a crafted manifest could try path traversal, huge allocations, or reference media outside the project | the loader treats the manifest as *data*: paths are resolved relative to the project directory with traversal rejected, sizes are bounded and validated against the actual file, and a malformed manifest fails with a diagnostic instead of throwing. Fuzzing the parser is on the M5 list |
| **WAV/audio files** | malformed chunks, absurd counts, non-finite samples | the reader validates chunk sizes against the file length, refuses what it cannot describe, and maps NaN/Inf to 0.0 — a corrupt file can never inject NaN into the graph |
| **Plug-ins** | third-party code runs **in-process** today: a malicious or buggy plug-in has the user's privileges | no sandbox yet (M7). This is the largest real risk in the product and it is documented in `PLUGIN_HOST.md` rather than glossed over |
| **Plug-in scanner** | loading an untrusted binary | scanning is designed to run in a *separate process* with a timeout and a result file (M6/M7) |
| **Network** | AURA has no networking code; there is no telemetry, no auto-update, no cloud feature | the only outbound action is an optional manual "check for updates" link in the About box, which opens the browser |
| **Crash dumps** | a minidump can contain project data and memory contents | dumps are written locally, never uploaded, and the UI says where they are and what they contain |
| **Elevation** | the installer may request elevation to install per-machine | the application itself never runs elevated; the portable ZIP needs no installer at all |
| **Dependencies** | supply chain | the production dependency list is the standard library and the Windows SDK; optional SDKs are supplied by the *user*, never downloaded by the build. See `LICENSES.md` |

## Practices

* No secrets, tokens or credentials are committed — including in CI configuration.
  Release signing uses GitHub's own secret store, not files in the repository.
* Tests never download code: the Catch2 fetch is the single network operation, is
  pinned to a tag, and can be disabled entirely (`AURA_FETCH_DEPS=OFF`).
* Builds are reproducible from a tag: toolchain version, options and dependency
  revisions are all recorded in the repository.
* The audio thread never reads a file or parses anything (see `AUDIO_ENGINE.md`), so
  a hostile project file cannot affect real-time behaviour, only the load path.
* Data safety is treated as a security property: atomic writes, backup rotation,
  recovery, and "refuse rather than half-load" are documented in
  `PROJECT_FORMAT.md`.

## Out of scope

* Attacks that require an attacker to already have code execution as the user.
* Physical access, malicious drivers, or a compromised Windows install.
* Plug-in behaviour that is annoying but not exploitable (sound quality, crashes
  without memory corruption — those are bugs, reported as issues).
