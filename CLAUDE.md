# SubuwuTuner — Claude orientation

> Quick context for any future Claude session that opens this repo.

## What this project is

**SubuwuTuner is a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23.** It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup.

This is original work, not a port. Public references like RomRaider (GPL) and source-available competitors like Atlas (All Rights Reserved) are studied **clean-room** — concepts and protocol facts only, never expression. See `docs/01-reverse-engineering.md` for the day-to-day boundary rules and `docs/15-clean-room-engineering.md` for the full methodology.

## What is already in the workspace

```
SubuwuTuner/
├── CLAUDE.md                            (this file)
├── README.md, LICENSE (Apache 2.0), NOTICE, DISCLAIMER.md
├── CMakeLists.txt, CMakePresets.json    (Phase 0 build system)
├── .clang-format, .clang-tidy, .gitignore, .gitattributes
├── cmake/
│   ├── CompilerWarnings.cmake             (st::warnings + st::platform interface targets)
│   └── Sanitizers.cmake                   (st::sanitizers; driven by ST_SANITIZE list)
├── src/
│   ├── core/                              (st::core — Result, Error, Version, Crc32)
│   ├── rom/                               (st::Rom — file I/O, BE/LE reads + writes, slice, scan_ascii, crc32)
│   ├── defs/                              (st::Definition — TOML single-file + directory loader, typed reads + writes, scaling + inverse, axis/table values, diff, writeback)
│   ├── edit/                              (st::edit — Rect, set/add/multiply/percent/smooth/interpolate, Snapshot, History undo/redo)
│   ├── project/                           (st::Project — .stune directory persistence: source + working + def reference)
│   ├── transport/                         (st::transport — ITransport interface + MockTransport for hardware-free testing)
│   ├── ecu/                               (st::ecu::ssm — A8 read + B0/B8 write; st::ecu::uds — full ISO 14229 catalog)
│   ├── log/                               (st::log — LogStream SPSC ring + LogSession SSM I/O loop; Phase 3 datalogging)
│   ├── can/                               (st::can — Frame type + .asc reader/writer; foundation for the docs/14 CAN toolkit)
│   ├── dbc/                               (st::dbc — DBC parser + emitter; BO_/SG_ message + signal definitions)
│   ├── discover/                          (st::discover — BaselineModel + ChangeDetector for CAN reverse engineering)
│   ├── flash/                             (st::flash — Flasher + FlashPlan: read-full-ROM, sector erase/write/verify orchestration over UdsClient)
│   ├── ui/                                (subuwutuner-gui — Dear ImGui front-end on top of the domain libs)
│   └── cli/                               (subuwutuner-cli — rom-info, dump-axis, dump-table[--csv], rom-diff, table-edit, project-{new,info,edit,undo,redo}, pack-info, table-list, log, can-{replay,diff,discover,export-dbc,decode}, flash-{plan-info,delta})
├── tools/defgen/                          (Python: RomRaider XML -> our TOML; clean-room facts-only; handles <base> inheritance)
├── fixtures/
│   ├── demo-pack/                         (committed: multi-file demo TOML pack — axes, scalings, tables, pids)
│   ├── demo.stune/                        (committed: synthetic project, 1024-byte ROM)
│   └── demo-trace.hex                     (committed: SSM-response trace for `log` subcommand demo)
├── scripts/gen-demo-fixture.py            (regenerates fixtures/demo.stune source.bin)
├── .idea/runConfigurations/               (shared CLion/Rider launches: GUI demo, CLI variants, ctest)
├── tests/unit/{core,rom,defs,edit,project,transport,ecu,log,can,dbc,discover}/   (267 C++ tests; 36 Python tests under tools/defgen/tests/)
├── .github/workflows/ci.yml             (Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan)
└── docs/                                (design — read first; 00–12)
```

**Phase 0 done. Phase 1 CLI side done. Phase 2 MVP done. Phase 3 protocol-side started.** What's shipped:

