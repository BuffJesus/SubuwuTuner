# SubuwuTuner — Claude orientation

> Quick context for any future Claude session that opens this repo.

## What this project is

**SubuwuTuner is a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23.** It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup.

Original work, not a port. Public references like RomRaider (GPL) and source-available competitors like Atlas (All Rights Reserved) are studied **clean-room** — concepts and protocol facts only, never expression. Boundary rules in `docs/01-reverse-engineering.md`; full methodology in `docs/15-clean-room-engineering.md`.

The working directory on disk is `D:\Subuwu\code\`; only the project's internal identity is `SubuwuTuner`. Renaming the folder breaks editor and shell sessions — defer.

## Layout

```
src/        core, rom, defs, edit, project, transport, ecu (ssm + uds),
            log, can, dbc, discover, flash, autotune, feature (+ ir),
            policy, ui (subuwutuner-gui), cli (subuwutuner-cli)
tools/defgen/   Python: RomRaider XML → our TOML; clean-room facts-only
definitions/    defgen-generated ECU packs (Path B: off-tree; see docs/17)
fixtures/   demo-pack/, demo.stune/, samples/*.stmod, demo-trace.hex
tests/unit/<module>/   Catch2 v3
tools/defgen/tests/    Python tests
docs/       design — read first; numbered 00–28
```

For module-by-module detail, read the headers under `src/<module>/include/` and the corresponding `tests/unit/<module>/`. For CLI surface, run `subuwutuner-cli --help`.

## Status snapshot (as of 2026-06-13)

**2026-06-13 — tuning-knowledge synthesis day.** Analytics + RE + code shipped hardware-free while bench rig down. Outputs at `D:/Subuwu/findings/tuning-knowledge-2026-06-13/` (`INDEX.md` is the entry point); knowledge-base at `…/knowledge-base/README.md`. Headline tuning facts to remember: **50-table common core** defines "what tuning a Subaru DIT means"; **12 AVCS baro-comp tables are dead weight** (UI should collapse by default); **COBB +Redline ≡ +SF byte-identical** at Stage 1+ (marketing labels only); **Fehr tunes Intake AVCS, COBB/NexGen tune Exhaust AVCS** — cleanest tuner-style separator; user's WRK3 corpus = 54K samples / 36 logs, **zero knock retard events / DAM pinned 1.0**, tune working as designed; **WRK3→FA24** = 4.4 KB portable / 12 KB needs-retune / 17 KB FA20-specific (MAF VE Correction is the #1 first-flash retune target). Code: `ptm import` now writes populated `edits.toml` (one undo-able ByteEdit per patch, tagged `"ptm_import"` for `History::undo_while_tag` batch revert) — CLI + GUI; `st::library::interpret_inspect`/`interpret_diff` + GUI modals shipped (8+8 templated heuristics, **per-cell delta detection still has a known +/-/sign issue on WRK2→WRK3 — fix queued**); welcome-panel freeze on dead-recent click fixed (kb-Enter + click both gated on path-existence, new ✕ remove-from-recents button); `docs/40-delta-flash-brick-protection.md` spec for v1.5 delta flash. Test suite **1702 cases / 1700 pass / 2 skip / 219,950 assertions**.

**Pre-2026-06-13 (compressed).** Phases 0–5 shipped: ECU protocols (SSM + UDS + OBD-II Mode 0x09 incl. flash flow), datalogger pipeline via MockTransport, flash orchestrator (`st::flash`) hardware-free end-to-end with delta detection + journal-based resume + optional gated 0xB6 bulk-transfer path (`docs/26`), SecurityAccess variants (factory SSMCAN1 Gen-A 16-round Feistel + COBB-AP / Fehr-active L1+L3) CLI-selectable via `--sa-variant`, auto-tune kernels (MAF + knock-pull) with `edit::History` integration, CAN-RE toolkit (Frame + .asc I/O, DBC parser/emitter/decoder, BaselineModel + ChangeDetector, .cdb bundle, five CLI subcommands), custom features (`st::feature::Graph` + IR + SH-2A codegen + RH850 codegen at parity — RH850 float-compare has a Cond::Z↔NZ open question gated on bench-rig), `subuwutuner-cli ssm-a8-poll` for RAM polling over ISO-15765 (`docs/29`), `--authenticate --sa-variant` SA prelude. GUI: docking, themes (Dark/Light with **purple accent `(0.55, 0.35, 0.85)`**), Compare/Stats/Settings panels, Sidebar (hierarchical 9-group nav), Welcome panel, autotune + Flash + first-run + FA24-swap modals. Sidebar refactored to hierarchical 9 top-level groups (was 91 flat folders), both levels default-CLOSED. `[[workflow]]` registry in defs + `History::undo_while_tag` for transactional batches; FA24-swap modal is the first consumer.

**COBB AccessPort v3 file-vault integration shipped 2026-06-11 → 2026-06-12** (T1+T2+T3 cipher all three sessions including encrypt; `ets` rename pass landed 2026-06-12 PM — `st::transport::ets` / `st::devices::ets` / CLI `ets {state,ls,pull,push,rm,backup,raw}` + GUI **AccessPort Browser** panel). Marriage gate **load-bearing** via cmd 0x28 UserInfo (spec §6.13): `Not Installed` refuses operations, `--allow-unmarried-ap` bypass. Spec §6.0 **codec-level block list** in `is_blocked_command`: cmd bytes `0x05 / 0x06 / 0x07 / 0x08 / 0x18 / 0x29 / >0x31` are rejected with `PolicyDenied` before any wire byte (defense-in-depth against §4.2 daze). **End-to-end `.ptm` cycle live-validated on user's married AP** (SUB0484551 / v1.7.6.0-28785 / 2017 USDM WRX MT CCF Gen3): AP pull → `ptm import` → `ptm export` → AP push → AP pull-back MD5 byte-identical → `ptm verify` 4244/4244 matched. **NOT yet validated through an actual ECU flash** — that's the bench-rig Phase 5.5 gate in `docs/28`.

### Load-bearing technical invariants (do not lose)

**`.ptm` cipher chain — non-standard.** Always read these from this section, never re-derive from "standard" XTEA-CBC / AES-CTR defaults:

- **XTEA-CBC outer (layer 1):** block endianness **little-endian u32 pairs** (both read and write); file trailer is **5 bytes** = `[pad_count: u8][seed: u32 big-endian]`; padding is **zero bytes** with `pad_count` carried in the trailer (NOT PKCS#7); IV split = `prev_c0 = seed; prev_c1 = seed ^ 0x5AA5A55A` (`kIvHalfXor`). Key `kXteaKey = {0x374C2D3CU, 0x67255F4BU, 0x2A6B596DU, 0x27675B40U}`.
- **AES-256 inner (layer 3) is a custom CTR construction**, not standard CTR: 16 KB chunks, per-chunk `outer_nonce`, counter-repeated-4 input blocks, AES-ECB keystream. Standard CTR produces wrong bytes.
- **bzip2 layer 4** via vendored `bzip2-1.0.8` (decompress + compress sources at `src/devices/ets/third_party/bzip2_dec/`).
- **Layer 2 base64 + `<encData>` inject**: encrypt path inserts `<encData>BASE64</encData>` at `rfind("</")` so it sits before the last closing tag of the outer XML.
- Gated behind two flags: `ST_ENABLE_COBB_AP_CIPHER=ON` (read path) and `ST_ENABLE_COBB_AP_PTM_REWRITE=ON` (write path, the asymmetric default-OFF posture).

**AP workflow umbrella flag (2026-06-14).** Introduced `ST_ENABLE_COBB_AP_WORKFLOW` at the top-level CMakeLists as the master switch for the full AP file-vault workflow (browser panel, push / pull / Save-and-Push UI, mutating wire commands, libusb device enumeration). Default OFF. When ON, force-enables `ST_ENABLE_COBB_AP_CIPHER`, `ST_ENABLE_COBB_AP_PTM_REWRITE`, and `ST_ENABLE_AP3` so a single flag arms the whole capability + propagates `ST_HAVE_AP_WORKFLOW=1` as a global compile def. `ST_ENABLE_AP3` (libusb pull-in) default flipped to OFF — distribution doesn't ship libusb-backed code. `Client::write_file_impl` / `remove_file_impl` return `PolicyDenied` when off; `stop_token.stop_requested()` is checked FIRST so the cancellation contract holds in both build modes. Cmd 0x05 NAND barrier fires automatically after every write/remove AND on graceful disconnect via `Client::nand_barrier_best_effort()`. Cornelio's local build: `-DST_ENABLE_COBB_AP_WORKFLOW=ON`.

**ETS USB wire codec.** Sync `02 00` + u24-BE `wire_len` at `[2..4]` + reserved + type + body + u32-BE CRC trailer. CRC = `zlib.crc32(data + b'\x00'*4) ^ 0xFFFFFFFF`. VID `0x1A84` / PID `0x0121`. Bulk OUT `0x03` / IN `0x82`. Fragment-tolerant write loop for Windows WinUSB's ~45 KB single-transfer cap.

**FileInfo2 envelope shapes (single vs vector are DIFFERENT — getting this wrong dazes the AP).**

- **Single `FileInfo2`** (cmd 0x20/0x21/0x22/0x25/0x26 request bodies): 30-byte prefix (`kFileInfo2Prefix`) = 27-byte `serialization::archive` magic + 3 archive-config zero bytes; uleb128 string lengths; field order `name → path → metadata`.
- **`vector<FileInfo2>`** (ListFiles response carrier): u32 LE string lengths (NOT uleb128); field order **flips to `name → metadata → path`**; 9-byte carrier (u8 flag + 2× u32 LE) before u32 LE record count; per-record u32 LE prefix `0x00000001` first / `0x00000000` subsequent.
- **`kCmd28ProbeBody` = 39 bytes** (UserInfo probe only); never reuse for FileInfo2-bearing commands.
- cmd 0x21 file-data responses tolerate a **zero-CRC trailer** (firmware-skipped CRC).
- cmd 0x20 setup `path` is the **full relative path, not the directory**.

**Boot integrity gate (SH-2A 2 MB, verified on LF79103P).** `FUN_00000C54` three-signature check, gated by `FUN_00000D6E` before the JMP to `*(uint32_t *)0x00000D00 = _main` at `0x001F094C`. Signatures: `*(u16 *)0x00006000 == 0x5555`; `*(u16 *)0x001FFFF2 == 0xAAAA`; `*(u16 *)0x0000006C == *(u16 *)0x00006010`. Host-side mirror at `st::flash::verify_boot_signatures_sh2a_2mb`. **Bootloader sectors `0x00..0x0F` are FCU-locked**; the sector-erase allow-list excludes them. Aftermarket CRC slot table at `0x1FFF3C..0x1FFFA0` is **NOT runtime-validated** (AP-side metadata only). `0x4000` "secureboot stub" is **dead code** (Renesas runtime-library leaf helpers). See `docs/31` for the full recipe.

**Architectural classifier.** `st::devices::ets::Layer` + `LayerMap` + `classify()` + `classify_patches()` + `summarize()` + `layer_label()`. Default LF79103P region map per `docs/35-tuner-overlay-architecture.md`. `classify()` uses **most-specific (smallest containing range) match**, not first-match — `PrimaryCode` and `SecondaryCode` sit inside `MainCalibration` intentionally; first-match would always return MainCalibration for code addresses.

**Open live-hardware issue: cmd 0x21 large-file truncation.** Setup ACK echoes `name="."`, `size=0` for files in subdirectories (e.g. `/maps/Stage1*.ptm`); cmd 0x21 then never sends a body. `/backupcksum` (root) works. Workaround: `ST_AP3_READFILE_DRAIN_MODE=1` reads in 512-byte chunks until idle. Full byte-level decode at `findings/handoffs/HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation.md`.

**Workflow tips for users on Windows + Git Bash.** `MSYS_NO_PATHCONV=1` prefix is required for any CLI arg containing `/maps/`-style paths — without it Git Bash rewrites them to `C:/Program Files/Git/maps`. `ST_PTM_BASE_ROM_DIR/<vehicle_id>.bin` is the auto-discovery hook for `ptm import` base ROMs. `ST_AP3_TRACE_USB=1` dumps every USB OUT/IN payload as 16-byte rows.

**Path B distribution posture in effect** (per `docs/17`): public repo does NOT bundle `definitions/va/` or `definitions/vb/`. Definitions are user-supplied at runtime.

**Hardware gates** (OBDX Pro VX in hand 2026-05-24, bench rig stand-down 2026-06-13): Phase 1 ship gate (≥20 maps from a real definition pack on a real ROM), SSM/UDS validation against real ECUs, BLAKE3 upgrade for flash hashing, HIL tests against junkyard ECUs (`docs/08` Tier 4), delta-flash brick-protection validation (`docs/40` adds 4 tests on top of `docs/31`'s 5).

## Quick orientation for common tasks

| If the user asks you to… | Start here |
|---|---|
| Discuss the overall design | `docs/00-overview.md`, `docs/02-architecture.md` |
| Look at ECU protocols / definition format | `docs/01-reverse-engineering.md`, `docs/11-definition-format.md` |
| Set up CMake, vcpkg, CI | `docs/07-build-and-tooling.md` |
| Decide on a GUI framework | `docs/03-tech-stack.md` |
| Plan a phase or milestone | `docs/04-roadmap.md` |
| Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/31-brick-protection-by-isa.md` (per-ISA recipes), `docs/08-testing-strategy.md` Tier 4 |
| Reason about emissions / jurisdiction policy | `docs/06-legal-ethics.md` |
| Reason about clean-room IP boundaries | `docs/15-clean-room-engineering.md` |
| Reason about auto-tune | `docs/12-auto-tuning.md` |
| Reason about custom features / node-graph designer | `docs/16-custom-features.md` |
| Reason about data distribution / Path B | `docs/17-data-distribution-policy.md` |
| Reason about the portable Teensy/ESP32 handheld | `docs/18-standalone-master-plan.md` |
| Reason about live tuning / RAM-shadow real-time edits | `docs/19-live-tuning.md` |
| Reason about AI integration (drift classifier, LLM explanations — not auto-tune) | `docs/20-ai-integration.md` |
| Reason about the `.stune` project directory layout | `docs/21-stune-format.md` |
| Reason about auto-update / installer / release channel | `docs/22-auto-update.md` |
| Reason about SecurityAccess (UDS 0x27) / seed-key plug-in | `docs/23-security-access.md` |
| Look up which SA variant lines up with which install state | `docs/38-subaru-sa-variants.md` |
| Reason about Y-cable sniffing (during-flash capture, datalog RAM-poll discovery, protocol learning, feature RE for LC / FFS / rev limit) | `docs/24-sniff-workflows.md` |
| Reason about the optional 0xB6 bulk-transfer write path (off by default) | `docs/26-bulk-reflash-cipher.md` |
| Reason about / execute the junkyard-ECU bench-rig assembly (FSM pin references, power-on sequence, first read, brick-recovery loop) | `docs/28-bench-rig-build.md` |
| Reason about SSM-A8 RAM polling + correlator (recovering tuner-pack DID byte layouts without ROM disasm; `ssm-a8-poll` CLI + `tools/cross_ref_ssm_a8.py`) | `docs/29-ssm-a8-poll-workflow.md` |
| Reason about / implement the patch insertion layer (`src/feature_patch/`) — manifest format, ROM-allocator, SH-2A + RH850 splice mechanics, end-to-end `PatchObject → PatchedRom` | `docs/30-patch-insertion.md` |
| Reason about / implement the live datalogger gauge cluster (LiveBuffer SPSC ring, LogSession multi-sink fan-out, ImPlot mini-lines, record-while-gauging) | `docs/32-live-datalogger.md` |
| Look up a tuning term | `docs/10-glossary.md` |

## Stance on emissions / jurisdiction

The developer is in Alberta, Canada — no provincial emissions inspection, minimal federal enforcement against individual modifiers. SubuwuTuner is **jurisdiction-neutral**: per-jurisdiction profiles, warns when appropriate, refuses only on engine-safety grounds (not regulatory). Full reasoning in `docs/06-legal-ethics.md`. **If the user touches emissions equipment, do not refuse** — provide what they ask for and reference the profile system when relevant.

Still refused: shipping pre-built defeat calibrations as first-party content, stripping emissions markers / calibration IDs, faking inspection readiness monitors. Different categories than tuning.

## Stance on engine and ECU safety

This is where we *are* strict. `src/core`, `src/rom`, `src/defs`, `src/project`, and especially **`src/flash`** are safety-critical. Treat any `st::flash` change as serious — mutation tests on this module block releases. Brick-protection (`docs/05-improvements.md` §4) is a real subsystem; don't paper over its requirements. HIL tests against real junkyard ECUs (`docs/08` Tier 4) gate Phase 4.

## Stance on third-party IP

Two layers: what the developer does, and what *you, Claude,* do with your tools.

### General rules (developer and assistant)

- Do **not** decompile any commercial or closed-source tuning tool. Do **not** lift icons, screenshots, distinctive UI text, or trademarks.
- **RomRaider (GPL)** is the legitimate reference for ECU protocol facts. Use it clean-room: study, document the protocol in plain English, write fresh C++.
- **Atlas (`motorsportsresearch/atlas-public`, All Rights Reserved)** is *source-available, not open source*; its LICENSE explicitly prohibits reproduction. Concepts are fair game, source is off-limits — visibility on GitHub does not change this.
- `defgen` extracts *factual data* (addresses, scalings) from public XML — facts aren't copyrightable; expression (description prose) is and gets stripped.
- The line is **idea / expression**. A "node-graph custom feature designer" is an idea — build one freely. A specific node class hierarchy, file format, or compiler implementation copied from Atlas is expression — don't.
- **Path B distribution posture (current).** Public Apache-2.0 release ships infrastructure (loader, format, edit/undo, flash orchestrator, auto-tune, GUI) but does NOT bundle VA/VB WRX calibration packs. Definitions are user-supplied at runtime. See `docs/17` (reasoning) and `docs/install.md` (user workflow). Distribution choice (§1201/trade-secret axis), not clean-room compliance (handled by the wall in `docs/15`).

### Rules specific to you, Claude

Your tools (`web_fetch`, `view`, `bash_tool`, `conversation_search`) can pull protected source into this session and from there into the codebase. Off-limits for any task producing code, specs, or docs destined for the repo:

- **Do not `web_fetch`** anything under `github.com/motorsportsresearch/atlas-public/` other than `README.md` and `LICENSE` (orient only — no `.java`, `.kt`, `.xml`, definitions, or editor screenshots).
- **Do not `web_fetch`** RomRaider Java source — would be GPL contamination of an Apache 2.0 codebase. RomRaider's public *protocol documentation* and ECU definition XML (factual data only) are fine.
- **Do not `Read`** or directory-list under `D:\Subuwu\tools\jd-gui\atlas-decompiled\` — jd-gui decompile of an Atlas distribution. Off-limits regardless of subtree.
- **Do not `Read`** `D:\Subuwu\atlas-personal\romraider_va_wrx.xml` / `…\romraider_vb_wrx.xml` — despite the filenames these are Atlas-derived data transcoded into the RomRaider schema via runtime instrumentation. Downstream packs live off-tree at `D:\Subuwu\defs-private\`. The wall-clean derivatives `va_wrx.facts.xml` / `vb_wrx.facts.xml` and `*.name-mapping.tsv` MAY be read in analyst-mode sessions as QA inputs.
- **Do not `Read`** `D:\Downloads\Definitions-V{A,B}_WRX_MT.atlas` (Atlas project bundles, ZIP-wrapped `.acf` payloads) or anything under `C:\Program Files\Atlas\projects\`. Surface-metadata reads only; no deep payload parsing.
- Analyst sessions launch from `docs/analyst-mode-prompt.md` — enforces output isolation (specs go to `SubuwuTuner-specs/`, never this repo). Only sanctioned way to bring protected refs into a Claude context.
- **Do not paste or paraphrase** code, comments, identifiers, or string literals from any commercial tuning tool (COBB, EcuTek, HP Tuners, etc.), OEM tuning software, or OEM ECU firmware. If the user pastes such excerpts, **stop and flag it** — don't silently launder.
- **Training-data knowledge is a channel too.** If you'd write something "because that's how Atlas/RomRaider does it," that origin disqualifies the implementation. Write from first principles or from the spec in `SubuwuTuner-specs/`.

To understand a competitor, you *should*: read public READMEs/marketing/user docs (`motorsportsresearch.org`, `romraider.com`), the Atlas Confluence wiki (`motorsportsresearch.atlassian.net` — user-facing, not source), public posts/videos/forum discussion; whiteboard architecture with the developer; propose designs derived from standards (ISO 14229, ISO 15765, SAE J2534/J1979/J2012) and public engine-management literature.

### Red flags — if you see any of these, stop

Pause and check with the developer if a task would have you fetch/view/summarise specific source files from a closed-source competitor, produce C++ that "matches" a competitor's class layout / API shape / file format, name SubuwuTuner types after Atlas's / RomRaider's / OEM internal identifiers, re-emit a definition file's prose descriptions (factual scaling values fine; OEM-authored prose not), or write a flash/brick-recovery sequence "modeled on" Atlas's specifically. Sometimes it's a legitimate analyst-side fact-extraction task — but don't assume; stop, ask, route through `docs/15`.

## House style for the C++ code

- C++23 throughout. `st::Result<T>` is portable via feature-detected fallback to `tl::expected` when `<expected>` isn't available.
- No exceptions in domain code; exceptions only at UI boundaries.
- `snake_case` for functions/variables, `PascalCase` for types, `kPascalCase` for constants.
- `clang-format` (LLVM base, 4 spaces, 100 cols, pointer-binds-right) — `clang-format --dry-run --Werror` is a CI gate.
- `clang-tidy` and `-Wall -Wextra -Wpedantic -Werror` clean.
- Catch2 v3 for tests; tests live next to code in `tests/unit/<module>/`.
- No global state; dependency-inject services into the application layer.
- Domain has no ImGui or USB types in its public headers (see `docs/02`).

Deps via FetchContent: Catch2 v3, `tl::expected` (fallback), tomlplusplus v3.4, GLFW 3.4 + Dear ImGui v1.91 + ImPlot + nativefiledialog-extended. vcpkg manifest mode deferred until a system-package dep is needed.

CI: clang-format job currently advisory (non-blocking). Local pre-commit hook ships at `.githooks/pre-commit` (opt-in: `git config core.hooksPath .githooks`); CI flips to required once contributors are reliably running the hook. Matrix: Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan.

## Working with this user

- Windows (Cornelio, win32, `D:\Subuwu\code`). Bash shell available; PowerShell also available.
- Prefer `/` in shell commands; use `\` for Windows-path strings to the user.
- The user pushed back on emissions paternalism early. **Treat them as a knowledgeable adult who has read the docs.**

Repo: `https://github.com/BuffJesus/SubuwuTuner`.
