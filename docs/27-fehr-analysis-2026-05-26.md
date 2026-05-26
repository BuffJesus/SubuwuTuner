# Fehr e-tune calibration analysis (2026-05-26)

## Scope

Records the structural findings from the 2026-05-26 byte-level diff
between a factory-virgin 2017 LF79102P ROM and a live LF79101P dump
captured the same morning with the user's Fehr e-tune active. The
diff was produced analyst-side from the SubuwuTuner read pipeline's
2 MB output; the SubuwuTuner-side outcome is this document plus the
in-tree SA additions in commit `ca20b7f` (`--sa-variant fehr-active`).

This is a *findings* document — not an algorithm specification — and
captures the state of knowledge at the date in the filename. Specific
constants and bytes that survive long-term live in the relevant module
headers; transient analysis artifacts live off-tree under the user's
analyst-side `Findings/calibration-deltas/` directory.

## Inputs

| Artifact | Size | Pairing token | State |
|---|---|---|---|
| `2017-wrx-stock.bin` | 2 MB | `0xFFFFFFFF` (factory virgin) | LF79102P, no tune |
| `fehr-tune-plaintext.bin` | 2 MB | `0x64114A47` (Fehr-active) | LF79101P, 99.6% coverage from B6-decrypted install captures |
| `fehr-full-dump.bin` | 2 MB | `0x64114A47` | LF79101P, 100% coverage from live RMBA dump |

The full live dump and the 99.6%-coverage analyst reference match
byte-for-byte at every non-`0xFF` position (2,089,539 / 2,089,539).
The remaining 7,613 bytes are install-capture gaps that the live
dump fills.

## Diff headline

- 67,129 differing bytes (~3.2% of 2 MB), in 4,370 precise regions /
  520 after coalescing gaps ≤ 16 bytes.
- 22,173 are *new* (stock `0xFF` → Fehr data).
- 44,648 are *modified* (both had data, value changed).
- 308 are *erased* (stock had data, Fehr now `0xFF`) — mostly in
  knock-thresholds / ignition-timing adjacent regions, consistent
  with the tune's aggressive posture.
- **Bootloader (`0x000000..0x006000`) has zero differences.** Fehr's
  tune does not patch the bootloader — relevant input for the
  brick-protection model in `docs/05-improvements.md` §4.

## SecurityAccess constants

| Flash addr | Item | Stock | Fehr | In-tree status |
|---|---|---|---|---|
| `0x074338` | L1 round keys (16 × u16 BE) | factory | Fehr-modified | **shipped** as `kSaTableL1Fehr` in `src/ecu/src/subaru_security.cpp` |
| `0x074358` | L35 round keys (16 × u16 BE) | factory | Fehr-modified | **NOT shipped** — see "L35 status" below |
| `0x074378` | S-box (32 B) | factory | identical to factory | already in-tree as `kSBox` |
| `0x074398` | B6 cipher round keys | factory | identical to factory | already in-tree (gated on `ST_ENABLE_BULK_REFLASH_CIPHER`) |

The L1 key function lives at `st::ecu::subaru::ssmcan1_l1_fehr_active`
and is selectable via `subuwutuner-cli rom-pull --sa-variant fehr-active`.
The forward-Feistel + final-wordswap direction was validated 1/1
against the captured `cobb-uninstall-3 L1` pair (offline) and
live-validated against the user's ECU pairing token at `0x001FFFB0`
returning `64 11 4A 47` exactly.

### L35 status

The L35 round keys at `0x074358` are *byte-different* from factory
but, when plugged into the same Feistel structure that works for L1,
do **not** reproduce the captured Fehr-active L3 pair
(`0x4ADFFE07 → 0x24243A06`) under any tested direction or
permutation. The bytes at this address may be decoys; the live L35
cipher is most likely inside one of Fehr's injected SH-2A code
regions (see below), reading constants from an embedded literal
pool rather than the conventional flash slot.

Until the live L35 algorithm is recovered by full SH-2A static
analysis of the injected handler, no L35 variant ships in-tree.
Promoting the bytes-as-they-are would burn SA attempts to no
purpose.

## Patch manifests at `0x007C00` and `0x008000`

Fehr's tune ships two patch-manifest tables — 8-byte records, one
per modified region — that the installer uses to know what to
restore on uninstall. Same structure for both:

```
offset 0..3 : target_address  (u32 BE)
offset 4    : type            (u8 — 0x10 normal cal patch, 0x30 = "verify-only" / "fill-with-zero" candidate)
offset 5    : op              (u8 — 0x01 byte / 0x02 u16 / 0x04 u32 element width)
offset 6..7 : length_bytes    (u16 BE — exact size of patched region)
```

