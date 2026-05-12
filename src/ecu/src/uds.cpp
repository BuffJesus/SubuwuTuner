// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/uds.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::uds {

namespace {

// Surface a negative-response frame (0x7F SID NRC) as EcuRejected. Returns
// ok() when `resp` is not a negative response — caller continues parsing.
[[nodiscard]] Status reject_if_negative(std::span<std::uint8_t const> resp,
                                         std::uint8_t                  expected_sid) {
    if (resp.size() >= 3 && resp[0] == kNegativeResponse) {
        if (resp[1] != expected_sid) {
            return failure(ErrorCode::ParseError,
                           "UDS negative response for wrong SID: 0x"
                               + std::to_string(static_cast<unsigned>(resp[1])));
        }
        return failure(ErrorCode::EcuRejected,
                       "UDS NRC=0x"
                           + std::to_string(static_cast<unsigned>(resp[2])));
    }
    return ok();
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
                                                      std::uint16_t                 expected_did) {
    if (auto r = reject_if_negative(resp, kSidReadDataByIdentifier); !r.has_value()) {
        return failure(r.error());
    }
    // Positive response: [62] [did_hi] [did_lo] [data...]
    if (resp.size() < 3) {
        return failure(ErrorCode::ParseError, "UDS RDBI response too short");
    }
    if (resp[0] != kSidReadDataByIdentifier + kPositiveResponseOffset) {
        return failure(ErrorCode::EcuRejected,
                       "UDS RDBI unexpected SID: 0x"
                           + std::to_string(static_cast<unsigned>(resp[0])));
    }
    auto const got_did = read_be16(resp[1], resp[2]);
    if (got_did != expected_did) {
        return failure(ErrorCode::ParseError,
                       "UDS RDBI DID mismatch: expected 0x"
                           + std::to_string(static_cast<unsigned>(expected_did))
                           + " got 0x"
                           + std::to_string(static_cast<unsigned>(got_did)));
    }
    return std::vector<std::uint8_t>(resp.begin() + 3, resp.end());
}

// ---- WDBI ---------------------------------------------------------------

std::vector<std::uint8_t> build_wdbi_request(std::uint16_t                 did,
                                              std::span<std::uint8_t const> data) {
    // Size-initialised + indexed writes avoid a GCC 15 -O3 false positive
    // for -Wfree-nonheap-object on the reserve+push_back+insert pattern.
    std::vector<std::uint8_t> out(3 + data.size());
    out[0] = kSidWriteDataByIdentifier;
    out[1] = static_cast<std::uint8_t>((did >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>(did & 0xFFU);
    std::copy(data.begin(), data.end(), out.begin() + 3);
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
        return failure(ErrorCode::EcuRejected,
                       "UDS WDBI unexpected SID: 0x"
                           + std::to_string(static_cast<unsigned>(resp[0])));
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

Result<std::vector<std::uint8_t>> parse_security_access_seed(
    std::span<std::uint8_t const> resp, std::uint8_t expected_sub_function) {
    if (auto r = reject_if_negative(resp, kSidSecurityAccess); !r.has_value()) {
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

std::vector<std::uint8_t> build_security_access_send_key(std::uint8_t                  sub_function,
                                                          std::span<std::uint8_t const> key) {
    // See build_wdbi_request — same GCC -O3 workaround.
    std::vector<std::uint8_t> out(2 + key.size());
    out[0] = kSidSecurityAccess;
    out[1] = sub_function;
    std::copy(key.begin(), key.end(), out.begin() + 2);
    return out;
}

Status parse_security_access_key_ack(std::span<std::uint8_t const> resp,
                                      std::uint8_t                   expected_sub_function) {
    if (auto r = reject_if_negative(resp, kSidSecurityAccess); !r.has_value()) {
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

Status parse_dsc_response(std::span<std::uint8_t const> resp,
                          std::uint8_t                  expected_session) {
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
                                std::uint8_t                  expected_reset_type) {
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
    if (v <= 0xFFU)         return 1;
    if (v <= 0xFFFFU)       return 2;
    if (v <= 0xFFFFFFU)     return 3;
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

} // namespace

std::vector<std::uint8_t> build_request_download(std::uint8_t                  data_format,
                                                  std::uint32_t                 memory_address,
                                                  std::uint32_t                 memory_size) {
    auto const addr_bytes = bytes_to_encode(memory_address);
    auto const size_bytes = bytes_to_encode(memory_size);
    auto const fmt        = static_cast<std::uint8_t>(
        ((size_bytes & 0x0FU) << 4U) | (addr_bytes & 0x0FU));

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
        return failure(ErrorCode::ParseError,
                       "UDS RequestDownload bad lengthFormatIdentifier");
    }
    if (resp.size() < 2 + len_bytes) {
        return failure(ErrorCode::ParseError,
                       "UDS RequestDownload response truncated");
    }
    std::uint32_t max_block = 0;
    for (std::size_t i = 0; i < len_bytes; ++i) {
        max_block = (max_block << 8U) | resp[2 + i];
    }
    return max_block;
}

// ---- TransferData ------------------------------------------------------

std::vector<std::uint8_t> build_transfer_data(std::uint8_t                  block_sequence_counter,
                                               std::span<std::uint8_t const> data) {
    std::vector<std::uint8_t> out(2 + data.size());
    out[0] = kSidTransferData;
    out[1] = block_sequence_counter;
    std::copy(data.begin(), data.end(), out.begin() + 2);
    return out;
}

Status parse_transfer_data_response(std::span<std::uint8_t const> resp,
                                     std::uint8_t                   expected_counter) {
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
        return failure(ErrorCode::ParseError,
                       "UDS TransferData block-counter mismatch");
    }
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
        return failure(ErrorCode::EcuRejected,
                       "UDS RequestTransferExit unexpected SID");
    }
    // Some ECUs append a CRC; we accept and ignore.
    return ok();
}

// ---- UdsClient ----------------------------------------------------------

Result<std::vector<std::uint8_t>> UdsClient::read_data_by_identifier(
    std::uint16_t did, std::chrono::milliseconds timeout) {
    auto const req  = build_rdbi_request(did);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_rdbi_response(resp->data, did);
}

Status UdsClient::write_data_by_identifier(std::uint16_t did, std::span<std::uint8_t const> data,
                                            std::chrono::milliseconds timeout) {
    auto const req  = build_wdbi_request(did, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_wdbi_response(resp->data, did);
}

Result<std::vector<std::uint8_t>> UdsClient::security_access_request_seed(
    std::uint8_t sub_function, std::chrono::milliseconds timeout) {
    auto const req  = build_security_access_request_seed(sub_function);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_security_access_seed(resp->data, sub_function);
}

Status UdsClient::security_access_send_key(std::uint8_t                  sub_function,
                                            std::span<std::uint8_t const> key,
                                            std::chrono::milliseconds     timeout) {
    auto const req  = build_security_access_send_key(sub_function, key);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_security_access_key_ack(resp->data, sub_function);
}

Status UdsClient::diagnostic_session_control(std::uint8_t              session,
                                              std::chrono::milliseconds timeout) {
    auto const req  = build_dsc_request(session);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_dsc_response(resp->data, session);
}

Status UdsClient::ecu_reset(std::uint8_t reset_type, std::chrono::milliseconds timeout) {
    auto const req  = build_ecu_reset_request(reset_type);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_ecu_reset_response(resp->data, reset_type);
}

Status UdsClient::tester_present(std::chrono::milliseconds timeout) {
    auto const req  = build_tester_present_request(/*suppress=*/false);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_tester_present_response(resp->data);
}

Result<std::uint32_t> UdsClient::request_download(std::uint8_t              data_format,
                                                   std::uint32_t             memory_address,
                                                   std::uint32_t             memory_size,
                                                   std::chrono::milliseconds timeout) {
    auto const req  = build_request_download(data_format, memory_address, memory_size);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_request_download_response(resp->data);
}

Status UdsClient::transfer_data(std::uint8_t                  block_sequence_counter,
                                 std::span<std::uint8_t const> data,
                                 std::chrono::milliseconds     timeout) {
    auto const req  = build_transfer_data(block_sequence_counter, data);
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_transfer_data_response(resp->data, block_sequence_counter);
}

Status UdsClient::request_transfer_exit(std::chrono::milliseconds timeout) {
    auto const req  = build_request_transfer_exit();
    auto const resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) return failure(resp.error());
    return parse_request_transfer_exit_response(resp->data);
}

} // namespace st::ecu::uds
