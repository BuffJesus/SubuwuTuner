// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/can.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace can_ns = st::can;

namespace {

can_ns::Frame make_frame(std::int64_t  ts_ns,
                         can_ns::BusId bus,
                         std::uint32_t id,
                         bool          extended,
                         std::vector<std::uint8_t> const &data) {
    can_ns::Frame f;
    f.timestamp_ns = ts_ns;
    f.bus          = bus;
    f.id           = id;
    f.extended     = extended;
    f.dlc          = static_cast<std::uint8_t>(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        f.data[i] = data[i];
    }
    return f;
}

} // namespace

TEST_CASE("parse_asc handles a minimal single-frame log",
          "[can][asc][parse]") {
    constexpr std::string_view text =
        "date Wed Jan 01 00:00:00 1970\n"
        "base hex  timestamps absolute\n"
        "internal events logged\n"
        "// version 8.0.0\n"
        "Begin Triggerblock\n"
        "   0.001234 1  140             Rx   d 8 00 11 22 33 44 55 66 77\n"
        "End TriggerBlock\n";

    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    auto const &f = r->front();
    REQUIRE(f.timestamp_ns == 1'234'000);   // 0.001234 s → 1234 µs → 1234000 ns
    REQUIRE(f.bus == can_ns::BusId::Hs);     // channel 1 → bus 0
    REQUIRE(f.id == 0x140);
    REQUIRE_FALSE(f.extended);
    REQUIRE_FALSE(f.remote);
    REQUIRE(f.dlc == 8);
    std::vector<std::uint8_t> const want{0x00, 0x11, 0x22, 0x33,
                                          0x44, 0x55, 0x66, 0x77};
    for (std::uint8_t i = 0; i < 8; ++i) {
        REQUIRE(f.data[i] == want[i]);
    }
}

TEST_CASE("parse_asc preserves order across multiple frames + buses",
          "[can][asc][parse]") {
    constexpr std::string_view text =
        "Begin Triggerblock\n"
        "   0.000000 1  140             Rx   d 4 11 22 33 44\n"
        "   0.005000 2  215             Rx   d 2 AA BB\n"
        "   0.010000 1  140             Rx   d 4 11 22 33 45\n"
        "End TriggerBlock\n";

    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);

    REQUIRE((*r)[0].id == 0x140);
    REQUIRE((*r)[0].bus == can_ns::BusId::Hs);
    REQUIRE((*r)[1].id == 0x215);
    REQUIRE((*r)[1].bus == can_ns::BusId::Ms);
    REQUIRE((*r)[2].id == 0x140);
    REQUIRE((*r)[2].timestamp_ns == 10'000'000);
    REQUIRE((*r)[2].data[3] == 0x45);
}

TEST_CASE("parse_asc recognises extended (29-bit) IDs",
          "[can][asc][parse]") {
    constexpr std::string_view text =
        "   0.000100 1  12345678x       Rx   d 1 FF\n";
    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE(r->front().extended);
    REQUIRE(r->front().id == 0x12345678U);
}

TEST_CASE("parse_asc skips header / comment lines silently",
          "[can][asc][parse]") {
    // Header noise the parser must tolerate.
    constexpr std::string_view text =
        "date Wed Jan 01 00:00:00 1970\n"
        "base hex  timestamps absolute\n"
        "internal events logged\n"
        "// version 13.0.0\n"
        "Begin Triggerblock Wed Jan 01 00:00:00 1970\n"
        "\n"
        "   0.000001 1  140             Rx   d 1 42\n"
        "End TriggerBlock\n";
    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE(r->front().data[0] == 0x42);
}

TEST_CASE("parse_asc accepts zero-DLC (heartbeat) frames",
          "[can][asc][parse]") {
    constexpr std::string_view text =
        "   0.000000 1  100             Rx   d 0\n";
    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE(r->front().dlc == 0);
}

TEST_CASE("parse_asc rejects a frame line with too few data bytes",
          "[can][asc][parse][error]") {
    // DLC=4 but only 2 bytes supplied — must surface a parse error.
    constexpr std::string_view text =
        "   0.000000 1  140             Rx   d 4 11 22\n";
    auto const r = can_ns::parse_asc(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_asc rejects a frame line with a DLC > 8",
          "[can][asc][parse][error]") {
    constexpr std::string_view text =
        "   0.000000 1  140             Rx   d 9 11 22 33 44 55 66 77 88 99\n";
    auto const r = can_ns::parse_asc(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_asc handles CRLF line endings",
          "[can][asc][parse]") {
    std::string text;
    text += "Begin Triggerblock\r\n";
    text += "   0.000010 1  200             Rx   d 1 AA\r\n";
    text += "End TriggerBlock\r\n";

    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE(r->front().id == 0x200);
    REQUIRE(r->front().data[0] == 0xAA);
}

TEST_CASE("format_asc emits parseable canonical output",
          "[can][asc][format]") {
    std::vector<can_ns::Frame> const frames{
        make_frame(0,           can_ns::BusId::Hs, 0x140, false, {0x00, 0x01}),
        make_frame(1'500'000,   can_ns::BusId::Ms, 0x215, false, {0xAA}),
        make_frame(2'000'000,   can_ns::BusId::Hs, 0x12345678U, true, {0xDE, 0xAD}),
    };

    auto const text = can_ns::format_asc(frames);
    // Round-trip: re-parsing the output yields the same frames.
    auto const r = can_ns::parse_asc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        REQUIRE(can_ns::frames_equal((*r)[i], frames[i]));
    }
}

TEST_CASE("read_asc and write_asc round-trip via the filesystem",
          "[can][asc][file]") {
    auto const tmp = std::filesystem::temp_directory_path()
                    / ("st_can_test_" + std::to_string(std::random_device{}())
                       + ".asc");

    std::vector<can_ns::Frame> const frames{
        make_frame(0,         can_ns::BusId::Hs, 0x140, false,
                   {0x11, 0x22, 0x33, 0x44}),
        make_frame(500'000,   can_ns::BusId::Hs, 0x140, false,
                   {0x11, 0x22, 0x33, 0x45}),  // byte 3 flipped (1 bit)
        make_frame(1'000'000, can_ns::BusId::Ms, 0x300, false,
                   {0x00}),
    };
    REQUIRE(can_ns::write_asc(tmp, frames).has_value());

    auto const r = can_ns::read_asc(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        REQUIRE(can_ns::frames_equal((*r)[i], frames[i]));
    }

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("read_asc errors clearly when the file does not exist",
          "[can][asc][file][error]") {
    auto const r = can_ns::read_asc("/no/such/path/should/not.exist.asc");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}
