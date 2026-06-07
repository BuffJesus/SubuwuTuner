// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/defs.hpp"
#include "st/flash/checksum.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

// ============================================================================
// PerInstallBlockCrc32 — aftermarket-installer FA-DIT algorithm
// ============================================================================
//
// zlib-style CRC-32 over 25 install blocks; slots BE u32 at
// 0x1FFF3C..0x1FFFA0. Slot 24 (self-referential) left untouched on
// repair — non-blocking for EGR/TGV-class edits in slots 5..16.

TEST_CASE("name + parse round-trip per_install_block_crc32",
          "[flash][checksum][per_install_block_crc32]") {
    REQUIRE(std::string_view{fl::checksum_kind_name(
                fl::ChecksumKind::PerInstallBlockCrc32)} ==
            "per_install_block_crc32");
    auto parsed = fl::parse_checksum_kind("per_install_block_crc32");
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == fl::ChecksumKind::PerInstallBlockCrc32);
    // Legacy alias still parses but emitter writes the new name.
    auto legacy = fl::parse_checksum_kind("cobb_per_block_crc32");
    REQUIRE(legacy.has_value());
    REQUIRE(*legacy == fl::ChecksumKind::PerInstallBlockCrc32);
}

TEST_CASE("PerInstallBlockCrc32 rejects ROMs smaller than 2 MB",
          "[flash][checksum][per_install_block_crc32]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    REQUIRE(r->name() == "per_install_block_crc32");
    std::vector<std::uint8_t> rom(0x100000, 0); // 1 MB
    auto const s = r->repair(rom);
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("PerInstallBlockCrc32 repair is idempotent on a re-pass",
          "[flash][checksum][per_install_block_crc32]") {
    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    std::vector<std::uint8_t> rom(0x200000, 0);
    REQUIRE(r->repair(rom).has_value());
    auto const after_first = rom;
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(rom == after_first);
}

TEST_CASE("PerInstallBlockCrc32 single-block edit changes only that slot",
          "[flash][checksum][per_install_block_crc32]") {
    // Smoke-test pattern: mutate a byte in block 7 (the EGR/TGV
    // sector at 0x030000-0x040000), repair, verify only slot 7's CRC
    // moved.
    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    std::vector<std::uint8_t> rom(0x200000, 0);
    REQUIRE(r->repair(rom).has_value());
    // Snapshot all 25 slot values.
    constexpr std::uint32_t kTableBase = 0x1FFF3Cu;
    std::array<std::array<std::uint8_t, 4>, 25> baseline_slots{};
    for (std::size_t i = 0; i < 25; ++i) {
        for (std::size_t b = 0; b < 4; ++b)
            baseline_slots[i][b] = rom[kTableBase + i * 4 + b];
    }
    // Flip a byte in block 7 (the EGR Airflow region).
    rom[0x034DEA] = 0x42;
    REQUIRE(r->repair(rom).has_value());
    // Slot 7 must have changed.
    bool slot7_changed = false;
    for (std::size_t b = 0; b < 4; ++b) {
        if (rom[kTableBase + 7 * 4 + b] != baseline_slots[7][b]) {
            slot7_changed = true;
            break;
        }
    }
    REQUIRE(slot7_changed);
    // Slots 0..6, 8..23 must be unchanged. Slot 24 untouched by
    // design (self-referential).
    for (std::size_t i = 0; i < 25; ++i) {
        if (i == 7)
            continue;
        for (std::size_t b = 0; b < 4; ++b) {
            INFO("slot " << i << " byte " << b);
            REQUIRE(rom[kTableBase + i * 4 + b] == baseline_slots[i][b]);
        }
    }
}

TEST_CASE("PerInstallBlockCrc32: slot 24 is never written by repair",
          "[flash][checksum][per_install_block_crc32]") {
    // Slot 24's decode is still pending. repair() must leave whatever
    // was there untouched, even if it's garbage.
    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    std::vector<std::uint8_t> rom(0x200000, 0);
    // Stamp a recognizable pattern at slot 24.
    constexpr std::uint32_t kSlot24Off = 0x1FFF3Cu + 24 * 4;
    rom[kSlot24Off + 0] = 0xDE;
    rom[kSlot24Off + 1] = 0xAD;
    rom[kSlot24Off + 2] = 0xBE;
    rom[kSlot24Off + 3] = 0xEF;
    REQUIRE(r->repair(rom).has_value());
    REQUIRE(rom[kSlot24Off + 0] == 0xDE);
    REQUIRE(rom[kSlot24Off + 1] == 0xAD);
    REQUIRE(rom[kSlot24Off + 2] == 0xBE);
    REQUIRE(rom[kSlot24Off + 3] == 0xEF);
}

// Opt-in end-to-end validation against a real aftermarket-installed
// LF79101P ROM. Skipped when the file isn't present (CI / fresh
// checkout). Confirms the repair is byte-identical to the stored
// checksum bytes — i.e. the algorithm + block table matches what the
// installer actually writes.
TEST_CASE("PerInstallBlockCrc32 round-trip against live-dump reference",
          "[flash][checksum][per_install_block_crc32][rom]") {
    auto const path =
        std::filesystem::path{"D:/Subuwu/subaru-data/reference-dumps/"
                              "fehr-live-dump-2026-06-06.bin"};
    if (!std::filesystem::exists(path)) {
        WARN("Reference ROM not present at " << path.string()
             << " — skipping end-to-end validation.");
        return;
    }
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    std::vector<std::uint8_t> rom{(std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>()};
    REQUIRE(rom.size() == 0x200000u);
    auto const before = rom;
    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    REQUIRE(r->repair(rom).has_value());
    // ROM was unmodified before repair — the stored slot CRCs were
    // already correct. Post-repair must be byte-identical, proving
    // the algorithm matches what the installer writes.
    REQUIRE(rom == before);
}

// Opt-in end-to-end integration fixture. Replicates the EGR + TGV
// delete recipe, runs repair, and asserts byte-for-byte equality with
// an independently-produced candidate ROM.
//
// Three edits + slot-7 recompute:
//   - 0x034DEA..0x034EAA  EGR Airflow                       192 bytes
//   - 0x034F9A..0x03501A  EGR Absolute Pressure Main        128 bytes
//   - 0x03A922..0x03ABE2  ignition_comp_coolant_tgv_closed  704 bytes
//   - slot 7 (@ 0x1FFF58) recomputes 0x9E47A152 -> 0x30E98431
//
// All three regions land in block 7 (the 0x030000-0x040000 cal sector),
// so only one CRC slot moves. Confirms the algorithm AND the
// single-block edit-tracking math against a real-ROM cross-check.
TEST_CASE("PerInstallBlockCrc32 EGR+TGV delete fixture matches reference candidate",
          "[flash][checksum][per_install_block_crc32][rom][fixture]") {
    auto const input_path =
        std::filesystem::path{"D:/Subuwu/subaru-data/reference-dumps/"
                              "fehr-live-dump-2026-06-06.bin"};
    auto const candidate_path =
        std::filesystem::path{"D:/Subuwu/subaru-data/derived-dumps/"
                              "fehr-live-dump-2026-06-06_no-egr-tgv_candidate.bin"};
    if (!std::filesystem::exists(input_path) ||
        !std::filesystem::exists(candidate_path)) {
        WARN("Reference or candidate ROM not present — skipping fixture test.");
        return;
    }
    auto load_rom = [](std::filesystem::path const &p) {
        std::ifstream in{p, std::ios::binary};
        REQUIRE(in.is_open());
        std::vector<std::uint8_t> bytes{(std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>()};
        REQUIRE(bytes.size() == 0x200000u);
        return bytes;
    };
    auto rom = load_rom(input_path);
    auto const expected = load_rom(candidate_path);

    // Snapshot stored slot-7 before the edit. Pre-edit value is
    // 0x9E47A152.
    constexpr std::uint32_t kSlot7Off = 0x1FFF3Cu + 7u * 4u;
    REQUIRE(read_u32_be(rom, kSlot7Off) == 0x9E47A152u);

    // Apply the three table-zero edits.
    std::fill(rom.begin() + 0x034DEA, rom.begin() + 0x034EAA, std::uint8_t{0});
    std::fill(rom.begin() + 0x034F9A, rom.begin() + 0x03501A, std::uint8_t{0});
    std::fill(rom.begin() + 0x03A922, rom.begin() + 0x03ABE2, std::uint8_t{0});

    auto r = fl::make_checksum_repair(fl::ChecksumKind::PerInstallBlockCrc32);
    REQUIRE(r->repair(rom).has_value());

    // Slot 7 must land on the predicted new value.
    REQUIRE(read_u32_be(rom, kSlot7Off) == 0x30E98431u);

    // Byte-for-byte equality with the candidate proves the full
    // pipeline (edits + algorithm + slot table base + endianness)
    // matches the reference.
    REQUIRE(rom == expected);
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