- `st::Rom` — file I/O, big/little-endian typed reads + writes with overflow-safe bounds, slice, ASCII scanner, CRC32, mutable data span
- `st::Definition` — TOML single-file + directory loader (tomlplusplus), cross-reference validation, CID matching, typed value reads + writes, linear + piecewise scaling (both directions), axis-value extraction, table-value extraction (1D + 2D), per-table diff, writeback
- `st::edit` — Rect-scoped cell operations (set/add/multiply/percent/smooth/interpolate), Snapshot, History stack with branching-undo semantics
- `st::Project` — `.stune` directory persistence: copies source ROM, tracks working ROM, references definition pack, schema-versioned, CRC32 tampering detection on reopen
- `st::transport` — `ITransport` pure-virtual interface (open/close/send/send_recv/start_streaming/stop_streaming) + `MockTransport` for hardware-free SSM/UDS development. Real adapters wait on hardware.
- `st::ecu::ssm` — Subaru Select Monitor: address read (0xA8), single-byte address write (0xB0), and block write (0xB8) framing + `SsmClient`. Framing is per public documentation; needs validation against a real ECU when hardware lands.
- `st::ecu::uds` — ISO 14229 catalog complete: RDBI/WDBI (0x22/0x2E), ReadMemoryByAddress (0x23), WriteMemoryByAddress (0x3D), SecurityAccess (0x27), CommunicationControl (0x28), DiagnosticSessionControl (0x10), ECUReset (0x11), TesterPresent (0x3E), the three-step download flow (RequestDownload 0x34, TransferData 0x36, RequestTransferExit 0x37), and RoutineControl (0x31 with well-known RIDs eraseMemory / checkProgrammingDependencies / eraseMirrorMemory). Same framing-only caveat — validation pending real VB ECU. Two end-to-end tests exercise the flash flow through MockTransport: a minimal sketch and a realistic flow with erase + post-flash check-deps bracketing the download.
- `st::log` — `LogStream` (SPSC lock-free ring; stress-tested at 50k samples), `LogSession` (I/O-thread orchestrator: batched SSM A8 reads, per-channel scaling, timestamped sample push, cooperative shutdown), and `CsvSink` (header with per-channel `id [unit]`, ms-resolution timestamps, per-channel precision from scaling). End-to-end exercised by the `log` CLI subcommand.
- `st::can` — `Frame` type (classic CAN, 8-byte payload, standard or extended ID, BusId for multi-bus captures) and Vector `.asc` reader/writer (the lingua-franca log format used by SavvyCAN, cantools, CANalyzer, Wireshark). Header lines tolerated; CRLF tolerated; zero-DLC frames supported; round-trip stable.
- `st::dbc` — DBC (CAN database) parser + emitter + signal decoder. Parses `VERSION`, `BU_`, and `BO_`/`SG_` rows; preserves message id (with extended-ID flag via the high bit), length, sender, and per-signal name / start_bit / length / byte-order (Intel ↔ Motorola) / sign / factor / offset / min / max / unit / receivers. Unknown sections (NS_, BS_, CM_, BA_, VAL_, etc.) skip silently so any DBC tool's output round-trips through us. Round-trip stable; `find_message`/`find_signal` lookups. `extract_raw_bits` + `decode_signal` walk the Vector bit-numbering scheme (bit 0 = LSB of byte 0, bit 7 = MSB of byte 0) for both Intel and Motorola, sign-extend signed fields, and apply factor/offset.
- `st::flash` — Phase 4 flash orchestrator. `Flasher::read_full_rom` chunks UDS ReadMemoryByAddress for arbitrarily-sized reads. `Flasher::compute_delta` is a sector-aligned byte-diff that emits the `Sector` list a `FlashPlan` should target. `Flasher::execute(FlashPlan)` runs the full sequence — DiagnosticSessionControl(programming), optional CommunicationControl-off, per-sector { eraseMemory routine, RequestDownload, chunked TransferData honouring the ECU's reported `maxNumberOfBlockLength`, RequestTransferExit, checkProgrammingDependencies, optional verify via read-back }, CommunicationControl-on — and returns a `FlashReport` with per-sector outcome flags so partial failures are diagnosable. `dry_run` mode exercises only session+CC negotiation (no erase, no write), keeping flash-safe; per docs/05 §4 the literal "skip the write" interpretation would brick the sector and is deliberately avoided. `parse_plan`/`format_plan`/`read_plan`/`write_plan` round-trip a `FlashPlan` through TOML (schema_version-gated, hex-encoded data fields in `"""…"""`). `build_manifest(plan, plan_text, report)` produces a tamper-evident audit record per docs/05 §4: per-sector CRC32 + overall CRC32 of the transferred bytes + CRC32 of the source plan TOML + ISO-8601 timestamp; `parse_manifest`/`format_manifest`/`read_manifest`/`write_manifest` round-trip it as TOML. Setting `FlashPlan.journal_path` makes `Flasher::execute` rewrite a manifest snapshot after every per-sector outcome update — best-effort write failure, doesn't abort the flash — which is the resume-from-crash foundation per docs/05 §4: after a host-process death, the journal on disk reflects the last successfully completed sector plus any partial sector with `transferred=false`. Current hash is CRC32 (detection of accidental corruption); BLAKE3 is the upgrade target once the bench rig lands. All paths covered by 30 MockTransport- and TOML-driven tests; mutation testing on this module blocks releases per docs/08 Tier 4.
- `st::discover` — the CAN reverse-engineering algorithm. `BaselineModel::observe`/`finalize` classifies each byte position of each observed CAN ID as Stable, Cyclic (modal-non-zero-delta detector — catches both strict and mod-N counters), or Noisy; records per-byte stable value sets, modes, and per-id frequency in Hz. `ChangeDetector::observe` runs the watch phase frame-by-frame, surfaces `DiscoveryEvent`s for new IDs and for stable-byte deviations (cyclic ignored, noisy off by default), coalesces multi-byte changes in one frame, and debounces (500 ms default). `.cdb` (CAN Discovery Bundle) TOML format round-trips baseline + events. `can-replay`, `can-diff`, `can-discover`, and `can-export-dbc` CLI subcommands all wired against the replay path; `can-record` and live-mode `can-discover --live` wait on hardware. Same algorithm reused once live mode lands.
- `tools/defgen/` — Python tool that translates public RomRaider XML to our TOML schema; clean-room rules in `docs/01-reverse-engineering.md`. Handles `<base>` inheritance. `--apply-to-pack <existing.toml>` appends only records (by id) not already present, preserving hand-edits and comments byte-for-byte. Per-pack `warnings` list flags non-linear `toexpr` that got flattened to identity and unknown storage/endian combinations defaulted to `uint16_be`; surfaced on stderr. Standard-library Python only (tomllib for parsing the merge target).
- CLI: `rom-info`, `dump-axis`, `dump-table [--csv]`, `rom-diff`, `table-edit`, `project-{new,info,edit,undo,redo}`, `pack-info`, `table-list`, `log` (replays an SSM-response trace file through MockTransport and writes CSV), `can-replay` (per-id stats on a .asc file), `can-diff` (A-only / B-only / shared-with-delta across two .asc captures), `can-discover --from <FILE.asc> [--baseline <secs>] [--bus <0..3>] [--output <session.cdb>]` (drives BaselineModel + ChangeDetector offline, emits a .cdb Bundle), `can-export-dbc <session.cdb> [--output <draft.dbc>]` (one BO_ per baseline id, one SG_ per labeled Change event at identity scaling, Motorola/unsigned per docs/14), `can-decode --dbc <FILE.dbc> <FILE.asc> [--output <csv>]` (long-format CSV: one row per (frame, signal); skips frames whose id is not in the DBC), `flash-plan-info <FILE.toml>` (loads + summarizes a flash plan), `flash-delta <SOURCE.bin> <TARGET.bin> [--sector-size <N>] [--base-address <addr>] [--output <plan.toml>]` (computes a delta-flash plan covering every sector that differs between two ROMs of equal size). 1D / 2D / 3D tables all dump correctly. Project edit history persists in `edits.toml` for cross-session undo.

The end-to-end persistent edit workflow is exercisable without hardware:

```
$ subuwutuner-cli project-new --source stock.bin --def pack/ my.stune
$ subuwutuner-cli project-edit --table fuel_map --rows 0:0 --cols 2:3 set 12.5 my.stune
$ subuwutuner-cli project-info my.stune          # "edits applied", new CRC32
$ subuwutuner-cli dump-table --def pack/ --table fuel_map my.stune/working.bin
$ subuwutuner-cli rom-diff --def pack/ my.stune/source.bin my.stune/working.bin
```

What remains for the full Phase 1 ship gate: a real RomRaider XML through `defgen`, verified against a real stock dump showing ≥ 20 factory maps with correct scaling. That's a hardware/data gate; user is waiting on the OBDX Pro VX adapter to land before they can dump their own car. **Until then, do not block work on it.** Remaining hardware-free polish (3D tables, definition inheritance, defgen `<base>` handling) and Phase 3 implementation (SSM/UDS clients against MockTransport with canned traces) are all open.

The working directory on disk is still `D:\Documents\JetBrains\SubaruTuner\` — only the project's internal identity is `SubuwuTuner`. Renaming the folder would break editor and shell sessions; defer it.

## Quick orientation for common tasks

| If the user asks you to… | Start here |
|---|---|
| Discuss the overall design | `docs/00-overview.md` and `docs/02-architecture.md` |
| Look at ECU protocols / definition format | `docs/01-reverse-engineering.md`, `docs/11-definition-format.md` |
| Set up CMake, vcpkg, CI | `docs/07-build-and-tooling.md` |
| Decide on a GUI framework | `docs/03-tech-stack.md` |
| Plan a phase or milestone | `docs/04-roadmap.md` |
| Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/08-testing-strategy.md` Tier 4 |
| Reason about emissions / jurisdiction policy | `docs/06-legal-ethics.md` |
| Reason about clean-room IP boundaries | `docs/15-clean-room-engineering.md` |
| Reason about auto-tune | `docs/12-auto-tuning.md` |
| Reason about custom features / node-graph designer | `docs/16-custom-features.md` |
| Look up a tuning term you don't recognize | `docs/10-glossary.md` |

## Stance on emissions / jurisdiction

The developer is in Alberta, Canada, where there is no provincial emissions inspection and federal enforcement against individual modifiers is minimal. SubuwuTuner is **jurisdiction-neutral**: it ships configurable per-jurisdiction profiles, warns when appropriate, and refuses only on engine-safety grounds (not regulatory grounds). Full reasoning in `docs/06-legal-ethics.md`. If the user is exploring features that touch emissions equipment, do not refuse — provide what they ask for and reference the profile system if context calls for it.

What *does* still get refused: shipping pre-built defeat calibrations as first-party content, stripping emissions markers / calibration IDs from a ROM, and faking inspection readiness monitors. Those are different categories than tuning.

## Stance on engine and ECU safety

This is where we *are* strict. The four core modules in `src/core`, `src/rom`, `src/defs`, `src/project` and especially **`src/flash`** are safety-critical:

- Treat any change to `st::flash` as a serious change. Mutation tests on this module block releases by policy.
- Brick-protection (`docs/05-improvements.md` §4) is a real subsystem, not marketing. Don't paper over its requirements.
- HIL tests against real junkyard ECUs (`docs/08-testing-strategy.md` Tier 4) gate Phase 4 of the roadmap.

## Stance on third-party IP

Two layers: what the developer does, and what *you, Claude,* do with your tools.

### General rules (developer and assistant)

- Do **not** decompile any commercial or closed-source tuning tool.
- Do **not** lift icons, screenshots, distinctive UI text, or trademarks from any other tool.
- **RomRaider (GPL)** is the legitimate technical reference for ECU protocol facts. Use it clean-room: study, document the protocol in plain English, write fresh C++.
- **Atlas (`motorsportsresearch/atlas-public`, All Rights Reserved)** is *source-available, not open source*. The repo's own LICENSE file explicitly prohibits reproduction. Atlas is treated like any other proprietary competitor: concepts are fair game, source is off-limits. The fact that the source is visible on GitHub does not change this.
- The `defgen` tool extracts *factual data* (addresses, scalings) from public XML — facts aren't copyrightable; expression (description text) is and gets stripped.
- The line is **idea / expression**. A "node-graph custom feature designer" is an idea — build one freely. A specific node class hierarchy, file format, or compiler implementation copied from Atlas is expression — don't.
- See `docs/01-reverse-engineering.md` for day-to-day boundaries and `docs/15-clean-room-engineering.md` for the full methodology, including the analyst/implementer wall and the solo-developer adaptations.

### Rules specific to you, Claude

You have tools (`web_fetch`, `view`, `bash_tool`, `conversation_search`) that can pull protected source into this session and from there into the SubuwuTuner codebase. Treat the following as off-limits for any task that will produce code, specs, or documentation destined for the repo:

- **Do not `web_fetch`** any file under `github.com/motorsportsresearch/atlas-public/` other than the `README.md` and `LICENSE`. The README and LICENSE are fine — they're how you orient. Any `.java`, `.kt`, `.xml`, definition file, or screenshot of the Atlas editor is not.
- **Do not `web_fetch`** RomRaider source files. RomRaider's public *protocol documentation* and its public ECU definition XML (factual data only) are acceptable; its Java source is not, because the result would be GPL contamination of an Apache 2.0 codebase.
- **Do not paste or paraphrase** code, comments, identifier names, or string literals from any commercial tuning tool (COBB, EcuTek, HP Tuners, etc.), OEM tuning software (Subaru SSM, dealer tools), or OEM ECU firmware.
- **If the user pastes** code or excerpts from any of the above into the chat, **stop and flag it** before incorporating it. Don't silently launder it into a SubuwuTuner contribution.
- **Training-data knowledge is also a channel.** If you would have written a function a certain way "because that's how Atlas does it" or "because RomRaider does it like this," that origin disqualifies the implementation. Write from first principles or from the spec in `SubuwuTuner-specs/`.

What you *should* do when you need to understand a competitor:

- Read public README, marketing, and user-facing documentation (`motorsportsresearch.org`, `romraider.com`, etc.).
- Read the Atlas Confluence wiki (`motorsportsresearch.atlassian.net`) — that's user-facing documentation, not source.
- Read public posts, videos, and forum discussion that describe behavior at the user level.
- Discuss concepts and architecture at the whiteboard level with the developer.
- Propose SubuwuTuner designs derived from the standards (ISO 14229, ISO 15765, SAE J2534/J1979/J2012) and from public engine-management literature.

### Red flags — if you see any of these, stop

If a task would have you do any of the following, pause and check with the developer before continuing:

- Fetching, viewing, or summarising specific source files from a closed-source or restrictively-licensed competitor.
- Producing C++ that "matches" a competitor's class layout, API shape, or file format.
- Naming SubuwuTuner types after Atlas's types, RomRaider's types, or any OEM internal identifiers.
- Re-emitting a definition file's prose descriptions (factual scaling values are fine; OEM-authored prose is not).
- Writing a flash routine or brick-recovery sequence "modeled on" Atlas's specifically.

None of these are necessarily fatal — sometimes the user is doing an explicit analyst-side task and wants to extract facts. But you should not assume that; stop, ask, and route through the methodology in `docs/15-clean-room-engineering.md`.

## House style for the C++ code

- C++23 throughout. `st::Result<T>` is portable via a feature-detected fallback to `tl::expected` when `<expected>` isn't available (Apple Clang's libc++ historically lagged).
- No exceptions in domain code; exceptions only at UI boundaries
- `snake_case` for functions/variables, `PascalCase` for types, `kPascalCase` for constants
- `clang-format` (LLVM base, 4 spaces, 100 cols, pointer-binds-right) — `clang-format --dry-run --Werror` is a CI gate
- `clang-tidy` and `-Wall -Wextra -Wpedantic -Werror` clean
- Catch2 v3 for tests; tests live next to code in `tests/unit/<module>/`
- No global state; dependency-inject services into the application layer
- See `docs/02-architecture.md` for module boundaries — domain has no ImGui or USB types in its public headers

