# Contributing to AURA

Thanks for wanting to help. AURA is GPL-3.0-or-later and contributions are welcome
under the same licence.

## Before you write code

1. **Read the rule that matters most:** nothing on the audio thread allocates,
   locks, logs or touches a file. `docs/AUDIO_ENGINE.md` and
   `docs/research/THREADING_RESEARCH.md` explain why and how it is enforced.
2. **Check the research record** for the area you are touching
   (`docs/research/`). If you disagree with a decision, the way to change it is to
   argue with the document — the doc gets updated *with* the code, not after.
3. **Prefer original work.** No copied UI assets, no reverse-engineered commercial
   DAWs, no bundled third-party instruments or sample content, no proprietary code.
   Verify the licence of anything you bring in (`docs/LICENSES.md`).

## Workflow

```bash
git switch -c feature/thing-you-are-doing
# ... change code and docs ...
cmake -S . -B build -DAURA_BUILD_TESTS=ON && cmake --build build -j 2
cd build && ctest --output-on-failure
python3 ../tools/rt_audit.py ..
git commit    # tests green, docs updated in the same commit
```

House rules (from the project brief, enforced in review):

* feature branches; small, focused commits;
* **never** force-push a branch others may have, and never `main`;
* never delete code you do not understand — ask what it is for first;
* tests green before you commit, build green before you merge;
* `main` stays working: no "I'll fix it in the next commit".

## What a good pull request contains

| Element | Why |
|---------|-----|
| The change, focused on one thing | reviewable |
| A test that fails without it (or an explanation of why a test is impossible) | the project treats tests as evidence, not decoration |
| Doc updates (the matching `docs/*.md`, and `docs/research/` if a decision changed) | a stale document is worse than none |
| A note on real-time impact if you touched the audio path | "still zero allocations, audit clean" |
| Numbers if it is a performance change (median + p95, and the machine) | benchmarks on one machine are anecdotes elsewhere |

## Where help is most useful

Look for `good first issue` on the repository. Areas that are wide open:

* VST3/CLAP hosting (M6/M7) — the host surface exists and is tested with a fake
  plug-in; the adapters are the work.
* The desktop shell (M9/M10) — Win32 + Direct2D, widget toolkit, timeline canvas.
* Benchmarks and profiling (M5) — `bench/` needs a session-scale harness.
* Localisation catalogues (M12) — Sinhala, Tamil, German, Japanese. The
  architecture is in place: add a JSON catalogue, and the pseudo-localisation pass
  will tell you where the layout breaks.
* Documentation — every claim in `docs/` should be checkable against the code. If
  you find one that is not, that is a valuable issue.

## Code review expectations

Reviewers will ask about: allocation and locks on the audio path; whether a new
failure mode is diagnosable by a user; whether the tests would catch a regression;
whether the docs changed; and whether a new dependency earned its place. Nothing
personal — those are the questions that make a DAW survivable.

## Code of conduct

Be straightforward, be kind, assume good faith, criticise code and not people.
Harassment or exclusionary behaviour is not welcome here; maintainers may remove
comments or contributions that violate this and will say why.

## Licence of contributions

By contributing you agree your work is licensed under GPL-3.0-or-later, and you
confirm you have the right to submit it (no copied code, no employer restrictions).
Add the standard file header with the SPDX identifier to new files.
