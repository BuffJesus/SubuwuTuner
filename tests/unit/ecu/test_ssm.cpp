// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/ssm.hpp"
#include "st/ecu/subaru_security.hpp"
#include "st/ecu/uds.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;

namespace ssm = st::ecu::ssm;

TEST_CASE("ssm_checksum matches simple sum mod 256", "[ssm][framing]") {
    std::vector<std::uint8_t> const bytes{0x80, 0x10, 0xF0, 0x05};
    REQUIRE(ssm::ssm_checksum(bytes) == static_cast<std::uint8_t>(0x80 + 0x10 + 0xF0 + 0x05));
}

TEST_CASE("ssm_checksum wraps at 256", "[ssm][framing]") {
    std::vector<std::uint8_t> const bytes(4, 0xFF);
    REQUIRE(ssm::ssm_checksum(bytes) == static_cast<std::uint8_t>(0xFC));
}

TEST_CASE("build_a8_request constructs the canonical single-address frame", "[ssm][framing]") {
    std::uint32_t const addr{0x012345};
    std::vector<std::uint32_t> const addrs{addr};
    auto const r = ssm::build_a8_request(addrs);
    REQUIRE(r.has_value());

    // Expected frame:
    //   80 10 F0 05 A8 00 01 23 45 [csum]
    //   LEN = 1(cmd) + 1(pad) + 3(addr) = 5
    std::vector<std::uint8_t> const expect_no_csum{0x80, 0x10, 0xF0, 0x05, 0xA8,
                                                   0x00, 0x01, 0x23, 0x45};
    REQUIRE(std::vector<std::uint8_t>(r->begin(), r->end() - 1) == expect_no_csum);

    std::uint8_t const expected_csum = ssm::ssm_checksum(expect_no_csum);
    REQUIRE(r->back() == expected_csum);
}

TEST_CASE("build_a8_request handles multiple addresses", "[ssm][framing]") {
    std::vector<std::uint32_t> const addrs{0x000000, 0x000001, 0x000002};
    auto const r = ssm::build_a8_request(addrs);
    REQUIRE(r.has_value());
    // LEN = 1 + 1 + 3*3 = 11
    REQUIRE(r->at(3) == 0x0B);
    // Total frame size = 4 header bytes + 11 payload + 1 csum = 16
    REQUIRE(r->size() == 16);
}

TEST_CASE("build_a8_request rejects empty input", "[ssm][framing][error]") {
    std::vector<std::uint32_t> const addrs;
    auto const r = ssm::build_a8_request(addrs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_a8_request rejects addresses past 24-bit", "[ssm][framing][error]") {
    std::vector<std::uint32_t> const addrs{0x01000000};
    auto const r = ssm::build_a8_request(addrs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_a8_request rejects payload that overflows LEN byte", "[ssm][framing][error]") {
    // (LEN_max - 2) / 3 = (255 - 2) / 3 = 84 addresses is the cap.
    std::vector<std::uint32_t> too_many(85, 0x000001);
    auto const r = ssm::build_a8_request(too_many);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_a8_response extracts data and validates checksum", "[ssm][framing]") {
    // 80 F0 10 03 E8 12 34 [csum]
    // LEN = 1(rsp) + 2(data) = 3
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x03, 0xE8, 0x12, 0x34};
    resp.push_back(ssm::ssm_checksum(resp));

    auto const r = ssm::parse_a8_response(resp, /*expected_n=*/2);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0x12, 0x34});
}

TEST_CASE("parse_a8_response flags a bad checksum", "[ssm][framing][error]") {
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x03, 0xE8, 0x12, 0x34, 0xDE};
    // 0xDE is intentionally wrong.
    auto const r = ssm::parse_a8_response(resp, 2);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::BadChecksum);
}

