// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/crc32.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::span<std::uint8_t const> as_bytes(std::string_view sv) {
    return std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const *>(sv.data()), sv.size()};
}

} // namespace

TEST_CASE("CRC32 canonical test vector 123456789 == 0xCBF43926", "[core][crc32]") {
    REQUIRE(st::crc32(as_bytes("123456789")) == 0xCBF43926U);
}

TEST_CASE("CRC32 of empty input is zero", "[core][crc32]") {
    REQUIRE(st::crc32({}) == 0x00000000U);
}

TEST_CASE("CRC32 of single 'a' matches known value", "[core][crc32]") {
    // crc32("a") = 0xE8B7BE43 (zlib reference)
    REQUIRE(st::crc32(as_bytes("a")) == 0xE8B7BE43U);
}

TEST_CASE("CRC32 of 'The quick brown fox jumps over the lazy dog'",
          "[core][crc32]") {
    // Canonical pangram CRC32 = 0x414FA339
    REQUIRE(st::crc32(as_bytes("The quick brown fox jumps over the lazy dog"))
            == 0x414FA339U);
}

TEST_CASE("Streaming Crc32 matches one-shot", "[core][crc32]") {
    std::string_view const sv = "The quick brown fox jumps over the lazy dog";
    auto const bytes = as_bytes(sv);

    st::Crc32 streaming;
    streaming.update(bytes.subspan(0, 10));
    streaming.update(bytes.subspan(10));

    REQUIRE(streaming.value() == st::crc32(bytes));
}

TEST_CASE("Crc32 reset() returns state to initial", "[core][crc32]") {
    st::Crc32 c;
    c.update(as_bytes("garbage"));
    c.reset();
    c.update(as_bytes("123456789"));
    REQUIRE(c.value() == 0xCBF43926U);
}

TEST_CASE("CRC32 over a non-trivial byte sequence", "[core][crc32]") {
    std::vector<std::uint8_t> data(256);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i);
    }
    // 256-byte ramp 0..255 has CRC32 = 0x29058C73 (zlib reference).
    REQUIRE(st::crc32(data) == 0x29058C73U);
}
