# Handoff — 2026-05-21 late-session (P6 + two big pack-bug sweeps)

Long, dense session. Built on yesterday's `8f4b44f`. Eight commits, all pushed. P6 descriptor library is real and finding real bugs.

**HEAD `feeb6b5`**, in sync with `origin/main`. Working tree clean apart from `SubaruTuner.zip` (114 MB, leave) and `fixtures/projects/Test/` (user-created GUI state, leave alone).

## Eight commits shipped (oldest → newest)

```
c774e6d tools(defgen): bootstrap descriptors.py predicate library
b625cc6 tools(defgen): add validate.py — descriptor library consumer
7e0f06f tools(defgen): descriptors filter by expected_dims to kill over-match
30c8eb5 docs(handoff): 2026-05-21 — P6 descriptor library shipped
a58c73c tools(defgen): add intake_temp_axis + engine_load_axis descriptors
f0d4fd8 defs(packs): set factual cid_address on VA/VB packs with confirmed anchors
a3d6ad6 fix(defs): a2tb002c base_timing tables stored as uint8 not uint16
feeb6b5 fix(defs): sweep base_timing dtype uint16_be → uint8 across 334 packs
```

## What's new since yesterday (8f4b44f)

### P6 descriptor library (4 commits)

`tools/defgen/descriptors.py` + `tools/defgen/validate.py` are real, callable, useful tools.

**Library: 7 seed predicates** with `expected_dims` filtering, fnmatch id patterns, multi-scaling support, and real-ROM evidence drift-guards:

| id | kind | dims | notes |
|---|---|---|---|
| `engine_rpm_axis` | axis | 1 | monotonic 200-8500 RPM, 5-32 pts |
| `coolant_temp_axis` | axis | 1 | -50..150 °C, 4-24 pts |
| `intake_temp_axis` | axis | 1 | same band as coolant, different name patterns |
| `engine_load_axis` | axis | 1 | 0..10 g/rev monotonic with 0.5 minimum span |
| `wastegate_duty` | table | 2 | [0, 27000] uint16, covers ×100/×128/×256 raw scalings |
| `boost_target` | table | 2 | 20-350 kPa absolute, tries raw / /10 / /128 / ×0.1334 (EJ psi) |
| `base_timing` | table | 2 | -15..+60° BTDC via raw / /4 / /2 |

`expected_dims` filter (commit `7e0f06f`) was the principled fix for FAIL-noise: a 2D-only predicate no longer over-matches a 1D entry just because the id pattern fits. Dropped a2tb002c FAIL count from 22 to 6.

**Consumer: `validate.py`**

```
python tools/defgen/validate.py --pack <pack.toml> --rom <bin> [--tsv] [--kind ...] [--only ID]
```

Walks `[[table]]` + `[[axis]]`, resolves 2D shape via the referenced axes, decodes bytes, runs every applicable descriptor, reports PASS / FAIL / NO_DESCRIPTOR / SKIPPED. Exit 0 = clean, 1 = some FAIL, 2 = arg/file error.

**a2tb002c baseline through the session:**

| stage | PASS | FAIL | NO_DESCRIPTOR | SKIPPED |
|---|---|---|---|---|
| after bootstrap | 4 | 22 | 205 | 159 |
| after dims-filter | 5 | 6 | 220 | 159 |
| after intake+load axes | 7 | 6 | 218 | 159 |
| after base_timing dtype fix | 11 | 2 | 218 | 159 |

The 2 remaining a2tb002c FAILs (real signal): `fine_correction_stored_applied_rpm_ranges` axis declared at `0x00000000` (sentinel — pack bug); `initial_max_wastegate_duty_compensation_atm_pressure` matched by `*wastegate*duty*` but is a 1D-style relative-compensation map, not absolute duty (descriptor-pattern fix territory).

### Pack-bug fix #1: VA/VB cid_address (commit `f0d4fd8`)

