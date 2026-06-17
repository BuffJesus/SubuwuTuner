// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/subaru_dit.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace st::ecu::subaru_dit {

namespace {

// Bench capture from LF79002P (FA20DIT SH-2A 2 MB) donor ECU,
// 2026-06-16. SHA of donor ROM:
//   5fb404e70beb912224e8b141be8cc2be20cda1965d129002305aef5c116d0d11
// Wire sequence sent: AA (single byte). Wire sequence received:
// EA <104 B>. The 104 B body is stored here. This is the SSM-extension
// info handler reply (NOT AtlasFlashInformation per round-9 spec).
constexpr std::array<std::uint8_t, kSsmExtensionInfoBodyBytes>
    kReferenceBodyLf79002p = {
        0xA3, 0x10, 0x0F, 0xB0, 0x29, 0xB0, 0x40, 0x07, 0x41, 0x80, 0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

} // namespace

std::span<std::uint8_t const> reference_ssm_extension_info_body_lf79002p() noexcept {
    return {kReferenceBodyLf79002p.data(), kReferenceBodyLf79002p.size()};
}

std::string SsmExtensionInfoResponse::hex_dump() const {
    std::string out;
    out.reserve(raw.size() * 3U);
    char buf[4];
    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", raw[i]);
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(buf[0]);
        out.push_back(buf[1]);
    }
    return out;
}

bool SsmExtensionInfoResponse::matches_reference_lf79002p() const noexcept {
    if (raw.size() != kReferenceBodyLf79002p.size()) {
        return false;
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != kReferenceBodyLf79002p[i]) {
            return false;
        }
    }
    return true;
}

std::array<std::uint8_t, kDeviceIdPrefixBytes>
SsmExtensionInfoResponse::device_id_prefix() const noexcept {
    std::array<std::uint8_t, kDeviceIdPrefixBytes> out{};
    std::size_t const n = std::min(raw.size(), kDeviceIdPrefixBytes);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = raw[i];
    }
    return out;
}

bool SsmExtensionInfoResponse::has_lf79002p_primary_prefix() const noexcept {
    if (raw.size() < kDeviceIdPrefixBytes) {
        return false;
    }
    for (std::size_t i = 0; i < kDeviceIdPrefixBytes; ++i) {
        if (raw[i] != kDeviceIdPrefixLf79002pPrimary[i]) {
            return false;
        }
    }
    return true;
}

Result<SsmExtensionInfoResponse>
parse_ssm_extension_info_body(std::span<std::uint8_t const> body) {
    if (body.size() != kSsmExtensionInfoBodyBytes) {
        return failure(ErrorCode::InvalidArgument,
                       "ssm-extension info body must be 104 B");
    }

    SsmExtensionInfoResponse r;
    r.raw.assign(body.begin(), body.end());

    r.status_flag = body[0];
    r.field_a = static_cast<std::uint16_t>((body[1] << 8) | body[2]);
    r.field_b = static_cast<std::uint32_t>(body[3]) << 24 |
                static_cast<std::uint32_t>(body[4]) << 16 |
                static_cast<std::uint32_t>(body[5]) << 8 |
                static_cast<std::uint32_t>(body[6]);
    r.field_c = body[7];
    r.field_d = body[8];
    r.marker_pair_0[0] = body[9];
    r.marker_pair_0[1] = body[10];
    r.marker_offset_40 = body[40];
    r.marker_offset_48 = body[48];
    r.marker_offset_55 = body[55];

    return r;
}

Result<SsmExtensionInfoResponse>
parse_ssm_extension_info_frame(std::span<std::uint8_t const> frame) {
    if (frame.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "ssm-extension info frame is empty");
    }

    if (frame[0] == 0x7FU) {
        std::uint8_t nrc = frame.size() >= 3 ? frame[2] : 0U;
        char msg[64];
        std::snprintf(msg, sizeof(msg),
                      "ssm-extension info NRC 0x%02X", nrc);
        return failure(ErrorCode::EcuRejected, msg);
    }

    if (frame[0] != kAckSsmExtensionInfo) {
        char msg[80];
        std::snprintf(msg, sizeof(msg),
                      "ssm-extension info expected ACK 0xEA, got 0x%02X",
                      frame[0]);
        return failure(ErrorCode::InvalidArgument, msg);
    }

    if (frame.size() != kSsmExtensionInfoBodyBytes + 1U) {
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "ssm-extension info frame must be 105 B (1 ACK + 104), got %zu",
                      frame.size());
        return failure(ErrorCode::InvalidArgument, msg);
    }

    return parse_ssm_extension_info_body(frame.subspan(1));
}

