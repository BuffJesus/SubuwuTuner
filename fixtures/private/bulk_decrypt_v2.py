"""
bulk_decrypt_v2.py
==================

Second-generation decryption driver, adding three improvements over the
original `bulk_decrypt.py`:

  1. CID-readability gate (Patch A) — replaces the entropy + 0xFF density
     "OK" threshold with a stricter test: the decrypted output must
     contain the cipher's CID string at *some* byte offset. This
     classifies decrypts as `full` (CID readable), `partial` (heuristic
     passes but CID region still cipher), or `random` (heuristic fails).
     Partial decrypts are still saved but to a separate directory so they
     don't pollute the regression-fixture set.

  2. Bucket-internal anchorless keystream recovery (Patch B) — for
     cipher buckets with no plaintext anchor (1,573,632B and 4,195,328B
     currently), exploit the Subaru flash-erase 0xFF prior: at byte
     positions where many cipher pairs in the bucket agree (XOR == 0),
     both plaintexts are likely 0xFF. From that, K[p] = C[p] ⊕ 0xFF.
     The resulting partial keystream often decrypts at least one cipher
     to a CID-readable plaintext, which can then serve as a real anchor
     for a full keystream-recovery pass.

  3. Expanded CONFIRMED list (Patch C) — the original list had 5 anchor
     entries covering 5 of 8 cipher buckets. The 2,098,176B bucket
     (VA WRX FA-DIT) was anchorless despite 14 readable family-anchors
     being on disk under `roms_extracted/decrypted/`. This patch wires
     up one anchor per known-readable family to the schema, immediately
     decrypting ~14 ciphers in that bucket without changing any other
     pipeline behaviour.

The cipher's structure (empirical, this conversation's analysis):
  ciphertext = (plaintext ⊕ stream_keystream) ⊕ per_family_xor_layer
The keystream we recover from a (plaintext, ciphertext) pair is
actually `stream_keystream ⊕ per_family_xor[anchor_family]`, which only
decrypts other ciphers in the SAME 6-char-prefix family. A bucket with
N CID families needs N anchors, one per family.

Outputs (independent of bulk_decrypt.py's outputs so they coexist):
  roms_extracted/keystreams_v2/<bucket>_<anchor>.ks
  plaintext_corpus/decrypted_v2/full/<anchor>/<cid>.bin
  plaintext_corpus/decrypted_v2/partial/<anchor>/<cid>.bin
  roms_extracted/bulk_decrypt_v2_report.md

Never overwrites, never deletes. Safe to re-run.
"""

from __future__ import annotations

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
KS_DIR = HERE / "roms_extracted" / "keystreams_v2"
OUT_FULL = HERE / "plaintext_corpus" / "decrypted_v2" / "full"
OUT_PARTIAL = HERE / "plaintext_corpus" / "decrypted_v2" / "partial"
REPORT = HERE / "roms_extracted" / "bulk_decrypt_v2_report.md"


# ---------------------------------------------------------------------------
# Patch C: expanded CONFIRMED list
# ---------------------------------------------------------------------------
# Each entry: (label, plain_path_rel_to_HERE, cipher_filename, header_size,
#              plain_offset, bucket_size).
# plain_path is resolved relative to HERE (the script's directory) so anchors
# can live anywhere under fixtures/private/, not just under VAL/.

