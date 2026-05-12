// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubaruTuner Authors

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/rom.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> make_bytes(std::initializer_list<std::uint8_t> il) {
    return std::vector<std::uint8_t>(il);
}

// Writes a binary blob to a unique temp file. Caller owns deletion.
std::filesystem::path write_temp_file(std::vector<std::uint8_t> const &data) {
    auto path = std::filesystem::temp_directory_path()
              / ("st_rom_test_" + std::to_string(std::random_device{}()) + ".bin");
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<char const *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();
    return path;
}

} // namespace

TEST_CASE("Rom::from_bytes preserves data and size", "[rom][construction]") {
    auto const r = st::Rom::from_bytes(make_bytes({0xDE, 0xAD, 0xBE, 0xEF}));
    REQUIRE(r.size() == 4);
    REQUIRE_FALSE(r.empty());
    REQUIRE(r.data()[0] == 0xDE);
    REQUIRE(r.data()[3] == 0xEF);
}

TEST_CASE("Rom::from_bytes accepts empty input", "[rom][construction]") {
    auto const r = st::Rom::from_bytes({});
    REQUIRE(r.size() == 0);
    REQUIRE(r.empty());
}

TEST_CASE("Rom::from_file round-trips bytes", "[rom][io]") {
    auto const data = make_bytes({0x53, 0x55, 0x42, 0x41, 0x52, 0x55});
    auto const path = write_temp_file(data);
    auto const r    = st::Rom::from_file(path);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == data.size());
    REQUIRE(r->data()[0] == 0x53);
    std::filesystem::remove(path);
}

