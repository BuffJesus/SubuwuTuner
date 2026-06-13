// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/ssm.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
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

Result<std::vector<std::uint8_t>> build_a8_request(std::span<std::uint32_t const> addresses,
                                                   Framing framing) {
    if (addresses.empty()) {
        return failure(ErrorCode::InvalidArgument, "SSM read needs at least one address");
    }
    auto const hex24 = [](unsigned long v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%06lX", v & 0xFFFFFFUL);
        return std::string{buf};
    };
    for (auto const a : addresses) {
        if (a > kMaxAddress) {
            // Render the address as hex — std::to_string emits decimal,
            // which made the bare-minimum overflow input 0x01000000 print
            // as "0x16777216" (same bug-class as the SSM/UDS hex_byte
            // fixes in 011bc5c).
            return failure(ErrorCode::InvalidArgument,
                           "SSM address exceeds 24-bit range (max " +
                               hex24(static_cast<unsigned long>(kMaxAddress)) + "): " +
                               hex24(static_cast<unsigned long>(a)));
        }
    }
    // Payload length = CMD(1) + PAD(1) + 3*N.
    std::size_t const payload_len = 2 + 3 * addresses.size();

    if (framing == Framing::KLine) {
        // K-Line LEN is a single byte → max 255.
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

    // IsoTp: just the SSM logical payload — CMD + PAD + 3*N. ISO-TP carries
    // length in its PCI field; CAN ID identifies addressing; CSUM is
    // redundant against CAN's own CRC. No reserve() — GCC 15's allocator
    // analysis trips a false-positive -Werror=free-nonheap-object when the
    // reserve size is a pure function of the addresses span. Trading a
    // potential reallocation for cleanliness is fine on these small vectors.
    std::vector<std::uint8_t> out;
    out.push_back(kCmdReadByAddress);
    out.push_back(kPadByte);
    for (auto const a : addresses) {
        out.push_back(static_cast<std::uint8_t>((a >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((a >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(a & 0xFFU));
    }
    return out;
}

namespace {
// Format a single byte as two-digit uppercase hex. Used by the SSM
// diagnostic messages — `"0x" + std::to_string(byte)` would print the
// byte as decimal (e.g. 0xCA → "0x202"), which was the prior bug.
std::string hex_byte(unsigned b) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%02X", b & 0xFFU);
    return std::string{buf};
}
} // namespace

Result<std::vector<std::uint8_t>> parse_a8_response(std::span<std::uint8_t const> resp,
                                                    std::size_t expected_n, Framing framing) {
    if (framing == Framing::KLine) {
        // Minimum response: header + src + dst + len + rsp + csum = 6 bytes for zero data.
        if (resp.size() < 6) {
            return failure(ErrorCode::ParseError, "SSM response too short: got " +
                                                      std::to_string(resp.size()) +
                                                      " bytes, need ≥6");
        }
        if (resp[0] != kHeader) {
            return failure(ErrorCode::ParseError, "SSM response: bad header byte 0x" +
                                                      hex_byte(resp[0]) + " (expected 0x" +
                                                      hex_byte(kHeader) + ")");
        }
        if (resp[1] != kSrcTool || resp[2] != kDestEcu) {
            return failure(ErrorCode::ParseError, "SSM response: bad addressing: src=0x" +
                                                      hex_byte(resp[1]) + " dst=0x" +
                                                      hex_byte(resp[2]) + " (expected src=0x" +
                                                      hex_byte(kSrcTool) + " dst=0x" +
                                                      hex_byte(kDestEcu) + ")");
        }

        std::size_t const declared_len = resp[3];
        // Total frame = header(1) + dst(1) + src(1) + len(1) + payload(declared_len) + csum(1).
        std::size_t const expected_frame = 4 + declared_len + 1;
        if (resp.size() != expected_frame) {
            return failure(ErrorCode::ParseError,
                           "SSM response: declared LEN (" + std::to_string(declared_len) +
                               ") does not match frame size (" + std::to_string(resp.size()) +
                               ")");
        }

        // Verify checksum over [header .. last-payload-byte]. The prior
        // formatter rendered both bytes as decimal despite the "0x"
        // prefix — fixed via hex_byte().
        auto const csum_computed = ssm_checksum(resp.subspan(0, resp.size() - 1));
        auto const csum_given = resp.back();
        if (csum_computed != csum_given) {
            return failure(ErrorCode::BadChecksum, "SSM response: checksum 0x" +
                                                       hex_byte(csum_given) + " != computed 0x" +
                                                       hex_byte(csum_computed));
        }

        auto const rsp_byte = resp[4];
        if (rsp_byte == kNegativeResponse) {
            std::uint8_t const nrc = declared_len >= 2 ? resp[5] : 0xFFU;
            return failure(ErrorCode::EcuRejected,
                           "SSM negative response, NRC=0x" + hex_byte(nrc));
        }
        if (rsp_byte != kRespReadByAddress) {
            return failure(ErrorCode::EcuRejected,
                           "SSM unexpected response byte: 0x" + hex_byte(rsp_byte) +
                               " (expected 0x" + hex_byte(kRespReadByAddress) + ")");
        }

        // Data bytes = declared_len - 1 (the RSP byte).
        std::size_t const data_n = declared_len - 1;
        if (data_n != expected_n) {
            return failure(ErrorCode::ParseError,
                           "SSM response: expected " + std::to_string(expected_n) +
                               " data bytes, got " + std::to_string(data_n));
        }
        std::vector<std::uint8_t> data(resp.begin() + 5,
                                       resp.begin() + 5 + static_cast<std::ptrdiff_t>(data_n));
        return data;
    }

    // IsoTp: response is [RSP, data_0, ..., data_{N-1}] (positive) or
    // [0x7F, NRC] (negative). No wrapper, no checksum — ISO-TP / CAN
    // already provide framing + integrity.
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "SSM/CAN response empty");
    }
    auto const rsp_byte = resp[0];
    if (rsp_byte == kNegativeResponse) {
        std::uint8_t const nrc = resp.size() >= 2 ? resp[1] : 0xFFU;
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN negative response, NRC=0x" + hex_byte(nrc));
    }
    if (rsp_byte != kRespReadByAddress) {
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN unexpected response byte: 0x" + hex_byte(rsp_byte) +
                           " (expected 0x" + hex_byte(kRespReadByAddress) + ")");
    }
    std::size_t const data_n = resp.size() - 1;
    if (data_n != expected_n) {
        return failure(ErrorCode::ParseError, "SSM/CAN response: expected " +
                                                  std::to_string(expected_n) +
                                                  " data bytes, got " + std::to_string(data_n));
    }
    return std::vector<std::uint8_t>(resp.begin() + 1, resp.end());
}

Result<std::vector<std::uint8_t>> build_b0_request(std::uint32_t address, std::uint8_t data,
                                                   Framing framing) {
    if (address > kMaxAddress) {
        return failure(ErrorCode::InvalidArgument, "SSM write address exceeds 24-bit range");
    }
    if (framing == Framing::KLine) {
        // LEN = CMD(1) + 3*addr + 1*data = 5
        std::vector<std::uint8_t> out;
        out.reserve(9 + 1);
        out.push_back(kHeader);
        out.push_back(kDestEcu);
        out.push_back(kSrcTool);
        out.push_back(5);
        out.push_back(kCmdWriteByAddress);
        out.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(address & 0xFFU));
        out.push_back(data);
        out.push_back(ssm_checksum(out));
        return out;
    }
    // IsoTp: B0 addr_hi addr_med addr_lo data — 5 bytes
    std::vector<std::uint8_t> out;
    out.reserve(5);
    out.push_back(kCmdWriteByAddress);
    out.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(address & 0xFFU));
    out.push_back(data);
    return out;
}

