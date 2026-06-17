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

// Subaru-DIT proprietary protocol surface — two distinct layers that
// share a CAN-RX dispatch table:
//
//   1. **SSM-extension** — Subaru factory tool commands that exist
//      independently in firmware. Confirmed on the bench:
//        0xA8 = SSM ReadByAddress
//        0xAA = SSM-extension info (dual-purpose; see below)
//        0xB8 = SSM BlockWrite
//      These reuse standard SSM-IV semantics.
//
//   2. **Atlas-protocol extensions** — block-oriented flash commands
//      from the third-party Atlas suite, source-of-truth recovered
//      via JVM agent capture (round-9 analyst spec
//      `SubuwuTuner-specs/specs/atlas-sids-and-firmware-dispatch-
//      round-9.md` §1). All 6 Atlas SIDs sit in firmware's dispatch
//      table at ROM 0x001F0888 but are **session-state gated** —
//      respond only after DSC 0x10 0x02 ProgrammingSession is granted.
//
// The 0xAA SID collides: SSM-extension info handler (empty payload)
// and AtlasRead (`<addr u32 BE> <length u16 BE>` payload) both dispatch
// to the firmware's 0xAA handler, which routes by payload shape. The
// bench has only observed the SSM-extension info branch.

// ---------------------------------------------------------------------
// SSM-extension surface — bench-confirmed without authentication
// ---------------------------------------------------------------------

// SID 0xAA empty-payload returns ACK 0xEA + 104-byte info block. This
// is the SSM-extension info handler, NOT AtlasFlashInformation (which
// is 0xA1; see Atlas section below).
//
// Per round-7 firmware analysis (analyst spec
// `SubuwuTuner-specs/specs/atlas-sids-firmware-dispatch.md` §2):
//   - First 8 bytes are a fixed device-ID prefix from ROM 0x00061896
//     on LF79002P firmware (primary template).
//   - A second template at ROM 0x00022E00 starts with the alternate
//     prefix `A2 10 14 41 80 80` and is selected by SA grant state.
//   - The remaining 96 bytes are sourced from RAM addresses via a
//     per-byte mapping table at ROM 0x001EC6F0 — opaque vendor
//     capability flags, NOT a packed flash-partition struct.
inline constexpr std::uint8_t kSidSsmExtensionInfo = 0xAAU;
inline constexpr std::uint8_t kAckSsmExtensionInfo = 0xEAU; // 0xAA | 0x40
inline constexpr std::size_t kSsmExtensionInfoBodyBytes = 104U;

inline constexpr std::size_t kDeviceIdPrefixBytes = 8U;

inline constexpr std::array<std::uint8_t, kDeviceIdPrefixBytes>
    kDeviceIdPrefixLf79002pPrimary = {
        0xA3, 0x10, 0x0F, 0xB0, 0x29, 0xB0, 0x40, 0x07,
};

inline constexpr std::array<std::uint8_t, 6U>
    kDeviceIdPrefixLf79002pSecondary = {
        0xA2, 0x10, 0x14, 0x41, 0x80, 0x80,
};

// ---------------------------------------------------------------------
// Atlas-protocol SIDs — source-of-truth from `XM.J` static-init block
// (round-9 analyst spec §1). Wire-form facts only; no encoder/decoder
// shipped yet because every Atlas SID requires ProgrammingSession,
// which the bench rig has not yet entered (rounds 1-4 still blocked
// on DSC 0x10 0x02 NRC 0x22).
// ---------------------------------------------------------------------

// Wire form: `0xA1` (no payload). Returns Atlas's FlashInformation
// response — a structured block describing the ECU's flash partitions.
// DIFFERENT from kSidSsmExtensionInfo despite both returning info
// blocks; round-9 spec §2.3 enumerates the distinction.
inline constexpr std::uint8_t kSidAtlasFlashInformation = 0xA1U;

// Wire form: `0xA2 <addr u32 BE>`. Returns block bounds for the flash
// region containing `addr`. Used by Atlas before each Program call.
inline constexpr std::uint8_t kSidAtlasBlockInformation = 0xA2U;

// Wire form: `0xAA <addr u32 BE> <length u16 BE>`. Returns the
// requested `length` bytes from `addr`. SID collides with the
// SSM-extension info handler (empty payload); the firmware's 0xAA
// dispatcher routes by payload shape.
inline constexpr std::uint8_t kSidAtlasRead = 0xAAU;

// Wire form: `0xAC <addr u32 BE>`. Erases the flash region containing
// `addr`. ECU returns a short ACK on success.
inline constexpr std::uint8_t kSidAtlasClear = 0xACU;

// Wire form: `0xAD <addr u32 BE> <data N>`. Programs `N` bytes at
// `addr`. Atlas's preferred flash-write opcode — replaces UDS
// TransferData (0x36).
inline constexpr std::uint8_t kSidAtlasProgram = 0xADU;

// Wire form: `0xAF <cmd u8> <data N>`. Handshake / state-machine
// control. `AtlasControlRequest(0)` is the flash-entry handshake;
// other sub-codes pending verification (round-9 spec §4.1).
inline constexpr std::uint8_t kSidAtlasControl = 0xAFU;

// Wire form: `0xB6 ...`. Standalone Subaru SSM bulk-transfer service.
// Composed into Atlas's registry alongside the Atlas extensions.
inline constexpr std::uint8_t kSidSubaruTransfer = 0xB6U;

// ---------------------------------------------------------------------
// SSM-extension info parser — bench-confirmed
// ---------------------------------------------------------------------

[[nodiscard]] std::span<std::uint8_t const>
reference_ssm_extension_info_body_lf79002p() noexcept;

// Parsed body of a 0xAA empty-payload (SSM-extension info) response.
// Field names are provisional offsets, NOT semantic meanings — the
// 96 trailing bytes are RAM-sourced opaque vendor flags per round-7
// spec §2.4.
struct SsmExtensionInfoResponse {
    std::uint8_t status_flag = 0;          // byte 0
    std::uint16_t field_a = 0;             // bytes 1-2
    std::uint32_t field_b = 0;             // bytes 3-6
    std::uint8_t field_c = 0;              // byte 7
    std::uint8_t field_d = 0;              // byte 8
    std::array<std::uint8_t, 2> marker_pair_0 {}; // bytes 9-10
    std::uint8_t marker_offset_40 = 0;
    std::uint8_t marker_offset_48 = 0;
    std::uint8_t marker_offset_55 = 0;
    std::vector<std::uint8_t> raw;

    [[nodiscard]] std::string hex_dump() const;
    [[nodiscard]] bool matches_reference_lf79002p() const noexcept;
    [[nodiscard]] std::array<std::uint8_t, kDeviceIdPrefixBytes>
        device_id_prefix() const noexcept;
    [[nodiscard]] bool has_lf79002p_primary_prefix() const noexcept;
};

// Parse the 104-byte body of a SID 0xAA empty-payload response. Caller
// must strip the ACK byte (0xEA) first.
[[nodiscard]] Result<SsmExtensionInfoResponse>
parse_ssm_extension_info_body(std::span<std::uint8_t const> body);

// Parse an entire response frame including the ACK byte (0xEA).
// Returns EcuRejected if the first byte is the NRC marker 0x7F.
[[nodiscard]] Result<SsmExtensionInfoResponse>
parse_ssm_extension_info_frame(std::span<std::uint8_t const> frame);

} // namespace st::ecu::subaru_atlas

#endif // ST_ECU_SUBARU_ATLAS_HPP
