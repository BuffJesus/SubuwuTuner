# 37 — Subaru ECU flash protocol reference

Reference architecture captured from analyst RE5 (2026-06-12 PM,
`D:/Subuwu/findings/re-2026-06-12-pm/`). Method list at
`findings/re-2026-06-12-pm/libflashsubaru_methods.txt`.

This document records the **reference shape** of the protocol so the
SubuwuTuner `st::flash` orchestrator can be ported against it when the
bench-rig flash gate (`docs/28` Phase 5.5/6) is online. No source from
the reference binary is reproduced; only method *signatures* and the
*sequence* they imply (per the clean-room rules in `docs/15`).

## Surface — `ecu::subaru::SH_CAN_Flash` (SH-2A, VA-era)

Public method set, in the order they're called by `DoECUFlash`:

| Order | Method | Purpose | Our equivalent |
|---|---|---|---|
| 1 | `OpenComms(rom_bytes, init_type, state)` | Open transport + select Init variant (see SA section below) | `Flasher::set_security_key_fn`, transport open via `st::transport` |
| 2 | `Connect(timeout)` | UDS `DiagnosticSessionControl` to extended/programming | `ecu::uds::UdsClient::diagnostic_session_control` |
| 3 | `EnableFlashMode()` | Session → `programmingSession` (0x02) | session field in `FlashPlan` |
| 4 | `SetFMATSMode(mode)` | Factory-mode toggle (FMATS = Factory Mode Authentication / Test Suite). Subaru-specific. | **gap** — TBD when bench rig validates |
| 5 | `EraseBlock(addr)` | UDS `RoutineControl 0x31 0x01` with erase routine ID | `execute()` issues erase routine |
| 6 | `FlashBlock(block, data, *out_progress, timeout)` / `FlashData(addr, *data)` | `RequestDownload (0x34)` → `TransferData (0x36)` chunks → `RequestTransferExit (0x37)` | `execute()` orchestrates this end-to-end |
| 7 | `ChecksumECUData(start, end)` | Ask the ECU to compute its own checksum over `[start..end]` and return it. UDS `RoutineControl 0x31 0x01` with the checksum routine ID. | **new — `Flasher::ecu_compute_checksum`** wrapper (routine ID is per-platform, plumbed in by caller) |
| 8 | `DumpECUFull()` / `DumpECURange(start, end)` | Post-flash read-back via `ReadMemoryByAddress (0x23)` | `Flasher::read_full_rom`, `read_range` |
| 9 | `Exit()` | Return ECU to default session | session control back to 0x01 |

Helpers not in the main sequence but on the surface:

- `DownloadBlock(addr)` / `DownloadByte(addr)` — RDBI-style read helpers (used internally by `DumpECURange`)
- `UploadData(*data)` — TransferData write helper (used internally by `FlashBlock`)
- `StubTest()` — self-test entry; safe to ignore from the host
- `SetFMATSMode(e_FMATS_Mode)` — enum of factory modes; values not disclosed in RE5 (need disasm pass or bench observation)

## Surface — `ecu::subaru::Rh850` (VB-era, RH850 ECUs)

VB chassis (2022+ WRX) uses a different microcontroller (RH850 instead
of SH-2A). The flash protocol is the same UDS shape; checksum + ROM
fixup helpers differ:

| Method | Purpose | Our equivalent |
|---|---|---|
| `RomIsAValidSize(rom, cpu_info)` | Reject ROMs whose size doesn't match the target CPU | `st::flash::backup_store` size validation |
| `TrimTotalRomToProgramOnly(cpu_info, *rom)` | Strip non-program regions (RAM image, calibration extents) before flashing | **gap** — needs RH850 region map |
| `CalculateBlocksums(blocks, rom_bytes, *out_sums, cpu_info)` | Compute per-block sum-of-words for each `FlashBlock` | `st::flash::checksum` extended |
| `CalculateFixupValue(target, current)` | Solve for a u16 that makes a u16-sum reach the target — i.e., the magic adjustment word | **gap** — RH850 checksum fixup |
| `Checksum(cpu_info, rom_bytes, *out_bytes, *fixup_word)` | Compute the full ROM checksum + emit the fixup word | `st::flash::checksum::IChecksumRepair` |
| `CreateRomWithFixup(cpu_info, *rom, fixup_word)` | Inject the fixup word into the ROM at the well-known offset | **gap** |
| `WriteBlocksumsToRom(rom_in, *rom_out, block_sums, cpu_info)` | Inject per-block sums into the ROM blocksum table | **gap** |
| `GetValueAtFixupPosition(cpu_info, rom)` | Read the current fixup word (for validation) | **gap** |

