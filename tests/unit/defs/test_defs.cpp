// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/defs.hpp"
#include "st/rom.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
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

TEST_CASE("Definition parses identifications, axes, scalings, tables, pids", "[defs][parse]") {
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

TEST_CASE("Definition::validate accepts a fully consistent pack", "[defs][validate]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());
    auto const v = dr->validate();
    REQUIRE(v.has_value());
}

TEST_CASE("Definition::validate flags a dangling scaling reference", "[defs][validate]") {
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

TEST_CASE("Definition::validate flags a dangling axis reference on a table", "[defs][validate]") {
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

TEST_CASE("Definition::validate flags addresses past rom_size_bytes", "[defs][validate]") {
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

TEST_CASE("Definition::matches finds an identification by CID bytes", "[defs][matches]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x3000, 0xFF);
    std::string const cid = "AS80U   ";
    for (std::size_t i = 0; i < cid.size(); ++i) {
        bytes[0x2000 + i] = static_cast<std::uint8_t>(cid[i]);
    }
    auto const rom = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
    REQUIRE(result.has_value());
    REQUIRE(*result == "VA-WRX-MT 2019 (AS80U)");
}

TEST_CASE("Definition::matches returns nullopt on no match", "[defs][matches]") {
    auto const dr = st::Definition::from_toml_string(kPackWithEverything);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x3000, 0x00);
    auto const rom = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Definition::matches with cid_scan finds CID at variable offset",
          "[defs][matches][cid_scan]") {
    // FA-DIT WRX ROMs embed the CID in a descriptor block at a
    // per-firmware-variable offset (e.g. 0x2F7DD for LF75300E,
    // 0x38035 for LF9C000C). cid_scan=true tells the loader to scan
    // for the cid_match string anywhere in the ROM bytes instead of
    // looking at the fixed cid_address.
    constexpr std::string_view kPackScan = R"toml(
[pack]
id         = "lf75300e"
endianness = "big"

[[identification]]
name      = "LF75300E (scan)"
cid_match = "LF75300E"
cid_scan  = true
)toml";
    auto const dr = st::Definition::from_toml_string(kPackScan);
    REQUIRE(dr.has_value());

    // Plant the CID at a non-zero, non-0x2000 offset.
    constexpr std::size_t kPlantedOffset = 0x2F7DD;
    std::vector<std::uint8_t> bytes(0x40000, 0xFF);
    std::string const cid = "LF75300E";
    for (std::size_t i = 0; i < cid.size(); ++i) {
        bytes[kPlantedOffset + i] = static_cast<std::uint8_t>(cid[i]);
    }
    auto const rom = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
    REQUIRE(result.has_value());
    REQUIRE(*result == "LF75300E (scan)");
}

TEST_CASE("Definition::matches with cid_scan=true ignores cid_address",
          "[defs][matches][cid_scan]") {
    // cid_address is set to a value that points at non-matching bytes;
    // scan mode should still find the CID at a different location.
    constexpr std::string_view kPackScan = R"toml(
[pack]
id         = "scan_ignores_addr"
endianness = "big"

[[identification]]
name        = "scan-ignores-addr"
cid_address = 0x100
cid_match   = "HELLO123"
cid_scan    = true
)toml";
    auto const dr = st::Definition::from_toml_string(kPackScan);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x4000, 0xFF);
    // Plant "HELLO123" away from 0x100, somewhere deep:
    std::string const cid = "HELLO123";
    for (std::size_t i = 0; i < cid.size(); ++i) {
        bytes[0x2500 + i] = static_cast<std::uint8_t>(cid[i]);
    }
    auto const rom = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
    REQUIRE(result.has_value());
    REQUIRE(*result == "scan-ignores-addr");
}

TEST_CASE("Definition::matches with cid_scan=false uses fixed offset (regression)",
          "[defs][matches][cid_scan]") {
    // Existing behavior: without cid_scan, the loader compares
    // `cid_length` bytes at `cid_address`. Confirm a planted CID at
    // the WRONG offset doesn't match.
    constexpr std::string_view kPack = R"toml(
[pack]
id         = "fixed_offset"
endianness = "big"

[[identification]]
name        = "fixed"
cid_address = 0x2000
cid_match   = "FIXED123"
)toml";
    auto const dr = st::Definition::from_toml_string(kPack);
    REQUIRE(dr.has_value());

    std::vector<std::uint8_t> bytes(0x4000, 0xFF);
    // Plant CID at offset 0x2500 (wrong place under fixed mode):
    std::string const cid = "FIXED123";
    for (std::size_t i = 0; i < cid.size(); ++i) {
        bytes[0x2500 + i] = static_cast<std::uint8_t>(cid[i]);
    }
    auto const rom = st::Rom::from_bytes(std::move(bytes));
    auto const result = dr->matches(rom);
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
    auto const r = st::read_typed(rom, 0, st::DataType::Uint16Be);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("read_typed decodes IEEE 754 float32 (big-endian)", "[defs][read_typed][float]") {
    // 1.0f in IEEE 754 = 0x3F800000 (big-endian bytes: 3F 80 00 00).
    auto const rom = st::Rom::from_bytes({0x3F, 0x80, 0x00, 0x00});
    auto const r = st::read_typed(rom, 0, st::DataType::Float32Be);
    REQUIRE(r.has_value());
    REQUIRE(*r == 1.0);
}

TEST_CASE("apply_scaling: linear factor and offset", "[defs][apply_scaling]") {
    st::Scaling s;
    s.formula = st::LinearScaling{.factor = 0.5, .offset = 10.0};
    REQUIRE(st::apply_scaling(100.0, s) == 60.0);
    REQUIRE(st::apply_scaling(0.0, s) == 10.0);
    REQUIRE(st::apply_scaling(-20.0, s) == 0.0);
}

TEST_CASE("apply_scaling: piecewise interpolates between breakpoints", "[defs][apply_scaling]") {
    st::Scaling s;
    s.formula = st::PiecewiseScaling{
        .breakpoints = {0.0, 100.0, 200.0},
        .values = {1.0, 2.0, 3.0},
    };
    REQUIRE(st::apply_scaling(0.0, s) == 1.0);
    REQUIRE(st::apply_scaling(50.0, s) == 1.5);
    REQUIRE(st::apply_scaling(150.0, s) == 2.5);
    REQUIRE(st::apply_scaling(200.0, s) == 3.0);

    // Below first breakpoint clamps to first value; above last clamps to last.
    REQUIRE(st::apply_scaling(-10.0, s) == 1.0);
    REQUIRE(st::apply_scaling(500.0, s) == 3.0);
}

