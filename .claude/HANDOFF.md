# Handoff — 2026-05-20 (P1 cousin-seed + P2 LF79 audit + localize.py v2 + fake-anchor sweep)

Continuation of the 2026-05-19 handoff (§11 four-play arc, OBDX prep, tooling). This session executed all three deferred items from the prior plan plus the bonus of `localize.py` v2: ran the `cousin_seed.py → localize.py` pipeline against bludgod-corpus CIDs without pack coverage (6 new packs landed, 4 dropped where the cousin distance was too wide); ran the LF79xxx partial-decryption audit (one anchor confirmed, 30 partials catalogued, broader same-pattern problem identified across most FlashWrite buckets); built the pattern-search relocator for `localize.py` with 12 unit tests; swept all 99 anchor entries in `bulk_decrypt_v2.py` for fakes (9 disabled). **HEAD `68c2f02`**, in sync with `origin/main`. **Working tree clean apart from `SubaruTuner.zip`**.

## What shipped this session (top = newest)

```
68c2f02 fix(fixtures/private): comment out 9 fake-anchor PAK_* entries
70773de tools(defgen): localize.py --relocate-low pattern-search relocator
852198b docs(handoff): land 2026-05-20 session (P1 cousin-seed batch + P2 LF79 audit)
80a2a0d defs(packs): cousin-seed 6 new USDM packs from bludgod gap
```

Four commits on the public side (one is the mid-session handoff). P2 produced a private-side artifact (`fixtures/private/roms_extracted/decrypted/LF79/RECON.md`, gitignored) and a memory entry (`project_lf79_partial_decrypts.md`).

## The substantive arc

### 1. P1 — cousin-seed 6 new packs from bludgod gap

Cross-referenced bludgod USDM corpus (498 ROMs) against `definitions/` (358 packs as of HEAD start), producing a per-platform gap inventory:

```
impreza    have=177  bludgod-usdm=97   gap=40
forester   have=62   bludgod-usdm=35   gap=15
legacy     have=68   bludgod-usdm=47   gap=28
outback    have=13   bludgod-usdm=26   gap=17
baja       have=10   bludgod-usdm=11   gap=3
tribeca    have=4    bludgod-usdm=4    gap=3
```

Filtered to 10 high-confidence candidates (gap CIDs whose 4-char family already had ≥3 sibling packs). Ran `cousin_seed.py` + `localize.py` against bludgod-USDM ROM pairs:

| Target | Sibling pack | Sibling ROM | HIGH | MED | LOW | Verdict |
|---|---|---|---|---|---|---|
| a2tb002c | a2tb001c | A2TB001C bludgod | 99.7% | 0.3% | 0% | ✅ committed |
| az1g702i | az1g701i | AZ1G701I bludgod | 99.7% | 0.3% | 0% | ✅ committed |
| a2wc500s | a2wc500r | A2WC500R bludgod | 57.7% | 33.9% | 8.4% | ✅ committed |
| a2wc501k | a2wc501l | A2WC501L bludgod | 48.3% | 44.2% | 7.5% | ✅ committed |
| ez1d301a | ez1d302b | EZ1D302B bludgod | 49.2% | 42.1% | 8.6% | ✅ committed |
| ez1d303b | ez1d302b | EZ1D302B bludgod | 69.2% | 25.9% | 4.9% | ✅ committed |
| az1g701v | az1g701i / az1g401v | AZ1G701I / AZ1G401V | 0.9% / 2.8% | ~42% | ~56% | ❌ dropped |
| az1g710v | az1g701i / az1g401v | AZ1G701I / AZ1G401V | 0.9% / 2.4% | ~42% | ~56% | ❌ dropped |
| az1g601r | az1g101r | AZ1G101R bludgod | 2.8% | 40.7% | 56.6% | ❌ dropped |
| a2wc400l | a2wc500l / a2wc501l | bludgod | 1.7% | 50.3% | 48% | ❌ dropped |

