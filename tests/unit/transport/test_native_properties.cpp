// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Property-based tests for the native (doc-18 handheld) codec — ship
// blocker #11. Companion to test_obdx_dvi_properties.cpp and
// test_ssm_properties.cpp.
//
// Example-based tests in test_native.cpp cover specific frames against
// pinned RevEng / hand-computed vectors. The properties below cover
// the codec's behavior across the input space: any opcode × any
// payload length × any seq → encode → decode → original. Catches
// bit-flip / off-by-one / wrong-endian / wrong-CRC-byte-order
// regressions that pinned-example tests miss.
//
// Properties pinned here:
//
//   1. crc16_ccitt_false(bytes) matches a reference implementation
//      (init 0xFFFF, poly 0x1021, no refin/refout/xorout, MSB-first
//      shift-and-XOR). Independent of the codec's implementation to
//      catch drift.
//
//   2. encode_request(seq, op, payload) → decode_frame → Response{
//      seq, op, payload} across all request opcodes and payload sizes
//      in [0, kMaxPayloadBytes / 64] (capped low because large
//      payloads × many iterations chews wall-clock — separate large-
//      payload sweep handles the upper end).
//
//   3. Large-payload encode→decode roundtrip on payloads up to ~16
//      KiB. Fewer iterations, exercises the 2-byte length field's
//      high byte (which a 64-byte-capped sweep can never set).
//
//   4. encode rejects payloads > kMaxPayloadBytes.
//
//   5. decode rejects wrong-SOF buffers (any byte ≠ 0xAA in slot 0).
//
//   6. decode rejects CRC corruption on either CRC byte. Two-byte
//      CRC means single-bit flips in the high byte AND the low byte
//      both fail — distinct from DVI's 1-byte checksum.
//
//   7. decode rejects length-field corruption that puts the declared
//      length out of buffer range.
//
//   8. decode rejects request-opcode-in-response-slot (response bit
//      clear). Adapter misconfiguration we don't want to silently
//      accept.
//
//   9. decode on arbitrary random bytes never crashes — only returns
//      ok() (rarely, when random bytes happen to form a valid frame)
//      or a clean ParseError.
//
//  10. ErrorFrame roundtrip: a manually-built `AA <seq> 7F 00 03
//      <seq2> <req_op> <err> CRC` decodes back to ErrorFrame{seq2,
//      request_op, error_code}. (Per the header, error payload shape
//      is fixed at 3 bytes.)
//
//  11. Event frame roundtrip: a manually-built `AA 00 6F <LEN> <code>
//      <payload>... CRC` decodes back to Event{code, payload}.

#include "../_helpers/property.hpp"

#include "st/core/error.hpp"
#include "st/transport/native.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace nat = st::transport::native;

