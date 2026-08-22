#!/usr/bin/env python3
"""Build an offline ROM-corpus / reverse-engineering queue.

The private fixture tree contains far more plaintext ECU images than the
small supported-corpus mirror under findings/engine-ecu-rom-plaintext. This
report answers three practical questions without opening hardware:

* which exact CIDs have a matching in-tree definition pack;
* how many sibling images are available for cross-CID naming and validation;
* which CIDs already have Ghidra/index artifacts and which are untouched.

This is inventory only. It does not copy ROMs, edit definitions, run Ghidra,
or authorize a write.
"""

from __future__ import annotations

import argparse
import json
import re
import tomllib
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def definition_ids(root: Path) -> dict[str, Path]:
    """Return pack ids keyed by lower-case stem, excluding COBB overlays."""
    out: dict[str, Path] = {}
    for path in sorted(root.rglob("*.toml")):
        if "_cobb_" in path.stem.lower():
            continue
        # Only accept files that actually parse as a definition pack. This
        # avoids treating pids.toml and unrelated support TOML as CIDs.
        try:
            doc: dict[str, Any] = tomllib.loads(path.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError):
            continue
        if not isinstance(doc.get("pack"), dict):
            continue
        pack_id = str(doc["pack"].get("id", path.stem)).lower()
        out.setdefault(pack_id, path)
        out.setdefault(path.stem.lower(), path)
    return out


def matched_cid(path: Path, ids: dict[str, Path]) -> str | None:
    tokens = {token.lower() for token in TOKEN_RE.findall(path.stem)}
    matches = [token for token in tokens if token in ids]
    if not matches:
        return None
    # Prefer the longest exact token: it avoids choosing a short family token
    # when a filename carries the complete CID as well.
    return max(matches, key=lambda value: (len(value), value))


def artifact_status(cid: str, decompile_root: Path) -> dict[str, Any]:
    folder = decompile_root / cid
    index = folder / "function_index_named.txt"
    cal_index = folder / "calibration_index.tsv"
    decomp = list(folder.glob("decomp_*.txt"))
    return {
        "folder": str(folder),
        "folder_exists": folder.is_dir(),
        "function_index_named": index.exists(),
        "calibration_index": cal_index.exists(),
        "decomp_windows": len(decomp),
    }


def build_inventory(corpus_root: Path, definitions_root: Path,
                    decompile_root: Path) -> dict[str, Any]:
    ids = definition_ids(definitions_root)
    by_cid: dict[str, list[dict[str, Any]]] = defaultdict(list)
    unmatched = 0
    for path in sorted(corpus_root.rglob("*.bin")):
        cid = matched_cid(path, ids)
        if cid is None:
            unmatched += 1
            continue
        by_cid[cid].append({
            "path": str(path),
            "size": path.stat().st_size,
        })

    cids: list[dict[str, Any]] = []
    for cid, files in sorted(by_cid.items()):
        sizes = Counter(int(row["size"]) for row in files)
        status = artifact_status(cid, decompile_root)
        cids.append({
            "cid": cid.upper(),
            "family": cid[:4].upper(),
            "image_count": len(files),
            "sizes": dict(sorted(sizes.items())),
            "definition": str(ids[cid]),
            "artifacts": status,
            "files": files,
        })

    families: dict[str, dict[str, Any]] = {}
    for row in cids:
        family = row["family"]
        item = families.setdefault(family, {
            "family": family,
            "cid_count": 0,
            "image_count": 0,
            "cids": [],
            "defined_cids": 0,
            "named_cids": 0,
        })
        item["cid_count"] += 1
        item["image_count"] += row["image_count"]
        item["cids"].append(row["cid"])
        item["defined_cids"] += 1
        if row["artifacts"]["function_index_named"]:
            item["named_cids"] += 1

    return {
        "schema": "subuwutuner.rom-corpus-research-queue.v1",
        "corpus_root": str(corpus_root),
        "definitions_root": str(definitions_root),
        "decompile_root": str(decompile_root),
        "definition_count": len(ids),
        "matched_image_count": sum(row["image_count"] for row in cids),
        "unmatched_image_count": unmatched,
        "unique_cid_count": len(cids),
        "cids": cids,
        "families": sorted(families.values(), key=lambda row: (-row["cid_count"], row["family"])),
    }


