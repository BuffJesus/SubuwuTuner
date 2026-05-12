// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/defs.hpp"
#include "st/rom.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view kMinimalPack = R"toml(
[pack]
schema_version = 1
id             = "va-wrx-mt-2019"
display_name   = "Subaru WRX 2019 (VA, MT)"
platform       = "subaru.va.wrx"
transmission   = "manual"
endianness     = "big"
rom_size_bytes = 1572864
license        = "Apache-2.0"
)toml";

constexpr std::string_view kPackWithEverything = R"toml(
[pack]
schema_version = 1
id             = "va-wrx-mt-2019"
display_name   = "Subaru WRX 2019 (VA, MT)"
platform       = "subaru.va.wrx"
transmission   = "manual"
years          = [2019, 2020]
endianness     = "big"
rom_size_bytes = 1572864
authors        = ["The SubuwuTuner Authors"]
data_sources   = ["RomRaider XML (facts only)"]
license        = "Apache-2.0"

[[identification]]
name        = "VA-WRX-MT 2019 (AS80U)"
cid_address = 0x00002000
cid_length  = 8
cid_match   = "AS80U   "
ecu_part    = "22765-AS80U"

[[axis]]
id        = "rpm_16"
name      = "Engine speed"
unit      = "rpm"
type      = "static"
address   = 0x00040000
length    = 16
data_type = "uint16_be"
scaling   = "rpm_x1"

[[axis]]
id        = "load_16"
name      = "Engine load"
unit      = "g/rev"
type      = "static"
address   = 0x00040040
length    = 16
data_type = "uint16_be"
scaling   = "load_x0_001"

[[scaling]]
id        = "rpm_x1"
formula   = "linear"
factor    = 1.0
offset    = 0.0
unit      = "rpm"
min       = 0
max       = 9000
precision = 0
data_type = "uint16_be"

[[scaling]]
id        = "load_x0_001"
formula   = "linear"
factor    = 0.001
offset    = 0.0
unit      = "g/rev"
min       = 0.0
max       = 4.0
precision = 3
data_type = "uint16_be"

[[scaling]]
id          = "ego_target_lambda"
formula     = "piecewise"
data_type   = "uint8"
breakpoints = [0, 64, 128, 192, 255]
values      = [0.70, 0.85, 1.00, 1.14, 1.28]
unit        = "lambda"
precision   = 2

[[table]]
id          = "primary_open_loop_fuel"
name        = "Primary open-loop fuel"
category    = "fuel"
dimensions  = 2
address     = 0x00050000
data_type   = "uint16_be"
scaling     = "rpm_x1"
axis_x      = "rpm_16"
axis_y      = "load_16"
emissions_relevant      = false
engine_safety_critical  = true

[[pid]]
id          = "rpm"
name        = "Engine speed"
ssm_address = 0x000008
length      = 2
data_type   = "uint16_be"
scaling     = "rpm_x1"
unit        = "rpm"
default_log = true
)toml";

} // namespace

TEST_CASE("Definition rejects empty TOML", "[defs][parse]") {
    auto const d = st::Definition::from_toml_string("");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Definition rejects TOML with no [pack]", "[defs][parse]") {
    auto const d = st::Definition::from_toml_string(R"(
[[axis]]
id = "rpm_16"
)");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Definition parses a minimal pack", "[defs][parse]") {
    auto const d = st::Definition::from_toml_string(kMinimalPack);
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "va-wrx-mt-2019");
    REQUIRE(d->pack().endianness == "big");
    REQUIRE(d->pack().rom_size_bytes == 1572864);
}

TEST_CASE("Definition rejects non-big/little endianness", "[defs][parse]") {
    auto const d = st::Definition::from_toml_string(R"(
[pack]
id         = "x"
endianness = "weird"
)");
    REQUIRE_FALSE(d.has_value());
}

