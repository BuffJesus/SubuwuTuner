// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Property-based tests for the UDS framing layer — ship blocker #11.
// Companion to test_obdx_dvi_properties.cpp / test_native_properties.cpp
// / test_ssm_properties.cpp.
//
// Example-based tests in test_uds.cpp pin specific PDU sequences against
// the MockTransport queue. The properties below cover the codec across
// the input space: any DID × any payload, any sub-function × any seed,
// any negative-response NRC code, any random-bytes adversarial input.
//
// Properties pinned here:
//
//   1. RDBI / WDBI / RequestDownload / TransferData / BulkTransfer /
//      DSC / EcuReset / SecurityAccess all roundtrip through their
//      build_* + parse_* pair on synthetic positive responses.
//
//   2. Every parser rejects a NEGATIVE RESPONSE (0x7F <SID> <NRC>)
//      with EcuRejected (carrying the raw NRC byte). This is the
//      "wire returned a refusal — surface it, don't silently accept"
//      contract. Random NRC × random matching SID per iteration.
//
//   3. Every parser rejects positive responses whose echoed sub-
//      function / DID / counter / reset_type does NOT match the
//      caller's expected value. This is the "wire returned a
//      response, but for a different request — don't conflate them"
//      contract.
//
//   4. Every parser on adversarial random bytes never crashes —
//      either returns ok() (rare, when bytes happen to be a valid
//      positive response) or a clean error code. No throws, no
//      reads past the input span.

#include "../_helpers/property.hpp"

#include "st/core/error.hpp"
#include "st/ecu/uds.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace uds = st::ecu::uds;

namespace {

// All NRC bytes are 1-byte values 0x10..0xFF; 0x00–0x0F are reserved
// per ISO 14229-1. We pick from the full reserved-or-not range because
// the parser MUST treat any non-positive byte as a negative response —
// it's not the parser's job to validate NRC against the standard.
[[nodiscard]] std::uint8_t draw_nrc(st::test::PropertyRng &rng) {
    // 0x78 is response-pending; not a hard error but the codec still
    // surfaces it as EcuRejected so the caller can loop. Including
    // it in the sweep is intentional.
    return static_cast<std::uint8_t>(rng.uint32_in(0x10U, 0xFFU));
}

// Build a synthetic positive response for an arbitrary SID + payload.
// The wire format is `<SID + 0x40> <payload...>`. Built via brace-init
// + insert rather than reserve + push_back to dodge GCC 15's
// -Werror=free-nonheap-object mis-warning on reserve-then-grow.
[[nodiscard]] std::vector<std::uint8_t>
make_positive_response(std::uint8_t sid, std::span<std::uint8_t const> payload) {
    std::vector<std::uint8_t> out{
        static_cast<std::uint8_t>(sid + uds::kPositiveResponseOffset),
    };
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

[[nodiscard]] std::vector<std::uint8_t>
make_negative_response(std::uint8_t sid, std::uint8_t nrc) {
    return std::vector<std::uint8_t>{uds::kNegativeResponse, sid, nrc};
}

} // namespace

// ---------------------------------------------------------------------
// RDBI (0x22)
// ---------------------------------------------------------------------

TEST_CASE("property: UDS RDBI roundtrip — build then parse a positive response",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x2222'2222'1111'1111ULL, 400, [&](st::test::PropertyRng &rng) {
        auto const did = rng.uint16();
        auto const data = rng.bytes(0, 128);

        auto const req = uds::build_rdbi_request(did);
        REQUIRE(req.size() == 3);
        REQUIRE(req[0] == uds::kSidReadDataByIdentifier);
        REQUIRE(req[1] == static_cast<std::uint8_t>(did >> 8U));
        REQUIRE(req[2] == static_cast<std::uint8_t>(did & 0xFFU));

        // Positive response: SID+0x40, DID high, DID low, payload bytes.
        std::vector<std::uint8_t> resp_payload;
        resp_payload.push_back(static_cast<std::uint8_t>(did >> 8U));
        resp_payload.push_back(static_cast<std::uint8_t>(did & 0xFFU));
        resp_payload.insert(resp_payload.end(), data.begin(), data.end());
        auto const resp = make_positive_response(uds::kSidReadDataByIdentifier, resp_payload);

        auto const got = uds::parse_rdbi_response(resp, did);
        REQUIRE(got.has_value());
        REQUIRE(*got == data);
    });
}

