#!/usr/bin/env bash
# ============================================================================
# AURA DAW - tools/win_crosscheck.sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 The AURA DAW Project
#
# Compile every Windows-specific translation unit (and, with --headers, every
# public header on its own) with a MinGW-w64 cross compiler, so a Linux
# developer sees MSVC breakage before burning a Windows CI run.
#
# Why this exists: Windows-only code is invisible to a GCC/Linux build, and three
# Windows CI rounds were lost to problems a cross compiler finds in a second -
#   * src/audioio/WasapiDevice.cpp included <mmdeviceapi.h> and <wrl/client.h>
#     *inside* `namespace aura::audioio` -> error C2653: 'Microsoft': is not a
#     class or namespace name, and error C2786: invalid operand for __uuidof;
#   * `using ComPtr = Microsoft::WRL::ComPtr;` aliased a class template without
#     arguments (not valid C++ anywhere - it had simply never been compiled);
#   * `EnumAudioEndpoints(eRender | eCapture, ...)` passed an int where EDataFlow
#     was expected - and, since eRender is 0 and eCapture is 1, would have
#     enumerated capture endpoints only.
#
# MinGW is *not* a supported AURA build target (MSVC is). This is a
# syntax-and-types check for development and CI convenience, not a link test.
#
# Usage:
#   tools/win_crosscheck.sh [--require-toolchain] [--headers] [-j N]
#
#   --require-toolchain  exit 2 when no cross compiler is installed; without it
#                        the script reports and exits 0, so it is safe to wire
#                        into setups that do not have MinGW.
#   --headers            also compile each public header standalone (slower,
#                        catches headers that only build because of include
#                        order).
#
# Ubuntu/Debian install:
#   sudo apt-get install -y g++-mingw-w64-x86-64
#
# Exit status: 0 clean (or toolchain missing without --require-toolchain),
#              1 findings, 2 toolchain missing where it was required.
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="${BASH_SOURCE[0]##*/}"

REQUIRE=0
HEADERS=0
JOBS="$( (nproc 2>/dev/null || echo 2) )"

while [ $# -gt 0 ]; do
    case "$1" in
        --require-toolchain) REQUIRE=1; shift ;;
        --headers)           HEADERS=1; shift ;;
        -j)                  JOBS="$2"; shift 2 ;;
        -h|--help)           sed -n '2,40p' "$ROOT/tools/$SELF"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

find_cxx() {
    if [ -n "${AURA_MINGW_CXX:-}" ] && command -v "${AURA_MINGW_CXX}" >/dev/null 2>&1; then
        echo "${AURA_MINGW_CXX}"; return 0
    fi
    for candidate in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ \
                     x86_64-w64-mingw32-g++-win32; do
        if command -v "$candidate" >/dev/null 2>&1; then echo "$candidate"; return 0; fi
    done
    return 1
}

CXX="$(find_cxx || true)"
if [ -z "$CXX" ]; then
    echo "win_crosscheck: no Windows cross compiler found."
    echo "  install one with:  sudo apt-get install -y g++-mingw-w64-x86-64"
    echo "  or point AURA_MINGW_CXX at one."
    if [ "$REQUIRE" = "1" ]; then exit 2; fi
    echo "  skipping (the MSVC CI job still covers this)."
    exit 0
fi

LOG="$(mktemp)"
FAILED_LOG="$(mktemp)"
HEADER_DIR=""
trap '[ -n "$HEADER_DIR" ] && rm -rf "$HEADER_DIR"; rm -f "$LOG" "$FAILED_LOG"' EXIT

# The defines mirror CMakeLists.txt for MSVC; the warning set mirrors the Linux
# flags, so a cross-checked file is held to the same standard as a native build.
check_file() {
    local file="$1" label="$2"
    if ! "$CXX" -std=c++20 -I "$ROOT/include" \
            -D_WIN32 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE \
            -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0A00 \
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wunused-parameter -Wno-missing-field-initializers \
            -fsyntax-only "$file" >>"$LOG" 2>&1; then
        echo "FAILED: $label" >>"$FAILED_LOG"
    fi
}
export -f check_file
export CXX ROOT LOG FAILED_LOG

echo "win_crosscheck: compiler $($CXX --version | head -n1)"

# ---------------------------------------------------------------------------
# 1. every translation unit (src/, app/ once it exists)
# ---------------------------------------------------------------------------
# src/plugin/vst3 needs the Steinberg SDK, which is fetched only when
# AURA_ENABLE_VST3=ON: cross-checking it here would fail on a header that is not in
# the tree by design. The Linux "VST 3 hosting" CI job compiles and runs those files
# for real, which is a better check than a syntax pass anyway.
mapfile -t SOURCES < <(cd "$ROOT" && find src app bench -name '*.cpp' 2>/dev/null \
    | grep -v '^src/plugin/vst3/' | sort)
if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "win_crosscheck: no sources found - is this an AURA checkout?" >&2
    exit 2
fi

printf '%s\0' "${SOURCES[@]}" \
    | xargs -0 -P "$JOBS" -I{} bash -c 'cd "$ROOT" && check_file {} {}'

# ---------------------------------------------------------------------------
# 2. every public header standalone (opt-in: catches include-order luck)
# ---------------------------------------------------------------------------
HEADER_COUNT=0
if [ "$HEADERS" = "1" ]; then
    HEADER_DIR="$(mktemp -d)"
    mapfile -t HEADER_FILES < <(cd "$ROOT/include" && find aura -name '*.hpp' | sort)
    for rel in "${HEADER_FILES[@]}"; do
        probe="$HEADER_DIR/probe_$HEADER_COUNT.cpp"
        # Included through -I include/, i.e. exactly as the tree includes it.
        printf '#include "%s"\n' "$rel" >"$probe"
        HEADER_COUNT=$((HEADER_COUNT + 1))
        check_file "$probe" "include/$rel (standalone header)"
    done
fi

# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------
FAILED="$(grep -c '^FAILED' "$FAILED_LOG" || true)"
FAILED_TU="$(grep -c '^FAILED: \(src\|app\)/' "$FAILED_LOG" || true)"
ERRORS="$(grep -c 'error:' "$LOG" || true)"

if [ "$FAILED" = "0" ]; then
    echo "win_crosscheck: ${#SOURCES[@]} translation units + ${HEADER_COUNT} standalone headers -> 0 errors"
    exit 0
fi

echo "win_crosscheck: $FAILED check(s) failed - $FAILED_TU translation unit(s)," \
     "$((FAILED - FAILED_TU)) standalone header(s); $ERRORS compiler errors"
cat "$FAILED_LOG"
echo "--- first errors ---"
grep -E 'error:' "$LOG" | head -n 40
exit 1