TEST_CASE("apply_scaling: subaru AFR enrichment uses 14.7/(1+raw*k)",
          "[defs][apply_scaling][afr]") {
    st::Scaling s;
    s.formula = st::SubaruAfrEnrichment{.numerator = 14.7, .k = 0.0078125};
    // raw 0 → stoich. raw 47 → 14.7/(1+47*0.0078125) ≈ 10.74. raw 255 → ≈4.92.
    REQUIRE(st::apply_scaling(0.0, s) == Catch::Approx(14.7));
    REQUIRE(st::apply_scaling(47.0, s) == Catch::Approx(10.7456).margin(0.01));
    REQUIRE(st::apply_scaling(128.0, s) == Catch::Approx(7.35).margin(0.01));
    REQUIRE(st::apply_scaling(255.0, s) == Catch::Approx(4.917).margin(0.01));
}

TEST_CASE("apply_scaling: subaru AFR enrichment safe at the singularity",
          "[defs][apply_scaling][afr]") {
    // 1 + raw*k = 0 → raw = -1/k. The function returns 0 rather than ±∞;
    // not physically reachable for uint8 raw but defensive on float inputs.
    st::Scaling s;
    s.formula = st::SubaruAfrEnrichment{.numerator = 14.7, .k = 0.0078125};
    REQUIRE(st::apply_scaling(-128.0, s) == 0.0);
}

TEST_CASE("invert_scaling: subaru AFR enrichment round-trips",
          "[defs][invert_scaling][afr]") {
    st::Scaling s;
    s.formula = st::SubaruAfrEnrichment{.numerator = 14.7, .k = 0.0078125};
    for (double raw : {0.0, 47.0, 128.0, 200.0, 255.0}) {
        double const afr = st::apply_scaling(raw, s);
        auto const inverted = st::invert_scaling(afr, s);
        REQUIRE(inverted.has_value());
        REQUIRE(*inverted == Catch::Approx(raw).margin(1e-9));
    }
}

TEST_CASE("invert_scaling: subaru AFR enrichment rejects degenerate value=0",
          "[defs][invert_scaling][afr]") {
    st::Scaling s;
    s.formula = st::SubaruAfrEnrichment{.numerator = 14.7, .k = 0.0078125};
    auto const r = st::invert_scaling(0.0, s);
    REQUIRE(!r.has_value());  // value=0 maps to raw=±∞
}

TEST_CASE("scaling loader: subaru_afr_enrichment formula parsed", "[defs][parse]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[scaling]]
id        = "afr"
formula   = "subaru_afr_enrichment"
numerator = 14.7
k         = 0.0078125
unit      = "AFR"
data_type = "uint8"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;
    auto const *afr = def.find_scaling("afr");
    REQUIRE(afr != nullptr);
    REQUIRE(std::holds_alternative<st::SubaruAfrEnrichment>(afr->formula));
    auto const &f = std::get<st::SubaruAfrEnrichment>(afr->formula);
    REQUIRE(f.numerator == 14.7);
    REQUIRE(f.k == 0.0078125);
}

TEST_CASE("apply_scaling: inverse_divide computes numerator/raw",
          "[defs][apply_scaling][inverse_divide]") {
    st::Scaling s;
    s.formula = st::InverseDivideScaling{.numerator = 2707090.0};
    // The Subaru injector-flow-scaling case: raw float 4916 → 550.7 cc/min.
    REQUIRE(st::apply_scaling(4916.0, s) == Catch::Approx(550.67).margin(0.01));
    // Other anchor values for sanity.
    REQUIRE(st::apply_scaling(2707.09, s) == Catch::Approx(1000.0).margin(0.01));
    REQUIRE(st::apply_scaling(1000.0, s) == Catch::Approx(2707.09).margin(0.01));
}

TEST_CASE("apply_scaling: inverse_divide safe at raw==0",
          "[defs][apply_scaling][inverse_divide]") {
    st::Scaling s;
    s.formula = st::InverseDivideScaling{.numerator = 2707090.0};
    REQUIRE(st::apply_scaling(0.0, s) == 0.0);
}

TEST_CASE("invert_scaling: inverse_divide round-trips",
          "[defs][invert_scaling][inverse_divide]") {
    st::Scaling s;
    s.formula = st::InverseDivideScaling{.numerator = 2707090.0};
    for (double raw : {1.0, 100.0, 4916.0, 100000.0}) {
        double const eng = st::apply_scaling(raw, s);
        auto const inverted = st::invert_scaling(eng, s);
        REQUIRE(inverted.has_value());
        REQUIRE(*inverted == Catch::Approx(raw).margin(1e-6));
    }
}

TEST_CASE("invert_scaling: inverse_divide rejects engineering=0",
          "[defs][invert_scaling][inverse_divide]") {
    st::Scaling s;
    s.formula = st::InverseDivideScaling{.numerator = 2707090.0};
    REQUIRE(!st::invert_scaling(0.0, s).has_value());
}

TEST_CASE("scaling loader: inverse_divide formula parsed",
          "[defs][parse][inverse_divide]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[scaling]]
id        = "injector_flow"
formula   = "inverse_divide"
numerator = 2707090.0
unit      = "cc/min"
data_type = "float32_be"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;
    auto const *s = def.find_scaling("injector_flow");
    REQUIRE(s != nullptr);
    REQUIRE(std::holds_alternative<st::InverseDivideScaling>(s->formula));
    REQUIRE(std::get<st::InverseDivideScaling>(s->formula).numerator == 2707090.0);
}


TEST_CASE("scaling loader: subaru_afr_enrichment defaults",
          "[defs][parse][afr]") {
    // Constants omitted → defaults 14.7 / 0.0078125 (the canonical EJ form).
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 32

[[scaling]]
id        = "afr_default"
formula   = "subaru_afr_enrichment"
data_type = "uint8"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;
    auto const *afr = def.find_scaling("afr_default");
    REQUIRE(afr != nullptr);
    auto const &f = std::get<st::SubaruAfrEnrichment>(afr->formula);
    REQUIRE(f.numerator == 14.7);
    REQUIRE(f.k == 0.0078125);
}

