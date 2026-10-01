#!/usr/bin/env python3
# ============================================================================
# AURA DAW - scripts/github_setup.py
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# Sets up the GitHub repository: labels, the M0-M12 milestones, and (optionally)
# the starter issues for the current milestone.
#
# Why Python and not the GitHub CLI: `gh` is a great tool, but this script has to
# run on a bare machine (a build server, a contributor's laptop) without an
# installation step, and the REST calls involved are three. Standard library only.
#
# Credentials: read from the GITHUB_TOKEN environment variable. The script never
# writes a token anywhere, never prints it, and the repository contains no
# credentials - see docs/SECURITY.md.
#
# Usage
# -----
#   GITHUB_TOKEN=<token> python3 scripts/github_setup.py owner/repo
#   GITHUB_TOKEN=<token> python3 scripts/github_setup.py owner/repo --issues
#   GITHUB_TOKEN=<token> python3 scripts/github_setup.py owner/repo --dry-run
#
# The script is idempotent: labels and milestones that already exist are left
# alone, so it is safe to re-run after the roadmap changes.
# ============================================================================
"""Creates AURA's labels, milestones and starter issues on GitHub."""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"

# ---------------------------------------------------------------------------
# Labels: areas, types, priorities, and the flags reviewers filter by.
# ---------------------------------------------------------------------------
LABELS: list[tuple[str, str, str]] = [
    ("area:core", "Core utilities: errors, math, json, log, fs", "5319e7"),
    ("area:dsp", "DSP processors and algorithms", "5319e7"),
    ("area:engine", "Audio engine, graph, transport, offline render", "5319e7"),
    ("area:audioio", "Audio devices: WASAPI, ASIO, mock", "5319e7"),
    ("area:midi", "MIDI, instruments, piano roll", "5319e7"),
    ("area:project", "Project format, media pool, save/recovery", "5319e7"),
    ("area:ui", "Desktop shell, widgets, workflow", "5319e7"),
    ("area:plugins", "VST3/CLAP hosting and scanning", "5319e7"),
    ("area:build", "CMake, CI, packaging, toolchain", "5319e7"),
    ("area:docs", "Documentation and the research record", "5319e7"),
    ("type:bug", "Something is wrong", "d73a4a"),
    ("type:feature", "A new capability", "0e8a16"),
    ("type:task", "Work that is not a bug or a feature", "fbca04"),
    ("type:research", "Must be settled with evidence before coding", "c5def5"),
    ("priority:high", "Do next", "b60205"),
    ("priority:medium", "Normal queue", "fbca04"),
    ("priority:low", "Nice to have", "c2e0c6"),
    ("good first issue", "Approachable for a newcomer", "7057ff"),
    ("data-loss", "Involves possible loss of a project or a take", "b60205"),
    ("rt-safety", "Touches the audio thread rules", "d93f0b"),
    ("accessibility", "Keyboard, screen reader, contrast, scaling", "1d76db"),
    ("performance", "Budget or benchmark related", "006b75"),
]

# ---------------------------------------------------------------------------
# Milestones M0-M12, mirroring docs/ROADMAP.md. Keep the two in step: the
# roadmap is the human-readable version, this is the one GitHub sees.
# ---------------------------------------------------------------------------
MILESTONES: list[tuple[str, str]] = [
    ("M0 Foundations", "Repository, CMake build, core utilities, licence, error model"),
    ("M1 Time and tempo", "960 PPQN, tempo map, BBT, SMPTE"),
    ("M2 DSP suite", "14 processors with analytic and property tests"),
    ("M3 Graph and streams", "Graph, inserts, sends, memory and disk streams"),
    ("M4 Audio engine", "Devices, transport, recording, mixer, offline render"),
    ("M5 Performance & hardening", "Benchmarks, sanitizers, fuzzing, delay compensation"),
    ("M6 Plug-in hosting", "VST3 adapter, out-of-process scanner"),
    ("M7 Plug-in sandbox + CLAP", "Out-of-process audio host, CLAP adapter"),
    ("M8 ASIO + device hardening", "ASIO backend, device matrix, hot-plug recovery"),
    ("M9 Desktop shell", "Win32 + Direct2D window, widgets, transport, mixer"),
    ("M10 Arrangement & piano roll", "Timeline canvas, clip editing, piano roll, browser"),
    ("M11 Packaging & release", "Installer, portable ZIP, checksums, release workflow"),
    ("M12 v1.0 polish", "Localisation, crash reports, docs, soak testing"),
]

