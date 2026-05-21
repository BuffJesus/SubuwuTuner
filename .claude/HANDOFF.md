# Handoff — 2026-05-21 mid-session (P6 descriptor library shipped)

Picking up from yesterday's end-of-day handoff (HEAD `8f4b44f` then). Today's session attempted P2-batch (all dropped), pivoted "wide" into a defgen XML→TOML investigation that turned out to be a wrong premise, then bootstrapped P6 — the cal-table descriptor library — including a consumer CLI and shape-aware filtering. Three commits, all pushed.

**HEAD `7e0f06f`**, in sync with `origin/main`. Working tree clean apart from `SubaruTuner.zip` (114 MB, leave), `definitions/impreza/lf79100p.toml` (yesterday's throwaway VA GUI-test cousin-seed, still untracked), and `fixtures/projects/Test/` (user-created GUI project from yesterday's 22:50 session, leave alone).

## What shipped this session

```
7e0f06f tools(defgen): descriptors filter by expected_dims to kill over-match
b625cc6 tools(defgen): add validate.py — descriptor library consumer
c774e6d tools(defgen): bootstrap descriptors.py predicate library
```

### `tools/defgen/descriptors.py` (442 lines)

Predicate library describing what real Subaru cal tables look like. Data model:

- `DecodeHint(dtype, dims, length, rows, cols)` — how to interpret a byte region.
- `Verdict(matches, score, reasons)` — predicate output.
- `Evidence(pack_id, table_id, rom_path)` — pointer to a real-world positive example.
- `Descriptor(id, kind, description, id_patterns, predicate, evidence, expected_dims)` — the full triple. `expected_dims` filters out 2D-only predicates from being applied to 1D entries by id pattern alone.
- `register(d)`, `all_descriptors()`, `by_id(...)`, `candidates_for(id, kind, dims)` — registry API.
- Helper primitives: `decode_values`, `values_in_range`, `is_monotonic`, `distinct_fraction`.

**5 seed predicates** (handoff sized eventual lib at 30-50):

| id | kind | expected_dims | matches |
|---|---|---|---|
| `engine_rpm_axis` | axis | 1 | monotonic, 200-8500 RPM span, 5-32 pts |
| `coolant_temp_axis` | axis | 1 | monotonic, -50..150 °C, 4-24 pts |
| `wastegate_duty` | table | 2 | uint16 in [0, 27000] (covers ×100/×1/128/×1/256), ≥4×4, ≥10% distinct |
| `boost_target` | table | 2 | uint16 decodes to 20-350 kPa absolute via raw / /10 / /128 / ×0.1334 (EJ psi) |
| `base_timing` | table | 2 | signed decodes to -15..+60° BTDC via raw / /4 / /2 |

Each predicate handles multiple Subaru scaling families (the empirical-baseline iteration is built in). Each cites concrete evidence in `a2tb002c` and a real-ROM drift-guard test ensures predicates don't silently regress against their own evidence.

### `tools/defgen/validate.py` (332 lines)

Consumer CLI for the descriptor library:

```
python tools/defgen/validate.py --pack <pack.toml> --rom <rom.bin> [--tsv] [--kind table|axis] [--only ID]
```

For each `[[table]]` and `[[axis]]` in the pack, resolves the 2D shape from referenced axes, decodes bytes at the declared address, runs every applicable descriptor predicate. Reports PASS / FAIL / NO_DESCRIPTOR / SKIPPED with a coverage summary. Exit 0 = no FAILs, 1 = one or more FAILs, 2 = arg/file error.

**a2tb002c baseline (post-dims-filter):**
```
PASS                  5  (  1.3%)
FAIL                  6  (  1.5%)
NO_DESCRIPTOR       220  ( 56.4%)
SKIPPED             159  ( 40.8%, all 0D scalars)
Coverage            2.8%  of non-scalar entries
```

PASSing entries: `engine_speed` axis, `coolant_temperature` axis, `target_boost`, `initial_wastegate_duty`, `max_wastegate_duty`.

### Tests