def render_markdown(data: dict[str, Any]) -> str:
    cids = data["cids"]
    untouched = [row for row in cids if not row["artifacts"]["function_index_named"]]
    untouched.sort(key=lambda row: (-row["image_count"], row["cid"]))
    families = data["families"]

    lines = [
        "# Plaintext ROM corpus / RE queue",
        "",
        "> Inventory only. This report does not copy ROMs, edit definitions,",
        "> run Ghidra, prove calibration semantics, or authorize ECU writes.",
        "",
        f"- Plaintext corpus files matched to packs: **{data['matched_image_count']}**",
        f"- Unique defined CIDs: **{data['unique_cid_count']}**",
        f"- Unmatched binary files: **{data['unmatched_image_count']}**",
        f"- Definition packs considered: **{data['definition_count']}**",
        "",
        "## Best next targets",
        "",
        "These are the highest-leverage untouched CIDs: multiple sibling images",
        "make signature naming and table-address reconciliation substantially more",
        "useful than a one-off decompile.",
        "",
        "| CID | Family | Plaintext images | Definition | Existing artifacts | Why it matters |",
        "|---|---:|---:|---|---|---|",
    ]
    reasons = {
        "EP5G": "Existing EP5G analyst seed; natural first older-family baseline.",
        "EZ1G": "10-image NA Forester family; good cross-CID naming corpus.",
        "A8DH": "16-image FB/Forester family; broad definition coverage.",
        "LF79": "31-image VA family; strongest bridge to the current WRX work.",
        "LF9C": "21-image transitional VA family; useful sibling address drift.",
        "LF9D": "15-image 2.5 MB VA family; direct SH-2A calibration comparison.",
    }
    preferred = [row for row in untouched if row["family"] in reasons]
    preferred.sort(key=lambda row: (-row["image_count"], row["cid"]))
    for row in preferred[:24]:
        a = row["artifacts"]
        existing = (f"{a['decomp_windows']} windows" if a["decomp_windows"] else "none")
        lines.append(f"| `{row['cid']}` | `{row['family']}` | {row['image_count']} | "
                     f"`{Path(row['definition']).name}` | {existing} | {reasons[row['family']]} |")

    lines += [
        "",
        "## Family roll-up",
        "",
        "| Family | Defined CIDs | Plaintext images | Named/indexed CIDs | Example CIDs |",
        "|---|---:|---:|---:|---|",
    ]
    for family in families[:60]:
        examples = ", ".join(family["cids"][:8])
        lines.append(f"| `{family['family']}` | {family['cid_count']} | "
                     f"{family['image_count']} | {family['named_cids']} | {examples} |")

    lines += [
        "",
        "## Interpretation",
        "",
        "The current supported-corpus mirror is intentionally narrow, but the",
        "private plaintext corpus is not. The next useful RE expansion is therefore",
        "not another isolated VA pass: it is a reusable family baseline for an older",
        "A-series family with many sibling images, followed by definition promotion",
        "only after byte-verified table/address evidence exists.",
        "",
        "Recommended order:",
        "",
        "1. EP5G600A — finish the existing analyst seed and make it the first older",
        "   family with named functions plus a reproducible family index.",
        "2. EZ1G / A8DH — use the sibling volume to promote shared functions and",
        "   identify which definitions are truly stable across model years.",
        "3. LF79 / LF9C / LF9D — deepen VA coverage only where it improves the",
        "   tuning software: checksum/flash gates, live DIDs, and calibration roles.",
        "4. Treat every unlisted family as research-only until its image format,",
        "   architecture, checksum, and writable-region boundaries are independently",
        "   established.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path,
                        default=Path("fixtures/private/plaintext_corpus"))
    parser.add_argument("--definitions", type=Path, default=Path("definitions"))
    parser.add_argument("--decompile", type=Path,
                        default=Path(r"D:\Subuwu\findings\decompile"))
    parser.add_argument("--out", type=Path,
                        default=Path(r"D:\Subuwu\findings\decompile\ROM_CORPUS_RESEARCH_QUEUE.md"))
    parser.add_argument("--json-out", type=Path,
                        default=Path(r"D:\Subuwu\findings\decompile\ROM_CORPUS_RESEARCH_QUEUE.json"))
    args = parser.parse_args()

    data = build_inventory(args.corpus, args.definitions, args.decompile)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render_markdown(data), encoding="utf-8")
    args.json_out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(render_markdown(data).split("## Best next targets", 1)[0].rstrip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