# ---------------------------------------------------------------------------
# Starter issues: the M5 work queue from docs/ROADMAP.md, so the tracker is not
# an empty shell. Each one says what "done" means.
# ---------------------------------------------------------------------------
ISSUES: list[dict] = [
    {
        "title": "bench: micro-benchmark suite for the DSP primitives and processors",
        "labels": ["area:build", "performance", "type:task", "priority:high"],
        "milestone": "M5 Performance & hardening",
        "body": """`bench/` must give us numbers instead of opinions.

**Scope**
- biquad, delay, reverb, level meter, loudness, gain/pan/mix loops
- deterministic inputs, repeated runs, report **median and p95** (never the mean -
  an audio thread fails on the worst block)
- print the machine profile with the results

**Done when**
- `cmake -DAURA_BUILD_BENCHMARKS=ON` builds `aura_bench` and it runs in CI
- the numbers are pasted into `docs/PERFORMANCE.md`""",
    },
    {
        "title": "perf: session-scale benchmark (N tracks x M clips) at 64-2048 frame buffers",
        "labels": ["area:engine", "performance", "type:task", "priority:high"],
        "milestone": "M5 Performance & hardening",
        "body": """The micro-benchmarks say nothing about a real session.

**Scope**
- build synthetic sessions (1, 8, 24, 48 tracks; 4 clips each), render through the
  real `AudioEngine` at 64/128/256/512/1024/2048 frame buffers
- report callback time distribution, `cpuLoad` / `peakCpuLoad`, xruns
- confirm the silent-node skipping actually pays off (tracks playing vs. not)

**Done when**
- the 24-track result is published in `docs/PERFORMANCE.md` against the documented
  "modest PC" profile (2 cores, 8 GB, 5400 rpm disk)""",
    },
    {
        "title": "hardening: fuzz the project manifest and WAV chunk parsers",
        "labels": ["area:project", "type:task", "priority:medium"],
        "milestone": "M5 Performance & hardening",
        "body": """Both parsers take untrusted input (a user's file, a downloaded project).

**Scope**
- mutate `project.json` (truncated, huge counts, wrong types, path traversal,
  deeply nested JSON, non-UTF8)
- mutate WAV headers (absurd chunk sizes, missing `data`, non-finite float samples,
  zero channels)
- run under ASAN/UBSAN; a crash or an unbounded allocation is a bug

**Done when**
- every finding is fixed, with a regression test per input class
- the corpus is checked in under `tests/fuzz/` and wired into the Linux CI job
  (time-boxed, e.g. 60 s)""",
    },
    {
        "title": "hardening: memory-growth soak (8 h simulated playback, recording on and off)",
        "labels": ["area:engine", "performance", "type:task", "priority:medium"],
        "milestone": "M5 Performance & hardening",
        "body": """A DAW that grows 1 MB/minute is unusable for a long session, and the leak
is always somewhere boring.

**Scope**
- 8 h of simulated playback with recording on and off, plug-in chains, meters and
  the waveform worker running
- sample RSS and allocation counters; fail if the trend is upward beyond the
  preallocated high-water mark

**Done when**
- the harness exists and a run completes with a flat memory profile
- any leak found is fixed with a test that would catch it""",
    },
    {
        "title": "engine: apply delay compensation to dry paths",
        "labels": ["area:engine", "rt-safety", "type:task", "priority:high"],
        "milestone": "M5 Performance & hardening",
        "body": """`AudioEngine::totalLatencySamples()` reports the insert-chain latency today,
but `compensatedLatency_` is deliberately 0 because no compensating delay is
actually inserted yet (see the comment in `applyDelayCompensation()`).

**Scope**
- insert the matching delay on dry paths so parallel tracks stay phase-aligned
- keep the reported number honest: the compensation delay counts, a planned one
  does not
- rebuild compensation when the plan changes (a plug-in reporting latency late)

**Done when**
- a track with a 128-sample-latency insert and a dry track stay sample-aligned
- the test proves it by measuring the alignment, not by reading a number""",
    },
    {
        "title": "research: decide the SIMD path for gain/pan/mix/metering from profile data",
        "labels": ["area:dsp", "performance", "type:research", "priority:low"],
        "milestone": "M5 Performance & hardening",
        "body": """Open decision O-4 in `docs/research/TECHNOLOGY_DECISION_RECORD.md`.

**Question:** is the compiler sufficient, or do the trivially vectorisable loops
(gain, pan, mix, sanitize, metering) need explicit SSE2?

**Evidence needed**
- profile share of the callback spent in those loops on the reference machine
- measured before/after, not a guess; SSE2 is baseline on x64 so no dispatch is
  required, AVX2 would need runtime gating
- record the outcome (including "no change") in
  `docs/research/PERFORMANCE_RESEARCH.md`""",
    },
    {
        "title": "docs: publish measured performance numbers and update the budgets",
        "labels": ["area:docs", "performance", "type:task", "priority:medium"],
        "milestone": "M5 Performance & hardening",
        "body": """`docs/PERFORMANCE.md` currently states targets and marks most of them as
"measured in M5". Once the benchmark harness exists, replace the targets with
results, and be explicit about which budget is *not* met yet.

**Done when**
- every row in the budget table says either "measured: X on machine Y" or
  "not measured yet"
- the reference machine profile is described precisely enough to compare against""",
    },
]


