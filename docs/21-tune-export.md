# Atlas Tune-Export Pipeline (`subuwu::tune_export`)

> **Status**: Specification ready for implementation. Built on round-58 Tier 1 findings (checksum-balance at `0x1FFFFE`, FCU silent-drop catalog, boot integrity exact requirements). Cross-CID validity confirmed for the LF79xxxP family.

## What this module does

Transform an Atlas calibration tune (set of (offset, bytes) diffs against a base ROM) into a UDS write plan that:

1. **Cannot brick the ECU** — refuses cal diffs in FACI-locked or WDT-trap ranges.
2. **Preserves the boot integrity sum sentinel `0x5AA5`** — computes and writes the checksum-balance cell automatically.
3. **Emits a write plan in COBB-compatible ordering** — monotonic ascending addresses, trailer block (containing the balance cell) chronologically last.
4. **Is verifiable** — each block has a known expected post-write content for cross-cycle SSM-A8 RMBA verification.

## Architecture invariants

```cpp
namespace subuwu::tune_export {

// FROM ROUND-58 T1#1 (verified bit-exact against Fehr decat tune)
constexpr uint16_t SUM_TARGET        = 0x5AA5;
constexpr uint32_t SUM_RANGE_START   = 0x006000;   // inclusive
constexpr uint32_t SUM_RANGE_END     = 0x200000;   // exclusive
constexpr uint32_t BALANCE_OFFSET    = 0x1FFFFE;   // u16 BE; we own this

// FROM ROUND-58 T1#2 (FCU silent-drop catalog)
constexpr uint32_t FACI_LOCKED_END   = 0x008000;   // exclusive — addresses < this silently F6-lie
constexpr uint32_t WRITABLE_START    = 0x008000;
constexpr uint32_t WRITABLE_END      = 0x200000;   // exclusive
constexpr uint32_t WDT_TRAP_START    = 0x200000;   // addresses >= this trigger ROM 0x3A26 WDT loop

// FROM ROUND-58 T1#1 (trailer landmarks to preserve)
constexpr uint32_t TRAILER_RESERVED_START = 0x1FFFE0;
constexpr uint32_t TRAILER_RESERVED_END   = 0x1FFFFD; // up to (not including) the balance cell

// FROM ROUND-58 cross-CID diff (T3#7)
constexpr std::array<uint32_t, 5> BOOT_INTEGRITY_BYTES_MUST_BE_PRESERVED = {
    0x00006C,   // u8 paired-equality byte (mirrors high byte of *0x6010)
    0x006000,   // u16 BE flash sig 1 (= 0x5555)
    0x006010,   // u16 BE paired-equality cell (= 0x0504 stock; high byte must match *0x6C)
    0x1FFFF2,   // u16 BE flash sig 2 (= 0xAAAA)
};

}  // namespace subuwu::tune_export
```

## Core types

```cpp
namespace subuwu::tune_export {

struct CalDiff {
    uint32_t offset;
    std::vector<uint8_t> bytes;
};

struct WriteBlock {
    uint32_t addr;          // 256-byte aligned
    std::array<uint8_t, 256> data;
    enum class Tag { CalChange, BalanceWrite, RestoreFromBase } tag;
};

struct ValidationError {
    enum class Kind {
        FaciLockedAddress,       // diff in [0x0..0x7FFF]
        WdtTrapAddress,          // diff at or beyond 0x200000
        BalanceClobber,          // diff overlaps 0x1FFFFE
        TrailerClobber,          // diff overlaps [0x1FFFE0..0x1FFFFD]
        BootIntegrityClobber,    // diff modifies a byte the boot check reads
        UnalignedDiff,           // diff offset isn't on a 1-byte boundary (always allowed) — reserved
    };
    Kind kind;
    uint32_t offending_offset;
    std::string explanation;
};

}  // namespace subuwu::tune_export
```

## API