TEST_CASE("parse_a8_response flags wrong addressing", "[ssm][framing][error]") {
    std::vector<std::uint8_t> resp{0x80, 0xAA, 0xBB, 0x02, 0xE8, 0x12};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_a8_response(resp, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_a8_response surfaces an ECU negative response as EcuRejected",
          "[ssm][framing][error]") {
    // 80 F0 10 02 7F 12 [csum] -- NRC 0x12 (subFunctionNotSupported)
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0x7F, 0x12};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_a8_response(resp, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_a8_response flags a data-count mismatch", "[ssm][framing][error]") {
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x03, 0xE8, 0x12, 0x34};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_a8_response(resp, /*expected_n=*/5);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("SsmClient::read drives MockTransport with the right frame", "[ssm][client]") {
    std::vector<std::uint32_t> const addrs{0x000008};

    auto const req = ssm::build_a8_request(addrs);
    REQUIRE(req.has_value());

    // Build the matching response: one data byte 0x42.
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0xE8, 0x42};
    resp.push_back(ssm::ssm_checksum(resp));

    st::transport::MockTransport t;
    t.expect_send_recv(*req, resp);
    REQUIRE(t.open({st::transport::LinkKind::KLine, 10400}).has_value());

    ssm::SsmClient client{t};
    auto const r = client.read(addrs, 100ms);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0x42});
    REQUIRE(t.exhausted());
}

TEST_CASE("SsmClient::read_block reads N contiguous bytes", "[ssm][client][block]") {
    std::vector<std::uint32_t> const addrs{0x001000, 0x001001, 0x001002, 0x001003};

    auto const req = ssm::build_a8_request(addrs);
    REQUIRE(req.has_value());

    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x05, 0xE8, 0xDE, 0xAD, 0xBE, 0xEF};
    resp.push_back(ssm::ssm_checksum(resp));

    st::transport::MockTransport t;
    t.expect_send_recv(*req, resp);
    REQUIRE(t.open({}).has_value());

    ssm::SsmClient client{t};
    auto const r = client.read_block(0x001000, 4, 100ms);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("SsmClient::read propagates transport errors", "[ssm][client][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.inject_error(st::ErrorCode::TransportTimeout, "no reply");

    std::vector<std::uint32_t> const addrs{0x000008};
    ssm::SsmClient client{t};
    auto const r = client.read(addrs, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
}

TEST_CASE("SsmClient::read_block with length=0 returns empty", "[ssm][client][block]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    ssm::SsmClient client{t};
    auto const r = client.read_block(0, 0);
    REQUIRE(r.has_value());
    REQUIRE(r->empty());
    // Nothing should have been sent to the transport.
    REQUIRE(t.send_log().empty());
}

// ---- Write tests --------------------------------------------------------

TEST_CASE("build_b0_request constructs the canonical write frame", "[ssm][framing][write]") {
    auto const r = ssm::build_b0_request(0x012345, 0x42);
    REQUIRE(r.has_value());

    // 80 10 F0 05 B0 01 23 45 42 [csum]
    std::vector<std::uint8_t> const expect_no_csum{0x80, 0x10, 0xF0, 0x05, 0xB0,
                                                   0x01, 0x23, 0x45, 0x42};
    REQUIRE(std::vector<std::uint8_t>(r->begin(), r->end() - 1) == expect_no_csum);
    REQUIRE(r->back() == ssm::ssm_checksum(expect_no_csum));
}

TEST_CASE("build_b0_request rejects addresses past 24-bit", "[ssm][framing][write][error]") {
    auto const r = ssm::build_b0_request(0x01000000, 0x00);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_b0_response returns the echoed byte", "[ssm][framing][write]") {
    // 80 F0 10 02 F0 42 [csum]   LEN = 1(RSP) + 1(echoed)
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0xF0, 0x42};
    resp.push_back(ssm::ssm_checksum(resp));

    auto const r = ssm::parse_b0_response(resp);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0x42);
}

TEST_CASE("parse_b0_response surfaces a write negative response", "[ssm][framing][write][error]") {
    // 80 F0 10 02 7F 35 [csum]   NRC 0x35 = invalid key (example)
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0x7F, 0x35};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_b0_response(resp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_b0_response flags a bad checksum", "[ssm][framing][write][error]") {
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0xF0, 0x42, 0x00};
    auto const r = ssm::parse_b0_response(resp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::BadChecksum);
}

TEST_CASE("SsmClient::write end-to-end through MockTransport", "[ssm][client][write]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    auto const req = ssm::build_b0_request(0x001234, 0x7E);
    REQUIRE(req.has_value());

    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0xF0, 0x7E};
    resp.push_back(ssm::ssm_checksum(resp));

    t.expect_send_recv(*req, resp);

    ssm::SsmClient client{t};
    auto const r = client.write(0x001234, 0x7E);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0x7E);
    REQUIRE(t.exhausted());
}

