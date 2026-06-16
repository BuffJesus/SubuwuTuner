// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/rgb565.hpp"

#include "st/core/error.hpp"

#include <cstdint>
#include <vector>

namespace st::devices::ets::rgb565 {

Result<std::vector<std::uint8_t>> encode_rgb565_le(
    std::span<Rgb888 const> pixels, std::uint32_t width,
    std::uint32_t height) {
    auto const expected = static_cast<std::size_t>(width) * height;
    if (pixels.size() != expected) {
        return failure(ErrorCode::InvalidArgument,
                       "rgb565: pixel count " +
                           std::to_string(pixels.size()) +
                           " != width*height " + std::to_string(expected));
    }
    std::vector<std::uint8_t> out;
    out.reserve(expected * 2);
    for (auto const &p : pixels) {
        auto const packed = pack(p);
        // Little-endian byte order: low byte first.
        out.push_back(static_cast<std::uint8_t>(packed & 0xFF));
        out.push_back(static_cast<std::uint8_t>((packed >> 8) & 0xFF));
    }
    return out;
}

Result<std::vector<Rgb888>> decode_rgb565_le(
    std::span<std::uint8_t const> bytes, std::uint32_t width,
    std::uint32_t height) {
    auto const expected_bytes =
        static_cast<std::size_t>(width) * height * 2;
    if (bytes.size() != expected_bytes) {
        return failure(ErrorCode::InvalidArgument,
                       "rgb565: byte count " +
                           std::to_string(bytes.size()) +
                           " != width*height*2 " +
                           std::to_string(expected_bytes));
    }
    std::vector<Rgb888> out;
    out.reserve(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        auto const lo = static_cast<std::uint16_t>(bytes[i]);
        auto const hi = static_cast<std::uint16_t>(bytes[i + 1]);
        out.push_back(unpack(static_cast<std::uint16_t>((hi << 8) | lo)));
    }
    return out;
}

Result<std::vector<Rgb888>> fit_letterbox(
    std::span<Rgb888 const> src, std::uint32_t src_w, std::uint32_t src_h,
    std::uint32_t dst_w, std::uint32_t dst_h, Rgb888 fill) {
    auto const src_expected = static_cast<std::size_t>(src_w) * src_h;
    if (src.size() != src_expected) {
        return failure(ErrorCode::InvalidArgument,
                       "rgb565 fit: src pixel count " +
                           std::to_string(src.size()) +
                           " != src_w*src_h " +
                           std::to_string(src_expected));
    }
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "rgb565 fit: zero-dimension input");
    }

    // Aspect-preserving fit. Use integer-only math (no float) so
    // results are deterministic.
    std::uint64_t const scale_num_w =
        static_cast<std::uint64_t>(dst_w) * src_h;
    std::uint64_t const scale_num_h =
        static_cast<std::uint64_t>(dst_h) * src_w;
    std::uint32_t content_w = 0;
    std::uint32_t content_h = 0;
    if (scale_num_w < scale_num_h) {
        // Limited by destination width: content_w = dst_w; height
        // shrinks to preserve aspect.
        content_w = dst_w;
        content_h = static_cast<std::uint32_t>(scale_num_w / src_w);
        if (content_h == 0) {
            content_h = 1;
        }
    } else {
        content_h = dst_h;
        content_w = static_cast<std::uint32_t>(scale_num_h / src_h);
        if (content_w == 0) {
            content_w = 1;
        }
    }
    std::uint32_t const pad_x = (dst_w - content_w) / 2;
    std::uint32_t const pad_y = (dst_h - content_h) / 2;

    std::vector<Rgb888> out(static_cast<std::size_t>(dst_w) * dst_h, fill);

    // Nearest-neighbor sample from src into the content rectangle.
    for (std::uint32_t y = 0; y < content_h; ++y) {
        auto const sy = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * src_h) / content_h);
        for (std::uint32_t x = 0; x < content_w; ++x) {
            auto const sx = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * src_w) / content_w);
            auto const src_idx =
                static_cast<std::size_t>(sy) * src_w + sx;
            auto const dst_idx =
                static_cast<std::size_t>(pad_y + y) * dst_w + (pad_x + x);
            out[dst_idx] = src[src_idx];
        }
    }
    return out;
}

Result<std::vector<std::uint8_t>> encode_ap_full_screen(
    std::span<Rgb888 const> src, std::uint32_t src_w, std::uint32_t src_h,
    Rgb888 letterbox_fill) {
    auto fit_r = fit_letterbox(src, src_w, src_h, kApScreenWidth,
                               kApScreenHeight, letterbox_fill);
    if (!fit_r.has_value()) {
        return failure(fit_r.error());
    }
    return encode_rgb565_le(*fit_r, kApScreenWidth, kApScreenHeight);
}

} // namespace st::devices::ets::rgb565
