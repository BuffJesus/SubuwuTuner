// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/dbc.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace dbc_ns = st::dbc;

TEST_CASE("parse_dbc reads VERSION and BU_ correctly", "[dbc][parse]") {
    constexpr std::string_view text = "VERSION \"1.0.0\"\n"
                                      "\n"
                                      "NS_ :\n"
                                      "\n"
                                      "BS_:\n"
                                      "\n"
                                      "BU_: ECU GW HMI\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->version == "1.0.0");
    REQUIRE(r->nodes.size() == 3);
    REQUIRE(r->nodes[0] == "ECU");
    REQUIRE(r->nodes[1] == "GW");
    REQUIRE(r->nodes[2] == "HMI");
}

TEST_CASE("parse_dbc reads a single-message single-signal database", "[dbc][parse]") {
    constexpr std::string_view text =
        "VERSION \"\"\n"
        "\n"
        "BU_: ECU\n"
        "\n"
        "BO_ 320 EngineData: 8 ECU\n"
        " SG_ EngineRPM : 0|16@1+ (0.25,0) [0|16383.75] \"rpm\" GW,HMI\n"
        "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->messages.size() == 1);
    auto const &m = r->messages[0];
    REQUIRE(m.id == 320);
    REQUIRE_FALSE(m.extended);
    REQUIRE(m.name == "EngineData");
    REQUIRE(m.length == 8);
    REQUIRE(m.sender == "ECU");
    REQUIRE(m.signals.size() == 1);

    auto const &s = m.signals[0];
    REQUIRE(s.name == "EngineRPM");
    REQUIRE(s.start_bit == 0);
    REQUIRE(s.length_bits == 16);
    REQUIRE(s.byte_order == dbc_ns::ByteOrder::Intel);
    REQUIRE(s.sign == dbc_ns::SignKind::Unsigned);
    REQUIRE(s.factor == 0.25);
    REQUIRE(s.offset == 0.0);
    REQUIRE(s.min_value == 0.0);
    REQUIRE(s.max_value == 16383.75);
    REQUIRE(s.unit == "rpm");
    REQUIRE(s.receivers == std::vector<std::string>{"GW", "HMI"});
}

TEST_CASE("parse_dbc decodes byte-order and sign markers", "[dbc][parse]") {
    constexpr std::string_view text = "BO_ 256 X: 8 Vector__XXX\n"
                                      " SG_ A : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n"
                                      " SG_ B : 8|16@0- (1,0) [-32768|32767] \"\" Vector__XXX\n"
                                      " SG_ C : 24|8@1- (1,0) [-128|127] \"\" Vector__XXX\n"
                                      " SG_ D : 32|16@0+ (1,0) [0|65535] \"\" Vector__XXX\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->messages.size() == 1);
    auto const &sigs = r->messages[0].signals;
    REQUIRE(sigs.size() == 4);
    REQUIRE(sigs[0].byte_order == dbc_ns::ByteOrder::Intel);
    REQUIRE(sigs[0].sign == dbc_ns::SignKind::Unsigned);
    REQUIRE(sigs[1].byte_order == dbc_ns::ByteOrder::Motorola);
    REQUIRE(sigs[1].sign == dbc_ns::SignKind::Signed);
    REQUIRE(sigs[2].byte_order == dbc_ns::ByteOrder::Intel);
    REQUIRE(sigs[2].sign == dbc_ns::SignKind::Signed);
    REQUIRE(sigs[3].byte_order == dbc_ns::ByteOrder::Motorola);
    REQUIRE(sigs[3].sign == dbc_ns::SignKind::Unsigned);
}

TEST_CASE("parse_dbc tags extended (29-bit) IDs via the high bit", "[dbc][parse]") {
    // 0x12345678 | 0x80000000 = 0x92345678 = 2'452'903'544 decimal.
    constexpr std::string_view text = "BO_ 2452903544 Extended: 8 Vector__XXX\n"
                                      " SG_ X : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->messages.size() == 1);
    REQUIRE(r->messages[0].extended);
    REQUIRE(r->messages[0].id == 0x12345678U);
}