Result<std::uint8_t> parse_b0_response(std::span<std::uint8_t const> resp, Framing framing) {
    if (framing == Framing::KLine) {
        // Minimum response is 80 F0 10 LEN F0 [byte] CSUM = 7 bytes.
        if (resp.size() < 7) {
            return failure(ErrorCode::ParseError, "SSM write response too short");
        }
        if (resp[0] != kHeader || resp[1] != kSrcTool || resp[2] != kDestEcu) {
            return failure(ErrorCode::ParseError, "SSM write response: bad addressing");
        }
        std::size_t const declared_len = resp[3];
        std::size_t const expected_frame = 4 + declared_len + 1;
        if (resp.size() != expected_frame) {
            return failure(ErrorCode::ParseError, "SSM write response: LEN mismatch");
        }
        auto const csum_computed = ssm_checksum(resp.subspan(0, resp.size() - 1));
        if (csum_computed != resp.back()) {
            return failure(ErrorCode::BadChecksum, "SSM write response: bad checksum");
        }
        auto const rsp_byte = resp[4];
        if (rsp_byte == kNegativeResponse) {
            std::uint8_t const nrc = declared_len >= 2 ? resp[5] : 0xFFU;
            return failure(ErrorCode::EcuRejected,
                           "SSM write negative response, NRC=0x" +
                               std::to_string(static_cast<unsigned>(nrc)));
        }
        if (rsp_byte != kRespWriteByAddress) {
            return failure(ErrorCode::EcuRejected,
                           "SSM write unexpected response byte: 0x" +
                               std::to_string(static_cast<unsigned>(rsp_byte)));
        }
        // Echoed data byte sits at index 5; LEN must be at least 2 (RSP + data).
        if (declared_len < 2) {
            return failure(ErrorCode::ParseError, "SSM write response missing echoed data byte");
        }
        return resp[5];
    }
    // IsoTp: [F0, echo_byte] (positive) or [0x7F, NRC] (negative).
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "SSM/CAN write response empty");
    }
    auto const rsp_byte = resp[0];
    if (rsp_byte == kNegativeResponse) {
        std::uint8_t const nrc = resp.size() >= 2 ? resp[1] : 0xFFU;
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN write negative response, NRC=0x" +
                           std::to_string(static_cast<unsigned>(nrc)));
    }
    if (rsp_byte != kRespWriteByAddress) {
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN write unexpected response byte: 0x" +
                           std::to_string(static_cast<unsigned>(rsp_byte)));
    }
    if (resp.size() < 2) {
        return failure(ErrorCode::ParseError, "SSM/CAN write response missing echoed data byte");
    }
    return resp[1];
}

