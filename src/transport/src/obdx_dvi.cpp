// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/obdx_dvi.hpp"

#include "st/core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace st::transport::obdx::dvi {

std::uint8_t checksum(std::span<std::uint8_t const> bytes) noexcept {
    std::uint32_t sum = 0;
    for (auto b : bytes) {
        sum += b;
    }
    // Per the v1.06 spec: sum all preceding bytes, bitwise NOT,
    // keep as uint8. The implicit narrowing to uint8 + bitwise NOT
    // matches `~static_cast<uint8_t>(sum)`.
    return static_cast<std::uint8_t>(~static_cast<std::uint8_t>(sum));
}

Result<std::vector<std::uint8_t>> encode_request(Opcode op, std::span<std::uint8_t const> payload) {
    auto const max = max_payload_for(op);
    if (payload.size() > max) {
        std::string msg{"obdx::dvi::encode_request: payload size "};
        msg.append(std::to_string(payload.size()));
        msg.append(" exceeds opcode 0x");
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02X", static_cast<unsigned>(op));
        msg.append(hex);
        msg.append(" maximum of ");
        msg.append(std::to_string(max));
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    auto const lw = length_width_for(op);
    auto const len_size = (lw == LengthWidth::TwoBytes ? 2U : 1U);

    std::vector<std::uint8_t> frame;
    frame.reserve(1U + len_size + payload.size() + 1U);
    frame.push_back(static_cast<std::uint8_t>(op));
    if (lw == LengthWidth::TwoBytes) {
        frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
        frame.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFU));
    } else {
        frame.push_back(static_cast<std::uint8_t>(payload.size()));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(checksum(std::span{frame.data(), frame.size()}));
    return frame;
}

namespace {

// Recover the request opcode from a normal-response opcode. The spec
// sets bit 4 (0x10) of the request opcode to mark the response, so
// the inverse is to clear that bit. RX/TX opcodes that already have
// bit 4 set in the request collide on this scheme — we surface them
// as-is and leave higher-layer disambiguation to the caller. The
// codec's job is framing, not request/response matching semantics.
[[nodiscard]] constexpr std::uint8_t infer_request_opcode(std::uint8_t response_opcode) noexcept {
    return static_cast<std::uint8_t>(response_opcode & 0xEFU);
}

// Length-field-width selector for INCOMING frames. A response to
// RxLarge / TxLarge (request opcodes 0x09 / 0x11) carries 2-byte
// length; everything else carries 1-byte. Error frames (0x7F) use
// 1-byte length too.
[[nodiscard]] constexpr LengthWidth
length_width_for_response(std::uint8_t response_opcode) noexcept {
    auto const req = infer_request_opcode(response_opcode);
    if (req == static_cast<std::uint8_t>(Opcode::RxLarge) ||
        req == static_cast<std::uint8_t>(Opcode::TxLarge)) {
        return LengthWidth::TwoBytes;
    }
    return LengthWidth::OneByte;
}

} // namespace

Result<DecodedFrame> decode_frame(std::span<std::uint8_t const> bytes) {
    // Minimum frame: 1-byte CMD + 1-byte LEN + 1-byte CHK = 3 bytes.
    if (bytes.size() < 3) {
        return failure(ErrorCode::ParseError, "obdx::dvi::decode_frame: buffer too short "
                                              "(need at least CMD+LEN+CHK)");
    }

    std::uint8_t const opcode = bytes[0];
    bool const is_error = (opcode == kErrorOpcode);

    LengthWidth const lw = is_error ? LengthWidth::OneByte : length_width_for_response(opcode);

    std::size_t const len_off = 1;
    std::size_t const len_size = (lw == LengthWidth::TwoBytes ? 2U : 1U);

    if (bytes.size() < 1 + len_size + 1) { // CMD + LEN + CHK
        return failure(ErrorCode::ParseError, "obdx::dvi::decode_frame: buffer truncated before "
                                              "length field");
    }

    std::size_t declared_len = 0;
    if (lw == LengthWidth::TwoBytes) {
        declared_len = (static_cast<std::size_t>(bytes[len_off]) << 8U) |
                       static_cast<std::size_t>(bytes[len_off + 1]);
    } else {
        declared_len = bytes[len_off];
    }

    std::size_t const payload_off = len_off + len_size;
    std::size_t const expected = 1 + len_size + declared_len + 1; // CMD + LEN + payload + CHK
    if (bytes.size() != expected) {
        std::string msg{"obdx::dvi::decode_frame: frame size mismatch (declared "
                        "payload "};
        msg.append(std::to_string(declared_len));
        msg.append(" → expected ");
        msg.append(std::to_string(expected));
        msg.append(" total bytes, got ");
        msg.append(std::to_string(bytes.size()));
        msg.append(")");
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    // Verify checksum against every byte preceding the checksum slot.
    std::uint8_t const expected_chk = checksum(bytes.subspan(0, bytes.size() - 1));
    std::uint8_t const actual_chk = bytes[bytes.size() - 1];
    if (expected_chk != actual_chk) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::dvi::decode_frame: checksum mismatch "
                      "(expected 0x%02X, got 0x%02X)",
                      expected_chk, actual_chk);
        return failure(ErrorCode::ParseError, std::string{buf});
    }

    if (is_error) {
        // Error payload: CMD then ERR (2 bytes). Treat anything else
        // as malformed — the spec is explicit on the shape.
        if (declared_len != 2) {
            std::string msg{"obdx::dvi::decode_frame: error frame must declare "
                            "exactly 2 payload bytes (request_op + error_code), got "};
            msg.append(std::to_string(declared_len));
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        ErrorFrame ef;
        ef.request_opcode = bytes[payload_off];
        ef.error_code = bytes[payload_off + 1];
        return DecodedFrame{ef};
    }

    ResponseFrame rf;
    rf.response_opcode = opcode;
    rf.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_off),
                      bytes.begin() + static_cast<std::ptrdiff_t>(payload_off + declared_len));
    return DecodedFrame{std::move(rf)};
}

} // namespace st::transport::obdx::dvi
