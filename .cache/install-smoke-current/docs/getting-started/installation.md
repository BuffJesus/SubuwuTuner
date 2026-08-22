# Install

Two install paths depending on what you're trying to do:

- **Use the tool against a ROM you already have.** Build the binaries and
  drop your TOML definition pack somewhere on disk. ~5 minutes.
- **Read, datalog, or flash a real ECU.** Same as above plus a supported
  USB-OBD adapter (OBDX Pro VX recommended). ~10 minutes plus driver setup.

## Prerequisites

| Tool | Purpose | Windows | Linux | macOS |
|------|---------|---------|-------|-------|
| **C++23 compiler** | Build SubuwuTuner | MSVC 19.40+ or MinGW-w64 15+ | GCC 14+ or Clang 18+ | Apple Clang 16+ |
| **CMake 3.28+** + Ninja | Build system | [cmake.org](https://cmake.org/download/) + [ninja-build.org](https://ninja-build.org/) | `apt install cmake ninja-build` | `brew install cmake ninja` |
| **Git** | Clone + submodules | [git-scm.com](https://git-scm.com/) | system git | system git |
| **Python 3.10+** (optional) | `tools/defgen/` definition generation | [python.org](https://www.python.org/) | system python3 | system python3 |

No vcpkg or system packages required — every C++ dependency (Catch2,
GLFW, ImGui, ImPlot, tomlplusplus, tl::expected, nativefiledialog-extended)
pulls via `FetchContent` on first configure.

## 1. Clone

```bash
git clone https://github.com/BuffJesus/SubuwuTuner.git
cd SubuwuTuner
```

## 2. Configure and build

Pick the preset for your platform:

=== "Windows (MinGW)"

    ```bash
    cmake --preset win-mingw
    cmake --build --preset win-mingw
    ```

=== "Windows (MSVC)"

    ```bash
    cmake --preset win-msvc
    cmake --build --preset win-msvc
    ```

=== "Linux (GCC)"

    ```bash
    cmake --preset linux-gcc
    cmake --build --preset linux-gcc
    ```

=== "macOS (Apple Clang)"

    ```bash
    cmake --preset mac-clang
    cmake --build --preset mac-clang
    ```

First configure downloads ~12 dependencies and takes a few minutes.
Subsequent builds are incremental. The binaries land under
`build/<preset>/bin/`.

## 3. Verify

```bash
./build/win-mingw/bin/subuwutuner-cli --version
./build/win-mingw/bin/subuwutuner-cli doctor
```

`doctor` runs the install health check — config-dir presence, write
permissions, definition-pack discoverability, transport availability.
Replace `win-mingw` with whichever preset you built.

If you only see one of the two binaries, the GUI deps probably failed to
download — re-run configure with `--log-level=VERBOSE` and look for the
specific `FetchContent` step that timed out.

## 4. Install a definition pack

SubuwuTuner ships without bundled VA/VB calibration packs by design (see
[`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" }
for the why). You supply the pack at runtime.

**Quickest start** — the repo includes
[`fixtures/demo-pack/`](https://github.com/BuffJesus/SubuwuTuner/tree/main/fixtures/demo-pack){ target="_blank" },
a synthetic always-available pack. Use it to learn the tool with no
hardware and no real-world data:

```bash
subuwutuner-cli pack-info fixtures/demo-pack/
```

**For real tuning** — put TOML packs anywhere on disk and reference them
explicitly via `--def <path>` or via your `.stune` project. Convention
dirs (already used for the GUI's recents and settings):

| OS | Convention dir for definitions |
|---|---|
| Windows | `%APPDATA%\SubuwuTuner\definitions\` |
| Linux | `$XDG_CONFIG_HOME/subuwutuner/definitions/` (default `~/.config/subuwutuner/definitions/`) |
| macOS | `~/Library/Application Support/SubuwuTuner/definitions/` |

### Generating your own pack

`tools/defgen/` converts a public community-schema ECU definition XML
into the SubuwuTuner TOML schema:

```bash
# Single-ROM XML -> single TOML
python tools/defgen/defgen.py path/to/rom.xml -o path/to/out.toml

# Multi-ROM XML -> one TOML per <rom> in a directory
python tools/defgen/defgen.py path/to/multi-rom.xml -o path/to/out-dir/
```

Public sources include the community-maintained RomRaider definition
repository (GPL) and curated mirrors such as `Merp/SubaruDefs`. The
TOML schema is documented at [Definition format](../reference/definition-format.md);
the source-XML acceptance criteria live at
[`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" } §4.

## 5. USB hardware setup (only if reading or flashing a real ECU)

### OBDX Pro VX

Enumerates as a USB-CDC virtual COM port; no driver rebind needed.

```bash
# Windows
subuwutuner-cli rom-info --transport obdx --device COM5

# Linux / macOS
subuwutuner-cli rom-info --transport obdx --device /dev/ttyACM0
```

The COM port number varies by machine — check Device Manager (Windows)
or `ls /dev/ttyACM*` (Linux/macOS) when uncertain.

### COBB AccessPort V3 (Zadig + WinUSB on Windows)

The `subuwutuner-cli ap3` subcommand talks to a married COBB AccessPort
V3 as a file vault. On Windows it requires the **WinUSB** driver:

1. Install [Zadig](https://zadig.akeo.ie) (no installer, single .exe).
2. Plug in the AP.
3. **Options → List All Devices**, pick `1A84 / 0121`.
4. Pick **WinUSB** in the driver dropdown, click **Replace Driver**.
5. Verify: `subuwutuner-cli ap3 state` should print the AP serial,
   firmware version, and ROM MD5.

On Linux, install a udev rule instead:

```
# /etc/udev/rules.d/99-cobb-accessport.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a84", ATTRS{idProduct}=="0121", MODE="0666"
```

```bash
sudo udevadm control --reload && sudo udevadm trigger
```

On macOS libusb claims the AP directly without rebinding — just plug in
and run `subuwutuner-cli ap3 state`.

!!! warning "AP marriage state is load-bearing"
    SubuwuTuner refuses mutating operations on an AP whose UserInfo
    reports `Not Installed`. Bypass with `--allow-unpaired-vehicle` if you
    know what you're doing. Marriage state is read-only — SubuwuTuner
    never touches it.

## Troubleshooting

**"`bulk_transfer OUT failed: LIBUSB_ERROR_TIMEOUT`"** on AP3 — the AP
firmware is most likely dazed by a prior malformed packet from another
tool. Unplug + replug is the only known recovery. SubuwuTuner's codec
layer is pinned to the spec-correct envelope shapes, but if a
half-written third-party tool ran first the daze persists across
processes. See
[`docs/install.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/install.md#troubleshooting-ap3-connections){ target="_blank" }
for the longer triage list.

**"`/maps/...` mangled to `C:/Program Files/Git/maps`"** on Git Bash —
set `MSYS_NO_PATHCONV=1` for the invocation, or run from PowerShell. The
MSYS path-conversion layer rewrites anything that looks like an absolute
Unix path; the AP firmware then rejects the mangled string.

**"No definition pack found"** — the CLI saw a path that doesn't exist.
Check the path you passed; the pack file is somewhere else, or was never
put there.

**"Pack loads but ROM doesn't identify"** — the pack's
`[[identification]]` records didn't match any byte sequence in the ROM.
Confirm you have the right pack for your CID:

```bash
subuwutuner-cli rom-info --def <pack> path/to/rom.bin
```

`doctor` is also worth running:

```bash
subuwutuner-cli doctor
```

## Next

→ [Quickstart](quickstart.md) — full editing loop in 5 minutes against
the synthetic demo project, no hardware needed.
