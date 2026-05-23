// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/obdx_dvi.hpp"
#include "st/transport/obdx_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace obdx = st::transport::obdx;
using std::chrono::milliseconds;

namespace {

// Test double for IDeviceChannel: records every byte written,
// serves bytes from a pre-queued FIFO on each read. A read finds
// the queue empty -> returns an empty vector after the requested
// timeout elapses (mirrors a real USB endpoint that has nothing
// pending; the Transport's framing loop polls back).
class FakeChannel : public obdx::IDeviceChannel {
public:
    [[nodiscard]] st::Status write_bytes(std::span<std::uint8_t const> bytes) override {
        writes_.insert(writes_.end(), bytes.begin(), bytes.end());
        return st::ok();
    }

    [[nodiscard]] st::Result<std::vector<std::uint8_t>>
    read_bytes(std::size_t max_bytes, milliseconds /*timeout*/) override {
        if (read_queue_.empty()) {
            return std::vector<std::uint8_t>{};
        }
        auto &front = read_queue_.front();
        std::size_t const n = std::min(max_bytes, front.size());
        std::vector<std::uint8_t> out(front.begin(),
                                      front.begin() + static_cast<std::ptrdiff_t>(n));
        front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(n));
        if (front.empty()) {
            read_queue_.pop_front();
        }
        return out;
    }

    // Test helpers.
    void queue_read(std::vector<std::uint8_t> bytes) {
        read_queue_.push_back(std::move(bytes));
    }
    void queue_read_ascii(std::string_view s) {
        std::vector<std::uint8_t> b;
        for (char c : s)
            b.push_back(static_cast<std::uint8_t>(c));
        read_queue_.push_back(std::move(b));
    }
    [[nodiscard]] std::vector<std::uint8_t> const &writes() const noexcept {
        return writes_;
    }
    [[nodiscard]] std::string writes_as_string() const {
        return std::string{writes_.begin(), writes_.end()};
    }

private:
    std::vector<std::uint8_t> writes_;
    std::deque<std::vector<std::uint8_t>> read_queue_;
};

// Build a complete DVI response frame for a given request opcode
// + payload (response opcode = request | 0x10, then standard
// CMD/LEN/DATA/CHK). Use for PC→adapter request acks (TxSmall,
// SetProtocol, etc.) per VT v1.06 §3.2.
std::vector<std::uint8_t> dvi_response_frame(obdx::dvi::Opcode req_op,
                                             std::vector<std::uint8_t> payload) {
    std::uint8_t const resp_op = static_cast<std::uint8_t>(req_op) | 0x10U;
    std::vector<std::uint8_t> frame;
    frame.push_back(resp_op);
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    // Recompute checksum the same way the codec does.
    frame.push_back(obdx::dvi::checksum(std::span<std::uint8_t const>{frame.data(), frame.size()}));
    return frame;
}

// Build a complete DVI adapter-initiated push frame — the kind the
// OBDX sends to the PC when a network message arrives. Per VT v1.06
// §3.3 these use the BARE opcode (no |0x10), since they're not
// responses to a PC request. Use for RxSmall (0x08) / RxLarge (0x09).
std::vector<std::uint8_t> dvi_unsolicited_frame(obdx::dvi::Opcode op,
                                                std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>(op));
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(obdx::dvi::checksum(std::span<std::uint8_t const>{frame.data(), frame.size()}));
    return frame;
}

// Owned ptr helper (Transport ctor wants unique_ptr, tests want
// access to the underlying fake to inspect writes).
struct ChannelPair {
    FakeChannel *raw;
    std::unique_ptr<obdx::IDeviceChannel> owner;
};

[[nodiscard]] ChannelPair make_channel() {
    auto fake = std::make_unique<FakeChannel>();
    auto *raw = fake.get();
    return {raw, std::move(fake)};
}

} // namespace

// ---- open() handshake -------------------------------------------

