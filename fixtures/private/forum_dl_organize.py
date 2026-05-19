"""
forum_dl_organize.py
====================

Shared routing logic for sorting + renaming files in
roms_extracted/forumdownloads/ by extension and ECU ID.

Used both by:
  - romraider_forum_system.py at download time (download_one routes via
    classify_path + place_downloaded)
  - One-shot migrate_existing() over the directory at any time

Layout produced:

  forumdownloads/
    archives/<FAMILY>/<ECU_ID>_aid<N>.zip      # .zip / .rar where ECU ID detectable
    archives/_unknown/aid<N>_<orig>.zip        # .zip / .rar where ID can't be guessed
    binaries/<FAMILY>/<ECU_ID>_aid<N>.bin
    binaries/_unknown/aid<N>_<orig>.bin
    images/aid<N>_<orig>.png
    data/aid<N>_<orig>.csv
    documents/aid<N>_<orig>.pdf
    exe/aid<N>_<orig>.exe
    other/aid<N>_<orig>.<ext>                  # anything we don't recognize
    cookies.txt, cookies_session.txt, download_attempts.jsonl, download_summary.csv
                                               # stay at root, never moved
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

# Top-level files that the download script reads/writes — never touch.
PROTECTED_NAMES = {
    "cookies.txt", "cookies_session.txt",
    "download_attempts.jsonl", "download_summary.csv",
}

ARCHIVE_EXTS = {".zip", ".rar", ".7z", ".tar", ".gz", ".tgz"}
BINARY_EXTS = {".bin", ".hex", ".rom", ".srf"}
IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg", ".ico"}
DATA_EXTS = {".csv", ".xml", ".xls", ".xlsx", ".json", ".tsv", ".dbc"}
DOCUMENT_EXTS = {".pdf", ".txt", ".doc", ".docx", ".rtf", ".md", ".html", ".htm"}
EXE_EXTS = {".exe", ".dll", ".msi", ".bat", ".ps1"}

SEVENZ = Path(r"C:\Programs\7-Zip\7z.exe")

# Subaru ECU ID pattern: 7-9 char uppercase ID, with at least one digit.
_ECUID_FILE_RE = re.compile(r"[A-Z][A-Z0-9]{6,8}")
_ECUID_BIN_RE = re.compile(rb"[A-Z][A-Z0-9]{6,8}")


def extract_aid(filename: str) -> int | None:
    """Strip the `<aid:07d>_` prefix added by the download script, if present."""
    m = re.match(r"^(\d{7})_", filename)
    return int(m.group(1)) if m else None


def strip_aid_prefix(filename: str) -> str:
    """Return the original-uploader filename, without our aid prefix."""
    m = re.match(r"^\d{7}_(.+)$", filename)
    return m.group(1) if m else filename


def ecu_id_from_filename(name: str) -> str | None:
    """Pull a plausible Subaru ECU ID from a filename. Prefer 8-char hits.

    Rejects matches that don't include any digit (those are usually market tags
    like 'USDM' or vehicle words like 'Outback') and matches that are common
    English ASCII words. Also rejects our own naming artifacts like 'AID42602'.
    Returns the matched ID in uppercase or None."""
    # First strip our own naming-scheme suffix (_aidNNNNN[.ext]) so it doesn't
    # leak into the regex hits.
    stripped = re.sub(r"_aid\d+", "", name, flags=re.IGNORECASE)
    candidates = _ECUID_FILE_RE.findall(stripped.upper())
    candidates = [c for c in candidates if any(ch.isdigit() for ch in c)]
    candidates = [c for c in candidates if any(ch.isdigit() for ch in c[2:6])]
    # Reject lingering 'AID<digits>' artifacts
    candidates = [c for c in candidates if not c.startswith("AID")]
    if not candidates:
        return None
    eights = [c for c in candidates if len(c) == 8]
    return eights[0] if eights else candidates[0]


# Newer DIT/CVT ROMs embed the ECU ID flanked by null padding and a single
# market-marker byte: 00 00 00 00 00 00 [U|T|...] 00 <EID> 00 00 00 00 20 ...
_NEWER_SIG_RE = re.compile(
    rb"\x00\x00\x00\x00\x00\x00[A-Z]\x00([A-Z][A-Z0-9]{7})\x00\x00\x00\x00"
)


def _valid_subaru_id(s: str) -> bool:
    """Subaru ECU ID pattern: 8 chars, starts with letter, digit somewhere in
    middle. Excludes common false positives like ASCII boilerplate text."""
    if len(s) != 8 or not s[0].isalpha():
        return False
    if not any(ch.isdigit() for ch in s):
        return False
    if not any(ch.isdigit() for ch in s[2:6]):
        return False
    return True


def ecu_id_from_binary(data: bytes) -> str | None:
    """Find the embedded Subaru ECU ID string in a ROM binary.

    Strategy:
      1. Older Subaru: 0x2000 region (often with ``\\xff\\xff L`` prefix).
      2. Newer DIT/CVT: regex for the null-padded `\\x00...market\\x00<ID>\\x00...`
         signature (anywhere in the file).
      3. Last-ditch: full-file regex scan, return the most-frequent valid ID
         (the file's own ID typically appears multiple times)."""
    from collections import Counter

    # 1. Older Subaru: 0x2000 region. Skip leading 0xFF 0xFF and optional " L".
    if len(data) >= 0x200D:
        probe = data[0x2000:0x200D]
        skip = 0
        for ch in probe:
            if ch in (0xFF, 0x20, 0x4C) and skip < 4:
                skip += 1
            else:
                break
        cand = probe[skip:skip + 8]
        if len(cand) == 8 and all(0x30 <= b < 0x7F for b in cand):
            try:
                s = cand.decode("ascii").upper()
                if _valid_subaru_id(s):
                    return s
            except UnicodeDecodeError:
                pass

    # 2. Newer DIT/CVT signature.
    m = _NEWER_SIG_RE.search(data)
    if m:
        s = m.group(1).decode("ascii")
        if _valid_subaru_id(s):
            return s

    # 3. Full-file regex scan, take most frequent.
    counts: Counter[str] = Counter()
    for m in _ECUID_BIN_RE.finditer(data):
        s = m.group().decode("ascii", errors="ignore")
        if _valid_subaru_id(s):
            counts[s] += 1
    if not counts:
        return None
    most_common, n = counts.most_common(1)[0]
    if n >= 2:
        return most_common
    return None


def _peek_archive_inner_names(archive_path: Path) -> list[str]:
    """List inner filenames of a zip/rar without extracting. Used for ID detection."""
    suffix = archive_path.suffix.lower()
    try:
        if suffix == ".zip":
            with zipfile.ZipFile(archive_path) as z:
                return [i.filename for i in z.infolist() if not i.is_dir()]
        elif suffix == ".rar" and SEVENZ.exists():
            r = subprocess.run(
                [str(SEVENZ), "l", "-slt", str(archive_path)],
                capture_output=True, text=True,
            )
            if r.returncode != 0:
                return []
            names: list[str] = []
            for line in r.stdout.splitlines():
                if line.startswith("Path = "):
                    names.append(line[7:].strip())
            return names[1:]  # first Path = is the archive name itself
    except Exception:
        pass
    return []


def _read_archive_bin(archive_path: Path) -> bytes | None:
    """Extract the first .bin/.hex/.rom inside the archive and return its
    bytes. Returns None if no binary entry, archive unreadable, or extraction
    fails. Uses zipfile for .zip, 7z for .rar (size-capped to keep RAM sane)."""
    suffix = archive_path.suffix.lower()
    MAX_SIZE = 8 * 1024 * 1024  # cap at 8 MiB to ignore weird large entries
    try:
        if suffix == ".zip":
            with zipfile.ZipFile(archive_path) as z:
                for inf in z.infolist():
                    if inf.is_dir():
                        continue
                    if Path(inf.filename).suffix.lower() not in BINARY_EXTS:
                        continue
                    if inf.file_size > MAX_SIZE:
                        continue
                    return z.read(inf)
        elif suffix == ".rar" and SEVENZ.exists():
            with tempfile.TemporaryDirectory() as td:
                r = subprocess.run(
                    [str(SEVENZ), "x", str(archive_path), f"-o{td}", "-y"],
                    capture_output=True, text=True,
                )
                if r.returncode != 0:
                    return None
                for inner in Path(td).rglob("*"):
                    if inner.is_file() and inner.suffix.lower() in BINARY_EXTS:
                        if inner.stat().st_size <= MAX_SIZE:
                            return inner.read_bytes()
    except Exception:
        pass
    return None


def ecu_id_for_archive(archive_path: Path) -> str | None:
    """Try outer name, then inner filenames, then extract+scan inner bytes."""
    eid = ecu_id_from_filename(archive_path.name)
    if eid:
        return eid
    for inner in _peek_archive_inner_names(archive_path):
        eid = ecu_id_from_filename(inner)
        if eid:
            return eid
    # Final attempt: extract the first inner binary and scan its bytes
    data = _read_archive_bin(archive_path)
    if data is not None:
        return ecu_id_from_binary(data)
    return None


def classify(suffix: str) -> str:
    s = suffix.lower()
    if s in ARCHIVE_EXTS:
        return "archives"
    if s in BINARY_EXTS:
        return "binaries"
    if s in IMAGE_EXTS:
        return "images"
    if s in DATA_EXTS:
        return "data"
    if s in DOCUMENT_EXTS:
        return "documents"
    if s in EXE_EXTS:
        return "exe"
    return "other"


@dataclass(frozen=True)
class TargetPath:
    subdir: str          # e.g. 'archives', 'binaries', 'images'
    family: str | None   # e.g. 'D2UG' for ECU-IDed bins/archives, else None
    filename: str        # final basename


def target_path(root: Path, aid: int | None, orig_name: str,
                file_data: bytes | None = None,
                src_path: Path | None = None) -> TargetPath:
    """Decide where a downloaded file should live.

    `aid` is the forum attachment id (or None for legacy files).
    `orig_name` is the uploader-provided filename (no aid prefix).
    For binaries, `file_data` lets us scan the bytes for an embedded ECU ID.
    For archives, `src_path` lets us peek at inner filenames."""
    suffix = Path(orig_name).suffix
    bucket = classify(suffix)
    aid_str = f"aid{aid}" if aid is not None else "aid_unknown"

    # ECU-ID renaming only applies to archives and binaries
    eid: str | None = None
    if bucket == "binaries":
        eid = ecu_id_from_filename(orig_name)
        if not eid and file_data is not None:
            eid = ecu_id_from_binary(file_data)
    elif bucket == "archives":
        # try filename, then inner names if we have the file on disk
        eid = ecu_id_from_filename(orig_name)
        if not eid and src_path is not None and src_path.exists():
            eid = ecu_id_for_archive(src_path)

    if eid:
        family = eid[:4].upper()
        return TargetPath(subdir=bucket, family=family,
                          filename=f"{eid}_{aid_str}{suffix}")
    # No ID: fall back to original name preserved under _unknown/ for arch/bin,
    # or extension-only subdir for everything else.
    if bucket in ("archives", "binaries"):
        return TargetPath(subdir=bucket, family="_unknown",
                          filename=f"{aid_str}_{orig_name}")
    return TargetPath(subdir=bucket, family=None,
                      filename=f"{aid_str}_{orig_name}")


def resolve_collision(dest: Path) -> Path:
    """If dest exists, append .1, .2, ... before the extension."""
    if not dest.exists():
        return dest
    stem, suffix = dest.stem, dest.suffix
    n = 1
    while True:
        cand = dest.with_name(f"{stem}.{n}{suffix}")
        if not cand.exists():
            return cand
        n += 1


def place_file(root: Path, src: Path, tp: TargetPath,
               *, dry_run: bool = False) -> Path:
    """Move `src` to its sorted destination under `root`. Returns final path.
    Idempotent: if src is already at the target, no-op."""
    parts = [root, tp.subdir]
    if tp.family:
        parts.append(tp.family)
    dest_dir = Path(*parts)
    dest = dest_dir / tp.filename
    if src.resolve() == dest.resolve():
        return dest
    if not dry_run:
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = resolve_collision(dest)
        shutil.move(str(src), str(dest))
    return dest


def migrate_existing(root: Path, *, dry_run: bool = False) -> dict:
    """Walk root and sort every loose file into the new layout.

    Files in PROTECTED_NAMES stay at root. Files already in a subdir of the
    target layout (archives/, binaries/, ...) are skipped — idempotent."""
    counts: dict[str, int] = {}
    target_subdirs = {"archives", "binaries", "images", "data", "documents",
                      "exe", "other"}
    for p in sorted(root.iterdir()):
        if p.is_dir():
            continue
        if p.name in PROTECTED_NAMES:
            counts["protected"] = counts.get("protected", 0) + 1
            continue
        # Skip files whose parent is already a recognized subdir (impossible
        # at root, but covers re-running on already-migrated subdirs)
        if p.parent.name in target_subdirs:
            counts["already-sorted"] = counts.get("already-sorted", 0) + 1
            continue
        aid = extract_aid(p.name)
        orig = strip_aid_prefix(p.name)
        # For binary files we read a small head for ID detection
        file_head: bytes | None = None
        if Path(orig).suffix.lower() in BINARY_EXTS:
            try:
                file_head = p.read_bytes()[:65536]
            except OSError:
                pass
        tp = target_path(root, aid, orig, file_data=file_head, src_path=p)
        dest = place_file(root, p, tp, dry_run=dry_run)
        key = f"{tp.subdir}/{tp.family or '-'}"
        counts[key] = counts.get(key, 0) + 1
    return counts


def main(argv: list[str] | None = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--root", default="roms_extracted/forumdownloads",
                    help="Root forumdownloads directory")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    counts = migrate_existing(Path(args.root), dry_run=args.dry_run)
    print(f"Migrate summary ({'DRY-RUN' if args.dry_run else 'EXECUTED'}):")
    for k in sorted(counts):
        print(f"  {k:<32}  {counts[k]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