def request(token: str, method: str, path: str, payload: dict | None = None):
    url = path if path.startswith("http") else f"{API}{path}"
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "aura-repo-setup")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            body = response.read().decode()
            return response.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as error:
        body = error.read().decode()
        try:
            return error.code, json.loads(body)
        except json.JSONDecodeError:
            return error.code, {"message": body[:200]}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="AURA GitHub repository setup")
    parser.add_argument("repo", help="owner/name")
    parser.add_argument("--issues", action="store_true", help="also create the starter issues")
    parser.add_argument("--dry-run", action="store_true", help="print what would change")
    args = parser.parse_args(argv)

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token and not args.dry_run:
        print("error: set GITHUB_TOKEN (the script never reads a token from a file)", file=sys.stderr)
        return 2

    repo = args.repo
    status, info = request(token, "GET", f"/repos/{repo}")
    if status != 200:
        print(f"error: cannot read {repo}: {info.get('message')}", file=sys.stderr)
        return 1
    print(f"repository: {info['full_name']} (private={info['private']})")

    # --- labels -----------------------------------------------------------
    _, existing_labels = request(token, "GET", f"/repos/{repo}/labels?per_page=100")
    have = {label["name"] for label in (existing_labels or [])}
    created_labels = 0
    for name, description, colour in LABELS:
        if name in have:
            continue
        if args.dry_run:
            print(f"  + label {name}")
        else:
            code, _ = request(
                token, "POST", f"/repos/{repo}/labels",
                {"name": name, "description": description, "color": colour},
            )
            if code not in (200, 201):
                print(f"  ! label {name} failed ({code})", file=sys.stderr)
        created_labels += 1
    print(f"labels: {created_labels} created, {len(have)} already present")

    # --- milestones -------------------------------------------------------
    _, existing_milestones = request(
        token, "GET", f"/repos/{repo}/milestones?state=all&per_page=100"
    )
    titles = {milestone["title"] for milestone in (existing_milestones or [])}
    created_milestones = 0
    for title, description in MILESTONES:
        if title in titles:
            continue
        if args.dry_run:
            print(f"  + milestone {title}")
        else:
            code, _ = request(
                token, "POST", f"/repos/{repo}/milestones",
                {"title": title, "description": description, "state": "open"},
            )
            if code not in (200, 201):
                print(f"  ! milestone {title} failed ({code})", file=sys.stderr)
        created_milestones += 1
    print(f"milestones: {created_milestones} created, {len(titles)} already present")

    # --- starter issues ---------------------------------------------------
    if args.issues:
        _, milestones = request(
            token, "GET", f"/repos/{repo}/milestones?state=all&per_page=100"
        )
        by_title = {milestone["title"]: milestone["number"] for milestone in (milestones or [])}

        _, open_issues = request(token, "GET", f"/repos/{repo}/issues?state=all&per_page=100")
        existing_titles = {issue["title"] for issue in (open_issues or [])}

        created_issues = 0
        for issue in ISSUES:
            if issue["title"] in existing_titles:
                continue
            payload = {
                "title": issue["title"],
                "body": issue["body"],
                "labels": issue["labels"],
            }
            number = by_title.get(issue["milestone"])
            if number is not None:
                payload["milestone"] = number
            if args.dry_run:
                print(f"  + issue {issue['title']}")
            else:
                code, result = request(token, "POST", f"/repos/{repo}/issues", payload)
                if code not in (200, 201):
                    print(f"  ! issue '{issue['title']}' failed ({code}): {result.get('message')}",
                          file=sys.stderr)
            created_issues += 1
        print(f"issues: {created_issues} created, {len(existing_titles)} already present")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
