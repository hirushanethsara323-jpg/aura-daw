#!/usr/bin/env python3
# ============================================================================
# AURA DAW - tools/rt_audit.py
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Static real-time-safety audit for the AURA audio engine.
#
# What it is
# ----------
# A heuristic source scan that looks for operations which are not allowed on the
# audio thread (allocation, locks, file I/O, logging, unbounded container
# growth, exceptions) *inside functions that the audio thread can call*.
#
# It is a fast, dependency-free gate for CI and for local use. It is NOT a
# proof: it uses a light-weight C++ scanner (comments and string literals are
# stripped, brace depth is tracked to find function bodies) rather than a real
# parser, so it can miss things and it will occasionally need an explicit
# allow-list entry. The real guarantees come from the tests
# (tests/engine/EngineTests.cpp runs the whole block path with allocation
# tracking enabled) and from review.
#
# Usage
# -----
#   python3 tools/rt_audit.py [repo-root] [--list] [--json] [--quiet]
#
# Exit codes: 0 = clean (or only warnings with --allow-warnings), 1 = findings.
#
# Escaping a finding
# ------------------
# If an operation is genuinely safe in a specific place (e.g. a container that
# was reserved in prepare() and only ever indexed), annotate the line or the
# line above it:
#
#     buffer_.assign(n, 0.0f);   // rt-audit: allow - capacity reserved in prepare()
#
# ============================================================================
"""AURA real-time-safety audit."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Scope: which files, which functions
# ---------------------------------------------------------------------------

# Files that can be reached from the audio callback. Globs are matched against
# the repository-relative path with forward slashes.
RT_SCOPE_GLOBS = [
    "include/aura/dsp/*.hpp",
    "src/dsp/*.cpp",
    "include/aura/graph/*.hpp",
    "src/graph/*.cpp",
    "include/aura/audio/*.hpp",
    "src/audio/AudioClip.cpp",
    "src/audio/RecordingEngine.cpp",
    "include/aura/engine/*.hpp",
    "src/engine/AudioEngine.cpp",
    "include/aura/midi/*.hpp",
    "src/midi/Instrument.cpp",
    "include/aura/automation/*.hpp",
    "src/automation/Automation.cpp",
    "include/aura/transport/*.hpp",
    "src/transport/Transport.cpp",
    "src/audioio/WasapiDevice.cpp",
    "src/audioio/AsioDevice.cpp",
    # M7: the sandbox transport is on the audio path by definition - the host's
    # push/pop pair is called from inside the device callback, once per block.
    "include/aura/plugin/sandbox/*.hpp",
    "src/plugin/sandbox/*.cpp",
    # M7: the helper's loop is a real-time thread in every sense the engine's
    # callback is - it must not allocate, lock, touch a file, log or throw. Auditing
    # it is what makes that a property of the build rather than an intention of
    # whoever wrote it. It is allowed to SLEEP when idle, which the engine's
    # callback is not, and nothing in the check list forbids sleeping.
    "tools/plugin_host/*.cpp",
]

# Function names (matched on the identifier before the opening parenthesis) that
# run on the audio thread. Anything matching is scanned.
RT_FUNCTION_PATTERNS = [
    r"^process$", r"^processBlock$", r"^processSample$", r"^processGain$",
    r"^processInterleaved$", r"^render.*", r"^read$", r"^write$",
    r"^next$", r"^nextSample$", r"^tick$",
    r"^sampleInto$", r"^valueAt$", r"^advance$", r"^clear$",
    r"^fillRamp$", r"^applyGain$", r"^applyGainSmoothing$", r"^setTargetGains$",
    r"^noteOn$", r"^noteOff$", r"^snapTo$", r"^applyMixerStateToGraph$",
    r"^drain$", r"^renderClips$", r"^renderInstruments$", r"^renderRecording$",
    r"^callback$", r"^onAudio.*",
    # Lock-free boundary crossings: a queue's push/pop run on whichever side of the
    # boundary the caller is on, and for the sandbox transport that side is the
    # audio thread. Named explicitly rather than as "^write$/^read$" so the audit
    # reads like the contract it enforces.
    r"^push$", r"^pop$", r"^push[A-Z].*", r"^pop[A-Z].*",
    r"^exchange$", r"^publish.*",
    # The plug-in helper's real-time loop. Named rather than matched by a shape so
    # that the audit and the comment above it stay about one specific function.
    r"^runHelperLoop$",
]

# Functions that look like they are in scope but are documented control-thread
# entry points: prepare/reset/open/close/configure never run on the audio
# thread, so they are excluded even when the file is in scope.
CONTROL_THREAD_FUNCTIONS = [
    r"^prepare$", r"^reset$", r"^open$", r"^close$", r"^create$", r"^init.*",
    r"^set[A-Z].*", r"^configure.*", r"^load.*", r"^save.*", r"^scan.*",
    r"^enumerate.*", r"^start$", r"^stop$", r"^shutdown$", r"^rebuild.*",
    r"^applyDelayCompensation$",
    # The control-thread half of rebuildGraph(): it swaps the plan pointer and
    # retires the old one into a vector, which is exactly what the audio thread may
    # not do. Its header states the contract ("Call this only from the control
    # thread") and the audio thread reaches the plan through a bare atomic pointer
    # instead. Added when ^publish.* joined the RT patterns, which had left this
    # function unaudited until then - it is control-thread, not a violation.
    r"^publishPlan$",
    # Lane::sampleInto() fills a caller-owned std::vector for the UI to draw and for
    # the offline renderer to pre-compute a curve - it reserves and push_backs, which
    # no audio-thread function may do, and its only caller in the tree is a test.
    # Per-block automation on the audio path goes through valueAt() instead.
    r"^sampleInto$",
]

# ---------------------------------------------------------------------------
# Banned operations
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Banned:
    name: str
    pattern: str
    reason: str
    severity: str = "error"  # "error" | "warning"


BANNED: list[Banned] = [
    # --- allocation -------------------------------------------------------
    Banned("new", r"(?<![\w:.>])new\s+[A-Za-z_]", "dynamic allocation on the audio thread", ),
    Banned("new-array", r"(?<![\w:.>])new\s+\w+\s*\[", "dynamic allocation on the audio thread"),
    Banned("malloc", r"\b(malloc|calloc|realloc)\s*\(", "C heap allocation"),
    Banned("make-shared", r"\bstd::make_shared\s*<|\bstd::make_unique\s*<", "heap allocation"),
    Banned("shared-ptr-ctor", r"\bstd::shared_ptr\s*<[^>]*>\s*\(", "reference-counted allocation"),
    # Only an object *definition* can allocate: `const std::vector<float>& view`
    # (a parameter) and `std::vector<float>& inserts()` (a return type) cannot.
    Banned(
        "vector-ctor",
        r"\bstd::vector\s*<[^;{}()]*>\s*[A-Za-z_]\w*\s*[;=({]",
        "container construction may allocate",
    ),
    Banned("string-ctor", r"\bstd::string\b", "string construction/copy allocates"),
    Banned("function-ctor", r"\bstd::function\s*<", "type erasure allocates on assignment"),
    # --- unbounded container growth --------------------------------------
    Banned("push-back", r"\.(push_back|emplace_back)\s*\(", "container growth"),
    Banned("resize", r"\.(resize|reserve|assign)\s*\(", "container (re)allocation"),
    Banned("insert", r"\.(insert|emplace)\s*\(", "container growth"),
    # --- locks ------------------------------------------------------------
    Banned("mutex", r"\bstd::mutex\b|\brecursive_mutex\b|\bshared_mutex\b", "blocking synchronisation"),
    Banned("lock-guard", r"\b(lock_guard|unique_lock|scoped_lock|shared_lock)\b", "blocking synchronisation"),
    Banned("lock-call", r"\.lock\s*\(\s*\)", "blocking synchronisation"),
    Banned("condition", r"\bstd::condition_variable\b", "blocking synchronisation"),
    Banned("thread-ctor", r"\bstd::thread\b", "thread creation"),
    Banned("join", r"\.(join|detach)\s*\(", "blocking thread operation"),
    # --- file / device I/O -----------------------------------------------
    Banned("fopen", r"\b(std::)?(fopen|freopen)\s*\(", "file I/O"),
    Banned("stream-io", r"\bstd::(ifstream|ofstream|fstream)\b", "file I/O"),
    Banned("fread", r"\b(fread|fwrite|fseek|fclose)\s*\(", "file I/O"),
    Banned("filesystem", r"\bstd::filesystem\b|\bfilesystem::", "file I/O"),
    Banned("printf", r"\b(std::)?(printf|fprintf|puts)\s*\(", "I/O in the audio callback"),
    # --- exceptions / logging --------------------------------------------
    Banned("throw", r"\bthrow\b", "exception propagation", severity="warning"),
    Banned("log", r"\bAURA_LOG_[A-Z]+\s*\(", "logging may allocate or block", severity="warning"),
    # --- allocation behind standard algorithms ---------------------------
    Banned("stable-sort", r"\bstd::stable_sort\s*\(", "stable_sort allocates"),
    Banned("map", r"\bstd::(map|unordered_map|set|unordered_set)\s*<", "node-based container allocation"),
]

# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

ALLOW_MARKER = re.compile(r"//\s*rt-audit:\s*allow", re.IGNORECASE)


@dataclass
class Finding:
    path: str
    line: int
    function: str
    rule: str
    severity: str
    reason: str
    text: str


@dataclass
class ScanStats:
    files_scanned: int = 0
    functions_scanned: int = 0
    allows: list[str] = field(default_factory=list)


def strip_comments_and_strings(line: str, in_block_comment: bool) -> tuple[str, bool]:
    """Returns (code-only text, still_in_block_comment).

    Removes // line comments, /* */ block comments (tracked across lines) and
    the contents of string/char literals, so a mention of `new` inside a comment
    or a log message never trips the audit.
    """
    out: list[str] = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                return "".join(out), True
            i = end + 2
            in_block_comment = False
            continue
        ch = line[i]
        two = line[i : i + 2]
        if two == "//":
            break
        if two == "/*":
            in_block_comment = True
            i += 2
            continue
        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block_comment


SIGNATURE_STOP = re.compile(r"[;{}]")
CONTROL_KEYWORDS = {"if", "for", "while", "switch", "catch", "else", "do", "return"}


def extract_functions(path: str, text: str):
    """Yields (name, start_line, end_line, body_lines).

    `body_lines` are `(line_number, code_without_comments, raw_line)` triples: the
    stripped text is what the rules are matched against, the raw text is needed to
    see annotations such as `// rt-audit: allow ...`.

    Heuristic: a function body starts at a `{` whose pending declaration text
    contains a `(` and is not a control statement, an initialiser list or a
    lambda. Good enough for AURA's style (K&R braces, definitions at file scope).
    """
    lines = text.splitlines()
    in_block = False
    pending = ""
    pending_line = 0
    depth = 0
    current: tuple[str, int, list[tuple[int, str]]] | None = None

    for number, raw in enumerate(lines, start=1):
        code, in_block = strip_comments_and_strings(raw, in_block)
        if current is None:
            if "{" in code:
                head, _, tail = code.partition("{")
                signature = (pending + " " + head).strip()
                name = function_name_from_signature(signature)
                if name:
                    current = (name, number, [])
                    depth = 1
                    pending = ""
                    pending_line = 0
                    # Body text after the brace on the same line. Its braces have to
                    # be counted too: a one-line definition
                    # (`Type C::f() noexcept { return g(); }`) closes on the line it
                    # opened, and not counting that close left the scanner believing
                    # it was still inside `f` for the rest of the translation unit -
                    # every function after it went unaudited. Found by the sandbox
                    # transport, whose accessors are one-liners.
                    if tail.strip():
                        current[2].append((number, tail, raw))
                        depth += tail.count("{") - tail.count("}")
                        if depth <= 0:
                            yield current[0], current[1], number, current[2]
                            current = None
                            pending = ""
                            pending_line = 0
                else:
                    pending = ""
                    pending_line = 0
                continue
            pending += " " + code
            if not pending.strip():
                pending_line = number
            if SIGNATURE_STOP.search(code):
                pending = ""
                pending_line = 0
            continue

        # Inside a body.
        opens = code.count("{")
        closes = code.count("}")
        current[2].append((number, code, raw))
        depth += opens - closes
        if depth <= 0:
            yield current[0], current[1], number, current[2]
            current = None
            pending = ""
            pending_line = 0


def function_name_from_signature(signature: str) -> str | None:
    signature = signature.strip()
    if not signature or "(" not in signature:
        return None
    # Reject initialiser lists, lambdas, control statements.
    if signature.startswith("=") or "= [" in signature or signature.endswith("="):
        return None
    if "[" in signature.split("(")[0]:
        return None
    head = signature.split("(")[0].strip()
    head = head.split("::")[-1].strip()  # Class::method
    match = re.search(r"([A-Za-z_~][A-Za-z0-9_]*)$", head)
    if not match:
        return None
    name = match.group(1)
    if name in CONTROL_KEYWORDS:
        return None
    if "operator" in head:
        return None
    return name


def matches_any(name: str, patterns: list[str]) -> bool:
    return any(re.match(pattern, name) for pattern in patterns)


def scan_file(repo_root: str, rel_path: str, stats: ScanStats) -> list[Finding]:
    full_path = os.path.join(repo_root, rel_path)
    try:
        with open(full_path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:  # pragma: no cover - defensive
        print(f"rt_audit: cannot read {rel_path}: {error}", file=sys.stderr)
        return []

    stats.files_scanned += 1
    findings: list[Finding] = []
    allowed_lines: set[int] = set()

    for name, start, end, body in extract_functions(rel_path, text):
        if not matches_any(name, RT_FUNCTION_PATTERNS):
            continue
        if matches_any(name, CONTROL_THREAD_FUNCTIONS):
            continue
        stats.functions_scanned += 1
        # An allow comment annotates the *statement* that follows it, which may
        # wrap over several lines (`if (guard)\n    push_back(...)`), so keep
        # marking lines until the statement's terminator.
        marking = False
        for line_number, code, raw in body:
            # The marker text itself may contain ';' (e.g. "reserved in prepare();"),
            # so statement boundaries are judged on the comment-free text.
            statement_text = code
            if ALLOW_MARKER.search(raw):
                marking = True
                allowed_lines.add(line_number)
                stats.allows.append(f"{rel_path}:{line_number} ({name})")
                if ";" in statement_text:
                    marking = False
                continue
            if marking:
                allowed_lines.add(line_number)
                if ";" in statement_text or statement_text.count("{") > statement_text.count("}"):
                    marking = False

    for name, start, end, body in extract_functions(rel_path, text):
        if not matches_any(name, RT_FUNCTION_PATTERNS):
            continue
        if matches_any(name, CONTROL_THREAD_FUNCTIONS):
            continue
        for line_number, code, raw in body:
            if line_number in allowed_lines or not code.strip():
                continue
            for rule in BANNED:
                if re.search(rule.pattern, code):
                    findings.append(
                        Finding(
                            path=rel_path,
                            line=line_number,
                            function=name,
                            rule=rule.name,
                            severity=rule.severity,
                            reason=rule.reason,
                            text=code.strip()[:160],
                        )
                    )
    return findings


def collect_files(repo_root: str) -> list[str]:
    matches: list[str] = []
    for root, dirs, files in os.walk(repo_root):
        dirs[:] = [
            d for d in dirs
            if d not in {"build", ".git", "_deps", "node_modules", "dist", "out"}
        ]
        for name in files:
            if not name.endswith((".cpp", ".hpp", ".h", ".cc")):
                continue
            rel = os.path.relpath(os.path.join(root, name), repo_root).replace(os.sep, "/")
            if any(fnmatch.fnmatch(rel, glob) for glob in RT_SCOPE_GLOBS):
                matches.append(rel)
    return sorted(matches)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="AURA real-time-safety audit")
    parser.add_argument("repo_root", nargs="?", default=".")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--list", action="store_true", help="list scanned files/functions")
    parser.add_argument(
        "--allow-warnings",
        action="store_true",
        help="exit 0 when only warnings were found (CI uses this for the log rule)",
    )
    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)
    stats = ScanStats()
    findings: list[Finding] = []
    for rel_path in collect_files(repo_root):
        findings.extend(scan_file(repo_root, rel_path, stats))

    errors = [f for f in findings if f.severity == "error"]
    warnings = [f for f in findings if f.severity == "warning"]

    if args.json:
        print(
            json.dumps(
                {
                    "files_scanned": stats.files_scanned,
                    "functions_scanned": stats.functions_scanned,
                    "errors": [f.__dict__ for f in errors],
                    "warnings": [f.__dict__ for f in warnings],
                    "allows": stats.allows,
                },
                indent=2,
            )
        )
    else:
        if args.list:
            for rel_path in collect_files(repo_root):
                print(f"scanned {rel_path}")
        for finding in findings:
            stream = sys.stderr if finding.severity == "error" else sys.stdout
            print(
                f"{finding.path}:{finding.line}: {finding.severity}: "
                f"[{finding.rule}] {finding.reason} in {finding.function}()",
                file=stream,
            )
            print(f"    {finding.text}", file=stream)
        for entry in stats.allows:
            print(f"allowed (annotated): {entry}")
        print(
            f"rt_audit: {stats.files_scanned} files, {stats.functions_scanned} audio-thread "
            f"functions scanned -> {len(errors)} error(s), {len(warnings)} warning(s)"
        )

    if errors:
        return 1
    if warnings and not args.allow_warnings:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