Post-seed metadata patches: years (2008↔2009) and transmission (AT↔MT) corrected per filename when target differed from sibling. Each new pack carries the `COUSIN-SEED` header so the inherited-unverified status stays visible to anyone opening the file.

**Lesson:** `cousin_seed` earns its keep on ones/tens-digit CID deltas within the same trim. Trim crossovers (STI↔WRX = different ECU hardware) and hundreds-digit deltas (model-year/major-revision jumps) hit real address shifts — needs the pattern-search relocator that's still v2 on `localize.py`.

`definitions/` count: 358 → 364 → 367 (counting all subdirs).

### 2. P2 — LF79xxx decryption-quality audit

Investigated the LF79101P open thread from 2026-05-19 (random-looking bytes at calibration offsets). Cross-referenced every `LF79*.bin` in `decrypted/LF79/` against:

- `bulk_decrypt_v2.py` CONFIRMED anchor list (which files are claimed as plaintext anchors)
- `decrypted_v2/partial/<anchor>/<cid>.bin` outputs (md5 match → file is a promoted partial)
- `bulk_decrypt_v2_report.md` per-bucket totals

**Finding:** Out of 31 LF79xxx files in `decrypted/LF79/`:

- **1 real anchor**: `LF79100P.bin` (from ECUTune purchase, zip-on-disk source)
- **30 partials**: every other LF79xxx, including `LF79120P.bin` which is listed as the plaintext source for the `PAK_LF79120P` anchor at `bulk_decrypt_v2.py:369` — fake-anchor (file at that path is itself a partial)

Per-bucket evidence from the v2 report:
```
## LF79100_family
- Bucket 2,098,176B has 186 ciphers; 1 full / 167 partial / 18 random
  Full decrypts: lf79100p_ori.hex
```

Direct byte-level confirmation at known cal offsets:
```
@ 0x18F92  fuel_open_loop_avcs_disabled_target_base_tgv_open
  LF79100P:  00 00 00 00 00 66 66 66 66 66 66 66 66 66 66 66  (real fuel map)
  LF79101P:  5d 38 ca 47 c9 2c 9f 64 21 ad 53 a4 3d d6 a7 88  (random)
  LF79120P:  5d 38 ca 47 c9 2c 9f 64 21 ad 53 a4 3d d6 a7 88  (identical to 101P)
```

Bootloader region (`0x2010-0x2050`) is byte-identical across all sampled LF79xxx — keystream is right in non-cal regions; per-CID XOR layer covers only the cal body.

### 3. Broader audit — same pattern across most FlashWrite buckets

Expanded the audit to all 48 family subdirs under `decrypted/`. Most LF7x, LF9x, LV9x families are dominated by promoted partials:

```
LF79: 31 files, 2 listed-anchors (1 real, 1 fake), 29 partials
LV9N: 44 files, 2 anchors, 41 partials
LF75: 45 files, 4 anchors, 38 partials
LF76: 12 files, 0 anchors, 12 partials (entirely partial)
LF77: 3 files, 0 anchors, 3 partials
LF9B: 4 files, 0 anchors, 4 partials
LF9C: 23 files, 3 anchors, 18 partials
LF78: 21 files, 2 anchors, 15 partials
```

vs. some families that are mostly clean (ECUtune-bought + pak-decoded anchors covering most of the corpus):
```
EZ1G: 10 files, 10 anchors, 0 partials
EA1T: 5 files, 5 anchors, 0 partials
EA1U: 3 files, 3 anchors, 0 partials
DE5M: 2 files, 2 anchors, 0 partials
ZA1J: 3 files, 3 anchors, 0 partials
```

Captured in `RECON.md` (private-side; gitignored) and in a project memory entry (`project_lf79_partial_decrypts.md`) so future sessions don't re-derive the wall.

### 4. localize.py --relocate-low pattern-search relocator (`70773de`)

