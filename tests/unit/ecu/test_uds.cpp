// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/uds.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;

namespace uds = st::ecu::uds;

// ---- RDBI ---------------------------------------------------------------

TEST_CASE("build_rdbi_request constructs [22 hi lo]", "[uds][framing][rdbi]") {
    auto const r = uds::build_rdbi_request(0xF190);
    REQUIRE(r == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});
}

TEST_CASE("parse_rdbi_response extracts the data payload", "[uds][framing][rdbi]") {
    std::vector<std::uint8_t> const resp{0x62, 0xF1, 0x90, 'A', 'B', '8', '0', 'U'};
    auto const                       r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{'A', 'B', '8', '0', 'U'});
}

TEST_CASE("parse_rdbi_response flags a DID mismatch", "[uds][framing][rdbi][error]") {
    std::vector<std::uint8_t> const resp{0x62, 0xF1, 0x91, 0x00};
    auto const                       r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_rdbi_response surfaces a negative response",
          "[uds][framing][rdbi][error]") {
    // [0x7F] [0x22] [0x33] — securityAccessDenied for RDBI
    std::vector<std::uint8_t> const resp{0x7F, 0x22, 0x33};
    auto const                       r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_rdbi_response flags an unexpected SID",
          "[uds][framing][rdbi][error]") {
    std::vector<std::uint8_t> const resp{0x63, 0xF1, 0x90, 0x00};
    auto const                       r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- WDBI ---------------------------------------------------------------

TEST_CASE("build_wdbi_request constructs [2E hi lo data...]",
          "[uds][framing][wdbi]") {
    std::vector<std::uint8_t> const data{0xDE, 0xAD};
    auto const                       r = uds::build_wdbi_request(0xABCD, data);
    REQUIRE(r == std::vector<std::uint8_t>{0x2E, 0xAB, 0xCD, 0xDE, 0xAD});
}

TEST_CASE("parse_wdbi_response accepts a positive ack",
          "[uds][framing][wdbi]") {
    std::vector<std::uint8_t> const resp{0x6E, 0xAB, 0xCD};
    REQUIRE(uds::parse_wdbi_response(resp, 0xABCD).has_value());
}

TEST_CASE("parse_wdbi_response rejects on NRC", "[uds][framing][wdbi][error]") {
    // securityAccessDenied
    std::vector<std::uint8_t> const resp{0x7F, 0x2E, 0x33};
    auto const                       r = uds::parse_wdbi_response(resp, 0xABCD);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- SecurityAccess -----------------------------------------------------

TEST_CASE("build_security_access_request_seed constructs [27 sub]",
          "[uds][framing][sa]") {
    auto const r = uds::build_security_access_request_seed(0x01);
    REQUIRE(r == std::vector<std::uint8_t>{0x27, 0x01});
}

TEST_CASE("parse_security_access_seed extracts the seed bytes",
          "[uds][framing][sa]") {
    std::vector<std::uint8_t> const resp{0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const                       r = uds::parse_security_access_seed(resp, 0x01);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_security_access_seed flags sub-function mismatch",
          "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x67, 0x03, 0xDE, 0xAD};
    auto const                       r = uds::parse_security_access_seed(resp, 0x01);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("build_security_access_send_key constructs [27 sub key...]",
          "[uds][framing][sa]") {
    std::vector<std::uint8_t> const key{0xC0, 0xDE};
    auto const                       r = uds::build_security_access_send_key(0x02, key);
    REQUIRE(r == std::vector<std::uint8_t>{0x27, 0x02, 0xC0, 0xDE});
}

TEST_CASE("parse_security_access_key_ack accepts a positive ack",
          "[uds][framing][sa]") {
    std::vector<std::uint8_t> const resp{0x67, 0x02};
    REQUIRE(uds::parse_security_access_key_ack(resp, 0x02).has_value());
}

TEST_CASE("parse_security_access_key_ack rejects invalidKey NRC",
          "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x27, 0x35}; // invalidKey
    auto const                       r = uds::parse_security_access_key_ack(resp, 0x02);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- UdsClient through MockTransport -----------------------------------

TEST_CASE("UdsClient::read_data_by_identifier round-trip",
          "[uds][client][rdbi]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    t.expect_send_recv({0x22, 0xF1, 0x90},
                       {0x62, 0xF1, 0x90, 'A', 'S', '8', '0', 'U'});

    uds::UdsClient client{t};
    auto const     r = client.read_data_by_identifier(0xF190, 100ms);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{'A', 'S', '8', '0', 'U'});
    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient::write_data_by_identifier round-trip",
          "[uds][client][wdbi]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<std::uint8_t> const payload{0x01, 0x02, 0x03};
    t.expect_send_recv({0x2E, 0xAB, 0xCD, 0x01, 0x02, 0x03},
                       {0x6E, 0xAB, 0xCD});

    uds::UdsClient client{t};
    REQUIRE(client.write_data_by_identifier(0xABCD, payload, 100ms).has_value());
    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient seed/key flow against MockTransport",
          "[uds][client][sa]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Step 1: request seed at sub 0x01, ECU returns a 4-byte seed.
    t.expect_send_recv({0x27, 0x01},
                       {0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF});
    // Step 2: send key at sub 0x02, ECU acks.
    t.expect_send_recv({0x27, 0x02, 0xC0, 0xDE, 0xBA, 0xBE},
                       {0x67, 0x02});

    uds::UdsClient client{t};

    auto const seed = client.security_access_request_seed(0x01, 100ms);
    REQUIRE(seed.has_value());
    REQUIRE(*seed == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    // Caller would compute the real key from the seed; we just send canned
    // bytes the mock expects.
    std::vector<std::uint8_t> const key{0xC0, 0xDE, 0xBA, 0xBE};
    REQUIRE(client.security_access_send_key(0x02, key, 100ms).has_value());

    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient surfaces invalidKey as EcuRejected",
          "[uds][client][sa][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    t.expect_send_recv({0x27, 0x02, 0x00, 0x00},
                       {0x7F, 0x27, 0x35});

    uds::UdsClient            client{t};
    std::vector<std::uint8_t> const bad_key{0x00, 0x00};
    auto const                       r = client.security_access_send_key(0x02, bad_key, 100ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient propagates transport errors",
          "[uds][client][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.inject_error(st::ErrorCode::TransportTimeout, "no reply");

    uds::UdsClient client{t};
    auto const     r = client.read_data_by_identifier(0xF190, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
}
