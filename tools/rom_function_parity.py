#!/usr/bin/env python3
"""Byte-verify Ghidra function-index parity between sibling ROMs."""
from __future__ import annotations
import argparse, hashlib, json
from collections import defaultdict
from pathlib import Path

def load_index(path: Path) -> dict[int, tuple[int, str, bool]]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"): continue
        columns = line.split("\\t")
        if len(columns) >= 5:
            result[int(columns[0], 16)] = (int(columns[1]), columns[4], columns[3] != "-")
    return result

def compare(left_rom: Path, left_index: Path, right_rom: Path, right_index: Path,
            left_base: int = 0, right_base: int = 0) -> dict:
    lb, rb = left_rom.read_bytes(), right_rom.read_bytes()
    li, ri = load_index(left_index), load_index(right_index)
    common = sorted(set(li) & set(ri)); same_span=[]; same_body=[]; changed=[]; size_mismatch=[]
    for address in common:
        if li[address][0] != ri[address][0]: size_mismatch.append(address); continue
        same_span.append(address); size=li[address][0]
        left_offset, right_offset = address-left_base, address-right_base
        left_body = lb[left_offset:left_offset+size] if left_offset >= 0 else b""
        right_body = rb[right_offset:right_offset+size] if right_offset >= 0 else b""
        (same_body if left_body == right_body and len(left_body) == size else changed).append(address)
    left_hashes, right_hashes = defaultdict(list), defaultdict(list)
    for address, (size, name, named) in li.items():
        offset = address-left_base
        body = lb[offset:offset+size] if offset >= 0 else b""
        if len(body) == size:
            left_hashes[hashlib.sha256(body).hexdigest()].append((address,size,name,named))
    for address, (size, name, named) in ri.items():
        offset = address-right_base
        body = rb[offset:offset+size] if offset >= 0 else b""
        if len(body) == size:
            right_hashes[hashlib.sha256(body).hexdigest()].append((address,size,name,named))
    shared_hashes = set(left_hashes) & set(right_hashes)
    unique_mappings, unique_relocations = [], []
    ambiguous = 0
    for body_hash in sorted(shared_hashes):
        left_matches, right_matches = left_hashes[body_hash], right_hashes[body_hash]
        if len(left_matches) != 1 or len(right_matches) != 1:
            ambiguous += 1
            continue
        left_match, right_match = left_matches[0], right_matches[0]
        mapping = {
            "left_address":f"0x{left_match[0]:X}", "right_address":f"0x{right_match[0]:X}",
            "size":left_match[1], "sha256":body_hash,
            "left_name":left_match[2], "right_name":right_match[2],
            "left_named":left_match[3], "right_named":right_match[3]}
        unique_mappings.append(mapping)
        if left_match[0] != right_match[0]:
            unique_relocations.append(mapping)
    return {"schema":"subuwutuner.rom-function-parity.v1", "left_functions":len(li), "right_functions":len(ri),
            "common_starts":len(common), "same_spans":len(same_span), "identical_bodies":len(same_body),
            "shared_body_hashes_any_address":len(shared_hashes),
            "unique_identical_body_mappings":unique_mappings,
            "unique_relocated_identical_bodies":unique_relocations,
            "ambiguous_shared_body_hashes":ambiguous,
            "changed_bodies":[f"0x{x:X}" for x in changed], "size_mismatches":[f"0x{x:X}" for x in size_mismatch],
            "left_only":[f"0x{x:X}" for x in sorted(set(li)-set(ri))],
            "right_only":[f"0x{x:X}" for x in sorted(set(ri)-set(li))]}

def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("left_rom",type=Path); p.add_argument("left_index",type=Path)
    p.add_argument("right_rom",type=Path); p.add_argument("right_index",type=Path); p.add_argument("--out",type=Path)
    p.add_argument("--left-base",type=lambda value:int(value,0),default=0)
    p.add_argument("--right-base",type=lambda value:int(value,0),default=0)
    a=p.parse_args(); result=compare(a.left_rom,a.left_index,a.right_rom,a.right_index,a.left_base,a.right_base)
    text=json.dumps(result,indent=2)+"\n"
    if a.out: a.out.write_text(text,encoding="utf-8")
    else: print(text,end="")
    incompatible=len(result["changed_bodies"])+len(result["size_mismatches"])+len(result["left_only"])+len(result["right_only"])
    return 1 if incompatible else 0
if __name__=="__main__": raise SystemExit(main())
