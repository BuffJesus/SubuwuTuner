# Optional capability: COBB AccessPort V3 as a tune vault

## Scope

This document describes an optional integration. The file-vault subset (Capability A) is **on by default**; the cipher subset (introspecting or editing `.ptm` patches) is **off by default**. The information below is for the project's own contributors; it is not a recommendation to enable the cipher subset.

## What gets enabled

SubuwuTuner can act as a host-side client for the COBB AccessPort V3 (AP3) over USB. The integration has three capability tiers:

**Capability A — File vault (always on).** SubuwuTuner can list, read, write, and remove files on the AP's user filesystem (`/maps/*.ptm`, `/datalog/*.csv.gz`, `/presets/*.cfg`, `/settings`, `/backupcksum`, `/images/*`). It treats `.ptm` files as opaque blobs. No cipher source is involved. Available unconditionally in the default public build.

**Capability B — ROM-pull through AP (open question).** The AP exposes a `/backupcksum` (MD5 of the currently-flashed ROM) and potentially a ROM-data-pull command (`cmd 0x12`, not yet characterized). Out of scope for this document; tracked under `specs/cobb-ap3-implementation-checklist.md` as a future-tier item.

**Capability C — OBD flash bridge via AP (post-1.0).** The AP3 can act as a J2534-equivalent bridge to the vehicle's OBD-II port via its `cmd 0x2f` passthrough path. Not addressed in v1.0; out of scope for this document.

This document covers Capability A and the **gated cipher source** that enables `.ptm` patch introspection on top of Capability A. The gated cipher is required only if a user wants to (a) read what changes a tune file makes vs the stock ROM, (b) edit a tune file's patches, or (c) repackage an edited tune into an AP-loadable `.ptm`. For pure file-vault use (back up the AP, stage tunes from disk, audit which tunes are present), the cipher is not needed.

## Two-step arming (cipher subset only)

When the CMake option `ST_ENABLE_COBB_AP_CIPHER` is `ON`, the build includes the 4-layer `.ptm` cipher chain (XTEA-CBC, base64 envelope, AES-256-CTR, bzip2-L4) and the Blowfish-CTR-double cipher used by COBB's OTA `.img` firmware archives. The cipher key constants live alongside the cipher source in the same gated translation unit.

When the option is `OFF` (the default), the cipher source is excluded from the build and any code path that would decrypt or encrypt a `.ptm` refuses with `PolicyDenied`. The file-vault Capability A continues to function — it does not depend on the cipher.

**Layer 2 — build:** `-DST_ENABLE_COBB_AP_CIPHER=ON` at configure time.

**Layer 1 — runtime:** `--enable-cobb-ap-cipher` on the CLI invocation. Process-scoped; no persistent state across invocations.

Both layers are required for any cipher operation. Either alone yields `PolicyDenied`.

The file-vault Capability A is not gated. It runs in the default build with no flags.

## Failure modes

| Build flag | Runtime flag | Operation | Behaviour |
|---|---|---|---|
| (any) | (any) | `ap3 ls`, `ap3 pull`, `ap3 push`, `ap3 rm`, `ap3 state`, `ap3 backup` | available, runs normally |
| OFF | (any) | `ap3 inspect <file.ptm>`, `ap3 patch-list <file.ptm>`, any `.ptm` edit | `PolicyDenied`. The CLI also notes when `--enable-cobb-ap-cipher` is passed to a flag-OFF build. |
| ON | omitted | same cipher operations | `PolicyDenied`. |
| ON | passed | same cipher operations | proceeds. |

## Why gated

The cipher key constants are **facts** recovered from freely-distributed COBB binaries (APManager.exe and the AP firmware OTA `.img` archives, both served by COBB over plain HTTP). On the copyright axis they are not authored expression, and on the §1201 axis recovering them required no circumvention of an access control mechanism — they sit in plaintext in product rodata.

That makes them facts the project may legitimately distribute. **The choice not to distribute them by default is a posture choice**, not a legal one. The reasons:

1. **Brand and trademark surface.** "COBB" and "AccessPort" are trademarks the project does not use in its own UI, packaging, or marketing (per `docs/15-clean-room-engineering.md` §12). Distributing keys that decrypt COBB-format files in a default public build invites brand-association questions even where the keys themselves are not protected material.
2. **Conservative default for a tool that interoperates with a closed-source product.** Users who actively want this capability take an explicit opt-in step (build flag + runtime flag), the same model `docs/26-bulk-reflash-cipher.md` adopts for the brick-risky ECU write path. The default-cloned-and-built binary behaves with the most conservative posture.
3. **Forward compatibility.** If COBB rotates the keys in a future AP firmware revision, the gated build allows the project to ship a key update as a contributor-side patch without disturbing the default build's behavior.

