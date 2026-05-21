# Handoff — 2026-05-20 end-of-day (CLI audit + VA/VB categorization)

Continuation of an already-long session. After the morning's 11-pack cousin-seed arc + localize.py v2 work (snapshotted at `5473115`), the day continued through a wide CLI-audit pass (eight commands cleaned up), a rom_size_bytes regression sweep with three layers of defensive fix, the bulk_regen `--dry-run` upgrade, and finally a VA/VB category-inference tool that retroactively gave 25 sparse packs navigable structure. User was GUI-testing with a2tb002c at end-of-session.

**HEAD `78de957`**, in sync with `origin/main`. **373 .toml packs in `definitions/`**, all axis-validated. **Working tree clean** apart from `SubaruTuner.zip` (untracked 114 MB), `definitions/impreza/lf79100p.toml` (untracked cousin-seed for VA GUI testing), and `fixtures/projects/` (untracked, no idea what that is — may be a previous session leftover).

## What shipped since the last handoff (5473115, top = newest)

```
78de957 defs(packs): infer categories for 25 VA/VB packs via tools/defgen/categorize
4cab7e6 fix(cli): "table/axis not found" errors now suggest similar ids
3ce83f1 fix(cli): primitive-list --type does case-insensitive match
d7bdd49 fix(cli): table-list --category does case-insensitive substring match
eb63698 fix(cli): dump-axis summary + rom-diff sort by max delta
bd311ca fix(cli): rom-info — category histogram in the def-pack summary
22daea3 fix(cli): dump-table — at-a-glance summary line (cells/min/max/mean/zeros)
1a5eca3 fix(cli): rom-info ASCII listing — filter noise + sort by length
b5ccce3 tools(defgen): bulk_regen --dry-run actually previews changes
f09c2ae tools(defgen): bulk_regen preserves manually-patched [pack] fields
4c44a68 docs(handoff): note rom_size_bytes regression sweep + defgen default
8ea6d71 fix(defgen): default rom_size_bytes to 1MB when XML lacks <filesize>
0008695 fix(defs): restore rom_size_bytes=1048576 on ez1gc00c + ep5g600a
```

For everything before `5473115` see `git log 5473115` — that's the cousin-seed arc + LF79 audit + localize v2 + axis fix.

## Today's substantive arcs (chronological condensed)

1. **11 new packs** via cousin_seed + localize (`80a2a0d`, `897e53b`, `b3a5e14`, `261f5f8`). Same-trim same-region tens/ones-digit deltas work cleanly; cross-region / hundreds-delta drop.

2. **LF79 audit** — only 1 of 31 LF79xxx files in `decrypted/` is a real anchor (LF79100P). 30 are promoted partials with encrypted cal bodies. Pattern repeats across LF7x/LF9x/LV9N. RECON.md (gitignored) + memory `project_lf79_partial_decrypts.md`.

3. **localize.py v2** (`70773de`, `de647ba`, `3eb6202`): `--relocate-low` pattern-search relocator → `--patch-pack` in-place rewriter → axis-tracking + axis-aware classifier. 22 unit tests.

4. **Fake-anchor sweep** (`68c2f02`): 9 of 99 PAK_* entries in bulk_decrypt_v2 disabled (plaintext at path was itself a partial).

5. **Re-patched 6 cousin-seed packs** (`5598683`, `1df935b`) after dump-table validation exposed the axis-blindness bug.

6. **CLI pack-discovery bug fix** (`21f89c2`): pack-list + rom-identify finally see single-file packs. Was returning 0 matches against the entire `definitions/` tree.

7. **rom_size_bytes regression sweep** (`0008695`, `8ea6d71`, `f09c2ae`): two packs silently wiped by bulk_regen, plus defensive default + preservation layer to stop the class.

8. **CLI UX audit pass** — eight commands cleaned up (rom-info x2, dump-table, dump-axis, rom-diff, table-list, primitive-list, all "table not found" sites). Pattern that kept holding: commands shipped fast, never exercised heavily, small papercuts accumulated. Each fix improves first-5-minutes-with-the-tool experience.

9. **VA/VB category inference** (`78de957`): hand-tuned rule table maps id-prefix patterns to category strings. 100% coverage on all 25 VA/VB packs in `definitions/impreza/` (22,067 categories inferred, 0 unmatched). Makes `table-list --category boost` actually work for VA/VB packs (was returning 0).

