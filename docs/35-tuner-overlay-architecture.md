# 35 — Tuner-Overlay Architecture

## Why this doc exists

When SubuwuTuner displays a flashed ROM (or inspects a `.ptm` tune file via the optional cipher gating in `docs/34`), patches don't form a flat list of "edits to OEM tables." They form a **layered overlay** with characteristic architectural patterns. This document explains that pattern so contributors building inspection, diff, or analysis features can model it correctly.

The pattern was identified by corpus-wide analysis of 56 `.ptm` files (every COBB OTS tune + a personal tuner ETune iterations + Anti-Theft Mode + Stage 0) on the LF79103P platform (2017 USDM WRX MT). It is consistent across all tuners studied (COBB, a personal tuner, NTMotorsports, a community tune vendor). It is **not** an LF79103P quirk — the same pattern is expected on related SH-2A Subaru ECUs (LF75404S/H, LF79101P/102P, LF9C/D/G/L variants).

## The three layers

A typical performance tune for these ECUs has three distinct patch categories:

### Layer 1 — Calibration overrides (OEM-table edits)

**Where:** main calibration region of the ROM (typically 0x12000 to 0x1efc38 on LF79103P).
**What:** byte-level modifications to existing OEM lookup tables (Target Throttle, Boost Targets, Ignition Compensation, Fuel - Closed Loop, etc.).
**Purpose:** tweak existing behavior without changing the ECU's control logic.
**Detection:** patches fall within the address range of a known table in the loaded definition (`*.toml` per `docs/11-definition-format.md`).
**Typical share:** 60–75% of patch bytes in a Stage 1 tune; up to 75% in heavily-OEM-leveraged tunes like a personal tuner's ETunes.

### Layer 2 — Tuner additions (dead-fill exploitation)

