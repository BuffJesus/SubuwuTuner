// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/obdx_dvi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace dvi = st::transport::obdx::dvi;

// ---- checksum ----------------------------------------------------

TEST_CASE("dvi::checksum: empty span sums to 0, NOT'd to 0xFF", "[transport][obdx_dvi]") {
    std::vector<std::uint8_t> empty;
    REQUIRE(dvi::checksum(std::span<std::uint8_t const>{empty}) == 0xFFU);
}

TEST_CASE("dvi::checksum: single 0x00 -> 0xFF", "[transport][obdx_dvi]") {
    std::array<std::uint8_t, 1> one{0x00};
    REQUIRE(dvi::checksum(one) == 0xFFU);
}

TEST_CASE("dvi::checksum: single 0xFF -> 0x00", "[transport][obdx_dvi]") {
    std::array<std::uint8_t, 1> one{0xFFU};
    REQUIRE(dvi::checksum(one) == 0x00U);
}

TEST_CASE("dvi::checksum: 0x22 0x01 -> ~(0x23)=0xDC", "[transport][obdx_dvi]") {
    // ScantoolInfo (0x22) + sub-op 0x01 (HW string request payload byte)
    // is a real shape we'll send. Sum = 0x23, NOT = 0xDC.
    std::array<std::uint8_t, 2> bytes{0x22U, 0x01U};
    REQUIRE(dvi::checksum(bytes) == 0xDCU);
}

TEST_CASE("dvi::checksum: overflows past 8 bits cleanly", "[transport][obdx_dvi]") {
    // 0xFF + 0xFF + 0xFF = 0x2FD; the spec truncates to uint8 before
    // NOT, so result = ~(0xFD) = 0x02.
    std::array<std::uint8_t, 3> bytes{0xFFU, 0xFFU, 0xFFU};
    REQUIRE(dvi::checksum(bytes) == 0x02U);
}

// ---- encode_request ----------------------------------------------

TEST_CASE("dvi::encode_request: zero-payload ScantoolInfo frame", "[transport][obdx_dvi]") {
    auto r = dvi::encode_request(dvi::Opcode::ScantoolInfo, {});
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);   // CMD + LEN + CHK
    REQUIRE((*r)[0] == 0x22U); // opcode
    REQUIRE((*r)[1] == 0x00U); // length = 0
    // checksum of [0x22, 0x00] = ~(0x22) = 0xDD
    REQUIRE((*r)[2] == 0xDDU);
}

TEST_CASE("dvi::encode_request: ScantoolInfo + sub-op 0x02 (firmware string)",
          "[transport][obdx_dvi]") {
    std::array<std::uint8_t, 1> sub{0x02U};
    auto r = dvi::encode_request(dvi::Opcode::ScantoolInfo, sub);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 4); // CMD + LEN + 1 payload + CHK
    REQUIRE((*r)[0] == 0x22U);
    REQUIRE((*r)[1] == 0x01U);
    REQUIRE((*r)[2] == 0x02U);
    // checksum of [0x22, 0x01, 0x02] = ~(0x25) = 0xDA
    REQUIRE((*r)[3] == 0xDAU);
}

TEST_CASE("dvi::encode_request: TxSmall accepts up to 255 B payload", "[transport][obdx_dvi]") {
    std::vector<std::uint8_t> payload(255, 0xABU);
    auto r = dvi::encode_request(dvi::Opcode::TxSmall, payload);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1 + 1 + 255 + 1);
    REQUIRE((*r)[0] == 0x10U);
    REQUIRE((*r)[1] == 0xFFU);
}

TEST_CASE("dvi::encode_request: TxSmall rejects 256 B payload", "[transport][obdx_dvi]") {
    std::vector<std::uint8_t> payload(256, 0xABU);
    auto r = dvi::encode_request(dvi::Opcode::TxSmall, payload);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("dvi::encode_request: TxLarge accepts up to 12000 B payload", "[transport][obdx_dvi]") {
    std::vector<std::uint8_t> payload(12000, 0xCDU);
    auto r = dvi::encode_request(dvi::Opcode::TxLarge, payload);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1 + 2 + 12000 + 1);
    REQUIRE((*r)[0] == 0x11U);
    // Length is 2-byte big-endian: 12000 = 0x2EE0.
    REQUIRE((*r)[1] == 0x2EU);
    REQUIRE((*r)[2] == 0xE0U);
}

