# SubuwuTuner — Claude orientation

> Quick context for any future Claude session that opens this repo.

## What this project is

**SubuwuTuner is a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23.** It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup.

Original work, not a port. Public references like RomRaider (GPL) and source-available competitors like Atlas (All Rights Reserved) are studied **clean-room** — concepts and protocol facts only, never expression. Boundary rules in `docs/01-reverse-engineering.md`; full methodology in `docs/15-clean-room-engineering.md`.

The working directory on disk is `D:\Documents\JetBrains\SubaruTuner\`; only the project's internal identity is `SubuwuTuner`. Renaming the folder breaks editor and shell sessions — defer.

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

## Status snapshot (as of 2026-05-26)

Phase 0/1/2 done. Phase 3 protocol framing complete (SSM + full UDS catalog incl. flashing flow + OBD-II Mode 0x09 vehicle-info) and datalogger pipeline end-to-end via MockTransport. Phase 4 flash orchestrator (`st::flash`) end-to-end hardware-free incl. delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based resume; optional gated 0xB6 bulk-transfer write path (see `docs/26`). SecurityAccess: factory SSMCAN1 (Gen-A 16-round Feistel) + COBB-AP / Fehr-active L1+L3 variants in tree, CLI-selectable via `--sa-variant`. v1.1 auto-tune kernels (MAF + knock-pull) shipped with CSV readers, smoothing, lint, CLI, and project-integration via `edit::History`. CAN reverse-engineering toolkit replay-path complete (Frame + .asc I/O, DBC parser/emitter/decoder, BaselineModel + ChangeDetector, .cdb bundle, five CLI subcommands). Phase 5 custom-features: `st::feature::Graph` + `st::feature::ir` (lower, dump, lint, RT-budget cost) shipped; **SH-2A codegen for VA shipped** (designer canvas + sample graphs in `fixtures/samples/`); RH850 codegen for VB is now the single biggest open feature in the tree. GUI: docking, themes (Dark/Light, **purple accent** `(0.55, 0.35, 0.85)`), autotune modals, Flash… modal with policy gate, Stats panel, status-bar profile chip, jurisdiction-profile persistence, Settings dialog.

**Path B distribution posture in effect** (per `docs/17`): public repo does NOT bundle `definitions/va/` or `definitions/vb/`. Definitions are user-supplied at runtime.

**Hardware gates** (OBDX Pro VX in hand 2026-05-24 — these are now actionable, not blocked): Phase 1 ship gate (≥20 maps from a real definition pack on a real ROM), SSM/UDS validation against real ECUs, BLAKE3 upgrade for flash hashing, HIL tests against junkyard ECUs (docs/08 Tier 4).

## Quick orientation for common tasks

| If the user asks you to… | Start here |
|---|---|
| Discuss the overall design | `docs/00-overview.md`, `docs/02-architecture.md` |
| Look at ECU protocols / definition format | `docs/01-reverse-engineering.md`, `docs/11-definition-format.md` |
| Set up CMake, vcpkg, CI | `docs/07-build-and-tooling.md` |
| Decide on a GUI framework | `docs/03-tech-stack.md` |
| Plan a phase or milestone | `docs/04-roadmap.md` |
| Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/08-testing-strategy.md` Tier 4 |
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
| Reason about Y-cable sniffing (during-flash capture, datalog RAM-poll discovery, protocol learning, feature RE for LC / FFS / rev limit) | `docs/24-sniff-workflows.md` |
| Reason about the optional 0xB6 bulk-transfer write path (off by default) | `docs/26-bulk-reflash-cipher.md` |
| Reason about the user's specific Fehr e-tune (cal delta vs factory, SA L1/L35 status, patch manifests) | `docs/27-fehr-analysis-2026-05-26.md` |
| Reason about / execute the junkyard-ECU bench-rig assembly (FSM pin references, power-on sequence, first read, brick-recovery loop) | `docs/28-bench-rig-build.md` |
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
- **Do not `Read`** or directory-list under `C:\Users\Cornelio\Desktop\jd-gui-master\atlas-decompiled\` — jd-gui decompile of an Atlas distribution. Off-limits regardless of subtree.
- **Do not `Read`** `D:\Documents\atlas-personal\romraider_va_wrx.xml` / `…\romraider_vb_wrx.xml` — despite the filenames these are Atlas-derived data transcoded into the RomRaider schema via runtime instrumentation. Downstream packs live off-tree at `D:\Documents\SubuwuTuner-defs-private\`. The wall-clean derivatives `va_wrx.facts.xml` / `vb_wrx.facts.xml` and `*.name-mapping.tsv` MAY be read in analyst-mode sessions as QA inputs.
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

CI: clang-format job currently advisory (non-blocking) — no pre-commit hook yet. Flip to required once one is wired in. Matrix: Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan.

## Working with this user

- Windows (Cornelio, win32, `D:\Documents\JetBrains\SubaruTuner`). Bash shell available; PowerShell also available.
- Prefer `/` in shell commands; use `\` for Windows-path strings to the user.
- The user pushed back on emissions paternalism early. **Treat them as a knowledgeable adult who has read the docs.**

Repo: `https://github.com/BuffJesus/SubuwuTuner`.
