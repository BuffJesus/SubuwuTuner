// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/uds.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::uds {

namespace {

// Surface a negative-response frame (0x7F SID NRC) as EcuRejected. Returns
// ok() when `resp` is not a negative response — caller continues parsing.
// Hex-format a byte as "0xXX" for inclusion in error messages.
// std::to_string is decimal; using "0x" + to_string(0x33) produces the
// confusing "0x51" (51 decimal).
std::string hex_byte(std::uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", static_cast<unsigned>(b));
    return std::string{buf};
}

[[nodiscard]] Status reject_if_negative(std::span<std::uint8_t const> resp,
                                        std::uint8_t expected_sid) {
    if (resp.size() >= 3 && resp[0] == kNegativeResponse) {
        if (resp[1] != expected_sid) {
            return failure(ErrorCode::ParseError,
                           "UDS negative response for wrong SID: " + hex_byte(resp[1]));
        }
        return failure(ErrorCode::EcuRejected, "UDS NRC=" + hex_byte(resp[2]));
    }
    return ok();
}

// SA-aware variant of reject_if_negative. Same shape, but for the four NRCs
// that show up regularly in seed/key flows it appends a human-readable name
// plus (where actionable) recovery guidance. The bare NRC byte alone is
// hard to interpret in the heat of a stuck SA session; tagging the message
// with "exceededNumberOfAttempts" + "power-cycle the ECU" turns a "wtf is
// happening" moment into a directly actionable one.
[[nodiscard]] Status reject_if_sa_negative(std::span<std::uint8_t const> resp) {
    if (!(resp.size() >= 3 && resp[0] == kNegativeResponse)) {
        return ok();
    }
    if (resp[1] != kSidSecurityAccess) {
        return failure(ErrorCode::ParseError,
                       "UDS negative response for wrong SID: " + hex_byte(resp[1]));
    }
    auto const nrc = resp[2];
    std::string msg = "UDS NRC=" + hex_byte(nrc);
    switch (nrc) {
        case kNrcSecurityAccessDenied:
            msg += " (securityAccessDenied) - this SA level is not permitted "
                   "for the current session / target operation";
            break;
        case kNrcInvalidKey:
            msg += " (invalidKey) - the seed/key derivation produced a key the "
                   "ECU rejected. After ~3 total consecutive invalidKey responses "
                   "(per ISO 14229 §10.4.2.3) the ECU returns exceededNumberOfAttempts "
                   "(NRC 0x36) and locks SA for ~10 minutes; verify the key function "
                   "before retrying.";
            break;
        case kNrcExceededNumberOfAttempts:
            msg += " (exceededNumberOfAttempts) - ECU has locked SecurityAccess "
                   "after too many bad-key attempts. To reset: power-cycle the "
                   "ECU (ignition off, wait 10s, ignition on) OR wait ~10 minutes "
                   "for automatic unlock.";
            break;
        case kNrcRequiredTimeDelayNotExpired:
            msg += " (requiredTimeDelayNotExpired) - wait briefly (~10s) before "
                   "retrying SA; the ECU is in a post-failure cooldown.";
            break;
        default:
            break;
    }
    return failure(ErrorCode::EcuRejected, std::move(msg));
}

void append_be16(std::vector<std::uint8_t> &out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

[[nodiscard]] std::uint16_t read_be16(std::uint8_t hi, std::uint8_t lo) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8U) | lo);
}

} // namespace

// ---- RDBI ---------------------------------------------------------------

std::vector<std::uint8_t> build_rdbi_request(std::uint16_t did) {
    std::vector<std::uint8_t> out;
    out.reserve(3);
    out.push_back(kSidReadDataByIdentifier);
    append_be16(out, did);
    return out;
}