Result<std::vector<std::uint8_t>> build_b8_request(std::uint32_t address,
                                                   std::span<std::uint8_t const> data,
                                                   Framing framing) {
    if (data.empty()) {
        return failure(ErrorCode::InvalidArgument, "SSM block write needs at least one data byte");
    }
    if (framing == Framing::KLine && data.size() > kMaxBlockWriteBytes) {
        return failure(ErrorCode::InvalidArgument, "SSM block write exceeds single-frame limit (" +
                                                       std::to_string(data.size()) + " > " +
                                                       std::to_string(kMaxBlockWriteBytes) + ")");
    }
    if (address > kMaxAddress) {
        return failure(ErrorCode::InvalidArgument, "SSM block write address exceeds 24-bit range");
    }
    auto const end = static_cast<std::uint64_t>(address) + data.size() - 1U;
    if (end > kMaxAddress) {
        return failure(ErrorCode::InvalidArgument,
                       "SSM block write extends past 24-bit address range");
    }

    if (framing == Framing::KLine) {
        // LEN = CMD(1) + 3*addr + N*data = 4 + N
        auto const payload_len = static_cast<std::uint8_t>(4U + data.size());

        std::vector<std::uint8_t> out;
        out.reserve(std::size_t{4} + payload_len + 1U);
        out.push_back(kHeader);
        out.push_back(kDestEcu);
        out.push_back(kSrcTool);
        out.push_back(payload_len);
        out.push_back(kCmdBlockWrite);
        out.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(address & 0xFFU));
        out.insert(out.end(), data.begin(), data.end());
        out.push_back(ssm_checksum(out));
        return out;
    }
    // IsoTp: B8 addr_hi addr_med addr_lo <data...>. push_back without
    // reserve, per the GCC 15 workaround above.
    std::vector<std::uint8_t> out;
    out.push_back(kCmdBlockWrite);
    out.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(address & 0xFFU));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Result<std::vector<std::uint8_t>> parse_b8_response(std::span<std::uint8_t const> resp,
                                                    std::size_t expected_n, Framing framing) {
    if (framing == Framing::KLine) {
        // Minimum response is 80 F0 10 LEN F8 [N bytes] CSUM = 6 + N bytes.
        if (resp.size() < 6) {
            return failure(ErrorCode::ParseError, "SSM block-write response too short");
        }
        if (resp[0] != kHeader || resp[1] != kSrcTool || resp[2] != kDestEcu) {
            return failure(ErrorCode::ParseError, "SSM block-write response: bad addressing");
        }
        std::size_t const declared_len = resp[3];
        std::size_t const expected_frame = 4 + declared_len + 1;
        if (resp.size() != expected_frame) {
            return failure(ErrorCode::ParseError, "SSM block-write response: LEN mismatch");
        }
        auto const csum_computed = ssm_checksum(resp.subspan(0, resp.size() - 1));
        if (csum_computed != resp.back()) {
            return failure(ErrorCode::BadChecksum, "SSM block-write response: bad checksum");
        }
        auto const rsp_byte = resp[4];
        if (rsp_byte == kNegativeResponse) {
            std::uint8_t const nrc = declared_len >= 2 ? resp[5] : 0xFFU;
            return failure(ErrorCode::EcuRejected,
                           "SSM block-write negative response, NRC=0x" +
                               std::to_string(static_cast<unsigned>(nrc)));
        }
        if (rsp_byte != kRespBlockWrite) {
            return failure(ErrorCode::EcuRejected,
                           "SSM block-write unexpected response byte: 0x" +
                               std::to_string(static_cast<unsigned>(rsp_byte)));
        }
        // Echoed data = declared_len - 1 (the RSP byte).
        std::size_t const data_n = declared_len - 1;
        if (data_n != expected_n) {
            return failure(ErrorCode::ParseError,
                           "SSM block-write response: expected " + std::to_string(expected_n) +
                               " echoed bytes, got " + std::to_string(data_n));
        }
        std::vector<std::uint8_t> echoed(resp.begin() + 5,
                                         resp.begin() + 5 + static_cast<std::ptrdiff_t>(data_n));
        return echoed;
    }
    // IsoTp: [F8, echo_0, ..., echo_{N-1}] (positive) or [0x7F, NRC] (negative).
    if (resp.empty()) {
        return failure(ErrorCode::ParseError, "SSM/CAN block-write response empty");
    }
    auto const rsp_byte = resp[0];
    if (rsp_byte == kNegativeResponse) {
        std::uint8_t const nrc = resp.size() >= 2 ? resp[1] : 0xFFU;
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN block-write negative response, NRC=0x" +
                           std::to_string(static_cast<unsigned>(nrc)));
    }
    if (rsp_byte != kRespBlockWrite) {
        return failure(ErrorCode::EcuRejected,
                       "SSM/CAN block-write unexpected response byte: 0x" +
                           std::to_string(static_cast<unsigned>(rsp_byte)));
    }
    std::size_t const data_n = resp.size() - 1;
    if (data_n != expected_n) {
        return failure(ErrorCode::ParseError, "SSM/CAN block-write response: expected " +
                                                  std::to_string(expected_n) +
                                                  " echoed bytes, got " + std::to_string(data_n));
    }
    return std::vector<std::uint8_t>(resp.begin() + 1, resp.end());
}