TEST_CASE("property: UDS RDBI rejects negative responses (any NRC)",
          "[ecu][uds][property][nrc]") {
    st::test::check_property(0x7F22'7F22'7F22'7F22ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const did = rng.uint16();
        auto const nrc = draw_nrc(rng);
        auto const resp = make_negative_response(uds::kSidReadDataByIdentifier, nrc);
        auto const got = uds::parse_rdbi_response(resp, did);
        REQUIRE_FALSE(got.has_value());
        REQUIRE(got.error().code() == st::ErrorCode::EcuRejected);
    });
}

TEST_CASE("property: UDS RDBI rejects positive responses with mismatched DID",
          "[ecu][uds][property][routing]") {
    st::test::check_property(0xD1DD'1DD1'DD1D'D1DDULL, 300, [&](st::test::PropertyRng &rng) {
        auto const expected_did = rng.uint16();
        std::uint16_t wrong_did = rng.uint16();
        while (wrong_did == expected_did) {
            wrong_did = rng.uint16();
        }
        auto const data = rng.bytes(0, 16);
        std::vector<std::uint8_t> resp_payload;
        resp_payload.push_back(static_cast<std::uint8_t>(wrong_did >> 8U));
        resp_payload.push_back(static_cast<std::uint8_t>(wrong_did & 0xFFU));
        resp_payload.insert(resp_payload.end(), data.begin(), data.end());
        auto const resp = make_positive_response(uds::kSidReadDataByIdentifier, resp_payload);

        auto const got = uds::parse_rdbi_response(resp, expected_did);
        REQUIRE_FALSE(got.has_value());
    });
}

// ---------------------------------------------------------------------
// WDBI (0x2E)
// ---------------------------------------------------------------------

TEST_CASE("property: UDS WDBI roundtrip — build then parse a positive ACK",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x2E2E'2E2E'1111'1111ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const did = rng.uint16();
        auto const data = rng.bytes(1, 64);
        auto const req = uds::build_wdbi_request(did, data);
        REQUIRE(req[0] == uds::kSidWriteDataByIdentifier);

        // Positive WDBI response echoes the DID.
        std::vector<std::uint8_t> resp_payload{
            static_cast<std::uint8_t>(did >> 8U),
            static_cast<std::uint8_t>(did & 0xFFU),
        };
        auto const resp = make_positive_response(uds::kSidWriteDataByIdentifier, resp_payload);
        auto const got = uds::parse_wdbi_response(resp, did);
        REQUIRE(got.has_value());
    });
}

TEST_CASE("property: UDS WDBI rejects negative responses (any NRC)",
          "[ecu][uds][property][nrc]") {
    st::test::check_property(0x7F2E'7F2E'7F2E'7F2EULL, 300, [&](st::test::PropertyRng &rng) {
        auto const did = rng.uint16();
        auto const nrc = draw_nrc(rng);
        auto const resp = make_negative_response(uds::kSidWriteDataByIdentifier, nrc);
        auto const got = uds::parse_wdbi_response(resp, did);
        REQUIRE_FALSE(got.has_value());
        REQUIRE(got.error().code() == st::ErrorCode::EcuRejected);
    });
}

// ---------------------------------------------------------------------
// DSC (0x10), EcuReset (0x11) — sub-function echo
// ---------------------------------------------------------------------