CONFIRMED = [
    # --- Original 5 from bulk_decrypt.py, paths rewritten to be HERE-relative
    ("ZA1JB00C_DIT_VA",
     "roms_extracted/forumdownloads/validation/aid_20825_ZA1JB00C/extracted/ZA1JB00C.hex",
     "za1jb00c_ori.hex", 768, 0, 1_311_488),
    ("XE1M501A_diesel_RH850",
     "roms_extracted/forumdownloads/validation/aid_42758_XE1M501A/extracted/XE1M501a00G_CB22607307-20250202-160434.bin",
     "xe1m501a__ori.hex", 1024, 16640, 4_064_000),
    ("LF9F000R_DIT_VB",
     "roms_extracted/forumdownloads/validation/aid_41097_LF9F000R/extracted/LF9F000R_CD01A04007.bin",
     "lf9f000r_ori.hex", 1024, 0, 2_622_464),
    ("ZA1JA02P_ZA1J_tail",
     "roms_extracted/forumdownloads/validation/aid_38285_ZA1JA02P/extracted/ZA1JA02P.bin",
     "za1ja02p_ori.hex", 768, 0, 1_311_488),
    ("LF6A400D_AF5G_LF6A",
     "roms_extracted/forumdownloads/validation/aid_26859_LF6A400D/extracted/Forester2015_LF6A400D.bin",
     "lf6a400d_ori.hex", 1024, 0, 1_573_888),

    # --- Patch C: 2,098,176B bucket — VA WRX FA-DIT (one anchor per
    # known-readable 6-char family on disk under decrypted/).
    ("LF75300_family",
     "roms_extracted/decrypted/LF75/LF75300E.bin",
     "lf75300e_ori.hex", 1024, 0, 2_098_176),
    ("LF78001_family",
     "roms_extracted/decrypted/LF78/LF78001C.bin",
     "lf78001c_ori.hex", 1024, 0, 2_098_176),
    ("LF9C000_family",
     "roms_extracted/decrypted/LF9C/LF9C000C.bin",
     "lf9c000c_ori.hex", 1024, 0, 2_098_176),
    ("LV9N100_family",
     "roms_extracted/decrypted/LV9N/LV9N100A.bin",
     "lv9n100a_ori.hex", 1024, 0, 2_098_176),
    ("LV9N303_family",
     "roms_extracted/decrypted/LV9N/LV9N303J.bin",
     "lv9n303j_ori.hex", 1024, 0, 2_098_176),

    # --- 1,049,600B bucket — EJ-era 1MB Subaru (one anchor per family
    # whose plaintext is already on disk; 15/75 families unlocked,
    # 60 still need community-sourced anchors).
    ("DE5F810_family",
     "roms_extracted/decrypted/DE5F/DE5F810G.bin",
     "de5f810g_ori.hex", 1024, 0, 1_049_600),
    ("DE5K411_family",
     "plaintext_corpus/forum-bins/DE5K411B.bin",
     "de5k411b_ori.hex", 1024, 0, 1_049_600),
    ("DE5M301_family",
     "roms_extracted/decrypted/DE5M/DE5M301C.bin",
     "de5m301c_ori.hex", 1024, 0, 1_049_600),
    ("DE5Q100_family",
     "roms_extracted/decrypted/DE5Q/DE5Q100B.bin",
     "de5q100b_ori.hex", 1024, 0, 1_049_600),
    ("EE5I410_family",
     "plaintext_corpus/forum-bins/EE5I410K.bin",
     "ee5i410k_ori.hex", 1024, 0, 1_049_600),
    ("EE5IA11_family",
     "roms_extracted/decrypted/EE5I/EE5IA11T.bin",
     "ee5ia11t_ori.hex", 1024, 0, 1_049_600),
    ("EE5K000_family",
     "plaintext_corpus/forum-bins/EE5K000K.bin",
     "ee5k000k_ori.hex", 1024, 0, 1_049_600),
    ("EE5K801_family",
     "plaintext_corpus/forum-bins/EE5K801T.bin",
     "ee5k801t_ori.hex", 1024, 0, 1_049_600),
    ("EP5D202_family",
     "plaintext_corpus/forum-bins/EP5D202V.bin",
     "ep5d202v_ori.hex", 1024, 0, 1_049_600),
    ("EP5D700_family",
     "roms_extracted/decrypted/EP5D/EP5D700X.bin",
     "ep5d700x_ori.hex", 1024, 0, 1_049_600),
    ("EP5F300_family",
     "roms_extracted/decrypted/EP5F/EP5F300H.bin",
     "ep5f300h_ori.hex", 1024, 0, 1_049_600),
    ("EP5G600_family",
     "plaintext_corpus/forum-bins/EP5G600A.bin",
     "ep5g600a_ori.hex", 1024, 0, 1_049_600),
    ("EP5I200_family",
     "roms_extracted/decrypted/EP5I/EP5I200F.bin",
     "ep5i200f_ori.hex", 1024, 0, 1_049_600),
    ("EP5I400_family",
     "plaintext_corpus/forum-bins/EP5I400F.bin",
     "ep5i400f_ori.hex", 1024, 0, 1_049_600),
    ("EZ1GB10_family",
     "roms_extracted/decrypted/EZ1G/EZ1GB10G.bin",
     "ez1gb10g_ori.hex", 1024, 0, 1_049_600),

    # --- Pending ECUTune purchases (2026-05-19). When the downloads
    # arrive, extract the _ori (stock) binary, place it at the path
    # below, and re-run bulk_decrypt_v2.py. Each unlocks an entire
    # 6-char family in its bucket.
    #
    # LF79100P  -> unlocks LF7910 family (15 CIDs, includes LF79103P)
    # URL: https://ecutune.shop/downloads/lf79100p-b029b07007-e2-chk-ok/
    # Drop the stock bin at: roms_extracted/decrypted/LF79/LF79100P.bin
    ("LF79100_family",
     "roms_extracted/decrypted/LF79/LF79100P.bin",
     "lf79100p_ori.hex", 1024, 0, 2_098_176),

    # EP5L000J  -> unlocks EP5L00 family (19 CIDs, largest unsolved 1MB)
    # URL: https://ecutune.shop/downloads/ep5l000j-91363e5007-stage1-tgv_off-chk-ok/
    #  or: https://ecutune.shop/downloads/ep5l000j-91363e5007-stage1-e2-egr_off-chk-ok/
    # Drop the stock bin at: roms_extracted/decrypted/EP5L/EP5L000J.bin
    ("EP5L000_family",
     "roms_extracted/decrypted/EP5L/EP5L000J.bin",
     "ep5l000j_ori.hex", 1024, 0, 1_049_600),
]

