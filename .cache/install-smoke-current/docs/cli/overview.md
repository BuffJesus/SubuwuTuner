# CLI overview

`subuwutuner-cli` is the headless front-end. Every domain capability is
reachable from here; `subuwutuner-gui` is a strict superset only on the
visualization axis.

```bash
subuwutuner-cli --help
subuwutuner-cli --version
subuwutuner-cli doctor
```

## Subcommand groups

### ROM and definition

| Subcommand | What it does |
|---|---|
| `rom-info` | Identify ROM, print CID / family / metadata |
| `rom-pull` | Read ROM off a live ECU via a transport |
| `rom-diff` | Per-cell diff between two ROMs through a definition |
| `dump-table` | Render a table to stdout (text or JSON) |
| `dump-axis` | Render an axis (RPM, MAP, etc.) |
| `pack-info` | Definition-pack summary |
| `pack-lint` | Schema hygiene + cross-reference checks |

### Project

| Subcommand | What it does |
|---|---|
| `project-new` | Create a `.stune` directory from a ROM + pack |
| `project-info` | Project summary |
| `project-validate` | Replay edits + pack hygiene |
| `project-clone` | Branch a project |
| `project-flash` | End-to-end flash flow (lives against MockTransport today; live-bench gated) |
| `table-edit` | Apply a single cell edit through `st::edit::History` |

### Logging

| Subcommand | What it does |
|---|---|
| `log` | Datalog from a live ECU through a transport |
| `knock-snapshot` | Knock-event snapshot (`--json` / `--csv`) |
| `coldstart-analyze` | Cold-start datalog analysis |

### Flash and integrity

| Subcommand | What it does |
|---|---|
| `flash` | Lower-level flash orchestrator |
| `checksum-verify` | Boot-integrity verification on a candidate ROM |
| `checksum-repair` | COBB-style AP CRC slot-table repair (AP metadata only) |

### Custom features

| Subcommand | What it does |
|---|---|
| `feature-compile` | Compile a `.stmod` node graph to a ROM patch (SH-2A or RH850) |
| `feature-flash` | Insert a `.stmod` into a source ROM and emit a policy-gated `FlashPlan` |

### CAN reverse-engineering

| Subcommand | What it does |
|---|---|
| `can-replay` | Replay a `.asc` or `.cdb` bundle |
| `can-diff` | Diff two captures with `BaselineModel` + `ChangeDetector` |
| `can-discover` | Programmatic signal discovery |
| `can-decode` | DBC decode |
| `can-export-dbc` | Emit a DBC from a `.cdb` bundle |

### Transport diagnostics

| Subcommand | What it does |
|---|---|
| `transport-list` | Enumerate connected adapters |
| `doctor` | Install + transport + pack health check |

### Subaru-specific protocol probes

| Subcommand | What it does |
|---|---|
| `ssm-a8-poll` | OEM SSM-0xA8 RAM polling over ISO-15765 |
| `subaru-uds-send-raw` | Raw UDS request — low-level diagnostic |
| `subaru-dsc-unblock-sequence` | DSC 0x10 0x02 + Phase D + flash chain (bench / advanced) |
| `subaru-live-log` | Live signal capture from the LF79103P signal catalog |

### COBB AccessPort V3 (file vault)

| Subcommand | What it does |
|---|---|
| `ap3 state` | AP serial, firmware version, current ROM MD5 |
| `ap3 ls <path>` | List a vault path (`/maps/`, `/presets/`, etc.) |
| `ap3 pull <vault-path> <local>` | Read a file out of the vault |
| `ap3 push <local> <vault-path>` | Write into the vault (gated by AP marriage) |
| `ap3 rm <vault-path>` | Remove from the vault |
| `ap3 backup` | Full vault backup |
| `ap3 raw` | Send a raw envelope (advanced) |
| `ets {state,ls,pull,push,rm,backup,raw}` | Same surface, AccessPort Manager-side namespace |

### `.ptm` tune files

| Subcommand | What it does |
|---|---|
| `ptm import` | Decrypt a `.ptm` against a base ROM, populate `edits.toml` |
| `ptm export` | Build a `.ptm` from a project |
| `ptm verify` | Round-trip integrity check |

### Audit

| Subcommand | What it does |
|---|---|
| `audit stats <log>` | Summary of events in an audit log |
| `audit export <log>` | NDJSON export (with `--pinned-only` scope) |

### AI

| Subcommand | What it does |
|---|---|
| `ai-drift` | Tier 1 rules-based drift classifier |

### Tuner Atlas

| Subcommand | What it does |
|---|---|
| `tuner-atlas` | Query the consolidated tuning-knowledge atlas |

## Common flags

| Flag | Purpose |
|---|---|
| `--def <path>` | Path to a definition pack (file or directory) |
| `--project <dir>` | Path to a `.stune` project directory |
| `--transport {obdx,j2534,handheld,mock}` | Transport selector |
| `--device <name>` | Device identifier (`COM5`, `/dev/ttyACM0`, etc.) |
| `--sa-variant <name>` | SecurityAccess variant ([see SA concepts](../concepts/security-access.md)) |
| `--authenticate` | Run SA prelude before the requested operation |
| `--enable-bulk-reflash-cipher` | Arm the gated 0xB6 bulk-transfer write path |
| `--allow-unpaired-vehicle` | Bypass the AP marriage gate for `ets *` subcommands |
| `--json` | Emit structured JSON instead of human text |

## Useful environment variables

| Variable | Purpose |
|---|---|
| `ST_PTM_BASE_ROM_DIR` | Auto-discovery dir for `ptm import` base ROMs (looks for `<vehicle_id>.bin`) |
| `ST_AP3_TRACE_USB` | Dump every USB OUT/IN payload to stderr as 16-byte rows |
| `ST_AP3_READFILE_DRAIN_MODE` | Workaround for the cmd 0x21 large-file truncation issue |
| `STT_AP3_LIVE_TEST` | Enable the opt-in `[.live][ap3]` integration test |
| `MSYS_NO_PATHCONV` | Disable Git Bash path conversion (required for `/maps/`-style args) |

## In-progress pages

Per-subcommand pages — flag tables, example invocations, exit codes — are
being written. Until they land, `subuwutuner-cli <subcommand> --help` is
authoritative.
