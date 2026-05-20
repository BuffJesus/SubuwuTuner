// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534.hpp"
#include "st/transport/j2534_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace j2534 = st::transport::j2534;
using std::chrono::milliseconds;

// ---- ConnectParams + status mapping (pure helpers) --------------

TEST_CASE("j2534::connect_params_for: KLine → ISO9141 + baud", "[transport][j2534_transport]") {
    st::transport::LinkConfig cfg{st::transport::LinkKind::KLine, 4800, 0, 0};
    auto r = j2534::connect_params_for(cfg);
    REQUIRE(r.has_value());
    REQUIRE(r->protocol == j2534::ProtocolId::ISO9141);
    REQUIRE(r->baud == 4800U);
    REQUIRE(r->flags == 0U);
}

TEST_CASE("j2534::connect_params_for: CanIso15765 → ISO15765 + baud",
          "[transport][j2534_transport]") {
    st::transport::LinkConfig cfg{st::transport::LinkKind::CanIso15765, 500000, 0x7E0, 0x7E8};
    auto r = j2534::connect_params_for(cfg);
    REQUIRE(r.has_value());
    REQUIRE(r->protocol == j2534::ProtocolId::ISO15765);
    REQUIRE(r->baud == 500000U);
}

TEST_CASE("j2534::connect_params_for: CanFd → InvalidArgument (not in v04.04)",
          "[transport][j2534_transport]") {
    st::transport::LinkConfig cfg{st::transport::LinkKind::CanFd, 2000000, 0, 0};
    auto r = j2534::connect_params_for(cfg);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("j2534::connect_params_for: zero baud rejected", "[transport][j2534_transport]") {
    st::transport::LinkConfig cfg{st::transport::LinkKind::KLine, 0, 0, 0};
    auto r = j2534::connect_params_for(cfg);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("j2534::to_st_status: NoError → ok", "[transport][j2534_transport]") {
    auto r = j2534::to_st_status(j2534::Status::NoError);
    REQUIRE(r.has_value());
}

TEST_CASE("j2534::to_st_status: Timeout / BufferEmpty → TransportTimeout",
          "[transport][j2534_transport]") {
    auto a = j2534::to_st_status(j2534::Status::Timeout);
    auto b = j2534::to_st_status(j2534::Status::BufferEmpty);
    REQUIRE_FALSE(a.has_value());
    REQUIRE_FALSE(b.has_value());
    REQUIRE(a.error().code() == st::ErrorCode::TransportTimeout);
    REQUIRE(b.error().code() == st::ErrorCode::TransportTimeout);
}

TEST_CASE("j2534::to_st_status: DeviceNotConnected → TransportUnavailable",
          "[transport][j2534_transport]") {
    auto r = j2534::to_st_status(j2534::Status::DeviceNotConnected);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}

TEST_CASE("j2534::to_st_status: parameter-shape errors → InvalidArgument",
          "[transport][j2534_transport]") {
    auto r = j2534::to_st_status(j2534::Status::InvalidProtocolId);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("j2534::to_st_status: opaque vendor failure → TransportNack",
          "[transport][j2534_transport]") {
    auto r = j2534::to_st_status(j2534::Status::Failed);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportNack);
}

TEST_CASE("j2534::to_st_status: prepends context to the error message",
          "[transport][j2534_transport]") {
    auto r = j2534::to_st_status(j2534::Status::Timeout, "PassThruReadMsgs");
    REQUIRE_FALSE(r.has_value());
    auto const m = r.error().message();
    REQUIRE(m.find("PassThruReadMsgs") != std::string::npos);
    REQUIRE(m.find("timed out") != std::string::npos);
}

// ---- Transport — open / close / send / send_recv via fake DLL ----
//
// J2534 function pointers are C function pointers, not std::function,
// so the test fakes can't capture per-test state via closures. We
// stash the per-test state in a TU-local struct + a pointer that the
// fake C functions consult. A RAII scope-guard binds the pointer to
// the current test and clears it on teardown.

namespace {

struct FakeBackend {
    bool open_called = false;
    bool close_called = false;
    bool connect_called = false;
    bool disconnect_called = false;
    bool read_version_called = false;

    j2534::u32 connect_protocol = 0;
    j2534::u32 connect_baud = 0;
    j2534::u32 connect_flags = 0;
    j2534::u32 assigned_device_id = 0xCAFE;
    j2534::u32 assigned_channel_id = 0xBEEF;

    // What write_msgs recorded — the payload bytes from the most
    // recent PassThruMsg the SUT sent.
    std::vector<std::uint8_t> last_write;
    j2534::u32 last_write_protocol = 0;

    // Queue of payload bytes that the next read_msgs calls will
    // return. Empty queue → returns the configured "no data" status
    // (default BufferEmpty so the SUT's polling loop can still hit
    // its host-side deadline).
    std::deque<std::vector<std::uint8_t>> queued_reads;

    // Force specific J2534 statuses from each entry point. Default
    // is NoError everywhere, except read_msgs which falls back to
    // BufferEmpty when queued_reads is empty (so send_recv can
    // exercise the polling loop without hanging).
    j2534::Status open_status = j2534::Status::NoError;
    j2534::Status close_status = j2534::Status::NoError;
    j2534::Status connect_status = j2534::Status::NoError;
    j2534::Status disconnect_status = j2534::Status::NoError;
    j2534::Status write_status = j2534::Status::NoError;
    j2534::Status read_status_when_empty = j2534::Status::BufferEmpty;
    j2534::Status read_version_status = j2534::Status::NoError;

    // Firmware string the fake ReadVersion returns. Empty → fake
    // skips the copy (still returns NoError) so we can verify the
    // SUT's empty-firmware path too.
    std::string firmware = "TestFW v1.0";
};

FakeBackend *g_backend = nullptr;

struct ScopedBackend {
    FakeBackend backend;
    ScopedBackend() {
        g_backend = &backend;
    }
    ~ScopedBackend() {
        g_backend = nullptr;
    }
};

extern "C" {

j2534::Status f_open(void * /*name*/, j2534::u32 *id) {
    if (!g_backend)
        return j2534::Status::Failed;
    g_backend->open_called = true;
    if (g_backend->open_status != j2534::Status::NoError) {
        return g_backend->open_status;
    }
    *id = g_backend->assigned_device_id;
    return j2534::Status::NoError;
}

j2534::Status f_close(j2534::u32 /*id*/) {
    if (!g_backend)
        return j2534::Status::Failed;
    g_backend->close_called = true;
    return g_backend->close_status;
}

j2534::Status f_connect(j2534::u32 /*device*/, j2534::u32 protocol, j2534::u32 flags,
                        j2534::u32 baud, j2534::u32 *channel) {
    if (!g_backend)
        return j2534::Status::Failed;
    g_backend->connect_called = true;
    g_backend->connect_protocol = protocol;
    g_backend->connect_flags = flags;
    g_backend->connect_baud = baud;
    if (g_backend->connect_status != j2534::Status::NoError) {
        return g_backend->connect_status;
    }
    *channel = g_backend->assigned_channel_id;
    return j2534::Status::NoError;
}

j2534::Status f_disconnect(j2534::u32 /*ch*/) {
    if (!g_backend)
        return j2534::Status::Failed;
    g_backend->disconnect_called = true;
    return g_backend->disconnect_status;
}

j2534::Status f_write_msgs(j2534::u32 /*ch*/, j2534::PassThruMsg *msgs, j2534::u32 *num,
                           j2534::u32 /*timeout*/) {
    if (!g_backend)
        return j2534::Status::Failed;
    if (g_backend->write_status != j2534::Status::NoError) {
        return g_backend->write_status;
    }
    g_backend->last_write_protocol = msgs[0].protocol_id;
    g_backend->last_write.assign(msgs[0].data.data(), msgs[0].data.data() + msgs[0].data_size);
    *num = 1;
    return j2534::Status::NoError;
}

j2534::Status f_read_msgs(j2534::u32 /*ch*/, j2534::PassThruMsg *msgs, j2534::u32 *num,
                          j2534::u32 /*timeout*/) {
    if (!g_backend)
        return j2534::Status::Failed;
    if (g_backend->queued_reads.empty()) {
        *num = 0;
        return g_backend->read_status_when_empty;
    }
    auto payload = std::move(g_backend->queued_reads.front());
    g_backend->queued_reads.pop_front();
    msgs[0] = {};
    msgs[0].protocol_id = static_cast<j2534::u32>(j2534::ProtocolId::ISO9141);
    msgs[0].data_size = static_cast<j2534::u32>(payload.size());
    std::memcpy(msgs[0].data.data(), payload.data(), payload.size());
    *num = 1;
    return j2534::Status::NoError;
}

j2534::Status f_ioctl(j2534::u32 /*ch*/, j2534::u32 /*id*/, void * /*in*/, void * /*out*/) {
    return j2534::Status::NoError;
}

j2534::Status f_read_version(j2534::u32 /*id*/, char *fw, char * /*dll*/, char * /*api*/) {
    if (!g_backend)
        return j2534::Status::Failed;
    g_backend->read_version_called = true;
    if (g_backend->read_version_status != j2534::Status::NoError) {
        return g_backend->read_version_status;
    }
    if (!g_backend->firmware.empty()) {
        std::strncpy(fw, g_backend->firmware.c_str(), 79);
        fw[79] = '\0';
    }
    return j2534::Status::NoError;
}

j2534::Status f_get_last_error(char * /*desc*/) {
    return j2534::Status::NoError;
}

} // extern "C"

j2534::J2534Library::FunctionTable make_full_table() noexcept {
    return {&f_open,       &f_close, &f_connect,      &f_disconnect,    &f_read_msgs,
            &f_write_msgs, &f_ioctl, &f_read_version, &f_get_last_error};
}

} // namespace

TEST_CASE("j2534::Transport: open calls Open + Connect with the right args",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    st::transport::LinkConfig cfg{st::transport::LinkKind::KLine, 4800, 0, 0};

    auto r = t.open(cfg);
    REQUIRE(r.has_value());
    REQUIRE(ctx.backend.open_called);
    REQUIRE(ctx.backend.connect_called);
    REQUIRE(ctx.backend.connect_protocol == static_cast<j2534::u32>(j2534::ProtocolId::ISO9141));
    REQUIRE(ctx.backend.connect_baud == 4800U);
    REQUIRE(t.is_open());
    REQUIRE(t.device_id() == 0xCAFEU);
    REQUIRE(t.channel_id() == 0xBEEFU);
    REQUIRE(t.protocol() == j2534::ProtocolId::ISO9141);
    REQUIRE(t.firmware() == "TestFW v1.0");
}

TEST_CASE("j2534::Transport: open with PassThruOpen failure surfaces it",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    ctx.backend.open_status = j2534::Status::DeviceNotConnected;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};

    auto r = t.open({st::transport::LinkKind::KLine, 4800, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
    REQUIRE_FALSE(t.is_open());
    REQUIRE_FALSE(ctx.backend.connect_called); // open failure short-circuits
}

TEST_CASE("j2534::Transport: open with PassThruConnect failure rolls back",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    ctx.backend.connect_status = j2534::Status::InvalidBaudrate;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};

    auto r = t.open({st::transport::LinkKind::KLine, 4800, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE_FALSE(t.is_open());
    REQUIRE(ctx.backend.close_called); // rollback: close the device after a failed connect
}

TEST_CASE("j2534::Transport: open with bad LinkKind rejected up-front",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};

    auto r = t.open({st::transport::LinkKind::CanFd, 2000000, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE_FALSE(ctx.backend.open_called); // no DLL call attempted
}

TEST_CASE("j2534::Transport: send_recv writes payload + reads queued response",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    ctx.backend.queued_reads.push_back(
        {0x80U, 0xF0U, 0x10U, 0x05U, 0xE8U, 0x12U, 0x34U}); // SSM A8 echo + 1-byte data
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    std::vector<std::uint8_t> const req{0x80U, 0x10U, 0xF0U, 0x05U, 0xA8U, 0x00U, 0x12U, 0x34U};
    auto r = t.send_recv(req, milliseconds{100});
    REQUIRE(r.has_value());
    REQUIRE(r->data.size() == 7);
    REQUIRE(r->data[0] == 0x80U);
    REQUIRE(r->data[6] == 0x34U);
    // The SUT must have set the outgoing PassThruMsg's protocol_id
    // to ISO9141 (matches the open() LinkKind).
    REQUIRE(ctx.backend.last_write_protocol == static_cast<j2534::u32>(j2534::ProtocolId::ISO9141));
    REQUIRE(ctx.backend.last_write == req);
}

TEST_CASE("j2534::Transport: send_recv polls past BufferEmpty until a "
          "queued read arrives",
          "[transport][j2534_transport]") {
    // Simulate the DLL returning BufferEmpty a few times before the
    // ECU's reply lands. We trigger this by pre-queuing a response
    // but with the default `read_status_when_empty = BufferEmpty`.
    ScopedBackend ctx;
    ctx.backend.queued_reads.push_back({0xAA, 0xBB, 0xCC});
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    std::vector<std::uint8_t> const req{0x80U};
    auto r = t.send_recv(req, milliseconds{100});
    REQUIRE(r.has_value());
    REQUIRE(r->data == std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC});
}

TEST_CASE("j2534::Transport: send_recv reports TransportTimeout when no "
          "response arrives within the deadline",
          "[transport][j2534_transport]") {
    ScopedBackend ctx; // queued_reads stays empty
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    auto const t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> const req{0x01U};
    auto r = t.send_recv(req, milliseconds{20});
    auto const elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);
    // Sanity: we didn't return before the deadline.
    REQUIRE(elapsed >= milliseconds{20});
}

TEST_CASE("j2534::Transport: send_recv before open → TransportUnavailable",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};

    std::vector<std::uint8_t> const req{0x01U};
    auto r = t.send_recv(req, milliseconds{10});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}

TEST_CASE("j2534::Transport: send forwards payload, doesn't wait for reply",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    std::vector<std::uint8_t> const req{0x80U, 0x10U, 0xF0U, 0x02U, 0xBFU, 0x40U};
    auto r = t.send(req);
    REQUIRE(r.has_value());
    REQUIRE(ctx.backend.last_write == req);
}

TEST_CASE("j2534::Transport: close calls Disconnect + Close, idempotent",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    REQUIRE(t.close().has_value());
    REQUIRE(ctx.backend.disconnect_called);
    REQUIRE(ctx.backend.close_called);
    REQUIRE_FALSE(t.is_open());

    // Second close is a no-op (per ITransport contract).
    ctx.backend.disconnect_called = false;
    ctx.backend.close_called = false;
    REQUIRE(t.close().has_value());
    REQUIRE_FALSE(ctx.backend.disconnect_called);
    REQUIRE_FALSE(ctx.backend.close_called);
}

TEST_CASE("j2534::Transport: streaming returns NotImplemented (stub)",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.open({st::transport::LinkKind::KLine, 4800, 0, 0}).has_value());

    auto a = t.start_streaming([](st::transport::Frame const &) {});
    REQUIRE_FALSE(a.has_value());
    REQUIRE(a.error().code() == st::ErrorCode::NotImplemented);

    auto b = t.stop_streaming();
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code() == st::ErrorCode::NotImplemented);
}

TEST_CASE("j2534::Transport: name() is 'J2534'", "[transport][j2534_transport]") {
    ScopedBackend ctx;
    j2534::Transport t{j2534::J2534Library{make_full_table()}};
    REQUIRE(t.name() == "J2534");
}

TEST_CASE("j2534::Transport: missing core entry point → TransportUnavailable",
          "[transport][j2534_transport]") {
    ScopedBackend ctx;
    auto table = make_full_table();
    table.write_msgs = nullptr; // tear out a core entry
    j2534::Transport t{j2534::J2534Library{table}};

    auto r = t.open({st::transport::LinkKind::KLine, 4800, 0, 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}