```cpp
namespace subuwu::tune_export {

// Validates the cal diffs against architecture invariants.
// Returns empty vector if all diffs are accepted; otherwise lists every violation.
[[nodiscard]] std::vector<ValidationError>
validate(std::span<const CalDiff> diffs) noexcept;

// Builds the post-tune ROM image: applies diffs to base ROM, recomputes balance,
// asserts sum invariant. Throws std::invalid_argument if validation fails.
[[nodiscard]] std::vector<uint8_t>
build_image(std::span<const uint8_t> base_rom,
            std::span<const CalDiff> diffs);

// Emits a write plan from a built image. Blocks are 256 bytes each, aligned to 0x100.
// The trailer block (containing BALANCE_OFFSET) is always the LAST entry.
[[nodiscard]] std::vector<WriteBlock>
emit_write_plan(std::span<const uint8_t> built_image,
                std::span<const uint8_t> base_rom);

// Helper: compute the balance cell value that, when written at BALANCE_OFFSET,
// makes sum(image, SUM_RANGE_START, SUM_RANGE_END) == SUM_TARGET.
[[nodiscard]] uint16_t
compute_balance(std::span<const uint8_t> image_with_balance_zeroed) noexcept;

// Helper: naive u16 BE sum over a range.
[[nodiscard]] uint16_t
u16be_sum(std::span<const uint8_t> buf, uint32_t start, uint32_t end_exclusive) noexcept;

}  // namespace subuwu::tune_export
```

## Implementation outline

### `validate`
```cpp
std::vector<ValidationError> errors;
for (auto const& d : diffs) {
    if (d.offset >= WDT_TRAP_START) {
        errors.push_back({Kind::WdtTrapAddress, d.offset, "WDT trap"});
        continue;
    }
    if (d.offset + d.bytes.size() > WDT_TRAP_START) {
        errors.push_back({Kind::WdtTrapAddress, d.offset, "extends into WDT trap"});
        continue;
    }
    if (d.offset < FACI_LOCKED_END) {
        errors.push_back({Kind::FaciLockedAddress, d.offset, "FACI-locked silent drop"});
        continue;
    }
    if (d.offset <= BALANCE_OFFSET && d.offset + d.bytes.size() > BALANCE_OFFSET) {
        errors.push_back({Kind::BalanceClobber, d.offset, "pipeline owns 0x1FFFFE"});
        continue;
    }
    if (d.offset >= TRAILER_RESERVED_START && d.offset < TRAILER_RESERVED_END) {
        errors.push_back({Kind::TrailerClobber, d.offset, "trailer is preserved"});
        continue;
    }
    for (auto b : BOOT_INTEGRITY_BYTES_MUST_BE_PRESERVED) {
        if (d.offset <= b && d.offset + d.bytes.size() > b) {
            errors.push_back({Kind::BootIntegrityClobber, b, "boot integrity check reads this"});
            break;
        }
    }
}
return errors;
```

### `build_image`
```cpp
auto errors = validate(diffs);
if (!errors.empty()) throw std::invalid_argument(format_errors(errors));

std::vector<uint8_t> img(base_rom.begin(), base_rom.end());
for (auto const& d : diffs) {
    std::copy(d.bytes.begin(), d.bytes.end(), img.begin() + d.offset);
}

img[BALANCE_OFFSET]     = 0;
img[BALANCE_OFFSET + 1] = 0;
uint16_t bal = compute_balance(img);
img[BALANCE_OFFSET]     = static_cast<uint8_t>(bal >> 8);
img[BALANCE_OFFSET + 1] = static_cast<uint8_t>(bal & 0xFF);

assert(u16be_sum(img, SUM_RANGE_START, SUM_RANGE_END) == SUM_TARGET);
return img;
```

