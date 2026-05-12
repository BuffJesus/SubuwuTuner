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

} // namespace st::ecu::uds