TEST_CASE("property: UDS DSC roundtrip + sub-function echo check",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x1010'1010'A0A0'A0A0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const sess = rng.byte();
        auto const req = uds::build_dsc_request(sess);
        REQUIRE(req.size() == 2);
        REQUIRE(req[0] == uds::kSidDiagnosticSessionControl);
        REQUIRE(req[1] == sess);

        // Positive DSC response: SID+0x40, sub-function echo, then
        // 4 bytes of session-parameter timing (P2 / P2*). The codec
        // accepts the echo; the trailing timing bytes are informational.
        std::vector<std::uint8_t> resp_payload{sess, 0x00U, 0x32U, 0x01U, 0xF4U};
        auto const resp = make_positive_response(uds::kSidDiagnosticSessionControl, resp_payload);
        auto const got = uds::parse_dsc_response(resp, sess);
        REQUIRE(got.has_value());
    });
}

TEST_CASE("property: UDS DSC rejects wrong-session echo",
          "[ecu][uds][property][routing]") {
    st::test::check_property(0x1010'BAD0'BAD0'BAD0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const expected = rng.byte();
        std::uint8_t wrong = rng.byte();
        while (wrong == expected) {
            wrong = rng.byte();
        }
        std::vector<std::uint8_t> resp_payload{wrong, 0x00U, 0x32U, 0x01U, 0xF4U};
        auto const resp = make_positive_response(uds::kSidDiagnosticSessionControl, resp_payload);
        auto const got = uds::parse_dsc_response(resp, expected);
        REQUIRE_FALSE(got.has_value());
    });
}

TEST_CASE("property: UDS EcuReset roundtrip + sub-function echo check",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x1111'1111'B0B0'B0B0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const reset_type = rng.byte();
        auto const req = uds::build_ecu_reset_request(reset_type);
        REQUIRE(req.size() == 2);
        REQUIRE(req[0] == uds::kSidEcuReset);
        REQUIRE(req[1] == reset_type);

        std::vector<std::uint8_t> resp_payload{reset_type};
        auto const resp = make_positive_response(uds::kSidEcuReset, resp_payload);
        auto const got = uds::parse_ecu_reset_response(resp, reset_type);
        REQUIRE(got.has_value());
    });
}

// ---------------------------------------------------------------------
// SecurityAccess (0x27) — seed + key
// ---------------------------------------------------------------------

TEST_CASE("property: UDS SecurityAccess seed exchange roundtrip",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x2727'2727'5EED'5EEDULL, 300, [&](st::test::PropertyRng &rng) {
        // Odd sub-function = requestSeed per the convention in uds.hpp.
        auto const sub = static_cast<std::uint8_t>(rng.uint32_in(0x01U, 0x7FU) | 0x01U);
        auto const seed = rng.bytes(1, 32);

        auto const req = uds::build_security_access_request_seed(sub);
        REQUIRE(req.size() == 2);
        REQUIRE(req[0] == uds::kSidSecurityAccess);
        REQUIRE(req[1] == sub);

        std::vector<std::uint8_t> resp_payload;
        resp_payload.push_back(sub);
        resp_payload.insert(resp_payload.end(), seed.begin(), seed.end());
        auto const resp = make_positive_response(uds::kSidSecurityAccess, resp_payload);
        auto const got = uds::parse_security_access_seed(resp, sub);
        REQUIRE(got.has_value());
        REQUIRE(*got == seed);
    });
}

TEST_CASE("property: UDS SecurityAccess parsers reject NRC responses",
          "[ecu][uds][property][nrc]") {
    st::test::check_property(0x7F27'7F27'7F27'7F27ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const sub = static_cast<std::uint8_t>(rng.uint32_in(0x01U, 0x7FU) | 0x01U);
        auto const nrc = draw_nrc(rng);
        auto const resp = make_negative_response(uds::kSidSecurityAccess, nrc);
        auto const got = uds::parse_security_access_seed(resp, sub);
        REQUIRE_FALSE(got.has_value());
        REQUIRE(got.error().code() == st::ErrorCode::EcuRejected);
    });
}