## Status snapshot

- **HEAD `78de957`**, in sync with `origin/main`
- **definitions/ pack count: 373** (up from 361 at session start, +12 net — includes the 11 cousin-seeded + the implicit categorization of all VA/VB packs)
- **defgen test suite: 152 tests green** (22 localize + 11 bulk_regen + ~119 defgen/cousin_seed/loggergen)
- **C++ build: clean** (cli rebuilt many times today, all green)
- **CI clang-format gate: required** — applied each time the CLI was edited
- **All 706 .toml files under definitions/ load cleanly** (verified via `pack-list definitions --quiet`)

Pre-existing C++ test failures (~80) exist on `main` due to a Windows-mingw + ctest interaction with non-ASCII test names (`→`/`—` mangle into `ΓåÆ`/`ΓÇö` when ctest invokes the test binary). Not a regression from any of today's work — verified by stashing changes and running on plain `main`. CI on Linux/MSVC presumably handles UTF-8 fine since the historical handoffs all said "807 tests green".

## What the user was doing at end-of-session

GUI-testing with the Legacy combo. Confirmed paths I gave them:
- Pack: `definitions/legacy/a2tb002c.toml`
- ROM: `fixtures/private/plaintext_corpus/bludgod-roms/USDM/Legacy/A2TB002C-2009-USDM-Subaru-Legacy-GT-AT.hex`

Plus left a throwaway `definitions/impreza/lf79100p.toml` (untracked) for them to test VA-side GUI behavior against `fixtures/private/roms_extracted/decrypted/LF79/LF79100P.bin`.

The conversation closed on me explaining VA/VB coverage paths (P1 hardware-wait, P2 cousin-seed-existing-anchors, P3 just-landed-category-inference, P4 source-more-XMLs).

## Open threads (for tomorrow)

### Likely-to-surface from the GUI session

- **GUI bugs / feedback from a2tb002c session** — user may bring observations. The new dump-table / table-list / rom-info displays I added today aren't wired through the GUI (CLI-only). If the GUI feels less informative than the CLI, that's where to look.
- **VA-side GUI testing on lf79103p** — now that categorization landed, the sidebar tree should have 26 categories. Confirm visually.

### P2 — cousin-seed remaining VA packs (concrete, today's tools)

We have 5 VA real-anchor ROMs but only 2 of them have matching packs (lf79103p was already present; lf79100p I cousin-seeded as a throwaway). The other 3 still need packs:

| Anchor ROM                  | Sibling pack to seed from | Suggested CID |
|----------------------------|---------------------------|---------------|
| LF75300E (in `decrypted/LF75/`) | lf75404h or lf75404s | lf75300e      |
| LV9N100A (in `decrypted/LV9N/`) | lv9n001d              | lv9n100a      |
| LV9N303J (in `decrypted/LV9N/`) | lv9n001d              | lv9n303j      |
| LF9C000C (in `decrypted/LF9C/`) | lf9c102p              | lf9c000c      |
| LF78001C (in `decrypted/LF78/`) | (no LF78 sibling)     | — needs LF78 sourcing first |

For each: `cousin_seed.py` → `localize.py --patch-pack` → metadata fixup → commit. Same flow as today's 11 packs. Each lands ~290-300 categorized tables (the categorize tool runs on these too).

Also: promote the throwaway `definitions/impreza/lf79100p.toml` to a real commit if the user's GUI test confirmed it loads OK.

### P3 — docs/21-oem-baselines.md (deferred 5×)

Still on the deck. Empirical OEM behavior reference doc derived from the corpus. Needs RE work (load packs + read specific table addresses across ROMs, tabulate medians). Constraint from the LF79 audit: use only RELIABLE ROMs (bludgod corpus + the high-anchor decrypted/ families).

Honest read: this needs a fresh session with full focus. Five deferrals in a row says the work is too heavy to slot into an end-of-session "small slice" pick.

### P4 — Axis-fingerprint relocator (localize.py v3)