TEST_CASE("obdx::Transport::open drives ELM probe -> DX DP 1 -> SetProtocol -> EnableNetwork",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>"); // probe response
    cp.raw->queue_read_ascii("OK\r>");               // DX DP 1 response
    cp.raw->queue_read(
        dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x01, 0x02})); // SetProtocol ack (sub=01, HS CAN=02)
    cp.raw->queue_read(
        dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x02, 0x01})); // EnableNetwork ack (sub=02, state=ON)

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE(r.has_value());
    REQUIRE(t.is_open());

    auto const writes = cp.raw->writes_as_string();
    REQUIRE(writes.find("AT @1\r") == 0);
    REQUIRE(writes.find("DX DP 1\r") != std::string::npos);

    // VT v1.06 §3.10.1 + §3.10.2 specify the exact 5-byte frames on
    // the wire after DVI switch:
    //   SetProtocol(HS CAN) = 31 02 01 02 <chk>
    //   EnableNetwork(ON)   = 31 02 02 01 <chk>
    // Verify both showed up, in order.
    auto const &raw_writes = cp.raw->writes();
    auto find_frame = [&](std::uint8_t sub, std::uint8_t arg) -> std::size_t {
        for (std::size_t i = 0; i + 3 < raw_writes.size(); ++i) {
            if (raw_writes[i] == 0x31U && raw_writes[i + 1] == 0x02U &&
                raw_writes[i + 2] == sub && raw_writes[i + 3] == arg) {
                return i;
            }
        }
        return std::string::npos;
    };
    auto const set_pos = find_frame(0x01U, 0x02U);    // SetProtocol(HS CAN)
    auto const enable_pos = find_frame(0x02U, 0x01U); // EnableNetwork(ON)
    REQUIRE(set_pos != std::string::npos);
    REQUIRE(enable_pos != std::string::npos);
    REQUIRE(enable_pos > set_pos);

    REQUIRE(t.firmware() == "OBDX Pro VX v1.0\r");
}

TEST_CASE("obdx::Transport::open rejects device whose probe doesn't say OBDX",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    cp.raw->queue_read_ascii("ELM327 v1.4\r>"); // wrong device on this port

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
    REQUIRE_FALSE(t.is_open());
}

TEST_CASE("obdx::Transport::open rejects K-Line LinkKind with a helpful "
          "message pointing at Tactrix",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");
    // (No SetProtocol response queued; we expect open() to bail
    // BEFORE the DVI exchange when K-Line is requested.)

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::KLine, 4800, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    auto const m = r.error().message();
    REQUIRE(m.find("K-Line") != std::string::npos);
    REQUIRE(m.find("Tactrix") != std::string::npos);
}

TEST_CASE("obdx::Transport::open rejects CAN-FD with a helpful message",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanFd, 2000000, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    auto const m = r.error().message();
    REQUIRE(m.find("CAN-FD") != std::string::npos);
}

TEST_CASE("obdx::Transport::open fails when no ELM prompt arrives "
          "(timeout)",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    // No queued reads -> reads return empty vectors until the host-
    // side deadline expires.
    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
}

TEST_CASE("obdx::Transport::open rejects EnableNetwork ACK with state != ON",
          "[transport][obdx_transport]") {
    // Adapter acknowledges EnableNetwork but reports state OFF (0x00) —
    // the bus didn't actually come up. open() must fail at this step
    // with a descriptive TransportNack instead of silently succeeding
    // and leaving every subsequent send_recv to time out without
    // explanation. open() must ALSO emit a best-effort SoftReboot to
    // return the adapter to ELM mode so the user's next open() attempt
    // doesn't fail confusingly on the ELM probe.
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x01, 0x02}));
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x02, 0x00}));
    // SoftReboot ACK — best-effort cleanup the validation-failure path emits.
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SoftReboot, {}));

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportNack);
    REQUIRE(std::string{r.error().message()}.find("state byte 0x00") != std::string::npos);

    // Verify SoftReboot opcode (0x25) appears in the raw write log.
    auto const &raw_writes = cp.raw->writes();
    bool soft_reboot_sent = false;
    for (std::size_t i = 0; i + 1 < raw_writes.size(); ++i) {
        if (raw_writes[i] == 0x25U && raw_writes[i + 1] == 0x00U) {
            soft_reboot_sent = true;
            break;
        }
    }
    REQUIRE(soft_reboot_sent);
}

TEST_CASE("obdx::Transport::open rejects EnableNetwork ACK with wrong sub-op echo",
          "[transport][obdx_transport]") {
    // First byte of EnableNetwork response must echo sub-op 0x02. A
    // garbage prefix here means the firmware misinterpreted the request
    // — surface immediately rather than push ahead with a half-open
    // adapter. SoftReboot is emitted on the cleanup path; not asserted
    // here (it's verified in the state != ON test).
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x01, 0x02}));
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0xEE, 0x01}));
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SoftReboot, {}));

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportNack);
    REQUIRE(std::string{r.error().message()}.find("sub-op") != std::string::npos);
}