- 31 tests in `test_descriptors.py` (28 synthetic + 3 real-ROM evidence checks).
- 11 tests in `test_validate.py` (synthetic mini-pack + ROM, exercises loader, validate_entry, main exit codes).
- **196/196 defgen tests green** (152 prior + 44 new).

## Real bugs validate.py surfaced in a2tb002c

These are pack metadata bugs, NOT descriptor problems. Logging for follow-up:

1. **`base_timing_*_cruise` and `base_timing_*_non_cruise` claim `data_type = "uint16_be"`** but the bytes only make sense as `uint8`. Pack scaling factor (0.3515625, offset -20) maps uint8 raw 13-152 → -15..+33° BTDC (sensible). uint16 raw 29336 → 10314° BTDC (nonsense). CLI `dump-table` on these returns garbage values.

2. **`fine_correction_stored_applied_rpm_ranges` axis declares `address = 0x00000000`** — that's inside the CID region of every Subaru ROM, not a real axis address. Likely a sentinel that survived defgen.

3. **`initial_max_wastegate_duty_compensation_atm_pressure`** is matched by `*wastegate*duty*` pattern but it's a signed delta-percentage comp curve, not absolute duty. Needs its own 1D-compensation descriptor.

Items (1) and (2) are pack fixes (touch `definitions/legacy/a2tb002c.toml` after verifying with the bludgod ROM). Item (3) is a descriptor addition (1D wastegate-comp descriptor).

## Earlier this session (chronological)

### P2 batch — all four candidates dropped

Hundreds-digit-delta lesson held perfectly:

| Target | Sibling | HIGH% | Verdict |
|---|---|---|---|
| LF9C000C | (no lf9c102p.bin) | — | blocked: sibling pack has no .bin |
| LV9N100A | lv9n001d | 1.3% | drop |
| LV9N303J | lv9n001d | 0.0% | drop |
| LF75300E | lf75404h | 0.7% | drop |

No commits, no new packs. Working tree clean after.

### "Wide" defgen XML→TOML axes investigation

Premise: defgen drops axes for VA/VB packs, demoting tables to 0D-only. Investigated, **premise wrong**:

- defgen's `_axis_from_element` (line 1041-1043) does return None for axes without a `name` attribute, triggering demote-to-scalar at line 1006-1013. Behaviorally accurate diagnosis.
- BUT the per-CID VA/VB XMLs are structurally minimal: no `sizex`/`sizey`/`size`/`elements` attributes anywhere, no inheritance base, no scaling sub-elements. Without axis lengths, even accepting nameless axes would only produce 2D-shaped tables with length=0 axes — still unusable.
- Real unlock requires ROM-byte axis-length inference (the P6 descriptor library, in principle). defgen is doing the right thing given the input.

Also discovered: **`build/scratch/SubaruDefs/RomRaider/ecu/standard/ecu_defs.xml`** is Merp's canonical XML (~21 MB), source of the EJ-era pack richness. VA/VB CIDs aren't in it.

### P6 bootstrap (the rest of the session)

Three commits described above. Real-ROM calibration loop worked: first descriptor batch rejected its own a2tb002c evidence (bands too narrow), refined predicates to absorb multiple Subaru scaling variants, added real-ROM tests to prevent regression.

## Status snapshot

- **HEAD `7e0f06f`**, in sync with `origin/main`
- **definitions/ pack count: 373** (unchanged from yesterday)
- **defgen test suite: 196 tests green** (152 prior + 31 descriptors + 11 validate + 2 dims-filter)
- **C++ build: not rebuilt this session** (pure Python work)
- **CI clang-format gate: required** — no C++ touched
- **All 706 .toml files under definitions/ load cleanly** (verified yesterday; nothing changed today)

## Open threads (for next session)

### Tier 1 — finish P6 descriptor library

The framework is solid; coverage is the next axis.

**(P6a) Add 1D compensation predicates.** Biggest NO_DESCRIPTOR cluster in a2tb002c is `*timing_compensation_*` curves (signed small magnitudes around 0). Adding a generic `timing_compensation_1d` descriptor would catch ~8 entries. Same for `*_compensation_iat`, `*_compensation_ect`, `*_compensation_atm_pressure` — these are 1D scalars vs the matching reference axis. Easy wins.

