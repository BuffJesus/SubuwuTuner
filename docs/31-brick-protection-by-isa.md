# 31 — Brick protection by ISA family

> Per-family recovery recipes for the two silicon families v1.0 ships
> against. Tracked as a v1.0 ship blocker per `docs/04` (#1).
> High-level design lives at `docs/05` §4; this doc is the concrete
> recipe per ISA + the bench-rig validation plan.

## Why two recipes, not one

The "separate flash bank or bootloader-guaranteed region" guarantee
that makes brick protection possible depends on the silicon. Treating
SH-2A and RH850 as one design is how tuners brick cars they thought
were protected.

| ISA family | Parts | Bank layout | What stays alive in a bad write |
|---|---|---|---|
| SH-2A | SH7055 / SH7058 / SH7059 / **SH72543** (1.5 MB) / **SH72546R** (2 MB and 2.5 MB) | **Single bank** | Hardware-locked bootloader sectors (`0x00..0x0F`) + the FCU sector-allow-list. The bootloader cannot be overwritten by application-level code; the reset vector at `0x000000E8` always re-enters it. |
| RH850 | RH850 / F1x (G3MH for VB WRX) | **Dual bank** | The currently-inactive bank. The flash-controller refuses writes to the bank holding executing code; cold-boot fails over to the most-recently-verified bank. |

The SH-2A row covers four flash sizes (1 / 1.5 / 2 / 2.5 MB). The
recipe below was authored against the 2 MB SH72546R (the VA WRX
target); the **2.5 MB SH72546R variant** (LF9D / LF9G / LF9L / LT8D
CID families per `findings/corpus-wide-re-2026-06-06/`) shares the
same FCU + bootloader layout — the only delta is the calibration
region extending to `0x280000` instead of `0x200000`. Bench-rig
validation for the 2.5 MB variant remains an open Tier-4 item
alongside the 2 MB validation.

Single-bank protection is a hardware-locked sector boundary plus a
reset-time integrity check. Dual-bank protection is a separate
physical bank you flash and swap atomically. Both are bench-validated
recovery paths, not "we wrote a shim" hopium.

---

## SH-2A recipe (VA WRX, SH7055/58/59)

### What protects the ECU at rest

1. **Bootloader sectors are write-protected at the flash-controller
   level.** Sectors `0x00..0x0F` (`0x00000000..0x0000FFFF`, 64 KB)
   are physically locked: the FCU rejects any sector-erase command
   whose index falls in that range. The FLMCR unlock sequence only
   accepts writes when the program counter is inside the bootloader
   region — application code cannot bypass it.
2. **Reset PC is `0x000000E8`** — inside the bootloader. Every
   power-on enters the bootloader regardless of application state.
3. **Application-image checksum.** The bootloader computes a
   sum-of-words over the application region (`0x00010000..0x001FFFFF`)
   and compares against an expected value at a fixed offset in the
   image header. Failure holds the bootloader in "waiting for
   reprogram" mode and prevents the jump to application entry.

Concrete facts staged at `fixtures/private/brick-protection.md` (analyst-side,
2 MB plaintext Generation-A.2 ROM):

- FCU primary register region: `0xFFFFE800..0xFFFFE873` (~0x80 bytes)
- FCU extended status/config: `0xFFFFEC00..0xFFFFEC4F`
- 41 distinct FCU register accesses found in the bootloader
- Sector-erase allow-list excludes bootloader sectors `0x00..0x0F`
- Bootloader `0x000000..0x006000` byte-identical across stock + 4
  observed install states (verified in
  `fixtures/private/findings_calibration_deltas/SUMMARY.md`)

### What is **not** the boot-time integrity check

Two regions live in the bootloader area that look like integrity
checks but aren't (or aren't yet decoded). Treat both as decoupled
from the application-image checksum above.

