// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/cobb_ap_packet.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::transport::ap3 {

namespace {

// CRC-32 reflected, polynomial 0xEDB88320, init 0xFFFFFFFF, no final
// XOR (i.e. zlib's crc32 with the final XOR removed). The standard
// zlib.crc32 *does* the final XOR; we mirror its behavior by XOR-ing
// the result with 0xFFFFFFFF at the end, which inverts back.
//
// Equivalent to `zlib.crc32(data) ^ 0xFFFFFFFF` in the spec's Python.
constexpr std::array<std::uint32_t, 256> build_crc_table() noexcept {
    std::array<std::uint32_t, 256> tbl{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1) : (c >> 1);
        }
        tbl[i] = c;
    }
    return tbl;
}

constexpr auto kCrcTable = build_crc_table();

std::uint32_t crc32_zlib(std::span<std::uint8_t const> bytes) noexcept {
    std::uint32_t c = 0xFFFFFFFFU;
    for (auto b : bytes) {
        c = kCrcTable[(c ^ b) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

} // namespace

std::uint32_t packet_crc(std::span<std::uint8_t const> packet_without_crc) noexcept {
    // Per spec §5, the CRC input includes 4 zero bytes appended after
    // the packet bytes (the on-wire CRC slot is part of the input
    // domain). zlib.crc32(data + b'\0\0\0\0') ^ 0xFFFFFFFF.
    std::vector<std::uint8_t> buf;
    buf.reserve(packet_without_crc.size() + kCrcSize);
    buf.insert(buf.end(), packet_without_crc.begin(), packet_without_crc.end());
    buf.insert(buf.end(), kCrcSize, std::uint8_t{0});
    return crc32_zlib(buf) ^ 0xFFFFFFFFU;
}

bool is_blocked_command(std::uint8_t type) noexcept {
    // Per spec §6.0 codec-level command filtering. Defense-in-depth:
    // even a buggy higher layer can't send these through us. Allowed
    // cmd bytes are NOT enumerated here on purpose — the spec leaves
    // a few "characterized enough for callers to decide" (cmd 0x12,
    // 0x1a, 0x2f) and that policy lives one layer up.
    switch (type) {
    case 0x05: // reboot
    case 0x06: // OTA firmware update
    case 0x07: // legacy upload setup to path_type=0 (system area)
    case 0x08: // legacy upload data to path_type=0
    case 0x18: // documented infinite loop in handler — guaranteed daze
    case 0x29: // OnStartDirectDongleTalk — may exit USB state
        return true;
    default:
        break;
    }
    return type > 0x31; // beyond the documented dispatcher range
}

Result<std::vector<std::uint8_t>>
encode_packet(std::uint8_t type, std::span<std::uint8_t const> body) {
    if (is_blocked_command(type)) {
        char buf[16];
        (void)std::snprintf(buf, sizeof(buf), "0x%02X", type);
        return st::failure(st::ErrorCode::PolicyDenied,
                           std::string{"ap3::encode_packet: cmd "} + buf +
                               " is on the spec §6.0 codec-level block list "
                               "(reboot / OTA / legacy-upload-to-system / "
                               "infinite-loop / DirectDongleTalk / above-range)");
    }
    if (body.size() > kMaxBodySize) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "ap3::encode_packet: body size " + std::to_string(body.size()) +
                               " exceeds wire_len cap " + std::to_string(kMaxBodySize));
    }
    std::uint32_t const wire_len = static_cast<std::uint32_t>(body.size() + kCrcSize);

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + body.size() + kCrcSize);
    out.push_back(kSyncByte0);
    out.push_back(kSyncByte1);
    // u24 BE at [2..4]. wire_len fits in 24 bits (capped above).
    out.push_back(static_cast<std::uint8_t>((wire_len >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((wire_len >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(wire_len & 0xFFU));
    out.push_back(0x00U); // reserved
    out.push_back(type);
    out.insert(out.end(), body.begin(), body.end());

    std::uint32_t const crc = packet_crc(out);
    out.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return out;
}

Result<PacketHeader> decode_header(std::span<std::uint8_t const> bytes) {
    if (bytes.size() < kHeaderSize) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::decode_header: need " + std::to_string(kHeaderSize) +
                               " bytes, have " + std::to_string(bytes.size()));
    }
    if (bytes[0] != kSyncByte0 || bytes[1] != kSyncByte1) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_header: sync magic mismatch");
    }
    if (bytes[5] != 0x00U) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_header: reserved byte [5] not zero");
    }
    std::uint32_t const wire_len = (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[3]) << 8) |
                                   static_cast<std::uint32_t>(bytes[4]);
    if (wire_len < kCrcSize) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_header: wire_len " + std::to_string(wire_len) +
                               " is shorter than CRC trailer");
    }
    PacketHeader h{};
    h.type = bytes[6];
    h.wire_len = wire_len;
    h.body_size = wire_len - static_cast<std::uint32_t>(kCrcSize);
    return h;
}

Status verify_crc(std::span<std::uint8_t const> packet) {
    if (packet.size() < kFixedOverhead) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::verify_crc: packet too short");
    }
    std::size_t const without_crc_size = packet.size() - kCrcSize;
    std::uint32_t const expected = packet_crc(packet.subspan(0, without_crc_size));
    std::uint32_t const observed = (static_cast<std::uint32_t>(packet[without_crc_size]) << 24) |
                                   (static_cast<std::uint32_t>(packet[without_crc_size + 1]) << 16) |
                                   (static_cast<std::uint32_t>(packet[without_crc_size + 2]) << 8) |
                                   static_cast<std::uint32_t>(packet[without_crc_size + 3]);
    if (expected != observed) {
        char buf[64];
        (void)std::snprintf(buf, sizeof(buf), "expected 0x%08X observed 0x%08X", expected, observed);
        return st::failure(st::ErrorCode::BadChecksum,
                           std::string{"ap3::verify_crc: "} + buf);
    }
    return st::ok();
}

Result<std::span<std::uint8_t const>>
decode_packet_body(std::span<std::uint8_t const> packet) {
    auto header = decode_header(packet);
    if (!header.has_value()) {
        return st::failure(std::move(header).error());
    }
    std::size_t const expected_size = kHeaderSize + header->body_size + kCrcSize;
    if (packet.size() != expected_size) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_packet_body: packet size " +
                               std::to_string(packet.size()) + " != expected " +
                               std::to_string(expected_size));
    }
    if (auto v = verify_crc(packet); !v.has_value()) {
        return st::failure(std::move(v).error());
    }
    return packet.subspan(kHeaderSize, header->body_size);
}

} // namespace st::transport::ap3
