# SubuwuTuner

> A comprehensive, free, open-source **Subaru ECU tuning suite** written from scratch in modern C++23.

**Status:** pre-1.0, active development. Read / edit / datalog / flash pipelines run end-to-end on a hardware-free path; the bench rig is up (Phase 5 stop-point cleared 2026-06-15). Remaining v1.0 ship-gates are brick-protection HIL coverage and the patch-insertion layer — see [`docs/04-roadmap.md`](docs/04-roadmap.md) for the current cut.

## Why this exists

Subaru tuning has historically meant picking between uncomfortable options: a sluggish Java GUI built around a decade-old definition format, a polished but locked-down commercial app, or hand-editing hex in IDA. Each makes different tradeoffs; none of them is *natively fast, scriptable, open, and friendly to community-curated definitions all at once.*

SubuwuTuner is the result of asking: **what would a tuning tool look like if it were designed today, from first principles, with the same care a modern compiler or IDE puts into responsiveness and ergonomics?** Native C++23 throughout. Sub-second startup. ~10 MB portable bundle. A CLI that does everything the GUI does, because tuning shops, dyno operators, and CI pipelines are first-class users. Definitions you can `git diff`, `git blame`, and pull-request. Engine-safety rules baked in by default; emissions policy left to the user's jurisdiction.

It targets 2015–2021 (VA) and 2022+ (VB) WRX manual transmission at v1.0 because that's what the bench rig can credibly brick-test. v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup as definition packs land and flash routines clear bench testing.

## What you can do with it

**Edit a calibration.** Open a `.stune` project. The sidebar is a hierarchical 9-group view of every map the definition pack knows about — fuel, ignition, boost, throttle, AVCS, transmission, idle, etc. Click a table; it opens in a 2D/3D editor with axis labels, scaling, units, glossary hovers, and policy badges (S = safety-critical, E = emissions-flagged). Click a cell, type a value, hit Enter. The history panel records the edit with timestamps and tags; Ctrl+Z walks it back. Ctrl+S persists everything to `edits.toml` inside the `.stune` directory — a TOML file you can diff, commit, and code-review.

**Datalog a session.** Plug in a supported adapter, pick PIDs, hit record. Logs land as CSV via `st::log::CsvSink` for grep-friendly downstream tooling. Sustained high-rate logging across 20+ PIDs is the baseline target. The live gauge-cluster panel paints ImPlot mini-lines while you're recording; CSV-replay panels work the same way after the fact.

**Auto-tune from your logs.** Feed the MAF kernel a CSV from a real drive; it proposes scaling adjustments cell-by-cell with engine-safety linting on by default (won't recommend a fuel cell into the danger zone, won't propose ignition advance past a known-knock-prone region). The knock-pull kernel watches feedback knock retard and proposes ignition pulls where the ECU keeps pulling timing. Both surface as previewable, undoable `edit::History` operations — nothing touches your project until you accept it.

**Design a custom feature.** The `.stmod` node-graph designer lets you sketch a feature like "when clutch is engaged AND throttle > 80% AND RPM > 4500, override the boost target table." Drag nodes, wire them up, hit compile. The IR backend emits **SH-2A or RH850 patch bytes** — actual machine code, with FPU bridging on SH-2A and mask-merge select on RH850, 22 primitives at parity across both architectures. Sample graphs ship in `fixtures/samples/`.

**Reflash with brick protection.** The flash orchestrator is a multi-stage pipeline: pre-write backup (mandatory, CRC-gated, refuses to proceed without a verified backup), per-ISA delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based power-loss resume, audit log. The recovery model is per-ISA — SH-2A uses serial-boot recovery, RH850 uses dual-bank atomic-swap. If power drops mid-flash, the journal lets the next attempt pick up exactly where the last one left off. (The orchestrator runs end-to-end against `MockTransport` today; live-ECU validation is the remaining v1.0 ship gate.)

**Script the whole thing.** Everything in the GUI has a `subuwutuner-cli` equivalent. 70+ subcommands. JSON emitters on the structured ones (`pack-info --json`, `workflow-list --json`, `knock-snapshot --json`) for piping into other tools or CI gates.

**Reverse-engineer CAN traffic.** Five `can-*` subcommands for replay-path RE: ingest an `.asc` log, score against a `BaselineModel`, surface change-detector signals, draft a DBC. Bundles into `.cdb` for sharing.

## Highlights