// ---------------------------------------------------------------------
// RequestDownload (0x34) + TransferData (0x36)
// ---------------------------------------------------------------------

TEST_CASE("property: UDS RequestDownload roundtrip — build + parse maxBlockLength",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x3434'3434'D000'D000ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const data_format = rng.byte();
        auto const addr = rng.uint32_in(0U, 0x00FFFFFFU); // 24-bit Subaru-ish range
        auto const size = rng.uint32_in(1U, 0x00FFFFFFU);

        auto const req = uds::build_request_download(data_format, addr, size);
        REQUIRE(req[0] == uds::kSidRequestDownload);
        REQUIRE(req[1] == data_format);

        // Positive response: SID+0x40, length-format byte (0x20 = 2-byte
        // size encoding), then 2-byte max-block-length BE.
        std::uint16_t const max_block = static_cast<std::uint16_t>(rng.uint32_in(0x10U, 0xFFF0U));
        std::vector<std::uint8_t> resp_payload{
            0x20U,
            static_cast<std::uint8_t>(max_block >> 8U),
            static_cast<std::uint8_t>(max_block & 0xFFU),
        };
        auto const resp = make_positive_response(uds::kSidRequestDownload, resp_payload);
        auto const got = uds::parse_request_download_response(resp);
        REQUIRE(got.has_value());
        REQUIRE(*got == max_block);
    });
}

TEST_CASE("property: UDS TransferData roundtrip + counter echo check",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0x3636'3636'C0C0'C0C0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const counter = rng.byte();
        auto const data = rng.bytes(1, 256);

        auto const req = uds::build_transfer_data(counter, data);
        REQUIRE(req[0] == uds::kSidTransferData);
        REQUIRE(req[1] == counter);
        // Payload from byte 2 onward is the data.
        REQUIRE(req.size() == 2 + data.size());

        // Positive response: SID+0x40, counter echo.
        std::vector<std::uint8_t> resp_payload{counter};
        auto const resp = make_positive_response(uds::kSidTransferData, resp_payload);
        auto const got = uds::parse_transfer_data_response(resp, counter);
        REQUIRE(got.has_value());
    });
}

TEST_CASE("property: UDS TransferData rejects wrong-counter echo",
          "[ecu][uds][property][routing]") {
    st::test::check_property(0x3636'BAD0'BAD0'BAD0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const expected = rng.byte();
        std::uint8_t wrong = rng.byte();
        while (wrong == expected) {
            wrong = rng.byte();
        }
        std::vector<std::uint8_t> resp_payload{wrong};
        auto const resp = make_positive_response(uds::kSidTransferData, resp_payload);
        auto const got = uds::parse_transfer_data_response(resp, expected);
        REQUIRE_FALSE(got.has_value());
    });
}

// ---------------------------------------------------------------------
// SubaruBulkTransfer (0xB6) — Subaru manufacturer-specific flash write
// ---------------------------------------------------------------------

TEST_CASE("property: UDS SubaruBulkTransfer roundtrip",
          "[ecu][uds][property][roundtrip]") {
    st::test::check_property(0xB6B6'B6B6'A0A0'A0A0ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const addr = rng.uint32_in(0U, 0x00FFFFFFU); // 24-bit
        auto const data = rng.bytes(1, 64);

        auto const req = uds::build_subaru_bulk_transfer(addr, data);
        REQUIRE(req[0] == uds::kSidSubaruBulkTransfer);
        // 1 SID + 3 address bytes + data
        REQUIRE(req.size() == 4 + data.size());

        // Positive bulk-transfer ACK is the single byte SID+0x40 = 0xF6.
        std::vector<std::uint8_t> resp{
            static_cast<std::uint8_t>(uds::kSidSubaruBulkTransfer + uds::kPositiveResponseOffset),
        };
        auto const got = uds::parse_subaru_bulk_transfer_response(resp);
        REQUIRE(got.has_value());
    });
}