TEST_CASE("Definition::read_axis_values reads and scales axis bytes", "[defs][read_axis_values]") {
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
    auto const rom =
        st::Rom::from_bytes({0x00, 0x08, 0x00, 0x14, 0x00, 0x28, 0x00, 0x3C, 0x00, 0x00});

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

    auto const rom = st::Rom::from_bytes({0x00, 0x00, 0x00, 0x00});
    auto const axis = def_r->find_axis("huge");
    REQUIRE(axis != nullptr);

    auto const vals = def_r->read_axis_values(rom, *axis);
    REQUIRE_FALSE(vals.has_value());
    REQUIRE(vals.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("Definition accepts dimensions=0 for scalar tables (no axes required)",
          "[defs][scalar]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 64

[[scaling]]
id        = "raw_u8"
formula   = "linear"
factor    = 1.0
data_type = "uint8"

[[table]]
id         = "rev_limit_slot"
dimensions = 0
address    = 16
data_type  = "uint8"
scaling    = "raw_u8"
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;

    auto const rom = st::Rom::from_bytes({
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x42, // scalar at offset 16
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });

    auto const *table = def.find_table("rev_limit_slot");
    REQUIRE(table != nullptr);
    REQUIRE(table->dimensions == 0);

    auto const td = def.read_table_values(rom, *table);
    REQUIRE(td.has_value());
    REQUIRE(td->axis_x.empty());
    REQUIRE(td->axis_y.empty());
    REQUIRE(td->axis_z.empty());
    REQUIRE(td->values.size() == 1);
    REQUIRE(td->values[0].size() == 1);
    REQUIRE(td->values[0][0] == 0x42);
}

TEST_CASE("Definition rejects out-of-range dimensions", "[defs][scalar]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 64

[[table]]
id         = "bogus"
dimensions = 4
address    = 0
data_type  = "uint8"
)toml");
    REQUIRE_FALSE(def_r.has_value());
}

TEST_CASE("Definition parses [[switch]] records with bit-position validation", "[defs][switch]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 0

[[switch]]
id          = "s1"
name        = "Neutral Switch"
ssm_address = 0x000062
bit         = 7
default_log = false

[[switch]]
id          = "s2"
name        = "Idle Switch"
ssm_address = 0x000062
bit         = 6
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;
    REQUIRE(def.switches().size() == 2);
    REQUIRE(def.switches()[0].id == "s1");
    REQUIRE(def.switches()[0].bit == 7);
    REQUIRE(def.switches()[0].ssm_address == 0x62);
    REQUIRE(def.find_switch("s2") != nullptr);
    REQUIRE(def.find_switch("s2")->name == "Idle Switch");
    REQUIRE(def.find_switch("nope") == nullptr);
}

TEST_CASE("Definition rejects switch with out-of-range bit", "[defs][switch]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"

[[switch]]
id          = "bogus"
ssm_address = 0x100
bit         = 8
)toml");
    REQUIRE_FALSE(def_r.has_value());
}

TEST_CASE("Definition parses [[dtc_bitmap]] + [[dtc]] records", "[defs][dtc]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 0x00200000

[[dtc_bitmap]]
id           = "primary_dtc_enable"
name         = "Primary DTC enable bitmap"
address      = 0x00100000
length_bytes = 32
endianness   = "big"

[[dtc]]
code               = "P0401"
name               = "Insufficient EGR Flow Detected"
bitmap_id          = "primary_dtc_enable"
byte_offset        = 12
bit                = 3
emissions_relevant = true

[[dtc]]
code               = "P0420"
name               = "Catalyst System Efficiency Below Threshold (Bank 1)"
bitmap_id          = "primary_dtc_enable"
byte_offset        = 14
bit                = 0
emissions_relevant = true
)toml");
    REQUIRE(def_r.has_value());
    auto const &def = *def_r;
    REQUIRE(def.dtc_bitmaps().size() == 1);
    REQUIRE(def.dtc_bitmaps()[0].id == "primary_dtc_enable");
    REQUIRE(def.dtc_bitmaps()[0].length_bytes == 32);
    REQUIRE(def.dtc_bitmaps()[0].address == 0x00100000);
    REQUIRE(def.find_dtc_bitmap("primary_dtc_enable") != nullptr);
    REQUIRE(def.find_dtc_bitmap("nope") == nullptr);

    REQUIRE(def.dtcs().size() == 2);
    auto const *p0401 = def.find_dtc("P0401");
    REQUIRE(p0401 != nullptr);
    REQUIRE(p0401->byte_offset == 12);
    REQUIRE(p0401->bit == 3);
    REQUIRE(p0401->emissions_relevant);
    REQUIRE(def.find_dtc("P9999") == nullptr);
}

TEST_CASE("Definition rejects dtc with out-of-range bit", "[defs][dtc]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id = "x"

[[dtc_bitmap]]
id           = "bm"
address      = 0x100
length_bytes = 4

[[dtc]]
code        = "P0001"
bitmap_id   = "bm"
byte_offset = 0
bit         = 8
)toml");
    REQUIRE_FALSE(def_r.has_value());
}

TEST_CASE("Definition rejects dtc_bitmap with length_bytes = 0", "[defs][dtc]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id = "x"

[[dtc_bitmap]]
id           = "empty"
address      = 0x100
length_bytes = 0
)toml");
    REQUIRE_FALSE(def_r.has_value());
}

TEST_CASE("Definition::validate flags dtc with unknown bitmap", "[defs][dtc][validate]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
rom_size_bytes = 0x1000

[[dtc_bitmap]]
id           = "known"
address      = 0x100
length_bytes = 4

[[dtc]]
code        = "P0001"
bitmap_id   = "ghost"
byte_offset = 0
bit         = 0
)toml");
    REQUIRE(def_r.has_value());
    auto const v = def_r->validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(std::string{v.error().message()}.find("unknown bitmap") != std::string::npos);
}

TEST_CASE("Definition::validate flags dtc byte_offset past bitmap end", "[defs][dtc][validate]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
rom_size_bytes = 0x1000

[[dtc_bitmap]]
id           = "bm"
address      = 0x100
length_bytes = 4

[[dtc]]
code        = "P0001"
bitmap_id   = "bm"
byte_offset = 4
bit         = 0
)toml");
    REQUIRE(def_r.has_value());
    auto const v = def_r->validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(std::string{v.error().message()}.find("outside bitmap") != std::string::npos);
}

TEST_CASE("set_dtc_enabled toggles the correct bit", "[defs][dtc][bit]") {
    auto rom = st::Rom::from_bytes(std::vector<std::uint8_t>(32, 0xFFU));

    st::DtcBitmap const bm{"bm", "Bitmap", /*address*/ 0, /*length_bytes*/ 32, "big"};
    st::Dtc const d{"P0401",
                    "EGR Flow Insufficient",
                    "bm",
                    /*byte_offset*/ 4,
                    /*bit*/ 3,
                    /*emissions_relevant*/ true};

    // Starts enabled (all-FF bitmap is the factory default).
    auto en1 = st::is_dtc_enabled(rom, bm, d);
    REQUIRE(en1.has_value());
    REQUIRE(*en1);

    // Disable: bit 3 of byte 4 clears.
    auto disable = st::set_dtc_enabled(rom, bm, d, false);
    REQUIRE(disable.has_value());
    REQUIRE(disable->before == 0xFFU);
    REQUIRE(disable->after == 0xF7U); // 0b1111_0111
    auto en2 = st::is_dtc_enabled(rom, bm, d);
    REQUIRE(en2.has_value());
    REQUIRE_FALSE(*en2);

    // Re-enable: bit 3 sets back.
    auto enable = st::set_dtc_enabled(rom, bm, d, true);
    REQUIRE(enable.has_value());
    REQUIRE(enable->before == 0xF7U);
    REQUIRE(enable->after == 0xFFU);
}

