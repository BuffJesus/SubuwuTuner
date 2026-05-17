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
// touching USB. Real-hardware byte channels (libusb on Windows/
// Linux, native CDC on macOS) land alongside the OBDX adapter
// when it arrives.
//
// At open() the Transport drives the OBDX-specific handshake:
//   1. ELM mode (boot default): write `AT @1\r`, read until `>`,
//      verify the response signature claims OBDX. (Wrong device →
//      bail out before we corrupt anything.)
//   2. Write `DX DP 1\r`, read until `>` — last ELM-mode response.
//      Adapter is now in DVI binary mode.
//   3. Send a DVI SetProtocol request (opcode 0x31) configuring
//      the bus per LinkConfig. The exact sub-op + payload bytes
//      need verification against the VT v1.06 PDF when adapter
//      arrives; for now we emit the request shape + leave the
//      payload as TODO bytes.
//
// At close() the Transport sends a DVI SoftReboot (opcode 0x25)
// which returns the adapter to ELM mode for the next consumer.
//
// Out of scope for this slice (same shape as j2534::Transport):
//   - Streaming (start_streaming / stop_streaming return
//     NotImplemented). Wires up with the datalogger I/O thread.
//   - Platform USB CDC IDeviceChannel implementations (libusb +
//     CDC native). Lands when the adapter arrives.

#ifndef ST_TRANSPORT_OBDX_TRANSPORT_HPP
#define ST_TRANSPORT_OBDX_TRANSPORT_HPP

#include "st/transport.hpp"
#include "st/transport/obdx_dvi.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::transport::obdx {

// Raw byte channel — the abstraction over "things that look like a
// USB CDC ACM endpoint." Implementations include the real platform
// USB driver (future) and a test fake (in the unit tests).
//
// Semantics:
//   write_bytes(span) — synchronously write every byte. Returns
//     TransportUnavailable if the channel went away.
//   read_bytes(max, timeout) — read AT MOST `max` bytes, with up
//     to `timeout` to wait for any bytes. Returns an empty vector
//     on timeout (NOT an error). Real USB endpoints exhibit this
//     "return what's there" behavior; the Transport's framing
//     layer is responsible for accumulating multiple read_bytes
//     calls into a complete frame.
class IDeviceChannel {
  public:
    IDeviceChannel()                                   = default;
    virtual ~IDeviceChannel()                          = default;
    IDeviceChannel(IDeviceChannel const &)             = delete;
    IDeviceChannel &operator=(IDeviceChannel const &)  = delete;
    IDeviceChannel(IDeviceChannel &&) noexcept         = default;
    IDeviceChannel &operator=(IDeviceChannel &&) noexcept = default;

    [[nodiscard]] virtual st::Status write_bytes(
        std::span<std::uint8_t const> bytes) = 0;

    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_bytes(
        std::size_t max_bytes,
        std::chrono::milliseconds timeout) = 0;
};

// OBDX Transport. Owns the byte channel; serializes every
// send/recv internally (no concurrent DVI exchanges).
class Transport : public ITransport {
  public:
    explicit Transport(std::unique_ptr<IDeviceChannel> channel) noexcept;
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
        return "OBDX";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return firmware_;
    }

    // ---- Inspection (tests) ----------------------------------------

    [[nodiscard]] bool is_open() const noexcept { return open_; }

  private:
    std::unique_ptr<IDeviceChannel> channel_;
    bool                            open_{false};
    std::string                     firmware_;
};

} // namespace st::transport::obdx

#endif // ST_TRANSPORT_OBDX_TRANSPORT_HPP