**Where:** a large dead-fill region in the ROM that the OEM build process leaves filled with a constant pattern (on LF79103P this is `0x0CB0E530` repeating from offset 0x078bc to 0x10000, a 34,628-byte block).
**What:** entirely new lookup tables that no stock OEM code references.
**Purpose:** add calibration data that the OEM tune format cannot express — typically supplementary fuel/timing/cam tables driven by tuner-added code (Layer 3).
**Detection:** patches land in the dead-fill region; the stock ROM bytes there match the filler pattern.
**Typical share:** 22–35% of patch bytes; varies by tuner (a personal tuner's tunes use 26%, COBB OTS uses ~34%).

### Layer 3 — Code patches (overlay glue)

**Where:** main code region (0x60000+ on LF79103P) plus secondary code clusters (e.g., 0xC0000+ on LF79103P for UDS handlers).
**What:** new SH-2A instructions that reference Layer 2 tables via PC-relative literal-pool loads, plus retargeting of the OEM UDS/datalog handlers to expose tuner-specific channels.
**Purpose:** make the OEM control loop actually USE Layer 2 tables; expose new datalog channels via F3xx custom DIDs.
**Detection:** new 32-bit big-endian word values appear in the code region that point into Layer 2's address range; patches in the high-address code region (~0x1FF02C–0x1FFFE0 on LF79103P) modify the UDS service dispatch table.
**Typical share:** 1–5% of patch bytes; small but architecturally load-bearing.

## Why this pattern exists

The OEM ROM is signed/checksum-protected against gross modifications, but specific regions are tolerant:

1. **The main calibration region is byte-modifiable** without breaking ECU integrity checks — see `docs/31-brick-protection-by-isa.md` for which checks apply per ISA.
2. **The dead-fill region is never read by stock code**, so writes there cannot break stock behavior.
3. **Code-region patches are small and surgical** — typically replacing dead bytes (alignment fill) or single instructions, again avoiding integrity-check triggers.

By spreading changes across three regions in a known way, tuners avoid checksum collisions, preserve diagnostic compatibility, and make their work reversible by a stock-flash overwrite.

## Implications for SubuwuTuner features

### Inspection (T3 GUI)

When the `Ap3Browser` panel (per `docs/34`) inspects a `.ptm`:

- **Group patches by region first**, not by table. A user sees "267 patches in OEM tables, 39 patches in tuner additions, 3 patches in UDS retargets" before drilling into individual tables.
- **Mark each region with its characteristic color/icon** in the GUI so the architectural picture is visible at a glance.
- **Hide tuner-addition patches by default** — they're "implementation detail" of the tune, not user-actionable. Provide a "Show all patches" toggle.

### Diff (future)

When comparing two tunes:

- **Same-tuner iterations (e.g., a personal tuner WRK1 → WRK2)** typically differ ONLY in Layer 1 byte values; the patch envelope is identical. Diff display should highlight the changed table rows, not the whole patch list.
- **Different-tuner comparisons (a personal tuner vs a community swap-basemap vendor)** show large Layer-2 and Layer-3 differences. Group by region first; let the user pick which layer to diff.

### Validation (future)

Pre-flash sanity checks (per `docs/05-improvements.md` §4 brick-protection):

- **Layer 1 patches** can be validated against the loaded definition's table ranges — anything outside a known table is suspicious.
- **Layer 2 patches** must land entirely within the dead-fill region. A Layer-2 patch that bleeds into OEM-table address space is a likely tune authoring bug.
- **Layer 3 patches** are the highest-risk — they modify executable code. Validate they don't touch known integrity-check addresses (per `docs/31`).

### Project storage (`.stune` format)

When SubuwuTuner stores a project:

- The OEM-table edits go in `tables/` per the definition's table model.
- Tuner additions can go in `tuner_data/` as raw byte ranges keyed by ROM offset.
- Code patches go in `tuner_code/` with the same model.
- A `tuner_overlay.toml` manifest describes which layers are present.

This three-section project structure lets `.stune` files round-trip through SubuwuTuner without losing the architectural information.

## Per-ISA region addresses

The dead-fill and code-region addresses are platform-specific. Documented here are the LF79103P (2017 USDM WRX MT) values; SubuwuTuner's definition format should expose these as platform metadata.

| Region | LF79103P address range |
|---|---|
| Header / boot / security | 0x00000–0x06000 |
| Early calibration tables | 0x06000–0x078bc |
| **Dead-fill (tuner addition)** | **0x078bc–0x10000** (34,628 B) |
| Main calibration | 0x12000–0x1efc38 |
| Pre-security alignment pad | 0x1efc38–0x1f0000 |
| Security / checksum approach | 0x1f0000–0x1ff02c |
| **UDS dispatch retarget** | **0x1ff02c–0x1fffe0** (4,020 B) |
| ROM tail | 0x1fffe0–0x200000 |
| Primary code region | 0x60000–0x80000 |
| Secondary code region | 0xC0000–0xD0000 |

Other LF79*** and LF9*** ECUs share the same general structure with platform-specific offsets. The SubuwuTuner definition format (`docs/11`) should declare these as `[platform_metadata]` fields per CID, populated by the corpus build process (`tools/defgen` or equivalent).

## Per-tuner architectural fingerprints

From corpus analysis, each tuner has a characteristic dead-fill % that fingerprints their tuning approach:

| Tuner | Dead-fill share | Tuning style |
|---|---|---|
| a personal tuner (a personal tuner ETune) | 26% | Heavy OEM-table tuning; minimal additions |
| a community tune vendor Stage 2 | 33% | Balanced — uses additions but not aggressively |
| COBB OTS Stage 1 | 34% | COBB's standard reflash structure |
| COBB OTS feature modes (Anti-Theft, etc.) | 38–47% | Mostly tuner-side feature implementation |
| COBB Stage 0 | 47% | Base reflash structure dominates |

A future SubuwuTuner tune-management feature could surface this as "tuning style fingerprint" — useful for quickly understanding what kind of tune a user is looking at.

## How this was discovered

Analyst-side analysis of `.ptm` patch payloads, decrypted via the cipher gating in `docs/34`. The full corpus analysis is in `findings/ptm-decrypt-2026-06-09/CORPUS_PATCH_SUMMARY_2026_06_11.md` (analyst-side, not in this repo). The architectural picture itself is the legitimate fact that crossed the wall: this doc.

## Cross-references

- `docs/11-definition-format.md` — the table model that Layer-1 patches reference
- `docs/26-bulk-reflash-cipher.md` — the bulk-reflash cipher (related but orthogonal to `.ptm` cipher)
- `docs/31-brick-protection-by-isa.md` — what integrity checks each layer must avoid
- `docs/34-cobb-ap-as-tune-vault.md` — the cipher gating that enables Layer 1/2/3 inspection
- `docs/05-improvements.md` §4 — brick-protection considerations for validation