TEST_CASE("set_dtc_enabled rejects out-of-range byte_offset", "[defs][dtc][bit]") {
    auto rom = st::Rom::from_bytes(std::vector<std::uint8_t>(8, 0xFFU));
    st::DtcBitmap const bm{"bm", "", 0, 4, "big"};
    st::Dtc const d{"P0001", "", "bm", /*byte_offset*/ 4, /*bit*/ 0, false};
    auto r = st::set_dtc_enabled(rom, bm, d, false);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("set_dtc_enabled rejects address past end of ROM", "[defs][dtc][bit]") {
    auto rom = st::Rom::from_bytes(std::vector<std::uint8_t>(8, 0xFFU));
    st::DtcBitmap const bm{"bm", "", /*address*/ 0x100, /*length_bytes*/ 4, "big"};
    st::Dtc const d{"P0001", "", "bm", 0, 0, false};
    auto r = st::set_dtc_enabled(rom, bm, d, false);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Definition::validate flags dtc_bitmap past rom_size_bytes", "[defs][dtc][validate]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
rom_size_bytes = 0x100

[[dtc_bitmap]]
id           = "huge"
address      = 0xF0
length_bytes = 32
)toml");
    REQUIRE(def_r.has_value());
    auto const v = def_r->validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(std::string{v.error().message()}.find("past rom_size_bytes") != std::string::npos);
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
        0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,                  // axis
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 80, 88, 96, 104, // table
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
        1,
        2,
        3,
        0, // axis x
        10,
        20,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0, // axis y at offset 4
        // Table at offset 16: row 0 (y=10): 11, 12, 13
        //                     row 1 (y=20): 21, 22, 23
        11,
        12,
        13,
        21,
        22,
        23,
        0,
        0,
    });

    auto const *t = def_r->find_table("grid");
    auto const td = def_r->read_table_values(rom, *t);
    REQUIRE(td.has_value());
    REQUIRE(td->axis_x == std::vector<double>{1.0, 2.0, 3.0});
    REQUIRE(td->axis_y == std::vector<double>{10.0, 20.0});
    REQUIRE(td->values.size() == 2);
    REQUIRE(td->values[0] == std::vector<double>{11.0, 12.0, 13.0});
    REQUIRE(td->values[1] == std::vector<double>{21.0, 22.0, 23.0});
}

TEST_CASE("Definition::read_table_values reads a 3D table (z slices, row-major)",
          "[defs][read_table_values][3d]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 512

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 2
data_type = "uint8"

[[axis]]
id        = "y"
type      = "static"
address   = 4
length    = 2
data_type = "uint8"

[[axis]]
id        = "z"
type      = "static"
address   = 8
length    = 2
data_type = "uint8"

[[table]]
id         = "cube"
dimensions = 3
address    = 16
data_type  = "uint8"
axis_x     = "x"
axis_y     = "y"
axis_z     = "z"
)toml");
    REQUIRE(def_r.has_value());

    // x = [1, 2]      at 0..1
    // y = [10, 20]    at 4..5
    // z = [100, 200]  at 8..9
    // table at 16: z0 then z1, each is a 2-row x 2-col uint8 grid.
    //   z0 (slice 0): 11, 12, 13, 14
    //   z1 (slice 1): 21, 22, 23, 24
    auto const rom = st::Rom::from_bytes({
        1,   2,   0,  0,                  // x at 0..3
        10,  20,  0,  0,                  // y at 4..7
        100, 200, 0,  0,  0,  0,  0,  0,  // z at 8..15
        11,  12,  13, 14, 21, 22, 23, 24, // table at 16..23
    });

    auto const *t = def_r->find_table("cube");
    REQUIRE(t != nullptr);
    auto const td = def_r->read_table_values(rom, *t);
    REQUIRE(td.has_value());
    REQUIRE(td->axis_x == std::vector<double>{1.0, 2.0});
    REQUIRE(td->axis_y == std::vector<double>{10.0, 20.0});
    REQUIRE(td->axis_z == std::vector<double>{100.0, 200.0});
    REQUIRE(td->slices.size() == 2);
    REQUIRE(td->slices[0] == std::vector<std::vector<double>>{{11, 12}, {13, 14}});
    REQUIRE(td->slices[1] == std::vector<std::vector<double>>{{21, 22}, {23, 24}});
    // For 3D, the 2D `values` field is left empty.
    REQUIRE(td->values.empty());
}

