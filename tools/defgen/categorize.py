#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
categorize — infer [[table]].category from id prefix patterns.

The VA/VB-era packs we ship were defgen'd from sparse forum-sourced
RR XMLs that didn't include category tags. Every table loads as
`category = ""`, which makes the GUI's category sidebar empty and
the rom-info histogram say "(uncategorized): N tables" for the
whole pack.

The table ids themselves are richly descriptive — `airflow_turbo_
wastegate_wastegate_duty_maximum`, `fuel_open_loop_avcs_disabled_
target_base_tgv_open`. The first 1-3 underscore-separated segments
encode the subsystem. Mapping prefix → category gives us a
useful sidebar tree without touching the XMLs or the loader.

This tool is read-mostly: it only rewrites the `category` field
inside [[table]] blocks where the existing category is empty.
Non-empty categories are preserved (use --force to override).
Tables with no matching rule are listed as "unmatched" — the user
can extend _PREFIX_RULES with more patterns and re-run.

Usage:
    # One pack, preview only
    python tools/defgen/categorize.py --pack definitions/impreza/lf79103p.toml --dry-run

    # One pack, apply
    python tools/defgen/categorize.py --pack definitions/impreza/lf79103p.toml

    # All VA/VB packs in impreza/
    python tools/defgen/categorize.py --glob 'definitions/impreza/lf*.toml'

