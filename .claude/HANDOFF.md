# Handoff — 2026-05-19 (§11 four-play arc + OBDX prep + tooling buildout)

Continuation of the 2026-05-18 FA-engine ROM unlock. This session delivered the entire `docs/05` §11 "under-served-coverage" thesis end-to-end (all four plays — knock dashboard, adaptive history, cold-start workflow, EBCS PID assistant — header → impl → tests → CLI → GUI → demo CSV), shipped the live-tuning + AI-integration design docs, pre-staged the Win32 USB-CDC layer for OBDX arrival (~May 22-25), flipped the clang-format CI gate from advisory to required (full codebase swept), and built the def-pack-acceleration toolchain (`tools/defgen/localize.py` + `tools/checksum_discover.py`). **HEAD `f35b05e`**, in sync with `origin/main`. **807 unit tests / 101k+ assertions green**.

## What shipped this session (top = newest)

```
f35b05e tools(defgen): map-localization helper — verify sibling pack addresses
8cd060a tools(flash): checksum byte-location discovery driver
52757da style: apply clang-format 18 to all src/ and tests/ + flip CI to required
79a4890 test(flash): mutation-test driver + docs/08 update
472e153 docs(05): document §11 suggestion → edit::History path (v1.2)
c64b717 feat(transport): Win32 USB-CDC IByteChannel for OBDX adapter
1461f3e feat(cli+ui): cold-start CLI + GUI panel + demo fixture
0af78f5 feat(log+cli+ui): EBCS PID assistant — closes §11 play 4
f40844d docs(ai-integration): draft design doc for AI as advisory surface (v2.0+)
7e61462 docs: connect custom features + live tuning + handheld for COBB-style toggles
045fa75 feat(log): implement st::log::coldstart::snapshot_from_samples + CSV
ece7509 feat(log): scaffold st::log::coldstart tuning workflow
7193c94 docs(live-tuning): draft design doc for RAM-shadow live tuning (v1.5+)
d2692d9 feat(cli): adaptive-history subcommand + demo fixture
5529978 feat(ui): wire adaptive-history GUI panel
05951f2 fix(ui): restore sidebar table tooltip after policy-badge regression
1f31e78 feat(log): implement st::log::adaptive::snapshot_from_samples + CSV
96ee9d7 feat(log): scaffold st::log::adaptive history visualizer
6b60afc feat(ui): wire per-cylinder knock dashboard panel into the GUI
f161bfd feat(cli+log): wire knock-snapshot CLI + CSV reader
ab24bb1 feat(log): implement st::log::knock::snapshot_from_samples + tests
7a555c3 docs(fixtures): ship demo-knock-log.csv for the knock dashboard
2eb12c0 docs(improvements): add §11 under-served definition coverage
5edd1a7 feat(defs): populate ecu_part for LF75600H + LF9L000E from pak headers
dd565f9 docs(fixtures/private): lock in v370-is-final logger XML finding
b9be7b6 feat(log+defgen): scaffold knock dashboard + FA-DIT logger supplement
a4d59df docs(legal): record FA-DIT WRX shipping reality after 2026-05-19 unlock
eddce2b refactor(core): factor shared csv helpers; drop 3-way duplication
```

## The substantive arc

Five interleaved threads.

### 1. The §11 four-play landing

Identified the "under-served-coverage" thesis (tables every Subaru pack exposes but no community workflow tunes) and delivered all four plays end-to-end:

| Play | Header | Impl | Tests | CLI | GUI | Demo CSV |
|---|---|---|---|---|---|---|
| Per-cylinder knock dashboard | ✅ | ✅ | 10 cases | ✅ | ✅ | ✅ |
| Adaptive-learning history | ✅ | ✅ | 12 cases | ✅ | ✅ | ✅ |
| Cold-start workflow | ✅ | ✅ | 15 cases | ✅ | ✅ | ✅ |
| EBCS PID assistant | ✅ | ✅ | 9 cases | ✅ | ✅ | ✅ |

