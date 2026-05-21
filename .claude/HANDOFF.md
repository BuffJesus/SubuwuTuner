# Handoff — 2026-05-21 (P6 + 3 pack-bug sweeps via root-cause defgen fix)

Long session. Built on yesterday's `8f4b44f`. **Eleven commits**, all pushed. P6 descriptor library is real and finding real bugs; validate.py surfaced two classes of pack-quality bugs that we then fixed at the source (defgen) and applied corpus-wide.

**HEAD `fdde497`**, in sync with `origin/main`. Working tree clean apart from `SubaruTuner.zip` (114 MB, leave) and `fixtures/projects/Test/` (user-created GUI state, leave alone).

## Eleven commits shipped (oldest → newest)

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
```

## What's new since yesterday (8f4b44f)

### P6 descriptor library (4 commits)

`tools/defgen/descriptors.py` (442 lines) + `tools/defgen/validate.py` (332 lines) ship as a working pair. The library has the data model, registry, predicate building blocks, and 7 seed predicates; validate.py is the consumer that runs predicates against (pack, ROM) pairs.

**Library: 7 seed predicates** with `expected_dims` filtering, fnmatch id patterns, multi-scaling support, and real-ROM evidence drift-guards:

| id | kind | dims | notes |
|---|---|---|---|
| `engine_rpm_axis` | axis | 1 | monotonic 200-8500 RPM, 5-32 pts |
| `coolant_temp_axis` | axis | 1 | -50..150 °C, 4-24 pts |
| `intake_temp_axis` | axis | 1 | same band as coolant, different patterns |
| `engine_load_axis` | axis | 1 | 0..10 g/rev monotonic, 0.5 min span |
| `wastegate_duty` | table | 2 | [0, 27000] uint16, ×100/×128/×256 raw scalings |
| `boost_target` | table | 2 | 20-350 kPa abs, tries raw / /10 / /128 / ×0.1334 (EJ psi) |
| `base_timing` | table | 2 | -15..+60° BTDC via raw / /4 / /2 |

`expected_dims` filter was the principled fix for FAIL-noise: a 2D-only predicate no longer over-matches a 1D entry by id pattern alone.

**Consumer: validate.py**

```
python tools/defgen/validate.py --pack <pack.toml> --rom <bin> [--tsv] [--kind ...] [--only ID]
```

Walks `[[table]]` + `[[axis]]`, resolves 2D shape via referenced axes, decodes bytes, runs every applicable descriptor, reports PASS / FAIL / NO_DESCRIPTOR / SKIPPED with a coverage summary. Exit 0/1/2.

**a2tb002c baseline progression through the session:**

| stage | PASS | FAIL | NO_DESC | SKIP |
|---|---|---|---|---|
| after bootstrap | 4 | 22 | 205 | 159 |
| after dims-filter | 5 | 6 | 220 | 159 |
| after intake+load axes | 7 | 6 | 218 | 159 |
| after base_timing dtype fix | 11 | 2 | 218 | 159 |

The 2 remaining a2tb002c FAILs are real signal: `fine_correction_stored_applied_rpm_ranges` axis at sentinel address `0x00000000`, and `initial_max_wastegate_duty_compensation_atm_pressure` matched by `*wastegate*duty*` but actually a 1D-style relative-compensation map.

### Pack-bug fix #1: VA/VB cid_address (commit `f0d4fd8`)

Empirical scan found literal CID strings at per-sub-family offsets clustered in 0x29000-0x38000 in real-anchor VA/VB ROMs. Set explicit `cid_address` on packs with direct/strong-evidence anchors:

| pack | cid_address | source |
|---|---|---|
| lf75600h | `0x000297DD` | direct (LF75600H.bin) |
| lf79100p | `0x00037C51` | direct (LF79100P.bin) — also promoted from throwaway |
| lf9l000e | `0x00035807` | direct (LF9L000E.bin) |
| lf9g003t | `0x00035802` | sibling-family (LF9G002+LF9G100) |
| lv9n001d | unchanged 0x0002AA1C | added `cid_scan = true` safety net |

cid_scan = true retained on all four explicit cases as fallback. Untouched (no anchor): lf75404h, lf75404s, lf79103p, lf9c102p.

### Pack-bug fix #2 + root cause: base_timing dtype (commits `a3d6ad6`, `feeb6b5`, `7a29de7`, `fdde497`)

validate.py flagged `base_timing_*` tables in a2tb002c decoding to ~8000° BTDC nonsense. Diagnosis: scaling factor 0.3515625 = 90/256 with offset -20 is dimensioned for uint8 raw, but tables declared `data_type = "uint16_be"`.

Independent confirmation: 1D `base_timing_idle_*` tables sit 16 bytes apart (0xCE201, 0xCE211, 0xCE221, 0xCE231) — uint16 cells would need 32 bytes per row and overlap. uint8 layout is consistent.

**Surgical fix first** (`a3d6ad6`): a2tb002c only, 9 tables. dump-table on `base_timing_primary_cruise` then shows -15.43..47.15° BTDC (engineering norm).

**Manual sweep second** (`feeb6b5`): same regex-narrow fix on all 334 packs using the scaling. 2184 table dtype rewrites + 334 scaling rewrites.

**Root-cause fix third** (`7a29de7`): the underlying defgen bug — `_scaling_from_element` only read storagetype from the scaling element, defaulted to uint16_be when missing, and that bad default propagated up to the table's data_type via `_extract_table` (line 956). Merp's canonical XML puts storagetype on the parent `<table>` and leaves the inline `<scaling>` child without one; defgen needed to inherit it. Added `fallback_storagetype`/`fallback_endian` kwargs to `_scaling_from_element`; wired the caller in `_extract_table` and `_axis_from_element` to pass parent attributes through. 3 new regression tests in `StoragetypeInheritanceTest`.

**Bulk_regen sweep fourth** (`fdde497`): 323 packs regenerated via the corrected defgen path. The manual sweep and the regen now agree on base_timing (both produce uint8). Additionally corrected dtype on:
- `estimated_air_fuel_ratio_14_7_1_x_0078125` (AFR enrichment maps) — uint16→uint8.
- Several smaller uint8 tables that shared the XML convention.

38 packs skipped (cousin-seeds without source XML, including a2tb002c which retains the manual fix).

### Important caveat — AFR formula still wrong post-fix

The AFR scaling `14.7/(1+x*.0078125)` is **non-linear**; defgen's `parse_toexpr` can't linearize it and falls back to factor=1.0, offset=0.0 with a `non-linear toexpr flattened to identity` warning. dump-table on AFR maps now shows raw 0..255 enrichment offsets — correct in dtype, but NOT directly AFR. Applying the expression by hand: raw 47 → 14.7/(1+47×0.0078125) = ~10.7 AFR (plausible enrichment).

Schema extension to carry non-linear formulas is **v1.x task** (would require pack.toml schema change + C++ loader update + defgen output).

## Status snapshot

- **HEAD `fdde497`**, in sync with `origin/main`
- **definitions/ pack count: 373** (one promoted from throwaway; net unchanged)
- **defgen test suite: 206 tests green** (152 prior + 31 descriptor + 11 validate + 9 added axes + 2 dims-filter + 3 storagetype-inheritance)
- **C++ build: not rebuilt this session** (pure Python + TOML data)
- **CI clang-format gate: required** — no C++ touched
- **All 707 .toml files under definitions/ load cleanly** (re-verified post-regen)

Spot-check coverage on cross-pack validate.py:
- a2tb002c (manual-fixed cousin-seed): PASS 11 / FAIL 2 / NO_DESC 218
- a2tb000l (defgen-regenerated): PASS 7 / FAIL 7 / NO_DESC 219
- A2WC400H Forester (defgen-regenerated): PASS 9 / FAIL 1 / NO_DESC 193

## Open threads (for next session)

### Tier 1 — finish P6

**(P6a) Add 1D compensation predicates.** Biggest NO_DESCRIPTOR cluster: `*_compensation_*` 1D curves (timing comp ECT/IAT/MRP, fuel comp, etc.). ~5 descriptors with signed-small-magnitude predicates would lift ~20-30 entries to PASS. Easy.

**(P6b) Add common 1D axes.** `manifold_pressure_axis`, `throttle_plate_opening_angle_axis`, `mass_airflow_axis`, `atmospheric_pressure_axis`. Each ~30 lines including tests.

**(P6c) Run validate.py across more packs.** ez1d* family (yesterday's known-good reference), an outback/forester/tribeca sample, to surface scaling variants the EJ-Legacy-seeded library missed.

**(P6f) Non-linear formula support.** Schema extension. Pack.toml `[[scaling]]` gains a `formula` value beyond `"linear"`; loader needs to handle. Then defgen's `parse_toexpr` extends to recognize `14.7/(1+x*K)` style expressions and emit `formula = "subaru_afr_enrichment" { k = 0.0078125, num = 14.7 }` or similar. After that, AFR dump-table shows actual AFR values (10-15 range) instead of raw 0-255.

**(P6g) Fix `fine_correction_stored_applied_rpm_ranges` axis at 0x0.** Pack-level fix in a2tb002c; needs the real address (probably pattern-search via the existing localize tooling).

### Tier 2 — yesterday's deck

**(P1)** GUI feedback from a2tb002c — pending. dump-table on base_timing in 334 packs now works correctly; user's GUI session would see the improvement automatically.

**(P2)** Remaining VA cousin-seeds — blocked (hundreds-delta lesson).

**(P3)** `docs/21-oem-baselines.md` — even more relevant; validate.py + descriptor library are baseline-derivation infrastructure.

**(P4)** Axis-fingerprint relocator v3 — drop (subsumed by P6).

**(P5)** `[[table.role]]` pack-format extension — C++ heavy, unchanged.

### Hardware

OBDX Pro VX adapter ETA May 22-25. First-light pre-staged command unchanged.

## Carry-over lessons (this session)

- **validate.py FAILs are signal, not noise.** Surfaced two real bug classes (cid_address placeholders + dtype mis-declaration) plus a third (non-linear scaling formula flatten) we've deferred but documented.
- **Always look for the root cause before bulk-fixing.** I patched 334 packs manually (feeb6b5) and only then realized the underlying defgen bug. Both commits land — manual sweep is the immediate cure, defgen fix is the principled cure — but in a future similar situation, do the root-cause investigation FIRST (would have saved the larger pack-only sweep entirely).
- **Real-ROM evidence drift-guard tests pay off.** Caught two predicate-band-too-narrow regressions before they reached commit.
- **bulk_regen is the canonical re-application path** for defgen-level fixes. Idempotent, preserves manually-patched [pack] fields, skips packs without source XML.

## House-style notes (carry-over)

- Terse. No trailing summaries.
- Push per-commit. Caveman commit messages.
- Don't `git add -A`; explicit paths.
- NEVER `rm -rf` user-content directories.
- `/caveman-review` + `/caveman-commit` before push.
- Investigation before bulk-fixing when the bug class might have a root cause.

## Suggested opener for next session

> "HEAD `fdde497`, in sync with `origin/main`. Big session yesterday: 11 commits. P6 descriptor library bootstrapped (7 predicates + validate.py), VA/VB cid_address fixed on 5 packs, base_timing+AFR dtype fixed at root cause in defgen and bulk_regen-swept across 323 packs (plus 334-pack manual sweep on the surface). 206/206 defgen tests green.
>
> Deck for this session:
> **(P6a)** 1D compensation predicates — biggest NO_DESCRIPTOR cluster. ~5 descriptors lifts cross-pack coverage to maybe 10-15%.
> **(P6f)** Non-linear formula support (schema extension) — AFR maps would then display real AFR (10-15 range) instead of raw 0-255 enrichment offsets. Pack-format + C++ loader change, biggest impact for tuner-facing usability.
> **(P6c)** Cross-pack validate runs (ez1d* family + other dirs) to surface descriptor gaps.
>
> Or pivot — OBDX adapter ETA May 22-25 any day now. If hardware lands, pause P6 and pre-stage the first-light rom-pull."