// ---------------------------------------------------------------------
// DIT block-flash encoders
// ---------------------------------------------------------------------

namespace {

// Common NRC / ACK check at the head of every positive-response
// decoder. Returns the body (response bytes after the SID echo) on
// success.
Result<std::span<std::uint8_t const>>
verify_positive_ack(std::span<std::uint8_t const> resp, std::uint8_t request_sid) {
    if (resp.empty()) {
        return failure(ErrorCode::InvalidArgument, "DIT response empty");
    }
    if (resp[0] == kNegativeResponseSid) {
        std::uint8_t nrc = resp.size() >= 3 ? resp[2] : 0;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "DIT NRC 0x%02X on SID 0x%02X", nrc, request_sid);
        return failure(ErrorCode::EcuRejected, msg);
    }
    auto const expected_ack = static_cast<std::uint8_t>(request_sid | kPositiveResponseOffset);
    if (resp[0] != expected_ack) {
        char msg[80];
        std::snprintf(msg, sizeof(msg),
                      "DIT expected ACK 0x%02X (SID 0x%02X | 0x40), got 0x%02X",
                      expected_ack, request_sid, resp[0]);
        return failure(ErrorCode::InvalidArgument, msg);
    }
    return resp.subspan(1);
}

} // namespace

// Encoders use assign+insert (rather than reserve+push_back) to work
// around a GCC 15 -Wfree-nonheap-object false positive in small-vector
// growth paths. The behavior is identical; the form just doesn't trip
// the heuristic.

std::vector<std::uint8_t> build_flash_information_request() {
    return {kSidDitFlashInformation};
}

std::vector<std::uint8_t> build_block_information_request(std::uint32_t address) {
    return {
        kSidDitBlockInformation,
        static_cast<std::uint8_t>((address >> 24) & 0xFF),
        static_cast<std::uint8_t>((address >> 16) & 0xFF),
        static_cast<std::uint8_t>((address >> 8) & 0xFF),
        static_cast<std::uint8_t>(address & 0xFF),
    };
}

std::vector<std::uint8_t> build_read_request(std::uint32_t address, std::uint16_t length) {
    return {
        kSidDitRead,
        static_cast<std::uint8_t>((address >> 24) & 0xFF),
        static_cast<std::uint8_t>((address >> 16) & 0xFF),
        static_cast<std::uint8_t>((address >> 8) & 0xFF),
        static_cast<std::uint8_t>(address & 0xFF),
        static_cast<std::uint8_t>((length >> 8) & 0xFF),
        static_cast<std::uint8_t>(length & 0xFF),
    };
}

std::vector<std::uint8_t> build_clear_request(std::uint32_t address) {
    return {
        kSidDitClear,
        static_cast<std::uint8_t>((address >> 24) & 0xFF),
        static_cast<std::uint8_t>((address >> 16) & 0xFF),
        static_cast<std::uint8_t>((address >> 8) & 0xFF),
        static_cast<std::uint8_t>(address & 0xFF),
    };
}

std::vector<std::uint8_t>
build_program_request(std::uint32_t address, std::span<std::uint8_t const> data) {
    std::vector<std::uint8_t> out{
        kSidDitProgram,
        static_cast<std::uint8_t>((address >> 24) & 0xFF),
        static_cast<std::uint8_t>((address >> 16) & 0xFF),
        static_cast<std::uint8_t>((address >> 8) & 0xFF),
        static_cast<std::uint8_t>(address & 0xFF),
    };
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t>
build_control_request(std::uint8_t sub_command, std::span<std::uint8_t const> data) {
    std::vector<std::uint8_t> out{kSidDitControl, sub_command};
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// ---------------------------------------------------------------------
// DIT block-flash decoders
// ---------------------------------------------------------------------

Result<std::vector<std::uint8_t>>
parse_flash_information_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitFlashInformation);
    if (!body.has_value()) {
        return failure(body.error());
    }
    return std::vector<std::uint8_t>(body->begin(), body->end());
}

