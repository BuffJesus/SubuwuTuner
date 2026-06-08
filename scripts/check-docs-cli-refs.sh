#!/usr/bin/env bash
# Lint docs/ for subuwutuner-cli subcommand references that don't exist in
# the dispatch registry. Runs in CI to catch the drift class fixed by
# commit a264b54 (read-rom → rom-pull, flash --resume → flash-resume,
# extended-did-datalog-preset → did-datalog-preset).
#
# The check is intentionally narrow: it extracts the FIRST token after
# `subuwutuner-cli[.exe]` in any code-block or inline `subuwutuner-cli …`
# pattern, then verifies that token appears in a `cmd == "…"` dispatch
# branch in src/cli/main.cpp. It does NOT validate flag names, argument
# shapes, or value enums — those would catch more bugs but also produce
# false positives on speculative / forward-looking design references.
#
# Exit codes: 0 = clean, 1 = stale references found, 2 = scaffolding error.
#
# Run locally: bash scripts/check-docs-cli-refs.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS_DIR="${REPO_ROOT}/docs"
MAIN_CPP="${REPO_ROOT}/src/cli/main.cpp"

if [[ ! -f "${MAIN_CPP}" ]]; then
    echo "check-docs-cli-refs: ${MAIN_CPP} not found — wrong CWD?" >&2
    exit 2
fi
if [[ ! -d "${DOCS_DIR}" ]]; then
    echo "check-docs-cli-refs: ${DOCS_DIR} not found" >&2
    exit 2
fi

# 1. Build the valid subcommand set from main.cpp's dispatch sites.
mapfile -t VALID_CMDS < <(
    grep -nE 'cmd == "[a-z][a-z0-9-]+"' "${MAIN_CPP}" \
        | sed -E 's/.*cmd == "([^"]+)".*/\1/' \
        | sort -u
)
if [[ "${#VALID_CMDS[@]}" -eq 0 ]]; then
    echo "check-docs-cli-refs: failed to extract any subcommands from main.cpp" >&2
    exit 2
fi

# 2. Collect every subcommand token referenced in docs/.
#    Pattern: `subuwutuner-cli[.exe] <token>` where token is [a-z][a-z0-9-]+.
#    Standard-help / standard-flag tokens are NOT subcommands, so filter
#    --foo and bare flag prefixes early.
mapfile -t DOC_REFS < <(
    grep -rhoE 'subuwutuner-cli(\.exe)?[[:space:]]+[a-z][a-z0-9-]+' "${DOCS_DIR}" \
        | sed -E 's/subuwutuner-cli(\.exe)?[[:space:]]+//' \
        | sort -u
)

# 3. Diff. Allowlist common doc-side tokens that aren't subcommands but
#    will show up after the CLI name in legitimate contexts.
ALLOWLIST=(
    "--def"
    "--help"
    "--version"
    "diagnose-drift"            # v2.0 forward-looking; docs/20 design
)

is_allowed() {
    local needle="$1"
    for a in "${ALLOWLIST[@]}"; do
        [[ "${a}" == "${needle}" ]] && return 0
    done
    return 1
}

is_valid_cmd() {
    local needle="$1"
    for c in "${VALID_CMDS[@]}"; do
        [[ "${c}" == "${needle}" ]] && return 0
    done
    return 1
}

STALE=()
for ref in "${DOC_REFS[@]}"; do
    is_allowed "${ref}" && continue
    is_valid_cmd "${ref}" && continue
    STALE+=("${ref}")
done

if [[ "${#STALE[@]}" -gt 0 ]]; then
    echo "check-docs-cli-refs: found ${#STALE[@]} stale CLI reference(s) in docs/:" >&2
    for s in "${STALE[@]}"; do
        echo "  - ${s}" >&2
        grep -rn "subuwutuner-cli\(.exe\)\?[[:space:]]\+${s}\b" "${DOCS_DIR}" \
            | sed 's/^/      /' >&2 || true
    done
    echo "" >&2
    echo "  If this is a real subcommand rename, update the docs." >&2
    echo "  If this is a forward-looking design reference, add it to" >&2
    echo "  ALLOWLIST in this script." >&2
    exit 1
fi

echo "check-docs-cli-refs: clean (${#DOC_REFS[@]} doc refs vs ${#VALID_CMDS[@]} registered subcommands)"
exit 0