TEST_CASE("parse_dbc handles signed signals with non-integer factor + negative offset",
          "[dbc][parse]") {
    constexpr std::string_view text = "BO_ 256 X: 8 Vector__XXX\n"
                                      " SG_ Temp : 0|8@1- (0.5,-40) [-40|87.5] \"C\" Vector__XXX\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    auto const &s = r->messages[0].signals[0];
    REQUIRE(s.sign == dbc_ns::SignKind::Signed);
    REQUIRE(s.factor == 0.5);
    REQUIRE(s.offset == -40.0);
    REQUIRE(s.min_value == -40.0);
    REQUIRE(s.max_value == 87.5);
    REQUIRE(s.unit == "C");
}

TEST_CASE("parse_dbc skips unknown sections without error", "[dbc][parse]") {
    constexpr std::string_view text = "VERSION \"\"\n"
                                      "BU_: ECU\n"
                                      "\n"
                                      "BO_ 320 X: 8 ECU\n"
                                      " SG_ A : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n"
                                      "\n"
                                      "CM_ \"a free-text comment\";\n"
                                      "BA_DEF_ SG_  \"GenSigStartValue\" FLOAT 0 100000000000;\n"
                                      "VAL_ 320 A 0 \"OFF\" 1 \"ON\" ;\n";
    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->messages.size() == 1);
    REQUIRE(r->messages[0].signals.size() == 1);
}

TEST_CASE("parse_dbc rejects a malformed SG_ row", "[dbc][parse][error]") {
    constexpr std::string_view text = "BO_ 100 X: 8 ECU\n"
                                      " SG_ Bad : not-a-bit-spec (1,0) [0|255] \"\" ECU\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_dbc rejects SG_ outside a BO_", "[dbc][parse][error]") {
    constexpr std::string_view text = "VERSION \"\"\n"
                                      " SG_ Floating : 0|8@1+ (1,0) [0|255] \"\" ECU\n";
    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Database::find_message + find_signal locate entries", "[dbc][lookup]") {
    constexpr std::string_view text = "BO_ 320 EngineData: 8 ECU\n"
                                      " SG_ EngineRPM : 0|16@1+ (0.25,0) [0|16383.75] \"rpm\" GW\n"
                                      " SG_ ThrottlePos : 16|8@1+ (1,0) [0|100] \"%\" GW\n"
                                      "\n";

    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->find_message(320) != nullptr);
    REQUIRE(r->find_message(321) == nullptr);
    REQUIRE(r->find_signal(320, "ThrottlePos") != nullptr);
    REQUIRE(r->find_signal(320, "ThrottlePos")->start_bit == 16);
    REQUIRE(r->find_signal(320, "NotASignal") == nullptr);
}

