"""
stage_forum_plaintexts.py
=========================

Walk roms_extracted/forumdownloads/, extract candidate plaintext ROMs from
.zip and direct .bin/.hex/.rom files, identify each by ECU ID (filename or
embedded ASCII), reject placeholders (zero vector table / ASCII-string vectors),
and copy validated plaintexts into plaintext_corpus/forum-bins/<ECU_ID>.bin
where they can be picked up by decrypt_epifan.py.

Does not delete or modify source files. Skips files already present in the
output directory (idempotent, run as often as desired).
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from forum_dl_organize import (
    ecu_id_from_binary,
    ecu_id_from_filename,
    SEVENZ,
    BINARY_EXTS,
)

HERE = Path(__file__).resolve().parent
SRC = HERE / "roms_extracted" / "forumdownloads"
DST = HERE / "plaintext_corpus" / "forum-bins"
_BIN_EXT = tuple(BINARY_EXTS)


def vector_table_ok(b: bytes) -> tuple[bool, str]:
    """Reject placeholders: zero vector table OR ASCII-text high bytes."""
    if len(b) < 8:
        return False, "too short"
    sp = int.from_bytes(b[:4], "big")
    rv = int.from_bytes(b[4:8], "big")
    if sp == 0 and rv == 0:
        return False, "vector table all-zero"
    if all(0x20 <= x < 0x7f for x in b[:8]):
        return False, "first 8 bytes are ASCII"
    if 0x20 <= (sp >> 24) < 0x7f or 0x20 <= (rv >> 24) < 0x7f:
        return False, f"vectors look ASCII (sp=0x{sp:08x} rv=0x{rv:08x})"
    return True, f"sp=0x{sp:08x} rv=0x{rv:08x}"


def stage_one(eid: str, data: bytes, source: str) -> str:
    DST.mkdir(parents=True, exist_ok=True)
    dst = DST / f"{eid}.bin"
    if dst.exists():
        existing = dst.read_bytes()
        if existing == data:
            return "DUP-IDENTICAL"
        # If a different file already lives at this ECU ID, keep both
        n = 1
        while True:
            alt = DST / f"{eid}.{n}.bin"
            if not alt.exists():
                alt.write_bytes(data)
                return f"WROTE-ALT {alt.name}"
            n += 1
    dst.write_bytes(data)
    return f"WROTE {dst.name} ({len(data):,} bytes) from {source}"


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    if not SRC.exists():
        print(f"Source dir not found: {SRC}", file=sys.stderr)
        return 2

    counts = {"zip-rom": 0, "zip-placeholder": 0, "direct-rom": 0,
              "direct-placeholder": 0, "skip": 0, "rar": 0, "noid": 0}
    for p in sorted(SRC.rglob("*")):
        if not p.is_file():
            continue
        # Skip our own protected/script-managed files
        if p.name in ("cookies.txt", "cookies_session.txt",
                      "download_attempts.jsonl", "download_summary.csv"):
            continue
        suffix = p.suffix.lower()
        if suffix == ".zip":
            try:
                with zipfile.ZipFile(p) as z:
                    for inf in z.infolist():
                        if inf.is_dir() or not inf.filename.lower().endswith(_BIN_EXT):
                            continue
                        data = z.read(inf)
                        eid = (ecu_id_from_filename(inf.filename)
                               or ecu_id_from_filename(p.name)
                               or ecu_id_from_binary(data))
                        if not eid:
                            counts["noid"] += 1
                            print(f"  NO-ID {p.name}::{inf.filename}")
                            continue
                        ok, why = vector_table_ok(data)
                        if not ok:
                            counts["zip-placeholder"] += 1
                            print(f"  PLACEHOLDER {p.name}::{inf.filename} -> {eid} ({why})")
                            continue
                        msg = stage_one(eid, data, f"{p.name}::{inf.filename}")
                        if msg.startswith("WROTE"):
                            counts["zip-rom"] += 1
                            print(f"  ZIP {p.name}::{inf.filename} -> {eid}: {msg}")
                        else:
                            counts["skip"] += 1
            except zipfile.BadZipFile as e:
                print(f"  BAD-ZIP {p.name}: {e}", file=sys.stderr)
        elif suffix == ".rar":
            if not SEVENZ.exists():
                counts["rar"] += 1
                print(f"  RAR (skipped, 7z not at {SEVENZ}): {p.name}")
                continue
            with tempfile.TemporaryDirectory() as td:
                r = subprocess.run(
                    [str(SEVENZ), "x", str(p), f"-o{td}", "-y"],
                    capture_output=True, text=True,
                )
                if r.returncode != 0:
                    counts["rar"] += 1
                    print(f"  RAR-FAIL {p.name}: exit={r.returncode}")
                    continue
                for inner in Path(td).rglob("*"):
                    if not inner.is_file():
                        continue
                    if inner.suffix.lower() not in _BIN_EXT:
                        continue
                    data = inner.read_bytes()
                    eid = (ecu_id_from_filename(inner.name)
                           or ecu_id_from_filename(p.name)
                           or ecu_id_from_binary(data))
                    if not eid:
                        counts["noid"] += 1
                        continue
                    ok, why = vector_table_ok(data)
                    if not ok:
                        counts["zip-placeholder"] += 1
                        print(f"  PLACEHOLDER (rar) {p.name}::{inner.name} -> {eid} ({why})")
                        continue
                    msg = stage_one(eid, data, f"{p.name}::{inner.name}")
                    if msg.startswith("WROTE"):
                        counts["zip-rom"] += 1
                        print(f"  RAR {p.name}::{inner.name} -> {eid}: {msg}")
                    else:
                        counts["skip"] += 1
        elif suffix in _BIN_EXT or suffix == "":
            try:
                data = p.read_bytes()
            except OSError as e:
                print(f"  READ-ERR {p.name}: {e}", file=sys.stderr)
                continue
            eid = ecu_id_from_filename(p.name) or ecu_id_from_binary(data)
            if not eid:
                counts["noid"] += 1
                continue
            ok, why = vector_table_ok(data)
            if not ok:
                counts["direct-placeholder"] += 1
                print(f"  PLACEHOLDER (direct) {p.name} -> {eid} ({why})")
                continue
            msg = stage_one(eid, data, p.name)
            if msg.startswith("WROTE"):
                counts["direct-rom"] += 1
                print(f"  DIRECT {p.name} -> {eid}: {msg}")
            else:
                counts["skip"] += 1

    print("\n=== Summary ===")
    for k, v in counts.items():
        print(f"  {k:>20}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
