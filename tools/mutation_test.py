"""
mutation_test.py — small, targeted mutation-testing driver for
safety-critical C++ in this project.

Why this exists
---------------
docs/08-testing-strategy.md Tier 4 calls for mutation testing on
st::flash before it can clear the Phase 4 release gate. The full
treatment (a real LLVM-based tool like mull-runner running over
every relevant operator in every safety-critical file) is heavyweight
and slow. This driver is the cheap, scriptable starting point: a few
operator mutations applied one at a time to a focused source-line
range, with the test suite re-run for each mutant and a survival
count reported at the end.

A "surviving mutant" — a code change that the test suite did not
catch — is a hint that test coverage for that line is shallow.
Hardening the suite to kill surviving mutants is the long-term work
this tool scaffolds.

What the driver does
--------------------
For each (mutation, target line) pair in a small library of supported
mutations:
  1. Read the target file fresh from disk.
  2. Apply exactly one mutation to one line.
  3. Write the mutated file in place.
  4. cmake --build the affected target.
  5. Run a subset of the test suite (filter by Catch2 tag).
  6. Record: SURVIVED (tests passed — bad), KILLED (tests failed —
     good), BUILD_FAIL (mutation broke the build — uninteresting),
     TIMEOUT (build/tests didn't complete).
  7. Restore the original file before moving to the next mutant.

Mutations supported:
  - swap_eq   :  `==` <-> `!=`
  - swap_lt   :  `<`  <-> `<=`
  - swap_gt   :  `>`  <-> `>=`
  - swap_bool :  `true` <-> `false`

Each mutation is applied at most once per run — single mutations
per cycle, classic mutation-testing convention.

This is intentionally a small tool. A real CI integration would
parallelize, batch, and produce a structured report. Run it locally
to spot-check one function at a time.

Usage
-----
    py tools/mutation_test.py \\
        --file src/flash/src/flash.cpp \\
        --line-start 515 --line-end 544 \\
        --target st_unit_tests \\
        --tag-filter "[flash]"

Exit code: 0 if no mutants survived; 1 otherwise. CI hint: treat
non-zero as a coverage gap report, not a hard fail (until the suite
is hardened to 0 surviving mutants).
"""
from __future__ import annotations

import argparse
import dataclasses
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
# Default build dir matches the developer's local checkout (MinGW).
# CI runs on Linux preset and overrides via --build-dir. Anyone
# whose local checkout uses a different preset can pass --build-dir
# rather than editing this constant.
BUILD_DIR = ROOT / "build" / "win-mingw"


@dataclasses.dataclass
class Mutation:
    name:      str
    pattern:   str            # regex with one capture group = match to swap
    swap_to:   Callable[[str], str]   # given the matched text, produce the swap


# Library of supported mutations. Keep this list short; each entry
# above adds a multiplicative factor to total run time.
MUTATIONS: list[Mutation] = [
    Mutation(
        name      = "swap_eq",
        pattern   = r"(==|!=)",
        swap_to   = lambda m: "!=" if m == "==" else "==",
    ),
    Mutation(
        name      = "swap_lt",
        pattern   = r"([^<])(<=?)([^<=])",  # avoid matching << or template<>
        # Use a callable that operates on the captured operator group only.
        swap_to   = lambda m: "<=" if m == "<" else "<",
    ),
    Mutation(
        name      = "swap_gt",
        pattern   = r"([^>])(>=?)([^>=])",
        swap_to   = lambda m: ">=" if m == ">" else ">",
    ),
    Mutation(
        name      = "swap_bool",
        pattern   = r"\b(true|false)\b",
        swap_to   = lambda m: "false" if m == "true" else "true",
    ),
]


@dataclasses.dataclass
class MutantResult:
    mutation:  str
    line:      int
    original:  str
    mutated:   str
    verdict:   str   # "KILLED" | "SURVIVED" | "BUILD_FAIL" | "TIMEOUT"


def find_candidates(lines: list[str], line_start: int, line_end: int,
                    mut: Mutation) -> list[tuple[int, int, int, str]]:
    """For one mutation, return (line_no, col_start, col_end, matched_text)
    tuples for every match in the given line range."""
    out: list[tuple[int, int, int, str]] = []
    for ln in range(line_start, line_end + 1):
        if ln > len(lines):
            break
        text = lines[ln - 1]
        # Skip lines that look like comments — mutating inside `// ...` is
        # a waste. Crude check; doesn't catch multi-line comments.
        stripped = text.lstrip()
        if stripped.startswith("//"):
            continue
        if mut.name in ("swap_lt", "swap_gt"):
            # These mutations use a 3-group pattern with the operator in
            # group 2 to avoid matching << / >> / template<>.
            for m in re.finditer(mut.pattern, text):
                op_start = m.start(2)
                op_end   = m.end(2)
                op       = m.group(2)
                out.append((ln, op_start, op_end, op))
        else:
            for m in re.finditer(mut.pattern, text):
                out.append((ln, m.start(1), m.end(1), m.group(1)))
    return out


def apply_mutation(lines: list[str], ln: int, col_start: int,
                   col_end: int, swap: str) -> list[str]:
    out = list(lines)
    original = out[ln - 1]
    out[ln - 1] = original[:col_start] + swap + original[col_end:]
    return out


