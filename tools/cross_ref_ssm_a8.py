#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Recover the byte-to-RAM-address map of a COBB-tuned ECU's polled-DID
response by cross-correlating two captures from the same vehicle:

  1. A `subuwutuner-cli sniff` log containing the active tuner-tool's
     Mode-0x22 polls of the COBB-injected F3xx DIDs (proprietary,
     opaque packing).
  2. A `subuwutuner-cli ssm-a8-poll` log of direct OEM RAM reads via
     SSM Command 0xA8 over ISO-15765 — the canonical, OEM-documented
     read path that returns raw RAM bytes.

Both sessions drive the SAME trajectory order (idle, cruises, accels,
WOT) so the value distributions overlap. They don't share a wall
clock, so alignment is done by sorted-quantile shape: each candidate
series is sorted, binned to N quantiles, and Pearson-r²-compared
against every same-width SSM-A8 series. Pairs with R² ≥ threshold
are candidate mappings.

A featureless-series filter rejects uniformly-distributed bytes
(noise sorted ≈ linear ramp), which would otherwise correlate at
R² ≈ 1.0 with each other and saturate the output with garbage.

Usage:
    python cross_ref_ssm_a8.py --self-test
    python cross_ref_ssm_a8.py \\
        --sniff <FILE.log> --ssm-a8 <FILE.log> \\
        [--out mapping.toml] [--min-r2 0.99] [--top-k 5] [--n-bins 50]

The --self-test mode plants a synthetic RPM mapping and verifies
recovery at R² > 0.99 — useful as a regression check independent of
any real capture data.

Companion docs:
  - docs/29-ssm-a8-poll-workflow.md (workflow / methodology)
  - docs/24-sniff-workflows.md      (sniff capture rig + log format)
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import namedtuple
from pathlib import Path
from statistics import mean

# F3xx response shape — sizes per DID, derived empirically from real
# captures (see docs/29 §3). Total 75 bytes across DIDs F300..F304.
DID_SIZES = {0xF300: 26, 0xF301: 20, 0xF302: 10, 0xF303: 10, 0xF304: 9}

ENCODINGS = ["u8", "s8", "u16_be", "u16_le", "s16_be", "s16_le",
             "f32_be", "f32_le"]

F3xxPoll = namedtuple("F3xxPoll", "t_ms blocks")  # blocks: {did: bytes}
SsmA8Poll = namedtuple("SsmA8Poll", "t_ms values")  # values: list[int|None]


def parse_f3xx_sniff(path: Path) -> list[F3xxPoll]:
    """Parse a `subuwutuner-cli sniff` log into per-poll F3xx blocks.

    Recognizes ECU response frames on 0x7E8 carrying a positive Mode
    0x22 response (`62 F3.. <data> F3.. <data> ...`). Multi-frame
    ISO-TP reassembly is already done by the OBDX adapter; the sniff
    log carries the reassembled hex bytes on the First-Frame line.
    """
    polls: list[F3xxPoll] = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        # Reassembled ISO-TP: <ms>  0x7E8  10  <ll>  62  F3..  ...
        # The OBDX strip leaves the original First-Frame PCI byte
        # (0x10..) and length byte in the data.
        if len(parts) < 8 or parts[1] != "0x7E8" or not parts[2].startswith("1") \
                or parts[4] != "62":
            continue
        try:
            body = bytes.fromhex("".join(parts[5:]))
        except ValueError:
            continue
        blocks: dict[int, bytes] = {}
        i = 0
        while i + 2 <= len(body):
            did = (body[i] << 8) | body[i + 1]
            if did not in DID_SIZES:
                break
            sz = DID_SIZES[did]
            blocks[did] = body[i + 2:i + 2 + sz]
            i += 2 + sz
        if blocks:
            polls.append(F3xxPoll(t_ms=int(parts[0]), blocks=blocks))
    return polls