TEST_CASE("Definition::write_table_values round-trips a 3D edit",
          "[defs][write_table_values][3d]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 64

[[axis]]
id        = "x"
type      = "static"
address   = 0
length    = 2
data_type = "uint8"

[[axis]]
id        = "y"
type      = "static"
address   = 4
length    = 2
data_type = "uint8"

[[axis]]
id        = "z"
type      = "static"
address   = 8
length    = 2
data_type = "uint8"

[[table]]
id         = "cube"
dimensions = 3
address    = 16
data_type  = "uint8"
axis_x     = "x"
axis_y     = "y"
axis_z     = "z"
)toml");
    REQUIRE(def_r.has_value());

    std::vector<std::uint8_t> bytes{
        1, 2, 0, 0, 10, 20, 0, 0, 100, 200, 0, 0, 0, 0, 0, 0, 11, 12, 13, 14, 21, 22, 23, 24,
        0, 0, 0, 0, 0,  0,  0, 0, 0,   0,   0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,
    };
    auto rom = st::Rom::from_bytes(std::move(bytes));

    auto const *t = def_r->find_table("cube");
    auto td = *def_r->read_table_values(rom, *t);
    td.slices[1][0][1] = 99.0; // bump one cell in z=1 slice

    REQUIRE(def_r->write_table_values(rom, *t, td).has_value());

    auto const re_read = def_r->read_table_values(rom, *t);
    REQUIRE(re_read.has_value());
    REQUIRE(re_read->slices[1][0][1] == 99.0);
    REQUIRE(re_read->slices[0] == std::vector<std::vector<double>>{{11, 12}, {13, 14}});
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
    auto const t = def_r->find_table("out_of_range");
    auto const td = def_r->read_table_values(rom, *t);
    REQUIRE_FALSE(td.has_value());
    REQUIRE(td.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("invert_scaling reverses linear", "[defs][invert_scaling]") {
    st::Scaling s;
    s.formula = st::LinearScaling{.factor = 0.5, .offset = 10.0};
    auto const r = st::invert_scaling(60.0, s); // 60 = raw*0.5 + 10 -> raw = 100
    REQUIRE(r.has_value());
    REQUIRE(*r == 100.0);
}

TEST_CASE("invert_scaling refuses factor=0", "[defs][invert_scaling]") {
    st::Scaling s;
    s.formula = st::LinearScaling{.factor = 0.0, .offset = 0.0};
    auto const r = st::invert_scaling(1.0, s);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("invert_scaling reverses ascending piecewise", "[defs][invert_scaling]") {
    st::Scaling s;
    s.formula = st::PiecewiseScaling{
        .breakpoints = {0.0, 100.0, 200.0},
        .values = {1.0, 2.0, 3.0},
    };
    REQUIRE(*st::invert_scaling(1.0, s) == 0.0);
    REQUIRE(*st::invert_scaling(1.5, s) == 50.0);
    REQUIRE(*st::invert_scaling(2.5, s) == 150.0);
    REQUIRE(*st::invert_scaling(3.0, s) == 200.0);
}

TEST_CASE("write_typed clamps and rounds correctly", "[defs][write_typed]") {
    std::vector<std::uint8_t> bytes(8, 0);
    auto rom = st::Rom::from_bytes(std::move(bytes));

    REQUIRE(st::write_typed(rom, 0, st::DataType::Uint8, 14.6).has_value());
    REQUIRE(rom.data()[0] == 15); // rounded

    REQUIRE(st::write_typed(rom, 1, st::DataType::Uint8, 999.0).has_value());
    REQUIRE(rom.data()[1] == 255); // clamped

    REQUIRE(st::write_typed(rom, 2, st::DataType::Uint8, -5.0).has_value());
    REQUIRE(rom.data()[2] == 0); // clamped

    REQUIRE(st::write_typed(rom, 4, st::DataType::Uint16Be, 0x1234).has_value());
    REQUIRE(rom.data()[4] == 0x12);
    REQUIRE(rom.data()[5] == 0x34);
}

TEST_CASE("write_typed round-trips Int16 negatives", "[defs][write_typed]") {
    std::vector<std::uint8_t> bytes(4, 0);
    auto rom = st::Rom::from_bytes(std::move(bytes));

    REQUIRE(st::write_typed(rom, 0, st::DataType::Int16Be, -1.0).has_value());
    auto const r = st::read_typed(rom, 0, st::DataType::Int16Be);
    REQUIRE(r.has_value());
    REQUIRE(*r == -1.0);
}

TEST_CASE("write_typed round-trips Float32", "[defs][write_typed][float]") {
    std::vector<std::uint8_t> bytes(8, 0);
    auto rom = st::Rom::from_bytes(std::move(bytes));

    REQUIRE(st::write_typed(rom, 0, st::DataType::Float32Be, 3.14159f).has_value());
    auto const r = st::read_typed(rom, 0, st::DataType::Float32Be);
    REQUIRE(r.has_value());
    REQUIRE(static_cast<float>(*r) == 3.14159f);
}

TEST_CASE("Definition::write_table_values round-trips edits through ROM bytes",
          "[defs][write_table_values][round_trip]") {
    auto const def_r = st::Definition::from_toml_string(R"toml(
[pack]
id             = "x"
endianness     = "big"
rom_size_bytes = 64

[[scaling]]
id        = "x0_5"
formula   = "linear"
factor    = 0.5
data_type = "uint8"

[[axis]]
id        = "rpm"
type      = "static"
address   = 0
length    = 4
data_type = "uint8"

[[axis]]
id        = "load"
type      = "static"
address   = 4
length    = 2
data_type = "uint8"

[[table]]
id         = "fuel"
dimensions = 2
address    = 8
data_type  = "uint8"
scaling    = "x0_5"
axis_x     = "rpm"
axis_y     = "load"
)toml");
    REQUIRE(def_r.has_value());

    std::vector<std::uint8_t> bytes{
        1,  2,  3, 4,                 // rpm axis (raw)
        10, 20, 0, 0,                 // load axis (2 values then padding)
        2,  4,  6, 8, 10, 12, 14, 16, // 2x4 fuel table raw -> 1,2,3,4 / 5,6,7,8 scaled
    };
    auto rom = st::Rom::from_bytes(std::move(bytes));
    auto const *fuel = def_r->find_table("fuel");
    auto const read1 = def_r->read_table_values(rom, *fuel);
    REQUIRE(read1.has_value());
    REQUIRE(read1->values ==
            std::vector<std::vector<double>>{{1.0, 2.0, 3.0, 4.0}, {5.0, 6.0, 7.0, 8.0}});

    // Edit: bump everything by 1 (engineering units).
    auto td = *read1;
    for (auto &row : td.values) {
        for (auto &v : row)
            v += 1.0;
    }

    REQUIRE(def_r->write_table_values(rom, *fuel, td).has_value());

    auto const read2 = def_r->read_table_values(rom, *fuel);
    REQUIRE(read2.has_value());
    REQUIRE(read2->values ==
            std::vector<std::vector<double>>{{2.0, 3.0, 4.0, 5.0}, {6.0, 7.0, 8.0, 9.0}});

    // Underlying raw bytes should be the edited values * 2.
    REQUIRE(rom.data()[8] == 4);   // raw for 2.0
    REQUIRE(rom.data()[15] == 18); // raw for 9.0
}

TEST_CASE("Definition::write_table_values rejects mismatched grid shapes",
          "[defs][write_table_values]") {
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

    std::vector<std::uint8_t> bytes(32, 0);
    auto rom = st::Rom::from_bytes(std::move(bytes));

    st::Definition::TableData wrong;
    wrong.values = {{1, 2, 3}}; // 3 cols, expected 4
    auto const r = def_r->write_table_values(rom, *def_r->find_table("t"), wrong);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
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

    auto const *t = def_r->find_table("t");
    auto const diff = def_r->diff_table(r, r, *t);
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
    auto const a = st::Rom::from_bytes({1, 2, 3, 4, 0, 0, 0, 0, 10, 20, 30, 40});
    // Tuned: 14, 22, 30, 38 raw -> 7, 11, 15, 19 scaled
    auto const b = st::Rom::from_bytes({1, 2, 3, 4, 0, 0, 0, 0, 14, 22, 30, 38});

    auto const *t = def_r->find_table("t");
    auto const diff = def_r->diff_table(a, b, *t);
    REQUIRE(diff.has_value());

    // Deltas: |7-5|=2, |11-10|=1, |15-15|=0, |19-20|=1
    REQUIRE(diff->total_cells == 4);
    REQUIRE(diff->cells_changed == 3);
    REQUIRE(diff->max_abs_delta == 2.0);
    REQUIRE(diff->mean_abs_delta == (2.0 + 1.0 + 1.0) / 3.0);
    REQUIRE(diff->changed());
}

// ----- Definition::from_directory ----------------------------------------

namespace {

// RAII helper for a unique temp directory.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("st_defs_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(TempDir const &) = delete;
    TempDir &operator=(TempDir const &) = delete;
};

void write_text(std::filesystem::path const &p, std::string_view contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p};
    out << contents;
}

} // namespace