Result<std::vector<std::uint8_t>> SsmClient::read(std::span<std::uint32_t const> addresses,
                                                  std::chrono::milliseconds timeout) {
    auto req = build_a8_request(addresses, framing_);
    if (!req.has_value())
        return failure(req.error());

    auto resp = transport_->send_recv(*req, timeout);
    if (!resp.has_value())
        return failure(resp.error());

    return parse_a8_response(resp->data, addresses.size(), framing_);
}

Result<std::uint8_t> SsmClient::write(std::uint32_t address, std::uint8_t data,
                                      std::chrono::milliseconds timeout) {
    auto req = build_b0_request(address, data, framing_);
    if (!req.has_value())
        return failure(req.error());
    auto resp = transport_->send_recv(*req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_b0_response(resp->data, framing_);
}

Result<std::vector<std::uint8_t>> SsmClient::write_block(std::uint32_t address,
                                                         std::span<std::uint8_t const> data,
                                                         std::chrono::milliseconds timeout) {
    auto req = build_b8_request(address, data, framing_);
    if (!req.has_value())
        return failure(req.error());
    auto resp = transport_->send_recv(*req, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    return parse_b8_response(resp->data, data.size(), framing_);
}

Result<std::vector<std::uint8_t>>
SsmClient::send_byte_command(std::uint8_t cmd, std::span<std::uint8_t const> payload,
                              std::chrono::milliseconds timeout) {
    // Build the request frame per the active framing. KLine wraps the
    // cmd + payload in a header / src / dest / len / ... / checksum
    // envelope; IsoTp ships the bare bytes and lets the transport's
    // ISO-TP layer handle segmentation.
    std::vector<std::uint8_t> req;
    if (framing_ == Framing::KLine) {
        // 80 10 F0 LEN <cmd> <payload...> CSUM
        // LEN = 1 (cmd byte) + payload.size()
        if (payload.size() > 0xFEU) {
            return failure(ErrorCode::InvalidArgument,
                           "SSM send_byte_command: KLine LEN field is 1 byte; "
                           "payload too large");
        }
        // Indexed assignment instead of reserve+insert — GCC 15 emits a
        // spurious -Wfree-nonheap-object on the reserve+insert pattern
        // when the parent function has multi-branch reserve sites.
        std::size_t const total = 5 + payload.size() + 1;
        req.resize(total);
        req[0] = kHeader;
        req[1] = kDestEcu;
        req[2] = kSrcTool;
        req[3] = static_cast<std::uint8_t>(1 + payload.size());
        req[4] = cmd;
        for (std::size_t i = 0; i < payload.size(); ++i) {
            req[5 + i] = payload[i];
        }
        std::span<std::uint8_t const> const view(req.data(), req.size() - 1);
        req[total - 1] = ssm_checksum(view);
    } else {
        // IsoTp: raw cmd + payload. The transport layer handles
        // multi-frame segmentation if the payload is large.
        req.push_back(cmd);
        for (auto const b : payload) {
            req.push_back(b);
        }
    }
    auto resp = transport_->send_recv(req, timeout);
    if (!resp.has_value())
        return failure(resp.error());

    auto const &raw = resp->data;
    // Strip the framing envelope to expose the response body. The SSM
    // response ACK byte is `cmd | 0x40` per the standard convention.
    std::span<std::uint8_t const> body{};
    if (framing_ == Framing::KLine) {
        // Response shape: 80 F0 10 LEN <ack> <body...> CSUM
        if (raw.size() < 6 || raw[0] != kHeader || raw[1] != kSrcTool ||
            raw[2] != kDestEcu) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: malformed KLine response "
                           "envelope");
        }
        std::uint8_t const len = raw[3];
        if (raw.size() != 4U + static_cast<std::size_t>(len) + 1U) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: KLine response LEN does "
                           "not match wire size");
        }
        if (len < 1U) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: KLine response missing "
                           "ack byte");
        }
        // Checksum byte (last) — defensive verify.
        std::span<std::uint8_t const> const csum_view(raw.data(), raw.size() - 1);
        if (raw.back() != ssm_checksum(csum_view)) {
            return failure(ErrorCode::BadChecksum,
                           "SSM send_byte_command: KLine response checksum "
                           "mismatch");
        }
        if (raw[4] != static_cast<std::uint8_t>(cmd | 0x40U)) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: KLine response ack byte "
                           "does not match `cmd | 0x40`");
        }
        body = std::span<std::uint8_t const>{raw.data() + 5, len - 1U};
    } else {
        // IsoTp response: bare <ack> <body...>
        if (raw.empty()) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: empty IsoTp response");
        }
        if (raw[0] != static_cast<std::uint8_t>(cmd | 0x40U)) {
            return failure(ErrorCode::ProtocolError,
                           "SSM send_byte_command: IsoTp response ack byte "
                           "does not match `cmd | 0x40`");
        }
        body = std::span<std::uint8_t const>{raw.data() + 1, raw.size() - 1};
    }
    return std::vector<std::uint8_t>{body.begin(), body.end()};
}

Result<std::vector<std::uint8_t>> SsmClient::read_block(std::uint32_t base_address,
                                                        std::size_t length,
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
