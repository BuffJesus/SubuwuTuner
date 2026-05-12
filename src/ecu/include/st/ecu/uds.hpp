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

// DiagnosticSessionControl (0x10) — switch the ECU into a named session.
// Common sub-functions:
//   0x01 default            0x02 programming
//   0x03 extendedDiagnostic 0x04 safetySystemDiagnostic
inline constexpr std::uint8_t kDscDefault            = 0x01;
inline constexpr std::uint8_t kDscProgramming        = 0x02;
inline constexpr std::uint8_t kDscExtendedDiagnostic = 0x03;

[[nodiscard]] std::vector<std::uint8_t> build_dsc_request(std::uint8_t session);
[[nodiscard]] Status parse_dsc_response(std::span<std::uint8_t const> resp,
                                         std::uint8_t                  expected_session);

// ECUReset (0x11) — usually sub-function 0x01 (hardReset) or 0x03 (softReset).
inline constexpr std::uint8_t kEcuResetHard = 0x01;
inline constexpr std::uint8_t kEcuResetSoft = 0x03;

[[nodiscard]] std::vector<std::uint8_t> build_ecu_reset_request(std::uint8_t reset_type);
[[nodiscard]] Status parse_ecu_reset_response(std::span<std::uint8_t const> resp,
                                               std::uint8_t expected_reset_type);

// TesterPresent (0x3E) — heartbeat to keep a non-default session alive.
// Sub-function 0x00 (zeroSubFunction) requests a response; 0x80 suppresses it
// (use with `send` rather than `send_recv`).
[[nodiscard]] std::vector<std::uint8_t> build_tester_present_request(
    bool suppress_response = false);
[[nodiscard]] Status parse_tester_present_response(std::span<std::uint8_t const> resp);

// RequestDownload (0x34) — initiate a flash sequence.
// data_format = high nibble: compression method, low nibble: encryption method.
// address_and_length_format = high nibble: number of bytes encoding size,
//                             low nibble: number of bytes encoding address.
// The ECU's positive response carries a length-format-prefix byte and the
// maxNumberOfBlockLength (the largest size the ECU will accept per
// TransferData call). We return that max-block-length.
[[nodiscard]] std::vector<std::uint8_t> build_request_download(
    std::uint8_t                  data_format,
    std::uint32_t                 memory_address,
    std::uint32_t                 memory_size);
[[nodiscard]] Result<std::uint32_t> parse_request_download_response(
    std::span<std::uint8_t const> resp);

// TransferData (0x36) — send one block. block_sequence_counter starts at 1 and
// increments per block (wraps from 0xFF to 0x00).
[[nodiscard]] std::vector<std::uint8_t> build_transfer_data(
    std::uint8_t block_sequence_counter, std::span<std::uint8_t const> data);
[[nodiscard]] Status parse_transfer_data_response(std::span<std::uint8_t const> resp,
                                                   std::uint8_t expected_counter);

// RequestTransferExit (0x37) — finalize. Some ECUs include a CRC in the response;
// for portability we accept any positive response and ignore extra bytes.
[[nodiscard]] std::vector<std::uint8_t> build_request_transfer_exit();
[[nodiscard]] Status parse_request_transfer_exit_response(
    std::span<std::uint8_t const> resp);

// ReadMemoryByAddress (0x23) — read `memory_size` bytes from the ECU at
// `memory_address`. The wire format uses the same addressAndLengthFormat
// nibble encoding as RequestDownload (high nibble: bytes encoding size,
// low nibble: bytes encoding address). Positive response: [0x63] [data...].
[[nodiscard]] std::vector<std::uint8_t> build_read_memory_by_address(
    std::uint32_t memory_address, std::uint32_t memory_size);
[[nodiscard]] Result<std::vector<std::uint8_t>> parse_read_memory_by_address_response(
    std::span<std::uint8_t const> resp);

// WriteMemoryByAddress (0x3D) — write `data.size()` bytes from `data` to
// `memory_address`. Same address/size nibble encoding. Positive response
// echoes the address+size header: [0x7D] [aLFI] [addr...] [size...].
[[nodiscard]] std::vector<std::uint8_t> build_write_memory_by_address(
    std::uint32_t memory_address, std::span<std::uint8_t const> data);
[[nodiscard]] Status parse_write_memory_by_address_response(
    std::span<std::uint8_t const> resp,
    std::uint32_t                 expected_address,
    std::uint32_t                 expected_size);