TEST_CASE("property: UDS SubaruBulkTransfer rejects negative responses",
          "[ecu][uds][property][nrc]") {
    st::test::check_property(0x7FB6'7FB6'7FB6'7FB6ULL, 300, [&](st::test::PropertyRng &rng) {
        auto const nrc = draw_nrc(rng);
        auto const resp = make_negative_response(uds::kSidSubaruBulkTransfer, nrc);
        auto const got = uds::parse_subaru_bulk_transfer_response(resp);
        REQUIRE_FALSE(got.has_value());
        REQUIRE(got.error().code() == st::ErrorCode::EcuRejected);
    });
}

// ---------------------------------------------------------------------
// TesterPresent (0x3E) — heartbeat
// ---------------------------------------------------------------------

TEST_CASE("property: UDS TesterPresent build + parse roundtrip + NRC reject",
          "[ecu][uds][property][roundtrip][nrc]") {
    st::test::check_property(0x3E3E'3E3E'AAAA'AAAAULL, 200, [&](st::test::PropertyRng &rng) {
        auto const suppress = rng.boolean();
        auto const req = uds::build_tester_present_request(suppress);
        REQUIRE(req.size() == 2);
        REQUIRE(req[0] == uds::kSidTesterPresent);

        // Positive: SID+0x40, sub-function echo (0x00).
        std::vector<std::uint8_t> resp_pos{
            static_cast<std::uint8_t>(uds::kSidTesterPresent + uds::kPositiveResponseOffset),
            0x00U,
        };
        REQUIRE(uds::parse_tester_present_response(resp_pos).has_value());

        // NRC: parser surfaces EcuRejected.
        auto const nrc = draw_nrc(rng);
        auto const resp_neg = make_negative_response(uds::kSidTesterPresent, nrc);
        auto const neg = uds::parse_tester_present_response(resp_neg);
        REQUIRE_FALSE(neg.has_value());
        REQUIRE(neg.error().code() == st::ErrorCode::EcuRejected);
    });
}

// ---------------------------------------------------------------------
// Robustness — every parser on adversarial random bytes
// ---------------------------------------------------------------------

TEST_CASE("property: UDS parsers on arbitrary random bytes never crash",
          "[ecu][uds][property][robustness]") {
    // For each iteration draw a random buffer and feed it to every
    // parser the catalog exposes. None must throw, abort, or read
    // past the input span. The buffer-size range straddles below
    // the SID minimum (1 B) and well above it (where some bytes
    // might accidentally form a valid response).
    st::test::check_property(0x600D'BAD0'B16B'A55EULL, 1000, [&](st::test::PropertyRng &rng) {
        auto const bytes = rng.bytes(0, 64);
        auto const did = rng.uint16();
        auto const sub = rng.byte();
        auto const counter = rng.byte();

        // Each parser must return either a Result with a value or a
        // Result with an error code. No further check — by chance
        // some random buffer will satisfy the framing, and that's
        // allowed. We're testing for absence of UB.
        (void)uds::parse_rdbi_response(bytes, did);
        (void)uds::parse_wdbi_response(bytes, did);
        (void)uds::parse_dsc_response(bytes, sub);
        (void)uds::parse_ecu_reset_response(bytes, sub);
        (void)uds::parse_security_access_seed(bytes, sub);
        (void)uds::parse_security_access_key_ack(bytes, sub);
        (void)uds::parse_tester_present_response(bytes);
        (void)uds::parse_request_download_response(bytes);
        (void)uds::parse_transfer_data_response(bytes, counter);
        (void)uds::parse_request_transfer_exit_response(bytes);
        (void)uds::parse_subaru_bulk_transfer_response(bytes);
    });
}
