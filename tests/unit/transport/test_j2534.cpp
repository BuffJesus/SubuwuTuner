// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace j2534 = st::transport::j2534;

// ---- ABI sanity ---------------------------------------------------
//
// These are not behavior tests -- they lock in the binary layout the
// vendor DLL reads/writes through. A regression here corrupts the
// CAN/K-Line bus the moment a real DLL is called.

TEST_CASE("j2534::PassThruMsg has the SAE-defined 4152-byte layout", "[transport][j2534]") {
    REQUIRE(sizeof(j2534::PassThruMsg) == 4152);
    REQUIRE(std::is_standard_layout_v<j2534::PassThruMsg>);
}

TEST_CASE("j2534::PassThruMsg field offsets match the v04.04 layout", "[transport][j2534]") {
    // Every header field is 4 bytes, naturally aligned, no padding.
    REQUIRE(offsetof(j2534::PassThruMsg, protocol_id) == 0);
    REQUIRE(offsetof(j2534::PassThruMsg, rx_status) == 4);
    REQUIRE(offsetof(j2534::PassThruMsg, tx_flags) == 8);
    REQUIRE(offsetof(j2534::PassThruMsg, timestamp) == 12);
    REQUIRE(offsetof(j2534::PassThruMsg, data_size) == 16);
    REQUIRE(offsetof(j2534::PassThruMsg, extra_data_index) == 20);
    REQUIRE(offsetof(j2534::PassThruMsg, data) == 24);
}

TEST_CASE("j2534::SConfig is exactly 2 uint32_t fields", "[transport][j2534]") {
    REQUIRE(sizeof(j2534::SConfig) == 8);
    REQUIRE(offsetof(j2534::SConfig, parameter) == 0);
    REQUIRE(offsetof(j2534::SConfig, value) == 4);
}

TEST_CASE("j2534::u32 is exactly 4 bytes (locks down vendor-DLL ABI)", "[transport][j2534]") {
    // Native `unsigned long` is 64-bit on Linux/macOS x64 but the
    // J2534 DLL was compiled against the Windows 32-bit `unsigned
    // long`. Using uint32_t explicitly keeps the wire layout right
    // on every host.
    REQUIRE(sizeof(j2534::u32) == 4);
    REQUIRE(std::is_unsigned_v<j2534::u32>);
}

// ---- Documented constants matter --------------------------------

TEST_CASE("j2534::ProtocolId values match the SAE v04.04 spec", "[transport][j2534]") {
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::J1850VPW) == 0x01U);
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::J1850PWM) == 0x02U);
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::ISO9141) == 0x03U);
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::ISO14230) == 0x04U);
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::Can) == 0x05U);
    REQUIRE(static_cast<j2534::u32>(j2534::ProtocolId::ISO15765) == 0x06U);
}

TEST_CASE("j2534::Status::NoError is 0 (every PassThru* call returns "
          "non-zero on failure)",
          "[transport][j2534]") {
    REQUIRE(static_cast<j2534::u32>(j2534::Status::NoError) == 0U);
}

// ---- status_message ---------------------------------------------

TEST_CASE("j2534::status_message: every documented code maps to a "
          "non-empty string",
          "[transport][j2534]") {
    constexpr j2534::Status all[] = {
        j2534::Status::NoError,
        j2534::Status::NotSupported,
        j2534::Status::InvalidChannelId,
        j2534::Status::InvalidProtocolId,
        j2534::Status::NullParameter,
        j2534::Status::InvalidIoctlValue,
        j2534::Status::InvalidFlags,
        j2534::Status::Failed,
        j2534::Status::DeviceNotConnected,
        j2534::Status::Timeout,
        j2534::Status::InvalidMsg,
        j2534::Status::InvalidTimeInterval,
        j2534::Status::ExceededLimit,
        j2534::Status::InvalidMsgId,
        j2534::Status::DeviceInUse,
        j2534::Status::InvalidIoctlId,
        j2534::Status::BufferEmpty,
        j2534::Status::BufferFull,
        j2534::Status::BufferOverflow,
        j2534::Status::PinInvalid,
        j2534::Status::ChannelInUse,
        j2534::Status::MsgProtocolId,
        j2534::Status::InvalidFilterId,
        j2534::Status::NoFlowControl,
        j2534::Status::NotUnique,
        j2534::Status::InvalidBaudrate,
        j2534::Status::InvalidDeviceId,
    };
    for (auto s : all) {
        auto const msg = j2534::status_message(s);
        REQUIRE_FALSE(msg.empty());
        REQUIRE(msg != "unknown J2534 status");
    }
}

TEST_CASE("j2534::status_message: unknown code -> 'unknown J2534 status'", "[transport][j2534]") {
    j2534::u32 const bogus = 0xDEAD;
    REQUIRE(j2534::status_message(bogus) == "unknown J2534 status");
}

