#!/usr/bin/env bash
# ============================================================================
# AURA DAW - scripts/create_milestones.sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Creates the M0-M12 milestones, the label set and the issue categories on the
# GitHub repository from the command line.
#
# Authentication: this script uses whatever credentials *you* have configured
# (`gh auth login`, or GH_TOKEN exported in your own shell for this run). It
# never stores or reads a token from the repository, and there are no
# credentials in the code.
#
# Usage:  ./scripts/create_milestones.sh [owner/repo]
# ============================================================================
set -euo pipefail

REPO="${1:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
echo "Configuring $REPO"

if ! command -v gh >/dev/null 2>&1; then
  echo "error: the GitHub CLI (gh) is required: https://cli.github.com" >&2
  exit 1
fi

echo "--- labels ---"
create_label() { gh label create "$1" --repo "$REPO" --description "$2" --color "$3" 2>/dev/null || \
                 gh label edit  "$1" --repo "$REPO" --description "$2" --color "$3" >/dev/null; }

create_label "area:core"     "Core utilities: errors, math, json, log, fs"      "5319e7"
create_label "area:dsp"      "DSP processors and algorithms"                    "5319e7"
create_label "area:engine"   "Audio engine, graph, transport, offline render"   "5319e7"
create_label "area:audioio"  "Audio devices: WASAPI, ASIO, mock"                "5319e7"
create_label "area:midi"     "MIDI, instruments, piano roll"                    "5319e7"
create_label "area:project"  "Project format, media pool, save/recovery"        "5319e7"
create_label "area:ui"       "Desktop shell, widgets, workflow"                 "5319e7"
create_label "area:plugins"  "VST3/CLAP hosting and scanning"                   "5319e7"
create_label "area:build"    "CMake, CI, packaging, toolchain"                  "5319e7"
create_label "area:docs"     "Documentation and the research record"            "5319e7"
create_label "type:bug"        "Something is wrong"                              "d73a4a"
create_label "type:feature"    "A new capability"                                "0e8a16"
create_label "type:task"       "Work that is not a bug or a feature"             "fbca04"
create_label "type:research"   "Must be settled with evidence before coding"     "c5def5"
create_label "priority:high"   "Do next"                                         "b60205"
create_label "priority:medium" "Normal queue"                                    "fbca04"
create_label "priority:low"    "Nice to have"                                    "c2e0c6"
create_label "good first issue" "Approachable for a newcomer"                     "7057ff"
create_label "data-loss"       "Involves possible loss of a project or a take"    "b60205"
create_label "rt-safety"       "Touches the audio thread rules"                   "d93f0b"
create_label "accessibility"   "Keyboard, screen reader, contrast, scaling"       "1d76db"
create_label "performance"     "Budget or benchmark related"                      "006b75"

echo "--- milestones M0-M12 ---"
m() { gh api "repos/$REPO/milestones" -f title="$1" -f description="$2" >/dev/null 2>&1 || true; }

m "M0  Foundations"            "Repo, CMake build, core utilities, licence, error model"
m "M1  Time and tempo"         "960 PPQN, tempo map, BBT, SMPTE"
m "M2  DSP suite"              "14 processors with analytic and property tests"
m "M3  Graph and streams"      "Graph, inserts, sends, memory and disk streams"
m "M4  Audio engine"           "Devices, transport, recording, mixer, offline render"
m "M5  Performance & hardening" "Benchmarks, sanitizers, fuzzing, delay compensation"
m "M6  Plug-in hosting"        "VST3 adapter, out-of-process scanner"
m "M7  Plug-in sandbox + CLAP" "Out-of-process audio host, CLAP adapter"
m "M8  ASIO + device hardening" "ASIO backend, device matrix, hot-plug recovery"
m "M9  Desktop shell"          "Win32 + Direct2D window, widgets, transport, mixer"
m "M10 Arrangement & piano roll" "Timeline canvas, clip editing, piano roll, browser"
m "M11 Packaging & release"    "Installer, portable ZIP, checksums, release workflow"
m "M12 v1.0 polish"            "Localisation, crash reports, docs, soak testing"

echo "Done. Existing labels and milestones were left in place."
