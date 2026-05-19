# Handoff — 2026-05-18 (long FA-engine session, ROM corpus + decryption + cid_scan)

Continuation of the earlier 2026-05-17/-18 work. This session broke open the FA-engine ROM situation: encryption cracked (user's parallel `fixtures/private/` pipeline + tooling), 974/1272 ECU IDs decrypted for $60, a coverage matrix built against community RR XML + private master, the SubaruTuner code side extended to handle the variable-offset CID convention these ROMs actually use, and a `rom_diff_localize.py` tool shipped for the harder "no RR XML exists" case. **HEAD `ce202da`**, in sync with `origin/main`. **761 unit tests / 100070 assertions green**, **102 defgen Python tests green**.

## What shipped this session (23 commits, top = newest)

```
ce202da fix(defgen): _table_address accepts bare-hex storageaddress
7ae1295 docs(defs): document cid_scan mode in docs/11
34ecba2 feat(cli+defs): surface discovered CID offset in rom-info, scan-mode in pack-info
df8120c feat(defs): cid_scan mode for variable-offset CID identification
aad2917 feat(tools): rom_diff_localize.py — locate likely table addresses by ROM diff
98cadb3 feat(fixtures/private): keystream-recovery pipeline           ← user's work, co-authored
ce2b1fb feat(fixtures/private): forum-scrape + epifan-decrypt pipeline ← user's work, co-authored
e072248 feat(samples+demo): map-selector-int.stmod exercises divide_int end-to-end
3bdf78c fix(ui): features designer subtitle reflects shipped reality
4ec4c33 feat(cli): primitive-list + hook-list browse a pack's signal surface
7cfba19 feat(feature_ir): per-primitive cycle costs in estimate_cost
c7d494d feat(feature_codegen): SH-2A divide_int via FPU bridge
a330bdf docs(arch+reverse-eng): narrow Path B claim to VA/VB only
4feffa8 docs(can+legal): CAN toolkit shipped status + drop fictional first-run wizard
4183c56 docs(overview+defs+autotune): align with shipped reality
b0e0ed9 docs(build): refresh docs/07 to match actual layout, presets, CI, tooling
dae1067 docs: drop fictional deps (Lua, FlatBuffers, nlohmann/json…)
0ad8179 docs(arch+transport): refresh docs/02 module map, fix docs/13 log subtree
e080f98 style(ui): text_subtle sweep — autotune ledger fallback + shortcuts subtitle
9a22bc1 style(ui): text_subtle sweep — table-picker tooltips + jurisdiction popup
655332a fix(cli): propagate pack.toml → directory upgrade to every --def loader
57e1ba5 fix(cli): pack-info upgrades pack.toml file path to directory load
31665bc style(ui): text_subtle sweep through flash-policy modal
```

## The substantive arc

Four threads interleaved:

1. **Phase 5 closure (SH-2A side).** `divide_int` shipped via FPU bridge (FLOAT → FDIV → FTRC) — sidesteps the DIV1-iterative encoding uncertainty the prior handoff deferred on. Closes the last SH-2A primitive gap. Per-primitive cycle costs in `feature_ir::estimate_cost` (`divide_int = 18`, `add_int = 1`, FPU divides dominate) replace the symbol-blind flat-3 model. New `map-selector-int.stmod` sample exercises the whole stack end-to-end against the demo pack with a new `set_active_map` Int-output hook.

2. **Docs audit (6 commits).** Substantive drift cleared across `docs/00, 01, 02, 03, 05, 06, 07, 08, 11, 12, 13, 14`. Module map in docs/02 had 3 fictional modules (`st::script` Lua, `st::nodegraph`, `st::transport.elm/stn`) and missed 8 real ones; docs/07 build & tooling was aspirational throughout (CI matrix, presets, code-signing); docs/03 deps table listed 11 libraries that aren't linked (Lua + Sol2, FlatBuffers, nlohmann/json, …). Now reflects shipped reality. Path-B claim narrowed: public repo strips VA/VB packs only; older Subarus + SSM PIDs + ecuparams still ship.

3. **The FA-engine ROM unlock (user-driven, co-authored).** User reverse-engineered EpifanSoft's protection: stream cipher with per-bucket (size, header-length) keystream. Built `decrypt_epifan.py`, `bulk_decrypt.py`, `ecutune_match.py`, `romraider_forum_system.py` etc. under `fixtures/private/` (force-tracked .py only; ROM/keystream/XML data still gitignored). **974/1272 ECU IDs decrypted (76.6%)** by combining recovered keystreams with forum-attachment plaintexts; one $60 ECUTune purchase (`XH3J2D0I`) unlocked 167 ROMs. `FINAL_BUY_DIRECTION.md` ranks remaining clusters by leverage. **298 still locked**: LHB family (VB WRX 2022+) + AE8M family — no known plaintext source, full stop.

4. **Code-side prep for FA-engine packs.** The user's private VA/VB master packs use `cid_address = 0x0` which the loader interpreted as "match bytes at offset 0" and always reported no-match. Empirically, FA-DIT WRX ROMs put the CID descriptor at a **variable per-firmware offset**: LF75300E at `0x0002F7DD`, LF9C000C at `0x00038035`, same surrounding shape (`\x00\x00 [letter] \x00 <CID> \x00\x00\x00\x00 2.0 [engine]`). Added a `cid_scan = true` boolean to the `[[identification]]` schema — when set, loader scans the entire ROM for `cid_match` instead of comparing at `cid_address`. CLI surfaces it: `rom-info --def` prints `Match: NAME @ 0xADDR (scanned)`; `pack-info` shows `(CID 'X', scan)` instead of misleading `@ 0x00000000`. Plus `tools/rom_diff_localize.py` ships — locates candidate table addresses by diffing stock+tuned ROMs; validated on EJ-era A2TB100K where 7 of 23 clusters labeled real tables (boost_limit_fuel_cut, fine_correction_retard, etc.) with 16 unlabeled candidates pointing at RR-XML omissions. Defgen `_table_address` parser bug fixed (accepts bare-hex storageaddress like `dc938` without `0x` prefix).

Plus user-flagged things now persistent in memory:
- **EpifanSoft `_mod` files = debug patches, not tunes.** 99.5% density rewrites of header + footer regions, no C-segment edits. Don't use `_ori/_mod` pairs as `rom_diff_localize.py` inputs — they fingerprint EpifanSoft's tool, not calibration tunes.
- **Never `rm -rf` user-content directories.** Lost hours of forum-sourced ROM dumps in `fixtures/private/roms_extracted/` to a careless reflex; `unzip -o` already overwrites, the wipe-and-recreate was unnecessary. Memory saved.

## State of play

- **Branch**: `main` at `ce202da`, in sync with `origin/main`. 23 commits this session beyond the prior `b005c17`.
- **Tests**: 761 C++ test cases / 100070 assertions on MinGW g++ 15.2. 102 defgen Python tests via `py -m unittest discover -v tests` (pytest not installed on dev box).
- **Working tree**: clean except the stale `SubaruTuner.zip` carry-over at root. Untouched.
- **Phase status**: Phases 0–4 done hardware-free. Phase 5 SH-2A backend feature-complete (Int + Bool + control flow + Float + Float compares + select_all + cross-hook flow + fan-out dedup + `divide_int`). `divide_int` was the last codegen gap.

## Known caveats and where the surprises hide

1. **The 5 "recovered" VA private CIDs are NOT actually pack-ready.** The user's `_decryption_summary.txt` marked `lf75404s` as OK and the bulk_decrypt cluster dirs contain `lf75404h/s`, `lf9d012h`, `lf9g003t`, `lf9l000e` — but checking the bytes shows entropy ~7.13–7.97 with no `LF[0-9A-Z]{6}` string anywhere in any of the 5 files. The OK threshold the user's pipeline uses is entropy-based, not CID-readable. The keystream that "succeeded" cleans flash-erase 0xFF regions but leaves the calibration descriptor (where the CID lives) still cipher. **End-to-end smoke against private master is BLOCKED until the right keystream anchor is used for these CIDs.** Detail below.

2. **EpifanSoft `_mod` empirically confirmed = debug.** See memory `project_epifan_mod_is_debug.md`. Don't pair `_ori`/`_mod` for table-localization; their diff is 2 dense clusters at file head + foot, zero C-segment edits.

3. **cid_scan landed but private packs don't use it yet.** All 26 VA/VB private packs still say `cid_address = 0x0`. Mechanical one-line-per-file edit pending: add `cid_scan = true` to each `[[identification]]` block. Per CLAUDE.md the private pack files are off-tree user-maintained content; do not Read them directly, but the user can do the edit themselves or a script-based transformation is safe.

4. **0 community RR XML coverage for FA engines.** Of Merp's 334 metric ecu_defs.xml CIDs, exactly 1 (`ae5f301d`) is FA-prefix. Generating a pack via `tools/defgen/` from RR XML is NOT a path forward for any LF/LV/LH/AF/AE CID we care about. The path forward for those CIDs is differential analysis or fresh research.

5. **The 298 still-locked CIDs include the LHB family.** All 18 VB private pack CIDs prefix-match LHB clusters in `FINAL_BUY_DIRECTION.md`'s "🔴 UNAVAILABLE — no known source" section. ECUTune doesn't carry them. Forum coverage doesn't reach them. **The VB end-to-end smoke is hardware-gated until OBDX adapter lands and the user can dump their own VB WRX directly.**

6. **GUI not smoke-tested today.** All UI work this session was `text_subtle` sweeps + one stale-claim fix in the features-designer subtitle. Build-clean only; user has not visually confirmed. Easy to revert per-commit if a sweep regresses readability.

7. **The 95-of-96 OK-decrypted EpifanSoft CIDs without packs** are mostly EJ-era CIDs that don't overlap with the user's VA/VB private master (which is FA-only). Useful as a corpus for `tools/defgen/` end-to-end stress-testing once 291 RR-XML-paired EJ packs get generated.

## How we reverse-engineer the unknown definitions important to us

Two distinct target sets with different paths:

### Target A — VA WRX MT (FA-DIT), 8 private packs

5 of 8 have matching plaintexts in the corpus once decryption is fixed for them. **Highest-leverage move:**

**A.1 — Fix decryption for the 5 actionable VA CIDs (hours of work, no purchase).** The forum-bin plaintexts already on disk include `LF75300E.1.bin` (lives in the AV9D100E_2098176 bucket, same as `lf75404h`, `lf75404s`) and `LF9C000C.1.bin` (LF9F bucket, same as `lf9d012h`, `lf9g003t`, `lf9l000e`). The correct keystream for each bucket = XOR(known plaintext, matching ciphertext). Confirm in the user's `bulk_decrypt.py` pipeline whether those specific plaintexts were already used as anchors. If they were and the result is still partial, the encryption isn't pure stream cipher across the whole file and there's an additional per-file or position-dependent layer to model. If they weren't (script chose a different anchor heuristically), re-run with LF75300E and LF9C000C as the explicit anchors. **Outcome:** clean LF75404h/s + LF9D012H + LF9G003T + LF9L000E plaintexts. Then add `cid_scan = true` to each of the 5 packs. Then `rom-info --def <pack> <plaintext>` should print `Match: <CID> @ 0x<discovered> (scanned)` and we have end-to-end first-light validation.

**A.2 — Validate each of the 5 packs against its plaintext.** `rom-info --def` for identification. `dump-table --def <pack> --table <id> <plaintext>` for representative tables (boost_target, fuel_main, ignition_main) — confirm values look plausible (monotonic axes, typical fuel/timing/boost ranges). `rom-diff --def` between siblings (e.g. lf75404h vs lf75404s) — should reveal small inter-revision differences, not large structural ones. Any discrepancy (out-of-range cells, mis-aligned columns, scaling errors) → bug in the private pack's address/scaling → patch the pack.

**A.3 — `checksum-verify` and `checksum-repair` smoke.** Each pack declares `checksum_type = "subaru_std"` or similar; the IChecksumRepair stubs at `src/flash/src/checksum.cpp` are NotImplemented but cite RomRaider's `ChecksumSTD.java` family. Implement the citations against real stock bytes from the 5 plaintexts. Byte-exact verification per docs/15 clean-room rules — read the RR algorithm, write fresh C++ from the spec, validate byte-for-byte. This is gated by A.1 + A.2 succeeding.

**A.4 — `rom_diff_localize.py` against tuned VA variants.** For each of the 5 actionable VA CIDs, locate other community sources (bludgod, forum threads) that ship tuned versions of the same CID. The tool's output (table-address clusters + density) lets the user verify each cluster is in the private pack's `[[table]]` set OR surface clusters the pack doesn't cover (gaps in the Atlas-derived private master).

**A.5 — For the 3 locked VA CIDs (`lf75600h`, `lf79103p`, `lf9c102p`).** No corpus source on the user's machine. Options: (a) **broad internet scour for stock dumps** — see "Internet-scour playbook" below; (b) RomRaider forum DMs / trade for stock dumps, (c) hardware-direct read from a matching car (gated on OBDX, only works if the user's own car has one of these CIDs flashed), (d) hand-fabricate by extrapolating from the 5 sibling packs (least reliable). Probably parked until A.1–A.4 land — at that point the user's pack-validation experience tells them whether the 5 working packs are healthy enough to inherit useful structure into the 3 locked ones.

