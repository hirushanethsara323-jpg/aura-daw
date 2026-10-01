# ACCESSIBILITY_LOCALIZATION_RESEARCH.md

**Status:** Accepted · **Code:** `aura::loc` (`include/aura/util/Localization.hpp`, `src/core/Localization.cpp`)

## Two questions, one document, because they are the same question

"Can someone who is not me use this?" — a screen-reader user, a keyboard-only user,
or someone who does not read English. Both answers are decided at the *beginning*
or not at all: retrofitting string externalisation or focus semantics into a mature
UI costs ten times what doing it from the first widget costs.

## Localization

**Architecture (shipped):**

* `loc::Translator` holds a catalogue: language code, display name, fallback code,
  and a keyed string map. Every user-visible string is addressed by a dotted key
  (`transport.play`, `error.audio_device_lost`), never by its English text.
* **English is a normal catalogue**, not a special case: it is loaded through the
  same path as any other, so a missing key in another language falls back to
  English and a missing key everywhere returns the key itself — visible in the UI
  rather than silently blank (and recorded in `missingKeys()` so a developer sees
  what to add).
* `loc::tr(key)` / `loc::trf(key, args)` are the only lookup functions; the
  `Translator` also owns the error-message catalogue
  (`translateError(AuraError)`) so a diagnostic is never a raw code.
* `Translator::setLanguage()` refuses a language it has no catalogue for, keeps
  English active, and returns a diagnosable error — the UI says "Sinhala is
  planned; continuing in English" instead of showing empty labels. Covered by tests.
* Keys are checked in as JSON, one file per language, with the English file being
  the reference.

**Languages:**

| Language | Status | Note |
|----------|--------|------|
| English | **shipped** | reference catalogue (~150 keys) |
| Sinhala (si) | planned | the project's home context; needs correct shaping and font fallback |
| Tamil (ta) | planned | same script-family concerns |
| German (de) | planned | long compound words — the layout must survive ~1.4× text growth |
| Japanese (ja) | planned | needs an IME-aware text field and CJK font fallback |

**Rules that keep it honest:**

1. No string concatenation to build sentences ("Recording " + count + " takes"):
   use placeholders in the catalogue, so word order is translatable.
2. No assumptions about text direction in *widgets* even though the planned
   languages are LTR — layout mirrors are cheap now, expensive later.
3. Font fallback is explicit: a missing glyph must fall back (DirectWrite's font
   fallback chain) rather than render as a box.
4. The German/Sinhala layouts are tested by *pseudo-localisation* (a debug
   catalogue that lengthens and accents every string) before a real translation
   exists.

## Accessibility

| Area | Decision |
|------|----------|
| Keyboard | every command has a command id and is reachable via `KeyboardShortcutManager`; no mouse-only operation exists in the command table |
| Focus | a visible focus ring on every focusable widget; the focus chain follows the visual layout (arrangement → track headers → mixer, tab order documented) |
| Screen readers | UI Automation providers (`IRawElementProviderSimple` / fragment providers) for the shell's custom widgets. Custom-drawn canvases are the hard part: the arrangement exposes clips as list items with name/position/length, and the mixer exposes each strip as a slider group with value + units |
| Contrast | default theme checked against WCAG AA (4.5:1 for text, 3:1 for UI boundaries); meter colours are not the only carrier of meaning (clip/peak indicators also change shape/label) |
| Motion | animation is off by default except for meters and the playhead; a preference disables those too |
| Text scaling | DPI-aware rendering (per-monitor v2) and a user font-size multiplier; the layout must reflow rather than clip |
| Colour blindness | selection and mute/solo states carry an icon/shape difference as well as a colour |
| Command palette | a searchable list of all commands with their shortcuts, so a user never has to find a menu by hand |

## The shortcut manager

* Commands are registered once (`commands::ids::*`) with a default shortcut,
  category and description; the manager maps keys to ids and is rebindable at
  runtime with the binding stored in preferences.
* **Conflict detection is the point**: assigning a key that is already taken
  reports the conflict and refuses, rather than silently shadowing a command the
  user relies on. Import/export of the binding set is planned with the preferences
  work.
* Reserved keys (OS shortcuts) are documented and cannot be taken.

## Test coverage today

* Localisation: English fallback for a missing catalogue, missing-key behaviour,
  `setLanguage("si")` refusal keeping English active (`tests/core/CoreTests.cpp`).
* Commands: registry, ids, search ranking, shortcut metadata
  (`tests/commands/CommandsTests.cpp`).
* Accessibility has **no automated tests yet** — it is enforced by review and by
  the pseudo-localisation pass, and `../TESTING.md` lists it as a known gap rather
  than claiming coverage that does not exist.

## Sources

* Microsoft, *UI Automation* documentation — provider interfaces for custom
  controls, and the "automation peer" model our widget tree mirrors.
* Microsoft, *High DPI desktop application development* — per-monitor v2 DPI
  awareness and `WM_DPICHANGED`.
* WCAG 2.2 — contrast requirements (1.4.3, 1.4.11) and the principle that colour
  must not be the sole carrier of information (1.4.1).
* Mozilla/Google pseudo-localisation practice — the technique used to test layout
  growth before real translations exist.