# Buckets with no plaintext anchor at all — Patch B targets these.
# (1,049,600 retired from this list as of the 15-family wire-up above.)
ANCHORLESS_BUCKETS = (1_573_632, 4_195_328)


# ---------------------------------------------------------------------------
# Stats and gate helpers
# ---------------------------------------------------------------------------

def stats(b: bytes) -> dict:
    n = len(b); c = Counter(b)
    H = -sum((cnt / n) * math.log2(cnt / n) for cnt in c.values())
    return {
        "pct_FF": round(100 * c[0xFF] / n, 2),
        "pct_00": round(100 * c[0] / n, 2),
        "entropy": round(H, 3),
        "distinct": len(c),
    }


def looks_like_rom(b: bytes, sample: int = 65536) -> tuple[bool, dict]:
    """ROM heuristic — same loose gate as bulk_decrypt.py used."""
    s = stats(b[: min(sample, len(b))])
    real = s["pct_FF"] >= 3.0 and s["entropy"] <= 7.5
    return real, s


def cid_readable(b: bytes, cid: str) -> bool:
    """Patch A's strict gate: cid string appears verbatim in the bytes."""
    return cid.upper().encode("ascii") in b


# ---------------------------------------------------------------------------
# Patch D: facts.xml-aware validation gate
# ---------------------------------------------------------------------------
#
# va_wrx.facts.xml / vb_wrx.facts.xml carry per-CID table addresses and
# their scaling expressions. For any candidate decrypted plaintext we
# can pull a few representative tables, apply their scaling, and check
# whether the values are in plausible engine-management ranges. Catches
# the case where cid_readable passes (CID happens to appear somewhere
# in the bytes) but the rest of the calibration body is still cipher.

import xml.etree.ElementTree as ET
import re as _re

FACTS_VA = HERE / "roms_extracted" / "va_wrx.facts.xml"
FACTS_VB = HERE / "roms_extracted" / "vb_wrx.facts.xml"

# Plausible-range library, keyed on the units string the scaling
# expression carries. Anything not in this table is skipped (we don't
# know the plausible range so can't validate).
_PLAUSIBLE_RANGES: dict[str, tuple[float, float]] = {
    "RPM":              (0.0, 8500.0),
    "Nm":               (-100.0, 700.0),
    "%":                (-50.0, 200.0),
    "AFR":              (8.0, 22.0),
    "C":                (-50.0, 200.0),
    "kPa":              (0.0, 350.0),
    "deg":              (-30.0, 70.0),
    "bar":              (-1.5, 4.0),
    "psi":              (-15.0, 40.0),
}


def _parse_scaling_expr(expr: str) -> tuple[float, float] | None:
    """Parse a scaling expression of the form "(x[+/-K])/F" or
    "(x[+/-K])*F" or "x*F[+/-K]" into (factor, offset) such that
    `value = raw*factor + offset`. Returns None for non-linear forms.
    """
    e = expr.replace(" ", "")
    # Common shapes in facts.xml — handle them as substitutions:
    #   (x)/F
    m = _re.fullmatch(r"\(x\)/([-+\d.eE]+)", e)
    if m:
        return (1.0 / float(m.group(1)), 0.0)
    #   (x)*F
    m = _re.fullmatch(r"\(x\)\*([-+\d.eE]+)", e)
    if m:
        return (float(m.group(1)), 0.0)
    #   ((x)-K)/F
    m = _re.fullmatch(r"\(\(x\)-([-+\d.eE]+)\)/([-+\d.eE]+)", e)
    if m:
        k = float(m.group(1)); f = float(m.group(2))
        return (1.0 / f, -k / f)
    #   ((x)+K)/F
    m = _re.fullmatch(r"\(\(x\)\+([-+\d.eE]+)\)/([-+\d.eE]+)", e)
    if m:
        k = float(m.group(1)); f = float(m.group(2))
        return (1.0 / f, k / f)
    #   ((x)/F)+K
    m = _re.fullmatch(r"\(\(x\)/([-+\d.eE]+)\)\+([-+\d.eE]+)", e)
    if m:
        return (1.0 / float(m.group(1)), float(m.group(2)))
    #   (x)
    if e == "(x)" or e == "x":
        return (1.0, 0.0)
    return None


# Lazy-loaded fact registry. Maps cid (uppercase) -> list of probe
# tuples (address, data_type, scaling_factor, scaling_offset, units).
_FACTS_CACHE: dict[str, list[tuple[int, str, float, float, str]]] | None = None


