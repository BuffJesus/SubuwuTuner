// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// UDS PDU atomicity tests — Tier 2a per docs/08-testing-strategy.md.
//
// The cancellation-invariant spec rests on every UDS client call being
// atomic at the PDU level: one public `UdsClient::xxx()` call → exactly
// one request PDU on the wire, regardless of whether the call succeeds,
// fails with a negative response, or fails with a transport-level error.
//
// The orchestrator's eventual "cancel is deferred to the next PDU" logic
// only works if each client call is itself indivisible. These tests pin
// that property so a future refactor that, say, sneaks a retry loop into
// `transfer_data()` can't silently break the cancel contract.
//
// What's tested here:
//   - One round-trip = one send_log entry, across the major services
//     (DSC, RDBI, RoutineControl, RequestDownload, TransferData,
//     RequestTransferExit, CommunicationControl, TesterPresent).
//   - Negative-response failures emit the request PDU exactly once, then
//     return EcuRejected without re-sending.
//   - Transport-level injected errors do not trigger a retry — the client
//     surfaces the failure on the first PDU.
//
// What's NOT tested here (because it can't be without enforcement code):
//   - Mid-PDU cancellation deferral at the ORCHESTRATOR level. That lives
//     in `Flasher::execute`, which doesn't accept a cancel token today.
//     See `tests/unit/flash/test_cancellation_invariants.cpp` for the
//     pending-enforcement notes on invariants #1-3.

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

// MockTransport holds a std::mutex and so is neither copyable nor movable.
// Inline the open() call in every test rather than wrap a helper that would
// need to return by reference.

// ---------------------------------------------------------------------------
// One round-trip = one send_log entry, across the public client surface.
// ---------------------------------------------------------------------------

TEST_CASE("UdsClient::diagnostic_session_control emits exactly one PDU",
          "[uds][atomicity][dsc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x10, uds::kDscProgramming}, {0x50, uds::kDscProgramming});
    uds::UdsClient c{t};
    auto const r = c.diagnostic_session_control(uds::kDscProgramming);
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::read_data_by_identifier emits exactly one PDU",
          "[uds][atomicity][rdbi]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x22, 0xF1, 0x90}, {0x62, 0xF1, 0x90, 'V', 'I', 'N'});
    uds::UdsClient c{t};
    auto const r = c.read_data_by_identifier(0xF190);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::routine_control emits exactly one PDU",
          "[uds][atomicity][routine]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv(uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory),
                       {0x71, 0x01, 0xFF, 0x00});
    uds::UdsClient c{t};
    auto const r = c.routine_control(uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::request_download emits exactly one PDU",
          "[uds][atomicity][download]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv(uds::build_request_download(0x00, 0x1234, 0x10),
                       {0x74, 0x20, 0x00, 0x10});
    uds::UdsClient c{t};
    auto const r = c.request_download(0x00, 0x1234, 0x10);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0x0010U);
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::transfer_data emits exactly one PDU",
          "[uds][atomicity][transfer]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    std::vector<std::uint8_t> const block{0xDE, 0xAD, 0xBE, 0xEF};
    t.expect_send_recv(uds::build_transfer_data(1, block), {0x76, 0x01});
    uds::UdsClient c{t};
    auto const r = c.transfer_data(1, block);
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::request_transfer_exit emits exactly one PDU",
          "[uds][atomicity][exit]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv(uds::build_request_transfer_exit(), {0x77});
    uds::UdsClient c{t};
    auto const r = c.request_transfer_exit();
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::communication_control emits exactly one PDU",
          "[uds][atomicity][commcontrol]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv({0x28, uds::kCcDisableRxAndTx, uds::kCtNormalAndNetworkManagement},
                       {0x68, uds::kCcDisableRxAndTx});
    uds::UdsClient c{t};
    auto const r =
        c.communication_control(uds::kCcDisableRxAndTx, uds::kCtNormalAndNetworkManagement);
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("UdsClient::tester_present emits exactly one PDU",
          "[uds][atomicity][testerpresent]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv(uds::build_tester_present_request(/*suppress=*/false), {0x7E, 0x00});
    uds::UdsClient c{t};
    auto const r = c.tester_present();
    REQUIRE(r.has_value());
    REQUIRE(t.send_log().size() == 1);
}

// ---------------------------------------------------------------------------
// Negative response = atomic failure. One PDU sent, no retry, error surfaced.
// ---------------------------------------------------------------------------

TEST_CASE("transfer_data negative response does not retry the PDU",
          "[uds][atomicity][transfer][nrc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    std::vector<std::uint8_t> const block{0x01, 0x02, 0x03, 0x04};
    // ECU rejects with conditionsNotCorrect (0x22) under SID 0x36 (TransferData).
    t.expect_send_recv(uds::build_transfer_data(1, block), {0x7F, 0x36, 0x22});
    uds::UdsClient c{t};
    auto const r = c.transfer_data(1, block);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    // Exactly one request on the wire — no silent retry that could double
    // up the block-sequence-counter and corrupt the download.
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("routine_control negative response does not retry the PDU",
          "[uds][atomicity][routine][nrc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.expect_send_recv(uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory),
                       {0x7F, 0x31, 0x22});
    uds::UdsClient c{t};
    auto const r = c.routine_control(uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("request_download negative response does not retry the PDU",
          "[uds][atomicity][download][nrc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    // securityAccessDenied (0x33) under SID 0x34 (RequestDownload).
    t.expect_send_recv(uds::build_request_download(0x00, 0x1234, 0x10), {0x7F, 0x34, 0x33});
    uds::UdsClient c{t};
    auto const r = c.request_download(0x00, 0x1234, 0x10);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
    REQUIRE(t.send_log().size() == 1);
}

// ---------------------------------------------------------------------------
// Transport-level failure is atomic too — no retry, no leakage.
// ---------------------------------------------------------------------------

TEST_CASE("transfer_data transport error surfaces without retry",
          "[uds][atomicity][transfer][transport]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    // No expect_send_recv queued; instead inject a transport-level error.
    // The first send_recv pops the injected error before scanning
    // expectations, so the request bytes are still recorded in send_log.
    t.inject_error(st::ErrorCode::TransportTimeout, "synthetic timeout");
    std::vector<std::uint8_t> const block{0x01, 0x02, 0x03, 0x04};
    uds::UdsClient c{t};
    auto const r = c.transfer_data(1, block);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
    // No second attempt. send_log captures whatever the transport saw before
    // failing — the point is we don't see TWO entries.
    REQUIRE(t.send_log().size() <= 1);
}

TEST_CASE("routine_control transport error surfaces without retry",
          "[uds][atomicity][routine][transport]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.inject_error(st::ErrorCode::TransportTimeout, "synthetic timeout");
    uds::UdsClient c{t};
    auto const r = c.routine_control(uds::kRcStart, uds::kRidEraseMemory);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
    REQUIRE(t.send_log().size() <= 1);
}
