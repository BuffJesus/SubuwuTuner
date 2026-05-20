# Handoff — 2026-05-20 (11 packs + localize.py v2 + axis correctness arc)

Continuation of the 2026-05-19 handoff. Marathon session: cousin_seed + localize landed 11 new packs across four batches; LF79xxx partial-decryption audit (1 anchor confirmed, 30 partials, broader pattern across most FlashWrite buckets); `localize.py` v2 with the pattern-search relocator AND `--patch-pack` AND axis-tracking (22 unit tests total); fake-anchor sweep across all 99 anchor entries in `bulk_decrypt_v2.py` (9 disabled); Windows stdout + bulk_decrypt docstring fixes; real user-facing CLI bug fix for pack-discovery (was missing every flat-file pack); axis-blindness bug discovered via dump-table validation and fixed across 6 of the 11 packs that needed re-patching. **HEAD `6a0130c`**, in sync with `origin/main`. **Working tree clean apart from `SubaruTuner.zip`**. **definitions/ pack count: 372** (up from 361 at session start = **+11 packs in one session**, all axis-validated).

## What shipped this session (top = newest)

```
8ea6d71 fix(defgen): default rom_size_bytes to 1MB when XML lacks <filesize>
0008695 fix(defs): restore rom_size_bytes=1048576 on ez1gc00c + ep5g600a
6a0130c docs(defgen): note axis tracking + end-to-end dump-table validation
1df935b defs(packs): axis re-patch sweep on 4 more packs from 80a2a0d
5598683 defs(packs): re-patch ez1g109j + ez1e401h with axis awareness
3eb6202 tools(defgen): localize.py tracks [[axis]] blocks + axis-aware classifier
8bf4d26 docs(defgen): document localize.py + cousin-seed lessons learned
366a3b5 docs(handoff): final-final 2026-05-20 refresh (11 packs + CLI bug fix)
261f5f8 defs(packs): cousin-seed ez1g108k via newly-landed ez1g109k sibling
21f89c2 fix(cli): pack-list + rom-identify discover single-file packs
05c439a docs(handoff): final 2026-05-20 refresh (10 new packs + relocator + patch-pack + sweep)
b3a5e14 defs(packs): cousin-seed 2 more 2006-era e2vg packs via USDM siblings
897e53b defs(packs): cousin-seed 2 more USDM packs via --patch-pack
de647ba tools(defgen): localize.py --patch-pack + Win stdout fix + bulk_decrypt docstring
5098cde docs(handoff): refresh 2026-05-20 handoff after relocator + anchor sweep
68c2f02 fix(fixtures/private): comment out 9 fake-anchor PAK_* entries
70773de tools(defgen): localize.py --relocate-low pattern-search relocator
852198b docs(handoff): land 2026-05-20 session (P1 cousin-seed batch + P2 LF79 audit)
80a2a0d defs(packs): cousin-seed 6 new USDM packs from bludgod gap
```

Fourteen content commits (plus three handoff snapshots). P2 produced a private-side artifact (`fixtures/private/roms_extracted/decrypted/LF79/RECON.md`, gitignored) and two memory entries (`project_lf79_partial_decrypts.md`, `feedback_cousin_seed_axis_validation.md`).

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

### 6. localize.py --patch-pack flow (`de647ba`)

The relocator from `70773de` only REPORTED candidate addresses in the TSV; users still had to hand-edit the pack TOML. New `--patch-pack` flag writes successful relocations back into the pack's `[[table]]` blocks in place. Line-based rewriter preserves comments, blank lines, indentation, and inline `# ...` comments on the address line. Only `[[table]]` sections are touched — `cid_address` in `[[identification]]` is left alone. Idempotent.

After patching, the pack should be evaluated against the TARGET ROM, not the original sibling — re-running localize with the same sibling+target pair is misleading (the sibling no longer has the table at the patched address, so `classify_pair` compares unrelated bytes). The relocator's per-row HIGH/MED in the TSV is the authoritative signal that each patch landed at a sensible location.

7 more unit tests (single-table patch, multi-table, inline-comment preservation, empty-dict no-op, unknown-id no-op, idempotency, non-table sections untouched). Full defgen suite: **137 tests green**.

Also bundled:
- **Windows stdout UnicodeEncodeError** — `classify_pair` reasons include Δ; cp1252 stdout couldn't encode it when no `--out-report` was set. `sys.stdout.reconfigure(encoding="utf-8", errors="replace")` at startup.
- **`bulk_decrypt_v2.py` docstring family-granularity claim** revised from "6-char-prefix family" to "at most 7-8 characters; rely on the bucket report" — per the LF79 audit, LF79100P and LF79101P share the 6-char prefix `LF7910` but only LF79100P fully decrypts under the LF79100_family anchor.