TEST_CASE("format_dbc + parse_dbc round-trip a multi-message database", "[dbc][round_trip]") {
    dbc_ns::Database db;
    db.version = "1.0.0";
    db.nodes = {"ECU", "GW", "HMI"};

    dbc_ns::Message m1;
    m1.id = 0x140;
    m1.name = "Lights";
    m1.length = 4;
    m1.sender = "ECU";
    m1.signals.push_back({"LeftBlinker",
                          0,
                          1,
                          dbc_ns::ByteOrder::Intel,
                          dbc_ns::SignKind::Unsigned,
                          1.0,
                          0.0,
                          0.0,
                          1.0,
                          "",
                          {"GW", "HMI"}});
    m1.signals.push_back({"RightBlinker",
                          1,
                          1,
                          dbc_ns::ByteOrder::Intel,
                          dbc_ns::SignKind::Unsigned,
                          1.0,
                          0.0,
                          0.0,
                          1.0,
                          "",
                          {"GW"}});
    db.messages.push_back(std::move(m1));

    dbc_ns::Message m2;
    m2.id = 0x12345678U;
    m2.extended = true;
    m2.name = "ExtendedDiag";
    m2.length = 8;
    m2.sender = "ECU";
    m2.signals.push_back({"Voltage",
                          0,
                          16,
                          dbc_ns::ByteOrder::Motorola,
                          dbc_ns::SignKind::Unsigned,
                          0.001,
                          0.0,
                          0.0,
                          65.535,
                          "V",
                          {"GW"}});
    db.messages.push_back(std::move(m2));

    auto const text = dbc_ns::format_dbc(db);
    auto const r = dbc_ns::parse_dbc(text);
    REQUIRE(r.has_value());
    REQUIRE(r->version == db.version);
    REQUIRE(r->nodes == db.nodes);
    REQUIRE(r->messages.size() == db.messages.size());

    for (std::size_t i = 0; i < db.messages.size(); ++i) {
        auto const &a = r->messages[i];
        auto const &b = db.messages[i];
        REQUIRE(a.id == b.id);
        REQUIRE(a.extended == b.extended);
        REQUIRE(a.name == b.name);
        REQUIRE(a.length == b.length);
        REQUIRE(a.sender == b.sender);
        REQUIRE(a.signals.size() == b.signals.size());
        for (std::size_t j = 0; j < b.signals.size(); ++j) {
            auto const &sa = a.signals[j];
            auto const &sb = b.signals[j];
            REQUIRE(sa.name == sb.name);
            REQUIRE(sa.start_bit == sb.start_bit);
            REQUIRE(sa.length_bits == sb.length_bits);
            REQUIRE(sa.byte_order == sb.byte_order);
            REQUIRE(sa.sign == sb.sign);
            REQUIRE(sa.factor == sb.factor);
            REQUIRE(sa.offset == sb.offset);
            REQUIRE(sa.unit == sb.unit);
            REQUIRE(sa.receivers == sb.receivers);
        }
    }
}

