// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_ECU_SUBARU_DIT_HPP
#define ST_ECU_SUBARU_DIT_HPP

#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::subaru_dit {

// Subaru-DIT (Direct Injection Turbo) proprietary protocol surface —
// the vendor-specific SSM extensions used by the FA20DIT / FA24DIT ECU
// family. Two distinct layers share the firmware's CAN-RX dispatch
// table at ROM 0x001F0888:
//
//   1. **SSM-extension** — Subaru factory-tool commands that exist
//      independently in firmware. Confirmed on the bench:
//        0xA8 = SSM ReadByAddress
//        0xAA = SSM-extension info (dual-purpose; see below)
//        0xB8 = SSM BlockWrite
//      These reuse standard SSM-IV semantics.
//
//   2. **DIT block-flash extensions** — block-oriented flash commands.
//      Source-of-truth wire formats recovered via JVM agent capture
//      of a third-party tool's dispatch table (round-9 analyst spec
//      `SubuwuTuner-specs/specs/atlas-sids-and-firmware-dispatch-
//      round-9.md` §1). All 6 SIDs sit in the firmware's dispatch
//      table at ROM 0x001F0888 but are **session-state gated** —
//      respond only after DSC 0x10 0x02 ProgrammingSession is granted.
//
// The 0xAA SID collides: SSM-extension info handler (empty payload)
// and DIT Read (`<addr u32 BE> <length u16 BE>` payload) both
// dispatch to the firmware's 0xAA handler, which routes by payload
// shape. The bench has only observed the SSM-extension info branch.

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
// DIT block-flash SIDs — wire formats from round-9 analyst spec §1.
// No encoder/decoder shipped yet because every SID requires the
// ProgrammingSession that the bench rig has not yet entered (rounds
// 1-12 still blocked on DSC 0x10 0x02 precondition bitmap).
// ---------------------------------------------------------------------

// Wire form: `0xA1` (no payload). Returns a FlashInformation block
// describing the ECU's flash partitions. DIFFERENT from
// kSidSsmExtensionInfo despite both returning info blocks; round-9
// spec §2.3 enumerates the distinction.
inline constexpr std::uint8_t kSidDitFlashInformation = 0xA1U;

// Wire form: `0xA2 <addr u32 BE>`. Returns block bounds for the flash
// region containing `addr`. Called before each Program call.
inline constexpr std::uint8_t kSidDitBlockInformation = 0xA2U;

// Wire form: `0xAA <addr u32 BE> <length u16 BE>`. Returns the
// requested `length` bytes from `addr`. SID collides with the
// SSM-extension info handler (empty payload); the firmware's 0xAA
// dispatcher routes by payload shape.
inline constexpr std::uint8_t kSidDitRead = 0xAAU;

// Wire form: `0xAC <addr u32 BE>`. Erases the flash region containing
// `addr`. ECU returns a short ACK on success.
inline constexpr std::uint8_t kSidDitClear = 0xACU;

// Wire form: `0xAD <addr u32 BE> <data N>`. Programs `N` bytes at
// `addr`. Replaces UDS TransferData (0x36) in the DIT flash flow.
inline constexpr std::uint8_t kSidDitProgram = 0xADU;

// Wire form: `0xAF <cmd u8> <data N>`. Handshake / state-machine
// control. `Control(0x00)` is the flash-entry handshake; other
// sub-codes pending verification (round-9 spec §4.1).
inline constexpr std::uint8_t kSidDitControl = 0xAFU;

// Wire form: `0xB6 ...`. Standalone Subaru SSM bulk-transfer service.
// Composed into the firmware's vendor-extension registry.
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

// ---------------------------------------------------------------------
// DIT block-flash encoders / decoders — pure-function builders for the
// 6 SIDs. Wire formats per round-9 analyst spec §4.
// ---------------------------------------------------------------------

// Positive ACK convention: response SID = request SID | 0x40. NRC
// marker 0x7F precedes the SID echo + NRC code.
inline constexpr std::uint8_t kPositiveResponseOffset = 0x40U;
inline constexpr std::uint8_t kNegativeResponseSid = 0x7FU;