When a `[[table]]` entry classifies as LOW, often the table moved within the target ROM rather than being recalibrated. New `--relocate-low` flag does the search: anchor on the sibling's first 16 bytes; `bytes.find` in target ROM (C-optimized, O(N)); back up by anchor offset if a uniform prefix forced an interior anchor (8/16/24/32). Each candidate is dtype-aligned and re-scored via `classify_pair`; only HIGH/MED hits survive. Ties broken by confidence then distance from the original address. `--relocate-max-distance` caps the search radius (default 64 KB).

TSV output gains two columns (`relocated_to`, `relocate_reason`) when the flag is set. Default behavior unchanged when off — regression-checked against `a2tb002c` (still 99.7% HIGH).

Empirical results on the 4 cousin-seed candidates dropped earlier today:
```
az1g701v: 31/183 LOW resolved (14% HIGH + 3% MED) -> 53.5% HIGH+MED total (was 44.0%)
a2wc400l: 32/141 LOW resolved (16% HIGH + 7% MED) -> 62.9% HIGH+MED total (was 52.0%)
az1g601r: 33/185 LOW resolved (17% HIGH + 1% MED) -> 53.5% HIGH+MED total (was 43.4%)
az1g710v: 24/197 LOW resolved ( 2% HIGH + 10% MED) -> 50.1% HIGH+MED total (was 43.2%)
```

None reach the 80% commit threshold — the remaining LOW entries are genuinely-recalibrated tables (model-year or trim changes), not address shifts. The relocator handles the pure-shift subset cleanly; recalibration is unavoidable manual RE. The `+0x54c` shift recurring across az1g701v is a textbook example of a whole sub-region of the ROM having moved by the same offset — exactly the pattern this tool is good at catching.

12 unit tests in `tools/defgen/tests/test_localize.py` cover exact match, interior anchor fallback, distance cap (accept + reject), dtype alignment, closest-wins tie-break, and the helpers. Full defgen suite: 130 tests, all green.

### 5. Fake-anchor sweep in bulk_decrypt_v2.py (`68c2f02`)

For each of the 99 CONFIRMED anchor entries in `bulk_decrypt_v2.py`, md5-checked the plaintext file at its path against the entire `decrypted_v2/partial/` tree. 9 entries point at files that are themselves promoted partials from prior family-anchor passes — the "plaintext" they reference still has its per-CID XOR layer in the calibration body. Disabled (commented out, with re-source path noted):

```
PAK_LF75600S, PAK_LF75500A, PAK_LF75500G
PAK_LF78200B
PAK_LF9C200B, PAK_LF9C300P
PAK_LF79120P
PAK_AF56E03B
PAK_LF61803B
```

All 9 are from the 2026-05-19 70-anchor pak-derived batch. Original 5 forum-anchored entries and the per-family anchors (LF75300, LF78001, LF9C000, LV9N100, LV9N303) are real and stay active. Active anchor count: 99 → 90.

## Status snapshot

