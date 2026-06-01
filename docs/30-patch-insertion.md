# 30 — Patch insertion layer

> The bridge between a compiled `PatchObject` (from `st::feature_codegen`)
> and an actual flashable ROM image. Status: design landed, implementation
> pending in `src/feature_patch/`.

## What it does

Phase 5 codegen turns a feature graph (`.stmod`) into a `PatchObject` — a
list of `HookPatch{symbol, splice_address, code, ram_claims}` entries plus
target `Arch` (SH-2A or RH850). The patch insertion layer takes that
`PatchObject` plus a target ROM image and produces a patched ROM:

1. **Allocate** a free region of ROM for each hook's `code` bytes.
2. **Splice** at each hook's `splice_address`: replace the existing
   instruction(s) with a branch/jump to the allocated code region.
3. **Emit a patch manifest** — a flat list of `(address, type, op, length)`
   records describing every byte the patch writes, so the inverse
   operation (uninstall) can restore factory values.
4. **Recompute checksums** so the patched ROM passes the ECU's on-boot
   integrity check.

The output is `Result<PatchedRom>` containing the modified bytes + the
manifest + an `applied_patches` record the project can re-load.

## Architectural fit

```
.stmod (graph) ──► feature::lower ──► ir::Module
                                          │
                                          ▼
                              feature_codegen::compile
                                  (SH-2A or RH850)
                                          │
                                          ▼  PatchObject
                                          │
                              feature_patch::apply  ◄── existing ROM
                                          │
                                          ▼  PatchedRom
                                          │
                                          ▼
                              checksum::recompute
                                          │
                                          ▼
                                   flash::execute
                                          │
                                          ▼
                                       ECU
```

`PatchedRom` is the contract surface for `st::flash`. The flash
orchestrator already accepts a byte-level diff plan; patch insertion is
the layer that turns the abstract "I want this hook to fire" into the
concrete "these N bytes change at these M addresses." The diff plan is
then derived by `flash::plan` comparing source vs patched.

## The free-RAM vs free-ROM split