## SecurityAccess Init variants (RE5 finding)

`libFlashSubaru.so` ships four COBB-specific SA Init variants per
generation. Two distinguishers — `_CF` for COBB Flash (the install-side
key flow) and `_MAF_SD` for MAF speed-density variants — multiplied by
SSMIII (Gen-A.2) and SSMIV (Gen-B / AES) generations:

- `Init_SSMIII_COBB_CF::GetDecryptKeys()`
- `Init_SSMIII_COBB_MAF_SD::GetDecryptKeys()`
- `Init_SSMIV_COBB_CF::GetDecryptKeys()`
- `Init_SSMIV_COBB_MAF_SD::GetDecryptKeys()`

Our existing SA variants (`docs/23-security-access.md`):

- `ssmcan1_key_stub` — factory Gen-A.2 (SH7058 era, the working baseline)
- `ssmcan1_l1_cobb_active` — COBB-AP L1 (decoded session 2026-05-25)
- `ssmcan1_l1_fehr_active` — Fehr-active L1
- `ssmcan1_l3_fehr_active` — Fehr-active L3 (cd2ef1d)

**Mapping uncertainty**: the analyst writeup does not assert which of
the 4 RE5 Init variants corresponds to which of our existing SA names.
Best-guess one-to-one mapping (verify by bench-rig key exchange when
the rig is up):

| RE5 variant | Likely our equivalent | Confidence |
|---|---|---|
| `Init_SSMIII_COBB_CF` | `ssmcan1_l1_cobb_active` | high — both are COBB-active Gen-A install-flow |
| `Init_SSMIII_COBB_MAF_SD` | unknown — likely a SubaruSelectMonitor flavor we haven't seen | low |
| `Init_SSMIV_COBB_CF` | unknown — Gen-B AES COBB-active not yet recovered | low |
| `Init_SSMIV_COBB_MAF_SD` | unknown — Gen-B AES MAF-SD not yet recovered | low |

A follow-up Capstone disasm pass against `libFlashSubaru.so` would
extract the actual key material per variant (analyst RE6 / future pass).

## Wire protocol — ζ1 reframe (2026-06-12 PM)

**Initial assumption corrected**: the reference architecture above maps
each call (EnableFlashMode / EraseBlock / ChecksumECUData) to a UDS
`RoutineControl 0x31 0x01` invocation with a Subaru-specific routine ID
(0xff00 / 0xff01 / …). This is **wrong for Subaru**.

Per ζ1 (`findings/re-2026-06-12-pm/RE_wave6_findings.md`):

- `EnableFlashMode` is dispatched via SSM byte **`0xa5`** through a
  C++ vmethod call, **not** UDS RoutineControl. The SSM byte goes
  straight onto the wire via the reference architecture's
  `PacketExchange_ISO` primitive (which we don't yet have on the
  SubuwuTuner side).
- `EraseBlock` and `ChecksumECUData` similarly wrap their byte
  sequences in a vector and dispatch through `vtable[N]` calls; the
  underlying wire shape is SSM byte stream + payload, not UDS.
- Block boundaries (size + count per family) are populated per-
  instance via the `FlashParams` constructor arg, **not** hardcoded
  in code. SubuwuTuner's `st::flash::Flasher::compute_delta` already
  assumes per-platform sector tables — the boundary list lives in
  the def pack at runtime.

### What this means for the `Flasher::ecu_*` primitives

The five primitives shipped in commits `1fa521c` / `a00cdb1` /
`140e166` / `bc54f87` / `9c7a265` / `c20a13d`
(`ecu_enable_flash_mode` / `ecu_erase_block` / `ecu_request_download` /
`ecu_transfer_data` / `ecu_request_transfer_exit` /
`ecu_compute_checksum`) are **standard UDS RoutineControl + UDS 0x34
+ UDS 0x36 + UDS 0x37 + UDS 0x31 0x01 wrappers**. They are correct
for any ECU that speaks generic UDS flashing — they are NOT useful
for Subaru's actual flash path.

