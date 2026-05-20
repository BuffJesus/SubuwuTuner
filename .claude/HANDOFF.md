# Handoff — 2026-05-20 (P1 cousin-seed batch + P2 LF79 decryption audit)

Continuation of the 2026-05-19 handoff (§11 four-play arc, OBDX prep, tooling). This session executed the P1 + P2 deck the prior handoff outlined: ran the `cousin_seed.py → localize.py` pipeline against bludgod-corpus CIDs without pack coverage (6 new packs landed, 4 dropped where the cousin distance was too wide), then ran the LF79xxx partial-decryption audit (one anchor confirmed, 30 partials catalogued, broader same-pattern problem identified across most FlashWrite buckets). **HEAD `80a2a0d`**, in sync with `origin/main`. **Working tree clean apart from `SubaruTuner.zip`**.

## What shipped this session (top = newest)

```
80a2a0d defs(packs): cousin-seed 6 new USDM packs from bludgod gap
```

One commit on the public side. P2 produced a private-side artifact (`fixtures/private/roms_extracted/decrypted/LF79/RECON.md`, gitignored) and a memory entry (`project_lf79_partial_decrypts.md`).

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

## Status snapshot

- **HEAD `80a2a0d`**, in sync with `origin/main`
- **definitions/ pack count: 367** (up from 361 at session start)
- **Working tree clean** apart from `SubaruTuner.zip` (untracked 114 MB; gitignore-equivalent — never `git add` it, would break GitHub's 100 MB push limit)
- **CI clang-format gate: required** (no C++ touched this session)

Did not re-run `ctest` this session — no C++ changes, only new TOML data files which would only fail to load at runtime (which is unlikely given they're cousin-seeds of working packs). If asserting test-green is needed before the next code change, run the full suite.

## Open threads / known issues

### LF79120P fake-anchor entry in bulk_decrypt_v2.py

`PAK_LF79120P` at `bulk_decrypt_v2.py:369` points at
`roms_extracted/decrypted/LF79/LF79120P.bin` as its plaintext anchor,
but that file is itself a promoted partial — the recovered keystream
doesn't actually unlock anything new. Action items for a future
session:

1. Either delete the `PAK_LF79120P` CONFIRMED entry (it's not earning
   keystreams that further decode the bucket), or
2. Replace the plaintext source with a real LF79120P stock dump
   (ECUtune purchase, forum source, or OBDX hardware dump once the
   adapter arrives).

Same audit pattern likely applies to other `PAK_*` anchors whose
plaintext path is under `decrypted/`. Worth a quick sweep when
cleaning up: for each `PAK_*` entry, md5-check whether the file at
that path is itself a known partial. Fakes should be removed or
re-sourced.

### Family-granularity claim in bulk_decrypt_v2.py docstring is loose

The docstring states the recovered keystream decrypts "other ciphers in
the SAME 6-char-prefix family". Empirically, LF79100P (anchor) does NOT
fully decrypt LF79101P or LF79120P — both share the 6-char prefix
`LF7910`/`LF7912` and would be expected to fully decode if the docstring
were right. The actual family granularity is tighter (at least 7 chars,
possibly variable). Not a blocker but the docstring should be updated to
reflect empirical truth from the bucket report.

### Map-localization is still verify-only (carried over)

`localize.py` reports HIGH/MED/LOW/ABSENT but doesn't relocate LOW
entries. v2 add — pattern-search by axis fingerprint — would have
saved the 4 dropped cousin-seed targets this session. Particularly
useful for axis tables whose values give a unique byte signature.

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

### Priority 1 (deferred from 2026-05-19): docs/21-oem-baselines.md

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
NOT reliable for cal data — use only the families with a high
anchor-to-partial ratio (DE5M, EA1{T,U,Y}, EZ1G, ZA1J, XH3J, and
similar listed in section 3 above).

Estimated 1500-2500 words, several tables. Pure docs, no tooling.

### Priority 2: Map-localization v2 (pattern-search relocator)

This session dropped 4 cousin-seed targets because the addresses had
shifted and `localize.py` can only verify, not relocate. The v2 add
would:

1. For each LOW entry, take the sibling-ROM byte fingerprint at the
   pack's address (especially axis tables — their values are usually
   strictly monotonic and unique within a small range).
2. Search the target ROM for a matching byte pattern (windowed
   correlation or exact substring search depending on entropy).
3. Output a relocation candidate per LOW entry with a confidence
   score.

Would convert most of the dropped 4 targets into commitable packs
without manual RE.

### Priority 3: Clean up fake-anchor entries in bulk_decrypt_v2.py

Sweep every `PAK_*` CONFIRMED entry; md5-check whether its plaintext
path is itself a known partial. Remove or re-source the fakes.
`LF79120P` is the confirmed one from this session — there are
probably others.

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

> "HEAD `80a2a0d`, in sync with `origin/main`. 367 packs in `definitions/`. Working tree clean apart from `SubaruTuner.zip`.
>
> Three things on the deck for this session:
> **(P1)** `docs/21-oem-baselines.md` — empirical OEM behavior reference doc from the 676-ROM corpus. Deferred twice now (originally P3 on 2026-05-19, then deferred again at end of 2026-05-20 in favor of finishing the P2 audit). Use only RELIABLE corpus subsets per the LF79 audit findings — bludgod corpus + the high-anchor families in `decrypted/` (EZ1G, EA1{T,U,Y}, DE5M, ZA1J, XH3J).
> **(P2)** Map-localization v2 — pattern-search relocator on `localize.py`. Would convert the 4 dropped cousin-seed targets from 2026-05-20 (az1g701v/710v/601r, a2wc400l) into commitable packs.
> **(P3)** Clean up fake-anchor entries in `bulk_decrypt_v2.py`. The LF79120P fake-anchor was confirmed this session — others likely exist.
>
> Adapter ETA May 22-25. After arrival, P1/P2/P3 stay valid as pre-test work; if adapter lands mid-session, pivot to the first-light `rom-pull --transport obdx` command pre-staged at `c64b717`."

If the user opens with hardware news:

> "OBDX landed? First post-arrival command:
>
> ```
> subuwutuner-cli rom-pull --transport obdx --device COM5 \
>     --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
> ```
>
> That dumps your CURRENT (COBB-tuned) calibration unencrypted, bypassing the COBB AppData encryption. Win32 USB-CDC layer pre-staged at `c64b717`. Battery > 12.0 V before connecting. The dump is read-only — no flash, no write — so safe with engine off."
