# 36 — Tune as patch set

## Why this doc exists

For most of SubuwuTuner's design history, a tune has been "this 2 MB ROM image." Editing meant patching ROM bytes. Flashing meant writing the whole ROM.

Recent reverse-engineering work (the `.ptm` cipher chain in `docs/34`) revealed that the tunes users actually exchange — COBB OTS tunes, ETune outputs from independent tuners, basemaps for engine swaps — are structurally **patch sets**, not ROM images. A `.ptm` file contains:

- Metadata (vendor, vehicle, lock state, ROM checksum)
- A list of `(rom_offset, length, replacement_bytes)` operations
- Wrapped in 4 cipher layers for distribution

This doc records what changes when SubuwuTuner adopts the patch-set view as a first-class model.

## The model

A tune is a triple:

```
tune = (base_rom_identifier, metadata, patch_list)
```

Where:
- **`base_rom_identifier`** — a stock ROM the tune targets (CID + romSum)
- **`metadata`** — vendor, lock mask, save timestamp, vehicle target string
- **`patch_list`** — ordered list of byte-replacement operations

To produce the flashable ROM, apply the patches to the base ROM in order.

This is a **denormalization** of the prior "tune is a ROM" model. The same tune content can be represented either way; SubuwuTuner can convert between them.

## What the patch-set view enables

Per `docs/35-tuner-overlay-architecture.md`, patches naturally cluster into 3 layers:

- **Layer 1:** OEM-table edits (the bulk of typical patches — ~60-75%)
- **Layer 2:** tuner additions in dead-fill ROM regions (~22-35%)
- **Layer 3:** code patches in 0x60000+ regions (~5%)

Each layer has different semantics, different editing UX, and different risk profiles. The patch-set model exposes this; the ROM-image model hides it.

Concrete features the patch-set model enables:

### Composable tunes

Users can stack patch sets:

```
stock + COBB_stage1 + Felix_WRX_iteration_2 + NTM_FA24_mechanical + user_tweaks
```

Each layer is independently sourced, independently versioned, independently audited. When Felix releases an updated iteration, the user re-composes — no need to manually merge.

See `specs/patch-composition-algebra.md` (private) for the composition model, conflict detection, and layer-aware resolution rules.

### Differential flash

Instead of writing 2 MB on every flash, write only the bytes that differ from currently-flashed:

```
delta_to_flash = bytes_where(new_tune.applied(stock) != current_flashed_rom)
```

For typical tune-to-tune transitions, the delta is <100 KB. Flash time goes from 3-5 minutes to ~30 seconds; brick-risk window shrinks correspondingly.

The brick-protection model (`docs/31`) needs updating for this — partial-overlay corner cases that don't exist in full-reflash mode.

### Architectural-layer-aware UI

A SubuwuTuner GUI that shows tunes can group display by layer:

```
Felix-on-FA24 v3 changes:
  Layer 1 (OEM tables — editable):     887 patches, 67,851 bytes
    Throttle - Target Throttle:        16 tables, 352 cells
    Boost - Boost Targets:              5 tables, ...
    ...
  Layer 2 (tuner additions — advanced): 39 patches, 19,596 bytes
    HPFP retune cluster @ 0x8000:       1,448 bytes
    AVCS target cluster @ 0x89b8:       1,756 bytes
    ...
  Layer 3 (code patches — DO NOT EDIT):  3 patches, 2,758 bytes
    UDS dispatch retarget @ 0x1ff040:    2,712 bytes  ⚠
```

This is a richer mental model than "this tune is 87 KB of changes."

### Pre-flash safety preview

Before any flash, SubuwuTuner shows the user the structured delta against currently-flashed:

```
You are about to flash: Felix-on-FA24_v3
Your current tune:       Fehr WRK3

Layer 1 changes:
  ✓ 423 cells of Throttle - Target Throttle (Felix's framework)
  ⚠ 22 cells of Ignition - Compensation - Coolant (overrides current)
  ...
Layer 2 changes:
  ✓ HPFP retune cluster (NTM FA24-mechanical)
  ...
Layer 3 changes:
  ⚠ UDS dispatch retarget will be REWRITTEN (Layer 3 = code; review carefully)

Bytes that WILL change:        10,584
Bytes that will NOT change:    1,500+ table cells share values between tunes

[Proceed] [Cancel] [Show table-by-table diff]
```

Users get informed-consent flash decisions instead of "trust me, this is the right tune."

### Tune-vs-tune diff

`subuwutuner-cli tune diff felix_wrk2.ptm felix_wrk3.ptm` produces structured output rather than a hex diff:

```
Differences:
  Throttle - Target Throttle - Main - A (TGV Open):  no change
  Boost - Wastegate Duty - Initial:                  16 cells, average +0.8%
  Boost - Wastegate Duty - Maximum:                  16 cells, average +0.8%
  ...
```

This matches how the user actually thinks about tunes — at the table/cell level, not at the byte level.

### Cross-project tune library

A user's tune library spans:
- AP-side `/maps/` (managed via Capability A per `docs/34`)
- Local disk (downloads, exports)
- `.stune` projects (in-progress edits)

SubuwuTuner can sync these three using the patch-set abstraction. A tune appears in multiple places but has a canonical identity (vendor + vehicle + content-hash of patches).

## Backwards compatibility with the ROM-image view

The patch-set model is a denormalization, not a replacement. Conversions remain bidirectional:

- **Patch set → ROM image:** apply each patch to the base ROM
- **ROM image → patch set:** byte-diff against a base ROM (canonicalize into minimum patch count)

Existing SubuwuTuner features that work on ROM images (the table editor, the bulk reflash flow, the brick-protection checker) continue to operate on the materialized ROM image after patches are applied. The patch-set view is an ADDITIONAL way to look at the same content.

## What doesn't change

- The `.stune` project schema still has a `source.bin` (the base ROM)
- The flash plan still writes byte-addressable PDUs to the ECU
- The brick-protection model in `docs/31` still applies
- Definition packs still describe tables in terms of byte offsets

What changes is **how tunes are exchanged, composed, and previewed** — not how the ECU is talked to.

## Implementation roadmap

Per the strategic findings (`findings/SUBUWUTUNER_STRATEGIC_APPLICATIONS_2026_06_11.md`, private):

### v1.0 (current scope)

Capabilities the patch-set view enables today:
- `.ptm` import / export — requires T3 cipher (`docs/34` gating)
- Layer-aware patch display in the GUI (architectural classifier shipped today)
- Pre-flash safety preview against `/backupcksum` (Capability A shipped today)

### v1.1

- Tune-vs-tune diff CLI command
- AP-side ROM attestation in the connect flow
- Datalog auto-pull tied to currently-flashed tune

### v1.5

- Differential flash (`docs/31` extension required)
- Composable patch sets (CLI first, GUI later)
- Tune library 3-way sync

### Post-1.0

- Logger preset inspection
- Firmware version detection
- Datalog automated analysis pipeline

## Related

- `docs/34-cobb-ap-as-tune-vault.md` — Capability A file vault + cipher gating
- `docs/35-tuner-overlay-architecture.md` — the 3-layer architectural model
- `docs/31-brick-protection-by-isa.md` — flash safety (needs differential-flash extension)
- `docs/26-bulk-reflash-cipher.md` — the gated-capability pattern
- `docs/11-definition-format.md` — the table model patch-set classification depends on
