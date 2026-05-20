// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::native::Transport — ITransport implementation on
// top of the native codec (ee09837) and an IByteChannel.
//
// When SubuwuTuner's own hardware (doc-18 handheld) is plugged in
// PC-side over USB CDC ACM, this is the Transport that drives it.
// Same architectural shape as obdx::Transport (70b2af4) but on the
// native framing — no ELM handshake (we own both ends), no vendor
// DLL, no protocol-specific gymnastics. Just the documented
// SOF/seq/opcode/LEN/payload/CRC frame layout and a small set of
// PC-defined opcodes (see st::transport::native::Opcode).
//
// Both K-Line and CAN-15765 are accepted at open() — the device
// firmware multiplexes between FlexCAN_T4 and the K-Line UART
// based on the protocol byte we pass in Connect. CAN-FD is out
// (the doc-18 hardware doesn't speak it).
//
// Out of scope for this slice (mirrors obdx::Transport):
//   - Streaming (StreamStart / StreamStop opcodes exist but the
//     ITransport start_streaming / stop_streaming hooks return
//     NotImplemented — wires up with the datalogger I/O thread).
//   - Platform USB CDC IByteChannel implementations (libusb +
//     macOS native CDC). Lands when the hardware exists.

#ifndef ST_TRANSPORT_NATIVE_TRANSPORT_HPP
#define ST_TRANSPORT_NATIVE_TRANSPORT_HPP

#include "st/transport.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/native.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace st::transport::native {

// LinkConfig → native Connect-opcode payload. Pulled out for
// testability + so the protocol-byte layout is one specific place.
struct ConnectPayload {
    std::uint8_t protocol_id{0};      // mirror J2534: 0x03 K-Line, 0x06 ISO15765
    std::uint32_t baud{0};            // big-endian on the wire
    std::uint16_t can_id_request{0};  // unused for K-Line
    std::uint16_t can_id_response{0}; // unused for K-Line

    [[nodiscard]] std::vector<std::uint8_t> encode() const;
};

// Map a LinkConfig to a ConnectPayload. Returns InvalidArgument for
// CAN-FD (doc-18 hardware is classical-CAN-only) or zero/negative
// baud.
[[nodiscard]] Result<ConnectPayload> connect_payload_for(LinkConfig const &cfg) noexcept;

// The transport. Owns the byte channel + a per-session sequence
// counter for the native protocol's seq-matched request/response.
class Transport : public ITransport {
public:
    explicit Transport(std::unique_ptr<IByteChannel> channel) noexcept;
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
        return "Native";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return firmware_;
    }

    // ---- Inspection (tests) ----------------------------------------

    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }
    [[nodiscard]] std::uint8_t next_seq_for_test() const noexcept {
        return next_seq_;
    }

private:
    std::unique_ptr<IByteChannel> channel_;
    bool open_{false};
    std::uint8_t next_seq_{0};
    std::string firmware_;
};

} // namespace st::transport::native

#endif // ST_TRANSPORT_NATIVE_TRANSPORT_HPP