The current `--relocate-low` catches byte-identical address shifts (typically 10-20% of LOW entries on cross-revision packs). Recalibrated tables miss. An axis-fingerprint matcher (monotonic windowed match against the sibling axis values) could catch axes that moved even when their data tables were recalibrated. Speculative payoff — wouldn't push today's 4 dropped P1 candidates (az1g701v/710v/601r, a2wc400l) to commit-worthy, but would chip at the edge.

### P5 — Pack-format extension for `[[table.role]]`

§11 panels (knock dashboard, adaptive history, cold-start, EBCS) surface advisory suggestions but don't apply them. Documented in `docs/05` §11.X as the v1.2 path. Substantial C++ work — schema extension, Definition loader, edit::History routing, lint wiring, UI integration.

### P6 — Cal-table descriptor library (def-pack acceleration, deterministic)

End-of-session question from the user: "should we start on `docs/20` Tier 5 (AI-driven def-pack-acceleration) sooner?" Read-through verdict: **no — Tier 5 stays gated to v2.x as documented**, but the prep work that pays double *should* start now.

The pitch: codify "what a real cal table looks like" as a library of runtime predicates. Examples:

  - "Engine-speed axes are monotonic floats, 600-7000 RPM range, ~10-20 points"
  - "Wastegate-duty maps are 11×11 uint16_be, value range 0-10000 (0-100% × 100)"
  - "AFR target tables are bounded 9.0-22.0 with consistent per-row monotonicity"
  - "EGT compensation tables have negative slopes vs temperature"
  - "DTC threshold tables are scalar uint16 in 0-65535 raw range"

Each descriptor: `(predicate, source_evidence_examples, table_id_patterns)`. Library is hand-curated from observed-real packs (a2tb002c, ez1d* family — packs we know are correctly categorized).

