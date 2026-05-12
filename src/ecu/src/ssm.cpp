// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/ssm.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::ssm {

std::uint8_t ssm_checksum(std::span<std::uint8_t const> bytes) noexcept {
    unsigned sum = 0;
    for (auto const b : bytes) {
        sum += b;
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

Result<std::vector<std::uint8_t>> build_a8_request(std::span<std::uint32_t const> addresses) {
    if (addresses.empty()) {
        return failure(ErrorCode::InvalidArgument, "SSM read needs at least one address");
    }
    for (auto const a : addresses) {
        if (a > kMaxAddress) {
            return failure(ErrorCode::InvalidArgument,
                           "SSM address exceeds 24-bit range: 0x"
                               + std::to_string(static_cast<unsigned long>(a)));
        }
    }
    // Payload length = CMD(1) + PAD(1) + 3*N. LEN is 1 byte → max 255.
    std::size_t const payload_len = 2 + 3 * addresses.size();
    if (payload_len > 0xFFU) {
        return failure(ErrorCode::InvalidArgument,
                       "SSM request too large for single-frame LEN byte");
    }

    std::vector<std::uint8_t> out;
    out.reserve(4 + payload_len + 1); // header(1)+dst(1)+src(1)+len(1)+payload+csum(1)
    out.push_back(kHeader);
    out.push_back(kDestEcu);
    out.push_back(kSrcTool);
    out.push_back(static_cast<std::uint8_t>(payload_len));
    out.push_back(kCmdReadByAddress);
    out.push_back(kPadByte);
    for (auto const a : addresses) {
        out.push_back(static_cast<std::uint8_t>((a >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((a >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(a & 0xFFU));
    }
    out.push_back(ssm_checksum(out));
    return out;
}

Result<std::vector<std::uint8_t>> parse_a8_response(std::span<std::uint8_t const> resp,
                                                    std::size_t expected_n) {
    // Minimum response: header + src + dst + len + rsp + csum = 6 bytes for zero data.
    if (resp.size() < 6) {
        return failure(ErrorCode::ParseError, "SSM response too short");
    }
    if (resp[0] != kHeader) {
        return failure(ErrorCode::ParseError, "SSM response: bad header");
    }
    if (resp[1] != kSrcTool || resp[2] != kDestEcu) {
        return failure(ErrorCode::ParseError, "SSM response: bad addressing");
    }

    std::size_t const declared_len = resp[3];
    // Total frame = header(1) + dst(1) + src(1) + len(1) + payload(declared_len) + csum(1).
    std::size_t const expected_frame = 4 + declared_len + 1;
    if (resp.size() != expected_frame) {
        return failure(ErrorCode::ParseError,
                       "SSM response: declared LEN ("
                           + std::to_string(declared_len)
                           + ") does not match frame size ("
                           + std::to_string(resp.size()) + ")");
    }

    // Verify checksum over [header .. last-payload-byte].
    auto const csum_computed = ssm_checksum(resp.subspan(0, resp.size() - 1));
    auto const csum_given    = resp.back();
    if (csum_computed != csum_given) {
        return failure(ErrorCode::BadChecksum,
                       "SSM response: checksum 0x"
                           + std::to_string(static_cast<unsigned>(csum_given))
                           + " != computed 0x"
                           + std::to_string(static_cast<unsigned>(csum_computed)));
    }

    auto const rsp_byte = resp[4];
    if (rsp_byte == kNegativeResponse) {
        std::uint8_t const nrc = declared_len >= 2 ? resp[5] : 0xFFU;
        return failure(ErrorCode::EcuRejected,
                       "SSM negative response, NRC=0x"
                           + std::to_string(static_cast<unsigned>(nrc)));
    }
    if (rsp_byte != kRespReadByAddress) {
        return failure(ErrorCode::EcuRejected,
                       "SSM unexpected response byte: 0x"
                           + std::to_string(static_cast<unsigned>(rsp_byte)));
    }

    // Data bytes = declared_len - 1 (the RSP byte).
    std::size_t const data_n = declared_len - 1;
    if (data_n != expected_n) {
        return failure(ErrorCode::ParseError,
                       "SSM response: expected " + std::to_string(expected_n)
                           + " data bytes, got " + std::to_string(data_n));
    }
    std::vector<std::uint8_t> data(
        resp.begin() + 5,
        resp.begin() + 5 + static_cast<std::ptrdiff_t>(data_n));
    return data;
}

Result<std::vector<std::uint8_t>> SsmClient::read(std::span<std::uint32_t const> addresses,
                                                  std::chrono::milliseconds       timeout) {
    auto req = build_a8_request(addresses);
    if (!req.has_value()) return failure(req.error());

    auto resp = transport_->send_recv(*req, timeout);
    if (!resp.has_value()) return failure(resp.error());

    return parse_a8_response(resp->data, addresses.size());
}

Result<std::vector<std::uint8_t>> SsmClient::read_block(std::uint32_t base_address,
                                                        std::size_t   length,
                                                        std::chrono::milliseconds timeout) {
    if (length == 0) {
        return std::vector<std::uint8_t>{};
    }
    if (base_address > kMaxAddress || base_address + length - 1 > kMaxAddress) {
        return failure(ErrorCode::InvalidArgument, "SSM block range exceeds 24-bit");
    }
    std::vector<std::uint32_t> addrs;
    addrs.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        addrs.push_back(base_address + static_cast<std::uint32_t>(i));
    }
    return read(addrs, timeout);
}

} // namespace st::ecu::ssm
