#!/usr/bin/env python3
# ============================================================================
# AURA DAW - tools/include_audit.py
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# "It compiles with libstdc++, therefore MSVC will be fine" is not true, and the
# first Windows CI run proved it: `std::mutex` in a header that never included
# <mutex> built everywhere on Linux (libstdc++ hands it out through <memory> and
# friends) and failed on Windows with
#
#     error C2039: 'mutex': is not a member of 'std'
#
# This tool checks the opposite direction: a file must ask for what it uses.
# For every translation unit in the tree it resolves the *transitive* project
# include closure and then reports a standard-library symbol whose header is not
# in that closure. A second rule catches the same class of failure from the other
# direction: an #include directive written inside a namespace, which buries that
# header's declarations in the namespace (and made the WASAPI COM code
# uncompilable on Windows while building fine on Linux).
#
# Scope, deliberately narrow: only headers that the two standard libraries do not
# reliably hand out for free. <vector>, <string>, <cstring>, <cstdlib>, <cmath>
# and friends are pulled in by nearly everything in both implementations, so
# reporting them would drown the real findings in noise (and the fix would be a
# redundant include). The list below is the set that actually breaks: containers
# and utilities whose declaration lives in exactly one header.
#
# Usage:
#   python3 tools/include_audit.py [repo-root] [--list] [--json] [--allow-warnings]
#
# Escape hatch (rare, must carry a reason on the same line):
#   #include <vector>   // include-audit: allow <chrono> - <chrono> arrives via Time.hpp
#
# Exit status: 0 clean, 1 findings (or warnings unless --allow-warnings).
# ============================================================================

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# ---------------------------------------------------------------------------
# symbol pattern -> header that declares it
# ---------------------------------------------------------------------------
RULES: list[tuple[str, str]] = [
    # threading and synchronisation
    (r"\bstd::mutex\b", "<mutex>"),
    (r"\bstd::recursive_mutex\b", "<mutex>"),
    (r"\bstd::timed_mutex\b", "<mutex>"),
    (r"\bstd::lock_guard\b[A-Za-z_]*", "<mutex>"),
    (r"\bstd::unique_lock\b", "<mutex>"),
    (r"\bstd::scoped_lock\b", "<mutex>"),
    (r"\bstd::adopt_lock\b", "<mutex>"),
    (r"\bstd::condition_variable\b[A-Za-z_]*", "<condition_variable>"),
    (r"\bstd::thread\b", "<thread>"),
    (r"\bstd::jthread\b", "<thread>"),
    (r"\bstd::this_thread\b", "<thread>"),
    (r"\bstd::stop_token\b", "<stop_token>"),
    (r"\bstd::atomic\b", "<atomic>"),
    (r"\bstd::atomic_flag\b", "<atomic>"),
    (r"\bstd::memory_order\b", "<atomic>"),
    (r"\bstd::future\b", "<future>"),
    (r"\bstd::promise\b", "<future>"),
    (r"\bstd::packaged_task\b", "<future>"),
    (r"\bstd::async\b", "<future>"),
    # time and files
    (r"\bstd::chrono\b", "<chrono>"),
    (r"\bstd::filesystem\b", "<filesystem>"),
    # vocabulary types
    (r"\bstd::optional\b", "<optional>"),
    (r"\bstd::nullopt\b", "<optional>"),
    (r"\bstd::variant\b", "<variant>"),
    (r"\bstd::monostate\b", "<variant>"),
    (r"\bstd::get_if\b", "<variant>"),
    (r"\bstd::visit\b", "<variant>"),
    (r"\bstd::function\b", "<functional>"),
    (r"\bstd::bind\b", "<functional>"),
    (r"\bstd::hash\b", "<functional>"),
    (r"\bstd::string_view\b", "<string_view>"),
    (r"\bstd::span\b", "<span>"),
    (r"\bstd::tuple\b", "<tuple>"),
    (r"\bstd::make_tuple\b", "<tuple>"),
    (r"\bstd::apply\b", "<tuple>"),
    (r"\bstd::source_location\b", "<source_location>"),
    # containers whose header is the only way in
    (r"\bstd::unordered_map\b", "<unordered_map>"),
    (r"\bstd::unordered_set\b", "<unordered_set>"),
    (r"\bstd::unordered_multimap\b", "<unordered_map>"),
    (r"\bstd::map\b", "<map>"),
    (r"\bstd::multimap\b", "<map>"),
    (r"\bstd::set\b", "<set>"),
    (r"\bstd::multiset\b", "<set>"),
    (r"\bstd::deque\b", "<deque>"),
    (r"\bstd::list\b", "<list>"),
    (r"\bstd::forward_list\b", "<forward_list>"),
    (r"\bstd::queue\b", "<queue>"),
    (r"\bstd::stack\b", "<stack>"),
    (r"\bstd::priority_queue\b", "<queue>"),
    (r"\bstd::array\b", "<array>"),
    (r"\bstd::vector\b", "<vector>"),
    (r"\bstd::bitset\b", "<bitset>"),
    (r"\bstd::complex\b", "<complex>"),
    (r"\bstd::valarray\b", "<valarray>"),
    (r"\bstd::initializer_list\b", "<initializer_list>"),
    # algorithms and numerics
    (r"\bstd::clamp\b", "<algorithm>"),
    (r"\bstd::minmax\b[A-Za-z_]*", "<algorithm>"),
    (r"\bstd::stable_sort\b", "<algorithm>"),
    (r"\bstd::partial_sort\b", "<algorithm>"),
    (r"\bstd::nth_element\b", "<algorithm>"),
    (r"\bstd::sort\b", "<algorithm>"),
    (r"\bstd::shuffle\b", "<algorithm>"),
    (r"\bstd::transform\b", "<algorithm>"),
    (r"\bstd::remove_if\b", "<algorithm>"),
    (r"\bstd::remove_copy\b[A-Za-z_]*", "<algorithm>"),
    (r"\bstd::replace_if\b", "<algorithm>"),
    (r"\bstd::unique\b", "<algorithm>"),
    (r"\bstd::lower_bound\b", "<algorithm>"),
    (r"\bstd::upper_bound\b", "<algorithm>"),
    (r"\bstd::equal_range\b", "<algorithm>"),
    (r"\bstd::iota\b", "<numeric>"),
    (r"\bstd::accumulate\b", "<numeric>"),
    (r"\bstd::inner_product\b", "<numeric>"),
    (r"\bstd::gcd\b", "<numeric>"),
    (r"\bstd::midpoint\b", "<numeric>"),
    (r"\bstd::bit_cast\b", "<bit>"),
    (r"\bstd::popcount\b", "<bit>"),
    (r"\bstd::has_single_bit\b", "<bit>"),
    (r"\bstd::countl_zero\b", "<bit>"),
    (r"\bstd::countr_zero\b", "<bit>"),
    (r"\bstd::endian\b", "<bit>"),
    (r"\bstd::numbers\b", "<numbers>"),
    # memory and limits
    (r"\bstd::make_unique\b", "<memory>"),
    (r"\bstd::make_shared\b", "<memory>"),
    (r"\bstd::shared_ptr\b", "<memory>"),
    (r"\bstd::weak_ptr\b", "<memory>"),
    (r"\bstd::enable_shared_from_this\b", "<memory>"),
    (r"\bstd::unique_ptr\b", "<memory>"),
    (r"\bstd::align\b", "<memory>"),
    (r"\bstd::numeric_limits\b", "<limits>"),
    (r"\bstd::launder\b", "<new>"),
    (r"\bstd::from_chars\b", "<charconv>"),
    (r"\bstd::to_chars\b", "<charconv>"),
    # diagnostics and errors
    (r"\bstd::error_code\b", "<system_error>"),
    (r"\bstd::error_category\b", "<system_error>"),
    (r"\bstd::system_error\b", "<system_error>"),
    (r"\bstd::runtime_error\b", "<stdexcept>"),
    (r"\bstd::logic_error\b", "<stdexcept>"),
    (r"\bstd::invalid_argument\b", "<stdexcept>"),
    (r"\bstd::out_of_range\b", "<stdexcept>"),
    (r"\bstd::exception_ptr\b", "<exception>"),
    (r"\bstd::uncaught_exceptions\b", "<exception>"),
    # I/O
    (r"\bstd::ifstream\b", "<fstream>"),
    (r"\bstd::ofstream\b", "<fstream>"),
    (r"\bstd::fstream\b", "<fstream>"),
    (r"\bstd::ostringstream\b", "<sstream>"),
    (r"\bstd::istringstream\b", "<sstream>"),
    (r"\bstd::stringstream\b", "<sstream>"),
    (r"\bstd::setw\b", "<iomanip>"),
    (r"\bstd::setfill\b", "<iomanip>"),
    (r"\bstd::setprecision\b", "<iomanip>"),
    (r"\bstd::quoted\b", "<iomanip>"),
    (r"\bstd::regex\b", "<regex>"),
    (r"\bstd::random_device\b", "<random>"),
    (r"\bstd::mt19937\b", "<random>"),
    (r"\bstd::uniform_[a-z_]*distribution\b", "<random>"),
    (r"\bstd::locale\b", "<locale>"),
]

