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

TEST_CASE("read_typed handles every supported DataType", "[defs][read_typed]") {
    // Bytes laid out big-endian: 0x12, 0x34, 0x56, 0x78
    auto const rom = st::Rom::from_bytes({0x12, 0x34, 0x56, 0x78});

    REQUIRE(*st::read_typed(rom, 0, st::DataType::Uint8) == 0x12);

    auto const int8 = st::read_typed(rom, 0, st::DataType::Int8);
    REQUIRE(int8.has_value());
    REQUIRE(*int8 == 0x12);

    auto const u16be = st::read_typed(rom, 0, st::DataType::Uint16Be);
    REQUIRE(u16be.has_value());
    REQUIRE(*u16be == 0x1234);

    auto const u16le = st::read_typed(rom, 0, st::DataType::Uint16Le);
    REQUIRE(u16le.has_value());
    REQUIRE(*u16le == 0x3412);

    auto const u32be = st::read_typed(rom, 0, st::DataType::Uint32Be);
    REQUIRE(u32be.has_value());
    REQUIRE(*u32be == static_cast<double>(0x12345678U));

    auto const u32le = st::read_typed(rom, 0, st::DataType::Uint32Le);
    REQUIRE(u32le.has_value());
    REQUIRE(*u32le == static_cast<double>(0x78563412U));
}

TEST_CASE("read_typed interprets signed bytes correctly", "[defs][read_typed]") {
    // 0xFF reads as -1 when typed as Int8, 255 as Uint8.
    auto const rom = st::Rom::from_bytes({0xFF, 0xFE});

    REQUIRE(*st::read_typed(rom, 0, st::DataType::Uint8) == 255.0);
    REQUIRE(*st::read_typed(rom, 0, st::DataType::Int8) == -1.0);

    REQUIRE(*st::read_typed(rom, 0, st::DataType::Uint16Be) == 65534.0);
    REQUIRE(*st::read_typed(rom, 0, st::DataType::Int16Be) == -2.0);
}