### 7. Two-batch follow-on cousin-seed sweep (`897e53b`, `b3a5e14`)

After `--patch-pack` landed, re-ran the bludgod-gap analysis to find tens/ones-delta candidates skipped in the original sweep. **+4 more packs landed**:

| pack | sibling | pre-patch | post-patch | notes |
|---|---|---|---|---|
| ez1g109j | ez1g109k | 88.7% | 92.9% | forester USDM 2009 MT→MT |
| ez1e401h | ez1e401g | 90.2% | 94.3% | impreza USDM 2008-09 MT→AT |
| e2vg212d | e2vg211d | 100% | n/a | forester USDM 2006 (no LOW to patch) |
| e2vg204b | e2vg222b | 100% | n/a | legacy USDM 2006 (cross-platform; outback sibling) |

The two e2vg packs (e2vg212d, e2vg204b) had been mis-paired against ADM siblings in the original 2026-05-20 P1 sweep and came out at 46% / 58% HIGH+MED then. Re-pairing against USDM siblings landed both essentially perfect (96-98% HIGH, no LOW).

Two more candidates from the follow-on sweep dropped (cross-region pairings: e2vg212d, ez1g108k → ADM siblings, 46-58% HIGH+MED). Same lesson keeps reinforcing: cousin_seed wants same-trim same-region siblings with small CID deltas.

### 8. CLI pack-discovery bug fix (`21f89c2`)

While validating today's new packs end-to-end via `subuwutuner-cli rom-identify`, discovered the command silently returns 0 matches against EVERY pack in `definitions/`. The walk filtered strictly on `filename() == "pack.toml"`, which is the multi-file layout (docs/11). But every shipped pack — all 371 of them — is a flat `<id>.toml` single-file pack. So pack discovery never found anything.

Fix: extracted a shared `discover_pack_paths()` helper that handles both layouts (single-file `<id>.toml` and multi-file `<dir>/pack.toml` + fragments). Companion fragments are filtered out by walking ancestors up to the top-level scan dir for a sibling `pack.toml`. Files that lack a `[pack]` section fail to load naturally and are reported as skips.

End-to-end empirically: `pack-list definitions/legacy` now finds all 72 legacy packs (was 0); `rom-identify` against the A2TB002C target ROM correctly matches `definitions/legacy/a2tb002c.toml` (was 0 matches).

clang-format clean; no test regressions (some pre-existing `obdx::Transport` / `dvi::checksum` failures on main are unrelated to this change).

### 9. Third-pass bludgod-gap sweep — +1 more pack (`261f5f8`)

After the CLI fix landed, re-ran the gap analysis with the now-larger sibling pool. Found one new candidate that opened up because of today's commits:

| pack | sibling | HIGH | MED | LOW | verdict |
|---|---|---|---|---|---|
| ez1g108k | ez1g109k | 98.1% | 1.9% | 0% | ✅ same-region tens-delta |

ez1g109k was NOT a viable sibling 24 hours ago. The sibling pool that today's 11 new packs grew opens candidates each pass; this one happens to be a same-region same-platform tens-delta with zero LOW — relocator not even needed.

End-to-end validated via the just-fixed rom-identify against the bludgod target ROM.

### 10. Axis correctness arc (`3eb6202`, `5598683`, `1df935b`, `6a0130c`)

While validating `ez1g109j` end-to-end via `subuwutuner-cli dump-table`, discovered the relocated DATA tables had correct cell values but the AXIS labels were nonsense (Y axis showed 0.0-0.9 fractional values instead of 2800-6300 RPM). Root cause: `[[axis]]` blocks are separate top-level entries from `[[table]]` blocks; the v1 `parse_pack` only walked `[[table]]`, so axes were completely invisible to localize/relocate/patch.

Three layers of fix in `3eb6202`:

1. **`parse_pack` walks `[[axis]]` blocks** alongside `[[table]]`. Each entry carries `_kind` of `"table"` or `"axis"`. `patch_pack_addresses` rewrites both kinds. TSV gains a `kind` column.
2. **Sample count capped at axis `length`**. Reading past axis-end into neighboring tables/padding corrupted monotonicity (10-value axis + 6 garbage values = "not monotonic"), pushing genuinely-moved axes into MED instead of LOW.
3. **"Both monotonic" needs cross-checks** before HIGH:
   - Range ratio ≤ 10x (catches lambda-vs-RPM mismatch).
   - First-value ratio ≤ 10x (catches the case where ranges coincide because target tail spans into a different axis, e.g. tgt `[0.5..1.3, 2800]` passes a span check by accident but first value 0.5 vs sib 2800 is decisive).

3 new unit tests in `5598683`. Full defgen suite: **22 tests green** (was 19).