TEST_CASE("Definition parses identifications, axes, scalings, tables, pids",
          "[defs][parse]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());
    auto const &d = *dr;

    REQUIRE(d.identifications().size() == 1);
    REQUIRE(d.identifications()[0].cid_match == "AS80U   ");
    REQUIRE(d.identifications()[0].cid_address == 0x2000);

    REQUIRE(d.axes().size() == 2);
    REQUIRE(d.find_axis("rpm_16") != nullptr);
    REQUIRE(d.find_axis("nonexistent") == nullptr);

    REQUIRE(d.scalings().size() == 3);
    auto const *rpm = d.find_scaling("rpm_x1");
    REQUIRE(rpm != nullptr);
    REQUIRE(std::holds_alternative<st::LinearScaling>(rpm->formula));
    REQUIRE(std::get<st::LinearScaling>(rpm->formula).factor == 1.0);

    auto const *ego = d.find_scaling("ego_target_lambda");
    REQUIRE(ego != nullptr);
    REQUIRE(std::holds_alternative<st::PiecewiseScaling>(ego->formula));
    REQUIRE(std::get<st::PiecewiseScaling>(ego->formula).breakpoints.size() == 5);

    REQUIRE(d.tables().size() == 1);
    auto const *fuel = d.find_table("primary_open_loop_fuel");
    REQUIRE(fuel != nullptr);
    REQUIRE(fuel->engine_safety_critical);
    REQUIRE(fuel->axis_x.value() == "rpm_16");

    REQUIRE(d.pids().size() == 1);
    REQUIRE(d.find_pid("rpm") != nullptr);
}

TEST_CASE("Definition::validate accepts a fully consistent pack",
          "[defs][validate]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());
    auto const v = dr->validate();
    REQUIRE(v.has_value());
}

TEST_CASE("Definition::validate flags a dangling scaling reference",
          "[defs][validate]") {
    auto const dr = st::Definition::from_toml_string(R"(
[pack]
id             = "x"
rom_size_bytes = 1024
endianness     = "big"

[[axis]]
id        = "rpm"
data_type = "uint16_be"
address   = 0
length    = 4
scaling   = "missing_scaling"
)");
    REQUIRE(dr.has_value());
    auto const v = dr->validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().message().find("missing_scaling") != std::string::npos);
}

TEST_CASE("Definition::validate flags a dangling axis reference on a table",
          "[defs][validate]") {
    auto const dr = st::Definition::from_toml_string(R"(
[pack]
id             = "x"
rom_size_bytes = 1024
endianness     = "big"

[[table]]
id         = "boost"
dimensions = 2
data_type  = "uint16_be"
address    = 0
scaling    = "linear_x1"
axis_x     = "rpm"
axis_y     = "load"

[[scaling]]
id        = "linear_x1"
formula   = "linear"
data_type = "uint16_be"
)");
    REQUIRE(dr.has_value());
    auto const v = dr->validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().message().find("rpm") != std::string::npos);
    REQUIRE(v.error().message().find("load") != std::string::npos);
}

TEST_CASE("Definition::validate flags addresses past rom_size_bytes",
          "[defs][validate]") {
    auto const dr = st::Definition::from_toml_string(R"(
[pack]
id             = "x"
rom_size_bytes = 16
endianness     = "big"

[[axis]]
id        = "huge"
data_type = "uint16_be"
address   = 12
length    = 16
)");
    REQUIRE(dr.has_value());
    auto const v = dr->validate();
    REQUIRE_FALSE(v.has_value());
}

TEST_CASE("Definition::matches finds an identification by CID bytes",
          "[defs][matches]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x3000, 0xFF);
    std::string const         cid = "AS80U   ";
    for (std::size_t i = 0; i < cid.size(); ++i) {
        bytes[0x2000 + i] = static_cast<std::uint8_t>(cid[i]);
    }
    auto const rom    = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
    REQUIRE(result.has_value());
    REQUIRE(*result == "VA-WRX-MT 2019 (AS80U)");
}

TEST_CASE("Definition::matches returns nullopt on no match", "[defs][matches]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x3000, 0x00);
    auto const                rom    = st::Rom::from_bytes(std::move(bytes));
    auto const                result = dr->matches(rom);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("parse_data_type round-trips known values", "[defs][types]") {
    REQUIRE(st::parse_data_type("uint8").has_value());
    REQUIRE(*st::parse_data_type("uint8") == st::DataType::Uint8);
    REQUIRE(st::to_string(*st::parse_data_type("uint16_be")) == "uint16_be");
    REQUIRE(st::byte_size(*st::parse_data_type("uint32_be")) == 4);
    REQUIRE(st::byte_size(*st::parse_data_type("float32_le")) == 4);
    REQUIRE_FALSE(st::parse_data_type("uint37").has_value());
}