TEST_CASE("Definition::from_directory merges pack.toml + sibling TOMLs", "[defs][from_directory]") {
    TempDir td;

    write_text(td.path / "pack.toml", R"toml(
[pack]
schema_version = 1
id             = "split-pack"
endianness     = "big"
rom_size_bytes = 65536
license        = "Apache-2.0"
)toml");

    write_text(td.path / "axes.toml", R"toml(
[[axis]]
id        = "rpm_axis"
type      = "static"
address   = 0
length    = 16
data_type = "uint16_be"
)toml");

    write_text(td.path / "scalings.toml", R"toml(
[[scaling]]
id        = "rpm_x1"
formula   = "linear"
factor    = 1.0
data_type = "uint16_be"
unit      = "rpm"

[[scaling]]
id        = "afr_x0_1"
formula   = "linear"
factor    = 0.1
data_type = "uint8"
unit      = "AFR"
)toml");

    write_text(td.path / "tables" / "fuel.toml", R"toml(
[[table]]
id         = "primary_fuel"
dimensions = 1
address    = 1024
data_type  = "uint8"
scaling    = "afr_x0_1"
axis_x     = "rpm_axis"
)toml");

    write_text(td.path / "tables" / "boost.toml", R"toml(
[[table]]
id         = "boost_target"
dimensions = 1
address    = 2048
data_type  = "uint16_be"
scaling    = "rpm_x1"
axis_x     = "rpm_axis"
)toml");

    auto const d = st::Definition::from_directory(td.path);
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "split-pack");
    REQUIRE(d->axes().size() == 1);
    REQUIRE(d->scalings().size() == 2);
    REQUIRE(d->tables().size() == 2);
    REQUIRE(d->find_table("primary_fuel") != nullptr);
    REQUIRE(d->find_table("boost_target") != nullptr);
}

TEST_CASE("Definition::from_directory fails without pack.toml", "[defs][from_directory]") {
    TempDir td;
    write_text(td.path / "axes.toml", "[[axis]]\nid = \"rpm\"\ndata_type = \"uint8\"\n");
    auto const d = st::Definition::from_directory(td.path);
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("Definition::from_directory ignores [pack] in non-manifest files",
          "[defs][from_directory]") {
    TempDir td;
    write_text(td.path / "pack.toml", R"toml(
[pack]
id         = "real-id"
endianness = "big"
)toml");
    // This [pack] should be silently ignored — id stays "real-id".
    write_text(td.path / "stray.toml", R"toml(
[pack]
id = "WRONG"

[[scaling]]
id        = "x"
formula   = "linear"
factor    = 1.0
data_type = "uint8"
)toml");

    auto const d = st::Definition::from_directory(td.path);
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "real-id");
    REQUIRE(d->scalings().size() == 1);
}

TEST_CASE("Definition::from_file dispatches to from_directory when given a directory",
          "[defs][from_file][from_directory]") {
    TempDir td;
    write_text(td.path / "pack.toml", R"toml(
[pack]
id         = "auto-dispatch"
endianness = "big"
)toml");
    auto const d = st::Definition::from_file(td.path);
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "auto-dispatch");
}

// ----- includes (single-file pack imports records from a sibling) --------

TEST_CASE("from_file resolves `includes` and merges fragment records", "[defs][includes]") {
    TempDir td;
    write_text(td.path / "frag.toml", R"toml(
[pack]
id         = "subaru-ssm-ecuparams-x"
endianness = "big"

[[scaling]]
id        = "raw_x"
formula   = "linear"
factor    = 1.0
data_type = "uint8"

[[pid]]
id          = "e1"
name        = "IAM*"
ssm_address = 0x20118
length      = 1
data_type   = "uint8"
scaling     = "raw_x"
unit        = "raw"
)toml");
    write_text(td.path / "main.toml", R"toml(
[pack]
id         = "test-pack-001"
endianness = "big"
includes   = ["frag.toml"]
)toml");
    auto const d = st::Definition::from_file(td.path / "main.toml");
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "test-pack-001");
    // Fragment's [pack] is ignored — parent id wins.
    REQUIRE(d->pids().size() == 1);
    REQUIRE(d->pids()[0].id == "e1");
    REQUIRE(d->pids()[0].ssm_address == 0x20118);
    REQUIRE(d->scalings().size() == 1);
    REQUIRE(d->find_pid("e1") != nullptr);
}

TEST_CASE("from_file resolves a chain of includes depth-first", "[defs][includes]") {
    TempDir td;
    write_text(td.path / "leaf.toml", R"toml(
[[pid]]
id          = "leaf_pid"
ssm_address = 0x00
length      = 1
data_type   = "uint8"
)toml");
    write_text(td.path / "mid.toml", R"toml(
[pack]
id         = "mid"
endianness = "big"
includes   = ["leaf.toml"]

[[pid]]
id          = "mid_pid"
ssm_address = 0x01
length      = 1
data_type   = "uint8"
)toml");
    write_text(td.path / "main.toml", R"toml(
[pack]
id         = "root"
endianness = "big"
includes   = ["mid.toml"]
)toml");
    auto const d = st::Definition::from_file(td.path / "main.toml");
    REQUIRE(d.has_value());
    REQUIRE(d->pids().size() == 2);
    REQUIRE(d->find_pid("mid_pid") != nullptr);
    REQUIRE(d->find_pid("leaf_pid") != nullptr);
}

TEST_CASE("from_file `includes` reports a missing fragment cleanly", "[defs][includes]") {
    TempDir td;
    write_text(td.path / "main.toml", R"toml(
[pack]
id         = "x"
endianness = "big"
includes   = ["does_not_exist.toml"]
)toml");
    auto const d = st::Definition::from_file(td.path / "main.toml");
    REQUIRE_FALSE(d.has_value());
}

TEST_CASE("from_file detects an include cycle", "[defs][includes]") {
    TempDir td;
    write_text(td.path / "a.toml", R"toml(
[pack]
id         = "a"
endianness = "big"
includes   = ["b.toml"]
)toml");
    write_text(td.path / "b.toml", R"toml(
[pack]
id         = "b"
endianness = "big"
includes   = ["a.toml"]
)toml");
    auto const d = st::Definition::from_file(td.path / "a.toml");
    REQUIRE_FALSE(d.has_value());
}