The file-vault Capability A is **not** gated for any of these reasons: it manipulates `.ptm` files as opaque blobs, contains no cipher constants, and is the same operation `cp` and `rm` do on any other USB mass storage device. The user can manage their AP's tune library, pull historic datalogs, and verify post-flash state via the AP-side MD5 — all in the default build.

## What the CLI surface looks like

```
# AP file vault — always available when the AP3 is built (default ON).
# All subcommands also accept --format text|json|toml for structured output.
subuwutuner-cli ap3 state                       # AP serial, firmware, marriage state, current ROM MD5
subuwutuner-cli ap3 ls [subdir]                 # list files under /user/ap-user/<subdir>
subuwutuner-cli ap3 pull <name> [--into <path>] # read a file off the AP
subuwutuner-cli ap3 push <local> [--as <name>]  # write a file to the AP
subuwutuner-cli ap3 rm <name>                   # remove a file from the AP
subuwutuner-cli ap3 backup [--into <dir>]       # full snapshot of /user/ap-user/

# `.ptm` tune-file family. Requires ST_ENABLE_COBB_AP_CIPHER=ON at build +
# --enable-cobb-ap-cipher at runtime. Export additionally requires
# ST_ENABLE_COBB_AP_PTM_REWRITE=ON.
subuwutuner-cli --enable-cobb-ap-cipher ptm list-patches <file.ptm> [--def <pack>]
subuwutuner-cli --enable-cobb-ap-cipher ptm inspect <file.ptm> [--def <pack>]
subuwutuner-cli --enable-cobb-ap-cipher ptm diff <a.ptm> <b.ptm> [--def <pack>] [--by-table]
subuwutuner-cli --enable-cobb-ap-cipher ptm import <file.ptm> --base-rom <path>
                                                                    [--into <dir>] [--def <pack>]
subuwutuner-cli --enable-cobb-ap-cipher ptm export <project.stune> --as <out.ptm>

# Env-var auto-discovery for `ptm import` — sets the base ROM to
# <ST_PTM_BASE_ROM_DIR>/<vehicle_id>.bin if it exists, so --base-rom
# becomes optional after one-time setup.
export ST_PTM_BASE_ROM_DIR=~/subuwutuner/base-roms
```

`ptm import` writes a `Project::open`-compatible 5-file skeleton (`project.toml` with `[ptm_metadata]` + `source.bin` + `working.bin` (patches applied) + `edits.toml` + `ptm_patches.toml`). The output project loads in the GUI immediately. `edits.toml` carries one `[[edit]]` (ByteEdit form) per imported patch, all tagged `"ptm_import"` — the imported tune appears as N undo-able entries in the history panel, and `History::undo_while_tag("ptm_import")` reverts the whole import as a single transaction. `ptm_patches.toml` is kept alongside as the round-trip-authoritative source for `ptm export`.

`ptm export` is the reverse — reads `[ptm_metadata]` + `ptm_patches.toml` from a project and rebuilds the .ptm via `encrypt_ptm`. Round-trip is byte-identical at the PrivateData XML level.

The GUI exposes the same operations through:

- **AP3 Browser panel** (`View → AccessPort Browser`) — file vault (ls / pull / push / rm / backup).
- **File → Import .ptm File…** — wizard modal mirroring `ptm import`. NFD pickers for the .ptm + base ROM + def-pack + output dir; Decode preview before commit; loads the resulting project on success.
- **File → Inspect .ptm File…** — read-only viewer mirroring `ptm inspect`. Renders identity + architectural breakdown + top-tables tables.
- **File → Export as .ptm…** — mirroring `ptm export`. Visible only when a project is loaded; gated on the same build flags as the CLI.
- **Status bar PTM-imported badge** — purple-accent chip showing the loaded project's vendor (from `[ptm_metadata].vendor_id`) when the project was created via `ptm import`.
- **Welcome panel cards** — surface the Import / Inspect entry points for first-run discoverability.

## Boot logo customization

The AccessPort displays two distinct splashes at power-on:

