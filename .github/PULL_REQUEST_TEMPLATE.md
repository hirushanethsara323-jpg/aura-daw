<!-- Keep it short. Reviewers care about: what was wrong, what changed, what proves it. -->

## What this changes

<!-- One paragraph. The "why" matters more than the "what". -->

## Why

<!-- Link the issue, the research document, or the bug this fixes. -->

## How it was verified

- [ ] `ctest --output-on-failure` is green locally
- [ ] `python3 tools/rt_audit.py .` is clean (or the escape is annotated with a reason)
- [ ] new/updated test that fails without this change: `tests/...`
- [ ] documentation updated in this same commit: `docs/...`
- [ ] if a decision changed: `docs/research/...` updated

## Real-time impact

<!-- "None" is a fine answer, but say it explicitly if the audio path was touched. -->
- Audio thread: <!-- no new allocation/lock/IO / describe -->
- Parameters or latency behaviour changed: <!-- no / describe -->

## Project format / compatibility

<!-- Does this change the .aura format or refuse older/newer projects? -->

## Notes for the reviewer

<!-- Anything you are unsure about, deliberately deferred, or would like a second opinion on. -->
