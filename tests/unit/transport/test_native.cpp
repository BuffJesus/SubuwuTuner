// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace nat = st::transport::native;

// ---- CRC-16/CCITT-FALSE -----------------------------------------

TEST_CASE("native::crc16_ccitt_false: empty input -> 0xFFFF (init)", "[transport][native]") {
    std::vector<std::uint8_t> empty;
    REQUIRE(nat::crc16_ccitt_false(std::span<std::uint8_t const>{empty}) == 0xFFFFU);
}

TEST_CASE("native::crc16_ccitt_false: '123456789' -> 0x29B1 (RevEng vector)",
          "[transport][native]") {
    // Standard RevEng CRC-16/CCITT-FALSE check vector. If this
    // shifts we picked the wrong polynomial / init / reflection.
    std::string_view const ascii = "123456789";
    std::span<std::uint8_t const> bytes{reinterpret_cast<std::uint8_t const *>(ascii.data()),
                                        ascii.size()};
    REQUIRE(nat::crc16_ccitt_false(bytes) == 0x29B1U);
}

TEST_CASE("native::crc16_ccitt_false: single 0x00 byte -> 0xE1F0", "[transport][native]") {
    // crc=0xFFFF, XOR with 0x00<<8 = 0xFFFF, then 8 left-shift +
    // conditional XOR with 0x1021 yields 0xE1F0. Independently
    // computable by hand or via any CRC-16/CCITT-FALSE reference.
    std::array<std::uint8_t, 1> zero{0x00U};
    REQUIRE(nat::crc16_ccitt_false(zero) == 0xE1F0U);
}

// ---- encode_request ---------------------------------------------

TEST_CASE("native::encode_request: zero-payload Hello frame, fixed seq 0", "[transport][native]") {
    auto r = nat::encode_request(0, nat::Opcode::Hello, {});
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 7);   // SOF + seq + op + len(2) + CRC(2)
    REQUIRE((*r)[0] == 0xAAU); // SOF
    REQUIRE((*r)[1] == 0x00U); // seq
    REQUIRE((*r)[2] == 0x01U); // Hello opcode
    REQUIRE((*r)[3] == 0x00U); // len hi
    REQUIRE((*r)[4] == 0x00U); // len lo
    // CRC over [AA 00 01 00 00] computed independently below.
    std::array<std::uint8_t, 5> header{0xAAU, 0x00U, 0x01U, 0x00U, 0x00U};
    auto const crc = nat::crc16_ccitt_false(header);
    REQUIRE((*r)[5] == static_cast<std::uint8_t>(crc >> 8U));
    REQUIRE((*r)[6] == static_cast<std::uint8_t>(crc & 0xFFU));
}

TEST_CASE("native::encode_request: payload bytes land in order after the header",
          "[transport][native]") {
    std::array<std::uint8_t, 4> payload{0xDEU, 0xADU, 0xBEU, 0xEFU};
    auto r = nat::encode_request(0x42, nat::Opcode::SendBytes, payload);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 7 + 4);
    REQUIRE((*r)[0] == 0xAAU);
    REQUIRE((*r)[1] == 0x42U); // seq echoes
    REQUIRE((*r)[2] == 0x04U); // SendBytes opcode
    REQUIRE((*r)[3] == 0x00U);
    REQUIRE((*r)[4] == 0x04U); // len = 4
    REQUIRE((*r)[5] == 0xDEU);
    REQUIRE((*r)[6] == 0xADU);
    REQUIRE((*r)[7] == 0xBEU);
    REQUIRE((*r)[8] == 0xEFU);
}

TEST_CASE("native::encode_request: rejects payloads larger than kMaxPayloadBytes",
          "[transport][native]") {
    std::vector<std::uint8_t> too_big(nat::kMaxPayloadBytes + 1, 0xCDU);
    auto r = nat::encode_request(0, nat::Opcode::SendBytes, too_big);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("native::encode_request: max-size payload accepted (~64 KiB)", "[transport][native]") {
    std::vector<std::uint8_t> at_limit(nat::kMaxPayloadBytes, 0xABU);
    auto r = nat::encode_request(0, nat::Opcode::SendBytes, at_limit);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 7 + nat::kMaxPayloadBytes);
    // Length field is 2-byte BE, so the at-limit case has bytes 3+4
    // set to 0xFF / 0xFF.
    REQUIRE((*r)[3] == 0xFFU);
    REQUIRE((*r)[4] == 0xFFU);
}

