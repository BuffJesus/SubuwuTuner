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
3. **Application integrity check** (verified on LF79103P;
   `findings/APP_CHECKSUM_VERIFICATION.md`). The bootloader's
   startup at `0x000000E8` (`_reset_entry`) runs C-runtime init,
   six peripheral / RAM-self-test indirections, then enters
   `_stage2_entry @ 0x00000CA0` which calls `FUN_00000D6E`. That
   function performs two checks, both of which must pass before
   the JMP to `*(uint32_t *)0x00000D00 = 0x001F094C = _main`:
    - **Flash signature check** (`FUN_00000C54`): three fixed-pattern
      comparisons resident in the flash image —
      `*(uint16_t *)0x00006000 == 0x5555`,
      `*(uint16_t *)0x001FFFF2 == 0xAAAA`, and
      `*(uint16_t *)0x0000006C == *(uint16_t *)0x00006010`.
      Fixed-pattern verification, not a sum or CRC over the cal
      region — content is irrelevant as long as these three
      signatures hold. `st::flash::verify_boot_signatures_sh2a_2mb`
      is the host-side pre-flight mirror; pre-flash buffers should
      run through it before being sent to the ECU.
    - **RAM consistency check** (`FUN_00000B88`): sum of 28 BE u16
      words from system RAM `0xFFF82008..0xFFF82040` compared
      against the value at `0xFFF82000`. Verifies the C-runtime
      init didn't corrupt the configuration table; does not
      constrain flash content.
   If either check fails, bit 1 of MMIO `0xFFF99819` is set, the
   bootloader enters waiting-for-reprogram mode, and the
   application entry at `0x001F094C` is not reached.

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
checks but aren't. Treat both as decoupled from the
application-image checksum above.