### Target B — VB WRX MT (FA-DIT 2022+), 18 private packs

**The user does not own a VB.** Owns a VA. So the standard "dump-your-own-car" route doesn't apply to VB at all. Combined with zero corpus alignment (LHB family is fully UNAVAILABLE per `FINAL_BUY_DIRECTION.md`, ECUTune doesn't sell LHB, forum scrapes find no LHB attachments), VB work is **speculative future-state**, not a current research priority.

The 18 VB private packs stay in the master as scaffolding for whenever ANY of these unlocks:
- User eventually acquires a VB and dumps it.
- Someone in the community shares an LHB plaintext (forum DM, trade for VA work, etc.).
- A new ECUTune (or similar) listing surfaces with LHB content.
- A wider-net internet scour turns up an LHB plaintext stashed somewhere outside the indexed-forum corpus — see "Internet-scour playbook" below.

Until one of those: **leave the 18 VB packs alone**, don't spend cycles trying to validate them, don't buy purportedly-VB content sight-unseen. The static-analysis (Ghidra) angle below is the only deterministic backup path and it's a multi-week labor cost — not worth running speculatively against a platform the user doesn't have to test against.

When (if) any LHB plaintext lands, the same workflow as Target A applies: `cid_scan = true` in the pack, `rom-info --def` to identify, `dump-table` to validate, etc.