TEST_CASE("obdx::Transport::open accepts EnableNetwork ACK with state=LISTEN-ONLY",
          "[transport][obdx_transport]") {
    // 0x02 = LISTEN-ONLY is a valid bus state, even though send paths
    // won't actually reach the bus. Accept the open so the caller can
    // still use the adapter for sniffing; verbose-mode emits a warning
    // (not checked here — would require capturing stderr).
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x01, 0x02}));
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x02, 0x02}));

    obdx::Transport t{std::move(cp.owner)};
    auto const r = t.open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE(r.has_value());
}

// ---- send_recv --------------------------------------------------

namespace {

// Open a Transport ready to send_recv, with the ELM/DVI handshake
// already queued + run. Returns the Transport (heap-allocated since
// it's not movable) + a borrow of the raw channel so the test can
// queue further reads + inspect writes.
struct ReadyTransport {
    std::unique_ptr<obdx::Transport> transport;
    FakeChannel *raw;
};

[[nodiscard]] ReadyTransport build_open_transport() {
    auto cp = make_channel();
    cp.raw->queue_read_ascii("OBDX Pro VX v1.0\r>");
    cp.raw->queue_read_ascii("OK\r>");
    // SetProtocol(HS CAN) ack — VT v1.06 §3.10.1. Response echoes the
    // sub-op (0x01) and the selected protocol enum (0x02 = HS CAN).
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x01, 0x02}));
    // EnableNetwork(ON) ack — VT v1.06 §3.10.2; sent right after
    // SetProtocol because the default network state is OFF. Response
    // echoes the sub-op (0x02) and the final state byte (0x01 = ON).
    cp.raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SetProtocol, {0x02, 0x01}));
    auto t = std::make_unique<obdx::Transport>(std::move(cp.owner));
    auto const open_r = t->open({st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8});
    REQUIRE(open_r.has_value());
    return {std::move(t), cp.raw};
}

} // namespace

TEST_CASE("obdx::Transport::send_recv writes TX request, reads unsolicited RX push, "
          "returns the RX payload",
          "[transport][obdx_transport]") {
    auto pair = build_open_transport();
    auto &t = *pair.transport;
    auto *raw = pair.raw;

    // Queue what the adapter would actually send back per VT v1.06:
    //   1. TxSmall ack: response opcode 0x20 (= 0x10 | 0x10).
    //   2. RxSmall push: bare opcode 0x08, NOT 0x18 — adapter-initiated
    //      pushes (incoming network frames) don't get the |0x10 bit.
    raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::TxSmall, {0x00}));
    raw->queue_read(dvi_unsolicited_frame(obdx::dvi::Opcode::RxSmall,
                                          {0x80, 0xF0, 0x10, 0x05, 0xE8, 0x12, 0x34}));

    std::vector<std::uint8_t> const req{0x80U, 0x10U, 0xF0U, 0x05U, 0xA8U, 0x00U, 0x12U, 0x34U};
    auto r = t.send_recv(req, milliseconds{200});
    REQUIRE(r.has_value());
    REQUIRE(r->data.size() == 7);
    REQUIRE(r->data[0] == 0x80U);
    REQUIRE(r->data[6] == 0x34U);

    // PC writes exactly ONE DVI request frame after the handshake:
    // TxSmall (0x10). RxSmall (0x08) is an adapter-to-PC push only —
    // we do NOT send it ourselves (doing so triggers Error_Invalid-
    // Command from the firmware, diagnosed against real hardware
    // 2026-05-22). This property is implicit in the test passing:
    // if send_recv tried to write opcode 0x08, it would block on a
    // queued response we deliberately did not provide and the test
    // would time out. The TxSmall opcode appearing in the write
    // stream is the positive marker.
    auto const &writes = raw->writes();
    std::size_t tx_pos = std::string::npos;
    for (std::size_t i = 0; i < writes.size(); ++i) {
        if (writes[i] == static_cast<std::uint8_t>(obdx::dvi::Opcode::TxSmall)) {
            tx_pos = i;
            break;
        }
    }
    REQUIRE(tx_pos != std::string::npos);
}

