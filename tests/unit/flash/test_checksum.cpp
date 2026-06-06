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

TEST_CASE("make_checksum_repair(SubaruStd) rejects a ROM smaller than cal_end",
          "[flash][checksum][subaru_std]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::SubaruStd);
    REQUIRE(r != nullptr);
    REQUIRE(r->name() == "subaru_std");
    // Default config targets SH-2A 2 MB (cal_end = 0x200000); the
    // 1 KB ROM is too small and the impl should fail cleanly rather
    // than indexing past the end.
    std::vector<std::uint8_t> rom(1024, 0xAB);
    auto const status = r->repair(rom);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(status.error().message().find("too small") != std::string::npos);
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
          "InvalidArgument when ROM is too small for default cal region",
          "[flash][checksum][subaru_std]") {
    auto def = st::Definition::from_toml_string(kPackSubaruStdToml);
    REQUIRE(def.has_value());

    std::vector<std::uint8_t> rom(1024, 0xAB);
    auto const before = rom;
    auto const status = st::flash::apply_checksum_repair(rom, *def);

    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::InvalidArgument);
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

// ============================================================================
// SubaruStd behavioral tests
// ============================================================================
//
// The impl is fresh-from-spec; these tests pin the documented algorithm
// shape (sum of 16-bit BE words excluding slot bytes, sum at slot_A,
// ~sum at slot_B). Byte-validation against a known-good stock ROM
// remains a pending Tier-4 item per docs/04 — these tests confirm the
// math, not that it matches what the ECU bootloader accepts.

namespace {

// Tiny synthetic config so tests can run on small ROMs.
fl::SubaruStdConfig small_cfg() {
    fl::SubaruStdConfig cfg;
    cfg.cal_start = 0x10;
    cfg.cal_end = 0x100;       // 0xF0 bytes / 120 words
    cfg.sum_slot = 0x10;       // first 4 bytes of cal region
    cfg.complement_slot = 0x14;
    return cfg;
}

std::uint32_t read_u32_be(std::span<std::uint8_t const> rom, std::uint32_t off) {
    return (static_cast<std::uint32_t>(rom[off]) << 24) |
           (static_cast<std::uint32_t>(rom[off + 1]) << 16) |
           (static_cast<std::uint32_t>(rom[off + 2]) << 8) |
           static_cast<std::uint32_t>(rom[off + 3]);
}

} // namespace

TEST_CASE("SubaruStdConfig::validate accepts the default + rejects bad configs",
          "[flash][checksum][subaru_std][config]") {
    REQUIRE(fl::SubaruStdConfig{}.validate().empty());
    {
        fl::SubaruStdConfig c;
        c.sum_slot = 0x101; // unaligned
        REQUIRE_FALSE(c.validate().empty());
    }
    {
        fl::SubaruStdConfig c;
        c.complement_slot = c.sum_slot; // collision
        REQUIRE_FALSE(c.validate().empty());
    }
    {
        // Slots wholly outside the cal region are FINE — the SH-2A
        // 2 MB default puts them at 0x100/0x104 in the bootloader
        // region, well below cal_start (0x10000). Confirm.
        fl::SubaruStdConfig c;
        c.sum_slot = c.cal_start - 4; // ends right at cal_start
        REQUIRE(c.validate().empty());
    }
    {
        fl::SubaruStdConfig c;
        c.cal_end = c.cal_start; // empty cal
        REQUIRE_FALSE(c.validate().empty());
    }
}

TEST_CASE("SubaruStd repair: sum_slot + complement_slot post-repair invariant",
          "[flash][checksum][subaru_std]") {
    auto const cfg = small_cfg();
    auto r = fl::make_subaru_std_repair(cfg);
    std::vector<std::uint8_t> rom(0x200, 0);
    // Fill the cal region with a pattern so the sum is non-trivial.
    for (std::uint32_t i = cfg.cal_start; i < cfg.cal_end; ++i) {
        rom[i] = static_cast<std::uint8_t>((i * 31u) & 0xFFu);
    }
    REQUIRE(r->repair(rom).has_value());
    auto const sum = read_u32_be(rom, cfg.sum_slot);
    auto const comp = read_u32_be(rom, cfg.complement_slot);
    // The boot-time integrity check pattern: sum + complement equals
    // all-ones (each bit appears once across the pair).
    REQUIRE((sum ^ comp) == 0xFFFFFFFFu);
}

