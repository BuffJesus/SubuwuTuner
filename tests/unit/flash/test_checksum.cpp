// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/defs.hpp"
#include "st/flash/checksum.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl = st::flash;

// ---- name / parse round-trip ------------------------------------

TEST_CASE("checksum_kind_name maps every defined kind to its string", "[flash][checksum]") {
    REQUIRE(std::string_view{fl::checksum_kind_name(fl::ChecksumKind::None)} == "none");
    REQUIRE(std::string_view{fl::checksum_kind_name(fl::ChecksumKind::SubaruStd)} == "subaru_std");
    REQUIRE(std::string_view{fl::checksum_kind_name(fl::ChecksumKind::SubaruAlt)} == "subaru_alt");
    REQUIRE(std::string_view{fl::checksum_kind_name(fl::ChecksumKind::SubaruAlt2)} ==
            "subaru_alt2");
}

TEST_CASE("parse_checksum_kind round-trips canonical names", "[flash][checksum]") {
    for (auto k : {fl::ChecksumKind::None, fl::ChecksumKind::SubaruStd, fl::ChecksumKind::SubaruAlt,
                   fl::ChecksumKind::SubaruAlt2}) {
        auto const parsed = fl::parse_checksum_kind(fl::checksum_kind_name(k));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == k);
    }
}

TEST_CASE("parse_checksum_kind rejects unknown + case-mismatched + empty", "[flash][checksum]") {
    REQUIRE_FALSE(fl::parse_checksum_kind("").has_value());
    REQUIRE_FALSE(fl::parse_checksum_kind("SUBARU_STD").has_value());
    REQUIRE_FALSE(fl::parse_checksum_kind("subaru-std").has_value());
    REQUIRE_FALSE(fl::parse_checksum_kind("nissan_std").has_value());
}

// ---- pack-field lenient mapping ---------------------------------

TEST_CASE("checksum_kind_from_pack: empty -> None (default for legacy packs)",
          "[flash][checksum]") {
    REQUIRE(fl::checksum_kind_from_pack("") == fl::ChecksumKind::None);
}

TEST_CASE("checksum_kind_from_pack: valid kind passes through", "[flash][checksum]") {
    REQUIRE(fl::checksum_kind_from_pack("subaru_std") == fl::ChecksumKind::SubaruStd);
    REQUIRE(fl::checksum_kind_from_pack("subaru_alt2") == fl::ChecksumKind::SubaruAlt2);
}

TEST_CASE("checksum_kind_from_pack: unrecognized -> None (lenient)", "[flash][checksum]") {
    // Forward-compat: a future pack might declare a kind this build
    // doesn't know yet. Default to None so the Flasher's existing
    // path (no-op repair) runs; a stricter check belongs in
    // pack-validate, not here.
    REQUIRE(fl::checksum_kind_from_pack("subaru_future") == fl::ChecksumKind::None);
}

// ---- factory dispatch -------------------------------------------

TEST_CASE("make_checksum_repair(None) returns a working no-op", "[flash][checksum]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::None);
    REQUIRE(r != nullptr);
    REQUIRE(r->name() == "none");

    // Calling repair() on any-shaped ROM bytes succeeds without
    // mutating them.
    std::vector<std::uint8_t> rom(1024, 0xAB);
    auto const before = rom;
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(rom == before); // truly no-op
}

TEST_CASE("make_checksum_repair(SubaruStd) returns NotImplemented with citation",
          "[flash][checksum]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::SubaruStd);
    REQUIRE(r != nullptr);
    REQUIRE(r->name() == "subaru_std");

    std::vector<std::uint8_t> rom(1024, 0xAB);
    auto const status = r->repair(rom);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::NotImplemented);
    REQUIRE(status.error().message().find("ChecksumSTD.java") != std::string::npos);
}

TEST_CASE("make_checksum_repair(SubaruAlt) returns NotImplemented", "[flash][checksum]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::SubaruAlt);
    REQUIRE(r != nullptr);
    REQUIRE(r->name() == "subaru_alt");

    std::vector<std::uint8_t> rom(1024, 0xAB);
    REQUIRE_FALSE(r->repair(rom).has_value());
    REQUIRE(r->repair(rom).error().code() == st::ErrorCode::NotImplemented);
}