Empirical scan of decrypted VA/VB ROMs found literal CID strings at per-sub-family offsets in 0x29000-0x38000. Set explicit `cid_address` on packs where we have direct or strong-evidence anchors:

| pack | new cid_address | source |
|---|---|---|
| lf75600h | `0x000297DD` | direct (LF75600H.bin) |
| lf79100p | `0x00037C51` | direct (LF79100P.bin) — and promoted from yesterday's throwaway |
| lf9l000e | `0x00035807` | direct (LF9L000E.bin) |
| lf9g003t | `0x00035802` | sibling-family (LF9G002 + LF9G100 both at this offset) |
| lv9n001d | unchanged | added `cid_scan = true` as safety net for the hand-tuned `0x0002AA1C` |

All packs kept `cid_scan = true` as a fallback. Left alone (no anchor in corpus, cid_scan already works): lf75404h, lf75404s, lf79103p, lf9c102p.

### Pack-bug fix #2: base_timing dtype across 334 packs (commits `a3d6ad6` + `feeb6b5`)

validate.py found that a2tb002c's `base_timing_*` tables decode to nonsense (mean ~8000° BTDC). Root cause: the scaling `base_ignition_timing_degrees_btdc_x_3515625_20` has factor 0.3515625 = 90/256 with offset −20, dimensioned for **uint8 raw input** (0..255 → −20..+70° BTDC). Tables declared `data_type = "uint16_be"` decoded each cell as 2 bytes and got garbage.

Independent confirmation: the 1D base_timing_idle_* tables in a2tb002c sit at addresses 0xCE201, 0xCE211, 0xCE221, 0xCE231 — exactly 16 bytes apart. uint16 cells would need 32 bytes per row and overlap the next table. uint8 layout is consistent.

Surgical commit (`a3d6ad6`) fixed a2tb002c (9 tables + scaling block); sweep commit (`feeb6b5`) applied the same regex-narrow fix to all 334 packs using that scaling. 2184 table dtype fixes + 334 scaling dtype fixes. 707/707 packs still load; 203/203 defgen tests green; dump-table on A2WC400H Forester now shows 2..45° BTDC mean ~26° (engineering-norm timing values).

## Suspected third pack bug — NOT fixed

`estimated_air_fuel_ratio_14_7_1_x_0078125` scaling has `factor = 1.0` in the TOML but the name encodes factor 0.0078125 (1/128). AFR tables using it decode to 0..65535 mean ~14050 instead of ~14.7. **A different shape of bug** — wrong `scaling.factor`, not wrong `table.data_type`. Affects ~335 packs (essentially the same corpus as base_timing).

Did not fix because:
- Two valid hypotheses (defgen ate the factor; or upstream XML really has 1.0 and the name is misleading)
- No clean empirical disambiguator without checking source XMLs
- Same shape of fix may apply to other suspect scalings (123 distinct scalings have `factor >= 0.1` + `uint16_be`, though most are legitimate — the AFR is the obvious-bug example)

Logged as a deck item for next session.

## Status snapshot

- **HEAD `feeb6b5`**, in sync with `origin/main`
- **definitions/ pack count: 373** (unchanged; one pack promoted from throwaway)
- **defgen test suite: 203 tests green** (152 prior + 31 descriptor + 11 validate + 9 added axes + 2 dims-filter — net 51 new)
- **C++ build: not rebuilt this session** (pure Python + TOML data)
- **CI clang-format gate: required** — no C++ touched
- **All 707 .toml files under definitions/ load cleanly** (re-verified post-sweep)

## Open threads (for next session)

### Tier 1 — finish P6 descriptor library

**(P6e) Investigate the `_x_0078125` factor-1.0 scaling family.** Hardest of the three because the fix shape isn't `data_type`, it's `scaling.factor`. Steps: (a) load one Merp canonical XML, see if the source XML has factor 1.0 or 0.0078125; (b) if upstream is 0.0078125, this is a defgen bug — find the parsing path that drops it; if upstream is 1.0, the bug is in the XMLs (community-edited?) and we should fix it at the pack level. Affected packs: ~335.