TEST_CASE("SubaruStd repair: idempotent on a re-repair pass",
          "[flash][checksum][subaru_std]") {
    auto const cfg = small_cfg();
    auto r = fl::make_subaru_std_repair(cfg);
    std::vector<std::uint8_t> rom(0x200, 0xAB);
    REQUIRE(r->repair(rom).has_value());
    auto const after_first = rom;
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(rom == after_first);
}

TEST_CASE("SubaruStd repair: flipping a cal byte changes the checksum",
          "[flash][checksum][subaru_std]") {
    auto const cfg = small_cfg();
    auto r = fl::make_subaru_std_repair(cfg);
    std::vector<std::uint8_t> rom(0x200, 0);
    REQUIRE(r->repair(rom).has_value());
    auto const baseline_sum = read_u32_be(rom, cfg.sum_slot);
    // Flip a byte well inside the cal region (away from slots) and
    // re-repair. The new sum must differ.
    rom[cfg.cal_start + 0x40] = 0xFF;
    REQUIRE(r->repair(rom).has_value());
    auto const after_sum = read_u32_be(rom, cfg.sum_slot);
    REQUIRE(after_sum != baseline_sum);
}

TEST_CASE("SubaruStd repair: changing a byte outside cal region leaves the sum alone",
          "[flash][checksum][subaru_std]") {
    auto const cfg = small_cfg();
    auto r = fl::make_subaru_std_repair(cfg);
    std::vector<std::uint8_t> rom(0x200, 0);
    REQUIRE(r->repair(rom).has_value());
    auto const baseline_sum = read_u32_be(rom, cfg.sum_slot);
    // Byte outside [cal_start, cal_end) — should NOT contribute.
    rom[0x180] = 0xFF;
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(read_u32_be(rom, cfg.sum_slot) == baseline_sum);
}

TEST_CASE("SubaruStd repair: pre-existing slot bytes don't contaminate the sum",
          "[flash][checksum][subaru_std]") {
    auto const cfg = small_cfg();
    auto r = fl::make_subaru_std_repair(cfg);
    std::vector<std::uint8_t> rom_a(0x200, 0);
    std::vector<std::uint8_t> rom_b(0x200, 0);
    // Pre-pollute rom_b's slots with garbage. Repair should produce
    // the same sum on both ROMs (slots are skipped during summing).
    for (std::uint32_t off = cfg.sum_slot; off < cfg.sum_slot + 8; ++off)
        rom_b[off] = 0x55;
    REQUIRE(r->repair(rom_a).has_value());
    REQUIRE(r->repair(rom_b).has_value());
    REQUIRE(read_u32_be(rom_a, cfg.sum_slot) ==
            read_u32_be(rom_b, cfg.sum_slot));
}

TEST_CASE("SubaruStd repair: hand-computed sum matches the impl",
          "[flash][checksum][subaru_std][algorithm]") {
    // ROM is 0x200 bytes; cal region [0x10..0x100). Slots at 0x10,
    // 0x14. The 0xF0-byte cal region minus the 8 slot bytes leaves
    // 0xE8 bytes / 0x74 words to sum.
    auto const cfg = small_cfg();
    std::vector<std::uint8_t> rom(0x200, 0);
    // Word at 0x18 = 0x1234 (the first word after both slots).
    rom[0x18] = 0x12;
    rom[0x19] = 0x34;
    // Word at 0x1A = 0x0001.
    rom[0x1B] = 0x01;
    // Word at 0xFE = 0xFFFF (the last word before cal_end=0x100).
    rom[0xFE] = 0xFF;
    rom[0xFF] = 0xFF;
    // Expected sum = 0x1234 + 0x0001 + 0xFFFF = 0x11234.
    auto r = fl::make_subaru_std_repair(cfg);
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(read_u32_be(rom, cfg.sum_slot) == 0x11234u);
    REQUIRE(read_u32_be(rom, cfg.complement_slot) == ~0x11234u);
}
