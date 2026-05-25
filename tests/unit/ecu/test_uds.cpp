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
    auto const r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{'A', 'B', '8', '0', 'U'});
}

TEST_CASE("parse_rdbi_response flags a DID mismatch", "[uds][framing][rdbi][error]") {
    std::vector<std::uint8_t> const resp{0x62, 0xF1, 0x91, 0x00};
    auto const r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_rdbi_response surfaces a negative response", "[uds][framing][rdbi][error]") {
    // [0x7F] [0x22] [0x33] -- securityAccessDenied for RDBI
    std::vector<std::uint8_t> const resp{0x7F, 0x22, 0x33};
    auto const r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_rdbi_response flags an unexpected SID", "[uds][framing][rdbi][error]") {
    std::vector<std::uint8_t> const resp{0x63, 0xF1, 0x90, 0x00};
    auto const r = uds::parse_rdbi_response(resp, 0xF190);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- WDBI ---------------------------------------------------------------

TEST_CASE("build_wdbi_request constructs [2E hi lo data...]", "[uds][framing][wdbi]") {
    std::vector<std::uint8_t> const data{0xDE, 0xAD};
    auto const r = uds::build_wdbi_request(0xABCD, data);
    REQUIRE(r == std::vector<std::uint8_t>{0x2E, 0xAB, 0xCD, 0xDE, 0xAD});
}

TEST_CASE("parse_wdbi_response accepts a positive ack", "[uds][framing][wdbi]") {
    std::vector<std::uint8_t> const resp{0x6E, 0xAB, 0xCD};
    REQUIRE(uds::parse_wdbi_response(resp, 0xABCD).has_value());
}

TEST_CASE("parse_wdbi_response rejects on NRC", "[uds][framing][wdbi][error]") {
    // securityAccessDenied
    std::vector<std::uint8_t> const resp{0x7F, 0x2E, 0x33};
    auto const r = uds::parse_wdbi_response(resp, 0xABCD);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- SecurityAccess -----------------------------------------------------

TEST_CASE("build_security_access_request_seed constructs [27 sub]", "[uds][framing][sa]") {
    auto const r = uds::build_security_access_request_seed(0x01);
    REQUIRE(r == std::vector<std::uint8_t>{0x27, 0x01});
}

TEST_CASE("parse_security_access_seed extracts the seed bytes", "[uds][framing][sa]") {
    std::vector<std::uint8_t> const resp{0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = uds::parse_security_access_seed(resp, 0x01);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_security_access_seed flags sub-function mismatch", "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x67, 0x03, 0xDE, 0xAD};
    auto const r = uds::parse_security_access_seed(resp, 0x01);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("build_security_access_send_key constructs [27 sub key...]", "[uds][framing][sa]") {
    std::vector<std::uint8_t> const key{0xC0, 0xDE};
    auto const r = uds::build_security_access_send_key(0x02, key);
    REQUIRE(r == std::vector<std::uint8_t>{0x27, 0x02, 0xC0, 0xDE});
}

TEST_CASE("parse_security_access_key_ack accepts a positive ack", "[uds][framing][sa]") {
    std::vector<std::uint8_t> const resp{0x67, 0x02};
    REQUIRE(uds::parse_security_access_key_ack(resp, 0x02).has_value());
}

TEST_CASE("parse_security_access_key_ack rejects invalidKey NRC", "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x27, 0x35}; // invalidKey
    auto const r = uds::parse_security_access_key_ack(resp, 0x02);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    // Bare NRC byte is too cryptic — the message should name the NRC and
    // warn about the lockout that follows three more attempts.
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("0x35") != std::string::npos);
    REQUIRE(msg.find("invalidKey") != std::string::npos);
    REQUIRE(msg.find("exceededNumberOfAttempts") != std::string::npos);
}

TEST_CASE("parse_security_access_key_ack on exceededNumberOfAttempts NRC includes "
          "power-cycle recovery guidance",
          "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x27, 0x36}; // exceededNumberOfAttempts
    auto const r = uds::parse_security_access_key_ack(resp, 0x02);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("0x36") != std::string::npos);
    REQUIRE(msg.find("exceededNumberOfAttempts") != std::string::npos);
    // Must be actionable, not just descriptive.
    REQUIRE(msg.find("power-cycle") != std::string::npos);
    REQUIRE(msg.find("10 minutes") != std::string::npos);
}

TEST_CASE("parse_security_access_seed on securityAccessDenied NRC names the NRC",
          "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x27, 0x33}; // securityAccessDenied
    auto const r = uds::parse_security_access_seed(resp, 0x01);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("securityAccessDenied") != std::string::npos);
}