**(P6b) Add common axes that aren't currently modeled.** From the NO_DESCRIPTOR axis list:
- `intake_temperature` (IAT, -40..+120°C, like coolant)
- `atmospheric_pressure` (50-105 kPa)
- `engine_load` (g/rev or kg/h, 0-5 typical)
- `throttle_plate_opening_angle` (0-100%)
- `mass_airflow` (0-300 g/s or so)

Each is ~30 lines including tests. 5 descriptors brings coverage from ~3% to maybe 15-20% on a2tb002c.

**(P6c) Cross-pack validation runs.** Currently only validated a2tb002c. Run validate.py against:
- `a2tb100u.toml` + `a2tb100u_*.bin` (impreza EJ, same family)
- An ez1d* pack (also called out in handoff as known-good)
- An lf75404h cousin-seeded pack (VA-family, mostly 0D so won't show much, but baselines coverage there)

This surfaces scaling variants the a2tb002c-only seed library missed.

**(P6d) Fix a2tb002c pack bugs validate found.** base_timing dtype (uint16 → uint8) and fine_correction zero-address. Verify with dump-table after the fix.

### Tier 2 — back to yesterday's deck

**(P1)** GUI feedback from a2tb002c session — still nothing surfaced from user.

**(P2)** Remaining VA cousin-seeds — blocked per today's run. Hundreds-delta lesson holds across LF9C/LV9N/LF75 families. Needs closer siblings (no current candidates) or actual XMLs.

**(P3)** `docs/21-oem-baselines.md` — still deferred 5×. Now arguably even more relevant: validate.py + descriptor library is part of the baseline tooling. Could fold the empirical observation harvesting into a single doc-writing session.

**(P4)** Axis-fingerprint relocator v3 — superseded by P6 if P6 grows. Drop from deck unless P6 stalls.

**(P5)** `[[table.role]]` pack-format extension — C++ heavy, unchanged.

### Hardware

**OBDX Pro VX adapter** ETA May 22-25 2026 (1-4 days from this handoff). First-light pre-staged command in yesterday's handoff still valid.

## House-style notes (carry-over)

All from yesterday + reinforced today:

- Terse. No trailing summaries.
- Push per-commit. Caveman commit messages.
- Don't `git add -A`; explicit paths. (`SubaruTuner.zip` would break the push.)
- NEVER `rm -rf` user-content directories.
- `/caveman-review` the diff + `/caveman-commit` the message before push (today: did three commits this way).
- Real-ROM evidence tests are great safety net — descriptor refinements broke their own evidence twice today; tests caught it.
- Validate.py FAILs are SIGNAL, not noise. Each surfaced FAIL = either pack bug, predicate gap, or pattern over-match. All three are useful.

## Suggested opener for next session

> "HEAD `7e0f06f`, in sync with `origin/main`. P6 descriptor library bootstrapped this session: framework + 5 seed predicates + validate.py consumer CLI + dims-filter for over-match. 196/196 defgen tests green. a2tb002c coverage baseline: 1.3% PASS, 1.5% FAIL, 56.4% NO_DESCRIPTOR, 40.8% SKIPPED (0D scalars).
>
> Three things on the deck for this session:
> **(P6a)** Add 1D compensation descriptors — biggest NO_DESCRIPTOR cluster is `*_compensation_*` curves. ~5 new descriptors lifts a2tb002c coverage from ~3% to ~15-20%.
> **(P6c)** Cross-pack validate.py runs (a2tb100u, ez1d*, an lf75404h-cousin pack) to surface scaling variants we missed.
> **(P6d)** Fix a2tb002c.toml base_timing dtype (uint16 → uint8, validate.py flagged it; CLI dump-table currently returns garbage on those tables).
>
> Or pivot — adapter may land mid-session. If OBDX arrives, drop P6 and pre-stage the first-light dump."