TEST_CASE("make_checksum_repair(SubaruAlt2) returns NotImplemented", "[flash][checksum]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::SubaruAlt2);
    REQUIRE(r != nullptr);
    REQUIRE(r->name() == "subaru_alt2");

    std::vector<std::uint8_t> rom(1024, 0xAB);
    REQUIRE_FALSE(r->repair(rom).has_value());
    REQUIRE(r->repair(rom).error().code() == st::ErrorCode::NotImplemented);
}

// ---- pack -> kind -> repair end-to-end ----------------------------

TEST_CASE("end-to-end: pack's checksum_type field threads through to a repair impl",
          "[flash][checksum]") {
    // A pack author declares `checksum_type = "subaru_std"`; the
    // Flasher (eventually) calls checksum_kind_from_pack then
    // make_checksum_repair. This test pins that whole chain.
    std::string_view const pack_field = "subaru_std";
    auto const kind = fl::checksum_kind_from_pack(pack_field);
    REQUIRE(kind == fl::ChecksumKind::SubaruStd);

    auto repair = fl::make_checksum_repair(kind);
    REQUIRE(repair != nullptr);
    REQUIRE(repair->name() == "subaru_std");
}

TEST_CASE("end-to-end: pack with empty checksum_type -> working no-op", "[flash][checksum]") {
    // Pre-d68d796 packs don't carry the field. The Flasher should
    // still be able to invoke repair() without branching.
    auto repair = fl::make_checksum_repair(fl::checksum_kind_from_pack(""));
    REQUIRE(repair != nullptr);
    REQUIRE(repair->name() == "none");
    std::vector<std::uint8_t> rom(16, 0);
    REQUIRE(repair->repair(rom).has_value());
}

// ---- apply_checksum_repair (Definition-driven wrapper) ----------

namespace {
constexpr std::string_view kPackSubaruStdToml = R"toml(
[pack]
schema_version = 1
id             = "test-pack-stdsum"
endianness     = "big"
checksum_type  = "subaru_std"
)toml";

constexpr std::string_view kPackNoneToml = R"toml(
[pack]
schema_version = 1
id             = "test-pack-nosum"
endianness     = "big"
)toml";

constexpr std::string_view kPackUnknownKindToml = R"toml(
[pack]
schema_version = 1
id             = "test-pack-future"
endianness     = "big"
checksum_type  = "subaru_future"
)toml";
} // namespace

TEST_CASE("apply_checksum_repair: pack with checksum_type=subaru_std -> "
          "NotImplemented via the wrapper",
          "[flash][checksum]") {
    auto def = st::Definition::from_toml_string(kPackSubaruStdToml);
    REQUIRE(def.has_value());

    std::vector<std::uint8_t> rom(1024, 0xAB);
    auto const before = rom;
    auto const status = st::flash::apply_checksum_repair(rom, *def);

    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::NotImplemented);
    // Failure path must not mutate the bytes -- important contract
    // since callers will spill the repaired buffer to disk only
    // on success.
    REQUIRE(rom == before);
}

TEST_CASE("apply_checksum_repair: pack without checksum_type -> no-op success",
          "[flash][checksum]") {
    auto def = st::Definition::from_toml_string(kPackNoneToml);
    REQUIRE(def.has_value());

    std::vector<std::uint8_t> rom(1024, 0xCD);
    auto const before = rom;
    auto const status = st::flash::apply_checksum_repair(rom, *def);

    REQUIRE(status.has_value());
    REQUIRE(rom == before); // no-op preserves bytes
}

TEST_CASE("apply_checksum_repair: pack with unrecognized kind -> lenient None",
          "[flash][checksum]") {
    // checksum_kind_from_pack treats unknown values as None for
    // forward-compat with future schema revisions. Verify the
    // wrapper carries that semantic through.
    auto def = st::Definition::from_toml_string(kPackUnknownKindToml);
    REQUIRE(def.has_value());

    std::vector<std::uint8_t> rom(1024, 0xEF);
    auto const status = st::flash::apply_checksum_repair(rom, *def);
    REQUIRE(status.has_value()); // None -> no-op -> ok()
}