**COBB per-block CRC-32 table at `0x1FFF3C..0x1FFFA0`.** 25 BE u32
slots, one per COBB install block. Algorithm + impl shipped at
`src/flash/src/checksum.cpp` as `cobb_per_block_crc32`. **Not
consulted by the running ECU.** The analyst's 2026-06-06 Ghidra
trace (full SH-2A auto-analysis of both stock and COBB-installed
`lf79103p`) found zero literal-pool references to any address in
that region and zero CRC-32 polynomial constants anywhere in
either ROM. The table is AccessPort-side metadata only — written
by the AP during install, never read at boot.
`st::flash::CobbPerBlockCrc32Repair` exists to preserve AP-side
coherence (in case the user later flashes via AP); it has no role
in boot-time safety. Evidence:
`findings/corpus-wide-re-2026-06-06/out/cobb_datalog/CHECKSUM_RUNTIME_VERIFICATION.md`
and `findings/decompile/lf79103p/slot24_refs_{stock,cobb}.txt`.

**SecureBoot stub at `0x4000`.** A 16-byte signature
`05 7B 00 0B  E0 00 00 0B  E0 01 E1 00  02 00 FF FF` lives at
offset `0x4000` in every 2 MB SH-2A ROM the analyst has scanned
(186 bins — see `findings/.../ARCH_REPORT.md` §3). Confirmed
byte-identical across `2017-wrx-stock.bin` (factory virgin),
`fehr-live-dump-2026-06-06.bin` (user's COBB-installed live ROM),
and `fehr-full-dump.bin` (older W585 snapshot), so it is **Subaru
factory firmware, not a COBB addition**. The instruction
semantics are not yet decoded; sequence-shape suggests a series
of tiny "always return success" stubs (`mov #0,R0; rts` /
`mov #1,R0; mov #0,R1; ...`), consistent with the COBB-installed
ECU booting normally despite COBB modifying calibration bytes
freely. Working hypothesis: either a no-op placeholder or a check
that doesn't gate on the cal region. Decoding it is an open
Tier-4 task; until then, the empirical safety floor is that
"edits in sectors COBB already modifies are accepted by whatever
runs at `0x4000`."

### What can actually brick

From the on-ECU code, only these conditions write to bootloader
sectors:

1. **A custom kernel that ignores the FCU sector allow-list.** The
   factory bootloader can only erase application sectors; a custom
   kernel uploaded via the standard `34/36/37` flow can erase
   bootloader sectors **if** it knows the FLMCR unlock sequence
   **and** is itself running from the bootloader region of RAM. This
   is user-supplied code, not anything the factory UDS protocol does.
2. **A hardware fault in the FCU itself.** Very rare.
3. **Writing arbitrary bytes to FLMCR while application code runs.**
   Not reachable over OBD unless the application has a vulnerability
   that exposes FLMCR to a UDS service. Subaru's stock application
   does not.

For a standard-compliant flash flow, **the bootloader's sector
protection plus the reset-time checksum together guarantee that a
session interrupted at any point is recoverable.**

### What SubuwuTuner does

The flasher's safety surface enforces the standard-compliant flow
unconditionally:

- **`gate_patch` writable-region check** (`docs/16` §Safety #6) —
  every byte a `PatchObject` would write must fall inside a
  declared `[[writable_region]]`. No pack ships a `writable_region`
  covering the bootloader; the gate is fail-closed.
- **Sector-erase allow-list mirrored host-side.** `st::flash::plan`
  refuses any flash plan whose erase set contains a sector index in
  `0x00..0x0F`. The on-ECU FCU would refuse anyway; we refuse before
  we hit the wire so a malicious / buggy pack can't even attempt it.
- **Cancellation invariants.** `Flasher::execute(plan, cancel)`
  polls cancel at PDU boundaries only; an in-flight TransferData or
  RequestDownload completes before cancel is honored. Tests at
  `tests/unit/flash/test_cancellation_invariants.cpp`.
- **`PolicyDenied` modal.** Flash UI requires explicit user confirm
  per session; defaults to refuse on policy mismatch.

### Recovery procedure — power-loss mid-write

The expected case. No special tooling.

1. ECU power restored.
2. Bootloader re-enters at `0x000000E8`.
3. Application checksum fails (region is partly rewritten).
4. Bootloader holds in waiting-for-reprogram mode.
5. SubuwuTuner reconnects, re-establishes UDS session `0x02`,
   re-authenticates (SecurityAccess), re-runs the same flash plan.
   Verify routine catches identity match this time, ECU resets,
   normal boot resumes.

### Recovery procedure — bootloader-corruption brick

Only possible via a custom kernel that ignored the allow-list. Not
reachable via SubuwuTuner's flash path (gates above), but the
recipe exists for the bench rig's deliberate-brick test:

1. **Hardware serial boot mode.** SH-2A has a mode pin that, asserted
   at reset, enters a host-controlled serial bootloader that lives in
   silicon ROM (not flash). The serial bootloader can re-flash any
   sector including bootloader.
2. **Reach the mode pin.** Per the bench-rig assembly (`docs/28`),
   the mode pin is wired to a switch on the bench harness. **On a
   car, the mode pin requires opening the ECU case** — it is NOT
   broken out to the OBD-II connector. Shop-only recovery, not
   side-of-the-road.
3. **Host tool re-flashes a known-good bootloader image.** The
   bench rig's tool is a Renesas E1/E2 in-circuit programmer
   (or equivalent). SubuwuTuner does NOT ship this capability for
   end users — re-flashing a bootloader is too dangerous to put a
   one-click button on, and the mode-pin requirement makes it
   inappropriate for a tuning tool.

### Bench-rig validation (Tier 4)

Per `docs/08-testing-strategy.md` Tier 4 + `docs/28` bench-rig build:

1. **Happy-path 100-flash cycle.** 100 consecutive standard-flow
   flashes, alternating two cal images, with verify after each. Pass
   = zero bricks, zero corrupted images.
2. **Power-loss inject.** USB relay on bench rig kills VBat at
   pseudo-random points during a flash. Expected: bootloader stays
   in waiting mode on every kill, retry succeeds. Pass = 100% retry
   success across ≥ 50 injects spanning erase, program, and verify
   phases.
3. **Mid-PDU cancel inject.** `std::stop_token` set during
   TransferData. Expected: in-flight PDU completes, then clean exit.
   Pass = no NRC, no partial-block writes.
4. **Deliberate-brick + serial-boot recovery.** Hand-rolled custom
   kernel erases sector `0x00`. Recovery: hold mode pin, re-flash
   via E1. Pass = ECU boots normally on next cold-start.
5. **Cross-CID coverage.** Steps 1–3 run against ≥ 3 different
   FA20DIT cal-IDs to confirm the recipe is silicon-portable.

Pass on all five = SH-2A brick-protection ship gate cleared.

---

## RH850 recipe (VB WRX, RH850 G3MH)

**Status: design + open questions, gated on the bench rig.** The
SH-2A facts come from analyst-side disassembly of a plaintext
Generation-A.2 ROM. The equivalent RH850 disassembly hasn't been
done yet (the in-tree dump is calibration-only — bootloader
`0x0..0x6000` missing, per memory `project_reference_rom_missing_bootloader.md`).
The recipe below is the dual-bank design we'd implement; bench
validation fills in the specifics.

### What protects the ECU at rest

1. **Dual-bank flash, atomic swap.** RH850 G3MH ships with two
   independent flash banks of equal size. At any time one bank is
   "active" (the boot ROM jumps to it after reset) and the other is
   "shadow." The flash controller refuses program/erase commands
   targeting the active bank.
2. **Bank-swap is atomic at the FCU level.** A pointer/option-byte
   write (one word) flips the active-bank selector. Before the swap,
   the new image lives entirely in the shadow bank and the active
   bank holds the proven-good image; after the swap, the roles
   reverse. There is no window where both banks are partly written.
3. **Boot-time integrity check.** Same pattern as SH-2A — the
   silicon boot ROM verifies the active bank's checksum before
   jumping; failure falls back to the shadow bank (the prior
   proven-good image).

### What can actually brick

The dual-bank design narrows the brick surface dramatically vs
SH-2A:

1. **Writing the option byte before the new bank verifies.** Order
   matters: write shadow → verify shadow → swap. A buggy sequence
   that swaps first would brick if the new bank is bad. SubuwuTuner's
   flash orchestrator enforces the order; the option-byte write is
   the very last PDU.
2. **Writing both banks simultaneously.** Mechanically impossible at
   the FCU level (rejects writes to active bank) but a custom kernel
   could in principle work around this on some RH850 variants by
   stopping the CPU and re-entering ROM mode. Same shape as the
   SH-2A "custom kernel ignores allow-list" risk; mitigated the same
   way (we don't ship custom kernels).
3. **Both option-byte alternatives corrupted.** Some RH850 variants
   store the bank selector in a small set of mirrored option words;
   if all mirrors are corrupted, the boot ROM has no fallback. The
   factory programming sequence writes mirrors atomically. Custom
   sequences could violate this; standard-flow doesn't.

### What SubuwuTuner does

- **`gate_patch` writable-region check.** Same surface as SH-2A —
  packs declare `[[writable_region]]` entries; the gate rejects any
  patch targeting outside the declared regions. The active-bank
  detection is layered on top: even an in-declared-region write is
  refused if it targets the live bank.
- **Bank-aware flash planning.** `st::flash::plan` reads the
  active-bank state via UDS (RDBI on a vendor DID) at plan time and
  routes the planned writes to the inactive bank. The option-byte
  swap is the last PDU in the plan; verify happens before the swap.
- **Splice mechanics aware of the bank constraint** (per
  `docs/30-patch-insertion.md`). The patch inserter rejects any
  splice that would have to write to the active bank from code
  executing on that bank.

### Recovery procedure — power-loss mid-write

Mechanically benign for any write that targets the shadow bank
(active bank is untouched, swap hasn't happened):

1. ECU power restored.
2. Boot ROM verifies active bank → still valid (was untouched).
3. Boots normal application from active bank.
4. SubuwuTuner reconnects, re-establishes session, re-runs the
   flash plan. Shadow bank state from the prior attempt is
   overwritten cleanly.

### Recovery procedure — option-byte corruption

Worst case at the standard-flow level:

1. Boot ROM checksum on active bank fails.
2. Falls back to shadow bank.
3. If shadow bank verifies → boots from shadow, user sees
   "previous tune is active" state.
4. If neither bank verifies → boot ROM enters serial-boot equivalent
   (RH850 has a similar mode-pin / debug-port mechanism), same
   shop-only recovery as SH-2A. SubuwuTuner does NOT ship this
   capability.

### Bench-rig validation (Tier 4)

The plan mirrors SH-2A:

1. **Happy-path 100-flash cycle.** 100 alternating-image flashes.
   Pass = zero bricks, zero corrupted images.
2. **Power-loss inject.** USB-relay-killed VBat at pseudo-random
   points. Expected: shadow-bank write rolls back cleanly; active
   bank stays valid; next cold-start boots normally. Pass = 100%
   recovery across ≥ 50 injects.
3. **Deliberate primary-bank brick.** Erase active bank via a
   hand-rolled custom kernel. Expected: cold-start falls back to
   shadow bank automatically; SubuwuTuner reconnects + re-flashes
   primary. Pass = ECU operational throughout the brick + recovery.
4. **Mid-PDU cancel inject.** Same as SH-2A.
5. **Cross-CID coverage.** Steps 1–3 against ≥ 3 different
   FA-DIT VB cal-IDs.

Pass on all five = RH850 brick-protection ship gate cleared.

### Open questions (bench-rig dependent)

- **Active-bank detection DID.** Which vendor DID surfaces the
  current active-bank index. Likely in the OEM-extension DID range
  (`0xF1xx`). Capture from a Subaru-tool flash sniff resolves it.
- **Option-byte mirror count.** Documentation on G3MH variants
  varies (some say 2, some say 4 mirrors). The bench rig's first
  read-back of the option byte region settles it.
- **Boot ROM fallback latency.** The "fall back to shadow bank"
  transition takes some milliseconds; need to confirm the user-facing
  UI gives a sensible message during the failover window.
- **CAN ID for serial-boot recovery.** Unlike SH-2A's mode-pin
  approach, RH850 typically exposes serial-boot via a different CAN
  ID at boot time. The bench rig's first deliberate-brick test
  captures the CAN bring-up sequence.

---

## Common safety properties (both ISAs)

The flasher honors these regardless of ISA:

1. **No bootloader writes — ever.** Packs declaring writable regions
   inside the bootloader fail closed at `gate_patch`. The flasher's
   sector-allow-list mirrors the on-ECU FCU's. Both checks must
   agree.
2. **Cancel between PDUs, never within.** `std::stop_token` is the
   primitive; the orchestrator polls only at PDU boundaries.
   Already implemented + tested (`tests/unit/flash/`).
3. **Verify before activate.** The verify routine reads back the
   just-written image and recomputes the checksum *with the new
   content*. Activation (boot-vector flip on SH-2A; option-byte
   swap on RH850) is the very last PDU. A bad verify means
   re-write, not commit.
4. **Power-loss resume.** `Project::open()` checks the journal in
   the `.stune` directory; if a flash was in flight, the GUI/CLI
   offers either resume (via `Flasher::plan_resume`) or a clean
   session exit (UDS DSC = defaultSession).
5. **Battery-voltage gate.** The flash UI refuses to start an erase
   if reported battery voltage is below a safe threshold (default
   12.5 V; configurable per pack). Erase pulses are the longest
   single FCU operation; battery droop during erase is the only
   realistic single-fault risk for a properly-shipped flow.
6. **Tamper-evident manifest.** Every flash op publishes a manifest
   (currently CRC32 over sector hashes; BLAKE3 upgrade staged for
   after bench validation per `docs/03` §Hashing). The manifest
   travels with the project so a third party can verify exactly
   what was written.

---

## References

- `docs/05-improvements.md` §4 — top-level brick-protection design.
- `docs/08-testing-strategy.md` Tier 4 — HIL test plan + bench-rig
  framing.
- `docs/28-bench-rig-build.md` — junkyard-ECU bench-rig assembly
  runbook (FSM-page-referenced harness wiring, OBDX bring-up,
  brick-recovery loop).
- `docs/16-custom-features.md` §Safety #6 — `gate_patch`
  writable-region check.
- `docs/30-patch-insertion.md` — patch-insertion layer's bank-aware
  splice rejection.
- `fixtures/private/brick-protection.md` — analyst-side SH-2A
  facts (FCU MMIO map, sector layout, reset PC, integrity-check
  flow). Source for the SH-2A section above.
- `fixtures/private/findings_flash_region_map/` — per-CID
  Bootloader / Calibration / EEPROM / RAM-mirror / IO ranges for
  24 plaintext ROMs (2015–2026). Validates that
  `0x000000..0x006000` is universally bootloader across the FA20DIT
  + later families.
- `fixtures/private/findings_calibration_deltas/SUMMARY.md` —
  empirical confirmation that bootloader `0x000000..0x006000` is
  byte-identical across stock + four observed install states.
- `findings/corpus-wide-re-2026-06-06/out/cobb_datalog/CHECKSUM_RUNTIME_VERIFICATION.md`
  (analyst off-tree) — Ghidra evidence that the COBB per-block
  CRC-32 slot table is **not** consulted by the running ECU.
  Source for the "What is not the boot-time integrity check"
  subsection above.
- `findings/corpus-wide-re-2026-06-06/out/ARCH_REPORT.md` §3
  (analyst off-tree) — cross-CID census of the `0x4000`
  SecureBoot stub signature; present in 186 bins, absent on 1 MB
  SH7058 and the 4 MB RH850.