TEST_CASE("obdx::Transport::send_recv rejects oversized payload", "[transport][obdx_transport]") {
    auto pair = build_open_transport();
    std::vector<std::uint8_t> too_big(300, 0xABU);
    auto r = pair.transport->send_recv(too_big, milliseconds{100});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("obdx::Transport::send_recv before open -> TransportUnavailable",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    obdx::Transport t{std::move(cp.owner)};
    std::vector<std::uint8_t> const req{0x01U};
    auto r = t.send_recv(req, milliseconds{10});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}

TEST_CASE("obdx::Transport::send_recv surfaces a DVI error frame as "
          "TransportNack with the error code in the message",
          "[transport][obdx_transport]") {
    auto pair = build_open_transport();
    auto *raw = pair.raw;

    // TX-ack ok, RX returns an error frame: 0x7F LEN CMD ERR CHK.
    // LEN=2; CMD = RxSmall (0x08); ERR = 0x09 (some vendor-defined
    // code). Checksum over [0x7F, 0x02, 0x08, 0x09].
    raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::TxSmall, {0x00}));
    std::vector<std::uint8_t> err_frame{0x7FU, 0x02U, 0x08U, 0x09U};
    err_frame.push_back(
        obdx::dvi::checksum(std::span<std::uint8_t const>{err_frame.data(), err_frame.size()}));
    raw->queue_read(err_frame);

    std::vector<std::uint8_t> const req{0x01U};
    auto r = pair.transport->send_recv(req, milliseconds{100});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportNack);
    REQUIRE(r.error().message().find("0x09") != std::string::npos);
}

// ---- send -------------------------------------------------------

TEST_CASE("obdx::Transport::send writes a TxSmall request, no RX phase",
          "[transport][obdx_transport]") {
    auto pair = build_open_transport();
    auto *raw = pair.raw;
    raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::TxSmall, {0x00}));

    auto const writes_before = raw->writes().size();
    std::vector<std::uint8_t> const req{0xAAU, 0xBBU, 0xCCU};
    auto r = pair.transport->send(req);
    REQUIRE(r.has_value());

    // Exactly one DVI frame: SOF? No -- DVI doesn't have a SOF byte.
    // The added writes are CMD(0x10) + LEN(0x03) + payload(3) + CHK(1)
    // = 6 bytes total.
    auto const added = raw->writes().size() - writes_before;
    REQUIRE(added == 6);
}

TEST_CASE("obdx::Transport::send before open -> TransportUnavailable",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    obdx::Transport t{std::move(cp.owner)};
    std::vector<std::uint8_t> const req{0x01U};
    auto r = t.send(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}

// ---- close ------------------------------------------------------

TEST_CASE("obdx::Transport::close sends a SoftReboot, becomes idempotent",
          "[transport][obdx_transport]") {
    auto pair = build_open_transport();
    auto &t = *pair.transport;
    auto *raw = pair.raw;
    // Queue a response for the SoftReboot DVI exchange. close()
    // ignores the result either way, but giving it a real response
    // keeps the fake channel clean.
    raw->queue_read(dvi_response_frame(obdx::dvi::Opcode::SoftReboot, {}));

    auto const writes_before = raw->writes().size();
    REQUIRE(t.close().has_value());
    REQUIRE_FALSE(t.is_open());

    // A 0x25 (SoftReboot) opcode byte must have appeared after the
    // pre-close write count.
    auto const &writes = raw->writes();
    bool saw_reboot = false;
    for (std::size_t i = writes_before; i < writes.size(); ++i) {
        if (writes[i] == static_cast<std::uint8_t>(obdx::dvi::Opcode::SoftReboot)) {
            saw_reboot = true;
            break;
        }
    }
    REQUIRE(saw_reboot);

    // Second close is a no-op (per ITransport contract).
    auto const writes_after_first_close = raw->writes().size();
    REQUIRE(t.close().has_value());
    REQUIRE(raw->writes().size() == writes_after_first_close);
}

// ---- streaming + name + firmware --------------------------------

TEST_CASE("obdx::Transport::start_streaming returns NotImplemented (stub)",
          "[transport][obdx_transport]") {
    auto cp = make_channel();
    obdx::Transport t{std::move(cp.owner)};
    auto r = t.start_streaming([](st::transport::Frame const &) {});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
    REQUIRE_FALSE(t.stop_streaming().has_value());
}

TEST_CASE("obdx::Transport::name() reports 'OBDX'", "[transport][obdx_transport]") {
    auto cp = make_channel();
    obdx::Transport t{std::move(cp.owner)};
    REQUIRE(t.name() == "OBDX");
}