- **Native and fast** — sub-second startup, ~150 MB idle RAM, **~10 MB portable / ~8 MB installer** (Windows).
- **Headless-first** — same domain core drives the GUI and `subuwutuner-cli`; **70+ CLI subcommands** with JSON emitters on the structured ones.
- **Git-friendly definitions** — TOML schema, diffable, pull-requestable; `extends` chains let third-party packs override one field at a time. `pack-info` + `pack-lint` keep regressions out of CI.
- **Open auto-tune** — MAF + knock-pull kernels shipped first-party with engine-safety linting on by default. See [`docs/12-auto-tuning.md`](docs/12-auto-tuning.md).
- **Custom features** — node-graph designer (`.stmod` files) that compiles to SH-2A *or* RH850 patch bytes at 22-primitive parity. See [`docs/16-custom-features.md`](docs/16-custom-features.md).
- **Real brick protection** — per-ISA recovery model (SH-2A serial-boot, RH850 atomic-swap), mandatory pre-write full-ROM backup with CRC verify gate, delta-only writes, journal-based power-loss resume. `st::policy::FlashPreflight` runs `EcuIdMatch` / `VinMatch` / `BatteryVoltageOk` / `IgnitionState` / `ChecksumKnown` / `BackupStorePresent` before any byte goes on the wire. See [`docs/31-brick-protection-by-isa.md`](docs/31-brick-protection-by-isa.md) and [`docs/40-delta-flash-brick-protection.md`](docs/40-delta-flash-brick-protection.md).
- **Tuning-knowledge atlas** — consolidated machine-readable tuning facts wired into the table editor, with a sortable tune-library panel for your local catalog. See [`docs/39-tuning-knowledge-atlas.md`](docs/39-tuning-knowledge-atlas.md).
- **Workflow modals + transactional history** — pack-declared `[[workflow]]` registry drives guided multi-table operations (e.g. FA20 → FA24 swap). Each batch records under a single transaction tag so the entire workflow undoes in one action from the status-bar badge.
- **CAN reverse-engineering toolkit** — replay, DBC decode, baseline / change-detector, `.cdb` bundle. Five `subuwutuner-cli can-*` subcommands. See [`docs/14-can-reverse-engineering.md`](docs/14-can-reverse-engineering.md).
- **Jurisdiction-aware, not paternalistic** — first-run profile picker decides whether emissions-flagged edits warn, confirm, or stay silent. Engine-safety warnings stay strict regardless. See [`docs/06-legal-ethics.md`](docs/06-legal-ethics.md).
- **Tamper-evident audit** — `st::audit` is a CRC32-protected append-only log wired across UDS + Flasher; every reflash leaves a record. Pin/star sidecar in the GUI, NDJSON export with pinned-only scope.
- **Cross-platform on day one** — Windows, macOS (Intel + Apple Silicon), Linux (x64 + arm64). The asymmetry is in flashing transports; everything else is identical. See [Platform feature matrix](#platform-feature-matrix) below.

## Philosophy

A few things that are decided, not negotiable:

- **The user is in charge of their own car.** If you ask to edit something the tool considers risky, it warns once and lets you proceed. It does not lock you out. The only hard refusals are on engine-safety grounds (delete a knock sensor's threshold table and you get a refusal — that's a fast path to a broken engine regardless of jurisdiction).
- **No cloud, no telemetry, no always-online dependency.** SubuwuTuner does not phone home. Your tunes live on your disk. Auto-update fetches signed manifests when you ask it to, not on a schedule (see [`docs/22-auto-update.md`](docs/22-auto-update.md)).
- **Destructive operations get engineering-grade safety.** Reflash is the operation that can brick an ECU. It gets the most engineering. Pre-write backups are mandatory and CRC-verified. The flash orchestrator refuses to run without a verified backup. Mutation tests on `st::flash` are a release gate.
- **Clean-room throughout.** SubuwuTuner is original work, not a port. Protocol facts come from public standards (ISO 14229 UDS, ISO 15765 CAN-TP, SAE J2534, J1979 OBD-II) and from observing Subaru's own behavior on the wire. Expression-level borrowing from any other tuning tool, open or commercial, is a non-starter. See [`docs/15-clean-room-engineering.md`](docs/15-clean-room-engineering.md) for the methodology — including how the AI assistants in the loop respect the same boundary.

## What's NOT in scope

- **Non-Subaru OEMs.** Different OEM = different project. SubuwuTuner is opinionated about doing one thing well.
- **Bundled calibration definitions.** The infrastructure ships; definition packs are user-supplied. Reasoning lives in [`docs/17-data-distribution-policy.md`](docs/17-data-distribution-policy.md). The bundled `fixtures/demo-pack/` is synthetic and always-available so the tool is exercisable from a fresh clone.
- **First-party defeat-preset calibrations.** Performance edits are the user's call. The tool will not ship "delete cat" buttons as out-of-the-box content.
- **A SavvyCAN replacement.** The CAN toolkit is scriptable-discovery-focused, not a full visual signal-graph workstation.
- **Hiding what the tool is doing.** No silent edits, no obscured journal, no closed flash sequence. The audit log + manifest pair shows exactly what touched your ROM and when.

## New here?

- **[`docs/getting-started.md`](docs/getting-started.md)** — 5-minute path from `git clone` to your first tune edit (build → demo project → first edit). **Start here.**
- **[`docs/install.md`](docs/install.md)** — installing definition packs into the per-platform user-data directory
- **[`docs/00-overview.md`](docs/00-overview.md)** — design rationale; what we're building and why
- **[`docs/README.md`](docs/README.md)** — full design-doc index (architecture, protocols, flash safety, IP boundaries, distribution policy)
- **[`CHANGELOG.md`](CHANGELOG.md)** — running log of substantive changes
- **[`CONTRIBUTING.md`](CONTRIBUTING.md)** — clean-room methodology, code style, PR flow
- **[`SECURITY.md`](SECURITY.md)** — responsible-disclosure policy and scope

## Building

Requirements:

- C++23 compiler (MSVC 19.40+ / Apple Clang 16+ / GCC 14+ / MinGW-w64 15+)
- CMake 3.28+
- Ninja (recommended)
- Git

```bash
cmake --preset linux-gcc      # or win-mingw, win-msvc, mac-clang
cmake --build --preset linux-gcc
ctest --preset linux-gcc
./build/linux-gcc/bin/subuwutuner-cli --version
```

See [`CMakePresets.json`](CMakePresets.json) for the full preset list. No vcpkg or system packages — every dependency pulls via `FetchContent` on first configure (Catch2, `tl::expected` fallback, tomlplusplus, GLFW, Dear ImGui, ImPlot, nativefiledialog-extended). First configure takes a couple of minutes; subsequent builds are incremental.

The CI matrix runs **~1700 C++ tests + 36 Python tests** across Win MSVC, macOS Apple Clang, Linux GCC, and Linux Clang ASan+UBSan, plus the `defgen` freeze matrix (PyInstaller single-file binary per OS).

Optional build flags (off by default) live in [`docs/07-build-and-tooling.md`](docs/07-build-and-tooling.md).

### Contributors

Opt into the clang-format pre-commit hook so your changes match CI before pushing:

```bash
git config core.hooksPath .githooks
```

The hook runs `clang-format --dry-run --Werror` over staged C/C++ files and refuses commits that would change. Bypass once with `git commit --no-verify` for WIP commits you intend to fix up. Setup, override, and install-clang-format instructions live in [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`docs/07-build-and-tooling.md`](docs/07-build-and-tooling.md) §Pre-commit hook.

## Definition packs

SubuwuTuner ships the *infrastructure* — loader, table editor, project model, flash orchestrator, auto-tune kernels, custom-features designer, CAN toolkit, GUI — without bundled calibration definitions. Drop a TOML pack into the per-platform user-data directory (see [`docs/install.md`](docs/install.md)), or generate one from a public ECU definition XML with [`tools/defgen/`](tools/defgen/). The repo includes [`fixtures/demo-pack/`](fixtures/demo-pack/) as a synthetic always-available example so the tool is exercisable from a fresh checkout — no real definition pack required to see how the GUI feels.

A definition pack is a TOML file (or directory of them) declaring every table the ECU exposes: address, dimensions, axis layout, scaling, units, glossary text, policy flags, optional `[[workflow]]` registry entries. Inheritance via `extends` lets a child pack add or override one field at a time without forking. `pack-info` + `pack-lint` make pack hygiene a one-liner in CI.

The reasoning behind this distribution choice lives in [`docs/17-data-distribution-policy.md`](docs/17-data-distribution-policy.md).

## Platform feature matrix

The GUI, CLI, project model, auto-tune, custom-features designer, tune library, and the CAN replay / decode pipeline are platform-symmetric. The asymmetry is in **adapter / flashing transports**, because J2534 v04.04 is a Windows-DLL API.

| Capability | Windows | macOS | Linux |
|---|---|---|---|
| GUI, CLI, project work, ROM viewer / editor | ✅ | ✅ | ✅ |
| TOML pack loader + `defgen` pipeline | ✅ | ✅ | ✅ |
| Auto-tune kernels (MAF + knock-pull) | ✅ | ✅ | ✅ |
| Custom-features designer (SH-2A + RH850 codegen) | ✅ | ✅ | ✅ |
| Tune library + tuning-knowledge atlas | ✅ | ✅ | ✅ |
| CAN replay / DBC decode / `.cdb` discovery | ✅ | ✅ | ✅ |
| Datalogging via OBDX Pro VX (USB-CDC) | ✅ | ✅ | ✅ |
| Datalogging via native handheld (USB-CDC) | ✅ | ✅ | ✅ |
| Flashing via OBDX Pro VX / native handheld | ✅ | ✅ | ✅ |
| Flashing via **J2534** (Tactrix OpenPort 2.0, Tactrix Pro J) | ✅ | ❌ Windows-only DLL | ❌ Windows-only DLL |
| Live CAN bus capture (`can-record --live`) | ✅ | ✅ | ✅ |

On macOS and Linux, **flashing requires an OBDX Pro VX or the SubuwuTuner native handheld** (per [`docs/13-transport.md`](docs/13-transport.md)). J2534 support on those platforms is not on the roadmap — the realistic substitute is the OBDX-VX / native path, which is what SubuwuTuner has put its transport-layer engineering behind. If you only own a Tactrix OpenPort 2.0, you flash from Windows.

## Safety

ECU tuning can damage engines and brick ECUs. **Read [`DISCLAIMER.md`](DISCLAIMER.md) before connecting this software to a real vehicle.**

## License

[Apache 2.0](LICENSE). See [`docs/06-legal-ethics.md`](docs/06-legal-ethics.md) for the project's stance on emissions, IP, and distribution, [`docs/15-clean-room-engineering.md`](docs/15-clean-room-engineering.md) for the clean-room methodology, and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for bundled-library notices.