// Block bounds returned by AtlasBlockInformation. Per round-9 spec §4
// the response carries the addressable bounds of the flash region
// containing the queried address, plus a writable flag. Field names
// are descriptive of the bytes' role; semantic confirmation is
// pending bench validation.
struct DitBlockInformation {
    std::uint32_t base_address = 0;
    std::uint32_t end_address = 0;
    std::uint16_t block_kind = 0;   // u16 BE — region type tag
    std::uint32_t reserved = 0;     // trailing u32 — purpose TBD
    std::vector<std::uint8_t> raw;  // full positive body for re-decode
};

// FlashInformation request: `0xA1` (no payload).
[[nodiscard]] std::vector<std::uint8_t> build_flash_information_request();

// BlockInformation request: `0xA2 <addr u32 BE>`.
[[nodiscard]] std::vector<std::uint8_t>
build_block_information_request(std::uint32_t address);

// Read request: `0xAA <addr u32 BE> <length u16 BE>`.
[[nodiscard]] std::vector<std::uint8_t>
build_read_request(std::uint32_t address, std::uint16_t length);

// Clear request: `0xAC <addr u32 BE>`.
[[nodiscard]] std::vector<std::uint8_t>
build_clear_request(std::uint32_t address);

// Program request: `0xAD <addr u32 BE> <data N>`.
[[nodiscard]] std::vector<std::uint8_t>
build_program_request(std::uint32_t address, std::span<std::uint8_t const> data);

// Control request: `0xAF <cmd u8> <data N>`. `data` may be empty for
// the bare 1-byte sub-command form (e.g. Control(0x00) handshake).
[[nodiscard]] std::vector<std::uint8_t>
build_control_request(std::uint8_t sub_command, std::span<std::uint8_t const> data = {});

// Decoders. Each returns:
//   - the typed result on positive ACK,
//   - ErrorCode::EcuRejected with NRC code in the message on 0x7F NRC,
//   - ErrorCode::InvalidArgument if the frame is malformed.
[[nodiscard]] Result<std::vector<std::uint8_t>>
parse_flash_information_response(std::span<std::uint8_t const> resp);

[[nodiscard]] Result<DitBlockInformation>
parse_block_information_response(std::span<std::uint8_t const> resp);

[[nodiscard]] Result<std::vector<std::uint8_t>>
parse_read_response(std::span<std::uint8_t const> resp);

[[nodiscard]] Result<void>
parse_clear_response(std::span<std::uint8_t const> resp);

[[nodiscard]] Result<void>
parse_program_response(std::span<std::uint8_t const> resp);

[[nodiscard]] Result<std::vector<std::uint8_t>>
parse_control_response(std::span<std::uint8_t const> resp);

// ---------------------------------------------------------------------
// DitFlashClient — high-level wrapper around the 6 SIDs.
//
// Caller is expected to have entered DSC 0x10 0x02 ProgrammingSession
// (and any SA grant the firmware requires) before invoking any method
// on this client. The client does NOT manage session state; that's
// the orchestrator's job.
//
// Companion to st::ecu::ssm::SsmClient (which speaks the SSM Read /
// Write surface) and st::ecu::uds::UdsClient (standard UDS services).
// ---------------------------------------------------------------------

class DitFlashClient {
public:
    explicit DitFlashClient(transport::ITransport &t) noexcept : transport_{&t} {}

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    flash_information(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] Result<DitBlockInformation>
    block_information(std::uint32_t address,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    read(std::uint32_t address, std::uint16_t length,
         std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    [[nodiscard]] Result<void>
    clear(std::uint32_t address,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    [[nodiscard]] Result<void>
    program(std::uint32_t address, std::span<std::uint8_t const> data,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    control(std::uint8_t sub_command, std::span<std::uint8_t const> data = {},
            std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

private:
    transport::ITransport *transport_;
};

} // namespace st::ecu::subaru_dit

#endif // ST_ECU_SUBARU_DIT_HPP