What this pays off immediately (today's tooling tier):

  - **Relocator gets smarter.** `is_this_actually_a_boost_target_table?` instead of just "byte-pattern matches sibling". Cuts the false-positive HIGH count we saw on ez1g109j (where the relocator's classify_pair said HIGH on bytes that were really a different table starting nearby).
  - **Categorize gets smarter.** Can infer 2D shape + axis assignment from neighboring tables ("this 256-byte run between known axes X and Y is probably a 16×16 fuel map"). Could uplift the VA/VB packs from 0D-scalar-only to inferred 2D where the byte distribution matches.
  - **New cousin-seed validation pass.** After patch-pack, run descriptor checks: "table claims to be 'engine_speed_axis' but bytes don't satisfy monotonic-float 600-7000 — reject and flag for manual review."

What this pays off later (v2.x Tier 5 / Tier 7):

  - Each descriptor + example set is **labeled positive training data** if/when we build an ML model. The hand-curation is the expensive part of any ML pipeline; doing it via predicates first makes the data available either way.
  - Clean-room safe: derived from public RR XML facts + observed bytes in our reliable-anchor corpus (per the LF79 audit's RELIABLE set: bludgod + high-anchor decrypted/ families).

Implementation rough shape — Python first to match the rest of `tools/defgen/`:

  - `tools/defgen/descriptors.py` — predicate library + curator script
  - `tools/defgen/validate.py` — runs descriptors against a (pack, ROM) pair, reports table-by-table "matches expected shape / doesn't"
  - localize.py + categorize.py grow optional `--use-descriptors` flag for the smarter-relocation / smarter-shape-inference paths

Sizing: descriptor library bootstrap is probably 30-50 hand-written predicates for the most common Subaru table types. Tractable in 2-3 sessions of focused work. The validator + relocator/categorize integration is another 1-2 sessions.

Why NOT to start Tier 5 ML now:
  - Tier 5 requires the v2.0 Backend abstraction, Backend::info() provenance metadata, the prompt-confirmation UI, and a clean training corpus pipeline — none built yet.
  - Tier 5 is also LOWER priority than Tiers 1-3 (drift classifier, LLM explanation, explain-this-log assistant) in the doc's own ordering. Starting Tier 5 first inverts the architecture.
  - The deterministic descriptor library captures the same value with no ML infrastructure cost AND no training-data clean-room work. If the descriptor library proves the value (or its limits), THEN the case for ML is empirical, not speculative.

### Honest acknowledgement: where ML probably IS the right tool eventually

End-of-day discussion clarified the user's framing: ML to **expand coverage where data is sparse**, not just "improve what's known". The descriptor library answers the latter; for the former, ML has a real place. Two specific applications where the data-sparsity argument is strongest, ranked:

1. **Cipher / encryption-bucket classification (Tier 8 in `docs/20`).** Currently in the doc's "research" bucket but probably the highest-leverage ML application for the data-sparse case. The LF79 audit today proved encryption-bucket understanding is the bottleneck for the entire VA/VB family — only LF79100P is a real anchor among 31 LF79xxx, only 5 real anchors across the entire 2MB FA-DIT bucket. Every new ROM family that uses a different encryption scheme currently needs manual RE of the cipher (per_family_xor / per_CID layer). ML over byte-distribution features could fingerprint encryption schemes empirically and short-circuit a lot of that work. **One good cipher-fingerprint tool unlocks more cal coverage than a hundred def-pack drafts.** Worth promoting from "research" to "v2.x reachable" in the doc when we next revise it.

2. **Pack drafting for un-XML'd CIDs (Tier 5).** ML pattern recognition over byte distributions could say "this 0xC1000-0xC1FF region IS a fuel map" even where we have no sibling pack and no source XML. Descriptor library helps a little (scan for fuel-map-shaped regions) but ML scales better across the long tail of CIDs we'll never have RR XMLs for. Justified after the descriptor library establishes the deterministic ceiling.

The stance update: P6 (descriptor library) is the right next step because it sets the empirical baseline. But the long-arc thinking should NOT assume "deterministic is always enough" — for the genuinely-sparse cases (sparse CID coverage, undocumented ciphers), ML is the eventual answer. The descriptor library gives us the data to know when to cross that bridge.

If the user opens a future session with a "we're hitting the deterministic ceiling" observation, the right answer is to revisit `docs/20` Tier 8 first, Tier 5 second.

### Hardware ETA: OBDX Pro VX adapter, May 22-25 2026

Two days minimum from this handoff. First-light command pre-staged:

```
subuwutuner-cli rom-pull --transport obdx --device COM5 \
    --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
```

Win32 USB-CDC layer at `c64b717`; codec + transport + SSM/UDS clients tested against MockTransport. The dump will be the user's current COBB-tuned cal (bypassing the COBB AppData encryption) — also doubles as the first real LF79103P anchor we'd own, unlocking the LF7910 family in `bulk_decrypt_v2`.

### Forum thread to mine — 2017 WRX engine bin (carried over from 2026-05-19)

<https://mhhauto.com/Thread-2017-Subaru-WRX-need-engine-bin-file>. May contain a clean LF79xxx CID. The P2 LF79 audit confirmed every additional LF79xxx anchor multiplies into ~150 partial→full decrypts in that bucket. Login cookies at `D:\Documents\atlas-personal\forumdownloads\cookies.txt`.

### Investigate `fixtures/projects/` (untracked working-tree clutter)

Showed up in `git status` at end of session. Don't know what created it — could be a stray .stune project from a CLI test I ran (project-new perhaps). Glance + delete if it's empty/junk, leave alone if it's user state.

## Carry-over lessons (memory entries created today)

- **Always dump-table-validate cousin-seeded packs before committing** (`feedback_cousin_seed_axis_validation.md`) — localize.py's HIGH/MED summary isn't enough; axes can be wrong even when data tables are correct. Run dump-table on a 2D table and verify axis labels look sensible (RPM thousands, lambda 0-2, etc.).
- **decrypted/ ROMs are mostly partials** (`project_lf79_partial_decrypts.md`) — only bulk_decrypt_v2 CONFIRMED-list anchor sources have real cal bodies; partials have correct bootloader bytes but encrypted cal regions.

## House-style notes (carry-over)

- Terse. No trailing summaries.
- "Proceed" / "Continue" = next narrow thing OR pick a next slice.
- Push per-commit. Caveman-style messages.
- Modal failure feedback goes inline in the modal, not the status bar.
- UI/UX: intuitive + non-intimidating + modern + beautiful + functional, equally weighted.
- Accent purple `(0.55, 0.35, 0.85)` via `accent_for(Theme)`.
- `/` in bash paths; `\` in Windows-path strings.
- NEVER `rm -rf` directories that may hold user files.
- Don't `git add -A` blindly — `SubaruTuner.zip` (114 MB at repo root) will sweep in and break the push.
- GUI not smoke-testable by Claude (no display). State explicitly when something ships unverified.
- Action buttons must complete the action.
- clang-format gate is required. Binary at `C:\Users\Cornelio\AppData\Roaming\Python\Python314\Scripts\clang-format.exe` — invoke by full path.

Session-style additions from today:
- **The cousin_seed sweet spot is ones/tens-digit deltas within same trim + region.** Cross-trim (STI↔WRX), cross-region (USDM↔ADM), and hundreds-digit deltas all fail. Drop rather than commit a high-LOW pack.
- **Post-seed metadata fixes are manual.** cousin_seed doesn't update years/transmission from filename hints; patch them after.
- **VA/VB packs are 0D-scalar-only.** Inherent to the sparse forum-sourced XMLs. The `78de957` category-inference makes them GUI-navigable but doesn't conjure 2D maps that aren't in the source.
- **CLI-audit pattern works.** Every command I exercised surfaced either a bug or a UX gap. Worth continuing — see eight commits 1a5eca3 through 4cab7e6 for the cadence.

## Suggested opener for next session

> "HEAD `78de957`, in sync with `origin/main`. 373 packs in `definitions/`, all axis-validated AND VA/VB-categorized. Working tree clean apart from `SubaruTuner.zip` + `lf79100p.toml` (throwaway VA GUI-test cousin-seed). 152 defgen tests green; C++ build clean.
>
> Last session was a marathon — 30+ commits across cousin-seed packs, localize.py v2 + axis-tracking, LF79 audit, CLI UX audit (8 commands), rom_size_bytes regression three-layer fix, bulk_regen --dry-run upgrade, VA/VB category-inference tool. End of session, user was GUI-testing with a2tb002c + the Legacy ROM.
>
> On the deck for this session:
> **(P1)** Anything that surfaces from the GUI test (likely user feedback or bug reports).
> **(P2)** Cousin-seed the 3-4 remaining VA packs from existing anchor ROMs (lf75300e, lv9n100a, lv9n303j, lf9c000c). Today's tools handle this in batch — same pattern as the 11-pack arc.
> **(P6)** Cal-table descriptor library — bootstrap 30-50 hand-written predicates (RPM-axis shape, wastegate-duty shape, AFR-target shape, etc.) that runtime-validate "is this table what we think it is?". Pays off immediately (smarter relocator/categorize/validate) AND becomes labeled training data if/when we want Tier 5 ML in v2.x. **See the P6 section above for the full rationale on doing this instead of starting docs/20 Tier 5 work.**
> **(P3)** `docs/21-oem-baselines.md` (deferred 5×). Heavy doc work, needs focused session.
> **(P4)** Axis-fingerprint relocator v3 — marginal payoff per today's analysis (mostly subsumed by P6 if P6 lands first).
> **(P5)** `[[table.role]]` pack-format extension for §11 panel-to-pack routing (substantial C++).
>
> Adapter ETA May 22-25 (any day now). If it lands mid-session, pivot to the first-light `rom-pull` command pre-staged at `c64b717`."

If the user opens with hardware news:

> "OBDX landed? First post-arrival command:
>
> ```
> subuwutuner-cli rom-pull --transport obdx --device COM5 \
>     --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
> ```
>
> That dumps your CURRENT (COBB-tuned) calibration unencrypted, bypassing the COBB AppData encryption. Win32 USB-CDC layer pre-staged at `c64b717`. Battery > 12.0 V before connecting. The dump is read-only — no flash, no write — so safe with engine off.
>
> Bonus: once dumped, that LF79103P file becomes the FIRST real LF7910-family anchor we'd own — drop it at `fixtures/private/roms_extracted/decrypted/LF79/LF79103P.bin`, wire it into `bulk_decrypt_v2.py` as a CONFIRMED anchor, and re-run to unlock the ~150-cipher LF7910 family (validates the lf79103p pack at the same time)."

If the user opens with GUI feedback:

> "What did the GUI show? The new CLI displays from today (rom-info category histogram, dump-table summary, dump-axis monotonicity check, "did you mean" suggestions) aren't wired through the GUI yet — if you noticed it feels less informative than the CLI, that's the gap to close. Specific bugs go through the same flow as any other src/ui/ work."