def parse_ssm_a8_log(path: Path) -> tuple[list[int], list[SsmA8Poll]]:
    """Parse a `subuwutuner-cli ssm-a8-poll` log.

    Returns (addresses, polls). Addresses are 24-bit ints in request
    order; each SsmA8Poll.values has one byte per address (None for
    addresses where the ECU returned an error mid-stream).

    The log format (per cmd_ssm_a8_poll in src/cli/main.cpp):

        # subuwutuner ssm-a8-poll log v1
        # transport: obdx
        # framing: ssm-a8 over iso15765 (tester 0x7E0, ecu 0x7E8, 500 kbps)
        # interval_ms: 333
        # addresses(BE24): 0x00000E 0x00000F 0x000008 ...
        # format: <elapsed_ms>\\t<HH HH ...>  ...
        12\\t8A 03 00
        45\\t8B 03 00
        ...
        78\\t! transport timeout    <- inline error rows (! prefix)
        # polled N times, M errors, in T ms
    """
    addresses: list[int] = []
    polls: list[SsmA8Poll] = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.startswith("# addresses(BE24):"):
                tail = line.split(":", 1)[1].strip()
                addresses = [int(tok, 16) for tok in tail.split()]
            continue
        parts = line.split("\t", 1)
        if len(parts) != 2:
            continue
        try:
            t_ms = int(parts[0])
        except ValueError:
            continue
        payload = parts[1].strip()
        if payload.startswith("!"):
            polls.append(SsmA8Poll(t_ms=t_ms, values=[None] * len(addresses)))
            continue
        try:
            bs = [int(tok, 16) for tok in payload.split()]
        except ValueError:
            continue
        if len(bs) != len(addresses):
            # Schema violation — skip rather than guess.
            continue
        polls.append(SsmA8Poll(t_ms=t_ms, values=bs))
    return addresses, polls


def decode_raw(b: bytes, off: int, kind: str):
    """Decode a fixed-width field at offset `off` in `b`. None on
    failure or implausible-magnitude floats."""
    try:
        if kind == "u8":
            return float(b[off])
        if kind == "s8":
            return float(struct.unpack_from(">b", b, off)[0])
        if kind == "u16_be":
            return float(struct.unpack_from(">H", b, off)[0])
        if kind == "u16_le":
            return float(struct.unpack_from("<H", b, off)[0])
        if kind == "s16_be":
            return float(struct.unpack_from(">h", b, off)[0])
        if kind == "s16_le":
            return float(struct.unpack_from("<h", b, off)[0])
        if kind == "f32_be":
            v = struct.unpack_from(">f", b, off)[0]
            return float(v) if -1e7 < v < 1e7 else None
        if kind == "f32_le":
            v = struct.unpack_from("<f", b, off)[0]
            return float(v) if -1e7 < v < 1e7 else None
    except (IndexError, struct.error):
        return None
    return None


def enc_bytes(kind: str) -> int:
    if kind.endswith("8"):
        return 1
    if kind.startswith(("u16", "s16")):
        return 2
    return 4