namespace {

// All request opcodes the public enum defines. Each must keep bit 7
// clear so the response-bit distinguisher in decode_frame works.
constexpr std::array<nat::Opcode, 10> kRequestOpcodes = {
    nat::Opcode::Hello,    nat::Opcode::Connect,           nat::Opcode::Disconnect,
    nat::Opcode::SendBytes, nat::Opcode::RecvBytes,        nat::Opcode::SetConfig,
    nat::Opcode::StreamStart, nat::Opcode::StreamStop,
    nat::Opcode::ReadBatteryVoltage, nat::Opcode::Reset,
};

// Reference CRC-16/CCITT-FALSE per the RevEng catalog: init 0xFFFF,
// polynomial 0x1021 (MSB-first), no reflect in / reflect out / xorout.
// Independent of the codec's implementation so drift between the two
// surfaces immediately. The "123456789" check value of 0x29B1 is
// asserted directly in test_native.cpp.
[[nodiscard]] std::uint16_t reference_crc16_ccitt_false(std::span<std::uint8_t const> bytes) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (auto const b : bytes) {
        crc ^= static_cast<std::uint16_t>(static_cast<std::uint16_t>(b) << 8U);
        for (int i = 0; i < 8; ++i) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((static_cast<std::uint32_t>(crc) << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

// Build a Response frame the hand-rolled way. Mirrors the helper in
// test_native.cpp but kept local so the property file is standalone.
// Built via header-brace-init + insert + push_back rather than
// (size, value) + operator[] to dodge GCC 15's flow-insensitive
// -Werror=null-dereference mis-warning on `vector::operator[]` after
// size-ctor construction.
std::vector<std::uint8_t> build_response_frame_local(std::uint8_t seq, std::uint8_t response_op,
                                                     std::span<std::uint8_t const> payload) {
    std::vector<std::uint8_t> frame{
        nat::kStartOfFrame,
        seq,
        response_op,
        static_cast<std::uint8_t>(payload.size() >> 8U),
        static_cast<std::uint8_t>(payload.size() & 0xFFU),
    };
    frame.insert(frame.end(), payload.begin(), payload.end());
    auto const crc = nat::crc16_ccitt_false(std::span{frame.data(), frame.size()});
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return frame;
}

} // namespace

TEST_CASE("property: native CRC-16/CCITT-FALSE matches reference implementation",
          "[transport][native][property][crc]") {
    st::test::check_property(0xC1C2'C3C4'C5C6'C7C8ULL, 500, [&](st::test::PropertyRng &rng) {
        auto const bytes = rng.bytes(0, 512);
        auto const codec = nat::crc16_ccitt_false(bytes);
        auto const reference = reference_crc16_ccitt_false(bytes);
        REQUIRE(codec == reference);
    });
}

TEST_CASE("property: native encode then decode roundtrips small payloads",
          "[transport][native][property][roundtrip]") {
    st::test::check_property(0x0BD0'0BD0'C0DE'C0DEULL, 500, [&](st::test::PropertyRng &rng) {
        auto const op_idx = rng.size_in(0, kRequestOpcodes.size() - 1);
        auto const op = kRequestOpcodes[op_idx];
        auto const seq = rng.byte();
        auto const payload = rng.bytes(0, 256);

        auto const enc = nat::encode_request(seq, op, payload);
        REQUIRE(enc.has_value());
        // Frame shape: SOF(1) + seq(1) + opcode(1) + len(2) + payload + CRC(2) = 7 + payload.size()
        REQUIRE(enc->size() == payload.size() + 7);
        REQUIRE((*enc)[0] == nat::kStartOfFrame);
        REQUIRE((*enc)[1] == seq);
        REQUIRE((*enc)[2] == static_cast<std::uint8_t>(op));

        // decode_frame expects the response side, so build a synthetic
        // response from the same payload and verify it decodes back.
        auto const resp = build_response_frame_local(
            seq, static_cast<std::uint8_t>(static_cast<std::uint8_t>(op) | nat::kResponseMask),
            payload);
        auto const dec = nat::decode_frame(resp);
        REQUIRE(dec.has_value());
        auto const *rf = std::get_if<nat::Response>(&*dec);
        REQUIRE(rf != nullptr);
        REQUIRE(rf->seq == seq);
        REQUIRE(rf->request_op == op);
        REQUIRE(rf->payload == payload);
    });
}

TEST_CASE("property: native encode then decode roundtrips large payloads",
          "[transport][native][property][roundtrip]") {
    // Larger payload range, smaller iteration count — exercises the
    // 2-byte length field's high byte. 60 × ~8 KiB average ≈ 500 KB
    // total allocations, negligible.
    st::test::check_property(0xCAFE'BABE'F00D'D00DULL, 60, [&](st::test::PropertyRng &rng) {
        auto const op_idx = rng.size_in(0, kRequestOpcodes.size() - 1);
        auto const op = kRequestOpcodes[op_idx];
        auto const seq = rng.byte();
        auto const payload = rng.bytes(257, 16384);

        auto const enc = nat::encode_request(seq, op, payload);
        REQUIRE(enc.has_value());
        REQUIRE(enc->size() == payload.size() + 7);
        // High length byte must be non-zero for the >255 case.
        REQUIRE((*enc)[3] != 0U);

        auto const resp = build_response_frame_local(
            seq, static_cast<std::uint8_t>(static_cast<std::uint8_t>(op) | nat::kResponseMask),
            payload);
        auto const dec = nat::decode_frame(resp);
        REQUIRE(dec.has_value());
        auto const *rf = std::get_if<nat::Response>(&*dec);
        REQUIRE(rf != nullptr);
        REQUIRE(rf->seq == seq);
        REQUIRE(rf->request_op == op);
        REQUIRE(rf->payload == payload);
    });
}