TEST_CASE("j2534::status_message: Failed code mentions GetLastError", "[transport][j2534]") {
    // Vendor convention is to return STATUS::Failed and stash the
    // real diagnostic behind PassThruGetLastError. The user-facing
    // message must hint at that.
    auto const msg = j2534::status_message(j2534::Status::Failed);
    REQUIRE(msg.find("GetLastError") != std::string_view::npos);
}

// ---- J2534Library -----------------------------------------------

namespace {

// Test-only stand-ins for the J2534 vendor functions. Each returns
// a distinct status so we can verify the FunctionTable is wired
// through correctly without needing a real DLL.
extern "C" {
j2534::Status fake_open(void *, j2534::u32 *) {
    return j2534::Status::NoError;
}
j2534::Status fake_close(j2534::u32) {
    return j2534::Status::NoError;
}
j2534::Status fake_connect(j2534::u32, j2534::u32, j2534::u32, j2534::u32, j2534::u32 *) {
    return j2534::Status::InvalidProtocolId;
}
j2534::Status fake_disconnect(j2534::u32) {
    return j2534::Status::NoError;
}
j2534::Status fake_read_msgs(j2534::u32, j2534::PassThruMsg *, j2534::u32 *, j2534::u32) {
    return j2534::Status::BufferEmpty;
}
j2534::Status fake_write_msgs(j2534::u32, j2534::PassThruMsg *, j2534::u32 *, j2534::u32) {
    return j2534::Status::NoError;
}
j2534::Status fake_ioctl(j2534::u32, j2534::u32, void *, void *) {
    return j2534::Status::NoError;
}
j2534::Status fake_read_version(j2534::u32, char *, char *, char *) {
    return j2534::Status::NoError;
}
j2534::Status fake_get_last_error(char *) {
    return j2534::Status::NoError;
}
} // extern "C"

j2534::J2534Library::FunctionTable make_full_table() noexcept {
    return {
        &fake_open,       &fake_close, &fake_connect,      &fake_disconnect,     &fake_read_msgs,
        &fake_write_msgs, &fake_ioctl, &fake_read_version, &fake_get_last_error,
    };
}

} // namespace

TEST_CASE("j2534::J2534Library: holds an injected function table verbatim", "[transport][j2534]") {
    auto table = make_full_table();
    j2534::J2534Library lib{table};
    auto const &got = lib.functions();
    REQUIRE(got.open == &fake_open);
    REQUIRE(got.close == &fake_close);
    REQUIRE(got.connect == &fake_connect);
    REQUIRE(got.disconnect == &fake_disconnect);
    REQUIRE(got.read_msgs == &fake_read_msgs);
    REQUIRE(got.write_msgs == &fake_write_msgs);
    REQUIRE(got.ioctl == &fake_ioctl);
}

TEST_CASE("j2534::J2534Library: forwarded function pointers still callable", "[transport][j2534]") {
    // Sanity: calling through the table should hit our fake_connect
    // and return InvalidProtocolId. Catches accidental nullification
    // during copy/move.
    j2534::J2534Library lib{make_full_table()};
    j2534::u32 channel = 0;
    auto const s = lib.functions().connect(0, 0, 0, 0, &channel);
    REQUIRE(s == j2534::Status::InvalidProtocolId);
}

TEST_CASE("j2534::FunctionTable::core_complete: true on full table", "[transport][j2534]") {
    REQUIRE(make_full_table().core_complete());
}

TEST_CASE("j2534::FunctionTable::core_complete: false if any core entry "
          "is null (diagnostics fields don't count)",
          "[transport][j2534]") {
    auto t = make_full_table();
    t.connect = nullptr;
    REQUIRE_FALSE(t.core_complete());

    // ReadVersion + GetLastError are diagnostic-only -- a vendor DLL
    // missing them should NOT fail the readiness check.
    auto t2 = make_full_table();
    t2.read_version = nullptr;
    t2.get_last_error = nullptr;
    REQUIRE(t2.core_complete());
}

TEST_CASE("j2534::J2534Library::load: returns NotImplemented (stub)", "[transport][j2534]") {
    auto r = j2534::J2534Library::load("op20pt32.dll");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

TEST_CASE("j2534::J2534Library: move-construction transfers the table", "[transport][j2534]") {
    j2534::J2534Library a{make_full_table()};
    j2534::J2534Library b{std::move(a)};
    REQUIRE(b.functions().connect == &fake_connect);
    // Moved-from library has a zeroed table (move clears it so the
    // source can't accidentally double-call into a vendor DLL).
    REQUIRE(a.functions().connect == nullptr); // NOLINT(bugprone-use-after-move)
}
