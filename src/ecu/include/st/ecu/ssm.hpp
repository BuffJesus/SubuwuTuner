// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_ECU_SSM_HPP
#define ST_ECU_SSM_HPP

#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::ssm {

// Wire-format constants for Subaru Select Monitor. Modern Subaru ECUs
// speak SSM2 over two different transports — and the framing differs
// between them:
//
//   * K-Line (ISO 9141/14230 serial, 4800 baud) — pre-2008 EJ Subarus,
//     accessed via a Tactrix OpenPort cable. Carries the SSM frame
//     verbatim including header / addressing / length / checksum bytes.
//
//   * CAN ISO 15765-2 (ISO-TP, 500 kbps) — every 2008+ Subaru including
//     all VA / VB WRXs. The CAN ID + ISO-TP layer carry the addressing
//     and length, so the K-Line wrapper is STRIPPED. The ECU sees just
//     the bare SSM command bytes (`A8 00 + addrs` for a read).
//
// Picking the wrong framing for the bus is a silent failure: the ECU
// receives bytes that don't match its expectations and drops them
// without responding. Diagnosed 2026-05-23 on a real 2017 VA WRX —
// emitting K-Line-shaped frames on CAN caused the adapter's ISO-TP TX
// to hang waiting for a Flow Control that never came.
//
// K-Line request framing (read-by-address):
//
//     +------+------+------+-----+-----+-----+---- N times ----+------+
//     | 0x80 | dst  | src  | LEN | CMD | PAD | addr_hi|md|lo  | CSUM |
//     +------+------+------+-----+-----+-----+----------------+------+
//
//     dst   = 0x10 (engine ECU)
//     src   = 0xF0 (diagnostic tool)
//     LEN   = number of payload bytes (CMD + PAD + 3*N), excluding CSUM
//     CMD   = 0xA8 (read-by-address)
//     PAD   = 0x00
//     CSUM  = 8-bit sum of all preceding bytes, mod 256
//
// K-Line response framing:
//
//     +------+------+------+-----+-----+---- N times ----+------+
//     | 0x80 | 0xF0 | 0x10 | LEN | RSP | data_byte       | CSUM |
//     +------+------+------+-----+-----+-----------------+------+
//
//     RSP   = 0xE8 (positive A8 response = CMD + 0x40)
//     LEN   = 1 (RSP) + N (data bytes)
//
// CAN ISO-TP request framing (read-by-address):
//
//     +-----+-----+---- N times ----+
//     | CMD | PAD | addr_hi|md|lo  |
//     +-----+-----+----------------+
//
//     The 0x80 header, 0x10/0xF0 addressing, LEN, and CSUM bytes are
//     omitted — CAN ID 0x7E0 identifies the destination, the ISO-TP
//     PCI carries the length, and CAN's frame CRC supersedes CSUM.
//
// CAN ISO-TP response framing:
//
//     +-----+---- N times ----+
//     | RSP | data_byte       |
//     +-----+-----------------+
//
// Negative responses use RSP = 0x7F with a one-byte NRC; SsmClient maps
// these to ErrorCode::EcuRejected with the NRC in the message.

// Selects which wire format the builders / parsers / SsmClient produce
// and consume. Pick to match the underlying transport — KLine for
// ISO 9141 serial, IsoTp for CAN-bus ECUs (every 2008+ Subaru).
enum class Framing : std::uint8_t {
    KLine = 0, // K-Line / ISO 9141 — full wrapper (80 10 F0 LEN ... CSUM)
    IsoTp = 1, // CAN ISO 15765-2 — bare SSM payload, framing carried by CAN+ISO-TP
};

inline constexpr std::uint8_t kHeader = 0x80;
inline constexpr std::uint8_t kDestEcu = 0x10;
inline constexpr std::uint8_t kSrcTool = 0xF0;
inline constexpr std::uint8_t kCmdReadByAddress = 0xA8;
inline constexpr std::uint8_t kRespReadByAddress = 0xE8;
inline constexpr std::uint8_t kCmdWriteByAddress = 0xB0;
inline constexpr std::uint8_t kRespWriteByAddress = 0xF0;
inline constexpr std::uint8_t kCmdBlockWrite = 0xB8;
inline constexpr std::uint8_t kRespBlockWrite = 0xF8;
inline constexpr std::uint8_t kNegativeResponse = 0x7F;
inline constexpr std::uint8_t kPadByte = 0x00;

// Maximum number of data bytes in a single block-write frame, bounded by
// the 8-bit LEN field. LEN = 1 (CMD) + 3 (addr) + N, so N <= 251.
inline constexpr std::size_t kMaxBlockWriteBytes = 251;

// Maximum addressable address. SSM uses 24-bit memory addresses.
inline constexpr std::uint32_t kMaxAddress = 0x00FFFFFFU;