- **Layer 1 (~milliseconds)** — the "ACCESSPORT" wordmark, drawn by the kernel's `LINUX_LOGO` (`logo_linux_clut224.ppm` compiled into `.init.rodata`). The kernel + U-Boot live inside the i.MX28 SB v1 encrypted `bootstream.bin`. **Not replaceable.** The SB v1 master key is OTP-fused in the i.MX283 `HW_OCOTP_CRYPTO[0..3]` eFuses (silicon-side, never present in any host artifact), and the kernel source is closed. i.MX283 USB SDP recovery boot uses the same OTP key, so it only accepts COBB-signed bootstreams — no bypass. SubuwuTuner does not attempt this and does not surface UI for it. Analyst-side RE writeup at `findings/tuning-knowledge-2026-06-13/sb-bootlogo-re/`.
- **Layer 2 (held until GUI ready)** — vehicle splash drawn by `/ap-app/init`'s fallback ladder ending at `/ap-app/splash/AP-SUBARU_lg.fb`. The ladder's top priority is `/user/ap-user/images/startup_screen.fb` (RGB565 LE, 240×320 portrait, exactly 153,600 bytes) — a user-writable file in the `/user/` partition. **This is the replaceable slot.** Push via `cmd 0x22` (existing `ap3 push --as /images/startup_screen.fb`). The `st::devices::ets::rgb565` namespace (`pack`/`unpack`/`encode_rgb565_le`/`decode_rgb565_le`/`fit_letterbox`/`encode_ap_full_screen`) carries the format codec; GUI/CLI affordances compose on top.

**Clean-room posture.** SubuwuTuner ships the codec mechanics and the conversion recipe; users supply their own image. Restore-to-default works only against a user-saved backup (pull current `startup_screen.fb` to disk before replacing) — we do not bundle COBB/Subaru splash bytes for "restore default" because those are trademarked content. Deleting `/user/ap-user/images/startup_screen.fb` falls back to whichever firmware-baked `/ap-app/splash/*.fb` matches the AP's vehicle binding (which is also COBB-shipped trademarked content, but it's already on the device — we don't ship it, we just stop overriding it).

## Marriage-state caveat

Every observation in `specs/references/cobb-ap3-usb-protocol.md` is from a *married* AP. Whether the file-vault commands behave the same on an unmarried AP is unverified. The implementation must sanity-check via `cmd 0x28` (UserInfo) on connect and warn the user when the AP reports `Not Installed`, since downstream commands may behave differently. The default policy is: **refuse to operate on an unmarried AP until the unmarried-device path is bench-tested.** Override is by a separate flag.

## Tier 3 cipher status

**Sessions 1 + 2 landed 2026-06-11 evening:** the full 4-layer `.ptm` decrypt path is now functional when `-DST_ENABLE_COBB_AP_CIPHER=ON`. `decrypt_ptm(ptm_bytes)` returns `PtmContents { private_data_xml, outer_metadata_xml }` — the inner `<PrivateData>` XML with the patch list, plus the outer envelope (vendor / vehicle / lock state / etc.) with the now-decoded `<encData>` element stripped to avoid duplication. Layers wired:

- Layer 1: XTEA-CBC outer (Session 1)
- Layer 2: XML envelope extraction + base64 decode (Session 1)
- Layer 3: **AES-256-CTR (Session 2)** — implementation in `src/devices/ap3/src/aes256_ctr.cpp` wrapping vendored **tiny-AES-c** (public domain). Spec §13 key `bJTccI%878cPs%2$Tf8EXdzP2!cRUZw&` + IV `0x20 0x00…0x00`.
- Layer 4: **bzip2 inflate (Session 2)** — implementation in `src/devices/ap3/src/bzip2_decompress.cpp` wrapping vendored **bzip2-1.0.8 decompress** (BSD-style license). Compress sources dropped per the dep survey.

Both third-party sources live under `src/devices/ap3/third_party/` and compile only when the cipher build flag is ON. Default-off build emits `PolicyDenied` for every cipher function. Both layers pin byte-identically against the canonical fixture vectors (`aes_layer3_input.bin` / `aes_layer3_output.bin`, `bzip2_layer4_input.bin` / `bzip2_layer4_output.bin`).

**`encrypt_ptm` still returns `NotImplemented`** — compress-side bzip2 sources were intentionally dropped to keep the lift minimal. The asymmetric posture is fine for v1.0 (inspect a `.ptm`, edit patches via `st::edit::History`, but full re-pack requires an external tool until compress is added back).

