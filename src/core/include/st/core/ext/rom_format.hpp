// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_ROM_FORMAT_HPP
#define ST_CORE_EXT_ROM_FORMAT_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace st::ext {

// RomFormat — reads + writes a container format that wraps raw ROM
// bytes. Examples: `.bin` (raw), `.hex` (Intel HEX), `.srec` / `.mot`
// / `.s19` (Motorola SREC).
//
// Implementations are pure functions of the bytes — no filesystem
// access. The host layer is responsible for opening files; this
// interface is byte-in / byte-out so adapters can be tested in
// isolation and so the in-memory pipeline (decode, edit, encode) is
// uniform.
class RomFormat {
public:
    virtual ~RomFormat() = default;

    // Stable identifier (e.g. "raw-bin", "intel-hex", "motorola-srec").
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Canonical file extension WITHOUT the leading dot (e.g. "bin",
    // "hex", "srec", "s19"). Used by file pickers + auto-detect
    // tie-breaking.
    [[nodiscard]] virtual std::string_view canonical_extension() const noexcept = 0;

    // Quick byte-pattern probe. Returns true if `head` "looks like"
    // this format. Hosts call this on every registered format with
    // the first ~256 bytes of the input to pick a decoder. False
    // negatives are OK (caller falls back to extension); false
    // positives risk decoding the wrong format.
    [[nodiscard]] virtual bool looks_like(std::span<std::uint8_t const> head) const noexcept = 0;

    // Decode the format's bytes into raw ROM bytes (the contiguous,
    // little-or-big-endian byte image of the ECU's flash). Empty
    // input is allowed; the implementation returns an empty result
    // vector. Returns ErrorCode::ParseError / BadMagic / UnexpectedEof
    // for malformed input.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>>
    decode(std::span<std::uint8_t const> bytes) const = 0;

    // Encode raw ROM bytes into the format's container bytes. The
    // inverse of decode within tolerance: `decode(encode(x))` should
    // equal `x` for every valid input. Whitespace + comment
    // differences in text-based formats are acceptable round-trip
    // tolerances; document them in the implementation.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>>
    encode(std::span<std::uint8_t const> rom) const = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_ROM_FORMAT_HPP