// CommunicationControl (0x28) — gate which message classes the ECU sends or
// receives. Used during flashing to silence non-diagnostic traffic on the
// bus. Positive response: [0x68] [controlType].
inline constexpr std::uint8_t kCcEnableRxAndTx                = 0x00;
inline constexpr std::uint8_t kCcEnableRxAndDisableTx         = 0x01;
inline constexpr std::uint8_t kCcDisableRxAndEnableTx         = 0x02;
inline constexpr std::uint8_t kCcDisableRxAndTx               = 0x03;

// communicationType low-nibble bits — combine to gate multiple classes.
inline constexpr std::uint8_t kCtNormalCommunication          = 0x01;
inline constexpr std::uint8_t kCtNetworkManagement            = 0x02;
inline constexpr std::uint8_t kCtNormalAndNetworkManagement   = 0x03;

[[nodiscard]] std::vector<std::uint8_t> build_communication_control(
    std::uint8_t control_type, std::uint8_t communication_type);
[[nodiscard]] Status parse_communication_control_response(
    std::span<std::uint8_t const> resp, std::uint8_t expected_control_type);

// RoutineControl (0x31) — start, stop, or query an ECU-defined routine.
// The flash flow exercises this twice: eraseMemory before RequestDownload,
// checkProgrammingDependencies after RequestTransferExit. Other routine
// identifiers are vendor-defined.
//
// Positive response: [0x71] [sub_function] [RID_hi] [RID_lo] [status_record...]
// `parse_routine_control_response` returns the optional status record.
inline constexpr std::uint8_t  kRcStart           = 0x01;
inline constexpr std::uint8_t  kRcStop            = 0x02;
inline constexpr std::uint8_t  kRcRequestResults  = 0x03;

// Well-known routine identifiers from ISO 14229-1 Annex F.
inline constexpr std::uint16_t kRidEraseMemory                  = 0xFF00;
inline constexpr std::uint16_t kRidCheckProgrammingDependencies = 0xFF01;
inline constexpr std::uint16_t kRidEraseMirrorMemory            = 0xFF02;

[[nodiscard]] std::vector<std::uint8_t> build_routine_control(
    std::uint8_t                  sub_function,
    std::uint16_t                 routine_id,
    std::span<std::uint8_t const> option_record = {});

[[nodiscard]] Result<std::vector<std::uint8_t>> parse_routine_control_response(
    std::span<std::uint8_t const> resp,
    std::uint8_t                  expected_sub_function,
    std::uint16_t                 expected_routine_id);

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

    [[nodiscard]] Status diagnostic_session_control(
        std::uint8_t              session,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    [[nodiscard]] Status ecu_reset(
        std::uint8_t              reset_type,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    // Without suppress: round-trip an ack. With suppress_response=true the
    // caller uses send() instead; this method always round-trips.
    [[nodiscard]] Status tester_present(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    // Returns the ECU's reported maxNumberOfBlockLength on success.
    [[nodiscard]] Result<std::uint32_t> request_download(
        std::uint8_t              data_format,
        std::uint32_t             memory_address,
        std::uint32_t             memory_size,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    [[nodiscard]] Status transfer_data(
        std::uint8_t                  block_sequence_counter,
        std::span<std::uint8_t const> data,
        std::chrono::milliseconds     timeout = std::chrono::milliseconds{2000});

    [[nodiscard]] Status request_transfer_exit(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    // Erase/check-deps routines can run several seconds on real ECUs while
    // they walk the flash bank, so the default timeout is longer than the
    // other UDS services. Returns the routine status record (often empty).
    [[nodiscard]] Result<std::vector<std::uint8_t>> routine_control(
        std::uint8_t                  sub_function,
        std::uint16_t                 routine_id,
        std::span<std::uint8_t const> option_record = {},
        std::chrono::milliseconds     timeout = std::chrono::milliseconds{5000});

    [[nodiscard]] Result<std::vector<std::uint8_t>> read_memory_by_address(
        std::uint32_t             memory_address,
        std::uint32_t             memory_size,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] Status write_memory_by_address(
        std::uint32_t                 memory_address,
        std::span<std::uint8_t const> data,
        std::chrono::milliseconds     timeout = std::chrono::milliseconds{2000});

    [[nodiscard]] Status communication_control(
        std::uint8_t              control_type,
        std::uint8_t              communication_type,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

  private:
    transport::ITransport *transport_;
};

} // namespace st::ecu::uds

#endif // ST_ECU_UDS_HPP
