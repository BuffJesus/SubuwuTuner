# Installing definition packs

SubuwuTuner ships without bundled VA/VB calibration packs (see [`17-data-distribution-policy.md`](17-data-distribution-policy.md) for why). This guide covers where to put your own packs and how to generate them.

## TL;DR

- **Quickest start:** the repo includes [`fixtures/demo-pack/`](../fixtures/demo-pack/), a synthetic always-available example. Use it to learn the tool without any setup.
- **For real tuning:** put TOML packs anywhere on disk and pass the path explicitly to `subuwutuner-cli --def <path>` or via your `.stune` project's pack reference.
- **Recommended convention:** keep your packs in the per-platform user-data directory below for organization (the GUI's recents and settings already live there).

## Where SubuwuTuner looks (today vs. tomorrow)

**Today** — the CLI requires an explicit `--def <path>`. The GUI loads whatever pack a `.stune` project points at. There is no auto-discovery from a fixed location yet.

**Tomorrow** — the welcome panel will point at a per-platform convention directory and offer one-click access to packs found there. The convention dirs (already used by the GUI for `recents.txt` and `settings.txt`):

| OS | Convention dir for definitions |
|---|---|
| Windows | `%APPDATA%\SubuwuTuner\definitions\` |
| Linux | `$XDG_CONFIG_HOME/subuwutuner/definitions/` (default: `~/.config/subuwutuner/definitions/`) |
| macOS | `~/Library/Application Support/SubuwuTuner/definitions/` |

Even though auto-discovery isn't wired up, putting packs there now is forward-compatible.

## Installing a pack you already have

If someone hands you a TOML pack (or a directory containing `pack.toml` plus sibling TOML fragments):

1. Drop the pack into the convention dir for your OS, or any path you prefer.
2. Reference it explicitly:

```bash
# CLI: pass the pack path (file or directory)
subuwutuner-cli pack-info path/to/your-pack.toml
subuwutuner-cli dump-table --def path/to/your-pack-dir/ --table fuel_main path/to/rom.bin

# Project: reference it once at create time, then it's persisted
subuwutuner-cli project-new --source path/to/rom.bin \
                            --def path/to/your-pack-dir/ \
                            my-tune.stune
```

The pack format is documented in [`11-definition-format.md`](11-definition-format.md).

## Generating your own pack from a public community XML

`tools/defgen/` converts a community-schema XML definition to SubuwuTuner's TOML schema:

```bash
# Single-ROM XML → single TOML
python tools/defgen/defgen.py path/to/rom.xml -o path/to/out.toml

# Multi-ROM XML → one TOML per <rom> in a directory
python tools/defgen/defgen.py path/to/multi-rom.xml -o path/to/out-dir/
```

Public sources:

- The community-maintained upstream XML repository (open-source GPL project; URLs in `docs/15`)
- Curated-pack mirrors (e.g., `Merp/SubaruDefs` on GitHub — `ecu/metric/ecu_defs.xml` covers most older Subarus)

Anything traceable to commercial-tool circumvention is **out of scope** for this repo as first-party content; see [`17-data-distribution-policy.md`](17-data-distribution-policy.md) §4 for the acceptance criteria.

## Generating from your own ROM dump

Planned: a hardware-capture workflow that builds a starter pack from a ROM you have read off your own car. This requires the OBDX Pro VX adapter (or equivalent) and is gated on the Phase 4 bench rig per [`04-roadmap.md`](04-roadmap.md). Until then, the defgen path above is the supported workflow.

## SSM datalogger fragments

The `definitions/pids.toml` (91 SSM PIDs + 68 switches) and `definitions/ecuparams/` (per-CID extended PIDs) are community-sourced from the public `logger.xml` shape and ship with the public repo. Your VA/VB pack will need to declare them as fragments via `[pack].includes`:

```toml
# in your pack.toml
[pack]
id       = "my-va-wrx-2019"
includes = ["../ecuparams/<your_cid>.toml", "../pids.toml"]
```

VA/VB packs use UDS rather than SSM and typically only need an ecuparams fragment, not `pids.toml`. See [`definitions/README.md`](../definitions/README.md) for the include conventions.

## Verifying a pack loaded correctly

```bash
subuwutuner-cli pack-info path/to/your-pack-dir/
```

Expected output: schema version, pack id, table count, axis count, scaling count, and any include resolutions. If the path is wrong or the pack is missing, the CLI emits a Path-B context hint pointing at this doc.

## Troubleshooting

**"No definition pack found at the path above."** — the CLI saw a path that doesn't exist. Either the pack file is somewhere else, or it was never put there. Check the path you passed.

**Pack loads but ROM doesn't identify.** — the pack's `[[identification]]` records didn't match any byte sequence in the ROM. Confirm you have the right pack for your CID (`subuwutuner-cli rom-info --def <pack> path/to/rom.bin` shows the matched CID, if any).

**Parse error on load.** — the TOML is malformed. If you generated with `defgen`, file an issue with the source XML attached and the defgen command line you used.

## USB hardware setup (optional)

### COBB AccessPort V3 — Zadig + WinUSB on Windows

SubuwuTuner's `subuwutuner-cli ap3` subcommand talks to a COBB AccessPort V3 over libusb. On Windows the AP enumerates as a vendor-specific device that libusb can only claim through the **WinUSB** driver. The OEM tooling (APManager) binds the AP to a different driver, so the rebind step below is required before SubuwuTuner can see the device.

1. Download **Zadig** from [https://zadig.akeo.ie](https://zadig.akeo.ie). It's a small single-file tool, no installer.
2. Plug in the AccessPort.
3. In Zadig: **Options → List All Devices**, then pick the device whose USB IDs read `1A84 / 0121`.
4. In the "Driver" picker, select **WinUSB**. Click **Replace Driver**.
5. Verify: `subuwutuner-cli ap3 state` should now print the AP serial, firmware version, and current ROM MD5 from `/backupcksum`.

Caveats:

- Replacing the driver does NOT touch the AP's at-rest state. The AP stays married to whatever vehicle it's installed on; SubuwuTuner does not write to that state (per `docs/34-cobb-ap-as-tune-vault.md` "What this does not do").
- The OEM APManager will no longer find the AP after the WinUSB switch. To get APManager back, use Zadig again and re-bind to the OEM driver. The two are mutually exclusive at any given moment.
- If you ever rebind the AP and want SubuwuTuner to "see" it again, re-run Zadig.

### Linux udev rule

On Linux the AP needs a udev rule so libusb can claim interface 0 without root:

```
# /etc/udev/rules.d/99-cobb-accessport.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a84", ATTRS{idProduct}=="0121", MODE="0666"
```

Reload with `sudo udevadm control --reload && sudo udevadm trigger`, replug the AP, and `subuwutuner-cli ap3 state` should work.

### macOS

libusb claims the AP directly without driver-rebind. `subuwutuner-cli ap3 state` should work out of the box once you've granted Terminal (or whichever shell) the **Input Monitoring** permission System Settings prompts for.

### OBDX Pro VX

The OBDX Pro VX enumerates as a USB-CDC virtual COM port and does not require Zadig; SubuwuTuner reaches it through the regular serial channel (`--transport obdx --device COM5` on Windows; `/dev/ttyACM0` etc. on Linux/macOS). See `docs/13-transport.md` for the transport architecture.

### Verifying the `ap3` integration end-to-end (developer-side smoke)

When the AP is plugged in, rebound to WinUSB via Zadig (Windows) or with the udev rule installed (Linux), an opt-in live-hardware smoke test exercises the full Capability-A surface in one shot. The test pushes a 50 KB synthetic file to a clearly-named scratch path (`/maps/_st_smoke_<unix_ts>.bin`), reads it back, asserts byte-identical, and removes it. Off by default in CI — the env var gates it.

```bash
# Windows (PowerShell):
$env:STT_AP3_LIVE_TEST = "1"
build/win-mingw/bin/st_unit_tests.exe "[.live][ap3]"

# Linux / macOS:
STT_AP3_LIVE_TEST=1 ./build/.../st_unit_tests "[.live][ap3]"
```

Three test cases run: `query_state` returns non-empty response bodies, `ls /maps/` decodes to a non-empty file list with printable names, and the push → read → byte-identical verify → remove round-trip. The synthetic filename's underscore prefix is a deliberate "this is a SubuwuTuner probe, safe to delete" signal in case the test fails mid-run before the remove fires.

### Troubleshooting `ap3` connections

**"bulk_transfer OUT failed: LIBUSB_ERROR_TIMEOUT"** — libusb successfully opened and claimed the AP but the device didn't accept the first packet within 5 seconds. The usual causes, in order of likelihood:

1. **The AP firmware is dazed by a prior malformed packet.** Per protocol spec §4.2, the AP's USB state machine permanently wedges when it receives a syntactically-valid envelope (sync + wire_len + CRC) wrapped around a malformed body — typically u32-LE string lengths where uleb128 was required. Once dazed, **every** subsequent bulk OUT hangs, even body-less probes like `ap3 state`. `libusb_clear_halt` and `libusb_reset_device` do not unstick the firmware. **The only known recovery is to unplug the AP and plug it back in.** SubuwuTuner's codec layer is unit-test-pinned to use uleb128 (`[ap3]` test suite, spec §4.2 anti-patterns), so if you're hitting the daze, the trigger is probably a different tool (a half-written third-party Python script, an old APManager build, etc.) that ran against the AP before SubuwuTuner did.
2. **APManager is still running.** Quit it. WinUSB can co-enumerate but the OEM driver tends to hold a pipe open that prevents bulk OUT from completing. (Even when the AP is rebound to WinUSB, the OEM agent service can still own the bus exclusively.)
3. **The AP was rebound to WinUSB but never re-plugged.** Windows caches the previous driver association across the same enumeration. Unplug + replug after running Zadig.
4. **A different process has the AP's bulk OUT pipe.** Check with `usbtree.exe` (Microsoft) which process owns the device handle.
5. **The AP is in a transient post-flash state.** If you just used APManager to flash a tune, give the AP 10 seconds to settle before re-running `ap3 state`.

**"no device matched VID 0x1A84 PID 0x0121"** — Zadig hasn't rebound the device to WinUSB, or the AP isn't plugged in. Re-run Zadig: **Options → List All Devices**, select the `1A84 / 0121` entry, pick WinUSB, **Replace Driver**.

**"claim_interface failed: LIBUSB_ERROR_ACCESS"** — on Linux, the udev rule above isn't installed or didn't reload. `sudo udevadm control --reload && sudo udevadm trigger`, then unplug + replug.

**Capturing the USB wire bytes** — set `ST_AP3_TRACE_USB=1` before running any `subuwutuner-cli ap3 …` subcommand and the libusb layer will hexdump every OUT and IN payload to stderr. The format is one row per 16 bytes prefixed with the byte offset, suitable for direct `diff` against the canonical request fixtures under `specs/fixtures/ap3/cmd*_*_request_known_good.bin`. Off by default; redirect stderr to a file if the trace is verbose.

**`/maps/`, `/presets/`, `/datalog/` paths get mangled into `C:/Program Files/Git/maps`** — on Windows under Git Bash / MSYS2, the bash layer auto-converts any argument that looks like an absolute Unix path. The AP firmware sees the mangled string, rejects it, and the CLI surfaces an error like `device returned error frame body=3338` (ASCII "38" = cmd 0x26). Set `MSYS_NO_PATHCONV=1` for the failing invocation, or run from PowerShell where the conversion doesn't apply.