// ----- extends / inheritance ---------------------------------------------

TEST_CASE("from_directory resolves a single 'extends' chain", "[defs][extends]") {
    TempDir td;

    write_text(td.path / "va-wrx-mt-2019" / "pack.toml", R"toml(
[pack]
id             = "va-wrx-mt-2019"
endianness     = "big"
rom_size_bytes = 65536

[[scaling]]
id        = "rpm_x1"
formula   = "linear"
factor    = 1.0
data_type = "uint16_be"
unit      = "rpm"

[[scaling]]
id        = "load_x001"
formula   = "linear"
factor    = 0.001
data_type = "uint16_be"
unit      = "g/rev"

[[axis]]
id        = "rpm_axis"
data_type = "uint16_be"
address   = 0
length    = 16
scaling   = "rpm_x1"

[[table]]
id         = "fuel_2019"
dimensions = 1
data_type  = "uint8"
address    = 256
scaling    = "rpm_x1"
axis_x     = "rpm_axis"
)toml");

    write_text(td.path / "va-wrx-mt-2020" / "pack.toml", R"toml(
[pack]
id             = "va-wrx-mt-2020"
endianness     = "big"
rom_size_bytes = 65536
extends        = "va-wrx-mt-2019"

# Override an inherited scaling.
[[scaling]]
id        = "rpm_x1"
formula   = "linear"
factor    = 2.0
data_type = "uint16_be"
unit      = "rpm"

# Pure addition.
[[table]]
id         = "boost_2020"
dimensions = 1
data_type  = "uint16_be"
address    = 1024
scaling    = "load_x001"
axis_x     = "rpm_axis"
)toml");

    auto const d = st::Definition::from_directory(td.path / "va-wrx-mt-2020");
    REQUIRE(d.has_value());

    // Pack header is the child's; 'extends' is consumed.
    REQUIRE(d->pack().id == "va-wrx-mt-2020");
    REQUIRE_FALSE(d->pack().extends.has_value());

    // Two scalings (inherited + overridden), not three.
    REQUIRE(d->scalings().size() == 2);
    auto const *rpm = d->find_scaling("rpm_x1");
    REQUIRE(rpm != nullptr);
    REQUIRE(std::get<st::LinearScaling>(rpm->formula).factor == 2.0);
    REQUIRE(d->find_scaling("load_x001") != nullptr);

    // Inherited axis still present.
    REQUIRE(d->find_axis("rpm_axis") != nullptr);

    // Both parent's table and child's addition resolve.
    REQUIRE(d->find_table("fuel_2019") != nullptr);
    REQUIRE(d->find_table("boost_2020") != nullptr);
}

TEST_CASE("from_directory appends identifications across inheritance", "[defs][extends]") {
    TempDir td;

    write_text(td.path / "parent" / "pack.toml", R"toml(
[pack]
id         = "parent-pack"
endianness = "big"

[[identification]]
name        = "Parent CID"
cid_address = 0x100
cid_length  = 8
cid_match   = "PARENT  "
)toml");

    write_text(td.path / "child" / "pack.toml", R"toml(
[pack]
id         = "child-pack"
endianness = "big"
extends    = "parent-pack"

[[identification]]
name        = "Child CID"
cid_address = 0x200
cid_length  = 8
cid_match   = "CHILD   "
)toml");

    auto const d = st::Definition::from_directory(td.path / "child");
    REQUIRE(d.has_value());
    REQUIRE(d->identifications().size() == 2);
    REQUIRE(d->identifications()[0].cid_match == "PARENT  ");
    REQUIRE(d->identifications()[1].cid_match == "CHILD   ");
}

TEST_CASE("from_directory follows multi-level extends chains", "[defs][extends]") {
    TempDir td;

    write_text(td.path / "grandparent" / "pack.toml", R"toml(
[pack]
id         = "gp"
endianness = "big"

[[scaling]]
id        = "from_gp"
formula   = "linear"
factor    = 1.0
data_type = "uint8"
)toml");

    write_text(td.path / "parent" / "pack.toml", R"toml(
[pack]
id         = "parent"
endianness = "big"
extends    = "gp"

[[scaling]]
id        = "from_parent"
formula   = "linear"
factor    = 1.0
data_type = "uint8"
)toml");

    write_text(td.path / "child" / "pack.toml", R"toml(
[pack]
id         = "child"
endianness = "big"
extends    = "parent"

[[scaling]]
id        = "from_child"
formula   = "linear"
factor    = 1.0
data_type = "uint8"
)toml");

    auto const d = st::Definition::from_directory(td.path / "child");
    REQUIRE(d.has_value());
    REQUIRE(d->pack().id == "child");
    REQUIRE(d->scalings().size() == 3);
    REQUIRE(d->find_scaling("from_gp") != nullptr);
    REQUIRE(d->find_scaling("from_parent") != nullptr);
    REQUIRE(d->find_scaling("from_child") != nullptr);
}

TEST_CASE("from_directory errors on missing parent pack", "[defs][extends]") {
    TempDir td;
    write_text(td.path / "orphan" / "pack.toml", R"toml(
[pack]
id         = "orphan"
endianness = "big"
extends    = "does-not-exist"
)toml");

    auto const d = st::Definition::from_directory(td.path / "orphan");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::FileNotFound);
    REQUIRE(d.error().message().find("does-not-exist") != std::string::npos);
}

TEST_CASE("from_directory detects an extends cycle", "[defs][extends]") {
    TempDir td;
    write_text(td.path / "a" / "pack.toml", R"toml(
[pack]
id         = "a"
endianness = "big"
extends    = "b"
)toml");
    write_text(td.path / "b" / "pack.toml", R"toml(
[pack]
id         = "b"
endianness = "big"
extends    = "a"
)toml");

    auto const d = st::Definition::from_directory(td.path / "a");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
    REQUIRE(d.error().message().find("cycle") != std::string::npos);
}

TEST_CASE("from_toml_string silently ignores 'extends' (no filesystem context)",
          "[defs][extends]") {
    auto const d = st::Definition::from_toml_string(R"(
[pack]
id      = "x"
extends = "ignored-without-filesystem"
)");
    REQUIRE(d.has_value());
    // The field is still surfaced on the Pack struct so callers can detect
    // it; it just isn't resolved.
    REQUIRE(d->pack().extends.has_value());
    REQUIRE(*d->pack().extends == "ignored-without-filesystem");
}