**Session 3 (Blowfish OTA `.img`) — landed**. Clean-room Blowfish-ECB primitive lives at `src/devices/ap3/src/blowfish_ecb.cpp` (gated alongside the other cipher source); pi-derived initialization constants are mathematical facts. The actual construction is **NOT** the originally-suspected "Blowfish-CTR-double"; per the analyst's static RE of APManager.exe's `StreamEncrypter::Process` it is a single-pass Blowfish-ECB in a custom CTR mode with 512-byte chunks (32 BF blocks per chunk; counter advances by 1 per 16-byte block, then jumps by 480 between chunks — `chunk_base = base_ctr + chunk_idx × 512`). The 8-byte BF input is the 4-byte BE counter doubled; the 8-byte BF output is doubled again to form a 16-byte keystream. Validated byte-identically against `fixtures/ap3/cipher/blowfish_input.bin` → `blowfish_output_body.bin` (8192 bytes through the full pipeline) and the `decrypt_ota_img` high-level entry point against `blowfish_output_full.bin` (8230-byte envelope).

**Session 1 retained for reference:** XTEA-CBC outer + base64 envelope + outer-XML extraction + `decrypt_ptm_outer` per spec §13 layer 1. Gated behind `ST_ENABLE_COBB_AP_CIPHER` per the existing precedent (`docs/26-bulk-reflash-cipher.md` + `src/ecu/src/bulk_reflash_cipher.cpp`). Code lives at:

- `src/devices/ap3/include/st/devices/ap3/ptm_cipher.hpp`
- `src/devices/ap3/src/ptm_cipher.cpp` (gated with `#ifdef ST_AP3_HAVE_CIPHER`)

`decrypt_ptm_outer` returns the outer XML envelope (with `<encData>...</encData>` base64 still intact) — sufficient for inspecting per-file metadata (vendor / vehicle / lock state / file_hash) without paying the cost of the inner decrypt.

**Sessions 2 + 3 not yet landed:**
- Session 2 — AES-256-CTR + bzip2-L4 → full `decrypt_ptm` + `encrypt_ptm` (needs `libbz2` FetchContent + an AES implementation; ~3-4 hours)
- Session 3 — Blowfish-CTR-double OTA `.img` cipher per spec §14 (~2 hours; needs Blowfish primitive)

Until Sessions 2 + 3 land:
- `decrypt_ptm` returns `NotImplemented` even with the cipher build flag ON
- `encrypt_ptm` returns `NotImplemented` even with the cipher build flag ON
- `ap3 inspect` and `ap3 patch-list` CLI subcommands are not yet wired

When the build flag is OFF (the default), every cipher function returns `PolicyDenied` with a docs/34 pointer.

**Test coverage:** `tests/unit/devices/ap3/test_ptm_cipher.cpp` covers XTEA-CBC round-trip, base64 RFC 4648 round-trip, `<encData>` extraction with ParseError on malformed XML, and a synthetic .ptm round-trip through `decrypt_ptm_outer`. The OFF-build path verifies every API returns `PolicyDenied`. 9 ON-path tests, 1 OFF-path test.

## Codec-level command-block list (spec §6.0)

`st::transport::ap3::encode_packet` refuses to encode any of the following cmd bytes and returns `PolicyDenied` with a `§6.0` cite before the bytes hit the wire:

| cmd byte | Why blocked |
|---|---|
| `0x05` | Reboots the AP — destructive to a running USB session |
| `0x06` | OTA firmware update — could brick the AP, JTAG recovery only |
| `0x07` | Legacy upload setup to path_type=0 (system area) |
| `0x08` | Legacy upload data to path_type=0 (system area) |
| `0x18` | Documented infinite-loop handler — guaranteed daze |
| `0x29` | DirectDongleTalk — may transition AP into a non-USB state |
| `> 0x31` | Beyond the documented dispatcher range |

This is **defense-in-depth**: even a buggy higher layer that constructs the wrong cmd byte gets caught at the codec before the wire. The block list is verified by three `[transport][ap3][safety]` test cases that pin every blocked byte fires PolicyDenied and that the safe file-vault set (`0x20`–`0x23`, `0x25`, `0x26`, `0x28`, the bodyless probes) is NOT caught. The block list is identity-based — adding a new spec-allowed cmd requires removing it from the list explicitly, not waiting for a runtime allowlist to "just work."

## Malformed-body daze (spec §4.2)