### `emit_write_plan`
```cpp
std::vector<WriteBlock> plan;
const uint32_t trailer_addr = BALANCE_OFFSET & ~0xFFu;   // 0x1FFF00
for (uint32_t addr = WRITABLE_START; addr < WRITABLE_END; addr += 256) {
    if (addr == trailer_addr) continue;  // reserve for last

    // Only emit a block if any byte in it actually changed
    bool changed = false;
    for (size_t i = 0; i < 256; ++i) {
        if (built_image[addr + i] != base_rom[addr + i]) { changed = true; break; }
    }
    if (!changed) continue;

    WriteBlock b{};
    b.addr = addr;
    std::copy_n(built_image.begin() + addr, 256, b.data.begin());
    b.tag = (/* any byte in block is a CalDiff offset */ ? Tag::CalChange : Tag::RestoreFromBase);
    plan.push_back(std::move(b));
}

// Trailer last — it always gets written because the balance cell is always recomputed
WriteBlock trailer{};
trailer.addr = trailer_addr;
std::copy_n(built_image.begin() + trailer_addr, 256, trailer.data.begin());
trailer.tag = Tag::BalanceWrite;
plan.push_back(std::move(trailer));

return plan;
```

### `compute_balance`
```cpp
uint16_t sum = u16be_sum(image_with_balance_zeroed, SUM_RANGE_START, SUM_RANGE_END);
return static_cast<uint16_t>((SUM_TARGET - sum) & 0xFFFF);
```

## Verification protocol

For each `WriteBlock` in the plan emitted to the wire:

1. Send `B6 + addr(3-byte BE) + data(256 bytes)`. Expect `F6`.
2. After the entire plan completes and `0x37` (finalize) is sent, **cross-power-cycle** verify via SSM-A8 RMBA:
   - For each block, after a power-cycle (which drops Programming session), read 16 bytes at the block's `addr` via SSM-A8 in default session.
   - Compare against the block's first 16 bytes of `data`.
   - If any block fails, abort: the FCU silent-dropped that block. Investigate (probably an unexpected FACI-locked range — round-58 T1#2 catalog assumed exhaustive but new firmware revisions might extend the lock).
3. Do **NOT** rely on the in-flow `0xB7` readback for verification — that reads the FCU staging buffer, not real flash (round-58 §5).

## Testing strategy

### Unit tests (no bench needed)
1. `compute_balance` produces correct value for synthetic inputs.
2. `validate` rejects each kind of invalid diff with the correct error type.
3. `build_image` produces an image whose sum is exactly `0x5AA5`.
4. `build_image` for cal_diffs=[] produces the base ROM unchanged (except possibly the balance cell, which should match the base ROM's already-correct value).
5. Round-trip against real Fehr decat tune (round-58 T1#1 verification): given the analyst-replicated diff list, `build_image` produces a balance value of `0x6181`.

### Integration tests (require bench)
1. Single small cal change (1 byte) at known address → expect 1 cal block + trailer block in plan.
2. End-to-end: ship a known cal change to the bench (or user's car), cross-cycle verify the byte landed, then commit, then verify `31 01 02 02 01` returns `71 01 02 02 ??` positive.

## CID portability

Verified in round-58 T3#7 that all members of the LF79xxxP family share:
- Sum target `0x5AA5` over `[0x6000, 0x200000)`
- Boot integrity signatures at `0x6000`, `0x6010`, `0x6C`, `0x1FFFF2`
- Balance cell at `0x1FFFFE`
- FACI-locked range `[0x000000, 0x008000)`
- Trailer-reserved range `[0x1FFFE0, 0x1FFFFE)`

This module is therefore CID-agnostic within the LF79xxxP family. The base ROM must match the target ECU's installed CID (so the unchanged-block detection in `emit_write_plan` works correctly), but the architecture invariants don't change.

## Future work (round-59+)

- **JTAG-recovery hook**: when ValidationError indicates clobber, optionally allow override with `--jtag-rescue-mode` flag that JTAG-restores boot integrity bytes if a tune accidentally targets them. Not needed for normal use.
- **Multi-CID base-ROM library**: bundle stock ROMs for all common CIDs (LF79002P, LF79101P, LF79102P, LF79103P, etc.) with a lookup by installed CID. Allows tune sharing without each user needing their own stock backup.
- **Cross-cycle verify automation**: implementer ships `subaru-tune-deploy` verb that does `--write-cycle` + auto-power-cycle prompt + SSM-A8 verify + commit, all in one user-facing command.
