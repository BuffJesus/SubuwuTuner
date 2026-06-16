// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/rgb565.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace rgb = st::devices::ets::rgb565;

TEST_CASE("rgb565: pack/unpack round-trips channel high-bits",
          "[devices][ets][rgb565]") {
    // Pack-then-unpack is canonical (the canonical RGB565 expands the
    // top-bit replication on the unpack side, so unpack(pack(x)) returns
    // x for values that already have the low bits replicated).
    rgb::Rgb888 const orig{0xF8, 0xFC, 0xF8};
    auto const packed = rgb::pack(orig);
    auto const back = rgb::unpack(packed);
    CHECK(back.r == 0xFF);
    CHECK(back.g == 0xFF);
    CHECK(back.b == 0xFF);

    // Pure black survives round-trip exactly.
    rgb::Rgb888 const black{0, 0, 0};
    CHECK(rgb::unpack(rgb::pack(black)).r == 0);
    CHECK(rgb::unpack(rgb::pack(black)).g == 0);
    CHECK(rgb::unpack(rgb::pack(black)).b == 0);
}

TEST_CASE("rgb565: bit layout matches RRRRRGGG GGGBBBBB",
          "[devices][ets][rgb565]") {
    // Pure red 0xF8 → 5-bit 0x1F → top 5 bits set.
    auto const red = rgb::pack({0xF8, 0, 0});
    CHECK(red == 0xF800);
    // Pure green 0xFC → 6-bit 0x3F → middle 6 bits set.
    auto const green = rgb::pack({0, 0xFC, 0});
    CHECK(green == 0x07E0);
    // Pure blue 0xF8 → 5-bit 0x1F → bottom 5 bits set.
    auto const blue = rgb::pack({0, 0, 0xF8});
    CHECK(blue == 0x001F);
}

TEST_CASE("rgb565: encode_rgb565_le emits little-endian byte order",
          "[devices][ets][rgb565]") {
    // Pure red pixel → packed 0xF800 → bytes [0x00, 0xF8] in LE.
    std::vector<rgb::Rgb888> const px{{0xF8, 0, 0}};
    auto const r = rgb::encode_rgb565_le(px, 1, 1);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    CHECK((*r)[0] == 0x00);
    CHECK((*r)[1] == 0xF8);
}

TEST_CASE("rgb565: encode rejects pixel-count mismatch",
          "[devices][ets][rgb565]") {
    std::vector<rgb::Rgb888> const px(5, {0, 0, 0});
    auto const r = rgb::encode_rgb565_le(px, 3, 2); // expects 6
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("rgb565: decode rejects byte-count mismatch",
          "[devices][ets][rgb565]") {
    std::vector<std::uint8_t> const bytes{0, 0, 0, 0}; // 2 pixels worth
    auto const r = rgb::decode_rgb565_le(bytes, 3, 1); // expects 6 bytes
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("rgb565: encode → decode round-trips at quantization grid",
          "[devices][ets][rgb565]") {
    std::vector<rgb::Rgb888> in;
    for (int i = 0; i < 8; ++i) {
        in.push_back({static_cast<std::uint8_t>(i * 32),
                      static_cast<std::uint8_t>(i * 32),
                      static_cast<std::uint8_t>(i * 32)});
    }
    auto const enc = rgb::encode_rgb565_le(in, 8, 1);
    REQUIRE(enc.has_value());
    auto const dec = rgb::decode_rgb565_le(*enc, 8, 1);
    REQUIRE(dec.has_value());
    // Values at canonical 5/6-bit grid points survive round-trip.
    for (std::size_t i = 0; i < 8; ++i) {
        // r/b channels: 5-bit grid (0, 8, 16, 24...) — index*32 might
        // not land exactly, but pack→unpack is idempotent after the
        // first round. Just confirm pack(in[i]) == pack(dec[i]).
        CHECK(rgb::pack(in[i]) == rgb::pack((*dec)[i]));
    }
}

TEST_CASE("rgb565: full AP-screen encode produces 153600 bytes",
          "[devices][ets][rgb565]") {
    std::vector<rgb::Rgb888> const black(
        rgb::kApScreenWidth * rgb::kApScreenHeight, {0, 0, 0});
    auto const r = rgb::encode_rgb565_le(black, rgb::kApScreenWidth,
                                          rgb::kApScreenHeight);
    REQUIRE(r.has_value());
    CHECK(r->size() == rgb::kApFullScreenBytes);
    CHECK(rgb::kApFullScreenBytes == 153600);
}

TEST_CASE("rgb565: fit_letterbox preserves aspect, pads borders",
          "[devices][ets][rgb565]") {
    // 100×100 white square → fit into 240×320 portrait.
    std::vector<rgb::Rgb888> src(10000, {0xFF, 0xFF, 0xFF});
    auto const r = rgb::fit_letterbox(src, 100, 100,
                                       rgb::kApScreenWidth,
                                       rgb::kApScreenHeight, {0, 0, 0});
    REQUIRE(r.has_value());
    REQUIRE(r->size() == rgb::kApScreenWidth * rgb::kApScreenHeight);
    // Top-left corner must be black (border).
    auto const &tl = (*r)[0];
    CHECK(tl.r == 0);
    CHECK(tl.g == 0);
    CHECK(tl.b == 0);
    // Center pixel must be white (content).
    auto const center_idx = (rgb::kApScreenHeight / 2) * rgb::kApScreenWidth +
                            (rgb::kApScreenWidth / 2);
    auto const &c = (*r)[center_idx];
    CHECK(c.r == 0xFF);
    CHECK(c.g == 0xFF);
    CHECK(c.b == 0xFF);
}

TEST_CASE("rgb565: encode_ap_full_screen end-to-end",
          "[devices][ets][rgb565]") {
    std::vector<rgb::Rgb888> src(10000, {0xF8, 0, 0});
    auto const r = rgb::encode_ap_full_screen(src, 100, 100, {0, 0, 0});
    REQUIRE(r.has_value());
    CHECK(r->size() == rgb::kApFullScreenBytes);
    // First pixel (top-left corner) is letterbox black → packed 0x0000.
    CHECK((*r)[0] == 0x00);
    CHECK((*r)[1] == 0x00);
}

TEST_CASE("rgb565: fit rejects zero-dimension input",
          "[devices][ets][rgb565]") {
    std::vector<rgb::Rgb888> src(0);
    auto const r = rgb::fit_letterbox(src, 0, 0, 240, 320);
    REQUIRE_FALSE(r.has_value());
}