The rules below are tuned for the 2015+ VA/VB FA-DIT naming
convention. They DON'T overlap with the older EJ-era naming
(those packs have explicit categories from their XMLs and are
skipped by the empty-category guard).
"""

from __future__ import annotations

import argparse
import glob as globlib
import re
import sys
from pathlib import Path

# Rule format: (prefix_segments_tuple, category_string).
# Matched against the table id by splitting on `_` and comparing
# the leading segments. Order matters — first match wins. Longer
# (more specific) prefixes go first so e.g. `airflow_turbo_wastegate_*`
# matches before the more general `airflow_turbo_*`.
_PREFIX_RULES: list[tuple[tuple[str, ...], str]] = [
    # --- Boost / Airflow ---
    (("airflow", "turbo", "wastegate"),   "boost control - wastegate"),
    (("airflow", "turbo", "boost"),       "boost control - target"),
    (("airflow", "turbo", "pi"),          "boost control - turbo dynamics"),
    (("airflow", "turbo"),                "boost control - turbo"),
    (("airflow", "manifold"),             "airflow - manifold (MAF/MAP)"),
    (("airflow",),                        "airflow"),

    # --- Fueling ---
    (("fuel", "open", "loop"),            "fueling - open loop"),
    (("fuel", "closed", "loop"),          "fueling - closed loop"),
    (("fuel", "cl"),                      "fueling - closed loop"),
    (("fuel", "ol"),                      "fueling - open loop"),
    (("fuel", "cranking"),                "fueling - cranking"),
    (("fuel", "tip"),                     "fueling - tip-in enrichment"),
    (("fuel", "warmup"),                  "fueling - warm-up enrichment"),
    (("fuel", "injectors"),               "fueling - injectors"),
    (("fuel", "injector"),                "fueling - injectors"),
    (("fuel", "timing"),                  "fueling - injection timing"),
    (("fuel", "pump"),                    "fueling - pump"),
    (("fuel", "rail"),                    "fueling - rail pressure"),
    (("fuel", "trim"),                    "fueling - trim (LTFT/STFT)"),
    (("fuel",),                           "fueling"),

    # --- Ignition ---
    (("ignition", "knock"),               "ignition timing - knock control"),
    (("ignition", "compensation"),        "ignition timing - compensation"),
    (("ignition", "main"),                "ignition timing - main"),
    (("ignition", "advance"),             "ignition timing - advance"),
    (("ignition", "dwell"),               "ignition - dwell"),
    (("ignition",),                       "ignition timing"),

    # --- AVCS ---
    (("avcs", "intake"),                  "avcs - intake"),
    (("avcs", "exhaust"),                 "avcs - exhaust"),
    (("avcs",),                           "avcs"),

    # --- Throttle / DBW ---
    (("throttle", "requested"),           "drive-by-wire throttle - request"),
    (("throttle", "target"),              "drive-by-wire throttle - target"),
    (("throttle", "pedal"),               "drive-by-wire throttle - pedal"),
    (("throttle",),                       "drive-by-wire throttle"),

    # --- Engine systems ---
    (("engine", "idle"),                  "idle"),
    (("engine", "radiator"),              "cooling - radiator"),
    (("engine", "coolant"),               "cooling - coolant"),
    (("engine", "oil"),                   "lubrication - oil"),
    (("engine", "speed"),                 "engine - speed/RPM"),
    (("engine",),                         "engine systems"),

    # --- Transmission / Drivetrain ---
    (("transmission", "gear"),            "transmission - gear"),
    (("transmission", "torque"),          "transmission - torque"),
    (("transmission",),                   "transmission"),
    (("drivetrain", "gear"),              "transmission - gear ratios"),
    (("drivetrain", "torque"),            "transmission - torque"),
    (("drivetrain", "shift"),             "transmission - shift"),
    (("drivetrain",),                     "transmission"),

    # --- Emissions / Cat ---
    (("catalyst",),                       "emissions - catalyst"),
    (("evap",),                           "emissions - evap"),
    (("egr",),                            "emissions - egr"),
    (("o2",),                             "emissions - o2 sensor"),
    (("oxygen",),                         "emissions - o2 sensor"),

    # --- Diagnostics ---
    (("diagnostic",),                     "diagnostic trouble codes"),
    (("dtc",),                            "diagnostic trouble codes"),
    (("dtcs",),                           "diagnostic trouble codes"),

    # --- Idle (VB packs split this out from `engine_idle_*`) ---
    (("idle",),                           "idle"),

    # --- Knock (VB packs use bare `knock_*`) ---
    (("knock",),                          "ignition timing - knock control"),

    # --- Misc VB stragglers ---
    (("dynamics",),                       "vehicle dynamics"),
    (("gauge",),                          "gauges"),
    (("ecu",),                            "ecu - meta"),

    # --- Sensors ---
    (("sensors", "oil"),                  "sensors - temperature"),
    (("sensors", "coolant"),              "sensors - temperature"),
    (("sensors", "intake"),               "sensors - temperature"),
    (("sensors", "manifold", "absolute"), "sensors - MAP"),
    (("sensors", "manifold"),             "sensors - temperature"),
    (("sensors", "mass"),                 "sensors - MAF"),
    (("sensors",),                        "sensors"),
    (("sensor",),                         "sensors"),
    (("temp",),                           "sensors - temperature"),
    (("temperature",),                    "sensors - temperature"),

    # --- Limits / Safety ---
    (("limit",),                          "limits"),
    (("limiter",),                        "limits"),
    (("rev",),                            "limits - rev"),
    (("speed",),                          "limits - speed"),

    # --- Launch / Boost ---
    (("launch",),                         "launch control"),
    (("boost",),                          "boost control"),
]

# DTC code pattern — table ids matching this are emissions-OBD
# trouble codes regardless of their leading segment.
_DTC_PATTERN = re.compile(r"^p[0-9]{4}_", re.IGNORECASE)


def infer_category(table_id: str) -> str | None:
    """Return the inferred category for `table_id`, or None if no rule
    matches.

    Tries DTC pattern first (P0123_..., P14AF_..., etc.), then walks
    _PREFIX_RULES top-to-bottom for the longest-prefix match.
    """
    if _DTC_PATTERN.match(table_id):
        return "diagnostic trouble codes"

    parts = table_id.lower().split("_")
    for prefix, category in _PREFIX_RULES:
        if len(parts) < len(prefix):
            continue
        if tuple(parts[: len(prefix)]) == prefix:
            return category
    return None


def categorize_pack_text(text: str, *, force: bool = False
                         ) -> tuple[str, dict[str, int]]:
    """Rewrite the pack `text` so [[table]] blocks get inferred
    categories. Returns (new_text, stats) where stats maps action
    name to count: 'inferred', 'kept', 'unmatched'.

    Only [[table]] blocks are touched; [[axis]] / [[scaling]] /
    [pack] are left alone. Within a [[table]] block, only the
    `category = "..."` line is modified.

    When `force=False`, non-empty existing categories are preserved
    (only blanks get filled). When `force=True`, every table's
    category is recomputed and overwritten.
    """
    stats = {"inferred": 0, "kept": 0, "unmatched": 0}
    lines = text.splitlines(keepends=True)
    out: list[str] = []

    in_table = False
    current_id: str | None = None
    block_start_idx: int = -1
    # Cached block lines so we can fix up the category line after seeing id
    pending_block: list[tuple[int, str]] = []  # (line_idx_in_out, line)
    cat_line_idx: int | None = None
    existing_cat: str = ""

    def flush_block():
        """Apply the inferred category to the held block."""
        nonlocal cat_line_idx, existing_cat, current_id
        if current_id is None:
            return
        inferred = infer_category(current_id)
        # Decide whether to write
        write_category = inferred
        if existing_cat and not force:
            stats["kept"] += 1
            return
        if inferred is None:
            stats["unmatched"] += 1
            return
        # Replace the category line (or insert one if missing)
        if cat_line_idx is None:
            # Need to insert. Place it right after the id line.
            # Find the id line in pending_block.
            for i, (out_idx, line) in enumerate(pending_block):
                if re.match(r"^\s*id\s*=", line):
                    new_line = f'category               = "{inferred}"\n'
                    out.insert(out_idx + 1, new_line)
                    # Adjust subsequent indices
                    for j in range(i + 1, len(pending_block)):
                        pending_block[j] = (pending_block[j][0] + 1,
                                            pending_block[j][1])
                    stats["inferred"] += 1
                    return
            return  # no id line found, skip
        # Replace existing line, preserving the prefix spacing
        old_line = out[cat_line_idx]
        m = re.match(r"^(\s*category\s*=\s*)", old_line)
        prefix = m.group(1) if m else "category = "
        # Preserve trailing comment and EOL
        body = old_line[len(prefix):]
        comment_idx = body.find("#")
        if comment_idx >= 0:
            trailing = body[comment_idx:].rstrip("\r\n")
            eol = body[len(body.rstrip("\r\n")):]
        else:
            stripped = body.rstrip("\r\n")
            eol = body[len(stripped):]
            trailing = ""
        new_line = f'{prefix}"{inferred}"'
        if trailing:
            new_line += "  " + trailing
        new_line += eol
        out[cat_line_idx] = new_line
        stats["inferred"] += 1

    def close_block():
        nonlocal in_table, current_id, cat_line_idx, existing_cat, pending_block
        flush_block()
        in_table = False
        current_id = None
        cat_line_idx = None
        existing_cat = ""
        pending_block = []

    for line in lines:
        s = line.strip()
        if s.startswith("[[table]]"):
            close_block()
            in_table = True
            out.append(line)
            pending_block.append((len(out) - 1, line))
            continue
        if s.startswith("[[") or s.startswith("["):
            close_block()
            out.append(line)
            continue
        if not in_table:
            out.append(line)
            continue

        # Inside a [[table]] block
        out.append(line)
        pending_block.append((len(out) - 1, line))

        if "=" in s and not s.startswith("#"):
            key, _, val = s.partition("=")
            key = key.strip()
            if key == "id":
                v = val.strip()
                if "#" in v:
                    v = v.split("#", 1)[0].strip()
                if v.startswith('"') and v.endswith('"'):
                    current_id = v[1:-1]
            elif key == "category":
                cat_line_idx = len(out) - 1
                v = val.strip()
                if "#" in v:
                    v = v.split("#", 1)[0].strip()
                if v.startswith('"') and v.endswith('"'):
                    existing_cat = v[1:-1]
                else:
                    existing_cat = v

    close_block()
    return ("".join(out), stats)


def process_file(path: Path, *, dry_run: bool, force: bool, verbose: bool
                 ) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    new_text, stats = categorize_pack_text(text, force=force)
    changed = new_text != text
    tag = "[dry]" if dry_run else ("[ok]" if changed else "[--]")
    print(f"  {tag}  {path}  "
          f"inferred={stats['inferred']} kept={stats['kept']} "
          f"unmatched={stats['unmatched']}")
    if not dry_run and changed:
        path.write_text(new_text, encoding="utf-8")
    if verbose and stats["unmatched"] > 0:
        # Walk the original text again to surface unmatched ids for
        # the human to add new rules for.
        import tomllib
        d = tomllib.loads(text)
        shown = 0
        for t in d.get("table", []):
            if t.get("category", ""):
                continue
            if infer_category(t["id"]) is None:
                print(f"      unmatched: {t['id']}")
                shown += 1
                if shown >= 10:
                    print(f"      ... ({stats['unmatched'] - shown} more)")
                    break
    return stats


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="categorize",
        description="Infer [[table]].category from id prefixes for "
                    "VA/VB-era packs.")
    ap.add_argument("--pack", action="append", default=[], type=Path,
                    help="A pack TOML to process. May be repeated.")
    ap.add_argument("--glob", action="append", default=[],
                    help="A glob pattern to expand. May be repeated.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would change without writing.")
    ap.add_argument("--force", action="store_true",
                    help="Recompute even tables that already have a "
                         "non-empty category. Default: leave existing "
                         "categories untouched.")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="List unmatched table ids (up to 10 per pack) "
                         "so you can extend the rule set.")
    args = ap.parse_args(argv)

    paths: list[Path] = list(args.pack)
    for pattern in args.glob:
        paths.extend(Path(p) for p in sorted(globlib.glob(pattern)))
    paths = sorted({p.resolve() for p in paths})

    if not paths:
        print("categorize: no --pack or --glob provided", file=sys.stderr)
        return 2

    totals = {"inferred": 0, "kept": 0, "unmatched": 0}
    for p in paths:
        if not p.is_file():
            print(f"  [skip] {p}: not a file", file=sys.stderr)
            continue
        stats = process_file(p, dry_run=args.dry_run, force=args.force,
                             verbose=args.verbose)
        for k, v in stats.items():
            totals[k] = totals.get(k, 0) + v

    print()
    print(f"summary: inferred={totals['inferred']} kept={totals['kept']} "
          f"unmatched={totals['unmatched']}"
          f"{' (dry-run, no files written)' if args.dry_run else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