TEST_CASE("Rom::from_file reports FileNotFound for a missing path", "[rom][io]") {
    auto const r = st::Rom::from_file("nope/this/path/does/not/exist.bin");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("Rom::from_file rejects directories", "[rom][io]") {
    auto const r = st::Rom::from_file(std::filesystem::temp_directory_path());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Rom::from_file enforces max_bytes", "[rom][io]") {
    auto const data = std::vector<std::uint8_t>(1024, 0xAA);
    auto const path = write_temp_file(data);
    auto const r    = st::Rom::from_file(path, /*max_bytes=*/512);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
    std::filesystem::remove(path);
}

TEST_CASE("Rom::slice clamps and validates ranges", "[rom][slice]") {
    auto const r = st::Rom::from_bytes(make_bytes({0x00, 0x01, 0x02, 0x03, 0x04}));

    SECTION("Valid slice returns the expected window") {
        auto const s = r.slice(1, 3);
        REQUIRE(s.has_value());
        REQUIRE(s->size() == 3);
        REQUIRE((*s)[0] == 0x01);
        REQUIRE((*s)[2] == 0x03);
    }

    SECTION("Past-end offset fails OutOfRange") {
        auto const s = r.slice(6, 1);
        REQUIRE_FALSE(s.has_value());
        REQUIRE(s.error().code() == st::ErrorCode::OutOfRange);
    }

    SECTION("Length spilling past end fails OutOfRange") {
        auto const s = r.slice(3, 10);
        REQUIRE_FALSE(s.has_value());
        REQUIRE(s.error().code() == st::ErrorCode::OutOfRange);
    }

    SECTION("Empty slice at end is valid") {
        auto const s = r.slice(5, 0);
        REQUIRE(s.has_value());
        REQUIRE(s->empty());
    }
}

TEST_CASE("Rom big-endian reads", "[rom][read][big-endian]") {
    auto const r = st::Rom::from_bytes(make_bytes({0x12, 0x34, 0x56, 0x78}));

    REQUIRE(*r.read_u8(0) == 0x12);
    REQUIRE(*r.read_u16_be(0) == 0x1234);
    REQUIRE(*r.read_u16_be(2) == 0x5678);
    REQUIRE(*r.read_u32_be(0) == 0x12345678U);
}

TEST_CASE("Rom little-endian reads", "[rom][read][little-endian]") {
    auto const r = st::Rom::from_bytes(make_bytes({0x12, 0x34, 0x56, 0x78}));

    REQUIRE(*r.read_u16_le(0) == 0x3412);
    REQUIRE(*r.read_u16_le(2) == 0x7856);
    REQUIRE(*r.read_u32_le(0) == 0x78563412U);
}

TEST_CASE("Rom reads fail at boundaries with OutOfRange", "[rom][read][bounds]") {
    auto const r = st::Rom::from_bytes(make_bytes({0xAA, 0xBB, 0xCC}));

    REQUIRE_FALSE(r.read_u8(3).has_value());
    REQUIRE_FALSE(r.read_u16_be(2).has_value());
    REQUIRE_FALSE(r.read_u32_be(0).has_value());

    REQUIRE(r.read_u8(3).error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("Rom reads survive near-SIZE_MAX offsets without UB", "[rom][read][safety]") {
    auto const r = st::Rom::from_bytes(make_bytes({0xAA, 0xBB, 0xCC, 0xDD}));

    auto const huge = static_cast<std::size_t>(-1);
    REQUIRE_FALSE(r.read_u8(huge).has_value());
    REQUIRE_FALSE(r.read_u16_be(huge - 1).has_value());
    REQUIRE_FALSE(r.read_u32_be(huge - 3).has_value());
    REQUIRE_FALSE(r.slice(huge, 1).has_value());
}

TEST_CASE("Rom::read_ascii stops at NUL", "[rom][read_ascii]") {
    auto const r = st::Rom::from_bytes(
        make_bytes({'A', 'S', '8', '0', 'U', 0x00, 'x', 'y'}));

    auto const s = r.read_ascii(0, 16);
    REQUIRE(s.has_value());
    REQUIRE(*s == "AS80U");
}

TEST_CASE("Rom::read_ascii respects max_length", "[rom][read_ascii]") {
    auto const r = st::Rom::from_bytes(make_bytes({'a', 'b', 'c', 'd', 'e'}));

    auto const s = r.read_ascii(0, 3);
    REQUIRE(s.has_value());
    REQUIRE(*s == "abc");
}

TEST_CASE("Rom::read_ascii errors on non-printable bytes", "[rom][read_ascii]") {
    auto const r = st::Rom::from_bytes(make_bytes({'a', 'b', 0x7F, 'd'}));

    auto const s = r.read_ascii(0, 8);
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Rom::crc32 matches canonical value for known vector", "[rom][crc32]") {
    // "123456789" → 0xCBF43926
    auto const r = st::Rom::from_bytes(make_bytes(
        {'1', '2', '3', '4', '5', '6', '7', '8', '9'}));
    REQUIRE(r.crc32() == 0xCBF43926U);
}

TEST_CASE("Rom::scan_ascii finds embedded calibration-ID-like strings",
          "[rom][scan_ascii]") {
    // Bytes of binary noise, an embedded CID "AS80U", more noise, then "ABC123"
    std::vector<std::uint8_t> data{
        0x00, 0xFF, 0x80, 0x12,
        'A', 'S', '8', '0', 'U',
        0x00, 0xCC, 0xDD,
        'A', 'B', 'C', '1', '2', '3',
        0x00,
        'x', 'y',                       // length 2 — below default threshold
    };
    auto const r       = st::Rom::from_bytes(std::move(data));
    auto const strings = r.scan_ascii(/*min_length=*/4);

    REQUIRE(strings.size() == 2);
    REQUIRE(strings[0].text == "AS80U");
    REQUIRE(strings[0].offset == 4);
    REQUIRE(strings[1].text == "ABC123");
    REQUIRE(strings[1].offset == 12);
}

TEST_CASE("Rom::scan_ascii respects min_length", "[rom][scan_ascii]") {
    auto const r = st::Rom::from_bytes(
        make_bytes({'h', 'i', 0x00, 'h', 'e', 'l', 'l', 'o'}));

    auto const strings = r.scan_ascii(/*min_length=*/3);
    REQUIRE(strings.size() == 1);
    REQUIRE(strings[0].text == "hello");
}
