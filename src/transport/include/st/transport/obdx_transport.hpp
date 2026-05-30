// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::obdx::Transport — ITransport implementation on
// top of the DVI codec (codec/265e692) and a USB CDC ACM byte
// channel. Targets the OBDX Pro VX adapter (and any other OBDX-
// family device that exposes the DVI binary protocol).
//
// Like j2534::Transport (2f0d054), the byte channel is abstracted
// behind a tiny interface so tests can inject a fake without
// touching USB. Real-hardware byte channel today is the
// platform serial port (`serial_byte_channel_win.cpp` /
// `serial_byte_channel_posix.cpp`) — the VX exposes a USB CDC ACM
// virtual COM port, so a raw libusb backend has not been needed.
//
// At open() the Transport drives the OBDX-specific handshake:
//   1. ELM mode (boot default): write `AT @1\r`, read until `>`,
//      verify the response signature claims OBDX. (Wrong device →
//      bail out before we corrupt anything.)
//   2. Write `DX DP 1\r`, read until `>` — last ELM-mode response.
//      Adapter is now in DVI binary mode.
//   3. Send a DVI SetProtocol request (opcode 0x31) configuring
//      the bus per LinkConfig. Payload shape per VT v1.06 §3.10.1
//      is `[SUB=0x01][PROTO]`; PROTO maps from `LinkConfig::kind`
//      in `set_protocol_payload()`. Bus baud + CAN IDs are NOT
//      carried by SetProtocol — they come from SetBusVoltage /
//      SetCanID frames that open() drives right after.
//   4. Send EnableNetwork (§3.10.2 — also opcode 0x31, SUB=0x02,
//      STATE=0x01). Without this the adapter accepts SetProtocol
//      but silently drops every ECU response.
//
// At close() the Transport sends a DVI SoftReboot (opcode 0x25)
// which returns the adapter to ELM mode for the next consumer.
//
// Streaming (start_streaming / stop_streaming) is wired up — see
// obdx_transport.cpp §RX thread.

#ifndef ST_TRANSPORT_OBDX_TRANSPORT_HPP
#define ST_TRANSPORT_OBDX_TRANSPORT_HPP

#include "st/transport.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/obdx_dvi.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace st::transport::obdx {

// Process-wide hex-trace toggle. When set, every send_recv on every
// obdx::Transport instance prints its TX payload and the RX frame (or
// the error code) as hex to stderr, prefixed with `[trace][obdx-tx]`
// / `[trace][obdx-rx]` / `[trace][obdx-err]`. Off by default; the GUI
// flips it on for the duration of a Read ROM operation when the
// "Verbose console output" checkbox is set, so the user can see
// whether bytes are actually moving between host and adapter.
//
// Implementation is a single std::atomic<bool> — cost when off is one
// relaxed load per send_recv, which is negligible compared to the
// USB round-trip the call is already doing.
void set_trace_enabled(bool on) noexcept;
[[nodiscard]] bool trace_enabled() noexcept;

// Back-compat alias. The byte-channel abstraction lives in
// st::transport::IByteChannel (byte_channel.hpp) — it's generic
// to USB CDC, not OBDX-specific, and shared with native::Transport
// (which speaks our own protocol). Existing references to
// obdx::IDeviceChannel continue to compile.
using IDeviceChannel = st::transport::IByteChannel;

// OBDX Transport. Owns the byte channel; serializes every
// send/recv internally (no concurrent DVI exchanges).
class Transport : public ITransport {
public:
    explicit Transport(std::unique_ptr<IDeviceChannel> channel) noexcept;
    ~Transport() override;

    Transport(Transport const &) = delete;
    Transport &operator=(Transport const &) = delete;
    Transport(Transport &&) = delete;
    Transport &operator=(Transport &&) = delete;

    // ---- ITransport -------------------------------------------------

    [[nodiscard]] st::Status open(LinkConfig const &cfg) override;
    [[nodiscard]] st::Status close() override;

    [[nodiscard]] Result<Frame> send_recv(std::span<std::uint8_t const> payload,
                                          std::chrono::milliseconds timeout) override;

    [[nodiscard]] st::Status send(std::span<std::uint8_t const> payload) override;

    [[nodiscard]] st::Status start_streaming(FrameCallback callback) override;
    [[nodiscard]] st::Status stop_streaming() override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "OBDX";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return firmware_;
    }

    // ---- Inspection (tests) ----------------------------------------

    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }

private:
    std::unique_ptr<IDeviceChannel> channel_;
    bool open_{false};
    std::string firmware_;
    // Cached at open() time. CAN-ISO15765 send_recv prepends the request
    // ID (BE 4-byte) to every TX payload, and validates that incoming
    // RxSmall pushes carry the response ID. Without these the adapter
    // either fires bytes onto an undefined CAN ID, or drops every
    // incoming ECU reply (no filter match).
    LinkKind kind_{LinkKind::CanIso15765};
    std::uint32_t can_id_request_{0};
    std::uint32_t can_id_response_{0};
    bool listen_only_{false};
    // start_streaming spawns this thread; stop_streaming flips
    // `streaming_stop_` and joins. The thread does blocking
    // read_dvi_frame calls on the byte channel and posts decoded frames
    // to streaming_callback_. send/send_recv are mutually exclusive with
    // streaming (the byte channel can't be shared) — the contract is
    // enforced at start_streaming time.
    std::thread streaming_thread_;
    std::atomic<bool> streaming_stop_{false};
    FrameCallback streaming_callback_;
};

} // namespace st::transport::obdx

#endif // ST_TRANSPORT_OBDX_TRANSPORT_HPP