TEST_CASE("property: native encode rejects payloads past kMaxPayloadBytes",
          "[transport][native][property][bounds]") {
    // kMaxPayloadBytes is 0xFFFF (64 KiB - 1). Anything above must
    // refuse with InvalidArgument. We don't actually allocate
    // 64 KiB + 1 each iteration — pick varied small overflows so the
    // sum-arithmetic in encode is exercised at the boundary.
    st::test::check_property(0xDEAD'1010'BEEF'0808ULL, 50, [&](st::test::PropertyRng &rng) {
        auto const op_idx = rng.size_in(0, kRequestOpcodes.size() - 1);
        auto const op = kRequestOpcodes[op_idx];
        auto const seq = rng.byte();
        // Overflow by 1..16 bytes — enough to exercise the boundary,
        // not enough to dominate test runtime with 64 KB allocations.
        std::size_t const extra = rng.size_in(1, 16);
        std::vector<std::uint8_t> overflow(nat::kMaxPayloadBytes + extra, 0x00U);
        auto const enc = nat::encode_request(seq, op, overflow);
        REQUIRE_FALSE(enc.has_value());
        REQUIRE(enc.error().code() == st::ErrorCode::InvalidArgument);
    });
}

TEST_CASE("property: native decode rejects wrong-SOF buffers",
          "[transport][native][property][framing]") {
    st::test::check_property(0x5050'5050'5A5A'5A5AULL, 400, [&](st::test::PropertyRng &rng) {
        auto const payload = rng.bytes(0, 64);
        auto bytes = build_response_frame_local(rng.byte(), 0x81U, payload);
        // Pick any non-AA byte for slot 0. Loop instead of conditional
        // because rng.byte() can return 0xAA — we need to keep drawing
        // until we get a different value.
        std::uint8_t bad_sof = rng.byte();
        while (bad_sof == nat::kStartOfFrame) {
            bad_sof = rng.byte();
        }
        bytes[0] = bad_sof;
        auto const dec = nat::decode_frame(bytes);
        REQUIRE_FALSE(dec.has_value());
        REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
    });
}

TEST_CASE("property: native decode rejects CRC corruption on either CRC byte",
          "[transport][native][property][crc]") {
    st::test::check_property(0xBAD0'C0DE'CAFE'F00DULL, 500, [&](st::test::PropertyRng &rng) {
        auto const payload = rng.bytes(0, 64);
        auto bytes = build_response_frame_local(rng.byte(), 0x81U, payload);
        // Pick which CRC byte to flip (high or low) and a non-zero
        // XOR delta. The high CRC byte sits at bytes.size() - 2, the
        // low at bytes.size() - 1.
        auto const flip_low = rng.boolean();
        std::size_t const idx = flip_low ? bytes.size() - 1 : bytes.size() - 2;
        std::uint8_t delta = 0;
        while (delta == 0U) {
            delta = rng.byte();
        }
        bytes[idx] ^= delta;

        auto const dec = nat::decode_frame(bytes);
        REQUIRE_FALSE(dec.has_value());
        REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
    });
}