`feature_codegen` already allocates **RAM** for each hook (the
`RamAllocator` working from the hook's `free_ram` region). RAM holds the
runtime values the patch reads/writes — e.g. a `LoadConstant` materializes
a 4-byte literal pool entry in RAM; `StoreHookOutput` writes the result to
a RAM slot the firmware can poll.

Patch insertion allocates **ROM** for the `code` bytes — the actual SH-2A
or RH850 machine instructions. The pack must declare at least one
`[[writable_region]]` of `kind = "code"` for the inserter to draw from.
For VA WRX MT (SH7058) the natural choice is the upper end of the
calibration region or an unused stretch in the early-cal scalar block.
For VB WRX (RH850 G3MH) the dual-bank flash layout opens up more space
but the inserter is bank-aware (the active bank must not have its boot
sector or vector table touched).

The codegen-side `gate_patch` (docs/16 §Safety #6) already verifies that
the inserter's chosen target address is inside a declared writable
region. That gate runs as a final check before any `PatchObject` is
returned. No `code` bytes can land in an undeclared region.

## Splice mechanics — SH-2A

A hook is a single instruction at `splice_address` whose execution the
patch wants to intercept. The original instruction is typically:

- A function prologue (e.g. `2F E6 MOV.L R14, @-R15`) → splice replaces
  the prologue and the patch jumps to the original after running.
- A specific call site (e.g. `4F 22 STS.L PR, @-R15`) → similar.
- A vector-table entry → the patch is the new ISR; the inserter writes
  the new entry into the vector table and the prior ISR address becomes
  the "after" continuation.

The splice itself is a `BRA` or `MOV.L #addr, Rx; JMP @Rx` pair depending
on distance. SH-2A's `BRA` reaches ±4 KB; for longer jumps the patch
uses the literal-pool pattern `MOV.L PC-relative, R0; JMP @R0; NOP`.
That's 8 bytes which forces the inserter to displace 4 16-bit
instructions (or 2 32-bit ones) — those displaced instructions move into
the patch's own preamble and execute before the hook body so the splice
is transparent to surrounding code.

## Splice mechanics — RH850

RH850's `JMP [reg]` plus a MOVHI+MOVEA address materialization is what
the existing `emit_rh850_*` fragments already use for their `JMP [lp]`
tail. The same 12-byte sequence (`MOVHI hi, r0, r12; MOVEA lo, r12, r12;
JMP [r12]`) can splice from any address to any 32-bit target. For shorter
in-bank jumps the 32-bit relative `JR disp22` reaches ±2 MB and only
takes 4 bytes.

RH850's dual-bank flash adds a constraint: a splice into bank B from
code executing on bank A is fine, but the inserter must **never** write
to the bank currently executing. Active-bank detection is via the
`bank_select` register state at flash time; the inserter rejects any
splice that targets the active bank with `FlashAuthority` error.

## The manifest format

The inserter emits an 8-byte-per-record descriptor table that names
every byte the patch writes. Format (vendor-neutral; this is the
universal shape OEM-flow tools converge on):

```
offset 0..3 : target_address  (u32 BE)
offset 4    : type            (u8)
offset 5    : op              (u8 — element width: 0x01/0x02/0x04)
offset 6..7 : length_bytes    (u16 BE)
```

Type codes (observed in analyst-side packs at `fixtures/private/`):

| `type` | meaning |
|---|---|
| `0x10` | normal cal patch (overwrite N bytes at target) |
| `0x30` | verify-only / zero-fill (`op=0x00`) |
| `0xFF` | terminator / padding sentinel |

The manifest is itself written to the ROM, in the patch's allocated
region. The location is recorded in the pack as a `[[manifest_region]]`
descriptor (defaults to the start of the first `[[writable_region]] kind
= "code"`). Two manifests are emitted:

1. **Manifest A** — primary cal tables. Larger, fewer records (tens).
2. **Manifest B** — scalar overlay. Smaller records, more entries
   (hundreds — every small parameter the patch touches).

This split matches the convention OEM-flow tools follow; it lets the
uninstaller scan a small fixed-position record for table-shaped patches
and a separate region for scalar parameters. Our implementation can
emit a single combined manifest at first, splitting later if a target
pack expects the two-region convention.

### Why an explicit manifest

The patch insertion layer could in principle just diff source vs
patched ROM at uninstall time. But:

- **Faster uninstall** — the manifest is O(records) to scan; a diff is
  O(rom size).
- **Robust against partial flashes** — if a flash was interrupted mid-
  install, the manifest names exactly which records have been applied
  vs which haven't. Diffing a partially-flashed ROM would falsely
  report mixed state as "different tune."
- **Aligns with OEM-flow tools** — packs produced by tuner-side
  toolchains already use this convention; emitting the same shape
  means a SubuwuTuner-installed patch can be uninstalled by any tool
  that recognizes the format (and vice versa).
- **Self-contained on the ECU** — the running ECU has everything it
  needs to know its current patch state without external metadata.

## Checksum recompute

The patched ROM must pass the ECU's on-boot integrity check. SH-2A
and RH850 use different checksum schemes; the existing
`st::checksum::Subaru{Std,Alt,Alt2}` modules cover the SH-2A cases
(see `src/checksum/`). RH850 checksums are similar in spirit but
distinct in window placement; the inserter calls
`st::checksum::recompute(rom, def)` once at the end with the patched
bytes in place. The pack's `[[checksum]]` entries tell the recomputer
which windows to recompute and which algorithm to use.

Checksum bytes themselves go into the manifest as
`type = 0x10, op = 0x02, length = 2` records — the ECU will overwrite
them on next install but the uninstaller needs to know the patched-
side checksum to detect "this ROM is currently patched."

## Module shape

```
src/feature_patch/
├── include/st/feature_patch/
│   ├── manifest.hpp          ManifestRecord struct + encode/decode
│   ├── inserter.hpp          PatchInserter (entry point, takes
│   │                          PatchObject + Rom, returns PatchedRom)
│   ├── splice.hpp            arch-tagged splice emitters (SH-2A / RH850)
│   └── rom_allocator.hpp     RomAllocator analog of RamAllocator
├── src/
│   ├── manifest.cpp
│   ├── inserter.cpp
│   ├── splice_sh2a.cpp
│   ├── splice_rh850.cpp
│   └── rom_allocator.cpp
└── ...
tests/unit/feature_patch/
├── test_manifest.cpp         round-trip, terminator handling, mixed types
├── test_inserter.cpp         end-to-end: PatchObject + Rom → PatchedRom
├── test_splice_sh2a.cpp      short BRA vs long JMP, prologue displacement
├── test_splice_rh850.cpp     JR disp22 vs MOVHI+MOVEA+JMP, dual-bank gate
└── test_rom_allocator.cpp    multi-region drawing, fragmentation
```

Public entry point:

```cpp
[[nodiscard]] Result<PatchedRom>
PatchInserter::apply(PatchObject const &patch,
                     Rom const &target_rom,
                     Definition const &def);
```

The `Rom` type is the existing `st::rom::Rom`. `PatchedRom` is a thin
wrapper:

```cpp
struct PatchedRom {
    std::vector<std::uint8_t> bytes;      // modified ROM image
    std::vector<ManifestRecord> manifest; // every record written
    std::uint32_t manifest_addr;          // where manifest lives in ROM
    std::string tuner_tag;                // 4-byte ASCII identity
};
```

`tuner_tag` defaults to `"SUBU"` (SubuwuTuner). The pack can override
via `[[pack]] tuner_tag = "..."` if a project's flash needs to
co-exist with another tool's manifest format.

## Test plan

Hardware-free coverage:

- **Manifest round-trip**: encode N records, decode, byte-identical.
- **Manifest terminator**: `addr=0, ctrl=0` ends iteration; trailing
  garbage past terminator ignored.
- **Splice short-form (SH-2A)**: a `splice_address` within ±4 KB of
  the patch code yields a single `BRA` (2 bytes); patch insertion
  succeeds without displacing any extra instructions.
- **Splice long-form (SH-2A)**: a `splice_address` beyond ±4 KB
  yields the 8-byte `MOV.L + JMP + NOP` sequence; the inserter
  reports correctly which displaced instructions moved into the
  patch preamble.
- **Splice short-form (RH850)**: similar with `JR disp22`.
- **Splice long-form (RH850)**: 12-byte `MOVHI + MOVEA + JMP`.
- **ROM allocator exhaustion**: small writable region + large patch
  code yields `OutOfRange` not silent overflow.
- **End-to-end**: load `fixtures/private/subaru_data/reference/2017-wrx-stock.bin`
  as the target, compile a trivial PatchObject (one LoadConstant →
  StoreHookOutput), apply, verify (a) the manifest names exactly the
  splice address and the code region, (b) the patched bytes match
  the codegen output bit-for-bit, (c) `checksum::verify` passes after
  recompute.
- **Cross-reference end-to-end against staged install-derived ROMs**:
  the analyst-side derived ROMs at
  `fixtures/private/findings_calibration_deltas/install-derived-roms/`
  give us tuner-flow patched ROMs to compare shape against. Our
  inserter's output should follow the same overall layout (free-region
  use + manifest at a known offset + bootloader untouched) even though
  the specific bytes differ.

Hardware-gated:

- Apply a SubuwuTuner-emitted patch to a junkyard ECU via the flash
  orchestrator; read back; byte-identical to expected.
- Uninstall via manifest scan; read back; byte-identical to original.
- Three install-uninstall cycles in a row; no drift.

## Open design questions

1. **Manifest position convention.** Tuner-flow tools place the manifest
   at conventional fixed offsets (`0x007C00` + `0x008000` on FA20DIT
   LF79xxx). Do we follow the convention for cross-tool interop, or
   route by pack-supplied `[[manifest_region]]` exclusively? Recommend:
   pack-supplied is authoritative; default to the conventional offset
   when the pack doesn't specify.

2. **Bootloader sanctity.** Analyst-side findings on FA20DIT show
   `0x000000..0x006000` is byte-identical across stock, three tuner
   stages, and the user's e-tune — bootloader is a hard no-write region.
   Should `gate_patch` extend with a hard-coded `[[no_write_region]]`
   entry that's enforced regardless of pack contents? Recommend: yes,
   with per-arch defaults (SH-2A: `0x000000..0x006000`; RH850: TBD per
   bench-rig validation).

3. **Multi-bank flash on RH850.** The current `gate_patch` only
   validates address containment, not bank state. Where does the
   "do not write to the active bank" check live — in the inserter
   (its addresses are still pre-flash and abstract) or in the flash
   orchestrator (which knows the live bank state)? Recommend: both —
   the inserter rejects patches whose target address could never be
   safe to write on either bank; the flash orchestrator rejects at
   commit time if the live-bank state forbids the planned write.

4. **Hook displacement audit trail.** When a splice displaces 4
   instructions into the patch preamble, the original behavior is
   conserved only if those displaced instructions are PC-independent.
   A `BSR` or `MOV.L @(disp, PC), Rn` in the displaced range breaks
   the splice silently. Recommend: a splice-time analyzer that
   rejects PC-relative instructions in the displacement window with
   a precise error pointing at the offending instruction.

## Sequencing

1. **`manifest.hpp` + `manifest.cpp` + tests** — encode/decode the
   8-byte record format. Zero dependencies; pure data. ~1 day.
2. **`rom_allocator.hpp` + `rom_allocator.cpp` + tests** — analog of
   `RamAllocator` but draws from a ROM `[[writable_region]] kind =
   "code"`. ~½ day.
3. **`splice_sh2a.cpp` + tests** — short/long-form splice emitters
   for SH-2A; PC-relative displacement audit. ~2 days.
4. **`splice_rh850.cpp` + tests** — same for RH850; dual-bank gate.
   ~1 day (less because fewer instruction-format variants).
5. **`inserter.cpp` end-to-end + integration tests** — wires
   everything; tests against `2017-wrx-stock.bin` fixture. ~1 day.
6. **`flash::plan` integration** — `PatchedRom` flows into the flash
   orchestrator's diff-planning stage. ~½ day. Hardware-free.
7. **Bench-rig validation** — hardware-gated; happens once the
   junkyard ECU is wired in.

Total: ~6 days of focused work to land items 1–6. Item 7 is its
own thing.

## References

- `docs/16-custom-features.md` — feature graph + codegen design + the
  `gate_patch` writable-region check (`§Safety #6`).
- `docs/11-definition-format.md` — `[[writable_region]]`,
  `[[checksum]]`, `[[hook]]` schemas the inserter consumes.
- `docs/05-improvements.md` `§4` — brick protection design; the
  inserter's safety surface aligns with the recovery design's
  assumption that no writes ever reach the bootloader.
- `docs/21-stune-format.md` — `.stune` project format; an applied
  patch lands as a `[[patch]]` entry the project tracks for the
  uninstall path.
- `fixtures/private/findings_calibration_deltas/` — analyst-side
  reference reports describing the OEM-flow manifest format we're
  matching. `SUMMARY.md` is the entry point; `patch_manifest.md` and
  `manifest_b.md` decode the two on-ROM manifests.
- `fixtures/private/subaru_data/reference/2017-wrx-stock.bin` — the
  end-to-end test fixture for SH-2A inserter integration tests.