TEST_CASE("read_dbc and write_dbc round-trip via the filesystem", "[dbc][file]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     ("st_dbc_test_" + std::to_string(std::random_device{}()) + ".dbc");

    dbc_ns::Database db;
    db.version = "1.0.0";
    db.nodes = {"ECU"};
    dbc_ns::Message m;
    m.id = 100;
    m.name = "Heartbeat";
    m.length = 1;
    m.sender = "ECU";
    m.signals.push_back({"counter",
                         0,
                         8,
                         dbc_ns::ByteOrder::Intel,
                         dbc_ns::SignKind::Unsigned,
                         1.0,
                         0.0,
                         0.0,
                         255.0,
                         "",
                         {"Vector__XXX"}});
    db.messages.push_back(std::move(m));

    REQUIRE(dbc_ns::write_dbc(tmp, db).has_value());

    auto const r = dbc_ns::read_dbc(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->messages.size() == 1);
    REQUIRE(r->messages[0].name == "Heartbeat");
    REQUIRE(r->messages[0].signals[0].name == "counter");

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("read_dbc errors clearly on a missing file", "[dbc][file][error]") {
    auto const r = dbc_ns::read_dbc("/no/such/path/should/not.exist.dbc");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// decode_signal / extract_raw_bits
// ---------------------------------------------------------------------

namespace {
dbc_ns::Signal make_sig(std::size_t start_bit, std::size_t length_bits, dbc_ns::ByteOrder order,
                        dbc_ns::SignKind sign, double factor = 1.0, double offset = 0.0) {
    dbc_ns::Signal s;
    s.name = "test";
    s.start_bit = start_bit;
    s.length_bits = length_bits;
    s.byte_order = order;
    s.sign = sign;
    s.factor = factor;
    s.offset = offset;
    return s;
}
} // namespace

TEST_CASE("decode_signal extracts an Intel unsigned byte", "[dbc][decode]") {
    std::array<std::uint8_t, 8> data{0xAB, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    auto const sig = make_sig(0, 8, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Unsigned);
    REQUIRE(dbc_ns::extract_raw_bits(data, sig) == 0xABU);
    REQUIRE(dbc_ns::decode_signal(data, sig) == 171.0);
}

TEST_CASE("decode_signal extracts an Intel 16-bit value across bytes", "[dbc][decode]") {
    // bytes [LSB, MSB] = 0x34 0x12 -> raw 0x1234
    std::array<std::uint8_t, 8> data{0x34, 0x12, 0, 0, 0, 0, 0, 0};
    auto const sig = make_sig(0, 16, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Unsigned);
    REQUIRE(dbc_ns::extract_raw_bits(data, sig) == 0x1234U);
}

TEST_CASE("decode_signal extracts a Motorola 16-bit value across bytes", "[dbc][decode]") {
    // For Motorola, start_bit is the MSB. Bit 7 of byte 0 = bit index 7.
    // Length 16 walks bit 7 -> 0 (high byte of byte 0), then bit 15 -> 8
    // (high byte of byte 1). bytes [0x12 0x34] -> raw 0x1234.
    std::array<std::uint8_t, 8> data{0x12, 0x34, 0, 0, 0, 0, 0, 0};
    auto const sig = make_sig(7, 16, dbc_ns::ByteOrder::Motorola, dbc_ns::SignKind::Unsigned);
    REQUIRE(dbc_ns::extract_raw_bits(data, sig) == 0x1234U);
}

TEST_CASE("decode_signal sign-extends a negative Intel value", "[dbc][decode][sign]") {
    // 8-bit signed: 0xFF should decode as -1
    std::array<std::uint8_t, 8> data{0xFF, 0, 0, 0, 0, 0, 0, 0};
    auto const sig = make_sig(0, 8, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Signed);
    REQUIRE(dbc_ns::decode_signal(data, sig) == -1.0);
}

TEST_CASE("decode_signal sign-extends a negative Motorola value", "[dbc][decode][sign]") {
    // 16-bit signed Motorola, bytes 0xFF 0xFF -> raw 0xFFFF -> -1
    std::array<std::uint8_t, 8> data{0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    auto const sig = make_sig(7, 16, dbc_ns::ByteOrder::Motorola, dbc_ns::SignKind::Signed);
    REQUIRE(dbc_ns::decode_signal(data, sig) == -1.0);
}

TEST_CASE("decode_signal applies factor and offset", "[dbc][decode]") {
    // Raw 100, factor 0.1, offset -40 -> 100*0.1 + -40 = -30.0
    std::array<std::uint8_t, 8> data{100, 0, 0, 0, 0, 0, 0, 0};
    auto const sig =
        make_sig(0, 8, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Unsigned, 0.1, -40.0);
    REQUIRE(dbc_ns::decode_signal(data, sig) == Catch::Approx(-30.0));
}

TEST_CASE("decode_signal extracts a single bit (boolean)", "[dbc][decode]") {
    // bit 4 of byte 0
    std::array<std::uint8_t, 8> data{0x10, 0, 0, 0, 0, 0, 0, 0};
    auto const sig = make_sig(4, 1, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Unsigned);
    REQUIRE(dbc_ns::decode_signal(data, sig) == 1.0);

    data[0] = 0x00;
    REQUIRE(dbc_ns::decode_signal(data, sig) == 0.0);
}

TEST_CASE("decode_signal returns 0 when data is too short", "[dbc][decode]") {
    std::array<std::uint8_t, 2> data{0xAA, 0xBB};
    // Asking for byte 3 (start_bit 24) when only 2 bytes available.
    auto const sig = make_sig(24, 8, dbc_ns::ByteOrder::Intel, dbc_ns::SignKind::Unsigned);
    REQUIRE(dbc_ns::extract_raw_bits(data, sig) == 0U);
}
