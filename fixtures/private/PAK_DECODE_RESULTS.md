# OBDTotal FlashWrite ISO decode — results recap (2026-05-19)

Outcome of the .pak-decode → bulk_decrypt_v2 pipeline run after wiring
the OBDTotal Subaru J2534 FlashWrite April-2023 ISO into the
decryption-anchor harvest.

## Inputs

| Source                  | Cost  | Coverage                          |
|-------------------------|-------|-----------------------------------|
| OBDTotal FlashWrite ISO | €30   | 417 .pak files, 2591-row CSV      |
| Subaru TSB 11-199-20R   | free  | Per-pak decryption keys (public)  |
| aalesv/pak-tools (GH)   | free  | unpak.py + RC2/crypt source (C++) |
| FastECU repo (GH)       | free  | Vendor-key reference for 4-word swap-bit cipher |

## Pipeline that emerged

```
.pak file (encrypted container)
  ├── stage 1: unpak.py            → header.csv + body.bid / .sob
  ├── stage 2: rc2decode.exe       → RC2 decrypt with per-pak Keyword from CSV
  ├── stage 3: srec2bin.exe        → (.sob path only — older Denso paks)
  └── stage 4: crypt.exe           → 4-word swap-bit decode, vendor key auto-tried
                                       (Denso CAN / Hitachi / Denso K-line)
output: raw ROM .bin with the CID stamped at its standard offset
```

74 of 74 paks staged got clean ROMs out:

| Vendor key       | Count | Examples                            |
|------------------|-------|-------------------------------------|
| Denso CAN        | 59    | Most older Subaru EJ-era + newer NA |
| Hitachi          | 15    | FA-DIT 2019+ (LF9D/G/L, LHB VB)     |

## Decryption-pipeline yield

Across all 99 CONFIRMED anchors (25 original + 74 PAK_*):

| Bucket            | Cipher count | Full   | Partial | Random |
|-------------------|--------------|--------|---------|--------|
| 1,049,600 (EJ-era 1MB) | 187     | **89** | 0       | 8,700  |
| 1,311,488         | 348          | 24     | 39      | 8,289  |
| 1,573,632         | 168          | 3*     | 498     | 3      |
| 1,573,888 (LF6A)  | 77           | 6      | 17      | 208    |
| 2,098,176 (VA WRX old) | 186     | 49     | 1,594   | 775    |
| 2,622,464 (VA WRX new) | 65      | **5**  | 196     | 124    |
| 4,064,000 (diesel RH850) | 211   | 1      | 209     | 1      |
| 4,195,328 (VB WRX)     | 30      | **1**  | 0       | 29     |

\* anchorless-bootstrap only (no real anchors for this bucket)

**TOTAL: 178 full decrypts, 2553 partial decrypts.** Pre-pipeline starting
state was 20 full — net 8.9× yield improvement this session.

## Critical-target status

User's VA WRX 6MT master CIDs (private):

| CID        | MY    | Status                                            |
|------------|-------|---------------------------------------------------|
| LF75404S   | 2015  | Post-TSB cousin LF75600S decoded (sibling)        |
| LF75404H   | 2016  | Direct: post-TSB CID LF75600H decoded from pak    |
| LF79103P   | 2017  | Locked — per-CID encryption blocks family-share   |
| LF9C102P   | 2018  | Post-TSB cousin LF9C300P decoded                  |
| LF9D012H   | 2019  | Sibling LF9D040H decoded                          |
| LF9G003T   | 2020  | Cousin LF9G100T / LF9G002S decoded                |
| **LF9L000E** | **2021** | ✅ **DECODED + cipher-bucket-unlocked**       |
| LF75600H   | -     | ✅ **DECODED from pak (no cipher in bucket)**     |

User's VB WRX master (18 LHB CIDs): **1 of 18 unlocked** (LHBHB10B00G).
Other 17 require newer FlashWrite snapshots — April-2023 dataset only
covers early VB.