| Table | Range | Records | Coverage |
|---|---|---|---|
| Manifest A | `0x007C00..0x007CE8` | 29 (null-terminated) | The big throttle / boost / fuel / AVCS tables |
| Manifest B | `0x008000..0x0085A8` | 180 (null-terminated at 0x0085A0) | Early-cal scalars in `0x0001B5XX..0x0001B6XX`, plus the pairing-token and `W585` tag writes |

Two independent cross-checks confirm the format:

- Manifest A record at `0x007C28` → addr `0x031868`, length 608
  matches `Airflow - Turbo - Boost - Boost Target Main`
  (16×19 u16) in the in-tree definition pack.
- Manifest A record at `0x007C30` → addr `0x036766`, length 512
  matches `Fuel - Closed Loop - Target - Low EGR - Target Base
  (TGV Open)` (16×16 u16).

The manifests are the data structure a clean-uninstall feature
would key off of: knowing every (address, length) the tune
modifies removes the need to keep a copy of the stock ROM around
for revert. The missing half is the reverse-lookup (factory byte
values for each manifest record) — Fehr's installer presumably
embeds these or fetches them from the AP, but the SubuwuTuner
side could resolve them by reading the same offsets from any
known-stock dump of the same CID family.

## Injected SH-2A code

The diff includes 16 candidate SH-2A function starts in regions
that were `0xFF` in stock and contain code-shaped bytes in Fehr.
Total injected-code surface area is roughly 14 KB across
`0x008600..0x00D618`. The two largest contiguous code regions:

- `0x0089B8..0x009094` (1,756 B) — single function with full
  register-save prologue (`2FE6 2FD6 2FC6 2FB6 2FA6 2F96 2F86 4F22`)
- `0x00C8D8..0x00D618` (3,392 B, mostly FF→data) — same prologue
  pattern at start

Beyond the two leading SA-handler candidates (`0x0089B8` and
`0x00C8D8`), no per-function semantics are recovered yet. A real
call-graph requires a full SH-2A static analyzer pass, which is
queued analyst-side. SubuwuTuner does not need this for read
operations — the L1 SA path is already live-validated through
the in-tree variant — but it gates the L35 algorithm recovery
and any future tune-state-aware behaviour beyond L1.

## Calibration surfaces touched

The diff hits every classical e-tune surface. From the in-tree
TOML pack's labeling pass (`definitions/impreza/lf79103p.toml`,
124 / 520 regions labeled, ~46% of differing bytes inside known
tables):

- Throttle: Target Throttle (Main + Alternate, TGV Open + Closed,
  Group 1 + 2) — 16 tables. Same monotonic 16-bit ramp written into
  all of them.
- Throttle: Requested Torque (B variants, including Group 2
  redundancy) — 4 tables, ~838 bytes each.
- Boost: Boost Target Main (608 B), Wastegate Duty Initial/Maximum
  (476 B each).
- MAF: Volumetric Efficiency Correction (530 B), Sensor Scale.
- Fuel: Closed Loop Target Low/High EGR (TGV Open + Closed) — 4 ×
  512 B. Open Loop AVCS Enabled/Disabled — 4 × ~480 B.
- AVCS: Intake + Exhaust cam targets under all baro multipliers,
  with Aggressive variants — 200–600 B each.
- Ignition: Primary AVCS Enabled TGV Open (334 B), Dynamic Advance
  Base + Adder (315 / 300 B), IAT Compensation, Knock Thresholds
  for all 4 cylinders.
- Rev Limits: Rev Limit Fuel Cut A/B/C/D — raised.

The remaining 54% of differing bytes are either Fehr-injected code
(out of any factory definition's scope), the patch manifests above,
or cal-footer / tuner-identification regions (`W585` tag at
`0x001FFFC0`, pairing token at `0x001FFFB0`, recomputed checksum
at `0x001FFFFE`).

## Related

- `docs/23-security-access.md` — SA algorithm structure + plug-in
  seam
- `docs/17-data-distribution-policy.md` §7 — in-tree vs. plug-in
  posture decision (closed for SA constants subcategory)
- `docs/15-clean-room-engineering.md` — facts-vs-expression
  boundary that lets these constants ship in the public repo
- `docs/05-improvements.md` §4 — brick-protection model; the
  zero-bootloader-diff finding here is one input to that model
- `src/ecu/src/subaru_security.cpp` — Fehr L1 constants live here
- `tests/unit/ecu/test_subaru_security.cpp` — Fehr L1 captured-pair
  validation
- `tests/private/test_fehr_active_live_read.cpp` (gitignored) —
  hardware-validated Fehr L1 path against the user's live ECU