## Working with this user

- They're working on Windows (Cornelio, win32, `D:\Documents\JetBrains\SubaruTuner`).
- Path separators in messages may use either `/` or `\` — prefer `/` in shell commands (bash shell) and `\` in Windows-path strings to the user.
- The user pushed back on emissions paternalism early. **Treat them as a knowledgeable adult who has read the docs.**

## Status

As of 2026-05-12: Phase 0 done. Phase 1 CLI side done. Phase 2 MVP done (persistence + undo/redo end-to-end). Phase 3 protocol framing complete (SSM read+single-byte-write+block-write, complete UDS catalog including the flashing flow, memory-by-address reads/writes, and bus gating). Phase 3 datalogger pipeline end-to-end hardware-free. Phase 4 flash orchestrator (st::flash) end-to-end hardware-free: read-full-ROM, delta detection, full erase/write/verify per-sector, dry-run, all driven through MockTransport. CAN reverse-engineering toolkit replay path complete end-to-end: `st::can::Frame` + `.asc` I/O, `st::dbc::Database` parser/emitter + `decode_signal`, `st::discover::BaselineModel` + `ChangeDetector` + `.cdb` Bundle I/O, and all five replay CLI subcommands (`can-replay`, `can-diff`, `can-discover`, `can-export-dbc`, `can-decode`). C++ TOML loader resolves `extends` chains. GUI has docking, ImPlot heatmap view, 3D slice picker, welcome panel, chip-based table-header + status-bar. **308 C++ + 42 Python tests** green on MinGW g++ 15.2. Repo at `https://github.com/BuffJesus/SubuwuTuner`. Phase 1 hardware gate (real ROM, ≥20 maps from real definitions) and protocol validation waiting on user's OBDX Pro VX adapter.