// ---- decode_frame ------------------------------------------------

// Build a syntactically valid response frame for opcode = request|0x80,
// with the given seq + payload, and return the bytes ready to feed
// to decode_frame. Used to keep the decode tests focused on the
// branch being exercised rather than on framing arithmetic.
namespace {
std::vector<std::uint8_t> build_response_frame(std::uint8_t seq, std::uint8_t response_op,
                                               std::span<std::uint8_t const> payload) {
    // GCC 15's `-Werror=free-nonheap-object` mis-fires on
    // `reserve() + push_back()` against a length derived from a
    // span size when it can't prove the upper bound. Construct
    // through the size-then-overwrite idiom instead — same final
    // layout, no warning.
    std::vector<std::uint8_t> frame(5 + payload.size() + 2, 0);
    frame[0] = nat::kStartOfFrame;
    frame[1] = seq;
    frame[2] = response_op;
    frame[3] = static_cast<std::uint8_t>(payload.size() >> 8U);
    frame[4] = static_cast<std::uint8_t>(payload.size() & 0xFFU);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame[5 + i] = payload[i];
    }
    auto const crc = nat::crc16_ccitt_false(std::span{frame.data(), 5 + payload.size()});
    frame[5 + payload.size() + 0] = static_cast<std::uint8_t>(crc >> 8U);
    frame[5 + payload.size() + 1] = static_cast<std::uint8_t>(crc & 0xFFU);
    return frame;
}
} // namespace