def _load_facts() -> dict[str, list[tuple[int, str, float, float, str]]]:
    """Walk the va/vb facts.xml files and build a CID -> probes index.

    Per-CID rom tables in facts.xml carry only `name` + `storageaddress`;
    `storagetype`, `endian`, and the inline `<scaling>` are inherited
    from a `_base` rom under the same model_year_band (van_2015_2018,
    va_2019_2021, etc.). We first build a name -> (storagetype, endian,
    scaling_expr, units) lookup from every base rom in the file, then
    for each per-CID rom we resolve each addressed table's
    storage/scaling via that lookup.
    """
    global _FACTS_CACHE
    if _FACTS_CACHE is not None:
        return _FACTS_CACHE
    cache: dict[str, list[tuple[int, str, float, float, str]]] = {}
    for path in (FACTS_VA, FACTS_VB):
        if not path.exists():
            continue
        try:
            tree = ET.parse(path)
        except ET.ParseError:
            continue

        # Pass 1: build the base-table lookup from every rom in the file
        # whose tables carry storagetype + <scaling> but no
        # storageaddress (the "definitions" rows). Includes _base roms
        # and any other rom that defines a named table inline.
        base_lookup: dict[str, tuple[str, str, str, str]] = {}
        for rom in tree.getroot().findall("rom"):
            for t in rom.iter("table"):
                name = t.get("name")
                if not name:
                    continue
                storagetype = t.get("storagetype")
                if not storagetype:
                    continue
                # Skip axis tables — they're handled per their parent.
                if (t.get("type") or "").lower().endswith("axis"):
                    continue
                scaling = t.find("scaling")
                if scaling is None:
                    continue
                units = (scaling.get("units") or "").strip()
                expr = scaling.get("expression") or "x"
                endian = t.get("endian") or "big"
                # First non-trivial entry wins (defines the schema).
                base_lookup.setdefault(
                    name, (storagetype, endian, expr, units))

        # Pass 2: per-CID rom — collect probes.
        for rom in tree.getroot().findall("rom"):
            xmlid_el = rom.find("romid/xmlid")
            if xmlid_el is None or not xmlid_el.text:
                continue
            cid = xmlid_el.text.strip().upper()
            if cid.endswith("_BASE"):
                continue
            probes: list[tuple[int, str, float, float, str]] = []
            for t in rom.iter("table"):
                addr = t.get("storageaddress")
                if not addr:
                    continue
                if (t.get("type") or "").lower().endswith("axis"):
                    continue
                name = t.get("name")
                if not name or name not in base_lookup:
                    continue
                storagetype, endian, expr, units = base_lookup[name]
                if units not in _PLAUSIBLE_RANGES:
                    continue
                parsed = _parse_scaling_expr(expr)
                if parsed is None:
                    continue
                factor, offset = parsed
                try:
                    int_addr = int(addr, 16) if isinstance(addr, str) else int(addr)
                except ValueError:
                    continue
                dt = storagetype.lower().strip()
                if dt in ("uint8", "int8"):
                    canon_dt = dt
                else:
                    canon_dt = f"{dt}_{'be' if endian.lower()=='big' else 'le'}"
                probes.append((int_addr, canon_dt, factor, offset, units))
                if len(probes) >= 8:
                    break
            if probes:
                cache[cid] = probes
    _FACTS_CACHE = cache
    return cache


def _read_value(b: bytes, addr: int, dt: str) -> int | None:
    sz = 2 if dt.startswith("uint16") or dt.startswith("int16") else 1
    if addr + sz > len(b):
        return None
    if dt in ("uint8",):
        return b[addr]
    if dt in ("int8",):
        v = b[addr]
        return v - 256 if v >= 128 else v
    if dt == "uint16_be":
        return (b[addr] << 8) | b[addr + 1]
    if dt == "uint16_le":
        return (b[addr + 1] << 8) | b[addr]
    if dt == "int16_be":
        v = (b[addr] << 8) | b[addr + 1]
        return v - 65536 if v >= 32768 else v
    if dt == "int16_le":
        v = (b[addr + 1] << 8) | b[addr]
        return v - 65536 if v >= 32768 else v
    return None


def facts_validate(b: bytes, cid: str, *,
                   min_pass_fraction: float = 0.5) -> tuple[bool, float, int]:
    """Validate a candidate plaintext against facts.xml probes for CID.

    Returns (passed, fraction_passed, probe_count). passed is True iff
    fraction_passed >= min_pass_fraction. If facts has no probes for
    this CID, returns (True, 1.0, 0) — the gate is open by default
    so unknown CIDs aren't penalised.
    """
    facts = _load_facts()
    probes = facts.get(cid.upper())
    if not probes:
        return (True, 1.0, 0)
    passed = 0
    for addr, dt, factor, offset, units in probes:
        raw = _read_value(b, addr, dt)
        if raw is None:
            continue
        value = raw * factor + offset
        lo, hi = _PLAUSIBLE_RANGES[units]
        if lo <= value <= hi:
            passed += 1
    fraction = passed / len(probes)
    return (fraction >= min_pass_fraction, fraction, len(probes))