Deps wired so far via FetchContent: Catch2 v3 (tests), `tl::expected` (fallback when libc++ lacks `<expected>`), tomlplusplus v3.4 (definition parser), GLFW 3.4 + Dear ImGui v1.91 + ImPlot + nativefiledialog-extended (UI). vcpkg manifest mode still deferred — would only be needed for a system-package dep like OpenSSL when the signed-update channel lands.

CI: clang-format job is advisory (non-blocking) since no pre-commit hook is set up yet. Once one is wired in, flip it back to required.

**Hardware-free work still on the table** (any of these can be picked up next):
- **defgen polish** — ✅ `--apply-to-pack` shipped (additive merge, idempotent on a second run); ✅ non-linear `toexpr` flattening + unknown storage/endian fallbacks now surface as per-pack warnings on stderr. Remaining: actually try the tool end-to-end against a real RomRaider XML.
- **GUI polish stack on top of the ImGui MVP** — ImGui docking branch, ImPlot for charts, nfd for native Open/Save dialogs, a tuned dark theme + Inter/JetBrains Mono fonts, and a first-party 2D/3D map editor widget bound to `st::edit::History`. The domain layer is stable; the UI just needs binding work.
- **Logger / datalogging design + Phase 3 implementation skeleton** — `st::log::LogStream` and the lock-free ring buffer per `docs/13`. Can be designed and unit-tested via `MockTransport`.
- **CAN reverse-engineering toolkit** — programmatic discovery loop for swap/cluster work, fully designed in `docs/14`. All replay-mode pieces shipped: `st::can::Frame` + `.asc` I/O, DBC parser/emitter + decoder, `BaselineModel`/`ChangeDetector`, `.cdb` bundle, and the `can-{replay,diff,discover,export-dbc,decode}` CLI commands. Live-mode `can-discover --live` and `can-record` wait on hardware.
