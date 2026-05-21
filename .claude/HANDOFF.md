# Handoff — 2026-05-21 (P6 + 3 pack-bug sweeps + VA/VB bundles + AFR formula)

Marathon session. Built on yesterday's `8f4b44f`. **37 commits** all pushed. P6 descriptor library is real and finding real bugs; validate.py surfaced multiple classes of pack-quality issues that we fixed at the source (defgen) and applied corpus-wide; user dropped two forum-sourced VA/VB bundle XMLs which we wired into bulk_regen to upgrade 25 packs from 0D-scalar-only to fully-typed 2D.

**HEAD `33aa0d1`**, in sync with `origin/main`. Working tree clean apart from `SubaruTuner.zip` (114 MB, leave) and `fixtures/projects/Test/` (user-created GUI state, leave alone).

## Commits shipped this session (oldest → newest)

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
083b9b7 docs(handoff): capture P6h baseline + engine_rpm_axis /5.12 extension
74624d3 feat(defs+defgen): subaru_afr_enrichment non-linear formula
f202081 defs(packs): bulk_regen sweep emits subaru_afr_enrichment (323 packs)
45f7d62 docs(handoff): capture P6e — subaru_afr_enrichment formula shipped
07c8cc3 tools(defgen): broaden compensation descriptors (1D patterns + new 2D)
b4a062e docs(handoff): capture compensation-descriptor broadening (07c8cc3)
6a4693a fix(defgen)+defs: float-endian override for non-axis scalings + cousin re-seed
451b28c docs(handoff): capture user-experience pass + 6a4693a
fc67e84 docs(handoff): fix stale commit count in suggested opener
e3c9bf6 docs(handoff): UX pass on a2tb001c deprioritizes P6a-continued
ad187c4 docs(handoff): P6e-VA investigated — VA fueling tables are correct
09342dd feat(defs+defgen): inverse_divide non-linear formula
9ec0c32 docs(handoff): inverse_divide formula shipped (09342dd)
d256920 docs(handoff): fix stale references (commit count, HEAD, test count)
bb659c1 tools(defgen): broaden AFR matcher (implicit k=1 + paren variants)
6418998 docs(handoff): final session refresh — 32 commits, all non-linear formulas handled
2745e73 docs(handoff): empirical coverage benchmark vs RomRaider/Merp
33aa0d1 feat(ui+flash): Tools → Read ROM from Car (pre-OBDX)
```

### Tools → Read ROM from Car (commit `33aa0d1`)

Pre-staged GUI flow for the OBDX adapter (ETA May 22-25). After this
commit, tuner workflow on adapter arrival is:

  Tools → Read ROM from Car → pick adapter + COM5 → Read → save .bin

Three pieces:
- `Flasher::read_full_rom` extended with optional progress callback + cancel atomic; 4 new test cases.
- `render_read_rom_modal()` state machine (Idle → Running → Done / Failed / Cancelled) with background `std::thread`; ~440 lines in main.cpp.
- New `Tools` top-level menu (between Edit and View).

The C++ + UDS path goes all the way through MockTransport today. The
real-OBDX leg lights up when `st::transport::open_transport(Kind::Obdx, ...)`
gets its platform-specific USB-CDC implementation (waiting on hardware).

**Write-ROM plan** documented as a 50-line comment block above
`render_read_rom_modal` — route through existing `render_flash_modal`
policy gate, add vehicle-state preflight (battery > 12.0V, ignition,
transmission), background-thread `Flasher::execute` with per-sector
ledger, journal-resume modal. Implementation deferred until OBDX
validation lands.

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

### Two non-linear formula types shipped this session

**AFR enrichment (commits `74624d3` + `f202081`):**

The non-linear AFR enrichment formula `14.7/(1+x*0.0078125)` is now a first-class formula type across the stack:

- **C++**: new `SubaruAfrEnrichment` variant in `ScalingFormula`. `apply_scaling` evaluates `numerator / (1 + raw*k)` with a singularity guard. `invert_scaling` solves `raw = (numerator/value - 1) / k` with a degenerate-value guard. The TOML loader recognizes `formula = "subaru_afr_enrichment"` with optional `numerator`/`k` fields.
- **defgen**: `match_afr_enrichment()` regex tolerantly matches the canonical expression (and structurally-equivalent forms). `_scaling_from_element` tries the AFR match before falling through to `parse_toexpr`'s linear path. `ScalingRecord` gains optional `numerator`/`k` fields; `to_toml` emits the right field set per formula type.
- **bulk_regen sweep**: 323 packs across legacy/forester/impreza/baja/outback/exiga/tribeca/ecuparams regenerated. Every AFR-bearing scaling now emits `formula = "subaru_afr_enrichment"` instead of the prior flattened linear identity.

**Verification**: `dump-table primary_open_loop_fueling` on a2tb000l now shows **AFR values 4.91-14.70 (mean 11.16)** instead of raw 0-255 enrichment offsets. Engineering-meaningful tuning data.

15 new tests total (7 C++ + 8 Python).

**Inverse divide (commit `09342dd`):**

Same shape of fix for the reciprocal expression `N/x`. Found via the
UX-pass discipline: `injector_flow_scaling` (318-pack scaling) was
showing raw float 4916 cc/min — Subaru injectors are 380-2000 cc/min.
The canonical expression `2707090/x` converts to 550.67 cc/min, exactly
the OEM EJ255 injector flow rate.

- **C++**: `InverseDivideScaling { numerator }` variant. `apply_scaling`:
  value = numerator / raw with raw==0 guard. `invert_scaling`: raw =
  numerator / value with value==0 guard. Loader parses `formula = "inverse_divide"`.
- **defgen**: `match_inverse_divide()` regex for `N/x` or `N/(x)` patterns,
  tried after AFR matcher but before parse_toexpr's linear path.
- **bulk_regen sweep**: 323 packs re-emit through fixed defgen path.

Affects: `injector_flow_scaling` (318 packs, high-impact for tuners
replacing injectors), `gear_determination_thresholds_a/b/c` (122 packs,
lower priority).

13 new tests total (5 C++ + 8 Python).

## Coverage benchmark (vs RomRaider/Merp canonical XML)

Empirical measurement at end of session (commit `6418998`):

| Source | CID count |
|---|---|
| Merp canonical ecu_defs.xml (RomRaider open-source reference) | 332 |
| Our packs in `definitions/` (excluding ecuparams/) | 373 |
| **Intersection** | **332 (100.0%)** |
| **Forum-sourced extras** (newer than Merp mainline) | **41** |

Extras break down to:
- 18 VB WRX packs (LHBH/LHBT/LHBK/LHBP series, 2022-2026 model years)
- 9 VA WRX packs (LF75/LF79/LF9C/LF9D/LF9G/LF9L/LV9N, 2015-2021)
- 14 misc EJ-era extras (cousin-seeds + community-only CIDs like ez1g/ez1d/e2vg)

**Stance:** for EJ-era Subaru we are at parity with the RomRaider open-source standard. For VA/VB we are essentially peer with the community-aggregated forum-sourced definitions (since our bundles ARE those definitions ingested). VB 2026-model-year coverage may actually lead community-RomRaider-mainline.

Concrete per-pack richness (post 875ed2f + 09342dd + bb659c1 sweeps):

| family | packs | tables/pack | 2D tables/pack |
|---|---|---|---|
| VA WRX | 9 | ~290 | ~144 |
| VB WRX | 18 | ~1100 | ~399 |
| EJ-era | 346 | varies | varies |

## Status snapshot

- **HEAD `33aa0d1`**, in sync with `origin/main`.
- **definitions/ pack count: 373** (one promoted from throwaway; net unchanged).
- **defgen test suite: 236 tests green**.
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
| after broader comp patterns + 2D comp (07c8cc3) | 31 | 6 | 194 | 159 |

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

**(P6e shipped this session)** — full stack support for `subaru_afr_enrichment` formula. Future extension: VA bundle uses different fueling-table naming and possibly different AFR formula variants; the matcher could be extended to cover those if needed.

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

- **User-experience pass is the complementary verification to validate.py.**
  validate.py checks predicate matches against raw bytes; it can't see
  scaling/units bugs (raw bytes that fall in any predicate band coincidentally
  but display as garbage after scaling). The session-end ritual: `dump-table`
  ~8 top-of-mind tables (boost target, max wastegate, base timing, AFR,
  knock correction, MAF cal, ECT comp, throttle DBW) on the user's primary
  test pack and eyeball that the ranges look engineering-sensible. Today's
  pass (commit `6a4693a`) found 2 real bugs validate.py missed: MAF scaling
  showed 1e35 g/s (float-endian gap in `_scaling_from_element` — the
  `_axis_from_element` had the override but the scaling path didn't), and
  a2tb002c AFR showed 0-67 raw because cousin re-seed in `c8db6c5` ran
  BEFORE the AFR formula commit `74624d3`. Fixed both.
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

> "HEAD `33aa0d1`, in sync with `origin/main`. Marathon yesterday: 24 commits (including this handoff refresh). P6 descriptor library bootstrapped (9 predicates + validate.py), VA/VB cid_address fixed on 5 packs, base_timing+AFR dtype fixed at defgen root cause and bulk_regen-swept across 323 packs, 11 cousin-seeds re-seeded, and 25 VA/VB packs upgraded from 0D-only to fully-typed 2D via two new forum bundles (va_wrx_bundle.xml + vb_wrx_bundle.xml) wired into bulk_regen.
>
> Deck for this session:
> **(P6a-continued — DEPRIORITIZED)** Fuel-comp scalings outside the ±100% family (`_x_003051758_100`, `_x_000224304213_7_35`). UX pass on a2tb001c showed they already display correctly via the linear-scaling path (enrichment offset −100..0%, AFR points ±7.35). Adding predicates would only improve validate.py coverage metric, not user-facing dump-table. Drop unless coverage % becomes a stakeholder concern.
> **(UX-pass)** Session-end ritual now established (commit `6a4693a`): `dump-table` ~8 top-of-mind tables on the primary test pack and eyeball ranges. Cross-family validation complete this session — Legacy GT cousin-seed (a2tb002c), Legacy regen (a2tb001c, a2tb000l), Forester regen (A2WC400H), Outback regen (a2wc500r) — all five came back clean across boost target, max wastegate, base timing, AFR, knock correction, MAF, injector flow, comp tables. The EJ-era corpus is in good shape; future scaling-change commits should re-run this on at least one pack.
> **(P6b)** More common axes: manifold_pressure_axis, throttle_plate_opening_angle_axis, mass_airflow_axis, atmospheric_pressure_axis. Each ~30 lines including tests, but each needs per-axis scaling investigation.
> **(P6e-VA — investigated, NOT a bug)** VA bundle's `Fuel - Open Loop - ... Target Base` tables use linear expression `((x)/256.0)+1.0` → displays lambda 1.0-1.46 with unit "AFR". This is correct per Merp's XML convention: VA-era fueling architecture decomposes the rich-target into BASE (stoich-to-lean lambda 1.0-1.5) + separate enrichment tables at high load. The EJ `14.7/(1+x*K)` formula doesn't apply. defgen correctly extracts the linear factor 0.00390625 + offset 1.0 and dump-table shows the right values per the source convention.
> **(P6h-next)** Forum-source matching-revision `.bin` files for the upgraded VA/VB packs if you want to validate them against real anchors.
>
> Or pivot — OBDX adapter ETA May 22-25, could arrive mid-session. If hardware lands, pause P6 and pre-stage the first-light rom-pull."