TEST_CASE("native::decode_frame: too short (< 7 B) -> ParseError", "[transport][native]") {
    std::array<std::uint8_t, 6> short_buf{0xAAU, 0, 0, 0, 0, 0};
    auto r = nat::decode_frame(short_buf);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("native::decode_frame: wrong SOF byte -> ParseError", "[transport][native]") {
    auto bytes = build_response_frame(0, 0x81U, {}); // SOF=AA, seq=0, op=Hello-response, len=0
    bytes[0] = 0x55U;
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: bad CRC -> ParseError", "[transport][native]") {
    auto bytes = build_response_frame(0, 0x81U, {});
    bytes[bytes.size() - 1] ^= 0x01U; // flip the low CRC byte
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: declared length overruns buffer -> ParseError",
          "[transport][native]") {
    // Build a valid 7-byte frame, then claim 4-byte payload via the
    // length field. CRC over the now-broken header won't match
    // either, but the size check fires first.
    auto bytes = build_response_frame(0, 0x81U, {});
    bytes[3] = 0x00U;
    bytes[4] = 0x04U; // claim 4-byte payload in a frame that has 0
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: trailing bytes -> ParseError", "[transport][native]") {
    auto bytes = build_response_frame(0, 0x81U, {});
    bytes.push_back(0xFFU);
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: Hello response round-trips through Response",
          "[transport][native]") {
    std::array<std::uint8_t, 6> payload{
        // fw_version major.minor.patch (3 B) + capabilities (3 B), say
        0x01U, 0x00U, 0x02U, 0xFFU, 0xFFU, 0xFFU};
    // Response to Hello (0x01): op = 0x01 | 0x80 = 0x81.
    auto bytes = build_response_frame(0x77, 0x81U, payload);
    auto r = nat::decode_frame(bytes);
    REQUIRE(r.has_value());
    auto const *rf = std::get_if<nat::Response>(&*r);
    REQUIRE(rf != nullptr);
    REQUIRE(rf->seq == 0x77);
    REQUIRE(rf->request_op == nat::Opcode::Hello);
    REQUIRE(rf->payload.size() == 6);
    REQUIRE(rf->payload[0] == 0x01U);
    REQUIRE(rf->payload[5] == 0xFFU);
}

TEST_CASE("native::decode_frame: response with the response-bit cleared -> ParseError",
          "[transport][native]") {
    // op = 0x01 (request opcode) in the response slot — adapter
    // misconfiguration. Must not silently pass through as a request.
    auto bytes = build_response_frame(0, 0x01U, {});
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: error frame parses out seq + request_op + err",
          "[transport][native]") {
    // Error frame payload: seq=0x42, request_op=SendBytes(0x04), err=0x07.
    std::array<std::uint8_t, 3> payload{0x42U, 0x04U, 0x07U};
    auto bytes = build_response_frame(0, // outer seq irrelevant for error frames
                                      nat::kErrorOpcode, payload);
    auto r = nat::decode_frame(bytes);
    REQUIRE(r.has_value());
    auto const *ef = std::get_if<nat::ErrorFrame>(&*r);
    REQUIRE(ef != nullptr);
    REQUIRE(ef->seq == 0x42U);
    REQUIRE(ef->request_op == nat::Opcode::SendBytes);
    REQUIRE(ef->error_code == 0x07U);
}

TEST_CASE("native::decode_frame: error frame with wrong payload length -> ParseError",
          "[transport][native]") {
    std::array<std::uint8_t, 2> payload{0x42U, 0x04U}; // missing err byte
    auto bytes = build_response_frame(0, nat::kErrorOpcode, payload);
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("native::decode_frame: event frame splits event_code + payload", "[transport][native]") {
    // Event code 0x10 (say: datalog sample) + 4 bytes of sample data.
    std::array<std::uint8_t, 5> payload{0x10U, 0xDEU, 0xADU, 0xBEU, 0xEFU};
    auto bytes = build_response_frame(0, nat::kEventOpcode, payload);
    auto r = nat::decode_frame(bytes);
    REQUIRE(r.has_value());
    auto const *ev = std::get_if<nat::Event>(&*r);
    REQUIRE(ev != nullptr);
    REQUIRE(ev->event_code == 0x10U);
    REQUIRE(ev->payload.size() == 4);
    REQUIRE(ev->payload[0] == 0xDEU);
    REQUIRE(ev->payload[3] == 0xEFU);
}

TEST_CASE("native::decode_frame: empty event payload -> ParseError", "[transport][native]") {
    auto bytes = build_response_frame(0, nat::kEventOpcode, {});
    auto r = nat::decode_frame(bytes);
    REQUIRE_FALSE(r.has_value());
}

// ---- round-trip --------------------------------------------------

TEST_CASE("native::encode_request round-trips THROUGH decode_frame for "
          "every Opcode (treating the request bytes as a hypothetical "
          "response with the response bit set)",
          "[transport][native]") {
    // Encoder + decoder must agree on framing for every opcode the
    // protocol defines. Trick: encode_request writes the request
    // opcode bare; we set the response bit in the buffer post-encode
    // to make the decoder accept it as a Response.
    constexpr nat::Opcode all[] = {
        nat::Opcode::Hello,       nat::Opcode::Connect,    nat::Opcode::Disconnect,
        nat::Opcode::SendBytes,   nat::Opcode::RecvBytes,  nat::Opcode::SetConfig,
        nat::Opcode::StreamStart, nat::Opcode::StreamStop, nat::Opcode::ReadBatteryVoltage,
        nat::Opcode::Reset,
    };
    for (auto op : all) {
        std::array<std::uint8_t, 2> body{0x12U, 0x34U};
        auto enc = nat::encode_request(0x55, op, body);
        REQUIRE(enc.has_value());
        // Flip on the response bit and re-checksum so the decoder
        // accepts the frame as a Response.
        (*enc)[2] = static_cast<std::uint8_t>((*enc)[2] | nat::kResponseMask);
        auto const new_crc = nat::crc16_ccitt_false(std::span{enc->data(), enc->size() - 2});
        (*enc)[enc->size() - 2] = static_cast<std::uint8_t>(new_crc >> 8U);
        (*enc)[enc->size() - 1] = static_cast<std::uint8_t>(new_crc & 0xFFU);

        auto dec = nat::decode_frame(*enc);
        REQUIRE(dec.has_value());
        auto const *rf = std::get_if<nat::Response>(&*dec);
        REQUIRE(rf != nullptr);
        REQUIRE(rf->seq == 0x55);
        REQUIRE(rf->request_op == op);
        REQUIRE(rf->payload.size() == 2);
        REQUIRE(rf->payload[0] == 0x12U);
        REQUIRE(rf->payload[1] == 0x34U);
    }
}
