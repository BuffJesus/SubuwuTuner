// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::j2534::Transport — ITransport implementation on
// top of a resolved J2534 v04.04 vendor DLL.
//
// Sits between the adapter-agnostic ECU clients (SsmClient,
// UdsClient) and the platform-specific PassThru* function pointers
// owned by a J2534Library. Tactrix OpenPort 2.0 + any other
// v04.04-compliant adapter routes through this class.
//
// Construction: takes a populated J2534Library by value (move).
// The Library carries the function table; for tests, inject one
// via the FunctionTable constructor; for real hardware, load it
// via J2534Library::load(dll_path) (stub today; lands when the
// developer's adapter arrives).
//
// Out-of-scope for this slice:
//   - Streaming (start_streaming / stop_streaming return
//     NotImplemented). Wires up when the datalogger lands and we
//     can give the spawned I/O thread a real consumer.
//   - Adapter discovery (Windows registry walk for installed
//     PassThruSupport.04.04 DLLs). Lands with platform dynload.

#ifndef ST_TRANSPORT_J2534_TRANSPORT_HPP
#define ST_TRANSPORT_J2534_TRANSPORT_HPP

#include "st/transport.hpp"
#include "st/transport/j2534.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace st::transport::j2534 {

// LinkConfig → J2534 Connect arguments. Pulled out for testability +
// so a future adapter-comparison layer can ask "would this LinkConfig
// be accepted?" without opening a device.
struct ConnectParams {
    ProtocolId protocol{ProtocolId::ISO9141};
    u32        flags{0};
    u32        baud{0};
};

// Map a LinkConfig to the J2534 protocol id + flags + baud. Returns
// InvalidArgument when LinkKind has no J2534 mapping (CAN-FD today —
// not in v04.04).
[[nodiscard]] Result<ConnectParams> connect_params_for(
    LinkConfig const &cfg) noexcept;

// Map a J2534 Status to a SubuwuTuner-side ErrorCode. NoError →
// ok(). Timeout / BufferEmpty → TransportTimeout. NotSupported,
// InvalidProtocolId, InvalidBaudrate, InvalidFlags, InvalidIoctlValue,
// InvalidIoctlId → InvalidArgument (the caller asked for something
// the adapter can't do). DeviceNotConnected → TransportUnavailable.
// Everything else (Failed, vendor opaque, etc.) → TransportNack.
// The message embeds the status's documented description plus any
// caller-supplied context.
[[nodiscard]] st::Status to_st_status(
    Status s, std::string_view context = {}) noexcept;

// The transport itself. Owns:
//   - the J2534Library (function table + eventual OS module handle)
//   - the device/channel ids returned by Open/Connect
//   - the active protocol (for outgoing PassThruMsg construction)
//   - a cached firmware-version string (populated on open via
//     ReadVersion when the DLL supports it).
class Transport : public ITransport {
  public:
    explicit Transport(J2534Library library) noexcept;
    ~Transport() override;

    Transport(Transport const &)            = delete;
    Transport &operator=(Transport const &) = delete;
    Transport(Transport &&)                 = delete;
    Transport &operator=(Transport &&)      = delete;

    // ---- ITransport -------------------------------------------------

    [[nodiscard]] st::Status open(LinkConfig const &cfg) override;
    [[nodiscard]] st::Status close() override;

    [[nodiscard]] Result<Frame> send_recv(
        std::span<std::uint8_t const> payload,
        std::chrono::milliseconds      timeout) override;

    [[nodiscard]] st::Status send(
        std::span<std::uint8_t const> payload) override;

    [[nodiscard]] st::Status start_streaming(FrameCallback callback) override;
    [[nodiscard]] st::Status stop_streaming() override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "J2534";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return firmware_;
    }

    // ---- Inspection (tests) ----------------------------------------

    [[nodiscard]] bool       is_open()    const noexcept { return open_; }
    [[nodiscard]] u32        device_id()  const noexcept { return device_id_; }
    [[nodiscard]] u32        channel_id() const noexcept { return channel_id_; }
    [[nodiscard]] ProtocolId protocol()   const noexcept { return protocol_; }

  private:
    J2534Library library_;
    bool         open_{false};
    u32          device_id_{0};
    u32          channel_id_{0};
    ProtocolId   protocol_{ProtocolId::ISO9141};
    std::string  firmware_;
};

} // namespace st::transport::j2534

#endif // ST_TRANSPORT_J2534_TRANSPORT_HPP
