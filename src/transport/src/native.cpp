// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/native.hpp"

#include "st/core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace st::transport::native {

std::uint16_t crc16_ccitt_false(std::span<std::uint8_t const> bytes) noexcept {
    // Polynomial 0x1021 (CRC-16/CCITT-FALSE / XMODEM variant), init
    // 0xFFFF, MSB-first, no input/output reflection, no final XOR.
    // Standard test vector: crc("123456789") = 0x29B1 (covered in
    // the unit tests).
    std::uint16_t crc = 0xFFFFU;
    for (auto b : bytes) {
        crc ^= static_cast<std::uint16_t>(b) << 8U;
        for (int i = 0; i < 8; ++i) {
            if ((crc & 0x8000U) != 0) {
                crc = static_cast<std::uint16_t>(static_cast<std::uint16_t>(crc << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

namespace {

// CRC covers everything from SOF through end-of-payload — i.e. the
// 5-byte header (SOF + seq + op + len_hi + len_lo) plus the payload.
// The CRC itself sits in the last 2 bytes and is not included in
// its own computation. Pulled into a helper so encode/decode use
// the same span boundary.
[[nodiscard]] std::uint16_t
crc_over_frame(std::span<std::uint8_t const> frame_without_crc) noexcept {
    return crc16_ccitt_false(frame_without_crc);
}

} // namespace

Result<std::vector<std::uint8_t>> encode_request(std::uint8_t seq, Opcode op,
                                                 std::span<std::uint8_t const> payload) {
    if (payload.size() > kMaxPayloadBytes) {
        std::string msg{"native::encode_request: payload size "};
        msg.append(std::to_string(payload.size()));
        msg.append(" exceeds frame maximum of ");
        msg.append(std::to_string(kMaxPayloadBytes));
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }

    std::vector<std::uint8_t> frame;
    // SOF + seq + op + 2-byte len + payload + 2-byte CRC.
    frame.reserve(7 + payload.size());
    frame.push_back(kStartOfFrame);
    frame.push_back(seq);
    frame.push_back(static_cast<std::uint8_t>(op));
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
    frame.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    auto const crc = crc_over_frame(std::span{frame.data(), frame.size()});
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return frame;
}

Result<DecodedFrame> decode_frame(std::span<std::uint8_t const> bytes) {
    // Minimum frame: SOF + seq + op + 2-byte len + 2-byte CRC = 7 B
    // (payload may be zero).
    if (bytes.size() < 7) {
        return failure(ErrorCode::ParseError, "native::decode_frame: buffer too short "
                                              "(need at least 7 bytes for SOF+seq+op+len+CRC)");
    }
    if (bytes[0] != kStartOfFrame) {
        char buf[80];
        std::snprintf(buf, sizeof buf,
                      "native::decode_frame: wrong start-of-frame byte "
                      "(expected 0x%02X, got 0x%02X)",
                      kStartOfFrame, bytes[0]);
        return failure(ErrorCode::ParseError, std::string{buf});
    }

    std::uint8_t const seq = bytes[1];
    std::uint8_t const raw_op = bytes[2];
    std::size_t const declared_len =
        (static_cast<std::size_t>(bytes[3]) << 8U) | static_cast<std::size_t>(bytes[4]);
    std::size_t const expected = 5 + declared_len + 2; // header + payload + CRC
    if (bytes.size() != expected) {
        std::string msg{"native::decode_frame: frame size mismatch (declared payload "};
        msg.append(std::to_string(declared_len));
        msg.append(" → expected ");
        msg.append(std::to_string(expected));
        msg.append(" total bytes, got ");
        msg.append(std::to_string(bytes.size()));
        msg.append(")");
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    std::uint16_t const expected_crc = crc_over_frame(bytes.subspan(0, bytes.size() - 2));
    std::uint16_t const actual_crc =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[bytes.size() - 2]) << 8U) |
                                   static_cast<std::uint16_t>(bytes[bytes.size() - 1]));
    if (expected_crc != actual_crc) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "native::decode_frame: CRC mismatch "
                      "(expected 0x%04X, got 0x%04X)",
                      expected_crc, actual_crc);
        return failure(ErrorCode::ParseError, std::string{buf});
    }

    // Now dispatch on the opcode-marker bits.
    std::size_t const payload_off = 5;
    auto payload_begin = bytes.begin() + static_cast<std::ptrdiff_t>(payload_off);
    auto payload_end = bytes.begin() + static_cast<std::ptrdiff_t>(payload_off + declared_len);

    if (raw_op == kErrorOpcode) {
        if (declared_len != 3) {
            std::string msg{"native::decode_frame: error frame must declare "
                            "exactly 3 payload bytes (seq+request_op+err), got "};
            msg.append(std::to_string(declared_len));
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        ErrorFrame ef;
        // The async-error frame's seq slot duplicates the request's
        // seq for ergonomic matching (PC can ignore the outer seq).
        ef.seq = bytes[payload_off];
        ef.request_op = static_cast<Opcode>(bytes[payload_off + 1]);
        ef.error_code = bytes[payload_off + 2];
        return DecodedFrame{ef};
    }

    if (raw_op == kEventOpcode) {
        if (declared_len == 0) {
            return failure(ErrorCode::ParseError, "native::decode_frame: event frame must "
                                                  "carry at least one byte (event_code)");
        }
        Event ev;
        ev.event_code = bytes[payload_off];
        ev.payload.assign(payload_begin + 1, payload_end);
        return DecodedFrame{std::move(ev)};
    }

    if ((raw_op & kResponseMask) == 0) {
        // Bare command opcode in the response slot — that's the
        // adapter echoing a request back to us, which only happens
        // in a misconfigured firmware. Surface as a parse error so
        // higher layers see it (rather than silently treating it
        // as a request).
        char buf[80];
        std::snprintf(buf, sizeof buf,
                      "native::decode_frame: opcode 0x%02X missing "
                      "response bit (0x80) — adapter sent a request "
                      "into the response stream",
                      raw_op);
        return failure(ErrorCode::ParseError, std::string{buf});
    }

    Response rf;
    rf.seq = seq;
    rf.request_op = static_cast<Opcode>(raw_op & 0x7FU);
    rf.payload.assign(payload_begin, payload_end);
    return DecodedFrame{std::move(rf)};
}

} // namespace st::transport::native
