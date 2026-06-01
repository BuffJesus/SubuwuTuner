#!/usr/bin/env bash
# Performance gate — binary sizes.
#
# Fail the CI build if either the CLI or GUI binary exceeds its
# threshold. Thresholds carry headroom over current observed sizes
# (~6 MB CLI, ~15 MB GUI as of 2026-06) so this is a regression gate,
# not a fit-tightness one — the goal is to flag a step-change (10x
# growth from accidentally bundling fixtures, statically linking
# something huge, etc.), not to nickel-and-dime normal feature growth.
#
# Per docs/05 §1: SubuwuTuner's "JVM-based tuning tools pay real
# cost in cold-start / idle RAM" thesis lives on the binary-size
# axis too. A 60 MB installer is the user-facing budget; the per-
# binary thresholds here keep that achievable.
#
# Usage: bash scripts/check-binary-sizes.sh <bin-dir>
#
# Exit codes:
#   0  all checks passed (or binary not built)
#   1  at least one threshold exceeded
#   2  bad usage

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <bin-dir>" >&2
    exit 2
fi

BIN_DIR="$1"

if [ ! -d "$BIN_DIR" ]; then
    echo "error: bin-dir does not exist: $BIN_DIR" >&2
    exit 2
fi

# Thresholds in bytes. Update with a commit message that explains the
# new floor — these should only ever grow with a documented reason.
CLI_MAX=$((20 * 1024 * 1024))   # 20 MB
GUI_MAX=$((40 * 1024 * 1024))   # 40 MB

# stat -c is GNU; macOS uses stat -f. Probe once.
if stat -c '%s' "$0" >/dev/null 2>&1; then
    file_size() { stat -c '%s' "$1"; }
else
    file_size() { stat -f '%z' "$1"; }
fi

check() {
    local label="$1" path="$2" max="$3"
    if [ ! -f "$path" ]; then
        echo "[skip] $label not built at $path"
        return 0
    fi
    local size
    size=$(file_size "$path")
    local size_mb=$((size / 1024 / 1024))
    local max_mb=$((max / 1024 / 1024))
    if [ "$size" -gt "$max" ]; then
        echo "[FAIL] $label: $size bytes (~${size_mb} MB) exceeds ${max_mb} MB threshold"
        return 1
    fi
    echo "[ ok ] $label: $size bytes (~${size_mb} MB) <= ${max_mb} MB threshold"
    return 0
}

fail=0

# Probe for the CLI and GUI binaries under both name conventions
# (Unix has no .exe suffix, Windows does).
CLI=""
for candidate in "$BIN_DIR/subuwutuner-cli.exe" "$BIN_DIR/subuwutuner-cli"; do
    if [ -f "$candidate" ]; then
        CLI="$candidate"
        break
    fi
done

GUI=""
for candidate in "$BIN_DIR/subuwutuner-gui.exe" "$BIN_DIR/subuwutuner-gui"; do
    if [ -f "$candidate" ]; then
        GUI="$candidate"
        break
    fi
done

if [ -n "$CLI" ]; then
    check "CLI" "$CLI" "$CLI_MAX" || fail=1
else
    echo "[skip] CLI not found in $BIN_DIR (checked .exe + bare)"
fi

if [ -n "$GUI" ]; then
    check "GUI" "$GUI" "$GUI_MAX" || fail=1
else
    echo "[skip] GUI not found in $BIN_DIR (checked .exe + bare)"
fi

if [ "$fail" -eq 0 ]; then
    echo
    echo "Binary-size gate: PASS"
else
    echo
    echo "Binary-size gate: FAIL — at least one binary grew past its threshold." >&2
    echo "If this is intentional growth, update CLI_MAX / GUI_MAX in" >&2
    echo "scripts/check-binary-sizes.sh with a commit explaining the new floor." >&2
fi

exit "$fail"