TEST_CASE("parse_security_access_key_ack on requiredTimeDelayNotExpired NRC names the NRC",
          "[uds][framing][sa][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x27, 0x37}; // requiredTimeDelayNotExpired
    auto const r = uds::parse_security_access_key_ack(resp, 0x02);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("requiredTimeDelayNotExpired") != std::string::npos);
    REQUIRE(msg.find("~10s") != std::string::npos);
}

// ---- UdsClient through MockTransport -----------------------------------

TEST_CASE("UdsClient::read_data_by_identifier round-trip", "[uds][client][rdbi]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({st::transport::LinkKind::CanIso15765, 500000}).has_value());

    t.expect_send_recv({0x22, 0xF1, 0x90}, {0x62, 0xF1, 0x90, 'A', 'S', '8', '0', 'U'});

    uds::UdsClient client{t};
    auto const r = client.read_data_by_identifier(0xF190, 100ms);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{'A', 'S', '8', '0', 'U'});
    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient::write_data_by_identifier round-trip", "[uds][client][wdbi]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<std::uint8_t> const payload{0x01, 0x02, 0x03};
    t.expect_send_recv({0x2E, 0xAB, 0xCD, 0x01, 0x02, 0x03}, {0x6E, 0xAB, 0xCD});

    uds::UdsClient client{t};
    REQUIRE(client.write_data_by_identifier(0xABCD, payload, 100ms).has_value());
    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient seed/key flow against MockTransport", "[uds][client][sa]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Step 1: request seed at sub 0x01, ECU returns a 4-byte seed.
    t.expect_send_recv({0x27, 0x01}, {0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF});
    // Step 2: send key at sub 0x02, ECU acks.
    t.expect_send_recv({0x27, 0x02, 0xC0, 0xDE, 0xBA, 0xBE}, {0x67, 0x02});

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