TEST_CASE("SsmClient::write detects an ECU that echoed the wrong byte", "[ssm][client][write]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    auto const req = ssm::build_b0_request(0x001234, 0xAB);
    REQUIRE(req.has_value());

    // ECU echoes 0xCD instead of 0xAB -- the caller is responsible for
    // comparing, but our client returns whatever was echoed.
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0xF0, 0xCD};
    resp.push_back(ssm::ssm_checksum(resp));

    t.expect_send_recv(*req, resp);

    ssm::SsmClient client{t};
    auto const r = client.write(0x001234, 0xAB);
    REQUIRE(r.has_value());
    REQUIRE(*r != 0xAB);
    REQUIRE(*r == 0xCD);
}

// ---- Block write (B8 / F8) --------------------------------------------

TEST_CASE("build_b8_request constructs the canonical block-write frame",
          "[ssm][framing][block_write]") {
    std::vector<std::uint8_t> const data{0xAA, 0xBB, 0xCC, 0xDD};
    auto const r = ssm::build_b8_request(0x012345, data);
    REQUIRE(r.has_value());

    // 80 10 F0 [LEN=8] B8 01 23 45 AA BB CC DD [csum]   (LEN = 4 + 4)
    std::vector<std::uint8_t> const expect_no_csum{0x80, 0x10, 0xF0, 0x08, 0xB8, 0x01,
                                                   0x23, 0x45, 0xAA, 0xBB, 0xCC, 0xDD};
    REQUIRE(std::vector<std::uint8_t>(r->begin(), r->end() - 1) == expect_no_csum);
    REQUIRE(r->back() == ssm::ssm_checksum(expect_no_csum));
}

TEST_CASE("build_b8_request rejects an empty payload", "[ssm][framing][block_write][error]") {
    std::vector<std::uint8_t> const empty;
    auto const r = ssm::build_b8_request(0x000000, empty);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_b8_request rejects payloads past the single-frame LEN limit",
          "[ssm][framing][block_write][error]") {
    std::vector<std::uint8_t> const oversize(ssm::kMaxBlockWriteBytes + 1U, 0x00);
    auto const r = ssm::build_b8_request(0x000000, oversize);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_b8_request rejects ranges that overflow 24-bit",
          "[ssm][framing][block_write][error]") {
    std::vector<std::uint8_t> const data(16, 0xFF);
    // Last address would be 0x00FFFFFF + 16 - 1 > kMaxAddress.
    auto const r = ssm::build_b8_request(ssm::kMaxAddress - 8U, data);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_b8_response returns the echoed bytes", "[ssm][framing][block_write]") {
    // 80 F0 10 [LEN=5] F8 AA BB CC DD [csum]   LEN = 1(RSP) + 4(echo)
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x05, 0xF8, 0xAA, 0xBB, 0xCC, 0xDD};
    resp.push_back(ssm::ssm_checksum(resp));

    auto const r = ssm::parse_b8_response(resp, 4);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD});
}

TEST_CASE("parse_b8_response flags an echo-count mismatch", "[ssm][framing][block_write][error]") {
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x05, 0xF8, 0xAA, 0xBB, 0xCC, 0xDD};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_b8_response(resp, 3);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_b8_response surfaces a negative response as EcuRejected",
          "[ssm][framing][block_write][error]") {
    // 80 F0 10 02 7F 12 [csum]    NRC 0x12 (sub-function not supported)
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x02, 0x7F, 0x12};
    resp.push_back(ssm::ssm_checksum(resp));
    auto const r = ssm::parse_b8_response(resp, 4);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_b8_response flags a bad checksum", "[ssm][framing][block_write][error]") {
    std::vector<std::uint8_t> const resp{0x80, 0xF0, 0x10, 0x05, 0xF8,
                                         0xAA, 0xBB, 0xCC, 0xDD, 0x00};
    auto const r = ssm::parse_b8_response(resp, 4);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::BadChecksum);
}

TEST_CASE("SsmClient::write_block end-to-end through MockTransport", "[ssm][client][block_write]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<std::uint8_t> const payload{0x11, 0x22, 0x33};
    auto const req = ssm::build_b8_request(0x00ABCD, payload);
    REQUIRE(req.has_value());

    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x04, 0xF8, 0x11, 0x22, 0x33};
    resp.push_back(ssm::ssm_checksum(resp));

    t.expect_send_recv(*req, resp);

    ssm::SsmClient client{t};
    auto const r = client.write_block(0x00ABCD, payload);
    REQUIRE(r.has_value());
    REQUIRE(*r == payload);
    REQUIRE(t.exhausted());
}