## Empirical findings about EpifanSoft encryption

  1. The cipher is `ciphertext = (plaintext ⊕ stream) ⊕ per_family_xor`.
  2. **Older firmware** (pre-2019): per_family_xor is keyed by ~7-char
     prefix. One anchor unlocks all siblings in the family. Verified
     for LF75300, LF78001, LF9C000, LV9N100, LV9N303, multiple EJ-era
     families.
  3. **Newer FA-DIT firmware** (Hitachi-based, LF9D/G/L, VB LHB):
     per-CID encryption. Each CID requires its own anchor. Verified
     for LF79100 (didn't unlock siblings), LF9L000E, LHBHB10B00G.
  4. The per-family layer is concentrated in the calibration body
     (0x10000-0x60000 of the 2MB ROM). Code/static-table regions
     (0x70000-0x200000) decode cleanly cross-family — this is why
     "partial" counts are high even when "full" counts are low.

## What this means for SubuwuTuner's public-repo work

Per `project_ecutune_terms.md` memory entry: the OBDTotal/Subaru FlashWrite
.pak files carry redistribution restrictions. Decoded plaintexts stay
private (under `fixtures/private/`, all gitignored). Definition metadata
generated from clean RR XMLs (already in repo, generated via defgen) is
the only public output — same as the existing 333 EJ-era packs in
`definitions/`.

## Files added this turn (committed)

  - `fixtures/private/decode_paks.py` — end-to-end pak decoder orchestrator
  - `fixtures/private/bulk_decrypt_v2.py` — +74 PAK_* CONFIRMED anchors
  - `fixtures/private/subaru_pak_metadata.tsv` — full 2,591-row Subaru
    FlashWrite CSV (UTF-8 normalized)
  - `fixtures/private/subaru_anchors_actionable.tsv` — 70-row cross-ref
    of paks whose destination CID matches a cipher in our bucket

## What's still locked

Hardware-direct read (OBDX adapter) is the only realistic path for:

  - LF79103P (user's 2017 master) — no LF79103* pak in any dataset
  - The 17 other VB LHB CIDs — need newer FlashWrite snapshot or
    direct dump from VB hardware (user doesn't own VB)
  - 168 ciphers in the 1,573,632 anchorless bucket — no anchors anywhere
  - 30 of 30 ciphers in the 4,195,328 VB bucket *except* the one
    LHBHB10B00G anchor — same per-CID encryption story

For everything else: 178 cleanly decrypted plaintexts available privately
for RE, comparison, custom-feature derivation, defgen XML cross-validation.

## Per-CID ecuparams gap (2026-05-19)

After landing the 25 new VA/VB WRX packs in `definitions/impreza/`,
ran `tools/defgen/loggergen.py` against `logger_v370.xml` (RomRaider
release 2021-11) to generate per-CID ecuparams overlays. Result:
**0 new ecuparams files** for the 25 new packs.

Root cause: v370 has no FA-DIT VA/VB WRX ecuid coverage. Of 47
ecuids extracted from OBDTotal pak headers (`pak_cid_to_ecuid.tsv`),
only 6 appear in v370 — all non-WRX (Forester DE5/EZ1, etc.). The
FA-DIT VA WRX ecuid prefix `A6XX-BX-XX-07` / `D8XX-BX-XX-07` is
entirely absent from v370.

What's populated on the new packs:
  - `lf75600h.toml`: ecu_part = `A629B07507` (from pak header + bin marker, cross-validated)
  - `lf9l000e.toml`: ecu_part = `D829B07107` (same)
  - 23 other new packs: ecu_part = `""` (no decoded bin or no pak header for those CIDs)

Future unlock path: a newer-than-v370 RomRaider logger XML release
that adds FA-DIT VA/VB ecuids. When one lands, re-run loggergen
against `definitions/impreza/` — the 2 already-populated ecu_part
values will pick up overlays automatically, and the remaining 23 can
be populated as their ecuids are read off real hardware (or matched
to a future newer pak dataset).

**2026-05-19 update — v370 is the current ceiling.** Comprehensive
search of public sources confirms no newer logger XML exists:

  - `romraider.com/forum/viewtopic.php?f=8&t=1642` — official thread,
    last edited 2020-11-24, latest version is 370 (Nov 2021 release).
    Cumulative changelog of v1–v370 inline in OP. Stops at FA-DIT
    early-2018 era: A66/A68/A6A/A62A904007/A62A907007 et al.
    A629B07507 (LF75600H) and D829B07107 (LF9L000E) are NOT in this
    set despite being valid SSM ecuids — they were never added to RR.
  - `github.com/Merp/SubaruDefs` — community canonical defs repo,
    Stable branch logger.xml is v0.3.5b (2009-10-02), 2,074,495 bytes
    vs our v370's 2,288,834. Far older. Alpha and MerpMod_dev branches
    pin to v290, also older. Merp doesn't track RR logger updates.
  - RomRaider 1.1.0 release (Nov 2025) on GitHub: release notes
    mention "M42 VANOS query group" addition but no FA-DIT VA/VB.
    Logger XML still distributed separately via the forum (= v370).

The 2 ecu_part values we populated will simply remain unused until
either (a) a v371+ release lands, (b) we write our own FA-DIT logger
XML supplement, or (c) a commercial source's data becomes
redistributable. (b) is the SubuwuTuner-distinctive path — see
docs/05 §11 for the broader under-served-coverage thesis.

All 25 packs still ship shared SSM PIDs via the
`includes = ["../pids.toml"]` line in every pack (235 PIDs + 172
switches from v370). The gap is in the per-CID extended-PID layer,
not the standard PID layer.