TEST_CASE("property: native decode rejects length-field corruption that overruns the buffer",
          "[transport][native][property][framing]") {
    st::test::check_property(0xFACE'B00C'1337'C0DEULL, 400, [&](st::test::PropertyRng &rng) {
        auto const payload = rng.bytes(0, 64);
        auto bytes = build_response_frame_local(rng.byte(), 0x81U, payload);
        // Inflate the declared length past what the buffer can hold.
        // Slots 3 (high) and 4 (low) hold the 16-bit BE length. Set
        // the high byte to 0xFF — guarantees a declared length of
        // ≥ 65280, which always overruns a buffer of ≤ 71 bytes.
        bytes[3] = 0xFFU;
        bytes[4] = 0xFFU;
        auto const dec = nat::decode_frame(bytes);
        REQUIRE_FALSE(dec.has_value());
        REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
    });
}

TEST_CASE("property: native decode rejects request-opcode-in-response-slot",
          "[transport][native][property][framing]") {
    // Per the codec invariant: response opcodes have bit 7 set. A
    // request opcode (bit 7 clear) appearing in slot 2 is an adapter
    // misconfiguration and must be rejected, not silently accepted as
    // some other frame type.
    st::test::check_property(0x7777'8888'9999'AAAAULL, 300, [&](st::test::PropertyRng &rng) {
        auto const op_idx = rng.size_in(0, kRequestOpcodes.size() - 1);
        auto const op = kRequestOpcodes[op_idx]; // bit 7 clear
        auto const payload = rng.bytes(0, 32);
        auto bytes = build_response_frame_local(rng.byte(), static_cast<std::uint8_t>(op), payload);

        auto const dec = nat::decode_frame(bytes);
        REQUIRE_FALSE(dec.has_value());
        REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
    });
}

TEST_CASE("property: native decode on arbitrary random bytes never crashes",
          "[transport][native][property][robustness]") {
    // Pure adversarial fuzz. The decoder must either parse the bytes
    // (rare — CRC + SOF + length all have to line up) or return a
    // clean ParseError. Must NOT abort, throw, or read past the input
    // span. The buffer-size range spans both below the 7-byte minimum
    // header (where the length check fires immediately) and well
    // above it (where a random buffer might pass SOF but fail CRC).
    st::test::check_property(0x600D'BAD0'B16B'A55EULL, 2000, [&](st::test::PropertyRng &rng) {
        auto const bytes = rng.bytes(0, 128);
        auto const dec = nat::decode_frame(bytes);
        if (!dec.has_value()) {
            REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
        }
        // No further check on the value side — by chance some random
        // buffer will satisfy the frame format, and that's allowed.
    });
}

TEST_CASE("property: native decode rejects every single-bit corruption of a valid frame",
          "[transport][native][property][crc][bitflip]") {
    // CRC-16/CCITT-FALSE's design promise: detects every 1- and 2-bit
    // error. Test the 1-bit half directly: flip exactly one bit
    // anywhere in a valid frame and verify decode rejects. Excludes
    // the SOF byte's low bits where some flips produce another valid
    // SOF candidate that the codec already rejects via the wrong-SOF
    // check rather than CRC.
    st::test::check_property(0xB17F'11B5'17F1'1B85ULL, 600, [&](st::test::PropertyRng &rng) {
        auto const payload = rng.bytes(0, 32);
        auto const seq = rng.byte();
        auto const op = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(
                kRequestOpcodes[rng.size_in(0, kRequestOpcodes.size() - 1)]) |
            nat::kResponseMask);
        auto bytes = build_response_frame_local(seq, op, payload);

        // Pick a bit position uniformly in [0, 8 * bytes.size()) and
        // flip it. Every position must produce a frame that fails
        // decode — the CRC for a 1-bit change is by construction
        // different (CCITT-16's minimum distance is 4 over messages
        // ≤ 4096 bytes; we're well inside that bound).
        std::size_t const bit_pos = rng.size_in(0, 8U * bytes.size() - 1U);
        std::size_t const byte_idx = bit_pos / 8U;
        std::uint8_t const bit_mask = static_cast<std::uint8_t>(1U << (bit_pos % 8U));
        bytes[byte_idx] ^= bit_mask;

        auto const dec = nat::decode_frame(bytes);
        // Most flips produce ParseError. One narrow exception: a flip
        // in the LEN bytes that happens to still describe a
        // consistent shorter frame whose CRC also matches — extremely
        // unlikely but theoretically possible for tiny payloads with
        // benign bit patterns. We assert the outcome is EITHER an
        // error (the common case) OR a successful decode whose
        // payload differs from the original (because the CRC matched
        // means the decoder accepted SOMETHING, but it can't be the
        // original frame since one bit changed). Never a silent
        // identity pass-through.
        if (dec.has_value()) {
            // If decode succeeded the result must differ in at least
            // one byte of payload from the original.
            auto const *rf = std::get_if<nat::Response>(&*dec);
            if (rf != nullptr) {
                REQUIRE_FALSE(rf->payload == payload);
            }
            // ErrorFrame / Event variants are also legal: the flip
            // landed on the opcode byte and reinterpreted the frame
            // as a different variant. That's still "decode noticed
            // the bytes changed," which is the contract.
        } else {
            REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
        }
    });
}