TEST_CASE("parse_data_type round-trips known values", "[defs][types]") {
    REQUIRE(st::parse_data_type("uint8").has_value());
    REQUIRE(*st::parse_data_type("uint8") == st::DataType::Uint8);
    REQUIRE(st::to_string(*st::parse_data_type("uint16_be")) == "uint16_be");
    REQUIRE(st::byte_size(*st::parse_data_type("uint32_be")) == 4);
    REQUIRE(st::byte_size(*st::parse_data_type("float32_le")) == 4);
    REQUIRE_FALSE(st::parse_data_type("uint37").has_value());
}

// ---- [[hook]] ----------------------------------------------------
// Custom-feature splice-point declarations per docs/16. The parser
// surfaces them on Definition; the editor builds nodes from them.

TEST_CASE("Definition parses a [[hook]] with typed inputs/outputs", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id          = "after_fuel_calc"
description = "After commanded injector PW is computed"
ecu_address = 0x000ABCD0
free_ram    = { base = 0x40000000, length = 256 }
inputs  = [
  { name = "rpm",          type = "float", unit = "rpm" },
  { name = "load",         type = "float", unit = "%"   },
  { name = "commanded_pw", type = "float", unit = "ms"  },
]
outputs = [
  { name = "commanded_pw_override", type = "float", unit = "ms" },
]
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->hooks().size() == 1);
    auto const &h = d->hooks().front();
    REQUIRE(h.id == "after_fuel_calc");
    REQUIRE(h.description == "After commanded injector PW is computed");
    REQUIRE(h.inputs.size() == 3);
    REQUIRE(h.outputs.size() == 1);
    REQUIRE(h.inputs[0].name == "rpm");
    REQUIRE(h.inputs[0].type == "float");
    REQUIRE(h.inputs[0].unit == "rpm");
    REQUIRE(h.outputs[0].name == "commanded_pw_override");
    REQUIRE(h.outputs[0].type == "float");
    REQUIRE(h.ecu_address.has_value());
    REQUIRE(*h.ecu_address == 0x000ABCD0u);
    REQUIRE(h.free_ram_base.has_value());
    REQUIRE(*h.free_ram_base == 0x40000000u);
    REQUIRE(h.free_ram_length.has_value());
    REQUIRE(*h.free_ram_length == 256u);
}

TEST_CASE("Hooks with no inputs/outputs and no codegen metadata still parse", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id          = "trigger_only"
description = "A pure side-effect hook"
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->hooks().size() == 1);
    auto const &h = d->hooks().front();
    REQUIRE(h.inputs.empty());
    REQUIRE(h.outputs.empty());
    REQUIRE_FALSE(h.ecu_address.has_value());
    REQUIRE_FALSE(h.free_ram_base.has_value());
}

TEST_CASE("Hook rejects missing id", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
description = "no id field"
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
    REQUIRE(d.error().message().find("missing id") != std::string::npos);
}

TEST_CASE("Hook rejects signal with unknown type", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id      = "h"
inputs  = [ { name = "x", type = "string" } ]
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
    REQUIRE(d.error().message().find("type must be") != std::string::npos);
}

TEST_CASE("Hook rejects signal with empty name", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id      = "h"
outputs = [ { name = "", type = "float" } ]
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
    REQUIRE(d.error().message().find("missing name") != std::string::npos);
}

TEST_CASE("Hook rejects signal that isn't an inline table", "[defs][hooks]") {
    // docs/16 uses bare strings in its illustrative example, but the
    // parsed schema requires typed signals — bare strings would not
    // carry a type, so they're rejected to fail-loud.
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id     = "h"
inputs = [ "rpm", "load" ]
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Hook + signal labels parse, default to empty when omitted", "[defs][hooks]") {
    // display_name on the hook, label on signals — both optional. Editor
    // uses them when non-empty; falls back to id/name otherwise.
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id           = "after_fuel_calc"
display_name = "After fuel calc"
description  = "Splice point."
inputs = [
  { name = "commanded_pw", label = "Commanded fuel PW", type = "float" },
  { name = "rpm",                                          type = "float" },
]
outputs = [
  { name = "commanded_pw_override", label = "Override fuel PW", type = "float" },
]
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->hooks().size() == 1);
    auto const &h = d->hooks().front();
    REQUIRE(h.display_name == "After fuel calc");
    REQUIRE(h.inputs[0].label == "Commanded fuel PW");
    REQUIRE(h.inputs[1].label == ""); // omitted → empty
    REQUIRE(h.outputs[0].label == "Override fuel PW");
}

// ---- [[primitive]] -----------------------------------------------
// Reusable computation nodes (arithmetic, boolean logic) declared
// in the pack and exposed by the editor's palette alongside hooks.

TEST_CASE("Definition parses a [[primitive]] with typed pins", "[defs][primitives]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[primitive]]
id           = "multiply"
display_name = "Multiply"
description  = "Pure floating-point product of two inputs."
inputs = [
  { name = "a", type = "float" },
  { name = "b", type = "float" },
]
outputs = [
  { name = "out", type = "float" },
]
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->primitives().size() == 1);
    auto const &p = d->primitives().front();
    REQUIRE(p.id == "multiply");
    REQUIRE(p.display_name == "Multiply");
    REQUIRE(p.inputs.size() == 2);
    REQUIRE(p.outputs.size() == 1);
    REQUIRE(p.outputs[0].name == "out");
}

TEST_CASE("Primitive rejects missing id", "[defs][primitives]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[primitive]]
display_name = "no id field"
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().message().find("missing id") != std::string::npos);
}

TEST_CASE("Primitive rejects signal with unknown type", "[defs][primitives]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[primitive]]
id     = "weird"
inputs = [ { name = "x", type = "decimal" } ]
)toml");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().message().find("type must be") != std::string::npos);
}

TEST_CASE("Hooks and primitives co-exist in one pack", "[defs][primitives]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id = "h"

[[primitive]]
id = "p"
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->hooks().size() == 1);
    REQUIRE(d->primitives().size() == 1);
    REQUIRE(d->find_hook("h") != nullptr);
    REQUIRE(d->find_primitive("p") != nullptr);
    REQUIRE(d->find_hook("p") == nullptr);
    REQUIRE(d->find_primitive("h") == nullptr);
}

TEST_CASE("Definition::find_hook locates a parsed hook by id", "[defs][hooks]") {
    auto const d = st::Definition::from_toml_string(R"toml(
[pack]
id         = "demo"
endianness = "big"

[[hook]]
id = "a"

[[hook]]
id = "b"
)toml");
    REQUIRE(d.has_value());
    REQUIRE(d->find_hook("a") != nullptr);
    REQUIRE(d->find_hook("b") != nullptr);
    REQUIRE(d->find_hook("c") == nullptr);
}