TEST_CASE("read_typed reports OutOfRange on a short ROM", "[defs][read_typed]") {
    auto const rom = st::Rom::from_bytes({0x12});
    auto const r   = st::read_typed(rom, 0, st::DataType::Uint16Be);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("read_typed decodes IEEE 754 float32 (big-endian)",
          "[defs][read_typed][float]") {
    // 1.0f in IEEE 754 = 0x3F800000 (big-endian bytes: 3F 80 00 00).
    auto const rom = st::Rom::from_bytes({0x3F, 0x80, 0x00, 0x00});
    auto const r   = st::read_typed(rom, 0, st::DataType::Float32Be);
    REQUIRE(r.has_value());
    REQUIRE(*r == 1.0);
}

TEST_CASE("apply_scaling: linear factor and offset", "[defs][apply_scaling]") {
    st::Scaling s;
    s.formula = st::LinearScaling{.factor = 0.5, .offset = 10.0};
    REQUIRE(st::apply_scaling(100.0, s) == 60.0);
    REQUIRE(st::apply_scaling(0.0,   s) == 10.0);
    REQUIRE(st::apply_scaling(-20.0, s) == 0.0);
}

TEST_CASE("apply_scaling: piecewise interpolates between breakpoints",
          "[defs][apply_scaling]") {
    st::Scaling s;
    s.formula = st::PiecewiseScaling{
        .breakpoints = {0.0, 100.0, 200.0},
        .values      = {1.0, 2.0,   3.0},
    };
    REQUIRE(st::apply_scaling(0.0,   s) == 1.0);
    REQUIRE(st::apply_scaling(50.0,  s) == 1.5);
    REQUIRE(st::apply_scaling(150.0, s) == 2.5);
    REQUIRE(st::apply_scaling(200.0, s) == 3.0);

    // Below first breakpoint clamps to first value; above last clamps to last.
    REQUIRE(st::apply_scaling(-10.0, s) == 1.0);
    REQUIRE(st::apply_scaling(500.0, s) == 3.0);
}

TEST_CASE("Definition::read_axis_values reads and scales axis bytes",
          "[defs][read_axis_values]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[scaling]]
id        = "rpm_x100"
formula   = "linear"
factor    = 100.0
data_type = "uint16_be"

[[axis]]
id        = "rpm_axis"
type      = "static"
address   = 0
length    = 4
data_type = "uint16_be"
scaling   = "rpm_x100"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;

    // ROM contains 4 big-endian uint16s: 8, 20, 40, 60. After x100 scaling:
    // 800, 2000, 4000, 6000.
    auto const rom = st::Rom::from_bytes(
        {0x00, 0x08, 0x00, 0x14, 0x00, 0x28, 0x00, 0x3C, 0x00, 0x00});

    auto const axis = def.find_axis("rpm_axis");
    REQUIRE(axis != nullptr);

    auto const vals = def.read_axis_values(rom, *axis);
    REQUIRE(vals.has_value());
    REQUIRE(vals->size() == 4);
    REQUIRE((*vals)[0] == 800.0);
    REQUIRE((*vals)[1] == 2000.0);
    REQUIRE((*vals)[2] == 4000.0);
    REQUIRE((*vals)[3] == 6000.0);
}

TEST_CASE("Definition::read_axis_values fails OutOfRange when axis extends past ROM",
          "[defs][read_axis_values]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id         = "x"
endianness = "big"

[[axis]]
id        = "huge"
type      = "static"
address   = 100
length    = 8
data_type = "uint16_be"
)toml");
    REQUIRE(def_r.has_value());

    auto const rom  = st::Rom::from_bytes({0x00, 0x00, 0x00, 0x00});
    auto const axis = def_r->find_axis("huge");
    REQUIRE(axis != nullptr);

    auto const vals = def_r->read_axis_values(rom, *axis);
    REQUIRE_FALSE(vals.has_value());
    REQUIRE(vals.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("Definition::read_table_values reads a 1D table", "[defs][read_table_values]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 64

[[scaling]]
id        = "afr_x0_125"
formula   = "linear"
factor    = 0.125
data_type = "uint8"

[[axis]]
id        = "rpm_axis"
type      = "static"
address   = 0
length    = 4
data_type = "uint16_be"

[[table]]
id         = "tip_in_enrichment"
dimensions = 1
address    = 16
data_type  = "uint8"
scaling    = "afr_x0_125"
axis_x     = "rpm_axis"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;

    // 8 bytes of axis data (4 uint16_be) then 4 raw bytes for the table.
    // Raw values 80, 88, 96, 104; scaled (x0.125): 10.0, 11.0, 12.0, 13.0.
    auto const rom = st::Rom::from_bytes({
        0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,  // axis
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        80, 88, 96, 104,                                  // table
        0x00, 0x00, 0x00, 0x00,
    });

    auto const *table = def.find_table("tip_in_enrichment");
    REQUIRE(table != nullptr);

    auto const td = def.read_table_values(rom, *table);
    REQUIRE(td.has_value());
    REQUIRE(td->axis_x.size() == 4);
    REQUIRE(td->axis_y.empty());
    REQUIRE(td->values.size() == 1);
    REQUIRE(td->values[0].size() == 4);
    REQUIRE(td->values[0][0] == 10.0);
    REQUIRE(td->values[0][3] == 13.0);
}

TEST_CASE("Definition::read_table_values reads a 2D table (row-major, X-innermost)",
          "[defs][read_table_values]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 256

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 3
data_type = "uint8"

[[axis]]
id        = "y"
type      = "static"
address   = 4
length    = 2
data_type = "uint8"

[[table]]
id         = "grid"
dimensions = 2
address    = 16
data_type  = "uint8"
axis_x     = "x"
axis_y     = "y"
)toml");
    REQUIRE(def_r.has_value());

    auto const rom = st::Rom::from_bytes({
        1, 2, 3, 0,                              // axis x
        10, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    // axis y at offset 4
        // Table at offset 16: row 0 (y=10): 11, 12, 13
        //                     row 1 (y=20): 21, 22, 23
        11, 12, 13, 21, 22, 23, 0, 0,
    });

    auto const *t  = def_r->find_table("grid");
    auto const  td = def_r->read_table_values(rom, *t);
    REQUIRE(td.has_value());
    REQUIRE(td->axis_x == std::vector<double>{1.0, 2.0, 3.0});
    REQUIRE(td->axis_y == std::vector<double>{10.0, 20.0});
    REQUIRE(td->values.size() == 2);
    REQUIRE(td->values[0] == std::vector<double>{11.0, 12.0, 13.0});
    REQUIRE(td->values[1] == std::vector<double>{21.0, 22.0, 23.0});
}

TEST_CASE("Definition::read_table_values fails OutOfRange when grid spills past ROM",
          "[defs][read_table_values]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 4
data_type = "uint8"

[[table]]
id         = "out_of_range"
dimensions = 1
address    = 16
data_type  = "uint16_be"
axis_x     = "x"
)toml");
    REQUIRE(def_r.has_value());
    auto const rom = st::Rom::from_bytes(std::vector<std::uint8_t>(20, 0));
    auto const t   = def_r->find_table("out_of_range");
    auto const td  = def_r->read_table_values(rom, *t);
    REQUIRE_FALSE(td.has_value());
    REQUIRE(td.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("Definition::diff_table reports zero changes when ROMs are identical",
          "[defs][diff_table]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 4
data_type = "uint8"

[[table]]
id         = "t"
dimensions = 1
address    = 8
data_type  = "uint8"
axis_x     = "x"
)toml");
    REQUIRE(def_r.has_value());
    std::vector<std::uint8_t> bytes{1, 2, 3, 4, 0, 0, 0, 0, 10, 20, 30, 40};
    auto const r = st::Rom::from_bytes(bytes);

    auto const *t    = def_r->find_table("t");
    auto const  diff = def_r->diff_table(r, r, *t);
    REQUIRE(diff.has_value());
    REQUIRE(diff->total_cells == 4);
    REQUIRE(diff->cells_changed == 0);
    REQUIRE_FALSE(diff->changed());
}

TEST_CASE("Definition::diff_table reports cell-level deltas in scaled units",
          "[defs][diff_table]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[scaling]]
id        = "x0_5"
formula   = "linear"
factor    = 0.5
data_type = "uint8"

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 4
data_type = "uint8"

[[table]]
id         = "t"
dimensions = 1
address    = 8
data_type  = "uint8"
scaling    = "x0_5"
axis_x     = "x"
)toml");
    REQUIRE(def_r.has_value());

    // Stock: 10, 20, 30, 40 raw -> 5, 10, 15, 20 scaled
    auto const a = st::Rom::from_bytes(
        {1, 2, 3, 4, 0, 0, 0, 0, 10, 20, 30, 40});
    // Tuned: 14, 22, 30, 38 raw -> 7, 11, 15, 19 scaled
    auto const b = st::Rom::from_bytes(
        {1, 2, 3, 4, 0, 0, 0, 0, 14, 22, 30, 38});

    auto const *t    = def_r->find_table("t");
    auto const  diff = def_r->diff_table(a, b, *t);
    REQUIRE(diff.has_value());

    // Deltas: |7-5|=2, |11-10|=1, |15-15|=0, |19-20|=1
    REQUIRE(diff->total_cells == 4);
    REQUIRE(diff->cells_changed == 3);
    REQUIRE(diff->max_abs_delta == 2.0);
    REQUIRE(diff->mean_abs_delta == (2.0 + 1.0 + 1.0) / 3.0);
    REQUIRE(diff->changed());
}

TEST_CASE("parse_data_type round-trips known values", "[defs][types]") {
    REQUIRE(st::parse_data_type("uint8").has_value());
    REQUIRE(*st::parse_data_type("uint8") == st::DataType::Uint8);
    REQUIRE(st::to_string(*st::parse_data_type("uint16_be")) == "uint16_be");
    REQUIRE(st::byte_size(*st::parse_data_type("uint32_be")) == 4);
    REQUIRE(st::byte_size(*st::parse_data_type("float32_le")) == 4);
    REQUIRE_FALSE(st::parse_data_type("uint37").has_value());
}