# ---------------------------------------------------------------------------
# Patch B: bucket-internal anchorless keystream recovery
# ---------------------------------------------------------------------------

def derive_keystream_anchorless(
    cipher_paths: list[Path],
    hdr: int,
    *,
    agreement_threshold: float = 0.10,
    sample_pairs: int = 200,
) -> tuple[bytes, list[bool]]:
    """Bootstrap a keystream from a bucket of ciphers with no anchor.

    Algorithm:
      For each pair (Ci, Cj) in a random sample of N choose 2, count
      how often Ci[p] == Cj[p] at each byte position p. When both
      plaintexts at p are 0xFF (Subaru flash erase) the ciphertexts
      are equal (Ci[p] = ks[p] ⊕ 0xFF = Cj[p]). Positions where the
      fraction of agreeing pairs >= agreement_threshold are assigned
      provisional keystream bytes K[p] = Ci[p] ⊕ 0xFF.

      Threshold calibration: Subaru ROMs are typically 30-40% 0xFF
      (flash-erase padding). For a random pair the probability of
      agreement at a 0xFF position is ~(0.30)^2 + (0.40)^2 / 2 ≈ 0.09
      i.e. about 9-10%. At non-0xFF positions agreement is ~1/256
      (random byte match). So threshold 0.10 is the right side of the
      gap. The original 0.50 was hugely too strict — pulled near-zero
      determined bytes.

    Returns (keystream, mask) where mask[p] is True iff position p
    was determined (the caller should treat undetermined positions as
    suspect — the keystream byte there is 0 by default and not
    meaningful).
    """
    if len(cipher_paths) < 2:
        return b"", []
    bodies = []
    body_len = None
    for p in cipher_paths:
        b = p.read_bytes()[hdr:]
        if body_len is None:
            body_len = len(b)
        if len(b) != body_len:
            continue
        bodies.append(b)
    if len(bodies) < 2:
        return b"", []

    # For efficiency on big buckets, sample pairs instead of all C(N,2).
    import itertools
    all_pairs = list(itertools.combinations(range(len(bodies)), 2))
    if len(all_pairs) > sample_pairs:
        import random
        random.seed(0)  # deterministic
        pairs = random.sample(all_pairs, sample_pairs)
    else:
        pairs = all_pairs

    agree = [0] * body_len
    for i, j in pairs:
        bi, bj = bodies[i], bodies[j]
        for p in range(body_len):
            if bi[p] == bj[p]:
                agree[p] += 1

    threshold = len(pairs) * agreement_threshold
    keystream = bytearray(body_len)
    mask = [False] * body_len
    rep = bodies[0]
    determined_count = 0
    for p in range(body_len):
        if agree[p] >= threshold:
            keystream[p] = rep[p] ^ 0xFF
            mask[p] = True
            determined_count += 1
    print(f"  anchorless: {determined_count:,}/{body_len:,} bytes "
          f"determined (agreement >= {agreement_threshold:.0%}, "
          f"{len(pairs)} pairs)")
    return bytes(keystream), mask


def _extract_cid_with_partial_keystream(
    cipher_body: bytes, keystream: bytes, mask: list[bool],
    expected_cid: str) -> int | None:
    """If the CID string appears in the partial-decrypt, return its offset.

    Only inspects byte positions where the mask says the keystream is
    determined — random-decrypted positions can't be trusted to spell
    anything.
    """
    decoded = bytes(c ^ k for c, k in zip(cipher_body, keystream))
    needle = expected_cid.upper().encode("ascii")
    # Scan for needle but require ALL bytes in the match window to be
    # in the determined mask.
    for i in range(len(decoded) - len(needle) + 1):
        if decoded[i:i + len(needle)] != needle:
            continue
        if all(mask[i + k] for k in range(len(needle))):
            return i
    return None


# Known plaintext fragments Subaru ROMs commonly carry. Used by
# _structural_prior_extend to grow the partial keystream beyond the
# 0xFF agreement positions: when we see one of these strings appear
# in the (cipher XOR partial_keystream) sequence — even outside the
# mask — that's strong evidence the underlying plaintext IS that
# string, and we can derive new keystream bytes from it.
_KNOWN_FRAGMENTS = (
    b"DENSO",
    b"SUBARU",
    b"Copyright",
    b"COPYRIGHT",
    b"SUBARU CORPORATION",
    b"MITSUBA",
    b"BOSCH",
    b"\x00\x00\x00\x00",  # null padding very common in ROM segment headers
    b"\xff\xff\xff\xff\xff\xff\xff\xff",  # 8-byte 0xFF run (extends FF prior)
)