Backup paths kept for the record:
- **Static analysis via Ghidra.** SH-2A code has a canonical table-lookup instruction pattern (`MOV.L/W index`, `MULU`, `ADD base`, `MOV.L @(R0,Rn)`). A Ghidra script that finds all such call sites and back-derives table base addresses gives a complete address map without needing any tunes. Doesn't yield scaling/units (still per-byte research) but the address inventory is deterministic and one-time per firmware family. Labor-heavy (days of script work + per-CID verification), worth considering only if hardware+community both stay dry for an extended period.
- **`rom_diff_localize.py` against tuned VB variants** — irrelevant until at least one clean LHB plaintext exists.

#### If sourcing a VB ECU separately to unblock VB pack research

The user may eventually acquire a VB-spec ECU as a standalone bench item (separate from owning a full VB WRX). Notes for what flavor to aim for:

**Highest-value targets** — the 4 LHB CID-prefix matches in the private master where pack + would-be-ROM align at 8-char level:

| Private pack | Likely ECU part-number prefix to source |
|---|---|
| `lhbhb10b00g` | `LHBHB10B` family — JDM, early-mid VB cycle, MT |
| `lhbhd00b00g` | `LHBHD00B` family — JDM, early VB, MT |
| `lhbkc40m00g` | `LHBKC40M` family — JDM, mid-cycle revision |
| `lhbp300d00g` | `LHBP300D` family — JDM, later VB, MT |