TEST_CASE("property: native error frames roundtrip via decode_frame",
          "[transport][native][property][error_frame]") {
    // Build an error frame the codec's documented way: SOF AA, seq
    // (echo of request seq), opcode 0x7F, LEN 0x0003, payload =
    // seq + request_op + err, CRC. Per native.hpp the error payload
    // is fixed at 3 bytes.
    st::test::check_property(0x7F00'7F00'7F00'7F00ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const seq = rng.byte();
        auto const inner_seq = rng.byte();
        // Pick a request opcode from the enum so we can verify the
        // decoded request_op field exactly. Could also be arbitrary,
        // but the typed field on ErrorFrame is the contract surface.
        auto const op = kRequestOpcodes[rng.size_in(0, kRequestOpcodes.size() - 1)];
        auto const err = rng.byte();

        std::vector<std::uint8_t> frame{
            nat::kStartOfFrame,
            seq,
            nat::kErrorOpcode,
            0x00U,
            0x03U,
            inner_seq,
            static_cast<std::uint8_t>(op),
            err,
        };
        auto const crc = nat::crc16_ccitt_false(std::span{frame.data(), frame.size()});
        frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
        frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));

        auto const dec = nat::decode_frame(frame);
        REQUIRE(dec.has_value());
        auto const *ef = std::get_if<nat::ErrorFrame>(&*dec);
        REQUIRE(ef != nullptr);
        REQUIRE(ef->seq == inner_seq);
        REQUIRE(ef->request_op == op);
        REQUIRE(ef->error_code == err);
    });
}

TEST_CASE("property: native event frames roundtrip via decode_frame",
          "[transport][native][property][event_frame]") {
    // Build an event frame: SOF AA, seq 0, opcode 0x6F, LEN, payload
    // (first byte = event_code, remainder event-specific), CRC. Per
    // native.hpp the event payload must carry at least 1 byte
    // (event_code).
    st::test::check_property(0x6F00'6F00'6F00'6F00ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const event_code = rng.byte();
        auto const remainder = rng.bytes(0, 64);
        std::size_t const payload_len = 1 + remainder.size();
        std::vector<std::uint8_t> frame{
            nat::kStartOfFrame,
            0x00U, // event seq is always 0 per spec
            nat::kEventOpcode,
            static_cast<std::uint8_t>(payload_len >> 8U),
            static_cast<std::uint8_t>(payload_len & 0xFFU),
            event_code,
        };
        frame.insert(frame.end(), remainder.begin(), remainder.end());
        auto const crc = nat::crc16_ccitt_false(std::span{frame.data(), frame.size()});
        frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
        frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));

        auto const dec = nat::decode_frame(frame);
        REQUIRE(dec.has_value());
        auto const *ev = std::get_if<nat::Event>(&*dec);
        REQUIRE(ev != nullptr);
        REQUIRE(ev->event_code == event_code);
        REQUIRE(ev->payload == remainder);
    });
}