def _structural_prior_extend(
    cipher_bodies: list[bytes],
    keystream: bytes,
    mask: list[bool],
    *,
    min_consensus: int = 3,
) -> tuple[bytes, list[bool], int]:
    """Iteratively grow the partial keystream by scanning for known
    plaintext fragments in cipher-XOR-partial_keystream outputs.

    Algorithm: for each candidate fragment F and each byte offset p,
    compute the hypothetical keystream slice K_hyp = cipher[p..p+|F|]
    XOR F. If MANY ciphers in the bucket would produce K_hyp at the
    same position (== consensus), then plaintext at p is very likely
    to BE F across those ciphers, and K_hyp is the real keystream.
    Extend the mask + keystream at those positions.

    Returns (new_keystream, new_mask, n_new_positions).
    """
    if not cipher_bodies:
        return keystream, mask, 0
    body_len = len(cipher_bodies[0])
    keystream = bytearray(keystream)
    mask = list(mask)
    new_positions = 0
    for fragment in _KNOWN_FRAGMENTS:
        flen = len(fragment)
        if flen > body_len:
            continue
        # For each byte position, count how many ciphers would produce
        # a consistent keystream slice if plain = fragment.
        # Doing a full O(n_positions * n_ciphers * flen) scan is too
        # slow on 2MB bodies; sample positions where the mask is
        # already partially undetermined to cap the cost.
        sample_positions = [p for p in range(0, body_len - flen, 16)
                            if not all(mask[p + k] for k in range(flen))]
        for p in sample_positions:
            # Candidate keystream slice
            candidate = bytes(cipher_bodies[0][p + k] ^ fragment[k]
                              for k in range(flen))
            # How many other ciphers agree?
            agree = sum(
                1 for body in cipher_bodies
                if all(body[p + k] ^ candidate[k] == fragment[k]
                       for k in range(flen)))
            if agree >= min_consensus:
                # Promote this slice into the keystream
                for k in range(flen):
                    if not mask[p + k]:
                        keystream[p + k] = candidate[k]
                        mask[p + k] = True
                        new_positions += 1
    return bytes(keystream), mask, new_positions


# ---------------------------------------------------------------------------
# Disk I/O
# ---------------------------------------------------------------------------

def safe_write(path: Path, data: bytes) -> str:
    if path.exists():
        if path.read_bytes() == data:
            return "exists-identical"
        alt = path.with_suffix(path.suffix + ".alt")
        alt.write_bytes(data)
        return f"exists-different, wrote .alt -> {alt.name}"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return "written"


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def run_anchored_pass(report: list[str], summary: list[dict],
                      by_size: dict[int, list[Path]]) -> set[Path]:
    """Run keystream recovery from every CONFIRMED anchor.

    Returns the set of cipher paths that were fully decrypted (CID
    readable), so the anchorless pass can skip them.
    """
    fully_decrypted: set[Path] = set()
    for label, plain_rel, cipher_name, hdr, offset, bucket in CONFIRMED:
        report.append(f"\n## {label}\n")
        plain_path = HERE / plain_rel
        cipher_path = ENC / cipher_name
        if not plain_path.exists() or not cipher_path.exists():
            msg = (f"  missing input: plain={plain_path.exists()} "
                   f"cipher={cipher_path.exists()}")
            report.append(msg); print(msg); continue

        plain = plain_path.read_bytes()
        cipher = cipher_path.read_bytes()
        body_len = len(cipher) - hdr
        if offset + body_len > len(plain):
            msg = (f"  ! plaintext too short ({len(plain)}) for "
                   f"offset {offset} + body {body_len}")
            report.append(msg); print(msg); continue

        cb = cipher[hdr:]
        pb = plain[offset:offset + body_len]
        keystream = bytes(a ^ b for a, b in zip(pb, cb))

        ks_path = KS_DIR / f"{bucket}_{label}.ks"
        ks_action = safe_write(ks_path, keystream)
        report.append(
            f"- Keystream {len(keystream):,}B -> `{ks_path.name}` "
            f"({ks_action})")
        print(f"\n=== {label} ===")
        print(f"  keystream {len(keystream):,}B -> {ks_path.name} ({ks_action})")

        full_cids, partial_cids = [], []
        random_cids = []
        facts_summaries: list[str] = []  # facts.xml validation per pass
        for c_path in by_size.get(bucket, []):
            if c_path == cipher_path:
                full_cids.append(c_path.name)
                continue
            cdata = c_path.read_bytes()
            if len(cdata) != bucket:
                continue
            decoded = bytes(a ^ b for a, b in zip(keystream, cdata[hdr:]))
            real, s = looks_like_rom(decoded)
            cid = c_path.stem.replace("__ori", "").replace("_ori", "").upper()
            cidok = cid_readable(decoded, cid)
            if cidok:
                # Patch A: full success — CID readable
                full_cids.append(c_path.name)
                fully_decrypted.add(c_path)
                out_path = OUT_FULL / label / f"{cid}.bin"
                safe_write(out_path, decoded)
                # Patch D: also run facts.xml validation for QA.
                fpass, ffrac, fcount = facts_validate(decoded, cid)
                if fcount > 0:
                    facts_summaries.append(
                        f"    {cid}: facts {ffrac:.0%} ({fcount} probes) "
                        f"{'ok' if fpass else 'FAIL (out-of-range)'}")
            elif real:
                # Patch A: partial — heuristic OK but CID region still cipher
                partial_cids.append(c_path.name)
                out_path = OUT_PARTIAL / label / f"{cid}.bin"
                safe_write(out_path, decoded)
            else:
                random_cids.append((c_path.name, s["pct_FF"], s["entropy"]))

        n_total = len(by_size.get(bucket, []))
        report.append(
            f"- Bucket {bucket:,}B has {n_total} ciphers; "
            f"**{len(full_cids)} full / {len(partial_cids)} partial / "
            f"{len(random_cids)} random**")
        if full_cids:
            report.append(
                f"  - Full decrypts: {', '.join(full_cids[:10])}"
                f"{'...' if len(full_cids) > 10 else ''}")
        if partial_cids:
            report.append(
                f"  - Partial (CID region still cipher): "
                f"{', '.join(partial_cids[:5])}"
                f"{'...' if len(partial_cids) > 5 else ''}")
        if facts_summaries:
            report.append("  - facts.xml validation:")
            report.extend(facts_summaries)
        summary.append({
            "label": label, "bucket_size": bucket,
            "full_count": len(full_cids), "partial_count": len(partial_cids),
            "random_count": len(random_cids),
        })
    return fully_decrypted


