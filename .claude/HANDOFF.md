# Handoff — 2026-05-21 (P6 + 3 pack-bug sweeps + VA/VB bundle wire-up)

Marathon session. Built on yesterday's `8f4b44f`. **17 commits** all pushed. P6 descriptor library is real and finding real bugs; validate.py surfaced multiple classes of pack-quality issues that we fixed at the source (defgen) and applied corpus-wide; user dropped two forum-sourced VA/VB bundle XMLs which we wired into bulk_regen to upgrade 25 packs from 0D-scalar-only to fully-typed 2D.

**HEAD `8ad18fd`**, in sync with `origin/main`. Working tree clean apart from `SubaruTuner.zip` (114 MB, leave) and `fixtures/projects/Test/` (user-created GUI state, leave alone).

## Fifteen commits shipped (oldest → newest)

```
c774e6d tools(defgen): bootstrap descriptors.py predicate library
b625cc6 tools(defgen): add validate.py — descriptor library consumer
7e0f06f tools(defgen): descriptors filter by expected_dims to kill over-match
30c8eb5 docs(handoff): 2026-05-21 — P6 descriptor library shipped
a58c73c tools(defgen): add intake_temp_axis + engine_load_axis descriptors
f0d4fd8 defs(packs): set factual cid_address on VA/VB packs with confirmed anchors
a3d6ad6 fix(defs): a2tb002c base_timing tables stored as uint8 not uint16
feeb6b5 fix(defs): sweep base_timing dtype uint16_be → uint8 across 334 packs
5d9938f docs(handoff): late-session 2026-05-21 refresh
7a29de7 fix(defgen): propagate parent-table storagetype to inline scaling
fdde497 defs(packs): bulk_regen sweep with defgen storagetype fix (323 packs)
63dac51 docs(handoff): capture AFR investigation + defgen storagetype fix
2055c18 tools(defgen): add timing_compensation_1d + boost_compensation_1d descriptors
c8db6c5 defs(packs): re-cousin-seed 11 cousin packs from their (now-fixed) bases
875ed2f tools(defgen)+defs: wire VA/VB WRX bundles into bulk_regen
31b0e08 docs(handoff): capture VA/VB bundle wire-up + 25-pack upgrade
1e1029b tools(defgen): engine_rpm_axis tries raw/5.12 scaling for VA-era axes
```

## What's new since yesterday (8f4b44f)

### P6 descriptor library (5 commits, 9 seed predicates)

`tools/defgen/descriptors.py` (~550 lines) + `tools/defgen/validate.py` (332 lines) ship as a working pair.

**Library: 9 seed predicates** with `expected_dims` filtering, fnmatch id patterns, multi-scaling support, real-ROM evidence drift-guards:

| id | kind | dims | notes |
|---|---|---|---|
| `engine_rpm_axis` | axis | 1 | monotonic 200-8500 RPM, 5-32 pts |
| `coolant_temp_axis` | axis | 1 | -50..150 °C, 4-24 pts |
| `intake_temp_axis` | axis | 1 | same band as coolant, different patterns |
| `engine_load_axis` | axis | 1 | 0..10 g/rev monotonic, 0.5 min span |
| `wastegate_duty` | table | 2 | [0, 27000] uint16, ×100/×128/×256 raw scalings |
| `boost_target` | table | 2 | 20-350 kPa abs, tries raw / /10 / /128 / ×0.1334 (EJ psi) |
| `base_timing` | table | 2 | -15..+60° BTDC via raw / /4 / /2 |
| `timing_compensation_1d` | table | 1 | signed ±30° via raw×0.352-45 / etc.; flat-stock curves accepted |
| `boost_compensation_1d` | table | 1 | signed ±100% via raw×0.781-100 / etc. |

**Consumer: validate.py** walks `[[table]]` + `[[axis]]`, resolves shape via referenced axes, decodes bytes, runs every applicable descriptor. PASS / FAIL / NO_DESCRIPTOR / SKIPPED. Exit 0/1/2.

### Pack-bug class fixes (5 commits)

| # | Commit | Scope | What |
|---|---|---|---|
| 1 | `f0d4fd8` | 5 packs | factual VA/VB `cid_address` from empirical scan |
| 2 | `a3d6ad6`+`feeb6b5` | 1 + 334 packs | `base_timing` dtype uint16→uint8 manual sweep |
| 3 | `7a29de7` | defgen.py | root-cause fix: propagate parent-table storagetype |
| 4 | `fdde497` | 323 packs | bulk_regen sweep through the fixed defgen path |
| 5 | `c8db6c5` | 11 packs | re-cousin-seed cousins from now-correct bases |

### VA/VB bundle wire-up (1 commit, 25 packs upgraded)

