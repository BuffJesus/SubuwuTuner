"""
select_rom_aids.py
==================

Query romraider_forum_index.sqlite for attachment IDs (aid) whose filename or
topic title matches a ROM family prefix from roms_extracted/. Emits the aids
to stdout (one per line) for passing to:

    python romraider_forum_system.py download --aid <aid> <aid> ...

Also prints a summary by 4-char prefix to help judge coverage.

Defaults:
  - prefix source: rom_family_prefixes.txt (one prefix per line)
  - DB:            romraider_forum_index.sqlite
  - extensions:    .hex .bin .zip .xml (anything calibration-shaped)

Usage:
  python select_rom_aids.py                       # summary + aid list
  python select_rom_aids.py --aids-only           # just the aids (for xargs)
  python select_rom_aids.py --top 50              # cap at 50 results
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_DB = HERE / "romraider_forum_index.sqlite"
DEFAULT_PREFIXES = HERE / "rom_family_prefixes.txt"
DEFAULT_EXTS = (".hex", ".bin", ".zip", ".xml")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    p.add_argument("--db", default=str(DEFAULT_DB))
    p.add_argument("--prefixes", default=str(DEFAULT_PREFIXES))
    p.add_argument("--exts", nargs="*", default=list(DEFAULT_EXTS))
    p.add_argument("--top", type=int, default=0, help="Cap aid count")
    p.add_argument("--aids-only", action="store_true",
                   help="Emit just the aids on stdout (one per line)")
    args = p.parse_args()

    prefixes = [
        line.strip().upper()
        for line in Path(args.prefixes).read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    ]
    if not prefixes:
        print(f"No prefixes in {args.prefixes}", file=sys.stderr)
        return 2

    conn = sqlite3.connect(args.db)
    conn.row_factory = sqlite3.Row

    # Pull every attachment with filename, restricted by extension.
    ext_filter = " OR ".join("LOWER(a.filename) LIKE ?" for _ in args.exts)
    ext_params = [f"%.{e.lstrip('.').lower()}" for e in args.exts]
    q = f"""
        SELECT a.aid, a.filename, a.byte_size, a.poster,
               t.t, t.title AS topic_title, t.f
        FROM attachments a JOIN topics t ON a.t = t.t
        WHERE ({ext_filter})
    """
    rows = list(conn.execute(q, ext_params))
    if not args.aids_only:
        print(f"Attachments with matching extension: {len(rows)}", file=sys.stderr)

    # Substring-match the upper-cased filename or topic title against each prefix.
    by_prefix: dict[str, list] = {p: [] for p in prefixes}
    matched: list = []
    for r in rows:
        fn = (r["filename"] or "").upper()
        tt = (r["topic_title"] or "").upper()
        hits = [p for p in prefixes if p in fn or p in tt]
        if hits:
            matched.append(r)
            for h in hits:
                by_prefix[h].append(r)

    if args.top:
        matched = matched[: args.top]

    if args.aids_only:
        # Distinct aids only
        seen: set[int] = set()
        for r in matched:
            if r["aid"] not in seen:
                print(r["aid"])
                seen.add(r["aid"])
        return 0

    print(f"Matched attachments (distinct): {len({r['aid'] for r in matched})}",
          file=sys.stderr)
    print("\n--- prefix coverage (non-zero only) ---", file=sys.stderr)
    for pfx, items in sorted(by_prefix.items(), key=lambda kv: -len(kv[1])):
        if items:
            print(f"  {pfx}  {len(items):>4}", file=sys.stderr)

    print("\n--- first 30 matches ---", file=sys.stderr)
    for r in matched[:30]:
        sz = "" if r["byte_size"] is None else f"{r['byte_size']:>10,}"
        print(f"  aid={r['aid']:>7}  f={r['f']:>2}  size={sz}  "
              f"{(r['filename'] or '(no name)')[:50]}  | {r['topic_title'][:50]}",
              file=sys.stderr)

    # aid list to stdout
    seen: set[int] = set()
    for r in matched:
        if r["aid"] not in seen:
            print(r["aid"])
            seen.add(r["aid"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
