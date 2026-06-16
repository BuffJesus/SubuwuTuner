// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// RGB565 little-endian image codec for the COBB AccessPort's
// `startup_screen.fb` family (and, pending the boot-logo RE pass at
// `findings/tuning-knowledge-2026-06-13/boot-logo-re/`, potentially
// the firmware-level COBB splash too).
//
// Format
// ------
//
// - 2 bytes per pixel, LE byte order.
// - Pixel bit layout (high-byte first as a uint16): `RRRRRGGG GGGBBBBB`.
// - AP screen geometry: 240 wide × 320 tall portrait = 76,800 pixels =
//   153,600 bytes for a full-screen image. Smaller banner formats
//   (e.g. 240×100) are valid for sub-screen splashes.
// - No header, no compression, no palette — raw pixel data only.
//
// RE7 (2026-06-12 PM) decoded `startup_screen.fb` against this format
// and confirmed the user-replaceable splash matches.
//
// IP boundary
// -----------
//
// This codec is format mechanics only — universally-known scanline RGB
// encoding. It does NOT bundle, transform, or otherwise wrap any
// COBB-branded image bytes. SubuwuTuner does not ship "restore to
// default COBB logo" content; users who want a restore point pull
// the current splash from their own AP and keep the bytes themselves.

#ifndef ST_DEVICES_ETS_RGB565_HPP
#define ST_DEVICES_ETS_RGB565_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::devices::ets::rgb565 {

inline constexpr std::uint32_t kApScreenWidth = 240;
inline constexpr std::uint32_t kApScreenHeight = 320;
inline constexpr std::uint32_t kApFullScreenBytes =
    kApScreenWidth * kApScreenHeight * 2;

struct Rgb888 {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
};

// Pack a single RGB888 sample into RGB565 LE form (2 bytes). The
// returned uint16's low byte is byte 0 on the wire.
[[nodiscard]] constexpr std::uint16_t pack(Rgb888 const &p) noexcept {
    auto const r5 = static_cast<std::uint16_t>((p.r >> 3) & 0x1F);
    auto const g6 = static_cast<std::uint16_t>((p.g >> 2) & 0x3F);
    auto const b5 = static_cast<std::uint16_t>((p.b >> 3) & 0x1F);
    return static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

// Unpack a single RGB565 sample (as a uint16) into RGB888. Round-trip
// is lossy: pack(unpack(x)) recovers x; unpack(pack(rgb888)) loses the
// low bits of each channel.
[[nodiscard]] constexpr Rgb888 unpack(std::uint16_t s) noexcept {
    auto const r5 = static_cast<std::uint8_t>((s >> 11) & 0x1F);
    auto const g6 = static_cast<std::uint8_t>((s >> 5) & 0x3F);
    auto const b5 = static_cast<std::uint8_t>(s & 0x1F);
    // Expand each channel back to 8 bits by replicating high bits into
    // the low slots (matches what canonical RGB565 decoders do — e.g.
    // libpng's tRNS expansion, SDL's RGB565 surface formats).
    return Rgb888{
        static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
        static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
        static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)),
    };
}

// Encode an RGB888 image (row-major, `width * height` pixels) into
// RGB565 LE bytes. Output is `width * height * 2` bytes. Returns
// InvalidArgument when the input pixel count doesn't match
// `width * height`.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_rgb565_le(
    std::span<Rgb888 const> pixels, std::uint32_t width,
    std::uint32_t height);

// Decode an RGB565 LE byte buffer into RGB888 pixels. Returns
// InvalidArgument when `bytes.size() != width * height * 2`.
[[nodiscard]] Result<std::vector<Rgb888>> decode_rgb565_le(
    std::span<std::uint8_t const> bytes, std::uint32_t width,
    std::uint32_t height);

// Resize-with-letterbox helper: given an arbitrary RGB888 source, fit
// it into a width × height frame, preserving aspect ratio, padding
// the non-content area with `fill`. Nearest-neighbor scaling — good
// enough for boot splashes; not a substitute for a real image library
// on the GUI side, but keeps the AP-side codec self-contained.
[[nodiscard]] Result<std::vector<Rgb888>> fit_letterbox(
    std::span<Rgb888 const> src, std::uint32_t src_w, std::uint32_t src_h,
    std::uint32_t dst_w, std::uint32_t dst_h, Rgb888 fill = {0, 0, 0});

// Convenience: encode an RGB888 image directly into AP-screen-sized
// RGB565 LE bytes (240 × 320), letterboxing as needed. Returns a
// 153,600-byte buffer suitable for pushing to /images/<name>.fb via
// `Client::write_file`.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_ap_full_screen(
    std::span<Rgb888 const> src, std::uint32_t src_w, std::uint32_t src_h,
    Rgb888 letterbox_fill = {0, 0, 0});

} // namespace st::devices::ets::rgb565

#endif // ST_DEVICES_ETS_RGB565_HPP