Audited all 11 of today's committed packs against the new flow:
- **Clean (axes all HIGH)**: a2tb002c, az1g702i, e2vg212d, e2vg204b, ez1g108k — 5 packs unchanged.
- **Re-patched** (`5598683` + `1df935b`): ez1g109j, ez1e401h, a2wc500s, a2wc501k, ez1d301a, ez1d303b — 6 packs, all axes now HIGH or relocated.

Pattern: same-trim same-region pairings (e.g. a2tb001c → a2tb002c, both USDM Legacy AT) have stable axis layouts. AT↔MT crossings (a2wc500r/AT → a2wc500s/MT) and wide-trim siblings shake axes. The dump-table validation step is now documented in `tools/defgen/README.md` (`6a0130c`) + memory (`feedback_cousin_seed_axis_validation.md`).

End-to-end: `dump-table primary_open_loop_fueling_a` on every re-patched pack now shows X axis 0.30-1.30 (lambda) + Y axis 2800-6300 (RPM) byte-identical to the sibling reference.

### 11. rom_size_bytes regression sweep (`0008695`, `8ea6d71`)

Full-corpus validation via `subuwutuner-cli pack-list definitions` surfaced an outlier: `ez1gc00c` had only 19 tables (10× smaller than the next pack). Investigation revealed `rom_size_bytes = 0` — the pack was originally created with a manual `rom_size_bytes = 1048576` patch (per commit `085450a`: "rom_size_bytes patched to 1048576 (XML had no <filesize>)"), but the bulk-regen at `2202fc2` silently clobbered it back to 0 by re-running `defgen.parse_rom_xml` on the same XML.

Sweep found a second affected pack: `ep5g600a` (same pattern, originally fixed at `7fa4beb`). Both restored to 1048576 in `0008695`.

Defensive follow-up `8ea6d71`: changed `defgen._parse_filesize` to return 1048576 (Subaru-default) instead of 0 when the XML lacks `<filesize>` or has unparseable size. A pack with rom_size_bytes=0 still loads but breaks every size-aware code path silently; defaulting to 1MB surfaces a clean size-mismatch error against an actual ROM when wrong, which is strictly better. 1 new unit test (141 total).

## Status snapshot