The AP firmware is strict about body shape, not lenient. A syntactically-valid envelope (sync magic, valid wire_len, valid CRC, recognised cmd byte) wrapped around a malformed body — for example, u32-LE string lengths in a FileInfo2 record where the protocol requires uleb128 — permanently wedges the AP's USB state machine. Symptoms: every subsequent bulk OUT (even body-less probes like `cmd 0x28`) returns Pipe error or hangs; `libusb_clear_halt` and `libusb_reset_device` do not recover the firmware; only unplugging and replugging the AP unwedges it.

SubuwuTuner's codec layer guards against this in two ways:

1. **The FileInfo2 encoder uses uleb128 for every string-length prefix** (the specific u32-LE-vs-uleb128 trap the spec calls out). Pinned in `tests/unit/devices/ap3/test_file_info.cpp` against the spec §4.2 anti-pattern.
2. **The wire-format encoder matches the §12.2 captured fixture byte-for-byte** for the cmd 0x28 UserInfo probe — `tests/unit/transport/test_cobb_ap_packet.cpp` asserts this against the §12.2 vector.

The `--allow-unpaired-vehicle` flag still applies regardless of the daze state.

If a user reports an AP that won't respond to any SubuwuTuner command, the first response is "unplug and replug, then try again." The second is `docs/install.md` → Troubleshooting `ap3` connections. The third is to flag the daze as a possible spec gap (a malformed body we're emitting that the spec hasn't documented yet).

## What this does not do

- **Does not write to the AP's marriage state.** The marriage byte lives inside an AES-256-CTR encrypted DeviceSettings blob in NAND, and the USB protocol does not expose write access to that blob. Flipping marriage state requires JTAG and is out of scope for SubuwuTuner. SubuwuTuner is a tuning tool, not an unmarry tool.
- **Does not use the AP as an OBD flash bridge.** Capability C is post-1.0. v1.0 routes flash operations through the existing OBDX Pro VX and J2534 transports (per `docs/13-transport.md`).
- **Does not extract or modify the AP firmware itself.** The Blowfish-CTR-double cipher for OTA `.img` archives is implemented under the same gate as the `.ptm` cipher because the spec covers both, but no SubuwuTuner code path produces or modifies AP firmware images.

## `OnEncFile*` family — reframing (RE wave 5 §ε2)

The AP firmware exposes a parallel file-handler family at cmd bytes
`0x09 / 0x0a / 0x0c / 0x0d / 0x0e` named `OnEncFile{Setup,}{IN,OUT}`
in the dispatch table. Earlier project memory described these as
"encrypted file transfer"; **RE wave 5 §ε2 reframes them as file
**metadata pre-declaration** handlers, NOT encryption ops.**

What actually happens:

- The OUT side (`OnEncFileSetupOUT` at 0x09, `OnEncFileOUT` at 0x0a)
  parses an `update::FileInfo` record plus an 8-byte content hash.
  No data bytes are exchanged here.
- The IN side (`OnEncFileSetupIN` at 0x0c, alias at 0x0d,
  `OnEncFileIN` at 0x0e) are no-op stubs — the AP never returns
  encrypted content via this path.
- The actual file data upload still uses the standard PutFile path
  at cmd 0x22 / 0x23 (or the modern `OnGetFile{Setup,}IN` at
  0x20 / 0x21 for reads).

The "Enc" prefix likely stands for "**Encapsulated**" or "**Endpoint**",
not "Encrypted". The handler family is a pre-flight metadata seam
the AP uses internally during APManager's install flow; it is not a
parallel encryption protocol. SubuwuTuner does not need to call
these for any current capability — the standard `cmd 0x20-0x26`
file-vault path covers read / write / list / remove of `.ptm`
content end-to-end.

## Related

- `specs/references/cobb-ap3-usb-protocol.md` — analyst-side spec (private) covering the wire format, dispatcher, FileInfo2 layout, cipher chain, and test vectors
- `specs/cobb-ap3-implementation-checklist.md` — tiered implementer checklist for landing this capability
- `docs/13-transport.md` — SubuwuTuner's `IByteChannel` abstraction; the AP3 codec sits on top
- `docs/15-clean-room-engineering.md` — facts-vs-expression boundary and §12 trademark posture
- `docs/26-bulk-reflash-cipher.md` — the analogous gating model for the ECU's bulk-reflash cipher
- `docs/17-data-distribution-policy.md` — what the public repo ships and why