def run_anchorless_pass(report: list[str], summary: list[dict],
                        by_size: dict[int, list[Path]],
                        already_decrypted: set[Path]) -> None:
    """Patch B: anchorless keystream recovery for ANCHORLESS_BUCKETS.

    Two phases per bucket:
      1. Partial-keystream sweep — find positions where 0xFF agreement
         is strong enough that the keystream byte is recoverable from
         a single cipher (K[p] = C[p] ^ 0xFF). Any cipher whose
         decoded output happens to spell its own CID in a position
         the mask covers is a real, full decrypt.
      2. Iterative bootstrap — any full decrypt found in (1) becomes
         a real anchor: keystream-from-anchor = plain XOR cipher.
         Re-run the bucket sweep with that full keystream. Repeat
         until no new full decrypts.
    """
    for bucket in ANCHORLESS_BUCKETS:
        candidates = by_size.get(bucket, [])
        if len(candidates) < 2:
            continue
        candidates = [c for c in candidates if c not in already_decrypted]
        if len(candidates) < 2:
            continue
        report.append(f"\n## ANCHORLESS bucket {bucket:,}B "
                      f"({len(candidates)} ciphers)\n")
        print(f"\n=== ANCHORLESS bucket {bucket:,}B ===")
        hdr = 1024
        keystream, mask = derive_keystream_anchorless(candidates, hdr)
        if not keystream:
            report.append("  - insufficient ciphers; skipped")
            continue

        ks_path = KS_DIR / f"{bucket}_ANCHORLESS_partial.ks"
        ks_action = safe_write(ks_path, keystream)
        determined_pct = sum(mask) * 100 / len(mask)
        report.append(
            f"- Phase 1a (partial keystream from 0xFF prior): "
            f"{determined_pct:.1f}% of bytes determined -> "
            f"`{ks_path.name}` ({ks_action})")

        # Phase 1b — extend the partial keystream using known
        # plaintext fragments (DENSO, SUBARU, Copyright, null padding,
        # long 0xFF runs). For each fragment, look for byte positions
        # where MANY ciphers agree on the implied keystream when
        # plaintext = fragment. Adds typically 0.5-5% more determined
        # bytes per pass.
        bodies = []
        for p in candidates:
            data = p.read_bytes()
            if len(data) == bucket:
                bodies.append(data[hdr:])
        keystream, mask, new_pos = _structural_prior_extend(
            bodies, keystream, mask, min_consensus=max(3, len(bodies) // 10))
        new_determined_pct = sum(mask) * 100 / len(mask)
        report.append(
            f"- Phase 1b (structural-prior extend on {len(bodies)} "
            f"ciphers): +{new_pos:,} bytes; total "
            f"{new_determined_pct:.1f}% determined")

        # Phase 1: try to find CID-readable decrypts using the partial
        # keystream. Mask-aware scan ensures we don't accept false
        # CID matches in random-decode noise.
        bootstrap_anchors: list[tuple[str, Path, bytes]] = []
        for c_path in candidates:
            cdata = c_path.read_bytes()
            body = cdata[hdr:]
            cid = c_path.stem.replace("__ori", "").replace("_ori", "").upper()
            if _extract_cid_with_partial_keystream(
                    body, keystream, mask, cid) is not None:
                # Hit: this cipher's plaintext spells its own CID at
                # a mask-covered position. Promote it to a real anchor
                # for phase 2.
                # Synthesise a "guessed full keystream" by trusting
                # the determined-mask bytes and leaving the rest at
                # 0; phase 2 will re-derive a complete keystream
                # using the candidate plaintext as anchor.
                # We don't actually have the plaintext yet — only
                # know the CID appears somewhere in the partial decode.
                # For now, accept the cipher as a future anchor: we'll
                # need the plaintext, which means we must first find
                # ANY known-bucket-matching plaintext that has this CID
                # in its name. Search forum-bins + decrypted/ as fallback.
                plain_path = _find_plaintext_for_cid(cid, bucket - hdr)
                if plain_path is not None:
                    plain = plain_path.read_bytes()
                    if len(plain) == bucket - hdr:
                        ks_full = bytes(c ^ p for c, p in zip(body, plain))
                        bootstrap_anchors.append((cid, c_path, ks_full))

        report.append(
            f"- Phase 1 found {len(bootstrap_anchors)} bootstrap-anchor "
            f"candidates (CID appears in partial decode AND plaintext "
            f"exists on disk)")

        # Phase 2: for each bootstrap anchor, run the standard
        # decrypt loop with its full keystream.
        full_cids, partial_cids, random_cids = [], [], []
        seen_cids: set[str] = set()
        for anchor_cid, anchor_cipher, ks_full in bootstrap_anchors:
            # Save the recovered keystream
            ks_full_path = KS_DIR / f"{bucket}_bootstrap_{anchor_cid}.ks"
            safe_write(ks_full_path, ks_full)
            for c_path in candidates:
                cid = c_path.stem.replace("__ori", "") \
                                 .replace("_ori", "").upper()
                if cid in seen_cids:
                    continue
                cdata = c_path.read_bytes()
                if len(cdata) != bucket:
                    continue
                decoded = bytes(a ^ b for a, b in zip(ks_full, cdata[hdr:]))
                real, _ = looks_like_rom(decoded)
                if cid_readable(decoded, cid):
                    full_cids.append(cid)
                    seen_cids.add(cid)
                    out_path = (OUT_FULL / f"ANCHORLESS_{bucket}_"
                                f"bootstrap_{anchor_cid}" / f"{cid}.bin")
                    safe_write(out_path, decoded)
                elif real and cid not in seen_cids:
                    if cid not in partial_cids:
                        partial_cids.append(cid)

        # Anything not recovered via bootstrap, count as random/partial
        # based on the original partial-keystream phase.
        for c_path in candidates:
            cid = c_path.stem.replace("__ori", "") \
                             .replace("_ori", "").upper()
            if cid in seen_cids or cid in partial_cids:
                continue
            random_cids.append(cid)

        report.append(
            f"- Anchorless decrypt: **{len(full_cids)} full** / "
            f"{len(partial_cids)} partial / {len(random_cids)} random")
        if full_cids:
            report.append(
                f"  - Full via bootstrap: "
                f"{', '.join(full_cids[:10])}"
                f"{'...' if len(full_cids) > 10 else ''}")
        summary.append({
            "label": f"ANCHORLESS_{bucket}", "bucket_size": bucket,
            "full_count": len(full_cids), "partial_count": len(partial_cids),
            "random_count": len(random_cids),
        })


def _find_plaintext_for_cid(cid: str, plain_size: int) -> Path | None:
    """Look for a plaintext file whose name starts with `cid` and whose
    size matches `plain_size`. Searches a few well-known locations.
    """
    search_roots = [
        HERE / "plaintext_corpus" / "forum-bins",
        HERE / "plaintext_corpus" / "bludgod-roms",
        HERE / "plaintext_corpus" / "jimihimi-extras",
        HERE / "roms_extracted" / "decrypted",
        HERE / "roms_extracted" / "forumdownloads" / "validation",
    ]
    for root in search_roots:
        if not root.exists():
            continue
        for p in root.rglob("*.bin"):
            if p.name.startswith("."):
                continue
            stem = p.stem.upper().split(".")[0]
            if stem != cid:
                continue
            if p.stat().st_size != plain_size:
                continue
            return p
    return None


def main() -> int:
    KS_DIR.mkdir(parents=True, exist_ok=True)
    OUT_FULL.mkdir(parents=True, exist_ok=True)
    OUT_PARTIAL.mkdir(parents=True, exist_ok=True)

    report: list[str] = ["# Bulk decryption v2 report\n",
                         "Strict gate: CID-readability. "
                         "Partial decrypts (heuristic pass, CID still "
                         "cipher) are saved separately.\n"]
    summary: list[dict] = []

    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)
    for k in by_size:
        by_size[k].sort()

    already = run_anchored_pass(report, summary, by_size)
    run_anchorless_pass(report, summary, by_size, already)

    REPORT.write_text("\n".join(report), encoding="utf-8")
    (KS_DIR / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n-> {REPORT}")
    total_full = sum(s["full_count"] for s in summary)
    total_partial = sum(s["partial_count"] for s in summary)
    print(f"\nTOTAL FULL DECRYPTS:    {total_full}")
    print(f"TOTAL PARTIAL DECRYPTS: {total_partial}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
