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
//
// Subaru bus history correction (verified 2026-05-21 against a real
// 2017 WRX + OBDX Pro VX):
//   - Pre-2008: SSM-over-K-Line (ISO 9141 / KWP2000). EJ era.
//   - 2008+:    CAN-ISO15765 (OBD-II CAN mandate). EJ-DIT, FA-DIT (VA/VB),
//               and everything since.
// Earlier comments here misrepresented "VA WRX = K-Line"; that's wrong.
// VA WRX (2015-2021) and VB WRX (2022+) both run CAN-ISO15765 at 500 kbaud.
enum class LinkKind {
    KLine,       // ISO 9141 / 14230 (KWP2000) — pre-2008 Subaru
    CanIso15765, // CAN-TP — 2008+ Subaru (EJ-DIT, VA WRX, VB WRX, all modern)
    CanFd,       // CAN-FD — future
};

// Standard Subaru OBD-II ECU CAN addressing (ISO15765-4). Engine ECU
// listens on 0x7E0 and responds on 0x7E8 — same as every other OBD-II
// vehicle. Functional broadcast is 0x7DF (not exposed here; tools that
// need it set can_id_request manually).
constexpr std::uint32_t kSubaruEngineCanIdRequest = 0x7E0;
constexpr std::uint32_t kSubaruEngineCanIdResponse = 0x7E8;

struct LinkConfig {
    // CAN-ISO15765 is the right default for every Subaru this project
    // targets (VA WRX, VB WRX, all 2008+ FA-DIT). K-Line was the wrong
    // default historically; corrected 2026-05-21 after a real-OBDX
    // smoke test on a 2017 WRX returned the K-Line-unsupported error.
    LinkKind kind{LinkKind::CanIso15765};
    // Subaru SSM K-Line is 4800 baud (not the 10400 you'd see for
    // KWP2000-on-K-Line on other vendors' platforms). Subaru
    // CAN-ISO15765 is 500000 baud. Default matches the kind default.
    int baud{500000};
    // Default to the Subaru OBD-II engine-ECU pair. Tools that talk to
    // other modules (TCU, ABS) override these.
    std::uint32_t can_id_request{kSubaruEngineCanIdRequest};
    std::uint32_t can_id_response{kSubaruEngineCanIdResponse};
    // Open in passive bus-monitor (sniff) mode. When true:
    //   * Adapter skips CAN filter setup — receives ALL bus traffic, not
    //     just `can_id_response`.
    //   * EnableNetwork uses STATE=0x02 (LISTEN-ONLY) instead of 0x01
    //     (ON), so the adapter never transmits an ACK or any other frame
    //     on the bus. Critical when another tool is bus-mastering on the
    //     same physical OBD-II port (e.g. COBB AccessPort via a Y-cable):
    //     two tools writing to the same CAN-H/CAN-L pair would collide.
    //   * send() / send_recv() reject with TransportUnavailable. Callers
    //     use start_streaming() to receive frames.
    //   * can_id_request / can_id_response are ignored.
    bool listen_only{false};
};

// One frame on the wire. `data` is the application-layer payload — the
// transport implementation handles ISO-TP fragmentation/reassembly on CAN.
// `can_id` is the source CAN ID for CAN-based transports (0 for K-Line
// and other non-CAN buses). In sniff mode this is how callers tell which
// module's traffic they're looking at; in active mode it's redundant
// with the transport's configured can_id_response.
struct Frame {
    std::vector<std::uint8_t> data;
    std::chrono::steady_clock::time_point arrived{};
    std::uint32_t can_id{0};
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

    [[nodiscard]] virtual Status open(LinkConfig const &cfg) = 0;
    [[nodiscard]] virtual Status close() = 0;

    [[nodiscard]] virtual Result<Frame> send_recv(std::span<std::uint8_t const> payload,
                                                  std::chrono::milliseconds timeout) = 0;

    [[nodiscard]] virtual Status send(std::span<std::uint8_t const> payload) = 0;

    [[nodiscard]] virtual Status start_streaming(FrameCallback callback) = 0;
    [[nodiscard]] virtual Status stop_streaming() = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::string_view firmware() const noexcept = 0;
};

} // namespace st::transport

#endif // ST_TRANSPORT_HPP