All four follow the same shape: `snapshot_from_samples()` pure-domain aggregator + `snapshot_from_csv()` replay path + `subuwutuner-cli <play>-snapshot` text-mode dashboard + `View → <play>` GUI panel + `fixtures/demo-<play>-log.csv` smoke fixture. Each panel surfaces metrics + advisory suggestions; the path through `st::edit::History` is **deferred to v1.2** (design captured in `docs/05` §11.X, gated on a `[[table.role]]` pack-format extension we haven't designed yet).

### 2. Forward-looking design docs (v1.5+ and v2.0+)

Two new docs/ entries capturing the longer-arc design now so future sessions don't re-derive:

- **`docs/19-live-tuning.md`** (v1.5) — RAM-shadow live tuning. Atlas-equivalent on-dyno cell editing via UDS WriteDataByIdentifier. Gated on Phase 4 hardware validation. Includes the "feature toggles via live writes" extension and ties to `docs/16` (custom features) + `docs/18` (standalone handheld) for the COBB-AccessPort-style "toggle launch control from the hardware screen" UX. Engine-safety linter runs on every write; advisory output only; no path into auto-flash.
- **`docs/20-ai-integration.md`** (v2.0+) — AI as advisory surface (interpretation side), never modification side. 8-tier spectrum from rules-based drift classifier (Tier 1) to LLM explanation (Tier 2) through trained models (Tier 7+) and cipher classification (Tier 8). Local-first via Ollama. Strict clean-room rules on training data (commercial-tool decompiles off-limits; cloud-backend output tagged + never auto-committed). Canonical example: LTFT/STFT drift classifier with rules-based diagnosis ("vacuum leak vs injector aging vs MAF aging vs O2 sensor failing") + LLM-prose explanation layer.

### 3. OBDX adapter pre-staging (time-sensitive — May 22-25 arrival)

User's OBDX Pro VX adapter is shipping; arrival expected May 22-25 (memory: `project_obdx_eta.md`). USB-CDC IByteChannel for Windows landed at `c64b717`:

- `src/transport/include/st/transport/serial_byte_channel.hpp` — cross-platform shape (`make_serial_byte_channel(SerialChannelConfig)`)
- `src/transport/src/serial_byte_channel_win.cpp` — Win32 implementation. CreateFile + DCB (8N1, no flow control, DTR/RTS enabled) + COMMTIMEOUTS (`ReadIntervalTimeout = MAXDWORD` pattern for return-when-bytes-available semantics) + RAII HandleOwner. Auto-canonicalizes `\\.\` prefix for COM10+.
- `src/transport/src/factory.cpp` — Obdx + Native paths now wire through `make_serial_byte_channel`. OBDX defaults to 500000 baud; native to 921600.

When the adapter arrives, the first-contact path is:

```bash
subuwutuner-cli rom-pull --transport obdx --device COM5 \
    --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
```

That dumps the user's CURRENT (COBB-tuned) cal unencrypted, bypassing the COBB-encrypted backup in their AppData. The OBDX codec + transport + SSM/UDS clients are all tested against MockTransport already — only the byte-channel wiring was missing.

### 4. Tooling buildout

Three new tools, all hardware-independent and immediately useful:

- **`tools/mutation_test.py`** (`79a4890`) — Python mutation-testing driver. Applies a focused set of operator swaps (`==↔!=`, `<↔<=`, `>↔>=`, `true↔false`) to a specified line range, rebuilds + retests, reports KILLED/SURVIVED/BUILD_FAIL/TIMEOUT per mutant + overall score. Smoke-tested on `evaluate_plan_policy` (lines 515-544): 1/1 compiling mutant KILLED. Documented in `docs/08` "Mutation testing" with a light/heavy tier split (heavy = future LLVM mull integration).
- **`tools/checksum_discover.py`** (`8cd060a`) — empirical discovery of Subaru ROM checksum byte locations. Diffs every pair of sibling ROMs per family; bytes that differ in EVERY pair are candidates for either the checksum slot or always-changing calibration cells. Run-length analysis + Subaru-convention zone distance scoring produces a ranked candidate list per family. Scans both `fixtures/private/roms_extracted/decrypted/` (178 pak-decoded ROMs) AND `fixtures/private/plaintext_corpus/bludgod-roms/` (498 additional ROMs across ADM/EDM/JDM/USDM). Output: per-family TSV at `fixtures/private/checksum_discovery/<FAM>_candidates.tsv`. Smoke-ran on LF79 (31 ROMs) — surfaced 20 single-byte candidates around 0x1B00. **Manual byte-pattern review needed to separate CID-string area from true checksum slot** — that's RE work, not automation.
- **`tools/defgen/localize.py`** (`f35b05e`) — verification half of the def-pack-acceleration pair. `cousin_seed.py` clones a sibling pack and swaps CID-bearing fields without verifying; `localize.py` walks every `[[table]]` and checks each address against a target ROM. Per-table verdict: HIGH (byte-identical or shape matches) / MED (content differs, address probably still valid) / LOW (table likely moved) / ABSENT (target ROM too short). Heuristics: Shannon entropy, range ratio, monotonicity for axis tables, padding-region exemption. Smoke-tested on `definitions/baja/a2wc400k.toml` against bludgod's A2WC400M (MT) and A2WC400H (AT) — 139 of 292 tables HIGH (byte-identical between model-year variants), 153 ABSENT because bludgod ROMs are 512 KB cal-body-only vs the pack's 1 MB full-ROM target. 0 MED + 0 LOW in the in-range subset — clean signal.

### 5. clang-format sweep + CI gate flip

Installed `clang-format 18.1.8` via `pip install --user clang-format==18.1.8` (matches CI binary). Ran `clang-format -i` on every `src/**/*.{cpp,hpp}` + `tests/**/*.{cpp,hpp}` — 110 files reformatted in one shot. `.github/workflows/ci.yml` flipped: `name: clang-format (advisory)` → `(required)`, removed `continue-on-error: true`, swapped `--dry-run || true` for `--dry-run --Werror`. Full suite still green post-sweep (807 cases / 101,322 assertions).

## Status snapshot

- **HEAD `f35b05e`**, in sync with `origin/main`
- **807 unit tests / 101,634 assertions green**
- **CI clang-format gate: required**
- **Working tree clean** apart from `SubaruTuner.zip` (untracked 114 MB; gitignore-equivalent — never `git add` it, would break GitHub's 100 MB push limit)

## Open threads / known issues

### LF79101P decryption is suspect (P2 for tomorrow)

Spotted during `localize.py` smoke testing — `fixtures/private/roms_extracted/decrypted/LF79/LF79101P.bin` has random-looking bytes at calibration offsets where structured cal data should be. Compared against `LF79100P.bin` at offset 0x18F92 (fuel_open_loop_avcs_disabled_target_base_tgv_open):

- LF79100P: `00 00 00 00 00 66 66 66 66 66 66 66 66 66 66 66 ...` (structured fuel map)
- LF79101P: `5d 38 ca 47 c9 2c 9f 64 21 ad 53 a4 3d d6 a7 88 ...` (random)

And at 0x31632 (wastegate-duty maximum):
- LF79100P: `0a 00 0a 00 0a 00 0a 00 0a 00 0a 00 19 00 23 00 ...` (wgdc map at 10%/25%/35%)
- LF79101P: `c5 58 78 fe fd a2 d8 b7 c0 14 bf b6 14 a7 cb 19 ...` (random)

Hypothesis: LF79101P went through the EpifanSoft layer-1 decode but retains a per-CID layer-2 encryption (matches the documented FA-DIT pattern from `PAK_DECODE_RESULTS.md` — "newer FA-DIT firmware uses per-CID encryption; one anchor doesn't unlock siblings"). Check `bulk_decrypt_v2.py`'s output log for LF79101P to confirm it shipped through the per-family path only and not a per-CID anchor.

**Implication:** `decrypted/LF79/LF79101P.bin` is unreliable as RE input until verified. Same risk applies to any LF79xxx that wasn't a direct `PAK_*` anchor in the bulk decrypt — should audit the corpus's provenance per family.

### §11 panels surface suggestions but don't apply them (by design)

`docs/05` §11.X captures the v1.2 path: pack-format extension for `[[table.role]]` strings → `Definition::find_table_by_role(role_string)` → `ByteEdit` proposal routed through `edit::History` with the engine-safety linter on the proposed bytes. Until that lands the user finds the relevant table manually in the sidebar.

### Map-localization is verify-only

`localize.py` reports HIGH/MED/LOW/ABSENT but doesn't relocate LOW entries by pattern search. That's a v2 add — particularly useful for axis tables where the values give a unique byte fingerprint.

### Checksum discovery is candidates-only

`checksum_discover.py` produces ranked TSV reports but doesn't identify the algorithm. The bytes that change in every pair are a mix of CID string + always-changing cal cells + the actual checksum slot. Manual review separates them. Once we know the slot location, the algorithm-shape investigation (brute-force-known-algorithms helper) is a separate phase.

### Forum thread to mine — 2017 WRX engine bin

User flagged at end-of-session: <https://mhhauto.com/Thread-2017-Subaru-WRX-need-engine-bin-file> — thread on mhhauto.com discussing/sharing a 2017 WRX engine bin. Worth checking attached files for any LF79xxx CID we don't already have a clean decrypted copy of. Likely most relevant to **P2** (LF79xxx decryption-quality audit — if the forum-attached bin is independently sourced from our pak-decode pipeline, it's a check against whether LF79101P et al. are actually correctly decoded). Login may be required; cookies.txt jar at `D:\Documents\atlas-personal\forumdownloads\cookies.txt` worked for romraider.com earlier — try it here too.

### `D:\Documents\atlas-personal\` reaches a clean-room boundary

CLAUDE.md is explicit: `romraider_va_wrx.xml` + `romraider_vb_wrx.xml` in that path are Atlas-derived (instrumentation-transcoded into the RR schema) and **off-limits to direct reading** in implementer-mode sessions. Sanctioned access is via analyst-mode (`docs/analyst-mode-prompt.md`) reading only the wall-clean derivatives `va_wrx.facts.xml` / `vb_wrx.facts.xml` + `*.name-mapping.tsv`, with outputs landing in `D:\Documents\SubuwuTuner-specs\`. We don't need that path — our forum-sourced VA/VB XMLs (per `project_intree_va_vb_xml_provenance.md` memory) are §1201-clean and already produced the 25 packs at `ae090b2`.

## Plan for tomorrow's session

User asked for three things on the deck:

### Priority 1: Run cousin_seed → localize against bludgod CIDs we don't have packs for

Goal: land 5-10 new packs for previously-uncovered CIDs in one session.

```bash
# Cross-reference what bludgod has vs what definitions/ ships
ls fixtures/private/plaintext_corpus/bludgod-roms/USDM/Impreza/ | \
    awk -F'-' '{print tolower($1)}' | sort -u > /tmp/bludgod-usdm-impreza.txt
ls definitions/impreza/*.toml | xargs -n1 basename | \
    sed 's/.toml//' | sort -u > /tmp/we-have.txt
comm -23 /tmp/bludgod-usdm-impreza.txt /tmp/we-have.txt
# ^ CIDs that bludgod has but definitions/ doesn't
```

For each gap CID:
1. Pick a sibling pack from the same family (same 4-char prefix)
2. `python tools/defgen/cousin_seed.py --base <sibling.toml> --cid <NEW_CID> -o definitions/impreza/<new_cid_lower>.toml`
3. Identify a decoded sibling ROM (either from `roms_extracted/decrypted/` or another bludgod file)
4. `python tools/defgen/localize.py --pack <new_pack.toml> --sibling-rom <sib.bin> --target-rom <bludgod.hex> --out-report /tmp/<NEW_CID>_localize.tsv`
5. If HIGH+MED ≥ 80% of in-range tables: commit the pack
6. If significant LOW count: defer to manual review

**Likely first targets** (high-value because popular USDM CIDs):
- USDM Impreza WRX / STI 2002-2007 family
- USDM Forester XT 2004-2008
- USDM Legacy GT 2005-2009

Cross-reference `definitions/impreza/*.toml` + `definitions/forester/*.toml` + `definitions/legacy/*.toml` against the bludgod USDM filenames before picking. The bludgod corpus has both 512 KB cal-body and 1 MB full-ROM dumps — if the sibling pack is 1 MB and the bludgod ROM is 512 KB, expect a high ABSENT count (that's fine, it's data-quality signal not an error).

### Priority 2: Investigate LF79101P partial-decryption

Steps:
1. Inspect `fixtures/private/bulk_decrypt_v2.py`'s anchor list (around the 70-anchor block) — does LF79101P appear as a direct `PAK_*` anchor, or only as a family-share derivation?
2. If only family-share: that's the layer-2 problem. Check 3-4 other LF79xxx ROMs the same way; tally which are direct-anchor (reliable) vs family-share-only (suspect).
3. Memory entry capturing which decoded ROMs in the FA-DIT families are reliable RE inputs.
4. Add a `RECON.md` note to `fixtures/private/roms_extracted/decrypted/LF79/` flagging which siblings shouldn't be used as `localize.py` targets.

This is an honesty pass, not a fix. The data is the data; we just need to label it accurately.

### Priority 3: #39 OEM behavior reference doc

`docs/21-oem-baselines.md` — curated knowledge derived from the 178 + 498 = 676-ROM corpus. Sections:

- Cold-start enrichment shape (typical Subaru cranking enrichment vs ECT — what's the OEM ramp?)
- Knock-learn rate (how fast does DAM move on a clean tank vs a misfiring engine?)
- Boost target schedule across model years (WRX vs STI vs Forester XT)
- Closed-loop entry conditions (ECT / load / throttle thresholds across generations)
- Catalyst protection thresholds (where Subaru intervenes to save the cats)

Empirical, derived from the corpus, no new tooling. Probably 1500-2500 words, several tables. Becomes a tuning-reference manual nothing else ships.

**Sequencing:** P1 → P2 → P3 in priority order. P1 is the biggest immediate value-add; P2 is a honesty pass that informs P1's input-data choices; P3 is curated-knowledge work. Recommend doing P1 + P2 in one session and deferring P3 if time runs short.

## House-style notes (carry-over + new)

Unchanged from 2026-05-18 handoff:
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

New this session:
- **clang-format gate is now required.** Install `pip install --user clang-format==18.1.8` to match CI before editing C++ files. The binary lives at `C:\Users\Cornelio\AppData\Roaming\Python\Python314\Scripts\clang-format.exe` on the user's machine. NOT on PATH by default — invoke by full path.
- **The bludgod corpus exists at `fixtures/private/plaintext_corpus/bludgod-roms/`** — 498 plaintext `.hex` (raw-binary-content) ROMs across ADM/EDM/JDM/USDM × Forester / Impreza / Legacy / Outback / Tribeca / Exiga / XV. Already integrated into `tools/checksum_discover.py`; use it as a sibling-ROM source for `localize.py`.
- **COBB-encrypted stock ROM in user's AppData** is the user's local store, not something a tuner sent (clarified mid-session, memory updated). Do not explore `%PROGRAMDATA%\COBB\` for SubuwuTuner data extraction — that's the clean-room red line.
- **OBDX adapter ETA: May 22-25 2026** (memory `project_obdx_eta.md`) — pre-stage hardware-paired work; expect a "the adapter arrived" pivot any session after that date.
- **`D:\Documents\atlas-personal\` stays off-limits** for implementer-mode reading. The `.facts.xml` + `.name-mapping.tsv` derivatives are analyst-mode-only inputs (`docs/analyst-mode-prompt.md`). We have a clean forum-sourced VA/VB pipeline; we don't need atlas-personal.

## Suggested opener for next session

> "HEAD `f35b05e`, in sync with `origin/main`. 807 unit tests / 101,634 assertions green. clang-format CI gate is now required (codebase swept at `52757da`). §11 four-play arc fully shipped end-to-end (header → impl → tests → CLI → GUI → demo CSV). OBDX USB-CDC layer landed at `c64b717` — adapter arrival expected May 22-25.
>
> Three things on the deck for this session:
> **(P1)** Run `cousin_seed.py → localize.py` against bludgod CIDs we don't have packs for — bludgod corpus has 498 ROMs, definitions/ ships 358 packs; the gap is the opportunity. Goal: 5-10 new packs in a single session.
> **(P2)** Investigate LF79101P partial-decryption. Random-looking bytes at calibration offsets — probably layer-2 (per-CID) encryption wasn't stripped. Audit which other ROMs in `decrypted/LF79/` likely have the same issue. Honesty pass on RE-input quality, not a fix.
> **(P3)** #39 OEM behavior reference doc — `docs/21-oem-baselines.md`, curated knowledge from the 676-ROM corpus. Cold-start enrichment shape, knock-learn rate, boost schedules across years, closed-loop entry thresholds. Pure-docs, no tooling.
>
> Recommend P1 → P2 → P3 in that order. P1 = highest immediate value; P2 = honesty about data quality (informs P1's input choices); P3 = nice-to-have.
>
> Adapter not arrived yet — Phase 3/4 hardware work stays blocked. Best hardware-prep moves between now and the 22nd are P1 + P2 (more data → better Phase 4 algorithm targets)."

If the user opens with hardware news:

> "OBDX landed? First post-arrival command:
>
> ```
> subuwutuner-cli rom-pull --transport obdx --device COM5 \\
>     --def definitions/impreza/lf79103p.toml -o my_current_cal.bin
> ```
>
> That dumps your CURRENT (COBB-tuned) calibration unencrypted, bypassing the COBB AppData encryption. Win32 USB-CDC layer pre-staged at `c64b717`; the rest of the OBDX path (codec, transport, SSM/UDS clients) is already tested against MockTransport. First-light test should just work.
>
> If the device shows up at a different COM port, replace `COM5` with whatever Device Manager reports under Ports (COM & LPT). Battery > 12.0 V before connecting. The dump is read-only — no flash, no write — so this is safe even on a healthy battery with the engine off."