- **HEAD `68c2f02`**, in sync with `origin/main`
- **definitions/ pack count: 367** (up from 361 at session start)
- **Working tree clean** apart from `SubaruTuner.zip` (untracked 114 MB; gitignore-equivalent — never `git add` it, would break GitHub's 100 MB push limit)
- **CI clang-format gate: required** (no C++ touched this session)
- **defgen test suite: 130 tests green** (incl. 12 new for the relocator)

Did not re-run `ctest` this session — no C++ changes, only TOML data + Python tooling + private-side script comments. If asserting C++ test-green is needed before the next code change, run the full suite.

## Open threads / known issues

### Family-granularity claim in bulk_decrypt_v2.py docstring is loose

The docstring states the recovered keystream decrypts "other ciphers in
the SAME 6-char-prefix family". Empirically, LF79100P (anchor) does NOT
fully decrypt LF79101P or LF79120P — both share the 6-char prefix
`LF7910`/`LF7912` and would be expected to fully decode if the docstring
were right. The actual family granularity is tighter (at least 7 chars,
possibly variable). Not a blocker but the docstring should be updated to
reflect empirical truth from the bucket report.

### Cross-revision cousin packs still need manual RE for recalibrated tables

The `--relocate-low` relocator catches the address-shift subset of LOW
entries (typically 10-20% of LOW on a cross-revision pack). The
remaining 80% are tables whose contents fundamentally differ — model-
year recalibrations the relocator can't handle by byte search. A
follow-on tool could fingerprint by axis structure (axis tables are
monotonic and often unique enough to match across recalibrations) but
it's a separate body of work.

The 4 dropped P1 candidates from today (az1g701v, az1g710v, az1g601r,
a2wc400l) remain unworkable via pure tooling — they'd need either a
new sibling pack closer to the target or hand-RE on the diverged
tables.

### Forum thread to mine — 2017 WRX engine bin (carried over)

User flagged at end of 2026-05-19 session:
<https://mhhauto.com/Thread-2017-Subaru-WRX-need-engine-bin-file>. May
contain a clean LF79xxx CID we don't have a real anchor for. P2 audit
confirmed this matters more than originally framed — every additional
LF79xxx anchor multiplies into ~150 other partial decrypts becoming
full. Login may be required; cookies jar at
`D:\Documents\atlas-personal\forumdownloads\cookies.txt` worked for
romraider.com earlier.

### §11 panels surface suggestions but don't apply them (carried over)

`docs/05` §11.X documents the v1.2 path (pack-format extension for
`[[table.role]]` strings, route through `edit::History` with the
engine-safety linter). Until that lands users find tables manually in
the sidebar.

### OBDX adapter ETA: May 22-25 2026 (carried over — imminent)

Two days minimum from this handoff. First-light command pre-staged:

```bash
subuwutuner-cli rom-pull --transport obdx --device COM5 \
    --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
```

Win32 USB-CDC layer landed at `c64b717`; codec + transport + SSM/UDS
clients already tested against MockTransport.

## Plan for the next session

### Priority 1 (deferred three times now): docs/21-oem-baselines.md

Empirical OEM behavior reference doc derived from the 178+498 ROM
corpus. Sections per 2026-05-19 plan:

- Cold-start enrichment shape vs ECT
- Knock-learn rate (DAM movement on clean tank vs misfiring)
- Boost target schedule across model years (WRX/STI/Forester XT)
- Closed-loop entry conditions (ECT/load/throttle thresholds across
  generations)
- Catalyst protection thresholds

**Constraint from P2:** Use only ROMs from the `RELIABLE` set when
extracting empirical values. The bludgod corpus is reliable
(plaintext, community-sourced). The `decrypted/` corpus is mostly
NOT reliable for cal data — use only families with a high
anchor-to-partial ratio (DE5M, EA1{T,U,Y}, EZ1G, ZA1J, XH3J).

Estimated 1500-2500 words, several tables. Pure docs work but
requires real RE — loading packs against ROMs and reading the
median/range of each cal table across the corpus. Deserves a fresh
session with full focus.

### Priority 2: Re-run cousin_seed batch with the v2 relocator wired in

The relocator currently only REPORTS candidate addresses in the TSV;
it doesn't patch the pack TOML. To actually multiply the new-packs
yield, add a `--patch-pack` flag (or a separate `relocate_apply.py`)
that rewrites each LOW table's `address` field with the relocator's
suggested new address. Then re-run the bludgod-gap sweep — the 4
candidates from today won't reach 80% even with patching, but the
~10-20% relocation rate adds up across a 50+ candidate batch.

Quick-win sweep ahead of this: re-run the gap analysis with the 6
new packs committed today, find any tens-digit-delta candidates we
skipped because their sibling-pack count was just below 3.

### Priority 3: Pack-format extension for `[[table.role]]`

The §11 panels (knock, adaptive, cold-start, EBCS) surface
advisory suggestions but don't apply them to the pack. Documented in
`docs/05` §11.X as the v1.2 path; gated on adding a `[[table.role]]`
string-tag schema. Once that lands, panels can route via
`Definition::find_table_by_role(role_string) -> ByteEdit -> edit::History`
with the engine-safety linter on the proposed bytes.

### Priority 4: docstring fix in bulk_decrypt_v2.py

Family-granularity claim is loose — should be at least 7 chars, not
6. Honesty pass, not a blocker.

## House-style notes (carry-over)

Unchanged from prior handoffs:
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
- clang-format gate is required. `pip install --user clang-format==18.1.8`. Binary at `C:\Users\Cornelio\AppData\Roaming\Python\Python314\Scripts\clang-format.exe` — invoke by full path.

New this session:
- **The cousin_seed sweet spot is ones/tens-digit deltas within same trim.** Cross-trim (STI↔WRX) and hundreds-digit deltas (model-year revisions) hit real address shifts — drop them rather than commit a high-LOW pack.
- **Post-seed metadata fixes are manual.** `cousin_seed.py` doesn't update years/transmission from filename hints. After generating a pack from a sibling, eyeball the target filename and patch `years` / `transmission` if they differ.
- **`decrypted/` ROMs are mostly partials.** When using a ROM as the target for `localize.py` (or as RE input for any cal work), verify it's a real anchor (path appears in `bulk_decrypt_v2.py` CONFIRMED list AND its md5 doesn't match any entry under `decrypted_v2/partial/`). Promoted partials look fine but their cal bodies are still XOR-encrypted. Memory `project_lf79_partial_decrypts.md` captures the structural finding.