SOURCE_GLOBS = ("include/**/*.hpp", "src/**/*.cpp", "src/**/*.hpp",
                "tests/**/*.cpp", "tests/**/*.hpp", "bench/**/*.cpp",
                "app/**/*.cpp", "app/**/*.hpp")

SEARCH_ROOTS = ("include", "src", "tests", "bench", "app")

NAMESPACE_RE = re.compile(r"^\s*namespace\b")
ALLOW_RE = re.compile(r"include-audit:\s*allow\s+(<[^>]+>)\s*-\s*(\S.*)$")
INCLUDE_RE = re.compile(r'#\s*include\s+([<"][^">]+[">])')


def strip_comments(text: str) -> str:
    """Blank out comments so that commented-out code is never scanned.

    Escapes are read from the raw text (they live in a trailing comment); the
    symbol check runs on the stripped text.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def includes_inside_namespaces(text: str) -> list[int]:
    """Line numbers of #include directives that appear inside a namespace.

    Including a header inside `namespace aura::x` nests every declaration it makes
    in that namespace. The Windows/COM headers make this fatal rather than
    cosmetic: src/audioio/WasapiDevice.cpp used to include <mmdeviceapi.h> and
    <wrl/client.h> inside `namespace aura::audioio`, so MSVC reported

        error C2653: 'Microsoft': is not a class or namespace name
        error C2786: '<error>': invalid operand for __uuidof

    Brace counting is an approximation, but a wrong answer here can only come from
    a file that is already confusing.
    """
    code = strip_comments(text)
    code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
    code = re.sub(r"'(?:\\.|[^'\\])*'", "''", code)
    findings: list[int] = []
    depth = 0
    namespace_depths: list[int] = []
    for lineno, line in enumerate(code.splitlines(), 1):
        if NAMESPACE_RE.match(line) and "{" in line:
            namespace_depths.append(depth + line.count("{"))
        if INCLUDE_RE.search(line) and namespace_depths:
            findings.append(lineno)
        depth += line.count("{") - line.count("}")
        while namespace_depths and depth < namespace_depths[-1]:
            namespace_depths.pop()
    return findings


def includes_of(text: str) -> tuple[set[str], list[str]]:
    system: set[str] = set()
    project: list[str] = []
    for match in INCLUDE_RE.finditer(text):
        token = match.group(1)
        if token.startswith("<"):
            system.add(token)
        else:
            project.append(token[1:-1])
    return system, project


def resolve_project_include(include: str, from_file: str, root: pathlib.Path) -> str | None:
    for base in SEARCH_ROOTS:
        candidate = root / base / include
        if candidate.exists():
            return candidate.relative_to(root).as_posix()
    sibling = (pathlib.Path(from_file).parent / include)
    if (root / sibling).exists():
        return sibling.as_posix()
    return None


def audit(root: pathlib.Path, as_json: bool, show_list: bool) -> tuple[int, list[dict]]:
    headers: list[pathlib.Path] = []
    for pattern in ("include/**/*.hpp", "src/**/*.hpp", "tests/**/*.hpp",
                    "bench/**/*.hpp", "app/**/*.hpp"):
        headers.extend(sorted(root.glob(pattern)))

    graph: dict[str, tuple[set[str], list[str]]] = {}
    for header in headers:
        rel = header.relative_to(root).as_posix()
        text = header.read_text(encoding="utf-8", errors="replace")
        system, project = includes_of(strip_comments(text))
        resolved = [r for r in (resolve_project_include(i, rel, root) for i in project) if r]
        graph[rel] = (system, resolved)

    def closure(path: str) -> set[str]:
        seen: set[str] = set()
        system: set[str] = set()
        stack = [path]
        while stack:
            current = stack.pop()
            if current in seen:
                continue
            seen.add(current)
            headers_here, project_here = graph.get(current, (set(), []))
            system |= headers_here
            stack.extend(project_here)
        return system

    findings: list[dict] = []
    files: list[pathlib.Path] = []
    for pattern in SOURCE_GLOBS:
        files.extend(sorted(root.glob(pattern)))

    for path in files:
        rel = path.relative_to(root).as_posix()
        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)

        allowed: dict[str, str] = {}
        for line in raw.splitlines():
            match = ALLOW_RE.search(line)
            if match:
                allowed[match.group(1)] = match.group(2)

        system_here, project_here = includes_of(code)
        system_all = set(system_here)
        for include in project_here:
            resolved = resolve_project_include(include, rel, root)
            if resolved:
                system_all |= closure(resolved)

        seen_symbols: dict[str, str] = {}
        for pattern, header in RULES:
            if header in system_all or header in allowed:
                continue
            match = re.search(pattern, code)
            if match:
                seen_symbols.setdefault(header, match.group(0))

        for header, symbol in sorted(seen_symbols.items()):
            findings.append({
                "kind": "missing-header",
                "file": rel,
                "line": 0,
                "header": header,
                "symbol": symbol,
                "reason": "symbol used but its header is not in the include closure",
            })

        for lineno in includes_inside_namespaces(raw):
            findings.append({
                "kind": "include-in-namespace",
                "file": rel,
                "line": lineno,
                "header": "",
                "symbol": "",
                "reason": "include directive inside a namespace: the header's "
                          "declarations become members of that namespace",
            })

    if as_json:
        print(json.dumps({"files": len(files), "findings": findings}, indent=2))
    else:
        print(f"include_audit: {len(files)} files scanned -> {len(findings)} finding(s)")
        for finding in findings:
            if finding["kind"] == "missing-header":
                print(f"  error: {finding['file']}: uses {finding['symbol']} without "
                      f"{finding['header']} (MSVC will reject this; libstdc++ hides it)")
            else:
                print(f"  error: {finding['file']}:{finding['line']}: include inside a "
                      f"namespace (declare the header at global scope)")
        for path in files:
            raw = path.read_text(encoding="utf-8", errors="replace")
            for line in raw.splitlines():
                match = ALLOW_RE.search(line)
                if match:
                    rel = path.relative_to(root).as_posix()
                    if show_list:
                        print(f"  allowed (annotated): {rel} - {match.group(2)}")

    return (1 if findings else 0), findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check that a file asks for every standard header it uses.")
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--list", action="store_true", help="print annotated escapes as well")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    if not (root / "include" / "aura").is_dir():
        print(f"not an AURA checkout: {root}", file=sys.stderr)
        return 2

    status, _ = audit(root, args.json, args.list)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
