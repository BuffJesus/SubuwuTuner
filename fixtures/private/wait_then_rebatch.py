"""
wait_then_rebatch.py
====================

Watch romraider_index_full.log for the forum walk to finish (no new lines
for 90 s), then remove NO_VALID_PLAINTEXT and NO_HITS entries from
batch_progress.jsonl so batch_harvest.py will retry those clusters with
any newly-indexed attachments.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / "romraider_index_full.log"
PROGRESS = HERE / "roms_extracted" / "batch_progress.jsonl"


def wait_for_walk_done(idle_s: int = 90, max_wait_s: int = 1800) -> None:
    print(f"watching {LOG} for {idle_s}s of inactivity (max {max_wait_s}s)…",
          flush=True)
    start = time.time()
    last_size = LOG.stat().st_size if LOG.exists() else 0
    last_change = time.time()
    while time.time() - start < max_wait_s:
        time.sleep(20)
        if not LOG.exists():
            continue
        sz = LOG.stat().st_size
        if sz != last_size:
            print(f"  log still growing ({sz:,}B)", flush=True)
            last_size = sz; last_change = time.time()
        else:
            idle = time.time() - last_change
            print(f"  idle for {idle:.0f}s", flush=True)
            if idle >= idle_s:
                print("  walk appears done.", flush=True)
                return
    print("  max wait reached; proceeding anyway.", flush=True)


def filter_progress() -> int:
    """Drop NO_VALID_PLAINTEXT and NO_HITS entries (so they get retried).
    Keep USABLE and NO_PRIMARY."""
    if not PROGRESS.exists():
        print("  no progress log to filter."); return 0
    kept, dropped = [], []
    for line in PROGRESS.read_text(encoding="utf-8").splitlines():
        if not line.strip(): continue
        try:
            r = json.loads(line)
        except Exception:
            kept.append(line); continue
        if r.get("outcome") in ("NO_VALID_PLAINTEXT", "NO_HITS"):
            dropped.append(r["anchor"])
        else:
            kept.append(line)
    PROGRESS.write_text("\n".join(kept) + "\n", encoding="utf-8")
    print(f"  kept {len(kept)} progress entries, dropped {len(dropped)} for retry: "
          f"{', '.join(dropped[:8])}{'…' if len(dropped) > 8 else ''}")
    return len(dropped)


def main() -> int:
    wait_for_walk_done(idle_s=90, max_wait_s=1800)
    n_retry = filter_progress()
    if n_retry == 0:
        print("nothing to retry; not running batch_harvest.")
        return 0
    print(f"\n=== re-running batch_harvest for {n_retry} clusters ===\n", flush=True)
    proc = subprocess.run(
        [sys.executable, "-X", "utf8", "-u", str(HERE / "batch_harvest.py")],
        cwd=str(HERE),
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
    )
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