The 8-char base CID would identify the ECU; the trailing 3-char suffix (`00g`, etc.) appears to be a market/emissions-cal code. Direct match on the 8-char base = highest probability the existing private pack's table addresses align with the sourced ROM's bytes.

**Second-tier targets** — any other CID in the user's 18-pack private VB collection (`lhbh800b/c00g`, `lhbh900b/d00g`, `lhbhe00b/cx0g`, `lhbkc40p00g`, `lhbkc50my0g`, `lhbp301b00g`, `lhbp400bz0g`, `lhbt120ba0g`, `lhbt210ub/vb0g`). Same logic — pack exists, sourcing the matching ECU produces validation bytes.

**Sourcing principles:**
- **JDM market for matching the existing private master.** All 18 private VB packs have `_0g`-tail suffix patterns matching the JDM/EDM convention. USDM VB WRX ECUs historically use *different base 8-char CIDs* (not just different suffix bytes) — they're a separate calibration thread. A USDM VB ECU will almost certainly not 8-char-prefix-match any of the 18 packs, so it doesn't validate the existing master. Source JDM/EDM only when the goal is private-master validation: Japanese auction proxies (`aucnet.jp`, `Goo-net Exchange`, `JDM Auction Watch`), Far-East Russia parts yards (heavy JDM-VB import scene there, see Russian forum scour list), Australian importers (AusSubaru contacts).
- **USDM as a separate research thread.** If the user eventually owns a USDM VB WRX (more likely in Alberta than importing a JDM): that's a fresh project. The adapter-direct-read path is clean (no decryption needed, ROM reads cannot brick), but the CID will be some `LHB*` USDM-specific code with zero pack coverage. Need to build a USDM VB pack from scratch via `rom_diff_localize.py` against any community USDM VB tunes that surface + Ghidra static table-address extraction. Community FA-era reverse-engineering for USDM VB is even thinner than for JDM (most published work is JDM-focused around the early-2022 launch). The cluster-keystream attack may still pay off if EpifanSoft has USDM LHB ciphers in their catalog and uses the same bucket-keystream scheme — testable once one USDM plaintext is in hand.
- **Transmission: MT only.** Entire private master is VA/VB MT. CVT ECUs from a CVT-spec VB are a completely different part number and tune surface — won't match anything.
- **Year: 2022 launch firmware preferred.** Earliest LHB firmware variants (`LHBH*`) have had the most time to accumulate community attention. `LHBT*` are later mid-cycle revisions; `LHBK*` and `LHBP*` are in the middle. The earlier the ECU, the more likely that the bucket-keystream attack (once one LHB plaintext is in hand) decrypts more sibling CIDs in the EpifanSoft LHB clusters too.
- **Non-runner ECU is fine.** ROM extraction only needs 12V + ground + the OBD-II flash pins exposed on the bench. A wrecked-VB salvage ECU with damaged drivers but intact flash is usable for ROM dumps. Per `docs/13` "open questions: bench-mode protocol" — the exact connect handshake may differ from in-car ECU, design pending until a bench ECU is on the bench.

