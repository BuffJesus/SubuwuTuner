// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_ECU_UDS_HPP
#define ST_ECU_UDS_HPP

#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::uds {

// ISO 14229 (UDS) service framing. Unlike SSM, UDS carries no checksum at
// the application layer — the transport (CAN-TP / DoIP) handles integrity.
//
// Request format:   [SID] [parameters...]
// Positive response: [SID + 0x40] [parameters...]
// Negative response: [0x7F] [SID] [NRC]
//
// SubuwuTuner targets the subset that VB WRX and similar modern Subaru ECUs
// actually use. Service catalog can grow as we have real ECU traces to test
// against; below are the ones SsmClient's analogues need.

inline constexpr std::uint8_t kSidDiagnosticSessionControl = 0x10;
inline constexpr std::uint8_t kSidEcuReset                 = 0x11;
inline constexpr std::uint8_t kSidReadDataByIdentifier     = 0x22;
inline constexpr std::uint8_t kSidReadMemoryByAddress      = 0x23;
inline constexpr std::uint8_t kSidSecurityAccess           = 0x27;
inline constexpr std::uint8_t kSidCommunicationControl     = 0x28;
inline constexpr std::uint8_t kSidWriteDataByIdentifier    = 0x2E;
inline constexpr std::uint8_t kSidRoutineControl           = 0x31;
inline constexpr std::uint8_t kSidRequestDownload          = 0x34;
inline constexpr std::uint8_t kSidTransferData             = 0x36;
inline constexpr std::uint8_t kSidRequestTransferExit      = 0x37;
inline constexpr std::uint8_t kSidTesterPresent            = 0x3E;
inline constexpr std::uint8_t kSidWriteMemoryByAddress     = 0x3D;

inline constexpr std::uint8_t kPositiveResponseOffset = 0x40;
inline constexpr std::uint8_t kNegativeResponse       = 0x7F;

// Selected NRC values worth name-checking. The Error.message() always
// carries the raw NRC byte so the caller can disambiguate.
inline constexpr std::uint8_t kNrcGeneralReject                = 0x10;
inline constexpr std::uint8_t kNrcServiceNotSupported          = 0x11;
inline constexpr std::uint8_t kNrcSubFunctionNotSupported      = 0x12;
inline constexpr std::uint8_t kNrcConditionsNotCorrect         = 0x22;
inline constexpr std::uint8_t kNrcSecurityAccessDenied         = 0x33;
inline constexpr std::uint8_t kNrcInvalidKey                   = 0x35;
inline constexpr std::uint8_t kNrcExceededNumberOfAttempts     = 0x36;
inline constexpr std::uint8_t kNrcRequiredTimeDelayNotExpired  = 0x37;
inline constexpr std::uint8_t kNrcResponsePending              = 0x78;

// ---- Framing helpers ----------------------------------------------------

// Read Data By Identifier (single DID).
[[nodiscard]] std::vector<std::uint8_t> build_rdbi_request(std::uint16_t did);
[[nodiscard]] Result<std::vector<std::uint8_t>> parse_rdbi_response(
    std::span<std::uint8_t const> resp, std::uint16_t expected_did);

// Write Data By Identifier.
[[nodiscard]] std::vector<std::uint8_t> build_wdbi_request(
    std::uint16_t did, std::span<std::uint8_t const> data);
[[nodiscard]] Status parse_wdbi_response(std::span<std::uint8_t const> resp,
                                         std::uint16_t                 expected_did);

// SecurityAccess: convention is odd sub-function = requestSeed,
// even sub-function = sendKey (one greater than the seed's level).
[[nodiscard]] std::vector<std::uint8_t> build_security_access_request_seed(
    std::uint8_t sub_function);
[[nodiscard]] Result<std::vector<std::uint8_t>> parse_security_access_seed(
    std::span<std::uint8_t const> resp, std::uint8_t expected_sub_function);

[[nodiscard]] std::vector<std::uint8_t> build_security_access_send_key(
    std::uint8_t sub_function, std::span<std::uint8_t const> key);
[[nodiscard]] Status parse_security_access_key_ack(
    std::span<std::uint8_t const> resp, std::uint8_t expected_sub_function);

// ---- High-level client --------------------------------------------------

class UdsClient {
  public:
    explicit UdsClient(transport::ITransport &t) noexcept : transport_{&t} {}

    [[nodiscard]] Result<std::vector<std::uint8_t>> read_data_by_identifier(
        std::uint16_t                did,
        std::chrono::milliseconds    timeout = std::chrono::milliseconds{500});

    [[nodiscard]] Status write_data_by_identifier(
        std::uint16_t                 did,
        std::span<std::uint8_t const> data,
        std::chrono::milliseconds     timeout = std::chrono::milliseconds{500});

    // Step 1 of seed/key: ask for the seed at `sub_function`. Returns the
    // seed bytes.
    [[nodiscard]] Result<std::vector<std::uint8_t>> security_access_request_seed(
        std::uint8_t              sub_function,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    // Step 2: send the computed key at the matching even sub_function
    // (typically request-seed level + 1). Returns ok() on positive ack.
    [[nodiscard]] Status security_access_send_key(
        std::uint8_t                  sub_function,
        std::span<std::uint8_t const> key,
        std::chrono::milliseconds     timeout = std::chrono::milliseconds{500});

  private:
    transport::ITransport *transport_;
};

} // namespace st::ecu::uds

#endif // ST_ECU_UDS_HPP