TEST_CASE("SsmClient::write_block surfaces an echo divergence", "[ssm][client][block_write]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<std::uint8_t> const payload{0xAB, 0xCD};
    auto const req = ssm::build_b8_request(0x000010, payload);
    REQUIRE(req.has_value());

    // ECU echoes 0xAB 0xFF -- second byte diverges. Client returns the echo;
    // verifying it against `payload` is the caller's responsibility.
    std::vector<std::uint8_t> resp{0x80, 0xF0, 0x10, 0x03, 0xF8, 0xAB, 0xFF};
    resp.push_back(ssm::ssm_checksum(resp));
    t.expect_send_recv(*req, resp);

    ssm::SsmClient client{t};
    auto const r = client.write_block(0x000010, payload);
    REQUIRE(r.has_value());
    REQUIRE(*r != payload);
    REQUIRE((*r)[1] == 0xFF);
}

// =====================================================================
// SSM-over-CAN (ISO-TP) framing — bare-payload variant for 2008+ Subarus
// =====================================================================
//
// On CAN, the K-Line wrapper (80 10 F0 LEN ... CSUM) is stripped — CAN
// IDs carry addressing, ISO-TP carries length, and CAN's frame CRC makes
// the SSM checksum redundant. Diagnosed against a real 2017 VA WRX:
// emitting K-Line-shaped frames on CAN caused the adapter's ISO-TP TX
// to hang waiting for a Flow Control the ECU never sent.

TEST_CASE("build_a8_request IsoTp omits the K-Line wrapper", "[ssm][framing][isotp]") {
    std::vector<std::uint32_t> const addrs{0x000008};
    auto const r = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    // Expected wire bytes per the Autosport Labs SSM-over-OBDII writeup:
    // single-byte read at 0x000008 emits `A8 00 00 00 08` (5 bytes), no
    // 0x80 header, no LEN byte, no CSUM.
    REQUIRE(*r == std::vector<std::uint8_t>{0xA8, 0x00, 0x00, 0x00, 0x08});
}

TEST_CASE("build_a8_request IsoTp handles multi-address requests past KLine LEN cap",
          "[ssm][framing][isotp]") {
    // K-Line caps at 84 addresses (LEN byte). IsoTp has no such limit
    // — ISO-TP's 12-bit length field allows up to 4095 bytes per
    // request.
    std::vector<std::uint32_t> addrs(100, 0x000001);
    auto const r = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    // 2 (CMD + PAD) + 100*3 = 302 bytes
    REQUIRE(r->size() == 302);
    REQUIRE((*r)[0] == 0xA8);
    REQUIRE((*r)[1] == 0x00);
}

