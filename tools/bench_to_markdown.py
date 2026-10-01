#!/usr/bin/env python3
# ============================================================================
# AURA DAW - tools/bench_to_markdown.py
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# Turns `aura_bench --json` output into the Markdown table that goes into
# docs/PERFORMANCE.md, and checks the documented budgets against it.
#
# Usage:
#   aura_bench --json bench.json
#   python3 tools/bench_to_markdown.py bench.json                 # full table
#   python3 tools/bench_to_markdown.py bench.json --group engine   # one group
#   python3 tools/bench_to_markdown.py bench.json --budgets        # budget verdicts
#   python3 tools/bench_to_markdown.py bench.json --top 10         # tightest cases
#
# Exit status: 0 always (this publishes numbers; it does not judge a build). The
# budget check prints FAIL lines for a human to read, because a slow shared CI
# machine is not a reason to fail a pipeline.
# ============================================================================

from __future__ import annotations

import argparse
import json
import pathlib
import sys

# The budgets in docs/PERFORMANCE.md, expressed the way a machine can check them:
# a case name (substring) and the fraction of one core's block period it may use.
BUDGETS = [
    ("dsp/instrument-64-voices", 0.50, "64 voices must fit in 50% of the block period"),
    ("engine/session-16x8-fx", 0.50, "16-track session with FX must fit in 50% of the block period"),
    ("engine/session-48x8-fx", 1.00, "48-track session with FX must still fit in one core"),
]


def load(path: pathlib.Path) -> dict:
    with path.open() as handle:
        return json.load(handle)


def table(results: list[dict], group: str | None) -> str:
    rows = [r for r in results if not group or r["group"] == group]
    if not rows:
        return f"_no results for group '{group}'_\n"
    out = ["| case | block | ch | ns/frame | p95/median | x-realtime |", "|---|---|---|---|---|---|"]
    for r in sorted(rows, key=lambda r: (r["group"], r["name"])):
        out.append(
            f"| `{r['name']}` | {r['blockSize']} | {r['channels']} | "
            f"{r['medianNsPerFrame']:.1f} | {r['p95NsPerFrame'] / r['medianNsPerFrame']:.2f} | "
            f"{r['xRealtime']:.0f}x |"
        )
    return "\n".join(out) + "\n"


def block_share(result: dict) -> float:
    """Fraction of one core's block period the case consumes.

    ns/frame * frames per block = nanoseconds per block; the block period at the
    case's sample rate is blockSize / sampleRate seconds. The ratio is what the
    audio thread's deadline actually constrains, and it is block-size
    independent in the limit (which is why a 50% budget can be stated once).
    """
    block_period_ns = 1.0e9 * result["blockSize"] / result["sampleRate"]
    block_cost_ns = result["medianNsPerFrame"] * result["blockSize"]
    return block_cost_ns / block_period_ns


def budget_report(results: list[dict]) -> str:
    lines = ["| case | block | share of one core | budget | verdict |", "|---|---|---|---|---|"]
    failed = 0
    for needle, allowed, description in BUDGETS:
        matches = [r for r in results if needle in r["name"]]
        if not matches:
            lines.append(f"| `{needle}*` | - | not measured | {allowed:.0%} | n/a |")
            continue
        worst = max(matches, key=block_share)
        share = block_share(worst)
        verdict = "PASS" if share <= allowed else "FAIL"
        if verdict == "FAIL":
            failed += 1
        lines.append(
            f"| `{worst['name']}` | {worst['blockSize']} | {share:.1%} | {allowed:.0%} "
            f"({description}) | **{verdict}** |"
        )
    return "\n".join(lines) + f"\n\n{failed} budget(s) not met on this machine.\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Publish aura_bench numbers as Markdown.")
    parser.add_argument("json", type=pathlib.Path)
    parser.add_argument("--group", help="only this group (dsp, chain, engine, instrument, metering)")
    parser.add_argument("--budgets", action="store_true", help="check the documented budgets")
    parser.add_argument("--top", type=int, default=0, help="print the N tightest cases instead")
    args = parser.parse_args(argv)

    document = load(args.json)
    results = document.get("results", [])
    if not results:
        print("no results in the file", file=sys.stderr)
        return 1

    print(f"<!-- {document.get('compiler', '?')}, {document.get('buildType', '?')} -->")
    if args.top:
        for r in sorted(results, key=lambda r: r["xRealtime"])[: args.top]:
            print(f"- `{r['name']}` — {r['xRealtime']:.1f}x realtime ({r['medianNsPerFrame']:.1f} ns/frame)")
        return 0
    if args.budgets:
        print(budget_report(results))
        return 0
    print(table(results, args.group))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