Result<DitBlockInformation>
parse_block_information_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitBlockInformation);
    if (!body.has_value()) {
        return failure(body.error());
    }
    if (body->size() < 14U) {
        char msg[80];
        std::snprintf(msg, sizeof(msg),
                      "DIT BlockInformation body too short: %zu B (need >=14)",
                      body->size());
        return failure(ErrorCode::InvalidArgument, msg);
    }
    DitBlockInformation info;
    info.raw.assign(body->begin(), body->end());
    info.base_address = static_cast<std::uint32_t>((*body)[0]) << 24 |
                        static_cast<std::uint32_t>((*body)[1]) << 16 |
                        static_cast<std::uint32_t>((*body)[2]) << 8 |
                        static_cast<std::uint32_t>((*body)[3]);
    info.end_address = static_cast<std::uint32_t>((*body)[4]) << 24 |
                       static_cast<std::uint32_t>((*body)[5]) << 16 |
                       static_cast<std::uint32_t>((*body)[6]) << 8 |
                       static_cast<std::uint32_t>((*body)[7]);
    info.block_kind = static_cast<std::uint16_t>(((*body)[8] << 8) | (*body)[9]);
    info.reserved = static_cast<std::uint32_t>((*body)[10]) << 24 |
                    static_cast<std::uint32_t>((*body)[11]) << 16 |
                    static_cast<std::uint32_t>((*body)[12]) << 8 |
                    static_cast<std::uint32_t>((*body)[13]);
    return info;
}

Result<std::vector<std::uint8_t>>
parse_read_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitRead);
    if (!body.has_value()) {
        return failure(body.error());
    }
    return std::vector<std::uint8_t>(body->begin(), body->end());
}

Result<void> parse_clear_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitClear);
    if (!body.has_value()) {
        return failure(body.error());
    }
    return {};
}

Result<void> parse_program_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitProgram);
    if (!body.has_value()) {
        return failure(body.error());
    }
    return {};
}

Result<std::vector<std::uint8_t>>
parse_control_response(std::span<std::uint8_t const> resp) {
    auto body = verify_positive_ack(resp, kSidDitControl);
    if (!body.has_value()) {
        return failure(body.error());
    }
    return std::vector<std::uint8_t>(body->begin(), body->end());
}

// ---------------------------------------------------------------------
// DitFlashClient
// ---------------------------------------------------------------------

Result<std::vector<std::uint8_t>>
DitFlashClient::flash_information(std::chrono::milliseconds timeout) {
    auto const req = build_flash_information_request();
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_flash_information_response(resp->data);
}

Result<DitBlockInformation>
DitFlashClient::block_information(std::uint32_t address,
                                  std::chrono::milliseconds timeout) {
    auto const req = build_block_information_request(address);
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_block_information_response(resp->data);
}

Result<std::vector<std::uint8_t>>
DitFlashClient::read(std::uint32_t address, std::uint16_t length,
                     std::chrono::milliseconds timeout) {
    auto const req = build_read_request(address, length);
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_read_response(resp->data);
}

Result<void>
DitFlashClient::clear(std::uint32_t address, std::chrono::milliseconds timeout) {
    auto const req = build_clear_request(address);
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_clear_response(resp->data);
}

Result<void>
DitFlashClient::program(std::uint32_t address, std::span<std::uint8_t const> data,
                        std::chrono::milliseconds timeout) {
    auto const req = build_program_request(address, data);
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_program_response(resp->data);
}

Result<std::vector<std::uint8_t>>
DitFlashClient::control(std::uint8_t sub_command, std::span<std::uint8_t const> data,
                        std::chrono::milliseconds timeout) {
    auto const req = build_control_request(sub_command, data);
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value()) {
        return failure(resp.error());
    }
    return parse_control_response(resp->data);
}

} // namespace st::ecu::subaru_dit
