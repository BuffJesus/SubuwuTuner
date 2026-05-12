// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_TRANSPORT_HPP
#define ST_TRANSPORT_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace st::transport {

// Which kind of physical link this transport will configure. Each adapter
// implementation supports a subset.
enum class LinkKind {
    KLine,       // ISO 9141 / 14230 (KWP2000) — VA WRX
    CanIso15765, // CAN-TP — VB WRX and most modern Subarus
    CanFd,       // CAN-FD — future
};

struct LinkConfig {
    LinkKind     kind{LinkKind::KLine};
    int          baud{10400};         // K-Line: 10400 typical; CAN: 500000 typical
    std::uint32_t can_id_request{0};   // for CAN protocols only
    std::uint32_t can_id_response{0};  // for CAN protocols only
};

// One frame on the wire. `data` is the application-layer payload — the
// transport implementation handles ISO-TP fragmentation/reassembly on CAN.
struct Frame {
    std::vector<std::uint8_t>             data;
    std::chrono::steady_clock::time_point arrived{};
};

using FrameCallback = std::function<void(Frame const &)>;

// Abstract transport interface. Adapter-specific code (J2534, ELM, STN,
// OBDX) implements this; ECU-protocol clients (SSM, UDS) and the datalogger
// hold a reference to it.
//
// Contracts:
//   * open() must be called before any send/send_recv/start_streaming.
//     close() is idempotent.
//   * send_recv blocks the calling thread up to `timeout` ms for a reply.
//     Internally the transport may use its own I/O thread (see docs/13
//     for the threading model); from the caller's perspective this method
//     looks synchronous.
//   * start_streaming runs the callback on the transport's I/O thread.
//     stop_streaming must wait for the callback to drain.
//   * All methods are safe to call from any thread, but the transport will
//     serialize them internally — there's no concurrent send_recv.
class ITransport {
  public:
    virtual ~ITransport() = default;

    [[nodiscard]] virtual Status open(LinkConfig const &cfg)  = 0;
    [[nodiscard]] virtual Status close()                       = 0;

    [[nodiscard]] virtual Result<Frame> send_recv(
        std::span<std::uint8_t const> payload,
        std::chrono::milliseconds      timeout) = 0;

    [[nodiscard]] virtual Status send(std::span<std::uint8_t const> payload) = 0;

    [[nodiscard]] virtual Status start_streaming(FrameCallback callback) = 0;
    [[nodiscard]] virtual Status stop_streaming()                          = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept     = 0;
    [[nodiscard]] virtual std::string_view firmware() const noexcept = 0;
};

} // namespace st::transport

#endif // ST_TRANSPORT_HPP