**Where to look:**
- **eBay** — search "Subaru WRX 2022 ECU MT" filtered to JDM listings; sellers occasionally list the CID/part number in the description.
- **LKQ Pick-Your-Part / similar US salvage yards** — usually USDM only (wrong market for the private master), but check VINs that match JDM imports.
- **Japanese auction proxies** — `aucnet.jp`, `Goo-net Exchange`, `JDM Auction Watch`. Salvage VB WRX ECUs surface here; part numbers usually visible in photos.
- **Far-East Russian parts importers** — heavy JDM-VB second-hand market in Vladivostok area. Same Russian forums in the scour playbook above often have parts-trade sub-sections.
- **AusSubaru / Australian Subaru clubs** — AU also gets JDM-spec imports; classifieds sometimes list bare ECUs.
- **Forum classifieds** on NASIOC / Subaru-Galleri / scoobynet — occasionally bare-ECU listings, mostly USDM though.

**Periodic check-backs that cost nothing but might land an unlock:**
- ECUTune (`ecutune.shop`): currently 0 LHB items (verified 2026-05-18 against the local ecutune index — 264 items, 0 lhb-prefix, 0 lh-prefix). If they ever list any LHB CID, even a tuned one, buying it would: (a) yield the underlying stock as the diff baseline (per the user's "stock+tuned" observation about ECUTune packaging), (b) recover the LHB-bucket keystream, (c) potentially unlock all the LHB ciphers in the EpifanSoft batch in one go. **Set a calendar/cron to re-check the ecutune Subaru index every few weeks.**
- The `romraider_forum_system.py` indexer should also be re-run periodically — new attachments land continuously.
- Telegram / Discord channels (the Russian tuning groups mentioned in the scour playbook below) get LHB material rarely but occasionally.

**Provenance bar before bringing a sourced ROM into the pipeline:**
- Stock dump read from a bench ECU you own: factual data, in bounds.
- Stock dump from a community member shared on a public forum: same as above.
- Stock dump from a commercial-tool extraction (output of a vendor flasher): provenance unclear; check the tool's EULA. If it doesn't forbid further use, fine. If it does, leave it.
- Stock dump from a decompile / extracted-from-flasher-binary route: out of bounds per `docs/15`.

### Target C — public-repo EJ-era packs (Path B carry, non-private)

Already actionable, blocked only on time. **291 triple-play CIDs** identified this session: RR XML coverage exists AND plaintext ROM bytes exist. Defgen-and-validate matrix:
- Run `tools/defgen/defgen.py` against each XML with `--rom-id <CID>` → generate `<cid>.toml`
- Run `rom-info --def <pack> <plaintext>` → confirm CID match + table count
- Generated packs land as `definitions/<model>/<cid>.toml` under the existing EJ-era directory tree
- Each pair is a regression fixture for the loader, address validator, scaling formula parser

Bulk run would be a focused single-session activity (~hour of CPU + manual spot-check) producing a ~300-pack increase in the public corpus. Strictly additive; doesn't help with VA/VB but meaningfully grows SubaruTuner's day-one usefulness for older Subarus.

### Internet-scour playbook (Target A.5 + Target B)

For CIDs that have no corpus presence — the 3 locked VA CIDs (`lf75600h`, `lf79103p`, `lf9c102p`) and the entire LHB / AE8M family — the user already harvested RomRaider's official forum via `romraider_forum_system.py`. The next-rung sources are broader but messier:

1. **Other Subaru tuning forums.**
   - **NASIOC** (`forums.nasioc.com`) — biggest English-language Subaru community; large attachment archive going back ~20 years.
   - **Subaru Forester / Outback / Legacy / Impreza GT forums** — model-specific.
   - **AusSubaru** (`aussubaru.com`) — JDM-relevant CIDs surface here more than on US-centric forums.
   - **Subaru-WRX.com** — UK / EDM focus.
   - **MyG37 / Subaru-Galleri / scoobynet** — regional, occasionally have rare CIDs in attachment threads.
   - Pattern: most have downloadable attachments behind free registration. A script in the shape of `romraider_forum_system.py` (HTML scrape + attachment fetch) per forum.

2. **Foreign-language tuning communities** (often hold ROMs that have been pulled from English-language sources).
   - **Russian sources** — major ECU reverse-engineering tradition; Far-East Russia (`drom.ru`, `farpost.ru`) has heavy JDM-import Subaru presence with corresponding tuning threads. Long-tail archives of stuff that's vanished from western forums under legal pressure.
     - `drive2.ru` — Russian car-enthusiast platform; Subaru sub-communities with tuning threads + attachments.
     - `4x4club.ru`, `subaru-club.ru`, `subaru-impreza.ru`, `clubsubaru.ru` — Russian Subaru forums (model-specific).
     - `chiptuner.ru` / `tunes.ru` / `vintunes.ru` — broader ECU-tuning forums; Subaru sections sometimes have stock-dump exchanges.
     - Telegram channels: search "субару тюнинг", "Subaru ECU", "субару прошивки" — pinned-message archives of community tunes + stock dumps are common.
     - Sites are typically `phpBB` / `vBulletin` variants — same scrape pattern as the English forums but UTF-8 / Cyrillic. Translation: ROMs and CIDs are still uppercase-ASCII so search by literal CID still works even without translating the surrounding thread.
   - **Japanese sources** — JDM CIDs (LF7x JDM market, AZ1G JDM) often have more coverage here than on US forums.
     - `minkara.carview.co.jp` (みんカラ) — huge JP car community; Subaru sub-blogs occasionally include ECU bin attachments.
     - `5ch.net` (formerly 2ch) — tuning sub-boards; attachments rare directly but external link drops to file hosts are common.
     - Japanese-language Subaru workshop/tuner blogs — `wedssport.jp`, individual tuning-shop blogs.
   - **European sources** beyond UK.
     - `motor-talk.de` — large German auto forum; Subaru sub-community.
     - `subaruimpreza.pl` — Polish.
     - `subaruclub.cz` — Czech.
   - Foreign-source caveats:
     - **Provenance still matters.** Same clean-room rules apply: forum-shared community dumps are factual data, in bounds. Output from regional commercial-tool clones (CMD, OpenPort regional repackagings) with EULA constraints is not. When uncertain, leave it.
     - **Registration friction varies.** Some Russian forums require phone-number verification (SMS), some require an inviter, some are wide-open. Lean toward wide-open first.
     - **Site availability is volatile.** Several historic Russian tuning forums have gone offline since 2022; Wayback Machine captures of those URLs are worth checking even when the live site is unreachable.

3. **Open file hosts and indexed CDNs.**
   - Google-dork for `<cid>.bin filetype:bin site:*.com` — sometimes turns up developer-published dumps.
   - GitHub: `<cid>.bin in:path` — occasionally posted in personal tuning research repos.
   - Internet Archive (`archive.org`): some Subaru community archives have been mirrored. The 2018-2022 RomRaider attachment dumps sometimes appear there.
   - SourceForge attachments under tuning-tool projects — rare but happens.

4. **Tuning shop / cobb / ecutek "stage" download portals.**
   - Most lock content behind a vehicle VIN — buying these to enable distribution is not in scope (legal posture), but as a leverage of last resort for the user's own VIN, can produce a stock dump as a byproduct.
   - **Do not** redistribute commercial-tool output back into the public repo per CLAUDE.md.

5. **Direct community DM.**
   - The handles `Bludgod`, `jimihimi`, `Merp` (per `plaintext_corpus/` source subdirs) are already in the user's known-contributor list. Worth asking directly via RomRaider PM if they hold any of the 3 locked VA CIDs or any LHB material.
   - Discord servers (Subaru tuning, NASIOC, Stratified Automotive) sometimes have ad-hoc trade channels.

6. **Wayback Machine** for dead forum threads.
   - RomRaider has multiple defunct sub-forums whose attachments are gone from the live site but cached at `web.archive.org`. The `romraider_forum_system.py` indexer doesn't see those; a one-off pass using the Wayback API against known dead-thread URLs could surface old stock dumps.

**Cost model.** Each scour-attempt is a few hours of script work + bandwidth, no per-ROM cost. Yield is unpredictable per source but the cumulative coverage of running 3-5 forums plus archive.org plus a Russian-forum sweep is meaningful. Prioritize by CID family:
- For VA gaps (`lf75600h`, `lf79103p`, `lf9c102p`): NASIOC + AusSubaru + Russian forums most likely. These are bread-and-butter VA CIDs and someone has posted them somewhere; Russian Far-East sites often have JDM-spec VA dumps.
- For LHB (VB): every source above is a long shot. The platform is too new and the community work is too sparse. Wayback is unlikely to help. Russian forums are the most plausible non-zero source for LHB given the JDM-import scene there; Japanese sources (`minkara`, JDM-shop blogs) are second-most plausible. The realistic LHB unlock if the user eventually acquires a VB remains hardware-direct read.

**Clean-room posture during the scour.** When/if a stock dump is found:
- Verify the source — forum attachment, archived community repo, etc. **Not** a leak from a commercial tuning tool, **not** a decompile output, **not** behind a circumvented access control.
- Stock dumps are factual data, not protected expression — they're the bytes Subaru burned into the ECU. Acquisition through public-channel community sharing is in bounds per `docs/15` + `docs/17`.
- Tuned dumps are also factual but their PROVENANCE matters: a tune by a community member shared on a public forum is fine; output from a commercial tool with a EULA forbidding redistribution is not. Apply common sense; when uncertain, leave it.

**Tracking what's been searched.** Add a column to the user's buy/source matrix recording per-CID scour status: `not-searched | nasioc-checked | aussubaru-checked | found | confirmed-dead-everywhere`. Avoids repeated searches across sessions and makes "give up and move on" a defensible decision.

### The clean-room boundary still applies

Per `docs/15` + CLAUDE.md: when actually authoring or refining a VA/VB pack (e.g. patching `cid_scan = true` into `D:\Documents\SubuwuTuner-defs-private\`), the in-session work touches **fact-only data** (the cid_match string, the cid_scan boolean, address values). No expression from Atlas, RomRaider Java, or commercial-tool source enters the SubuwuTuner repo. The user's existing 26 private packs were already wall-clean derivatives; we'd just be tightening their loader-compatibility metadata, not introducing new content.

The analyst-mode prompt at `docs/analyst-mode-prompt.md` remains the legitimate path if deeper extraction is needed from `D:\Documents\atlas-personal\` — but only in a separate session per the rules, with output isolation to `SubuwuTuner-specs/`.

## Next likely moves (ranked)

1. **Confirm/fix decryption for the 5 VA CIDs.** Hours of work, no purchase, immediate first-light. If the bulk_decrypt pipeline can be re-pointed with LF75300E + LF9C000C as anchors and it produces clean plaintexts, everything in A.1–A.3 cascades. If not — there's a deeper encryption layer to model, and that becomes the bottleneck.

2. **Add `cid_scan = true` to the 26 private VA/VB packs.** Mechanical. Could be done by the user via editor batch-replace or a script that opens each TOML and inserts the line under `[[identification]]`. Once landed, the loader stops reporting false no-matches even before decryption catches up.

3. **Bulk-generate the 291 EJ-era packs.** Single-session productive task: `defgen` per CID, validate, land under `definitions/<model>/`. Public repo growth + regression corpus.

4. **Implement `IChecksumRepair` for `subaru_std`/`alt`/`alt2`.** Gated on (1). Once any single FA-engine stock plaintext is byte-validated, the RomRaider-cited algorithms can be implemented and verified against it. Closes the last `NotImplemented` in `src/flash/`.

5. **OBDX adapter integration when it arrives.** Unblocks VA direct-read (the user's own car) → fresh stock-dump validation against the private VA pack collection + real Phase 4 flash testing. **Not** a VB unlock — the user doesn't own a VB. Wire `obdx::Transport::open` against libusb + DVI handshake — ~1 week. ROM reads are read-only and cannot brick, safe for the daily driver.

6. **Ghidra-based static table-address extraction.** Only if (1) hits a wall (deeper encryption layer than pure stream cipher) AND adapter is delayed AND community appeals don't produce LHB plaintexts. Labor cost is real but it's the last deterministic path.

## Active context, in brief

- v1.0 target: WRX VA/VB MT. Repo at `https://github.com/BuffJesus/SubuwuTuner` (public state: `ce202da`).
- House style: C++23, `st::Result<T>`, no exceptions in domain. clang-format/clang-tidy clean. MinGW gcc 15.2 on Windows; Python 3.12-3.14 for defgen.
- **Caveman cadence.** Small focused commits, conventional-commits subject, push direct per commit. Documented and durable.
- User in Alberta, Canada — no emissions paternalism. Engine-safety refusals still apply.
- **IP boundary: clean-room.** No Atlas source, no RomRaider Java verbatim. SH-2A encodings from public Renesas refs. The wall lives at `docs/15`; analyst sessions at `docs/analyst-mode-prompt.md`.
- **Path B distribution live.** Public repo ships infrastructure + EJ-era community packs + SSM PIDs + ecuparams. Does NOT ship VA/VB packs (`docs/17`).
- User's private master at `D:\Documents\SubuwuTuner-defs-private\` — 8 VA + 18 VB packs. Off-tree. Don't `Read` directly per CLAUDE.md; run CLI tooling against them is fine.
- User's atlas-personal at `D:\Documents\atlas-personal\` — OFF-LIMITS in main sessions per CLAUDE.md. `*.facts.xml` + `*.name-mapping.tsv` analyst-mode-only.
- Decryption corpus at `fixtures/private/` — Python tooling is git-tracked (force-added); ROM/keystream/XML data is gitignored. 974/1272 decrypted; full corpus survey + buy direction at `roms_extracted/FINAL_BUY_DIRECTION.md`.

## Working with this user

- Terse. No trailing summaries. They read the diff.
- "Proceed" / "Continue" / "Keep going" / "Proceed as you see fit" = continue current narrow thing OR pick a next slice yourself.
- "Next slice" = the next caveman commit.
- Push per-commit. Caveman-style messages. No bulk caveman-review unless flagged.
- Modal failure feedback goes **inline** in the modal, not the status bar.
- UI/UX philosophy: intuitive + non-intimidating + modern + beautiful + functional, all equally weighted.
- Accent purple `(0.55, 0.35, 0.85)` via `accent_for(Theme)`.
- Path style: `/` in bash; `\` in Windows-path strings.
- Force-push to main is NOT standing approval. Ask each time.
- GUI not smoke-testable by Claude (no display). State explicitly if a sweep ships unverified.
- **NEVER `rm -rf` directories that may hold user files.** Memory saved as `feedback_no_rm_rf_user_dirs.md`. `unzip -o` already overwrites; the wipe step destroyed forum ROM dumps on 2026-05-18.
- Edit then `git mv` loses the edit — stage the Edit before any rename.
- Action buttons must complete the action (Compile/Export/Build/Save produce the full artifact; no preview-then-commit splits).
- Don't use `git add -A` blindly — the carry-over `SubaruTuner.zip` at the repo root will sweep in and break the push (114MB > GitHub's 100MB limit). Use explicit paths.

## Suggested opener for next session

> "HEAD `ce202da`, in sync with `origin/main`. 761 C++ tests / 102 defgen Python tests green. Major unlock arc this session: encryption broken (974/1272 ECU IDs decrypted), `cid_scan` schema + loader + CLI shipped for variable-offset FA-DIT CIDs, `tools/rom_diff_localize.py` ships, defgen parser fixed for bare-hex addresses. **End-to-end first-light validation against private VA master is BLOCKED on a decryption refinement** — the 5 corpus-aligned CIDs (`lf75404h/s`, `lf9d012h`, `lf9g003t`, `lf9l000e`) decrypt to entropy ~7.2 but the CID descriptor region is still cipher; the OK threshold in `bulk_decrypt.py` is entropy-based, not CID-readable. Three reasonable first bundles: (a) re-run bulk_decrypt with `LF75300E.1.bin` + `LF9C000C.1.bin` as explicit keystream anchors — confirms whether the encryption is pure stream cipher or has a deeper per-file layer; (b) bulk-generate the 291 EJ-era packs from RR XML + plaintext pairs already on disk; (c) add `cid_scan = true` to the 26 private VA/VB packs as a mechanical batch edit. Pick?"

If the user opens with hardware news:

> "OBDX adapter landed? Minimum path to first VA ROM: implement libusb + DVI handshake inside `obdx::Transport::open` (already a stub in `src/transport/obdx/`). ROM dumps are read-only — first contact with your own VA WRX is safe given healthy battery. The user owns a VA, not a VB, so this unblocks: (a) fresh stock-dump bytes of the user's own car, (b) real-CID identification of their flashed firmware, (c) end-to-end validation against whichever of the 8 private VA packs matches, (d) eventually Phase 4 flash testing on a car they have skin in the game with. ~1 week of focused work. Want to start on the libusb wiring?"

If the user opens with scour news (new ROMs found via internet hunt):

> "Found stock dumps for [CIDs]? Drop them in `fixtures/private/plaintext_corpus/forum-bins/` (or a new subdir). The cid_scan loader path can validate them immediately — `subuwutuner-cli rom-info --def <pack> <plaintext>` should print `Match: <CID> @ 0xADDR (scanned)` if the pack and ROM match. Once any LHB plaintext lands, the VB private packs become validatable too. What did you find?"

If the user opens with decryption news:

> "Decryption refined? The 5 actionable VA CIDs (`lf75404h/s`, `lf9d012h`, `lf9g003t`, `lf9l000e`) now have readable CIDs at their descriptor offsets? Adding `cid_scan = true` to the matching private packs and running `rom-info --def` is the immediate next step — should print `Match: <CID> @ 0xADDR (scanned)` for each. Then `dump-table` on a representative table, `rom-diff` between siblings, and finally implementing `IChecksumRepair` against one of them. The whole Phase 1 ship gate (≥20 maps from a real definition pack on a real ROM, per docs/04) becomes reachable from there."