def pearson_r2(xs: list[float], ys: list[float]) -> float:
    """Coefficient of determination for OLS y = a·x + b. 0..1.
    Returns 0 if either series has no variance or fewer than 5 paired
    samples — keeps the score conservative on starvation cases."""
    n = len(xs)
    if n != len(ys):
        raise ValueError(f"series length mismatch: xs={n}, ys={len(ys)}")
    if n < 5:
        return 0.0
    mx, my = mean(xs), mean(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0.0 or syy == 0.0:
        return 0.0
    return (sxy * sxy) / (sxx * syy)


def quantile_resample(values: list[float], n_bins: int) -> list[float]:
    """Sort `values`, return the n_bins per-quantile means. The
    rank-quantile fallback alignment: two sessions through the same
    trajectory have similar value distributions even if not similar
    timestamps, so resampling each to n_bins quantile-means and
    correlating those is a shape-only comparison robust to pace
    drift. Returns [] if values is empty."""
    cleaned = [v for v in values if v is not None and v == v]  # drop NaN
    if not cleaned:
        return []
    cleaned.sort()
    out = []
    for i in range(n_bins):
        lo = i * len(cleaned) // n_bins
        hi = (i + 1) * len(cleaned) // n_bins
        if hi == lo:
            hi = lo + 1
        chunk = cleaned[lo:hi]
        out.append(sum(chunk) / len(chunk))
    return out


def enumerate_f3xx_series(polls: list[F3xxPoll]) -> dict[tuple[int, int, str], list[float]]:
    """All decodable (did, byte_offset, encoding) series across the
    F3xx capture, keyed by tuple. Skips invalid decodes per-poll
    rather than dropping the whole series."""
    series: dict[tuple[int, int, str], list[float]] = {}
    for did, size in DID_SIZES.items():
        for off in range(size):
            for enc in ENCODINGS:
                if off + enc_bytes(enc) > size:
                    continue
                vals: list[float] = []
                for p in polls:
                    block = p.blocks.get(did)
                    if block is None:
                        continue
                    v = decode_raw(block, off, enc)
                    if v is not None:
                        vals.append(v)
                if len(vals) >= 5:
                    series[(did, off, enc)] = vals
    return series


def enumerate_ssm_a8_series(addresses: list[int],
                             polls: list[SsmA8Poll]) -> dict[tuple[int, str], list[float]]:
    """All decodable (base_address, encoding) series across the SSM-A8
    capture. For multi-byte encodings, walks adjacent --addr slots
    assuming consecutive addresses are stored consecutively in the
    request — which they will be if the user passed `--addr 0x0E
    --addr 0x0F` (or the comma-list `--addr 0x0E,0x0F`) for a u16."""
    series: dict[tuple[int, str], list[float]] = {}
    addr_index = {a: i for i, a in enumerate(addresses)}
    for base in addresses:
        for enc in ENCODINGS:
            nb = enc_bytes(enc)
            slots = [addr_index.get(base + k) for k in range(nb)]
            if any(s is None for s in slots):
                continue
            vals: list[float] = []
            for p in polls:
                bs = bytes()
                ok = True
                for s in slots:
                    v = p.values[s]
                    if v is None:
                        ok = False
                        break
                    bs += bytes([v])
                if not ok:
                    continue
                v = decode_raw(bs, 0, enc)
                if v is not None:
                    vals.append(v)
            if len(vals) >= 5:
                series[(base, enc)] = vals
    return series


Pair = namedtuple("Pair", "did off f_enc base s_enc r2 n")


def _is_featureless(resampled: list[float], threshold: float = 0.995) -> bool:
    """True if `resampled` is suspiciously close to a perfect linear
    ramp from min to max. Such a shape comes from uniformly-distributed
    noise (sorted random bytes look like a ramp regardless of underlying
    signal), which would otherwise correlate >0.99 with anything else
    that's also uniform. Real engine-state signals — RPM, MAF, coolant
    — have plateaus around idle and acceleration phases that make their
    quantile shapes distinctly non-linear, so this filter doesn't
    discard real candidates."""
    n = len(resampled)
    if n < 5:
        return True
    lo, hi = resampled[0], resampled[-1]
    if hi == lo:
        return True
    ramp = [lo + (hi - lo) * i / (n - 1) for i in range(n)]
    return pearson_r2(resampled, ramp) > threshold


def correlate_rank_quantile(
    f_series: dict[tuple[int, int, str], list[float]],
    s_series: dict[tuple[int, str], list[float]],
    n_bins: int = 50,
    reject_featureless: bool = True,
) -> list[Pair]:
    """All-pairs rank-quantile correlation. Returns Pairs unsorted —
    the caller picks top-K per F3xx slot and applies the min-R² filter.
    Encoding pairs need not match (a u16_be F3xx byte may correspond
    to a u16_le RAM read after the tuner's per-monitor transform); we
    still only compare same-width encodings because cross-width
    correlation is geometric nonsense.

    `reject_featureless` drops series whose sorted-resample is too
    close to a pure linear ramp — i.e., near-uniform noise. Without
    this, any two noise bytes correlate at R² ≈ 1.0 because their
    sorted shapes coincide. Real engine signals survive the filter;
    pure noise doesn't."""
    f_resampled = {k: quantile_resample(v, n_bins) for k, v in f_series.items()}
    s_resampled = {k: quantile_resample(v, n_bins) for k, v in s_series.items()}
    if reject_featureless:
        f_resampled = {k: v for k, v in f_resampled.items()
                       if not _is_featureless(v)}
        s_resampled = {k: v for k, v in s_resampled.items()
                       if not _is_featureless(v)}
    pairs: list[Pair] = []
    for (did, off, f_enc), fv in f_resampled.items():
        if not fv:
            continue
        f_nb = enc_bytes(f_enc)
        for (base, s_enc), sv in s_resampled.items():
            if enc_bytes(s_enc) != f_nb:
                continue
            if not sv or len(sv) != len(fv):
                continue
            r2 = pearson_r2(fv, sv)
            pairs.append(Pair(did, off, f_enc, base, s_enc, r2, n_bins))
    return pairs


def write_mapping_toml(out_path: Path, pairs: list[Pair], min_r2: float,
                       top_k: int) -> int:
    """Group pairs by F3xx slot (did, off), keep top-K by R², filter
    on min_r2. Returns the number of accepted mappings."""
    by_slot: dict[tuple[int, int, str], list[Pair]] = {}
    for p in pairs:
        by_slot.setdefault((p.did, p.off, p.f_enc), []).append(p)
    accepted = 0
    lines: list[str] = [
        "# cross_ref_ssm_a8 output — F3xx byte position -> SSM-A8 RAM address",
        "# columns: did, byte_offset, f3xx_encoding, ssm_a8_base, ssm_a8_encoding, r_squared",
        f"# min_r2 = {min_r2}, top_k = {top_k}",
        "",
    ]
    for slot, slot_pairs in sorted(by_slot.items()):
        slot_pairs.sort(key=lambda p: p.r2, reverse=True)
        kept = [p for p in slot_pairs if p.r2 >= min_r2][:top_k]
        if not kept:
            continue
        accepted += 1
        did, off, f_enc = slot
        lines.append("[[mapping]]")
        lines.append(f"did = 0x{did:04X}")
        lines.append(f"byte_offset = {off}")
        lines.append(f"f3xx_encoding = \"{f_enc}\"")
        lines.append("candidates = [")
        for p in kept:
            lines.append(
                f"  {{ ssm_a8_base = 0x{p.base:06X}, "
                f"ssm_a8_encoding = \"{p.s_enc}\", "
                f"r_squared = {p.r2:.6f} }},"
            )
        lines.append("]")
        lines.append("")
    out_path.write_text("\n".join(lines))
    return accepted


# -------------------- self-test --------------------

def _self_test() -> int:
    """Synthetic mapping recovery. Plant: an F301:6 u16_be RPM-shaped
    field is a linear function of an SSM-A8 u16_be at 0x00000E (the
    canonical Subaru OEM RPM address). Drive a synthetic mixed-
    trajectory (idle / cruise / WOT) through both captures. Correlator
    must surface the planted pair at R² > 0.99 in the top-1 position."""
    import random
    random.seed(0xC0BB)

    # Simulated trajectory: 4 idle blocks + 4 cruise blocks + 1 WOT.
    rpm_trajectory: list[int] = []
    for _ in range(4):
        rpm_trajectory.extend(int(815 + random.gauss(0, 15)) for _ in range(60))
    for cruise_rpm in (1800, 2100, 2400, 2700):
        rpm_trajectory.extend(int(cruise_rpm + random.gauss(0, 30)) for _ in range(60))
    rpm_trajectory.extend(int(2300 + (6000 - 2300) * i / 60 + random.gauss(0, 25))
                          for i in range(60))

    # F3xx capture: F301 holds RPM at bytes 6/7 u16_be, with a
    # representative non-canonical scale (matching the empirical
    # finding that tuner-pack response bytes use derived scales).
    f3xx_polls: list[F3xxPoll] = []
    for t, rpm in enumerate(rpm_trajectory):
        raw_f3xx = max(0, min(0xFFFF, int((rpm + 130.37) / 0.21246)))
        f301 = bytearray(DID_SIZES[0xF301])
        for i in range(len(f301)):
            f301[i] = random.randint(0, 255)
        struct.pack_into(">H", f301, 6, raw_f3xx)
        blocks = {0xF301: bytes(f301)}
        for did, sz in DID_SIZES.items():
            if did == 0xF301:
                continue
            blocks[did] = bytes(random.randint(0, 255) for _ in range(sz))
        f3xx_polls.append(F3xxPoll(t_ms=t * 333, blocks=blocks))

    # SSM-A8 capture: a SECOND session that drove a similar shape but
    # with jittered duration. RAM 0x00000E/0x00000F holds canonical
    # OEM RPM/4 — tests that the correlator handles cross-encoding
    # transforms (raw bytes differ between sessions, sorted-quantile
    # shape still matches).
    addresses = [0x000008, 0x000009, 0x00000E, 0x00000F, 0x00FE7, 0x00FE8]
    s_polls: list[SsmA8Poll] = []
    s_trajectory = []
    for v in rpm_trajectory:
        s_trajectory.append(int(v + random.gauss(0, 5)))
        if random.random() < 0.1:
            s_trajectory.append(int(v + random.gauss(0, 5)))
    for t, rpm in enumerate(s_trajectory):
        raw_ssm = max(0, min(0xFFFF, rpm * 4))
        hi = (raw_ssm >> 8) & 0xFF
        lo = raw_ssm & 0xFF
        values = [
            random.randint(50, 95),
            random.randint(0, 255),
            hi, lo,
            random.randint(0, 255),
            random.randint(0, 255),
        ]
        s_polls.append(SsmA8Poll(t_ms=t * 333, values=values))

    f_series = enumerate_f3xx_series(f3xx_polls)
    s_series = enumerate_ssm_a8_series(addresses, s_polls)
    print(f"  f3xx series candidates: {len(f_series)}")
    print(f"  ssm-a8 series candidates: {len(s_series)}")

    pairs = correlate_rank_quantile(f_series, s_series, n_bins=50)

    planted = [p for p in pairs
               if p.did == 0xF301 and p.off == 6 and p.f_enc == "u16_be"]
    planted.sort(key=lambda p: p.r2, reverse=True)
    if not planted:
        print("  FAIL: planted slot produced no candidates", file=sys.stderr)
        return 1
    top = planted[0]
    print(f"  top match for F301:6 u16_be -> "
          f"0x{top.base:06X} {top.s_enc} R^2={top.r2:.6f}")
    if top.base != 0x00000E or top.s_enc != "u16_be":
        print(f"  FAIL: top match should be 0x00000E u16_be, "
              f"got 0x{top.base:06X} {top.s_enc}", file=sys.stderr)
        return 1
    if top.r2 < 0.99:
        print(f"  FAIL: R^2 {top.r2:.6f} below 0.99 threshold", file=sys.stderr)
        return 1

    noise_pairs = [p for p in pairs
                   if p.did == 0xF300 and p.off == 1 and p.r2 > 0.99]
    if noise_pairs:
        print(f"  WARN: noise byte F300:1 has {len(noise_pairs)} spurious "
              f">0.99 hits (rank-quantile false positive)", file=sys.stderr)
    print("  OK")
    return 0


# -------------------- CLI --------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Cross-correlate F3xx DID polls vs SSM-A8 RAM reads")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic mapping recovery; exit 0 on success")
    ap.add_argument("--sniff", type=Path, default=None,
                    help="path to a subuwutuner-cli sniff log carrying F3xx "
                         "responses")
    ap.add_argument("--ssm-a8", type=Path, default=None,
                    help="path to a subuwutuner-cli ssm-a8-poll log")
    ap.add_argument("--out", type=Path, default=Path("mapping_ssm_a8.toml"),
                    help="output mapping TOML path "
                         "(default: ./mapping_ssm_a8.toml)")
    ap.add_argument("--min-r2", type=float, default=0.99,
                    help="minimum R² to accept (default 0.99)")
    ap.add_argument("--top-k", type=int, default=5,
                    help="top-K candidates per F3xx slot to emit (default 5)")
    ap.add_argument("--n-bins", type=int, default=50,
                    help="rank-quantile bin count for shape alignment "
                         "(default 50)")
    ap.add_argument("--no-reject-featureless", action="store_true",
                    help="disable the featureless-series filter (debug only; "
                         "the filter knocks out uniform-noise bytes that "
                         "would otherwise spuriously correlate at R² ~1)")
    args = ap.parse_args()

    if args.self_test:
        print("self-test: synthetic mapping recovery")
        return _self_test()

    if args.ssm_a8 is None or args.sniff is None:
        print("error: --sniff and --ssm-a8 are both required "
              "(or use --self-test)", file=sys.stderr)
        return 2

    print(f"parsing sniff: {args.sniff}")
    f3xx_polls = parse_f3xx_sniff(args.sniff)
    print(f"  {len(f3xx_polls)} F3xx responses")

    print(f"parsing ssm-a8: {args.ssm_a8}")
    addresses, s_polls = parse_ssm_a8_log(args.ssm_a8)
    print(f"  {len(addresses)} addresses, {len(s_polls)} polls")

    f_series = enumerate_f3xx_series(f3xx_polls)
    s_series = enumerate_ssm_a8_series(addresses, s_polls)
    print(f"  f3xx series: {len(f_series)}, ssm-a8 series: {len(s_series)}")

    print(f"correlating (rank-quantile, n_bins={args.n_bins})")
    pairs = correlate_rank_quantile(
        f_series, s_series, n_bins=args.n_bins,
        reject_featureless=not args.no_reject_featureless)
    print(f"  {len(pairs)} candidate pairs total")

    accepted = write_mapping_toml(args.out, pairs, args.min_r2, args.top_k)
    print(f"wrote {accepted} mappings (R^2 >= {args.min_r2}) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