TEST_CASE("dvi::encode_request: TxLarge rejects 12001 B payload", "[transport][obdx_dvi]") {
    std::vector<std::uint8_t> payload(12001, 0xCDU);
    auto r = dvi::encode_request(dvi::Opcode::TxLarge, payload);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("dvi::encode_request: frame round-trips through decode_frame "
          "(treating the request bytes as a hypothetical response)",
          "[transport][obdx_dvi]") {
    // The codec doesn't distinguish request-from-response at the
    // framing layer; the bytes have identical structure. Encoding a
    // request and decoding it as a frame should recover the same
    // payload — sanity check that both halves use the same length-
    // width + checksum conventions.
    std::array<std::uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
    auto enc = dvi::encode_request(dvi::Opcode::Settings, payload);
    REQUIRE(enc.has_value());
    auto dec = dvi::decode_frame(*enc);
    REQUIRE(dec.has_value());
    auto const *rf = std::get_if<dvi::ResponseFrame>(&*dec);
    REQUIRE(rf != nullptr);
    REQUIRE(rf->response_opcode == 0x24U);
    REQUIRE(rf->payload.size() == 4);
    REQUIRE(rf->payload == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

// ---- decode_frame ------------------------------------------------

TEST_CASE("dvi::decode_frame: truncated buffer (< 3 bytes) -> ParseError",
          "[transport][obdx_dvi]") {
    std::array<std::uint8_t, 2> too_short{0x22U, 0x00U};
    auto r = dvi::decode_frame(too_short);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("dvi::decode_frame: bad checksum -> ParseError", "[transport][obdx_dvi]") {
    // [0x22, 0x00, 0xDD] is valid; [0x22, 0x00, 0xDE] differs by 1.
    std::array<std::uint8_t, 3> bad{0x22U, 0x00U, 0xDEU};
    auto r = dvi::decode_frame(bad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("dvi::decode_frame: declared-length overruns buffer -> ParseError",
          "[transport][obdx_dvi]") {
    // Claims 4-byte payload but the buffer doesn't carry that many.
    std::array<std::uint8_t, 3> short_frame{0x22U, 0x04U, 0xD9U};
    auto r = dvi::decode_frame(short_frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("dvi::decode_frame: extra trailing bytes -> ParseError", "[transport][obdx_dvi]") {
    // The decoder requires `bytes` to be exactly one frame.
    // [0x22, 0x00, 0xDD] is a valid 3-byte frame; appending a stray
    // byte should fail (stream reassembly is a higher-layer concern).
    std::array<std::uint8_t, 4> with_tail{0x22U, 0x00U, 0xDDU, 0xFFU};
    auto r = dvi::decode_frame(with_tail);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("dvi::decode_frame: error frame parses out request_op + error_code",
          "[transport][obdx_dvi]") {
    // 0x7F LEN CMD ERR CHK with declared LEN = 2.
    // Payload = CMD=0x22 ERR=0x04. Checksum = ~(0x7F+0x02+0x22+0x04) =
    // ~(0xA7) = 0x58.
    std::array<std::uint8_t, 5> err{0x7FU, 0x02U, 0x22U, 0x04U, 0x58U};
    auto r = dvi::decode_frame(err);
    REQUIRE(r.has_value());
    auto const *ef = std::get_if<dvi::ErrorFrame>(&*r);
    REQUIRE(ef != nullptr);
    REQUIRE(ef->request_opcode == 0x22U);
    REQUIRE(ef->error_code == 0x04U);
}

TEST_CASE("dvi::decode_frame: error frame with wrong LEN -> ParseError", "[transport][obdx_dvi]") {
    // Error frames must declare LEN=2 (CMD + ERR). LEN=3 is malformed.
    // Build with a payload that satisfies length-field math + checksum
    // so we don't trip the size check first.
    // [0x7F, 0x03, 0x22, 0x04, 0x00] payload + chk.
    // sum = 0x7F + 0x03 + 0x22 + 0x04 + 0x00 = 0xA8; chk = ~0xA8 = 0x57.
    std::array<std::uint8_t, 6> bad_len{0x7FU, 0x03U, 0x22U, 0x04U, 0x00U, 0x57U};
    auto r = dvi::decode_frame(bad_len);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("dvi::decode_frame: 2-byte-length response for RxLarge", "[transport][obdx_dvi]") {
    // Response to RxLarge has opcode = request(0x09) | 0x10 = 0x19,
    // a 2-byte BE length, then payload, then CHK. Build a 4-byte
    // payload to verify length parsing.
    // bytes: [0x19, 0x00, 0x04, 0xAA, 0xBB, 0xCC, 0xDD, CHK]
    // sum = 0x19 + 0x00 + 0x04 + 0xAA + 0xBB + 0xCC + 0xDD = 0x32B
    //     -> low byte 0x2B; chk = ~0x2B = 0xD4.
    std::array<std::uint8_t, 8> frame{0x19U, 0x00U, 0x04U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xD4U};
    auto r = dvi::decode_frame(frame);
    REQUIRE(r.has_value());
    auto const *rf = std::get_if<dvi::ResponseFrame>(&*r);
    REQUIRE(rf != nullptr);
    REQUIRE(rf->response_opcode == 0x19U);
    REQUIRE(rf->payload.size() == 4);
    REQUIRE(rf->payload[0] == 0xAAU);
    REQUIRE(rf->payload[3] == 0xDDU);
}