TEST_CASE("parse_a8_response IsoTp extracts data from bare payload",
          "[ssm][framing][isotp]") {
    // ECU response on 0x7E8: just `E8 <data...>` — no wrapper.
    std::vector<std::uint8_t> const resp{0xE8, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = ssm::parse_a8_response(resp, /*expected_n=*/4, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_a8_response IsoTp surfaces negative response as EcuRejected",
          "[ssm][framing][isotp][error]") {
    // `7F NRC` — minimum 2 bytes, NRC 0x33 = securityAccessDenied.
    std::vector<std::uint8_t> const resp{0x7F, 0x33};
    auto const r = ssm::parse_a8_response(resp, 0, ssm::Framing::IsoTp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_a8_response IsoTp flags data-count mismatch", "[ssm][framing][isotp][error]") {
    std::vector<std::uint8_t> const resp{0xE8, 0x12, 0x34};
    auto const r = ssm::parse_a8_response(resp, /*expected_n=*/5, ssm::Framing::IsoTp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("build_b0_request IsoTp produces a bare write frame", "[ssm][framing][isotp][write]") {
    auto const r = ssm::build_b0_request(0x012345, 0x42, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    // `B0 01 23 45 42` — 5 bytes, no wrapper, no checksum.
    REQUIRE(*r == std::vector<std::uint8_t>{0xB0, 0x01, 0x23, 0x45, 0x42});
}

TEST_CASE("parse_b0_response IsoTp returns the echoed byte", "[ssm][framing][isotp][write]") {
    std::vector<std::uint8_t> const resp{0xF0, 0x42};
    auto const r = ssm::parse_b0_response(resp, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0x42);
}

TEST_CASE("build_b8_request IsoTp produces a bare block-write frame",
          "[ssm][framing][isotp][write][block]") {
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = ssm::build_b8_request(0x000010, data, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    // `B8 00 00 10 DE AD BE EF` — 8 bytes, no wrapper.
    REQUIRE(*r ==
            std::vector<std::uint8_t>{0xB8, 0x00, 0x00, 0x10, 0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_b8_response IsoTp returns the echoed bytes",
          "[ssm][framing][isotp][write][block]") {
    std::vector<std::uint8_t> const resp{0xF8, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = ssm::parse_b8_response(resp, /*expected_n=*/4, ssm::Framing::IsoTp);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("SsmClient with IsoTp framing emits bare-payload frames", "[ssm][client][isotp]") {
    std::vector<std::uint32_t> const addrs{0x000008};
    auto const expected_req = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(expected_req.has_value());
    REQUIRE(*expected_req == std::vector<std::uint8_t>{0xA8, 0x00, 0x00, 0x00, 0x08});

    // Bare-payload response: `E8 42`.
    std::vector<std::uint8_t> const resp{0xE8, 0x42};

    st::transport::MockTransport t;
    t.expect_send_recv(*expected_req, resp);
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    ssm::SsmClient client{t, ssm::Framing::IsoTp};
    REQUIRE(client.framing() == ssm::Framing::IsoTp);
    auto const r = client.read(addrs, 100ms);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0x42});
    REQUIRE(t.exhausted());
}

TEST_CASE("SsmClient IsoTp sustains repeated multi-address polls",
          "[ssm][client][isotp][poll]") {
    // Models the wire shape the `ssm-a8-poll` CLI emits: the same
    // multi-address A8 request goes out each cycle; the ECU response
    // is a fresh value per byte each time. The CLI poll loop drives
    // SsmClient::read() in a fixed-interval loop and expects the
    // per-cycle responses to come back in request order.
    std::vector<std::uint32_t> const addrs{0x00000E, 0x00000F, 0x000008};
    auto const expected_req = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(expected_req.has_value());
    REQUIRE(*expected_req ==
            std::vector<std::uint8_t>{0xA8, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x0F, 0x00, 0x00,
                                      0x08});

    std::vector<std::vector<std::uint8_t>> const cycles{
        {0xE8, 0x0C, 0xC0, 0x52},  // RPM hi/lo, coolant raw — idle-ish
        {0xE8, 0x0C, 0xD2, 0x52},  // RPM ticked up
        {0xE8, 0x0D, 0x44, 0x53},  // RPM higher, coolant +1
    };

    st::transport::MockTransport t;
    for (auto const &resp : cycles) {
        t.expect_send_recv(*expected_req, resp);
    }
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    ssm::SsmClient client{t, ssm::Framing::IsoTp};
    std::vector<std::vector<std::uint8_t>> got;
    for (std::size_t i = 0; i < cycles.size(); ++i) {
        auto const r = client.read(addrs, 100ms);
        REQUIRE(r.has_value());
        REQUIRE(r->size() == addrs.size());
        got.push_back(*r);
    }
    REQUIRE(t.exhausted());

    REQUIRE(got[0] == std::vector<std::uint8_t>{0x0C, 0xC0, 0x52});
    REQUIRE(got[1] == std::vector<std::uint8_t>{0x0C, 0xD2, 0x52});
    REQUIRE(got[2] == std::vector<std::uint8_t>{0x0D, 0x44, 0x53});
}

TEST_CASE("ssm-a8-poll SA preamble: DSC + L3 unlock then poll on same transport",
          "[ssm][client][isotp][poll][sa]") {
    // Models the cmd_ssm_a8_poll preamble flow when the ECU returns
    // NRC 0x33 on bare A8 (COBB-tuned ECU lockdown). Pre-unlock with
    // DSC extendedDiagnosticSession + SA L3 via the captured-validated
    // Fehr-active L3 seed/key pair, then SSM-A8 polls succeed.
    //
    // Real captured pair (memory project_fehr_sa_l3_resolved):
    //   seed = 0x4ADFFE07 -> key = 0x24243A06
    std::vector<std::uint8_t> const seed_be{0x4A, 0xDF, 0xFE, 0x07};
    std::vector<std::uint8_t> const key_be{0x24, 0x24, 0x3A, 0x06};

    st::transport::MockTransport t;
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    // 1) DSC extendedDiagnostic.
    t.expect_send_recv({0x10, 0x03}, {0x50, 0x03});
    // 2) SecurityAccess L3 requestSeed.
    t.expect_send_recv({0x27, 0x03}, {0x67, 0x03, 0x4A, 0xDF, 0xFE, 0x07});
    // 3) SecurityAccess L3 sendKey (sub_function = level + 1 = 0x04).
    t.expect_send_recv({0x27, 0x04, 0x24, 0x24, 0x3A, 0x06}, {0x67, 0x04});
    // 4) SSM-A8 poll for RPM (hi byte of canonical OEM address).
    std::vector<std::uint32_t> const addrs{0x00000E};
    auto const a8_req = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(a8_req.has_value());
    t.expect_send_recv(*a8_req, {0xE8, 0x82});

    st::ecu::uds::UdsClient uds_client{t};
    REQUIRE(uds_client.diagnostic_session_control(st::ecu::uds::kDscExtendedDiagnostic).has_value());

    auto const seed = uds_client.security_access_request_seed(0x03, 100ms);
    REQUIRE(seed.has_value());
    REQUIRE(*seed == seed_be);

    auto const key = st::ecu::subaru::ssmcan1_l3_aftermarket(*seed);
    REQUIRE(key.has_value());
    REQUIRE(*key == key_be);

    REQUIRE(uds_client.security_access_send_key(0x04, *key, 100ms).has_value());

    ssm::SsmClient ssm_client{t, ssm::Framing::IsoTp};
    auto const polled = ssm_client.read(addrs, 100ms);
    REQUIRE(polled.has_value());
    REQUIRE(*polled == std::vector<std::uint8_t>{0x82});

    REQUIRE(t.exhausted());
}

TEST_CASE("ssm-a8-poll SA preamble: NRC 0x33 on bare A8 surfaces as EcuRejected",
          "[ssm][client][isotp][poll][sa][error]") {
    // The trigger for invoking --authenticate in the field: ECU returns
    // NRC 0x33 (securityAccessDenied) on every poll attempt. SsmClient
    // surfaces this as EcuRejected; the CLI logs it inline as a "! ..."
    // row. Without the SA preamble, every subsequent poll keeps NRCing.
    std::vector<std::uint32_t> const addrs{0xF8D424, 0xF8D425};  // RPM
    auto const expected_req = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(expected_req.has_value());

    st::transport::MockTransport t;
    t.expect_send_recv(*expected_req, {0x7F, 0xA8, 0x33});
    t.expect_send_recv(*expected_req, {0x7F, 0xA8, 0x33});
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    ssm::SsmClient client{t, ssm::Framing::IsoTp};

    for (int i = 0; i < 2; ++i) {
        auto const r = client.read(addrs, 100ms);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    }
    REQUIRE(t.exhausted());
}

TEST_CASE("SsmClient IsoTp poll loop tolerates mid-stream NRC and continues",
          "[ssm][client][isotp][poll][error]") {
    // The CLI poll loop logs NRC errors inline ('! <msg>' rows) and
    // keeps polling. This test confirms SsmClient::read() returns a
    // non-fatal EcuRejected on a `7F <nrc>` response without poisoning
    // the underlying transport for the next request.
    std::vector<std::uint32_t> const addrs{0x000008};
    auto const expected_req = ssm::build_a8_request(addrs, ssm::Framing::IsoTp);
    REQUIRE(expected_req.has_value());

    st::transport::MockTransport t;
    t.expect_send_recv(*expected_req, {0xE8, 0x40});           // good
    t.expect_send_recv(*expected_req, {0x7F, 0x12});           // NRC subFnNotSupported
    t.expect_send_recv(*expected_req, {0xE8, 0x42});           // good again
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    ssm::SsmClient client{t, ssm::Framing::IsoTp};

    auto const r1 = client.read(addrs, 100ms);
    REQUIRE(r1.has_value());
    REQUIRE(*r1 == std::vector<std::uint8_t>{0x40});

    auto const r2 = client.read(addrs, 100ms);
    REQUIRE_FALSE(r2.has_value());

    auto const r3 = client.read(addrs, 100ms);
    REQUIRE(r3.has_value());
    REQUIRE(*r3 == std::vector<std::uint8_t>{0x42});

    REQUIRE(t.exhausted());
}