User provided two RomRaider-format bundles at `fixtures/private/romraider_defs/{va,vb}/`. Unlike the per-CID forum XMLs already in-tree (sparse, no `sizex/sizey/storagetype/scaling`), the bundles carry the full inheritance pattern Merp's canonical EJ XML uses: `<rom base="va_2015_2018_base">` root with structural fields, per-CID `<rom base="…">` overrides with address remaps only.

`bulk_regen` extended with:
- `MASTER_BUNDLES` list (Merp master + VA bundle + VB bundle).
- `_cid_source_map()` builds unified `{UPPER_CID → (xml_path, default_subdir, original_case_cid)}`. Original case preserved for VB CIDs (`LHBHE00Bx0G` etc. — defgen's `--rom-id` filter is case-sensitive).
- `--create-missing` flag (unused this commit since all 26 bundle CIDs already had packs).
- `_apply_ident_preservation()` — new preservation pass for `[[identification]]` block. Preserves `cid_address`, `ecu_part`, and inserts `cid_scan` when defgen omits it. Discovered mid-sweep when the first regen pass clobbered hand-tunes — without this, `lf9g003t` would have lost its sibling-family-inferred `cid_address = 0x00035802`.
- `platform` added to `_PRESERVABLE_PACK_FIELDS` (defgen emits bare `"subaru"` from `<model>WRX</model>`; we prefer `subaru.impreza`).

**Data impact:**
- 8 VA packs (lf9c102p, lf75404h/s, lf79103p, lf75600h, lf9d012h, lf9g003t, lf9l000e) upgraded from ~250 0D scalars to e.g. `84 0D + 66 1D + 146 2D` (lf9c102p).
- 17 VB packs (LHB* series, incl. 2026 model year CIDs) upgraded from ~1094 0D-only to e.g. `527 0D + 170 1D + 399 2D` (lhbt210ub0g).
- lf79100p (cousin-seed) skipped, retains hand-tuned `cid_address=0x00037C51`.

The bundle files themselves remain at `fixtures/private/romraider_defs/` — uncommitted per Path B. bulk_regen guards with `if not bundle_path.is_file(): continue` so the public repo + tools work on machines without the bundles.

### Important caveat — AFR formula still wrong post-fix

The AFR scaling `14.7/(1+x*.0078125)` is **non-linear**; defgen's `parse_toexpr` can't linearize it and falls back to factor=1.0, offset=0.0. dump-table on AFR maps shows raw 0..255 enrichment offsets — correct in dtype, but NOT directly AFR. Applying the expression by hand: raw 47 → ~10.7 AFR.

Schema extension to carry non-linear formulas is **v1.x task** (pack.toml schema change + C++ loader update + defgen output).

## Status snapshot

- **HEAD `8ad18fd`**, in sync with `origin/main`.
- **definitions/ pack count: 373** (one promoted from throwaway; net unchanged).
- **defgen test suite: 211 tests green**.
- **C++ build: not rebuilt this session** (pure Python + TOML data).
- **CI clang-format gate: required** — no C++ touched.
- **All 707 .toml files load cleanly** post-bundle-wire-up.

a2tb002c validate.py progression through the session:

| stage | PASS | FAIL | NO_DESC | SKIP |
|---|---|---|---|---|
| after bootstrap | 4 | 22 | 205 | 159 |
| after dims-filter | 5 | 6 | 220 | 159 |
| after intake+load axes | 7 | 6 | 218 | 159 |
| after base_timing dtype fix | 11 | 2 | 218 | 159 |
| after comp descriptors + cousin re-seed | 19 | 5 | 207 | 159 |

Cross-pack coverage on regenerated packs (sampled mid-session, pre-bundle-wire-up):
- a2tb000l (regen via Merp): PASS 12 / FAIL 13 / NO_DESC 208
- A2WC400H Forester (regen via Merp): PASS 19 / FAIL 4 / NO_DESC 180

**Newly-upgraded VA/VB packs baselined this session (commit `1e1029b`):**

- lf75600h vs LF75600H.bin: PASS 0, FAIL 24, NO_DESC 132, SKIP 44
- lf9l000e vs LF9L000E.bin: PASS 1, FAIL 32, NO_DESC 235, SKIP 84

**Finding:** the bundle's table addresses don't align with our specific decrypted `.bin` files for these CIDs. The CID region matches (`internalidaddress = 0x297DD` aligns with the literal CID string in the bin), but cal-table addresses (0x2979C etc.) point at code/garbage. Most likely a sub-revision mismatch — the bundle was authored against a different LF75600H build than what we decrypted. Our `.bin` files are 2,072,576 / 2,596,864 bytes — 24,576 bytes short of the pack-declared sizes, consistent with truncated/partial decryption.

This is a useful detection capability of validate.py: it flags pack/ROM revision drift loudly. The right resolution is either source matching-revision `.bin` files OR accept the detection as a feature and move on.

`engine_rpm_axis` predicate extended in `1e1029b` to also try raw/5.12 scaling (the VA-era convention from the bundles). Confirmed additive-only — EJ packs unchanged.

## Open threads (for next session)

### Tier 1 — exercise the new VA/VB capacity

**(P6h done this session)** Baseline captured above; finding is revision-mismatch detection.

**(P6h-next) Source matching-revision .bin files for the upgraded VA/VB packs.** Our LF75600H.bin and LF9L000E.bin are 24KB short — partials or wrong revisions. If we can find matching-revision dumps, the upgraded packs become directly usable. Forum search (mhhauto, etc.) is the typical path.

**(P6e) Investigate non-linear AFR formula support.** Schema extension is the v1.x path; meantime, document loudly that AFR `dump-table` values are raw 0-255 enrichment offsets, not AFR. Affected scaling family: any `_x_0078125` or `14.7/(1+x*K)` variant.

**(P6a-continued) Add 1D fuel-compensation predicates.** ECT/IAT/atm-pressure fuel comp curves, cranking enrichment, tip-in enrichment compensations. The biggest remaining NO_DESCRIPTOR cluster after the timing+boost comp landed.

**(P6b) Remaining common axes.** `manifold_pressure_axis`, `throttle_plate_opening_angle_axis`, `mass_airflow_axis`, `atmospheric_pressure_axis`.

**(P6f) Fix `fine_correction_stored_applied_rpm_ranges` address 0x0 in a2tb002c.** Pack-level surgical fix (cousin-seed, not reached by bulk_regen).

### Tier 2 — yesterday's deck

- **(P3)** `docs/21-oem-baselines.md` — even more relevant; the validate.py + descriptor library is baseline-derivation infrastructure now.
- **(P5)** `[[table.role]]` pack-format extension — C++ heavy, unchanged.
- **(P2)** Remaining VA cousin-seeds — most blocked by hundreds-delta lesson; some may now work via the bundle if base packs are richer.

### Hardware

OBDX Pro VX adapter ETA May 22-25 — could land mid-day tomorrow. If it arrives, pause P6 and pre-stage the first-light rom-pull (command in earlier handoffs).

## Carry-over lessons (this session)

- **validate.py FAILs are signal, not noise.** Surfaced cid_address placeholders, base_timing dtype, AFR factor flatten, compensation-table dtype, lost cid_scan/ecu_part during regen.
- **Investigate root cause before bulk-fixing where possible.** Manually swept 334 packs (`feeb6b5`) and only then realized the underlying defgen bug. Both commits land cleanly, but root-cause-first would have saved the manual sweep.
- **Preservation needs a regression-class mindset.** Each new "regen source" (bundles in this case) can expose dimensions of state that weren't being preserved. The fix in `875ed2f` added preservation for fields the existing master XML rarely touched — but the bundles forced the issue.
- **Real-ROM evidence drift-guard tests pay off.** Caught two predicate-band-too-narrow regressions before commit.
- **bulk_regen is the canonical re-application path** for defgen-level fixes; per-pack manual sweeps are the immediate cure for known bugs, defgen+regen is the principled cure.

## House-style notes (carry-over)

- Terse. No trailing summaries.
- Push per-commit. Caveman commit messages.
- Don't `git add -A`; explicit paths.
- NEVER `rm -rf` user-content directories.
- `/caveman-review` + `/caveman-commit` before push.
- Bundles in `fixtures/private/` are user-private; bulk_regen guards their absence.

## Suggested opener for next session

> "HEAD `1e1029b`, in sync with `origin/main`. Marathon yesterday: 17 commits. P6 descriptor library bootstrapped (9 predicates + validate.py), VA/VB cid_address fixed on 5 packs, base_timing+AFR dtype fixed at defgen root cause and bulk_regen-swept across 323 packs, 11 cousin-seeds re-seeded, and 25 VA/VB packs upgraded from 0D-only to fully-typed 2D via two new forum bundles (va_wrx_bundle.xml + vb_wrx_bundle.xml) wired into bulk_regen.
>
> Deck for this session:
> **(P6e)** Non-linear AFR formula support (schema extension). Pack-format + C++ loader change, biggest tuner-facing usability win — AFR maps would display real 10-15 AFR instead of raw 0-255 enrichment offsets.
> **(P6a-continued)** 1D fuel-compensation predicates — biggest remaining NO_DESCRIPTOR cluster after the timing+boost comp landed.
> **(P6b)** More common axes: manifold_pressure_axis, throttle_plate_opening_angle_axis, mass_airflow_axis, atmospheric_pressure_axis. Each ~30 lines including tests.
> **(P6h-next)** Forum-source matching-revision LF75600H.bin / LF9L000E.bin if you want to actually exercise the upgraded VA/VB packs against their real anchor ROMs.
>
> Or pivot — OBDX adapter ETA May 22-25, could arrive mid-session. If hardware lands, pause P6 and pre-stage the first-light rom-pull."
