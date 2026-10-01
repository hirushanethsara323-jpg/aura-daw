# PACKAGING_RESEARCH.md

**Status:** Provisional (M11) · **Decides:** installer technology, install layout, signing

## Question

How does a Windows user install, update and uninstall AURA without it becoming a
support nightmare — and how much of that can we do without paying for a
certificate?

## Options

| Option | Licence | Format | Per-machine install | Custom UI | Auto-update | Verdict |
|--------|---------|--------|--------------------|-----------|-------------|---------|
| **Inno Setup** | free (own licence, source available) | single `.exe` | yes | yes (Pascal script) | no (or via add-on) | **Accepted** |
| WiX Toolset | MSI, open source | `.msi` | yes | limited | no | Rejected for v1 — MSI's strictness (component GUID rules, ICE validation, upgrade codes) is real work for little user-visible gain; keep as an option if enterprise deployment is requested |
| MSIX | open format, Microsoft tooling | `.msix` | **no — always per-user** | no dialogs during install | yes (store or sideload) | Rejected for v1 — cannot install globally, cannot run elevated, and a DAW installer that cannot place shared resources is a bad fit |
| NSIS | zlib-like | `.exe` | yes | yes (script) | no | Rejected — Inno Setup is easier to write and maintain, and used by VS Code/Delphi-class products |
| Portable ZIP | n/a | `.zip` | n/a | n/a | no | **Kept as a secondary artefact** — some users want no installer at all |

Sources agree on the practical ranking for a small team: Inno Setup is "among the
most capable solutions if the install builder must be free", easier than WiX,
mature, and well documented; the cost is learning Pascal Script for custom
actions. MSIX's per-user/elevation restrictions are the disqualifying detail for a
DAW that may want to register shared plug-in paths.

## Install layout (per-machine, Program Files)

```
C:\Program Files\AURA DAW\
  aura.exe                 the shell
  aura_core.dll / aura_engine.dll / aura_dsp.dll (or one aura_engine.dll)
  licenses\                our GPL text + third-party notices (MIT for VST3 when enabled)
  resources\               theme, icons, English catalogue
  tools\                   crash-report helper, offline docs
%PROGRAMDATA%\AURA\        machine-wide plug-in scan cache (shared by users)
%APPDATA%\AURA\            per-user: preferences, shortcuts, recent projects,
                           plugin database, autosave/recovery, crash logs
%LOCALAPPDATA%\AURA\cache\ regenerable caches (waveforms, scans)
```

Rules:

* **Never install into a user's Documents folder.** Projects live wherever the user
  puts them; the installer does not create a "My Projects" graveyard.
* **Never take file associations by force.** `.aura` association is offered on
  first run (a checkbox), not assumed.
* Uninstall removes program files and leaves user data, with a checkbox to remove
  preferences. Deleting a user's projects is never the uninstaller's business.
* Everything regenerable is in `cache\` and labelled as such.

## Signing (the honest position)

* An unsigned installer triggers SmartScreen warnings; users are (correctly)
  trained to distrust them. Options: an OV certificate (moderate annual cost,
  reputation must build), an **EV** certificate (immediate reputation, higher cost),
  or **Azure Trusted Signing** (a newer, cheaper per-signature option that several
  projects have moved to).
* For a GPL project with no revenue, the v1 position is: **ship signed if a
  certificate exists, otherwise ship unsigned with SHA-256 checksums published**
  and clear instructions. The research note is that checksums do not remove the
  warning, they only let a cautious user verify the artefact.
* Release artefacts: `AURA-DAW-<version>-x64.exe` (installer),
  `AURA-DAW-<version>-x64-portable.zip`, `SHA256SUMS.txt`, plus the GPL source
  tarball for the exact tag.

## Versioning and update story

* The version comes from `include/aura/core/Version.hpp` (`AURA_VERSION_STRING`),
  injected into the installer script at build time — one source of truth.
* v1 has **no self-updater** (an updater is a background service, and the brief
  explicitly rejects unnecessary background services). The app checks a static
  release feed at most once per launch, only when the user allows it, and links to
  the download page.
* The project format has its own version; the installer records which app version
  wrote settings, so a downgrade can warn instead of silently half-reading state.

## Build integration

```
cmake --preset release
cmake --build --preset release
cpack -G ...            # optional: a ZIP from the CMake install rules
iscc packaging/aura.iss # Inno Setup compile step, run on the Windows CI job
```

The `.iss` script is checked in, takes the version and the source directory as
`/D` defines, and never hard-codes paths. The Windows Release job in CI uploads
the installer and the portable ZIP as artefacts once the tests are green.

## Accepted costs

* Inno Setup means Pascal Script for anything clever (uninstall checks, per-user
  migration); that is a real learning cost, paid once.
* No MSI means no Group Policy deployment; if a university or studio asks, a WiX
  build can be added later without changing the app.
* No auto-update means users must do it themselves; the upside is no always-on
  updater, no silent network traffic, and one fewer service to secure.

## Sources

* augmentedmind.de, *Distributing Windows applications* — MSIX limitations for
  desktop apps (per-user, unprivileged, no custom install dialogs) and the
  Inno/WiX/NSIS comparison table.
* edopedia.com, *Best installers for Windows programs* — Inno Setup feature set,
  ~1.5 MB overhead, Pascal scripting, dual signing support.
* r/dotnet thread on installers — practitioner consensus ("WiX is powerful but the
  learning curve is a cliff"; Inno Setup is free, professional and signable;
  Azure Trusted Signing as a cheaper signing route).