// Sum bytes mod 256.
[[nodiscard]] std::uint8_t ssm_checksum(std::span<std::uint8_t const> bytes) noexcept;

// Build the wire bytes for a multi-address read-by-address request.
// Returns InvalidArgument if any address exceeds kMaxAddress, or if the
// resulting payload would exceed what the LEN byte can encode.
//
// The K-Line LEN-byte ceiling does not apply to IsoTp (ISO-TP carries
// length in its own 12-bit field), so on IsoTp the request can grow up
// to whatever the transport accepts — capped in practice by the SSM
// 24-bit address space and the ECU's own response buffer.
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_a8_request(std::span<std::uint32_t const> addresses, Framing framing = Framing::KLine);

// Parse a response frame from the ECU, expecting `expected_n` data bytes.
// On KLine, verifies the header, source/dest, length, response byte, and
// checksum. On IsoTp, the response is `RSP <data...>` (positive) or
// `7F <NRC>` (negative) — no wrapper bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>>
parse_a8_response(std::span<std::uint8_t const> resp, std::size_t expected_n,
                  Framing framing = Framing::KLine);

// Build the wire bytes for a single-byte write-by-address request.
// KLine frame: 80 10 F0 [LEN] B0 [addr_hi addr_med addr_low] [data] [CSUM]
// IsoTp frame: B0 [addr_hi addr_med addr_low] [data]
// Returns InvalidArgument on addr > 24-bit.
//
// For multi-byte writes at consecutive addresses, use build_b8_request
// instead.
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_b0_request(std::uint32_t address, std::uint8_t data, Framing framing = Framing::KLine);

// Parse a write response. The ECU typically echoes the written byte, which
// we surface to the caller so they can confirm the write took. Negative
// responses (NRC) become EcuRejected like for A8.
[[nodiscard]] Result<std::uint8_t> parse_b0_response(std::span<std::uint8_t const> resp,
                                                    Framing framing = Framing::KLine);

// Build the wire bytes for a block-write request — N consecutive bytes
// written at `address`, `address+1`, …, `address+N-1`.
//
// KLine frame: 80 10 F0 [LEN] B8 [addr_hi addr_med addr_low] [d0 … d_{N-1}] [CSUM]
// IsoTp frame: B8 [addr_hi addr_med addr_low] [d0 … d_{N-1}]
//
// Returns InvalidArgument if:
//   - data is empty,
//   - address > 24-bit, or address + N - 1 > 24-bit,
//   - on KLine: N > kMaxBlockWriteBytes (single-frame LEN limit).
//
// Same RAM-write semantics as build_b0_request — this is NOT a flash-write
// path. Block opcodes are not fully consistent across all Subaru ECU
// families; this framing matches publicly-documented modern (CAN-era)
// implementations and needs validation against a real ECU.
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_b8_request(std::uint32_t address, std::span<std::uint8_t const> data,
                 Framing framing = Framing::KLine);

// Parse a block-write response. The ECU echoes all N written bytes; we
// surface them so the caller can verify each one against what it sent.
// Negative responses (NRC) become EcuRejected like for A8 / B0.
[[nodiscard]] Result<std::vector<std::uint8_t>>
parse_b8_response(std::span<std::uint8_t const> resp, std::size_t expected_n,
                  Framing framing = Framing::KLine);

// High-level client that wraps the framing and talks to a transport.
class SsmClient {
public:
    explicit SsmClient(transport::ITransport &t,
                       Framing framing = Framing::KLine) noexcept
        : transport_{&t}, framing_{framing} {}

    [[nodiscard]] Framing framing() const noexcept {
        return framing_;
    }

    // Read one byte per address. The returned vector has size addresses.size().
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    read(std::span<std::uint32_t const> addresses,
         std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    // Read N contiguous bytes starting at base_address. Convenience over read().
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    read_block(std::uint32_t base_address, std::size_t length,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    // Write a single byte to the given ECU address. Returns the byte the ECU
    // echoed back; callers should verify it matches what they sent.
    //
    // Important: this is RAM/scratchpad write semantics. Flashing the ECU's
    // program memory is a separate Phase 4 routine with brick-protection and
    // sector-erase steps; do NOT use write() for that.
    [[nodiscard]] Result<std::uint8_t>
    write(std::uint32_t address, std::uint8_t data,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

    // Write `data.size()` consecutive bytes starting at `address`. The ECU
    // echoes every written byte; the returned vector is that echo so the
    // caller can verify the write took byte-by-byte. Same RAM-only
    // semantics as write(). For payloads larger than kMaxBlockWriteBytes,
    // the caller should chunk and call write_block() multiple times.
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    write_block(std::uint32_t address, std::span<std::uint8_t const> data,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

private:
    transport::ITransport *transport_;
    Framing framing_;
};

} // namespace st::ecu::ssm

#endif // ST_ECU_SSM_HPP
