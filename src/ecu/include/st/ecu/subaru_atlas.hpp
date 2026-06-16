// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_ECU_SUBARU_ATLAS_HPP
#define ST_ECU_SUBARU_ATLAS_HPP

#include "st/core/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::subaru_atlas {

// Subaru proprietary protocol on SID 0xAA — bench-confirmed 2026-06-16
// on LF79002P (FA20DIT SH-2A 2 MB) donor ECU.
//
// SSM2 has a public-facing surface (0xA8 ReadByAddress, 0xB8 BlockWrite,
// etc.) plus a proprietary surface used by Subaru factory tools and the
// Atlas third-party suite. The probe sweep across 0x80..0xFE on the
// bench surfaced exactly three responding SIDs: 0xA8 + 0xB8 (known) and
// **0xAA (Subaru proprietary)**.
//
// 0xAA with empty payload returns ACK 0xEA + a 104-byte structured body.
// Non-empty payloads return NRC 0x13 — the handler is single-purpose.
// The body's exact field semantics are not yet decoded (round-7 analyst
// work pending: WSL-side Atlas re-decompile + LF79103P firmware-side
// disassembly of the SID dispatch handler at ROM offset 0x66000).
//
// Provisional state: this header captures the **structural** layout
// observed on the bench so future code can refer to named fields, and
// exposes the raw 104-byte body so the parser is cheap to refine once
// the analyst returns the canonical field meanings.

inline constexpr std::uint8_t kSidAtlasProprietary = 0xAAU;
inline constexpr std::uint8_t kAckAtlasProprietary = 0xEAU; // 0xAA | 0x40
inline constexpr std::size_t kFlashInformationBodyBytes = 104U;

// Captured response from LF79002P bench donor 2026-06-16. Stored as a
// public reference so callers (tests, diagnostics) can baseline-compare
// without re-running the wire transaction.
//
// SHA of donor ROM the response was captured against:
//   5fb404e70beb912224e8b141be8cc2be20cda1965d129002305aef5c116d0d11
[[nodiscard]] std::span<std::uint8_t const>
reference_flash_information_body_lf79002p() noexcept;

// Parsed body of a 0xAA empty-payload response. Field names are
// **provisional** — they correspond to the byte offsets where non-zero
// values were observed on the bench donor. Final semantic names land
// once round-7 spec (atlas-sids-firmware-dispatch.md) confirms.
struct AtlasFlashInformationResponse {
    // Byte 0 — high bit set on the bench (0xA3). Possibly a status /
    // flag word containing "FlashInformation OK" + capability bits.
    std::uint8_t status_flag = 0;

    // Bytes 1-2 — u16 BE. On the bench: 0x100F = 4111. Plausible:
    // version field, sub-protocol marker, or response length echo.
    std::uint16_t field_a = 0;

    // Bytes 3-6 — u32 BE. On the bench: 0xB029B040. Distinctive value;
    // candidate: ECU build identifier, calibration ID hash, partition
    // table base address, or fixed marker.
    std::uint32_t field_b = 0;

    // Bytes 7-10 — four single bytes. Bench: 07 41 80 80. Last two
    // bytes look like a marker pair (potentially "end-of-header" or
    // "section break").
    std::uint8_t field_c = 0;
    std::uint8_t field_d = 0;
    std::array<std::uint8_t, 2> marker_pair_0 {};

    // Bytes 11..39 — 29 zero bytes on the bench. Likely a reserved /
    // padding region inside a fixed-size struct.

    // Byte 40 — single non-zero marker. Bench: 0x80. Could be a
    // partition record start marker, a flags byte, or a region count.
    std::uint8_t marker_offset_40 = 0;

    // Bytes 41..47 — 7 zero bytes.

    // Byte 48 — single non-zero marker. Bench: 0x04. Could be a record
    // count, an enum (FA20DIT region kind?), or a length prefix.
    std::uint8_t marker_offset_48 = 0;

    // Bytes 49..54 — 6 zero bytes.

    // Byte 55 — single non-zero marker. Bench: 0x20.
    std::uint8_t marker_offset_55 = 0;

    // Bytes 56..103 — 48 zero bytes on the bench. Trailing zero region
    // consistent with "this ECU has fewer partitions than the struct
    // reserves slots for."

    // Full body bytes preserved for re-decode without going back to the
    // wire. Always exactly kFlashInformationBodyBytes long after a
    // successful parse.
    std::vector<std::uint8_t> raw;

    // Hex string of `raw` for diagnostic dumps. Space-separated, upper
    // case, no line wrapping.
    [[nodiscard]] std::string hex_dump() const;

    // Returns true if every field matches the reference capture from
    // LF79002P. Diagnostic helper — a bench session capturing a
    // different body should investigate firmware-version-specific
    // variation.
    [[nodiscard]] bool matches_reference_lf79002p() const noexcept;
};

// Parse the body of a successful 0xAA response. The caller is expected
// to have stripped the ACK byte (0xEA) before passing the body in — the
// 104 bytes should be exactly what followed the ACK on the wire.
//
// Returns InvalidArgument if `body` is not exactly
// kFlashInformationBodyBytes long.
[[nodiscard]] Result<AtlasFlashInformationResponse>
parse_flash_information_body(std::span<std::uint8_t const> body);

// Parse an entire response frame including the ACK byte. Convenience
// over parse_flash_information_body — accepts the raw response that
// SsmClient::send_byte_command would return on a real bench call,
// which on Subaru-proprietary commands includes the ACK as byte 0.
//
// Returns EcuRejected if the first byte is the NRC marker 0x7F (with
// the NRC code in the message). Returns InvalidArgument if the first
// byte is neither kAckAtlasProprietary nor 0x7F, or if the total
// length is wrong.
[[nodiscard]] Result<AtlasFlashInformationResponse>
parse_flash_information_frame(std::span<std::uint8_t const> frame);

} // namespace st::ecu::subaru_atlas

#endif // ST_ECU_SUBARU_ATLAS_HPP