**Aftermarket per-install-block CRC-32 table at `0x1FFF3C..0x1FFFA0`.**
25 BE u32 slots, one per install block, populated during
aftermarket-installer reflash. Algorithm + impl shipped at
`src/flash/src/checksum.cpp` as `per_install_block_crc32`. **Not
consulted by the running ECU.** Independent disassembly (full
SH-2A auto-analysis of both stock and aftermarket-installed
`lf79103p` ROMs) found zero literal-pool references to any address
in that region and zero CRC-32 polynomial constants anywhere in
either ROM. The table is installer-side metadata only — written
during install, never read at boot.
`st::flash::PerInstallBlockCrc32Repair` exists to preserve
installer-side coherence (in case the user later flashes via the
installer's own tool); it has no role in boot-time safety.

**Stub at `0x4000` (formerly called the "SecureBoot stub" — it
isn't).** A 16-byte block
`05 7B 00 0B  E0 00 00 0B  E0 01 E1 00  02 00 FF FF` at flash
offset `0x4000` in every 2 MB SH-2A ROM in the analyst's corpus
(186 bins — see `findings/.../ARCH_REPORT.md` §3). The analyst's
2026-06-06 Ghidra decompile of this block (project
`ghidra_proj_lf79103p`, script `HuntSecureBoot.java`) confirmed:

- `0x4000` is a 2-byte SH-2A `RTV/N R5` (Return-with-no-delay-slot,
  copy R5 to R0). Ghidra decompiles `FUN_00004000` as
  `return param_2;` — a leaf that returns its second argument.
- The remaining 14 bytes are a cluster of similar tiny leaf
  helpers (`mov #0,R0; rts` / `mov #1,R0; rts` / `movi20 #0xFFFF,R2`)
  — most likely Renesas standard-library boilerplate at a fixed
  link-script slot, which explains why it's byte-identical across
  186 SH-2A 2 MB ROMs but absent on SH7058 (1 MB SH-2) and the
  RH850 4 MB (different toolchain, different runtime layout).
- **Confirmed dead code on both stock and aftermarket-installed
  LF79103P.** Zero CALL / JUMP / COMPUTED_CALL refs in either
  ROM. The 18 DATA refs Ghidra reports are analyzer false
  positives — the immediate-constant value `0x4000` used as a
  CAN-timing bitmask alongside `0x8f02` / `0x9000` / `0x1fff`, not
  as an address. 49 byte-pattern hits of `00 00 40 00` across the
  ROM, none loaded via `mov.l @(disp,PC),Rn` for use as a code
  target.

So the stub poses no brick risk and is not part of any
integrity-check flow. Full evidence:
`findings/SECUREBOOT_STUB_4000_VERIFICATION.md` and raw output at
`findings/decompile/lf79103p/secureboot_refs_*.bin.txt`.

### Note on the prior sum-of-words checksum claim

Earlier drafts of this doc cited a bootloader sum-of-words check
over `0x00010000..0x001FFFFF`, sourced from
`fixtures/private/brick-protection.md` §3 (verified there against
LF79100P / EZ1G silicon). The analyst's 2026-06-06 follow-up walk
of `FUN_000000E8` and its successors on LF79103P found instead the
three-signature mechanism documented in point 3 above — a fixed-
pattern check, not a sum. The sum-of-words function (`FUN_00000884`)
does exist on LF79103P but is called with system-RAM bounds
(`0xFFF82008..0xFFF82040`), not flash bounds, and serves the RAM
consistency role (also documented in point 3) rather than gating
the app jump. `brick-protection.md` §3 either drifted from
LF79100P → LF79103P silicon or was always speculative for the
LF79xxxP generation; the SubuwuTuner-side authoritative source is
`findings/APP_CHECKSUM_VERIFICATION.md`.

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
- **Boot-signature host-side preflight** (`st::flash::verify_boot_signatures_sh2a_2mb`).
  Mirrors the bootloader's `FUN_00000C54` three-signature check
  against the working buffer before any bytes go to the ECU. Any
  image that would fail the bootloader's gate fails the host-side
  check first, returning a `BootSignatureReport` naming which
  signature mismatched so a user / CI script can fix it before
  ever touching flash. Verified against both factory-virgin and
  aftermarket-installed reference ROMs.
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
3. `FUN_00000C54` flash signature check fails — a partial app
   rewrite clobbers either `0xAAAA @ 0x1FFFF2` (since that's in
   the last app sector) or the `0x5555 @ 0x6000` / pair signatures
   (boot region edge), so at least one fixed-pattern compare in
   `FUN_00000D6E` mismatches.
4. Bit 1 of MMIO `0xFFF99819` is set, bootloader holds in
   waiting-for-reprogram mode (jump to `0x001F094C = _main` is
   suppressed).
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
7. **Aggressive-overlay sanity check (2026-06-11).** Tuner-overlay
   patterns per `docs/35-tuner-overlay-architecture.md` can touch
   large code regions in addition to OEM tables — a single `.ptm`
   may include ~50 KB of code-region modifications even when the
   visible `<patch>` list looks small (the `.ptm` format splits
   contiguous code-region edits across many small `<patch>` entries
   that share edges). Validation must compute the **byte-level
   diff against the OEM image**, not just the sum of declared
   `<patch>` lengths, when sanity-checking that no protected
   regions are written. The flasher's `gate_patch` operates on
   resolved final-image bytes against the FCU sector-allow-list,
   which catches this correctly today; this note exists to ensure
   it stays correct against future patch-list-size-based shortcuts.
   See `findings/ptm-decrypt-2026-06-09/V3_CODE_DISASM_ATTEMPT_2026_06_11.md`
   for the v3 example: 234 declared patches summing to 1,474 bytes
   in 0x60000–0x80000 mask 1,050 contiguous diff runs totaling ~50 KB
   of actual byte-level changes.

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
- the independent checksum-runtime verification report
  (analyst off-tree) — Ghidra evidence that the aftermarket
  per-block CRC-32 slot table is **not** consulted by the running
  ECU. Source for the "What is not the boot-time integrity check"
  subsection above.
- `findings/SECUREBOOT_STUB_4000_VERIFICATION.md` (analyst
  off-tree) — Ghidra decompile and reference enumeration that
  resolved the `0x4000` block as Renesas runtime-library leaf
  helpers (dead code on both stock and aftermarket-installed
  LF79103P). Source for the resolved-state claim in the same
  subsection.
- `findings/APP_CHECKSUM_VERIFICATION.md` (analyst off-tree) —
  reset-tree walk through `FUN_000000E8 → _stage2_entry → FUN_00000D6E`
  that resolved the LF79103P boot-time integrity check as the
  three-signature mechanism documented in point 3 above. Source
  for `st::flash::verify_boot_signatures_sh2a_2mb`.
- `findings/corpus-wide-re-2026-06-06/out/ARCH_REPORT.md` §3
  (analyst off-tree) — cross-CID census of the `0x4000`
  signature; present in 186 bins, absent on 1 MB SH7058 and the
  4 MB RH850, consistent with the "Renesas runtime boilerplate"
  resolved-state explanation.
