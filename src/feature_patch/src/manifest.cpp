// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/manifest.hpp"

#include <cstring>
#include <string>

namespace st::feature_patch {

namespace {

void write_u32_be(std::uint8_t *dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

[[nodiscard]] std::uint32_t read_u32_be(std::uint8_t const *src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24U) |
           (static_cast<std::uint32_t>(src[1]) << 16U) |
           (static_cast<std::uint32_t>(src[2]) << 8U) | static_cast<std::uint32_t>(src[3]);
}

void write_u16_be(std::uint8_t *dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    dst[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

[[nodiscard]] std::uint16_t read_u16_be(std::uint8_t const *src) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[0]) << 8U) |
                                      static_cast<std::uint16_t>(src[1]));
}

void encode_record(std::uint8_t *dst, ManifestRecord const &r) noexcept {
    write_u32_be(dst, r.target_address);
    dst[4] = r.type;
    dst[5] = r.op;
    write_u16_be(dst + 6, r.length_bytes);
}

[[nodiscard]] ManifestRecord decode_record(std::uint8_t const *src) noexcept {
    ManifestRecord r{};
    r.target_address = read_u32_be(src);
    r.type = src[4];
    r.op = src[5];
    r.length_bytes = read_u16_be(src + 6);
    return r;
}

} // namespace

std::vector<std::uint8_t> encode(std::span<ManifestRecord const> records) {
    std::vector<std::uint8_t> out;
    out.resize((records.size() + 1) * kRecordSize, 0);
    std::size_t off = 0;
    for (auto const &r : records) {
        encode_record(out.data() + off, r);
        off += kRecordSize;
    }
    // Terminator is left as all-zero from the resize default.
    return out;
}

Result<std::vector<ManifestRecord>> decode(std::span<std::uint8_t const> bytes) {
    if (bytes.size() % kRecordSize != 0) {
        std::string msg{"feature_patch::decode: byte length "};
        msg.append(std::to_string(bytes.size()));
        msg.append(" is not a multiple of ");
        msg.append(std::to_string(kRecordSize));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    std::vector<ManifestRecord> out;
    out.reserve(bytes.size() / kRecordSize);
    for (std::size_t off = 0; off < bytes.size(); off += kRecordSize) {
        ManifestRecord const r = decode_record(bytes.data() + off);
        if (r.is_terminator()) {
            return out; // happy path — terminator found
        }
        out.push_back(r);
    }
    return failure(ErrorCode::ParseError,
                   "feature_patch::decode: ran out of input without finding a "
                   "terminator record (a manifest must be self-terminated; the "
                   "all-zero sentinel marks the end of scannable records)");
}

} // namespace st::feature_patch