**(P6a) Add 1D compensation predicates.** Biggest NO_DESCRIPTOR cluster in a2tb002c is `*_compensation_*` 1D curves (timing comp ECT/IAT/MRP, fuel comp, etc.). ~5 descriptors, ~20-30 entries lifted from NO_DESCRIPTOR to PASS. Easy wins.

**(P6b) More common axes.** `manifold_pressure_axis`, `throttle_plate_opening_angle_axis`, `mass_airflow_axis`, `atmospheric_pressure_axis`. Each ~30 lines including tests.

**(P6c) Cross-pack validation runs.** Currently only validated a2tb002c. Run validate.py against a2tb100u, A2WC400H, ez1d*, etc. Surfaces scaling variants the a2tb002c-seeded library missed.

**(P6f) Fix `fine_correction_stored_applied_rpm_ranges` address 0x00000000 in a2tb002c.** Smaller follow-up to the base_timing fix; need to find the real address by pattern search or by checking what the values "should" look like.

### Tier 2 — back to yesterday's deck

**(P1)** GUI feedback from yesterday's a2tb002c session — still pending, but bullet 4 of dtype fix means GUI dump-table on base_timing in 334 packs now works correctly. If user re-tests, they may notice the improvement automatically.

**(P2)** Remaining VA cousin-seeds — still blocked. Hundreds-delta lesson held perfectly.

**(P3)** `docs/21-oem-baselines.md` — even more relevant now that we have empirical descriptor coverage as a baseline-derivation tool.

**(P4)** Axis-fingerprint relocator v3 — drop from deck (subsumed by P6).

**(P5)** `[[table.role]]` pack-format extension — C++ heavy, unchanged.

### Hardware

OBDX Pro VX adapter ETA May 22-25. First-light pre-staged command unchanged.

## Carry-over lessons

- **validate.py FAILs are signal, not noise.** Found two real pack-bug classes today (cid_address placeholders, base_timing dtype); flagged a third (AFR scaling factor).
- **Real-ROM evidence drift-guard tests pay off.** Caught two predicate-band-too-narrow regressions before they reached commit.
- **Bulk-sweep pattern works** when (a) the bug shape is identical across packs, (b) the fix is locally scoped (regex anchored to the relevant scaling/table id), and (c) post-sweep `pack-list` + tests + spot-check via dump-table all green. 334-pack sweep landed cleanly.
- **Address-spacing arguments are independent evidence.** For base_timing, the 16-byte stride between consecutive 1D-table addresses was independent proof of uint8; would have caught the dtype bug even without dump-table verification.

## House-style notes (carry-over)

- Terse. No trailing summaries.
- Push per-commit. Caveman commit messages.
- Don't `git add -A`; explicit paths. (`SubaruTuner.zip` still in the working tree.)
- NEVER `rm -rf` user-content directories.
- `/caveman-review` + `/caveman-commit` before push.

## Suggested opener for next session

> "HEAD `feeb6b5`, in sync with `origin/main`. Big session yesterday: P6 descriptor library bootstrapped (7 predicates + validate.py), VA/VB cid_address fixed on 5 packs, base_timing dtype fixed on 334 packs (~2500 individual rewrites). 203/203 defgen tests green.
>
> Three things on the deck:
> **(P6e)** Investigate the `_x_0078125` AFR-scaling factor-1.0 bug. Affects ~335 packs. Need to check whether upstream Merp XML has factor 1.0 or 0.0078125, then fix wherever the truth lies. AFR tables currently decode to 0-65535 instead of 9-22.
> **(P6a)** Add 1D-compensation descriptors — biggest NO_DESCRIPTOR cluster in a2tb002c. ~5 predicates, easy lift to ~15% coverage.
> **(P6c)** Cross-pack validate.py runs (a2tb100u, A2WC400H Forester, ez1d*) to surface scaling variants we missed.
>
> Or pivot — adapter may land mid-session. If OBDX arrives, drop P6 and pre-stage the first-light dump."
