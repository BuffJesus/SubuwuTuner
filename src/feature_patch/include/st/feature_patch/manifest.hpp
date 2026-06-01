// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Patch manifest format — an 8-byte-per-record descriptor table that
// names every byte a tuner-installed patch writes to ROM. The on-ECU
// shape an installer leaves behind so the inverse operation
// (uninstall) can scan a small fixed table instead of diffing the
// whole image.
//
// Record format (matches the universal OEM-flow convention; see
// docs/30-patch-insertion.md and fixtures/private/
// findings_calibration_deltas/patch_manifest.md):
//
//   offset 0..3 : target_address  (u32 BE — into the ROM/cal map)
//   offset 4    : type            (u8)
//   offset 5    : op              (u8 — 0x01/0x02/0x04 element width)
//   offset 6..7 : length_bytes    (u16 BE — exact patched region size)
//
// Records are emitted in a flat sequence terminated by the all-zero
// sentinel `{address=0, type=0, op=0, length=0}`. Anything past the
// terminator is ignored.
//
// Type codes (observed across four analyst-side install images):
//
//   0x10 — normal cal patch (overwrite N bytes at target)
//   0x30 — verify-only / zero-fill (typically op=0x00)
//   0xFF — terminator / padding sentinel (also the empty-flash byte)
//
// The patch insertion layer (`st::feature_patch::PatchInserter`)
// emits these records alongside the actual code bytes; `feature_patch::
// flash::plan` reads them during uninstall to know what bytes to
// restore to factory values.

#pragma once

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace st::feature_patch {

// Type-byte enum mirroring the observed values. Stored as the raw
// `u8` byte on wire/in-ROM; the enum is the in-code presentation
// only.
enum class ManifestType : std::uint8_t {
    NormalPatch = 0x10,
    VerifyOnly = 0x30,
    Terminator = 0xFF,
};

// One 8-byte manifest record. `op` is left as a raw u8 since its
// value space is open (0x01 / 0x02 / 0x04 observed; future widths
// are possible). `target_address` is a 32-bit ROM-space address;
// `length_bytes` is the exact byte count the record covers.
struct ManifestRecord {
    std::uint32_t target_address{0};
    std::uint8_t type{0};
    std::uint8_t op{0};
    std::uint16_t length_bytes{0};

    [[nodiscard]] constexpr bool is_terminator() const noexcept {
        return target_address == 0 && type == 0 && op == 0 && length_bytes == 0;
    }
};

[[nodiscard]] constexpr bool operator==(ManifestRecord const &a,
                                        ManifestRecord const &b) noexcept {
    return a.target_address == b.target_address && a.type == b.type && a.op == b.op &&
           a.length_bytes == b.length_bytes;
}

inline constexpr std::size_t kRecordSize = 8;

// Encode a sequence of records to bytes. A terminator record is
// appended unconditionally — callers should pass the patch records
// only (without an explicit terminator). The output is
// `(records.size() + 1) * 8` bytes.
[[nodiscard]] std::vector<std::uint8_t>
encode(std::span<ManifestRecord const> records);

// Decode bytes back to records. Walks `bytes` 8 bytes at a time,
// stopping at the first terminator record encountered. Bytes past
// the terminator are ignored (the on-ROM convention is that the
// erased-flash filler past the manifest is `0xFF`, which is not a
// valid terminator — but a sequence may have been zero-padded after
// the terminator if it was written into a fresh region).
//
// Errors:
//   ParseError  — `bytes.size()` is not a multiple of 8, OR no
//                 terminator was found before the input ran out
//                 (a manifest must be self-terminated to be safe to
//                 scan on the ECU).
[[nodiscard]] Result<std::vector<ManifestRecord>>
decode(std::span<std::uint8_t const> bytes);

} // namespace st::feature_patch
