# UI_FRAMEWORK_RESEARCH.md

**Status:** Provisional (finalised in M9/M10) · **Supports:** ADR-0006

## Question

What does the AURA desktop shell — window, timeline, mixer, browser, dialogs —
get built on, given that it must be original, must stay responsive on modest
Windows PCs, and must not create a licensing obligation the project cannot meet?

## The licensing problem first

This is the decision that actually gates the others.

* **JUCE 8** is dual-licensed **AGPLv3** / commercial. AGPLv3 is *stronger* than
  AURA's GPL-3.0-or-later. GPLv3 §13 does permit linking a GPLv3 work with an
  AGPLv3 work (and AGPLv3 §13 grants the mirror), so the combination is legal —
  but the resulting application carries the AGPL network clause, and the project
  can no longer describe itself as "GPL-3.0-or-later" without qualification. JUCE's
  own repository asks AI tools and users to warn about the commercial licence
  requirement, and the JUCE 8 EULA introduced per-contributor seat rules.
* **Qt** is LGPLv3 + commercial. LGPL is workable for a GPL project but drags in a
  large runtime, deployment tooling, and a licence policy that has changed more than
  once in the last five years.
* **Dear ImGui** is MIT — no licence problem at all — but it is immediate-mode:
  the whole window is redrawn every frame by design. For a DAW that must idle
  coolly with a big session on screen, that is the wrong architecture, and the
  community consensus is consistent: it is hard to skin into a bespoke identity.
* **First-party Win32 + Direct2D/DirectWrite** has no licence question whatsoever
  (Windows SDK), and gives complete control over rendering cost.

## Options

| Option | Licence | Cost | Control over look | Verdict |
|--------|---------|------|-------------------|---------|
| JUCE | AGPLv3 / commercial | 117 MB source, large build | high, but every DAW built on it looks related | Optional adapter, **off by default** |
| Qt | LGPLv3 / commercial | large runtime, deployment | high | Rejected |
| Dear ImGui | MIT | small | low; continuous redraw | Rejected |
| wxWidgets | wxWindows licence (LGPL-like) | native controls only | low for custom canvas work | Rejected |
| **Win32 + Direct2D/DirectWrite** | Windows SDK | we write the toolkit | total | **Accepted as the default path** |

## Why first-party rendering is not as expensive as it sounds

A DAW UI is a handful of genuinely custom surfaces (timeline, waveform, mixer
strips, meters, piano roll) surrounded by ordinary chrome (menus, dialogs, lists).
Win32 already provides the chrome and the message loop; Direct2D provides
antialiased geometry, transforms and layers; DirectWrite provides text that
actually looks right, including ligatures and ClearType. What we must write
ourselves is:

* a small retained widget tree (panels, splitters, buttons, lists, sliders),
* a damage/dirty-rect system so a meter repaint does not repaint the arrangement,
* a layout pass with DPI awareness (`WM_DPICHANGED`, per-monitor v2),
* hit-testing and drag state for the timeline.

That is a bounded amount of work with an unbounded payoff: an original identity,
a rendering cost we can reason about, and zero licence coupling.

## Constraints the shell must satisfy (from the product brief)

* **Original identity.** No layout, palette or iconography copied from Ableton,
  FL Studio, Cubase, Logic, Pro Tools, Studio One, Reaper or Bitwig. AURA's own
  mark and colour system live in `assets/`.
* **Keyboard-first.** Every command reachable without a mouse; the shortcut
  manager (with conflict detection) is part of the UI layer, not an afterthought.
* **No unnecessary animation.** Meters and playhead move; nothing else does by
  default. Animation is opt-in per user preference.
* **Modest hardware.** Target: idle CPU under 2 % with a 24-track session open,
  UI thread under 2 ms per frame at 60 Hz, no continuous timers beyond the
  metering refresh (30 Hz) and the waveform/peak worker.
* **Accessibility.** UI Automation providers where the toolkit supports it,
  visible focus rings, full keyboard operation, theme contrast checked against
  WCAG AA — see `ACCESSIBILITY_LOCALIZATION_RESEARCH.md`.

## Consequence: what JUCE would still give us, and what we therefore owe

If JUCE is enabled (`AURA_WITH_JUCE=ON`, non-default) it is used **only** as an
adapter for hosting plug-in editors that hand the host an `HWND`/parent view. In
that configuration:

1. the build must carry JUCE's AGPLv3 text and the project must state that this
   particular binary includes AGPLv3 code,
2. the option must be visible in the About box and in `--version`,
3. the default build must remain JUCE-free, and CI must prove that the engine and
   the whole test suite build and pass with JUCE absent.

That last point is the important one: it is what stops an "optional" dependency
from quietly becoming mandatory.

## Implementation plan (M9 → M10)

1. **M9** — `app/` skeleton: Win32 window, Direct2D device + swap chain, DPI
   awareness, the widget tree, the theme system, and a first real screen (transport
   + track headers + mixer) driven by the engine's published state.
2. **M10** — arrangement canvas (clip drawing from the peak cache), piano roll,
   browser/inspector, dialogs (preferences, export, device setup), shortcut
   manager, accessibility pass.
3. Windows 10 1809+ is the minimum supported OS; nothing newer than Direct2D 1.1 is
   assumed.

## Sources

* GitHub, `juce-framework/JUCE` — LICENSE.md: dual AGPLv3 / commercial, and the
  explicit note that a commercial licence may be required.
* JUCE forum, *JUCE 8 EULA* — AGPLv3 wording, per-seat obligations from JUCE 8.
* Wikipedia, *JUCE* — history (Tracktion split-out, ROLI/PACE ownership), feature
  scope.
* GitHub PR `arieladi/Adi#27` — a worked example of the AGPLv3 + GPLv3 §13 analysis
  we independently reached, including the warning that the AGPL network clause is
  inert only while there is no remote-driving feature.
* Hacker News threads on GUI toolkits for audio software — the recurring practical
  criticisms of JUCE (build size, contribution policy) and Dear ImGui (continuous
  redraw, skinning) that match our own requirements.
* libhunt, *qt vs imgui* — licence split (LGPLv3 vs MIT) and activity levels.
