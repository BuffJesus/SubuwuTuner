# Subaru FlashWrite .pak decryption metadata

Extracted 2026-05-19 from Subaru TSB **11-199-20R rev. 02/04/21** ("Reprogramming
File Availability for Startability and Idle Stability at Low Temperatures —
2014-18 Forester DIT, 2015-20 WRX DIT"), hosted publicly on NHTSA at
<https://static.nhtsa.gov/odi/tsbs/2021/MC-10188164-0001.pdf>.

These are **public Subaru-published facts.** The TSB is a Subaru Service Bulletin
posted in the US government's National Highway Traffic Safety Administration
database; the data here is verbatim from that document, which is publicly
accessible without any login, paywall, or terms-of-service barrier.

## What this gives us

For every TSB-update covered, we have:

- **PAK file name** — the FlashWrite reprogramming file Subaru ships to dealers
- **New ECM part number** — the part-number version after the TSB update
- **Old ECM part numbers** — the predecessor versions superseded by the TSB
- **Decryption keyword** — the 4-byte password used to decrypt the .pk2 payload
  inside the .pak container (FlashWrite's per-file encryption key)
- **New ECM CID number** — the SubuwuTuner-format 8-char CID stamped into the
  ROM after reprogramming

The decryption keyword + .pak file gives us the raw decrypted ROM bytes, which
is the keystream anchor we want for the EpifanSoft bucket.

## VA WRX 6MT — full coverage 2015-2020 from this TSB

| MY | CID | PAK filename | Decryption | User's master? |
|---|---|---|---|---|
| 2015 | **LF75404S** | 22765AR961.pak | `9B7C74CA` | yes (actionable) |
| 2016 | **LF75404H** | 22765AH616.pak | `2A46DCFD` | yes (actionable) |
| 2017 | **LF79103P** | 22765AK385.pak | `B8B0D83C` | yes (locked) |
| 2018 | **LF9C102P** | 22765AL004.pak | `8C1EEE9C` | yes (locked) |
| 2019 | **LF9D012H** | 22765AM553.pak | `75ED5C0F` | yes (actionable) |
| 2020 | **LF9G003T** | 22765AN623.pak | `91D4548C` | yes (actionable) |

Coverage gaps in user's master (NOT on this TSB):
- **LF75600H** — likely a separate TSB (not the cold-start one)
- **LF9L000E** — likely 2021 6MT, separate TSB (this TSB stops at 2020)

## Where to get the .pak files

Three known channels, in increasing preference for the project's posture:

**1. Subaru of America directly (cleanest, $75)**
- Service Technical Information Company (STIC): 1-866-428-2278
- CD-ROM with all FlashWrite reprogramming files, $75 + S/H, quarterly updates
- Per <https://subaru.oemdtc.com/440/j2534-reprogramming-files-chart-subaru>

**2. OBDTotal commercial reseller (€29.99, instant email delivery)**
- <https://obdtotal.com/product/subaru-j2534-flashwrite-reprogramming-data/>
- April 2023 dataset; contains 22765-prefix files matching all of the user's
  master CIDs (or close-revision siblings)
- Same legal-shape question as ECUTune: private use likely fine, public
  redistribution blocked

**3. STIS subscription (Subaru's official dealer portal)**
- Dealer access required; cost varies
- Direct download access to all current pak files

Less-preferred / unverified:
- VXDIAG FlashWrite2 (Chinese clone tool that bundles the data)
- Forum-shared .pak files (NASIOC, RomRaider, MHH Auto — provenance varies)

## How to decode .pak

The .pak format is a Subaru-proprietary container:

1. **Outer wrapper** with header + per-file metadata
2. **Inner .pk2 stream** encrypted with the decryption keyword (4-byte AES key
   fragment / proprietary cipher — needs community RE)
3. **Decrypted payload** = raw ROM bytes (.bin equivalent)

Public community implementations of the .pak/.pk2 decoder exist in the
RomRaider/EcuFlash ecosystem; a focused search of `github.com` for
`subaru flashwrite pak decode` or similar typically surfaces working code.
Once decoded, the raw .bin is what we need as a keystream anchor.

## What this changes for the decryption pipeline

With these files in hand and decoded, we'd have **clean stock anchors for all 6
2015-2020 VA WRX 6MT CIDs** — covering 6 of the 8 CIDs in the user's private
master. The remaining 2 (LF75600H, LF9L000E) would need either separate TSB
lookups or hardware-direct reads.

For the 2,098,176B cipher bucket: 6 fresh anchors. Family-share testing across
these would resolve whether each per-year CID is a single-CID lock or shares
keystream across its 6-char prefix (LF754, LF791, LF9C1, LF9D0, LF9G0).
Empirically expected (based on the LF79100 finding earlier today): each is
likely a per-CID lock for newer firmware generations.

## Provenance posture

This file IS publishable to the public repo — it's a verbatim extract of a
Subaru bulletin posted in NHTSA's public database. The decryption keywords
are not protected access controls; they're documented in the bulletin so
dealers can use generic J2534 tools. No §1201 surface, no commercial-tool
EULA. Definitions metadata generated from .pak-derived ROMs would still need
the standard provenance check (preferred: defgen from a clean RR XML, not
from the .pak ROM directly).