Result<std::vector<std::uint8_t>> parse_rdbi_response(std::span<std::uint8_t const> resp,
                                                      std::uint16_t expected_did) {
    if (auto r = reject_if_negative(resp, kSidReadDataByIdentifier); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [62] [did_hi] [did_lo] [data...]
    if (resp.size() < 3) {
        return failure(ErrorCode::ParseError, "UDS RDBI response too short");
    }
    if (resp[0] != kSidReadDataByIdentifier + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS RDBI unexpected SID: " + hex_byte(resp[0]));
    }
    auto const got_did = read_be16(resp[1], resp[2]);
    if (got_did != expected_did) {
        return failure(ErrorCode::ParseError,
                       "UDS RDBI DID mismatch: expected 0x" +
                           std::to_string(static_cast<unsigned>(expected_did)) + " got 0x" +
                           std::to_string(static_cast<unsigned>(got_did)));
    }
    return std::vector<std::uint8_t>(resp.begin() + 3, resp.end());
}

// ---- WDBI ---------------------------------------------------------------

std::vector<std::uint8_t> build_wdbi_request(std::uint16_t did,
                                             std::span<std::uint8_t const> data) {
    // Init-list construction + insert avoids two distinct GCC false positives:
    // -Wnull-dereference on `out[N] = ...` after sized-init (GCC 13) and
    // -Wfree-nonheap-object on reserve+push_back (GCC 15). The init-list
    // constructor makes `out` non-empty by construction, so GCC's null-
    // analysis sees the storage as definitely allocated.
    std::vector<std::uint8_t> out{
        kSidWriteDataByIdentifier,
        static_cast<std::uint8_t>((did >> 8) & 0xFFU),
        static_cast<std::uint8_t>(did & 0xFFU),
    };
    out.reserve(3 + data.size());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Status parse_wdbi_response(std::span<std::uint8_t const> resp, std::uint16_t expected_did) {
    if (auto r = reject_if_negative(resp, kSidWriteDataByIdentifier); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 3) {
        return failure(ErrorCode::ParseError, "UDS WDBI response too short");
    }
    if (resp[0] != kSidWriteDataByIdentifier + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS WDBI unexpected SID: " + hex_byte(resp[0]));
    }
    auto const got_did = read_be16(resp[1], resp[2]);
    if (got_did != expected_did) {
        return failure(ErrorCode::ParseError, "UDS WDBI DID mismatch");
    }
    return ok();
}

// ---- SecurityAccess -----------------------------------------------------

std::vector<std::uint8_t> build_security_access_request_seed(std::uint8_t sub_function) {
    return std::vector<std::uint8_t>{kSidSecurityAccess, sub_function};
}

Result<std::vector<std::uint8_t>> parse_security_access_seed(std::span<std::uint8_t const> resp,
                                                             std::uint8_t expected_sub_function) {
    if (auto r = reject_if_sa_negative(resp); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [67] [sub_function] [seed...]
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS SA seed response too short");
    }
    if (resp[0] != kSidSecurityAccess + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS SA unexpected SID");
    }
    if (resp[1] != expected_sub_function) {
        return failure(ErrorCode::ParseError, "UDS SA sub-function mismatch");
    }
    return std::vector<std::uint8_t>(resp.begin() + 2, resp.end());
}

std::vector<std::uint8_t> build_security_access_send_key(std::uint8_t sub_function,
                                                         std::span<std::uint8_t const> key) {
    // See build_wdbi_request — same GCC false-positive workaround.
    std::vector<std::uint8_t> out{kSidSecurityAccess, sub_function};
    out.reserve(2 + key.size());
    out.insert(out.end(), key.begin(), key.end());
    return out;
}

Status parse_security_access_key_ack(std::span<std::uint8_t const> resp,
                                     std::uint8_t expected_sub_function) {
    if (auto r = reject_if_sa_negative(resp); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS SA key ack too short");
    }
    if (resp[0] != kSidSecurityAccess + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS SA unexpected SID");
    }
    if (resp[1] != expected_sub_function) {
        return failure(ErrorCode::ParseError, "UDS SA sub-function mismatch");
    }
    return ok();
}

// ---- DiagnosticSessionControl ------------------------------------------

std::vector<std::uint8_t> build_dsc_request(std::uint8_t session) {
    return std::vector<std::uint8_t>{kSidDiagnosticSessionControl, session};
}

Status parse_dsc_response(std::span<std::uint8_t const> resp, std::uint8_t expected_session) {
    if (auto r = reject_if_negative(resp, kSidDiagnosticSessionControl); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS DSC response too short");
    }
    if (resp[0] != kSidDiagnosticSessionControl + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS DSC unexpected SID");
    }
    if (resp[1] != expected_session) {
        return failure(ErrorCode::ParseError, "UDS DSC session mismatch");
    }
    // Spec: positive DSC response carries a sessionParameterRecord after the
    // sub-function (P2 timing values). We ignore it; callers that care can
    // read it from the raw response.
    return ok();
}

// ---- ECUReset ----------------------------------------------------------

std::vector<std::uint8_t> build_ecu_reset_request(std::uint8_t reset_type) {
    return std::vector<std::uint8_t>{kSidEcuReset, reset_type};
}

Status parse_ecu_reset_response(std::span<std::uint8_t const> resp,
                                std::uint8_t expected_reset_type) {
    if (auto r = reject_if_negative(resp, kSidEcuReset); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS ECUReset response too short");
    }
    if (resp[0] != kSidEcuReset + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS ECUReset unexpected SID");
    }
    if (resp[1] != expected_reset_type) {
        return failure(ErrorCode::ParseError, "UDS ECUReset type mismatch");
    }
    return ok();
}

// ---- TesterPresent -----------------------------------------------------

std::vector<std::uint8_t> build_tester_present_request(bool suppress_response) {
    return std::vector<std::uint8_t>{kSidTesterPresent,
                                     static_cast<std::uint8_t>(suppress_response ? 0x80 : 0x00)};
}

Status parse_tester_present_response(std::span<std::uint8_t const> resp) {
    if (auto r = reject_if_negative(resp, kSidTesterPresent); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS TesterPresent response too short");
    }
    if (resp[0] != kSidTesterPresent + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS TesterPresent unexpected SID");
    }
    // Sub-function in response should be 0x00 when we asked for an ack.
    return ok();
}

// ---- RequestDownload ---------------------------------------------------

namespace {

std::uint8_t bytes_to_encode(std::uint32_t v) noexcept {
    if (v <= 0xFFU)
        return 1;
    if (v <= 0xFFFFU)
        return 2;
    if (v <= 0xFFFFFFU)
        return 3;
    return 4;
}

void append_big_endian(std::vector<std::uint8_t> &out, std::uint32_t v, std::uint8_t bytes) {
    // Emit `bytes` big-endian bytes of v, dropping the high-order bytes that
    // wouldn't fit. Callers pass a `bytes` value computed via bytes_to_encode
    // so no data is lost.
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t read_big_endian(std::span<std::uint8_t const> src, std::size_t offset,
                              std::size_t bytes) noexcept {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        v = (v << 8U) | src[offset + i];
    }
    return v;
}

std::uint8_t encode_alfi(std::uint8_t size_bytes, std::uint8_t addr_bytes) noexcept {
    return static_cast<std::uint8_t>(((size_bytes & 0x0FU) << 4U) | (addr_bytes & 0x0FU));
}

} // namespace

std::vector<std::uint8_t> build_request_download(std::uint8_t data_format,
                                                 std::uint32_t memory_address,
                                                 std::uint32_t memory_size) {
    auto const addr_bytes = bytes_to_encode(memory_address);
    auto const size_bytes = bytes_to_encode(memory_size);
    auto const fmt = static_cast<std::uint8_t>(((size_bytes & 0x0FU) << 4U) | (addr_bytes & 0x0FU));

    std::vector<std::uint8_t> out;
    out.resize(3);
    out[0] = kSidRequestDownload;
    out[1] = data_format;
    out[2] = fmt;
    append_big_endian(out, memory_address, addr_bytes);
    append_big_endian(out, memory_size, size_bytes);
    return out;
}

Result<std::uint32_t> parse_request_download_response(std::span<std::uint8_t const> resp) {
    if (auto r = reject_if_negative(resp, kSidRequestDownload); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [0x74] [lengthFormatIdentifier] [maxBlockLength_be...]
    if (resp.size() < 3) {
        return failure(ErrorCode::ParseError, "UDS RequestDownload response too short");
    }
    if (resp[0] != kSidRequestDownload + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS RequestDownload unexpected SID");
    }
    auto const len_bytes = static_cast<std::size_t>((resp[1] >> 4U) & 0x0FU);
    if (len_bytes == 0 || len_bytes > 4) {
        return failure(ErrorCode::ParseError, "UDS RequestDownload bad lengthFormatIdentifier");
    }
    if (resp.size() < 2 + len_bytes) {
        return failure(ErrorCode::ParseError, "UDS RequestDownload response truncated");
    }
    std::uint32_t max_block = 0;
    for (std::size_t i = 0; i < len_bytes; ++i) {
        max_block = (max_block << 8U) | resp[2 + i];
    }
    return max_block;
}

// ---- TransferData ------------------------------------------------------

std::vector<std::uint8_t> build_transfer_data(std::uint8_t block_sequence_counter,
                                              std::span<std::uint8_t const> data) {
    // See build_wdbi_request — same GCC false-positive workaround.
    std::vector<std::uint8_t> out{kSidTransferData, block_sequence_counter};
    out.reserve(2 + data.size());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Status parse_transfer_data_response(std::span<std::uint8_t const> resp,
                                    std::uint8_t expected_counter) {
    if (auto r = reject_if_negative(resp, kSidTransferData); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS TransferData response too short");
    }
    if (resp[0] != kSidTransferData + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS TransferData unexpected SID");
    }
    if (resp[1] != expected_counter) {
        return failure(ErrorCode::ParseError, "UDS TransferData block-counter mismatch");
    }
    return ok();
}

// ---- SubaruBulkTransfer (0xB6) -----------------------------------------

std::vector<std::uint8_t>
build_subaru_bulk_transfer(std::uint32_t memory_address, std::span<std::uint8_t const> data) {
    // Wire format from cobb-reinstall-3.log: `B6 <addr_be3> <data...>`.
    // No addressAndLengthFormat byte and no block-sequence counter —
    // address width is implicit (always 3 bytes) and the address is the
    // sequence anchor that the orchestrator advances between calls.
    //
    // See build_wdbi_request — same GCC 15 false-positive workaround
    // (brace-init the fixed prefix, then reserve, then insert).
    std::vector<std::uint8_t> out{
        kSidSubaruBulkTransfer,
        static_cast<std::uint8_t>((memory_address >> 16) & 0xFFU),
        static_cast<std::uint8_t>((memory_address >> 8) & 0xFFU),
        static_cast<std::uint8_t>(memory_address & 0xFFU),
    };
    out.reserve(4 + data.size());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Status parse_subaru_bulk_transfer_response(std::span<std::uint8_t const> resp) {
    if (auto r = reject_if_negative(resp, kSidSubaruBulkTransfer); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "UDS SubaruBulkTransfer response empty");
    }
    if (resp[0] != kSidSubaruBulkTransfer + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS SubaruBulkTransfer unexpected SID");
    }
    // Positive response may carry trailing status bytes (observed `F6 00`
    // in cobb-reinstall-3.log) — accept and ignore.
    return ok();
}

// ---- RequestTransferExit -----------------------------------------------

std::vector<std::uint8_t> build_request_transfer_exit() {
    return std::vector<std::uint8_t>{kSidRequestTransferExit};
}

Status parse_request_transfer_exit_response(std::span<std::uint8_t const> resp) {
    if (auto r = reject_if_negative(resp, kSidRequestTransferExit); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "UDS RequestTransferExit response empty");
    }
    if (resp[0] != kSidRequestTransferExit + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS RequestTransferExit unexpected SID");
    }
    // Some ECUs append a CRC; we accept and ignore.
    return ok();
}

// ---- RoutineControl ----------------------------------------------------

std::vector<std::uint8_t> build_routine_control(std::uint8_t sub_function, std::uint16_t routine_id,
                                                std::span<std::uint8_t const> option_record) {
    // See build_wdbi_request — same GCC false-positive workaround.
    std::vector<std::uint8_t> out{
        kSidRoutineControl,
        sub_function,
        static_cast<std::uint8_t>((routine_id >> 8) & 0xFFU),
        static_cast<std::uint8_t>(routine_id & 0xFFU),
    };
    out.reserve(4 + option_record.size());
    out.insert(out.end(), option_record.begin(), option_record.end());
    return out;
}

Result<std::vector<std::uint8_t>>
parse_routine_control_response(std::span<std::uint8_t const> resp,
                               std::uint8_t expected_sub_function,
                               std::uint16_t expected_routine_id) {
    if (auto r = reject_if_negative(resp, kSidRoutineControl); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [0x71] [sub_function] [RID_hi] [RID_lo] [status_record...]
    if (resp.size() < 4) {
        return failure(ErrorCode::ParseError, "UDS RoutineControl response too short");
    }
    if (resp[0] != kSidRoutineControl + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS RoutineControl unexpected SID");
    }
    if (resp[1] != expected_sub_function) {
        return failure(ErrorCode::ParseError, "UDS RoutineControl sub-function mismatch");
    }
    auto const got_rid = read_be16(resp[2], resp[3]);
    if (got_rid != expected_routine_id) {
        return failure(ErrorCode::ParseError, "UDS RoutineControl RID mismatch");
    }
    return std::vector<std::uint8_t>(resp.begin() + 4, resp.end());
}

// ---- ReadMemoryByAddress -----------------------------------------------

std::vector<std::uint8_t> build_read_memory_by_address(std::uint32_t memory_address,
                                                       std::uint32_t memory_size) {
    // Subaru Hitachi ECUs (and most production ECUs) expect a fixed 4-byte
    // address + 1-byte size format (ALF = 0x14). Earlier versions of this
    // builder used the ISO 14229 dynamic encoding (minimum bytes needed),
    // which produced wire payloads like `23 11 00 10` (ALF 0x11 = 1 addr,
    // 1 size). On a stock 2017 VA WRX with FA20DIT the ECU silently drops
    // requests with non-fixed-width addressing — diagnosed 2026-05-23.
    //
    // Sizes > 255 bytes need 2 size bytes, which gives ALF = 0x24. The
    // builder falls back to that automatically.
    auto const addr_bytes = std::uint8_t{4};
    auto const size_bytes = memory_size > 0xFFU ? std::uint8_t{2} : std::uint8_t{1};

    std::vector<std::uint8_t> out;
    out.reserve(std::size_t{2} + addr_bytes + size_bytes);
    out.push_back(kSidReadMemoryByAddress);
    out.push_back(encode_alfi(size_bytes, addr_bytes));
    append_big_endian(out, memory_address, addr_bytes);
    append_big_endian(out, memory_size, size_bytes);
    return out;
}

Result<std::vector<std::uint8_t>>
parse_read_memory_by_address_response(std::span<std::uint8_t const> resp) {
    if (auto r = reject_if_negative(resp, kSidReadMemoryByAddress); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "UDS ReadMemoryByAddress response empty");
    }
    if (resp[0] != kSidReadMemoryByAddress + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS ReadMemoryByAddress unexpected SID");
    }
    return std::vector<std::uint8_t>(resp.begin() + 1, resp.end());
}

// ---- WriteMemoryByAddress ----------------------------------------------

std::vector<std::uint8_t> build_write_memory_by_address(std::uint32_t memory_address,
                                                        std::span<std::uint8_t const> data) {
    auto const memory_size = static_cast<std::uint32_t>(data.size());
    auto const addr_bytes = bytes_to_encode(memory_address);
    auto const size_bytes = bytes_to_encode(memory_size);

    std::vector<std::uint8_t> out;
    out.reserve(std::size_t{2} + addr_bytes + size_bytes + data.size());
    out.push_back(kSidWriteMemoryByAddress);
    out.push_back(encode_alfi(size_bytes, addr_bytes));
    append_big_endian(out, memory_address, addr_bytes);
    append_big_endian(out, memory_size, size_bytes);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Status parse_write_memory_by_address_response(std::span<std::uint8_t const> resp,
                                              std::uint32_t expected_address,
                                              std::uint32_t expected_size) {
    if (auto r = reject_if_negative(resp, kSidWriteMemoryByAddress); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [0x7D] [aLFI] [addr...] [size...]
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS WriteMemoryByAddress response too short");
    }
    if (resp[0] != kSidWriteMemoryByAddress + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS WriteMemoryByAddress unexpected SID");
    }
    auto const size_bytes = static_cast<std::size_t>((resp[1] >> 4U) & 0x0FU);
    auto const addr_bytes = static_cast<std::size_t>(resp[1] & 0x0FU);
    if (size_bytes == 0 || size_bytes > 4 || addr_bytes == 0 || addr_bytes > 4) {
        return failure(ErrorCode::ParseError,
                       "UDS WriteMemoryByAddress bad addressAndLengthFormat");
    }
    if (resp.size() < 2 + addr_bytes + size_bytes) {
        return failure(ErrorCode::ParseError, "UDS WriteMemoryByAddress response truncated");
    }
    auto const got_addr = read_big_endian(resp, 2, addr_bytes);
    auto const got_size = read_big_endian(resp, 2 + addr_bytes, size_bytes);
    if (got_addr != expected_address) {
        return failure(ErrorCode::ParseError, "UDS WriteMemoryByAddress address echo mismatch");
    }
    if (got_size != expected_size) {
        return failure(ErrorCode::ParseError, "UDS WriteMemoryByAddress size echo mismatch");
    }
    return ok();
}

// ---- CommunicationControl ----------------------------------------------

std::vector<std::uint8_t> build_communication_control(std::uint8_t control_type,
                                                      std::uint8_t communication_type) {
    return std::vector<std::uint8_t>{kSidCommunicationControl, control_type, communication_type};
}

Status parse_communication_control_response(std::span<std::uint8_t const> resp,
                                            std::uint8_t expected_control_type) {
    if (auto r = reject_if_negative(resp, kSidCommunicationControl); !r.has_value()) {
        return failure(r.error());
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "UDS CommunicationControl response too short");
    }
    if (resp[0] != kSidCommunicationControl + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected, "UDS CommunicationControl unexpected SID");
    }
    if (resp[1] != expected_control_type) {
        return failure(ErrorCode::ParseError, "UDS CommunicationControl controlType mismatch");
    }
    return ok();
}

// ---- UdsClient ----------------------------------------------------------

Result<std::vector<std::uint8_t>>
UdsClient::read_data_by_identifier(std::uint16_t did, std::chrono::milliseconds timeout) {
    auto const req = build_rdbi_request(did);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_rdbi_response(resp->data, did);
}

Status UdsClient::write_data_by_identifier(std::uint16_t did, std::span<std::uint8_t const> data,
                                           std::chrono::milliseconds timeout) {
    auto const req = build_wdbi_request(did, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_wdbi_response(resp->data, did);
}

Result<std::vector<std::uint8_t>>
UdsClient::security_access_request_seed(std::uint8_t sub_function,
                                        std::chrono::milliseconds timeout) {
    auto const req = build_security_access_request_seed(sub_function);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_security_access_seed(resp->data, sub_function);
}

Status UdsClient::security_access_send_key(std::uint8_t sub_function,
                                           std::span<std::uint8_t const> key,
                                           std::chrono::milliseconds timeout) {
    auto const req = build_security_access_send_key(sub_function, key);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_security_access_key_ack(resp->data, sub_function);
}

Status UdsClient::diagnostic_session_control(std::uint8_t session,
                                             std::chrono::milliseconds timeout) {
    auto const req = build_dsc_request(session);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_dsc_response(resp->data, session);
}

Status UdsClient::ecu_reset(std::uint8_t reset_type, std::chrono::milliseconds timeout) {
    auto const req = build_ecu_reset_request(reset_type);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_ecu_reset_response(resp->data, reset_type);
}

Status UdsClient::tester_present(std::chrono::milliseconds timeout) {
    auto const req = build_tester_present_request(/*suppress=*/false);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_tester_present_response(resp->data);
}

Result<std::uint32_t> UdsClient::request_download(std::uint8_t data_format,
                                                  std::uint32_t memory_address,
                                                  std::uint32_t memory_size,
                                                  std::chrono::milliseconds timeout) {
    auto const req = build_request_download(data_format, memory_address, memory_size);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_request_download_response(resp->data);
}

Status UdsClient::transfer_data(std::uint8_t block_sequence_counter,
                                std::span<std::uint8_t const> data,
                                std::chrono::milliseconds timeout) {
    auto const req = build_transfer_data(block_sequence_counter, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_transfer_data_response(resp->data, block_sequence_counter);
}

Status UdsClient::subaru_bulk_transfer(std::uint32_t memory_address,
                                       std::span<std::uint8_t const> data,
                                       std::chrono::milliseconds timeout) {
    auto const req = build_subaru_bulk_transfer(memory_address, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_subaru_bulk_transfer_response(resp->data);
}

Status UdsClient::request_transfer_exit(std::chrono::milliseconds timeout) {
    auto const req = build_request_transfer_exit();
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_request_transfer_exit_response(resp->data);
}

Result<std::vector<std::uint8_t>>
UdsClient::routine_control(std::uint8_t sub_function, std::uint16_t routine_id,
                           std::span<std::uint8_t const> option_record,
                           std::chrono::milliseconds timeout) {
    auto const req = build_routine_control(sub_function, routine_id, option_record);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_routine_control_response(resp->data, sub_function, routine_id);
}

Result<std::vector<std::uint8_t>>
UdsClient::read_memory_by_address(std::uint32_t memory_address, std::uint32_t memory_size,
                                  std::chrono::milliseconds timeout) {
    auto const req = build_read_memory_by_address(memory_address, memory_size);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_read_memory_by_address_response(resp->data);
}

Status UdsClient::write_memory_by_address(std::uint32_t memory_address,
                                          std::span<std::uint8_t const> data,
                                          std::chrono::milliseconds timeout) {
    auto const req = build_write_memory_by_address(memory_address, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_write_memory_by_address_response(resp->data, memory_address,
                                                  static_cast<std::uint32_t>(data.size()));
}

Status UdsClient::communication_control(std::uint8_t control_type, std::uint8_t communication_type,
                                        std::chrono::milliseconds timeout) {
    auto const req = build_communication_control(control_type, communication_type);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_communication_control_response(resp->data, control_type);
}

} // namespace st::ecu::uds