## Suggested opener for next session

> "HEAD `68c2f02`, in sync with `origin/main`. 367 packs in `definitions/`. Working tree clean apart from `SubaruTuner.zip`. 130 defgen tests green.
>
> Recap from 2026-05-20: shipped P1 (6 new packs), P2 (LF79 audit + RECON.md + memory), bonus P2.5 (localize.py v2 with 12 unit tests, --relocate-low pattern-search relocator), bonus P3 (9 fake-anchor PAK_* entries disabled in bulk_decrypt_v2.py — empirical finding that the relocator's 10-20% hit rate on cross-revision packs isn't enough to push the 4 dropped targets over 80%, so they stay deferred).
>
> On the deck for this session:
> **(P1)** `docs/21-oem-baselines.md` — empirical OEM behavior reference doc from the 676-ROM corpus. Deferred three times now. Use only RELIABLE corpus subsets per the LF79 audit findings — bludgod corpus + the high-anchor families in `decrypted/` (EZ1G, EA1{T,U,Y}, DE5M, ZA1J, XH3J).
> **(P2)** Wire the v2 relocator into a pack-patch flow — currently the relocator only reports candidate addresses in the TSV. A `--patch-pack` flag (or a separate `relocate_apply.py`) would rewrite the LOW table's `address` field with the relocated address, letting the cousin_seed yield rise. Then re-run the bludgod-gap sweep with patching enabled.
> **(P3)** Pack-format extension for `[[table.role]]` so §11 panels can apply their suggestions via `edit::History`. Documented in `docs/05` §11.X as the v1.2 path.
>
> Adapter ETA May 22-25. If it lands mid-session, pivot to the first-light `rom-pull --transport obdx` command pre-staged at `c64b717`."

If the user opens with hardware news:

> "OBDX landed? First post-arrival command:
>
> ```
> subuwutuner-cli rom-pull --transport obdx --device COM5 \
>     --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
> ```
>
> That dumps your CURRENT (COBB-tuned) calibration unencrypted, bypassing the COBB AppData encryption. Win32 USB-CDC layer pre-staged at `c64b717`. Battery > 12.0 V before connecting. The dump is read-only — no flash, no write — so safe with engine off."