def run_tests(target: str, tag_filter: str | None,
              build_timeout_s: int, test_timeout_s: int,
              build_dir: Path,
              verbose: bool = False) -> str:
    """Returns 'KILLED', 'SURVIVED', 'BUILD_FAIL', or 'TIMEOUT'."""
    try:
        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", target, "-j"],
            capture_output=True, text=True, timeout=build_timeout_s)
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    if build.returncode != 0:
        if verbose:
            # Surface the last 20 lines of compiler output so a
            # BUILD_FAIL cascade can be diagnosed without re-running by
            # hand. Long compiler dumps would drown the per-mutant
            # status line; the cap is a deliberate trade-off.
            tail = "\n".join((build.stderr or build.stdout).splitlines()[-20:])
            print("\n----- build stderr (last 20 lines) -----")
            print(tail)
            print("----- end build stderr -----")
        return "BUILD_FAIL"
    test_bin = build_dir / "bin" / f"{target}.exe"
    if not test_bin.exists():
        test_bin = build_dir / "bin" / target
    cmd = [str(test_bin)]
    if tag_filter:
        cmd.append(tag_filter)
    try:
        tests = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=test_timeout_s)
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    return "KILLED" if tests.returncode != 0 else "SURVIVED"


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--file",       required=True, type=Path,
                    help="Source file to mutate (relative to repo root).")
    p.add_argument("--line-start", required=True, type=int,
                    help="First line to consider (1-indexed).")
    p.add_argument("--line-end",   required=True, type=int,
                    help="Last line to consider (1-indexed, inclusive).")
    p.add_argument("--target",     default="st_unit_tests",
                    help="CMake target to rebuild + run.")
    p.add_argument("--tag-filter", default=None,
                    help="Catch2 tag filter (e.g. '[flash]').")
    p.add_argument("--max-mutants",type=int, default=20,
                    help="Stop after this many mutants (cost cap).")
    p.add_argument("--build-timeout", type=int, default=120,
                    help="Per-mutant build timeout (seconds).")
    p.add_argument("--test-timeout",  type=int, default=180,
                    help="Per-mutant test timeout (seconds).")
    p.add_argument("--mutations", default=",".join(m.name for m in MUTATIONS),
                    help="Comma-separated mutation names (default: all).")
    p.add_argument("--verbose", action="store_true",
                    help="On BUILD_FAIL, print the last 20 lines of compiler "
                         "stderr so cascades can be diagnosed in-place.")
    p.add_argument("--build-dir", type=Path, default=BUILD_DIR,
                    help="CMake build directory. Defaults to the developer's "
                         "local win-mingw checkout; CI passes its preset's "
                         "directory (e.g. 'build/' on the Linux runner).")
    args = p.parse_args()

    target_path = (ROOT / args.file).resolve()
    if not target_path.is_file():
        print(f"mutation: file not found: {target_path}", file=sys.stderr)
        return 2

    chosen_names = {n.strip() for n in args.mutations.split(",")}
    chosen = [m for m in MUTATIONS if m.name in chosen_names]
    if not chosen:
        print(f"mutation: no known mutations in {args.mutations}",
              file=sys.stderr)
        return 2

    original_text = target_path.read_text(encoding="utf-8")
    original_lines = original_text.splitlines(keepends=True)

    candidates: list[tuple[Mutation, int, int, int, str]] = []
    for mut in chosen:
        for ln, c0, c1, op in find_candidates(
                original_lines, args.line_start, args.line_end, mut):
            candidates.append((mut, ln, c0, c1, op))

    if not candidates:
        print(f"mutation: no candidates in lines {args.line_start}-"
              f"{args.line_end}", file=sys.stderr)
        return 0

    if len(candidates) > args.max_mutants:
        print(f"mutation: capping {len(candidates)} candidates at "
              f"{args.max_mutants} (use --max-mutants to raise)")
        candidates = candidates[:args.max_mutants]

    results: list[MutantResult] = []
    try:
        for i, (mut, ln, c0, c1, op) in enumerate(candidates, 1):
            swap = mut.swap_to(op)
            print(f"[{i}/{len(candidates)}] {mut.name}: line {ln}, "
                  f"'{op}' -> '{swap}'", end="", flush=True)
            mutated = apply_mutation(original_lines, ln, c0, c1, swap)
            target_path.write_text("".join(mutated), encoding="utf-8")
            verdict = run_tests(args.target, args.tag_filter,
                                 args.build_timeout, args.test_timeout,
                                 build_dir=args.build_dir,
                                 verbose=args.verbose)
            print(f"  -> {verdict}")
            results.append(MutantResult(
                mutation=mut.name, line=ln, original=op,
                mutated=swap, verdict=verdict))
    finally:
        # Always restore the original file.
        target_path.write_text(original_text, encoding="utf-8")

    print()
    print("=" * 60)
    print(f"Mutation results for {args.file}")
    print(f"Lines {args.line_start}-{args.line_end}")
    print("=" * 60)
    killed     = sum(1 for r in results if r.verdict == "KILLED")
    survived   = sum(1 for r in results if r.verdict == "SURVIVED")
    build_fail = sum(1 for r in results if r.verdict == "BUILD_FAIL")
    timeout    = sum(1 for r in results if r.verdict == "TIMEOUT")
    total = len(results)
    print(f"Total mutants run: {total}")
    print(f"  KILLED      (caught by tests):       {killed}")
    print(f"  SURVIVED    (NOT caught by tests):   {survived}")
    print(f"  BUILD_FAIL  (didn't compile):        {build_fail}")
    print(f"  TIMEOUT     (build or tests):        {timeout}")
    if total > 0:
        run_count = killed + survived
        if run_count > 0:
            print(f"Mutation score: {100.0 * killed / run_count:.1f}% "
                  f"({killed}/{run_count} compiling mutants killed)")
    if survived > 0:
        print()
        print("Surviving mutants (TESTS DID NOT CATCH THESE):")
        for r in results:
            if r.verdict == "SURVIVED":
                print(f"  line {r.line:4d}  {r.mutation:10s}  "
                      f"'{r.original}' -> '{r.mutated}'")
    return 0 if survived == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