- **HEAD `8ea6d71`**, in sync with `origin/main`
- **definitions/ pack count: 372** (up from 361 at session start — **+11 packs**, all axis-validated; +2 more rom_size_bytes-fixed)
- **Working tree clean** apart from `SubaruTuner.zip` (untracked 114 MB; gitignore-equivalent — never `git add` it, would break GitHub's 100 MB push limit)
- **CI clang-format gate: required** — applied to the CLI edit (clang-format 18.1.8 binary at `C:\Users\Cornelio\AppData\Roaming\Python\Python314\Scripts\clang-format.exe`)
- **defgen test suite: 141 tests green** (12 relocator + 7 patch-pack + 3 axis-tracking + 1 default-filesize added this session)
- **C++ build: passes** (cli binary built clean; some pre-existing `obdx::Transport` + `dvi::checksum` + checksum-kind test failures exist on main unrelated to this session — verified by stashing my changes and running the same tests on plain `main`)
- **All 706 .toml files under definitions/ load cleanly** (705 calibration packs + 333 ecuparams + pids.toml — verified via pack-list)

## Open threads / known issues

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

### Priority 1 (deferred four times now): docs/21-oem-baselines.md

Empirical OEM behavior reference doc derived from the 178+498 ROM
corpus. Sections per 2026-05-19 plan:

- Cold-start enrichment shape vs ECT
- Knock-learn rate (DAM movement on clean tank vs misfiring)
- Boost target schedule across model years (WRX/STI/Forester XT)
- Closed-loop entry conditions (ECT/load/throttle thresholds across
  generations)
- Catalyst protection thresholds

**Constraint from P2:** Use only ROMs from the RELIABLE set when
extracting empirical values. The bludgod corpus is reliable
(plaintext, community-sourced). The `decrypted/` corpus is mostly
NOT reliable for cal data — use only families with a high
anchor-to-partial ratio (DE5M, EA1{T,U,Y}, EZ1G, ZA1J, XH3J).

Estimated 1500-2500 words, several tables. Pure docs work but
requires real RE — loading packs against ROMs and reading the
median/range of each cal table across the corpus. Deserves a fresh
session with full focus.

### Priority 2: Third-pass bludgod-gap sweep

Two passes this session landed 10 packs (the original 6 + 4 from the
follow-on after `--patch-pack` landed). The gap should be smaller
now but not empty. Worth one more pass looking for:

1. Cousin candidates that became available after today's 10 new
   packs entered the sibling pool (e.g., a CID that's a ones-digit
   delta of a pack we landed today).
2. Cross-platform USDM-USDM pairings that I might have missed.
3. Same-family CIDs where bludgod has both target AND a sibling
   ROM, AND we have a pack for the sibling (the third condition is
   the one that keeps narrowing).

Probably 2-4 more packs available. Quick session.

### Priority 3: Pack-format extension for `[[table.role]]`

The §11 panels (knock, adaptive, cold-start, EBCS) surface
advisory suggestions but don't apply them to the pack. Documented in
`docs/05` §11.X as the v1.2 path; gated on adding a `[[table.role]]`
string-tag schema. Once that lands, panels can route via
`Definition::find_table_by_role(role_string) -> ByteEdit -> edit::History`
with the engine-safety linter on the proposed bytes. This is
substantial C++ work — multi-file, schema extension, Definition
loader change, edit::History routing, lint wiring, UI integration.

### Priority 4: Pattern-search relocator v3 — axis-fingerprint relocation

The current `--relocate-low` catches address shifts where the bytes
stayed identical. Cross-revision packs often recalibrate the
calibration cells but keep AXIS tables (RPM bins, MAP bins, ECT
bins) byte-identical. An axis-fingerprint matcher (look for
monotonic windowed runs matching the sibling axis values) could
relocate those even when the cal cells diverge — then the user
manually fixes the diverged cal tables.

Would push the recalibration-case packs (az1g701v, az1g710v,
az1g601r, a2wc400l from today) closer to commitable.

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
- **Always dump-table-validate cousin-seeded packs before committing.** `localize.py`'s HIGH/MED summary isn't sufficient — axes can be wrong even when data tables are correct. Run `subuwutuner-cli dump-table` on a representative 2D table (one with named axis_x + axis_y, e.g. `primary_open_loop_fueling_a`) and check that the AXIS labels look sensible (RPM in thousands, lambda 0-2, ECT in expected band, etc.). Memory `feedback_cousin_seed_axis_validation.md` captures this. Pre-fix on 2026-05-20, 6 of 11 packs had visually-broken axes despite localize reporting "HIGH+MED ≥ 80%".

## Suggested opener for next session

> "HEAD `6a0130c`, in sync with `origin/main`. 372 packs in `definitions/` (up +11 from the 2026-05-20 session start, all axis-validated). Working tree clean apart from `SubaruTuner.zip`. 140 defgen tests green.
>
> Recap from the 2026-05-20 marathon: shipped 11 new packs across four batches, P2 (LF79 audit + RECON.md + memory), localize.py v2 (--relocate-low pattern-search relocator + --patch-pack pack-rewriter + axis tracking, 22 unit tests), 9 fake-anchor PAK_* entries disabled in bulk_decrypt_v2.py, Windows stdout fix, bulk_decrypt docstring fix, one user-facing C++ bug fix (pack-list + rom-identify couldn't see flat-file packs), and one axis-correctness arc where dump-table validation surfaced a bug that bit 6 of the 11 packs — now fixed in tooling AND in the affected packs.
>
> On the deck for this session:
> **(P1)** `docs/21-oem-baselines.md` — empirical OEM behavior reference doc from the 676-ROM corpus. Deferred four times now. Use only RELIABLE corpus subsets per the LF79 audit findings — bludgod corpus + the high-anchor families in `decrypted/` (EZ1G, EA1{T,U,Y}, DE5M, ZA1J, XH3J).
> **(P2)** Third-pass bludgod-gap sweep. The 11 new packs from yesterday add to the sibling pool, opening more ones-digit-delta candidates. Quick session if any are still findable.
> **(P3)** Pack-format extension for `[[table.role]]` so §11 panels can apply their suggestions via `edit::History`. Documented in `docs/05` §11.X as the v1.2 path. Substantial C++ work — schema extension, Definition loader, edit::History routing, lint wiring, UI integration.
> **(P4)** Pattern-search relocator v3 — axis-fingerprint matching. Would push the recalibration-case packs (az1g701v et al.) closer to commitable. Separate body of work from the byte-identical relocator.
>
> Adapter ETA May 22-25 (anytime now). If it lands mid-session, pivot to the first-light `rom-pull --transport obdx` command pre-staged at `c64b717`."

If the user opens with hardware news:

> "OBDX landed? First post-arrival command:
>
> ```
> subuwutuner-cli rom-pull --transport obdx --device COM5 \
>     --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
> ```
>
> That dumps your CURRENT (COBB-tuned) calibration unencrypted, bypassing the COBB AppData encryption. Win32 USB-CDC layer pre-staged at `c64b717`. Battery > 12.0 V before connecting. The dump is read-only — no flash, no write — so safe with engine off."