The primitives stay in tree. They remain useful for:

- Future non-Subaru targets (e.g. when SubuwuTuner expands beyond
  the Subaru tuning suite scope).
- Documenting the UDS sequence shape for callers / contributors.
- Subaru ECUs running in a UDS-compatibility mode (if any actually
  expose one — open question).

### What `SubaruShCanFlash` Tier-B actually needs

A parallel `Flasher::subaru_*` primitive set (or, equivalently,
private methods on `SubaruShCanFlash`) that:

1. Sends an SSM byte directly via the transport layer (the analyst's
   `PacketExchange_ISO` equivalent). This is one level below
   `SsmClient::read_block` — the read primitive we already have
   speaks SSM 0xA8 (ReadByAddress); the flash path needs SSM 0xA5
   (EnableFlashMode) and the matching erase / checksum bytes.
2. Wraps the byte sequence in the vector layout the reference
   architecture's vtable dispatch expects.

This is Tier-B work; ship as a separate commit when the SSM-byte
send path is in. Until then `SubaruShCanFlash::*` methods stay at
`NotImplemented` (under the `ST_ENABLE_SUBARU_ECU_FLASH=ON` build)
or `PolicyDenied` (default OFF) — the reframe doesn't change the
gate behavior.

## Implementer roadmap

Ordered by what unblocks the bench-rig flash gate.

**Tier A skeleton shipped** (commit hash recorded in CLAUDE.md). The
class `st::flash::SubaruShCanFlash` lives at
`src/flash/include/st/flash/subaru_sh_can_flash.hpp` + `.cpp`. Every
method returns `PolicyDenied` by default (build flag
`ST_ENABLE_SUBARU_ECU_FLASH=OFF`); with the flag ON the methods
return `NotImplemented`. Tier B fills in the UDS sequence body.
Three test cases pin the gate behavior at
`tests/unit/flash/test_subaru_sh_can_flash.cpp`.


1. **Map our existing SA names to the RE5 variants** (live or via disasm).
   Without this, calling `SH_CAN_Flash::OpenComms(... init_type ...)`
   from the bench rig means picking the wrong key flow → NRC 0x33.

2. **`Flasher::ecu_compute_checksum(routine_id, start, end)`** — concrete
   wrapper around `UdsClient::routine_control` for the checksum-routine
   case. The routine ID is per-ECU (LF79103P, RH850, etc.) and gets
   plumbed in from the definition pack. This unblocks Phase 5.5's
   "compute ECU-side checksum + cross-check against host expectation"
   step. **Landed alongside this doc** — see `src/flash/include/st/flash.hpp`.

3. **`SetFMATSMode` enum + setter** — capture the FMATS mode values once
   they're observed on the bench rig, then expose a `Flasher::set_fmats_mode`
   that the platform layer can call as part of `OpenComms` equivalent.

4. **RH850 checksum-fixup pipeline** — `CalculateFixupValue` +
   `CreateRomWithFixup` + `WriteBlocksumsToRom` + `GetValueAtFixupPosition`.
   The RE6 Frida capture script (`findings/for-dan/ap3-toolkit/frida_capture_ecu_flash.py`)
   will surface the fixup-word offset and the per-block sum table layout.

5. **Routine IDs catalog** — every `RoutineControl` call in the
   reference architecture has a per-platform routine ID. Once observed
   on the bench rig, they go into the definition pack
   (`[pack].flash_routine.<name> = 0xXXXX`) so the `st::flash` orchestrator
   doesn't hardcode platform-specifics.

## Cross-references

- `docs/28-bench-rig-build.md` — Phase 5.5 is the validation gate for
  this work (APManager flash → SubuwuTuner flash).
- `docs/23-security-access.md` — existing SA variants + plug-in seam.
- `docs/26-bulk-reflash-cipher.md` — 0xB6 bulk-transfer write path.
- `docs/15-clean-room-engineering.md` — why this doc records method
  signatures only, not implementation details.
- `findings/re-2026-06-12-pm/libflashsubaru_methods.txt` — full method
  list (off-tree, analyst workspace).