TEST_CASE("UdsClient surfaces invalidKey as EcuRejected", "[uds][client][sa][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    t.expect_send_recv({0x27, 0x02, 0x00, 0x00}, {0x7F, 0x27, 0x35});

    uds::UdsClient client{t};
    std::vector<std::uint8_t> const bad_key{0x00, 0x00};
    auto const r = client.security_access_send_key(0x02, bad_key, 100ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient propagates transport errors", "[uds][client][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.inject_error(st::ErrorCode::TransportTimeout, "no reply");

    uds::UdsClient client{t};
    auto const r = client.read_data_by_identifier(0xF190, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
}

// ---- DiagnosticSessionControl ------------------------------------------

TEST_CASE("DSC request + positive response", "[uds][dsc]") {
    REQUIRE(uds::build_dsc_request(uds::kDscProgramming) == std::vector<std::uint8_t>{0x10, 0x02});

    // Real DSC responses include a 4-byte sessionParameterRecord (P2 timing).
    std::vector<std::uint8_t> const resp{0x50, 0x02, 0x00, 0x32, 0x01, 0xF4};
    REQUIRE(uds::parse_dsc_response(resp, uds::kDscProgramming).has_value());
}

TEST_CASE("DSC session mismatch is a ParseError", "[uds][dsc][error]") {
    std::vector<std::uint8_t> const resp{0x50, 0x03};
    auto const r = uds::parse_dsc_response(resp, uds::kDscProgramming);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("UdsClient::diagnostic_session_control through MockTransport", "[uds][client][dsc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x10, 0x02}, {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4});
    uds::UdsClient client{t};
    REQUIRE(client.diagnostic_session_control(uds::kDscProgramming).has_value());
}

// ---- ECUReset ----------------------------------------------------------

TEST_CASE("ECUReset request + ack", "[uds][reset]") {
    REQUIRE(uds::build_ecu_reset_request(uds::kEcuResetHard) ==
            std::vector<std::uint8_t>{0x11, 0x01});
    std::vector<std::uint8_t> const resp{0x51, 0x01};
    REQUIRE(uds::parse_ecu_reset_response(resp, uds::kEcuResetHard).has_value());
}

TEST_CASE("ECUReset NRC propagates", "[uds][reset][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x11, 0x22}; // conditionsNotCorrect
    auto const r = uds::parse_ecu_reset_response(resp, uds::kEcuResetHard);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---- TesterPresent -----------------------------------------------------

TEST_CASE("TesterPresent request defaults to ack-requested", "[uds][tester_present]") {
    REQUIRE(uds::build_tester_present_request() == std::vector<std::uint8_t>{0x3E, 0x00});
    REQUIRE(uds::build_tester_present_request(/*suppress=*/true) ==
            std::vector<std::uint8_t>{0x3E, 0x80});
}

TEST_CASE("TesterPresent ack parsed", "[uds][tester_present]") {
    std::vector<std::uint8_t> const resp{0x7E, 0x00};
    REQUIRE(uds::parse_tester_present_response(resp).has_value());
}

TEST_CASE("UdsClient::tester_present round-trip", "[uds][client][tester_present]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x3E, 0x00}, {0x7E, 0x00});
    uds::UdsClient client{t};
    REQUIRE(client.tester_present().has_value());
}

// ---- RequestDownload ---------------------------------------------------

TEST_CASE("RequestDownload encodes address + size in their minimum bytes", "[uds][download]") {
    // address 0x123456 -> 3 bytes; size 0x4000 -> 2 bytes
    // Format byte = (size_bytes << 4) | addr_bytes = 0x23
    auto const r = uds::build_request_download(0x00, 0x123456, 0x4000);
    REQUIRE(r == std::vector<std::uint8_t>{0x34, 0x00, 0x23, 0x12, 0x34, 0x56, 0x40, 0x00});
}

TEST_CASE("RequestDownload response extracts maxNumberOfBlockLength", "[uds][download]") {
    // [0x74] [0x20] [0x01 0x00] -- len-format-id high nibble=2 means 2 bytes
    // follow; 0x0100 = 256-byte max block.
    std::vector<std::uint8_t> const resp{0x74, 0x20, 0x01, 0x00};
    auto const r = uds::parse_request_download_response(resp);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0x0100U);
}

TEST_CASE("RequestDownload response rejects garbage length-format", "[uds][download][error]") {
    std::vector<std::uint8_t> const resp{0x74, 0x00, 0x00};
    auto const r = uds::parse_request_download_response(resp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("UdsClient::request_download round-trip", "[uds][client][download]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x34, 0x00, 0x23, 0x12, 0x34, 0x56, 0x40, 0x00}, {0x74, 0x20, 0x01, 0x00});
    uds::UdsClient client{t};
    auto const r = client.request_download(0x00, 0x123456, 0x4000);
    REQUIRE(r.has_value());
    REQUIRE(*r == 256);
}

// ---- TransferData ------------------------------------------------------

TEST_CASE("TransferData request packs the block", "[uds][transfer]") {
    std::vector<std::uint8_t> const data{0xAA, 0xBB, 0xCC};
    auto const r = uds::build_transfer_data(1, data);
    REQUIRE(r == std::vector<std::uint8_t>{0x36, 0x01, 0xAA, 0xBB, 0xCC});
}

TEST_CASE("TransferData response counter mismatch surfaces as ParseError",
          "[uds][transfer][error]") {
    std::vector<std::uint8_t> const resp{0x76, 0x02};
    auto const r = uds::parse_transfer_data_response(resp, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("UdsClient::transfer_data through MockTransport", "[uds][client][transfer]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x36, 0x01, 0xAA, 0xBB, 0xCC}, {0x76, 0x01});
    uds::UdsClient client{t};
    std::vector<std::uint8_t> const data{0xAA, 0xBB, 0xCC};
    REQUIRE(client.transfer_data(1, data).has_value());
}

// ---- SubaruBulkTransfer (0xB6) ----------------------------------------

TEST_CASE("build_subaru_bulk_transfer encodes address as 3 big-endian bytes",
          "[uds][framing][b6]") {
    // Wire format from cobb-reinstall-3.log: B6 + addr(3) + data.
    std::vector<std::uint8_t> const data{0xF1, 0x35, 0xFA, 0x11};
    auto const r = uds::build_subaru_bulk_transfer(0x006000, data);
    REQUIRE(r == std::vector<std::uint8_t>{0xB6, 0x00, 0x60, 0x00, 0xF1, 0x35, 0xFA, 0x11});
}

TEST_CASE("build_subaru_bulk_transfer with empty data is just the header",
          "[uds][framing][b6]") {
    auto const r = uds::build_subaru_bulk_transfer(0x123456, {});
    REQUIRE(r == std::vector<std::uint8_t>{0xB6, 0x12, 0x34, 0x56});
}

TEST_CASE("parse_subaru_bulk_transfer_response accepts F6 positive",
          "[uds][framing][b6]") {
    std::vector<std::uint8_t> const resp{0xF6};
    REQUIRE(uds::parse_subaru_bulk_transfer_response(resp).has_value());

    // Some ECUs include a trailing status byte (observed `F6 00` in
    // cobb-reinstall-3.log). Accept and ignore.
    std::vector<std::uint8_t> const resp_with_status{0xF6, 0x00};
    REQUIRE(uds::parse_subaru_bulk_transfer_response(resp_with_status).has_value());
}

TEST_CASE("parse_subaru_bulk_transfer_response surfaces an NRC",
          "[uds][framing][b6][error]") {
    std::vector<std::uint8_t> const nrc_resp{0x7F, 0xB6, 0x31};
    auto const r = uds::parse_subaru_bulk_transfer_response(nrc_resp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_subaru_bulk_transfer_response flags an unexpected SID",
          "[uds][framing][b6][error]") {
    std::vector<std::uint8_t> const wrong_sid{0x76, 0x01};
    auto const r = uds::parse_subaru_bulk_transfer_response(wrong_sid);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient::subaru_bulk_transfer round-trip through MockTransport",
          "[uds][client][b6]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    // Mirror the first B6 transfer captured at cobb-reinstall-3.log:163442.
    std::vector<std::uint8_t> const data(256, 0xFF);
    auto expected_req = std::vector<std::uint8_t>{0xB6, 0x00, 0x60, 0x00};
    expected_req.insert(expected_req.end(), data.begin(), data.end());
    t.expect_send_recv(expected_req, {0xF6});
    uds::UdsClient client{t};
    REQUIRE(client.subaru_bulk_transfer(0x006000, data).has_value());
}

TEST_CASE("UdsClient::subaru_bulk_transfer rejects addresses > 24 bits",
          "[uds][client][b6][error]") {
    // The 0xB6 wire format carries 3 address bytes. Catch any caller-side
    // off-by-one that would silently truncate the high byte into the wrong
    // flash region.
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    uds::UdsClient client{t};
    std::vector<std::uint8_t> const data(4, 0xAA);

    // Boundary: 0xFFFFFF accepted (queue the expected exchange).
    auto expected_max = std::vector<std::uint8_t>{0xB6, 0xFF, 0xFF, 0xFF};
    expected_max.insert(expected_max.end(), data.begin(), data.end());
    t.expect_send_recv(expected_max, {0xF6});
    REQUIRE(client.subaru_bulk_transfer(0xFFFFFF, data).has_value());

    // Just over: 0x01000000 rejected without touching the wire.
    auto const before = t.send_log().size();
    auto const r = client.subaru_bulk_transfer(0x01000000, data);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(t.send_log().size() == before); // no frame emitted
}

// ---- OBD-II Mode 09 ---------------------------------------------------

TEST_CASE("build_obd2_vehicle_info_request emits [09 pid]", "[uds][framing][obd2]") {
    REQUIRE(uds::build_obd2_vehicle_info_request(uds::kObd2PidCalibrationId) ==
            std::vector<std::uint8_t>{0x09, 0x04});
    REQUIRE(uds::build_obd2_vehicle_info_request(uds::kObd2PidCvn) ==
            std::vector<std::uint8_t>{0x09, 0x06});
}

TEST_CASE("parse_obd2_vehicle_info_response decodes one CAL ID (LF79102P)",
          "[uds][framing][obd2]") {
    // Real response from cobb-reinstall-3.log:14565 — engine ECU reports
    // its calibration ID as "LF79102P" (8 printable bytes) padded to 16
    // with NULs. Total UDS payload: 3-byte header + 16-byte message = 19.
    std::vector<std::uint8_t> const resp{0x49, 0x04, 0x01, 0x4C, 0x46, 0x37, 0x39, 0x31,
                                         0x30, 0x32, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCalibrationId, 16);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE((*r)[0].size() == 16);
    // First 8 bytes are the printable CID.
    std::string const cid((*r)[0].begin(), (*r)[0].begin() + 8);
    REQUIRE(cid == "LF79102P");
}

TEST_CASE("parse_obd2_vehicle_info_response decodes a 4-byte CVN", "[uds][framing][obd2]") {
    std::vector<std::uint8_t> const resp{0x49, 0x06, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCvn, 4);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE((*r)[0] == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_obd2_vehicle_info_response handles NODI>1", "[uds][framing][obd2]") {
    // Synthetic: two 4-byte CVNs back-to-back.
    std::vector<std::uint8_t> const resp{0x49, 0x06, 0x02, 0x01, 0x02, 0x03, 0x04,
                                         0x05, 0x06, 0x07, 0x08};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCvn, 4);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    REQUIRE((*r)[0] == std::vector<std::uint8_t>{0x01, 0x02, 0x03, 0x04});
    REQUIRE((*r)[1] == std::vector<std::uint8_t>{0x05, 0x06, 0x07, 0x08});
}

TEST_CASE("parse_obd2_vehicle_info_response rejects PID mismatch",
          "[uds][framing][obd2][error]") {
    std::vector<std::uint8_t> const resp{0x49, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCvn, 4);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_obd2_vehicle_info_response rejects length mismatch",
          "[uds][framing][obd2][error]") {
    // NODI=2 declares 8 bytes of payload, but only 4 follow.
    std::vector<std::uint8_t> const resp{0x49, 0x06, 0x02, 0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCvn, 4);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_obd2_vehicle_info_response surfaces an NRC", "[uds][framing][obd2][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x09, 0x12};
    auto const r = uds::parse_obd2_vehicle_info_response(resp, uds::kObd2PidCvn, 4);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient::obd2_vehicle_info round-trip via MockTransport",
          "[uds][client][obd2]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    // Mirror the install-log exchange: request 09 04, response carrying
    // the 16-byte CAL ID padded with NULs.
    t.expect_send_recv({0x09, 0x04}, {0x49, 0x04, 0x01, 0x4C, 0x46, 0x37, 0x39, 0x31,
                                       0x30, 0x32, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00});
    uds::UdsClient client{t};
    auto const r = client.obd2_vehicle_info(uds::kObd2PidCalibrationId, 16);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    std::string const cid((*r)[0].begin(), (*r)[0].begin() + 8);
    REQUIRE(cid == "LF79102P");
}

// ---- RequestTransferExit ----------------------------------------------

TEST_CASE("RequestTransferExit round-trip", "[uds][exit]") {
    REQUIRE(uds::build_request_transfer_exit() == std::vector<std::uint8_t>{0x37});
    std::vector<std::uint8_t> const resp{0x77};
    REQUIRE(uds::parse_request_transfer_exit_response(resp).has_value());

    // Variant: some ECUs append a CRC; we tolerate trailing bytes.
    std::vector<std::uint8_t> const resp_with_crc{0x77, 0xDE, 0xAD};
    REQUIRE(uds::parse_request_transfer_exit_response(resp_with_crc).has_value());
}

// ---- ReadMemoryByAddress -----------------------------------------------

TEST_CASE("build_read_memory_by_address emits fixed 4-byte address + 1-byte size for sizes <= 255",
          "[uds][framing][rmba]") {
    // Subaru Hitachi (FA20DIT and similar) require the fixed 4-byte
    // address format. ISO 14229 allows variable widths but production
    // ECUs silently drop requests with shorter addresses.
    // addr 0x12 → 00 00 00 12 (4 bytes), size 0xFF (1 byte) → aLFI = 0x14.
    auto const r = uds::build_read_memory_by_address(0x12, 0xFF);
    REQUIRE(r == std::vector<std::uint8_t>{0x23, 0x14, 0x00, 0x00, 0x00, 0x12, 0xFF});
}

TEST_CASE("build_read_memory_by_address widens size byte when size > 255",
          "[uds][framing][rmba]") {
    // size 0x4000 needs 2 size bytes → aLFI = 0x24. Address stays 4 bytes.
    auto const r = uds::build_read_memory_by_address(0x12, 0x4000);
    REQUIRE(r == std::vector<std::uint8_t>{0x23, 0x24, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00});
}

TEST_CASE("build_read_memory_by_address keeps 4-byte address for large addresses",
          "[uds][framing][rmba]") {
    auto const r = uds::build_read_memory_by_address(0x12345678, 0xFF);
    REQUIRE(r == std::vector<std::uint8_t>{0x23, 0x14, 0x12, 0x34, 0x56, 0x78, 0xFF});
}

TEST_CASE("parse_read_memory_by_address_response returns the data", "[uds][framing][rmba]") {
    std::vector<std::uint8_t> const resp{0x63, 0xAA, 0xBB, 0xCC, 0xDD};
    auto const r = uds::parse_read_memory_by_address_response(resp);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD});
}

TEST_CASE("parse_read_memory_by_address_response surfaces NRC", "[uds][framing][rmba][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x23, uds::kNrcSecurityAccessDenied};
    auto const r = uds::parse_read_memory_by_address_response(resp);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient::read_memory_by_address round-trip", "[uds][client][rmba]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Fixed 4-byte address + 2-byte size (size > 255) -> aLFI = 0x24.
    t.expect_send_recv({0x23, 0x24, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00},
                       {0x63, 0xDE, 0xAD, 0xBE, 0xEF});

    uds::UdsClient client{t};
    auto const r = client.read_memory_by_address(0x12, 0x4000);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    REQUIRE(t.exhausted());
}

// ---- WriteMemoryByAddress ----------------------------------------------

TEST_CASE("build_write_memory_by_address packs [3D aLFI addr... size... data...]",
          "[uds][framing][wmba]") {
    std::vector<std::uint8_t> const data{0xAA, 0xBB, 0xCC};
    auto const r = uds::build_write_memory_by_address(0xABCD, data);
    REQUIRE(r == std::vector<std::uint8_t>{0x3D, 0x12, 0xAB, 0xCD, 0x03, 0xAA, 0xBB, 0xCC});
}

TEST_CASE("parse_write_memory_by_address_response accepts a matching echo",
          "[uds][framing][wmba]") {
    std::vector<std::uint8_t> const resp{0x7D, 0x12, 0xAB, 0xCD, 0x03};
    REQUIRE(uds::parse_write_memory_by_address_response(resp, 0xABCD, 0x03).has_value());
}

TEST_CASE("parse_write_memory_by_address_response flags an address mismatch",
          "[uds][framing][wmba][error]") {
    std::vector<std::uint8_t> const resp{0x7D, 0x12, 0xAB, 0xCE, 0x03};
    auto const r = uds::parse_write_memory_by_address_response(resp, 0xABCD, 0x03);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    REQUIRE(r.error().message().find("address") != std::string::npos);
}

TEST_CASE("parse_write_memory_by_address_response flags a size mismatch",
          "[uds][framing][wmba][error]") {
    std::vector<std::uint8_t> const resp{0x7D, 0x12, 0xAB, 0xCD, 0x04};
    auto const r = uds::parse_write_memory_by_address_response(resp, 0xABCD, 0x03);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    REQUIRE(r.error().message().find("size") != std::string::npos);
}

TEST_CASE("UdsClient::write_memory_by_address round-trip", "[uds][client][wmba]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<std::uint8_t> const data{0xAA, 0xBB, 0xCC};
    t.expect_send_recv({0x3D, 0x12, 0xAB, 0xCD, 0x03, 0xAA, 0xBB, 0xCC},
                       {0x7D, 0x12, 0xAB, 0xCD, 0x03});

    uds::UdsClient client{t};
    REQUIRE(client.write_memory_by_address(0xABCD, data).has_value());
    REQUIRE(t.exhausted());
}

// ---- CommunicationControl ----------------------------------------------

TEST_CASE("build_communication_control packs [28 controlType commType]",
          "[uds][framing][commctl]") {
    auto const r = uds::build_communication_control(uds::kCcDisableRxAndTx,
                                                    uds::kCtNormalAndNetworkManagement);
    REQUIRE(r == std::vector<std::uint8_t>{0x28, 0x03, 0x03});
}

TEST_CASE("parse_communication_control_response accepts a matching ack",
          "[uds][framing][commctl]") {
    std::vector<std::uint8_t> const resp{0x68, 0x03};
    REQUIRE(uds::parse_communication_control_response(resp, 0x03).has_value());
}

TEST_CASE("parse_communication_control_response flags controlType mismatch",
          "[uds][framing][commctl][error]") {
    std::vector<std::uint8_t> const resp{0x68, 0x00};
    auto const r = uds::parse_communication_control_response(resp, 0x03);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_communication_control_response surfaces subFunctionNotSupported",
          "[uds][framing][commctl][error]") {
    std::vector<std::uint8_t> const resp{0x7F, 0x28, uds::kNrcSubFunctionNotSupported};
    auto const r = uds::parse_communication_control_response(resp, 0x03);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("UdsClient::communication_control round-trip", "[uds][client][commctl]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    t.expect_send_recv({0x28, 0x03, 0x03}, {0x68, 0x03});

    uds::UdsClient client{t};
    REQUIRE(client.communication_control(uds::kCcDisableRxAndTx, uds::kCtNormalAndNetworkManagement)
                .has_value());
    REQUIRE(t.exhausted());
}

// ---- RoutineControl ----------------------------------------------------

TEST_CASE("build_routine_control packs [31 sub rid_hi rid_lo opts...]", "[uds][framing][routine]") {
    auto const empty = uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE(empty == std::vector<std::uint8_t>{0x31, 0x01, 0xFF, 0x00});

    std::vector<std::uint8_t> const opts{0x12, 0x34, 0x56};
    auto const with_opts = uds::build_routine_control(uds::kRcStart, 0xABCD, opts);
    REQUIRE(with_opts == std::vector<std::uint8_t>{0x31, 0x01, 0xAB, 0xCD, 0x12, 0x34, 0x56});
}

TEST_CASE("parse_routine_control_response returns the status record", "[uds][framing][routine]") {
    // Positive response: [71 01 FF 00] [status 0xAA 0x55]
    std::vector<std::uint8_t> const resp{0x71, 0x01, 0xFF, 0x00, 0xAA, 0x55};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0xAA, 0x55});
}

TEST_CASE("parse_routine_control_response handles an empty status record",
          "[uds][framing][routine]") {
    std::vector<std::uint8_t> const resp{0x71, 0x01, 0xFF, 0x00};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE(r.has_value());
    REQUIRE(r->empty());
}

TEST_CASE("parse_routine_control_response flags sub-function mismatch",
          "[uds][framing][routine][error]") {
    // ECU echoed stopRoutine (0x02) when we asked for startRoutine (0x01).
    std::vector<std::uint8_t> const resp{0x71, 0x02, 0xFF, 0x00};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    REQUIRE(r.error().message().find("sub-function") != std::string::npos);
}

TEST_CASE("parse_routine_control_response flags RID mismatch", "[uds][framing][routine][error]") {
    std::vector<std::uint8_t> const resp{0x71, 0x01, 0xFF, 0x01};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    REQUIRE(r.error().message().find("RID") != std::string::npos);
}

TEST_CASE("parse_routine_control_response surfaces conditionsNotCorrect NRC",
          "[uds][framing][routine][error]") {
    // Negative response: 7F 31 22 -- conditionsNotCorrect (erase before session)
    std::vector<std::uint8_t> const resp{0x7F, 0x31, 0x22};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_routine_control_response flags a truncated response",
          "[uds][framing][routine][error]") {
    std::vector<std::uint8_t> const resp{0x71, 0x01, 0xFF};
    auto const r = uds::parse_routine_control_response(resp, uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("UdsClient::routine_control round-trip through MockTransport", "[uds][client][routine]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Tester asks for eraseMemory with a 4-byte address range; ECU returns
    // routineInfo = 0x00 (success).
    std::vector<std::uint8_t> const opts{0x00, 0x10, 0x00, 0x00};
    t.expect_send_recv({0x31, 0x01, 0xFF, 0x00, 0x00, 0x10, 0x00, 0x00},
                       {0x71, 0x01, 0xFF, 0x00, 0x00});

    uds::UdsClient client{t};
    auto const r = client.routine_control(uds::kRcStart, uds::kRidEraseMemory, opts);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::vector<std::uint8_t>{0x00});
    REQUIRE(t.exhausted());
}

TEST_CASE("UdsClient::routine_control surfaces an NRC as EcuRejected",
          "[uds][client][routine][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    t.expect_send_recv({0x31, 0x01, 0xFF, 0x00}, {0x7F, 0x31, uds::kNrcConditionsNotCorrect});

    uds::UdsClient client{t};
    auto const r = client.routine_control(uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("End-to-end flashing sequence through MockTransport",
          "[uds][client][flash][end_to_end]") {
    // Sketch of the seven-step flash flow:
    //   1. enter programming session
    //   2. (security access -- covered elsewhere)
    //   3. request download
    //   4. transfer data block 1
    //   5. transfer data block 2
    //   6. request transfer exit
    //   7. ecu reset

    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    t.expect_send_recv({0x10, 0x02}, {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4});
    t.expect_send_recv({0x34, 0x00, 0x23, 0x12, 0x34, 0x56, 0x40, 0x00}, {0x74, 0x20, 0x01, 0x00});
    t.expect_send_recv({0x36, 0x01, 0xAA, 0xBB}, {0x76, 0x01});
    t.expect_send_recv({0x36, 0x02, 0xCC, 0xDD}, {0x76, 0x02});
    t.expect_send_recv({0x37}, {0x77});
    t.expect_send_recv({0x11, 0x01}, {0x51, 0x01});

    uds::UdsClient client{t};
    REQUIRE(client.diagnostic_session_control(uds::kDscProgramming).has_value());
    auto const max_block = client.request_download(0x00, 0x123456, 0x4000);
    REQUIRE(max_block.has_value());
    REQUIRE(*max_block == 256);

    std::vector<std::uint8_t> const block1{0xAA, 0xBB};
    std::vector<std::uint8_t> const block2{0xCC, 0xDD};
    REQUIRE(client.transfer_data(1, block1).has_value());
    REQUIRE(client.transfer_data(2, block2).has_value());
    REQUIRE(client.request_transfer_exit().has_value());
    REQUIRE(client.ecu_reset(uds::kEcuResetHard).has_value());

    REQUIRE(t.exhausted());
}

TEST_CASE("Realistic flash flow with erase + check-deps via RoutineControl",
          "[uds][client][flash][end_to_end][routine]") {
    // Compared to the minimal flash sketch above this one inserts the two
    // RoutineControl steps every Subaru flash exercises:
    //   eraseMemory before RequestDownload, and
    //   checkProgrammingDependencies after RequestTransferExit.
    // Security access is still covered in its own test.

    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // 1. enter programming session
    t.expect_send_recv({0x10, 0x02}, {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4});
    // 2. eraseMemory(addr=0x00123456, len=0x4000)
    t.expect_send_recv({0x31, 0x01, 0xFF, 0x00, 0x00, 0x12, 0x34, 0x56, 0x00, 0x00, 0x40, 0x00},
                       {0x71, 0x01, 0xFF, 0x00, 0x00});
    // 3. requestDownload
    t.expect_send_recv({0x34, 0x00, 0x23, 0x12, 0x34, 0x56, 0x40, 0x00}, {0x74, 0x20, 0x01, 0x00});
    // 4-5. transferData x2
    t.expect_send_recv({0x36, 0x01, 0xAA, 0xBB}, {0x76, 0x01});
    t.expect_send_recv({0x36, 0x02, 0xCC, 0xDD}, {0x76, 0x02});
    // 6. requestTransferExit
    t.expect_send_recv({0x37}, {0x77});
    // 7. checkProgrammingDependencies -- ECU returns a CRC status byte
    t.expect_send_recv({0x31, 0x01, 0xFF, 0x01}, {0x71, 0x01, 0xFF, 0x01, 0x00});
    // 8. ecu reset
    t.expect_send_recv({0x11, 0x01}, {0x51, 0x01});

    uds::UdsClient client{t};

    REQUIRE(client.diagnostic_session_control(uds::kDscProgramming).has_value());

    std::vector<std::uint8_t> const erase_opts{0x00, 0x12, 0x34, 0x56, 0x00, 0x00, 0x40, 0x00};
    auto const erase = client.routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opts);
    REQUIRE(erase.has_value());
    REQUIRE(*erase == std::vector<std::uint8_t>{0x00});

    auto const max_block = client.request_download(0x00, 0x123456, 0x4000);
    REQUIRE(max_block.has_value());
    REQUIRE(*max_block == 256);

    REQUIRE(client.transfer_data(1, std::vector<std::uint8_t>{0xAA, 0xBB}).has_value());
    REQUIRE(client.transfer_data(2, std::vector<std::uint8_t>{0xCC, 0xDD}).has_value());
    REQUIRE(client.request_transfer_exit().has_value());

    auto const check = client.routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies);
    REQUIRE(check.has_value());
    REQUIRE(*check == std::vector<std::uint8_t>{0x00});

    REQUIRE(client.ecu_reset(uds::kEcuResetHard).has_value());
    REQUIRE(t.exhausted());
}
