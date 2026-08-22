// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Synthetic coverage for st::tune_export. The bit-exact Fehr
// cross-check from round-58 analyst §2.2 (decat diffs against
// fehr-full-dump.bin produced balance bytes 0x6181 matching
// fehr-plus-decat.bin) lives off-tree per the Path B distribution
// policy — we can't keep those ROMs in the repo. These tests
// exercise the math, the validator contract, and the write-plan
// ordering against synthetic ROMs we own.

#include "st/tune_export.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace st::tune_export;

namespace {

// A 2 MB ROM filled with a deterministic pattern that's NOT
// already balanced. compute_balance will need to produce a real
// non-zero correction for this.
std::vector<std::uint8_t> make_pattern_rom() {
    std::vector<std::uint8_t> rom(kWritableEnd, 0u);
    for (std::uint32_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<std::uint8_t>(i & 0xFFU);
    }
    // Bake the boot integrity bytes to their stock values so cal
    // tests don't trip the BootIntegrityClobber check from the
    // pattern bytes alone.
    rom[0x006000] = 0x55;  rom[0x006001] = 0x55;       // sig 1
    rom[0x006010] = 0x05;  rom[0x006011] = 0x04;       // paired-eq
    rom[0x00006C] = 0x05;                              // paired-eq byte
    rom[0x1FFFF2] = 0xAA;  rom[0x1FFFF3] = 0xAA;       // sig 2
    return rom;
}

// A 2 MB ROM that already has the balance cell set such that the
// brick-protection sum is 0x5AA5.
std::vector<std::uint8_t> make_pre_balanced_rom() {
    auto rom = make_pattern_rom();
    rom[kBalanceOffset]      = 0;
    rom[kBalanceOffset + 1U] = 0;
    auto const bal = compute_balance(rom);
    rom[kBalanceOffset]      = static_cast<std::uint8_t>((bal >> 8) & 0xFFU);
    rom[kBalanceOffset + 1U] = static_cast<std::uint8_t>(bal & 0xFFU);
    REQUIRE(u16be_sum(rom, kSumRangeStart, kSumRangeEnd) == kSumTarget);
    return rom;
}

}  // namespace

TEST_CASE("u16be_sum walks pairs as big-endian", "[tune_export]") {
    // 0x12 0x34 0x56 0x78 -> 0x1234 + 0x5678 = 0x68AC
    std::vector<std::uint8_t> b{0x12, 0x34, 0x56, 0x78};
    REQUIRE(u16be_sum(b, 0, static_cast<std::uint32_t>(b.size())) == 0x68ACU);
}

TEST_CASE("u16be_sum wraps mod 2^16", "[tune_export]") {
    // 0xFFFF + 0x0001 wraps to 0x0000.
    std::vector<std::uint8_t> b{0xFF, 0xFF, 0x00, 0x01};
    REQUIRE(u16be_sum(b, 0, static_cast<std::uint32_t>(b.size())) == 0x0000U);
}

TEST_CASE("compute_balance produces the sum-target invariant",
          "[tune_export]") {
    auto rom = make_pattern_rom();
    rom[kBalanceOffset]      = 0;
    rom[kBalanceOffset + 1U] = 0;
    auto const bal           = compute_balance(rom);

    rom[kBalanceOffset]      = static_cast<std::uint8_t>((bal >> 8) & 0xFFU);
    rom[kBalanceOffset + 1U] = static_cast<std::uint8_t>(bal & 0xFFU);

    REQUIRE(u16be_sum(rom, kSumRangeStart, kSumRangeEnd) == kSumTarget);
}

TEST_CASE("validate accepts empty diff list", "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    auto errs = validate(rom, {});
    REQUIRE(errs.empty());
}

TEST_CASE("validate rejects diffs in the FACI-locked range",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x004000U, std::vector<std::uint8_t>{0x12, 0x34, 0x56, 0x78}}};
    auto errs = validate(rom, diffs);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs[0].kind == ValidationError::Kind::FaciLockedAddress);
    REQUIRE(errs[0].offending_offset == 0x004000U);
}

TEST_CASE("validate rejects diffs at or past the WDT trap",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x200000U, std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00}}};
    auto errs = validate(rom, diffs);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs[0].kind == ValidationError::Kind::WdtTrapAddress);
}

TEST_CASE("validate rejects diffs that hit the balance cell directly",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {kBalanceOffset, std::vector<std::uint8_t>{0xFF, 0xFF}}};
    auto errs = validate(rom, diffs);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs[0].kind == ValidationError::Kind::BalanceClobber);
}

TEST_CASE("validate rejects diffs that overlap the preserved trailer",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x1FFFE0U, std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}}};
    auto errs = validate(rom, diffs);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs[0].kind == ValidationError::Kind::TrailerClobber);
}

TEST_CASE("validate rejects diffs that touch boot-integrity bytes",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    SECTION("sig 1 at 0x006000") {
        std::vector<CalDiff> diffs{
            {0x006000U, std::vector<std::uint8_t>{0x00, 0x00}}};
        auto errs = validate(rom, diffs);
        // 0x006000 is in FACI-locked range so FaciLocked fires first.
        // The point is: this diff is refused.
        REQUIRE_FALSE(errs.empty());
    }
    SECTION("sig 2 at 0x1FFFF2 (overlaps trailer too — trailer wins)") {
        // 0x1FFFF2 is in BOTH the trailer reserved range AND the
        // boot-integrity preserved set. Per analyst spec ordering,
        // TrailerClobber is reported (the broader check fires first).
        std::vector<CalDiff> diffs{
            {0x1FFFF2U, std::vector<std::uint8_t>{0x00, 0x00}}};
        auto errs = validate(rom, diffs);
        REQUIRE(errs.size() == 1);
        REQUIRE(errs[0].kind == ValidationError::Kind::TrailerClobber);
    }
    SECTION("sig 1 byte at 0x006C (outside trailer, boot-integrity-only)") {
        // 0x6C is well outside the trailer range but in the boot
        // integrity preserved set. ALSO in the FACI-locked range —
        // FACI fires first.
        std::vector<CalDiff> diffs{
            {0x00006CU, std::vector<std::uint8_t>{0x00}}};
        auto errs = validate(rom, diffs);
        REQUIRE(errs.size() == 1);
        REQUIRE(errs[0].kind == ValidationError::Kind::FaciLockedAddress);
    }
}

TEST_CASE("validate reports all violations, not just the first",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x001000U, std::vector<std::uint8_t>{0xAA}},  // FaciLocked
        {0x300000U, std::vector<std::uint8_t>{0xBB}},  // WdtTrap
        {0x024316U, std::vector<std::uint8_t>{0x41, 0xA0}},  // OK
    };
    auto errs = validate(rom, diffs);
    REQUIRE(errs.size() == 2);
    REQUIRE(errs[0].kind == ValidationError::Kind::FaciLockedAddress);
    REQUIRE(errs[1].kind == ValidationError::Kind::WdtTrapAddress);
}

TEST_CASE("validate accepts a clean writable-range u16 diff",
          "[tune_export][validate]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x024316U,  // Rev Limit A (LF79103P / LF79002P table layout)
         std::vector<std::uint8_t>{0x41, 0xA0}}};  // 7000 RPM
    auto errs = validate(rom, diffs);
    REQUIRE(errs.empty());
}

TEST_CASE("validate detects base size mismatch",
          "[tune_export][validate]") {
    std::vector<std::uint8_t> too_small(0x1000, 0u);
    std::vector<CalDiff> diffs;
    auto errs = validate(too_small, diffs);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs[0].kind == ValidationError::Kind::BaseSizeMismatch);
}

TEST_CASE("build_image preserves the brick-protection sum",
          "[tune_export][build]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x024316U, std::vector<std::uint8_t>{0x41, 0xA0}},
        {0x080000U, std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}}};
    auto r = build_image(rom, diffs);
    REQUIRE(r.has_value());
    auto const &img = *r;
    REQUIRE(img.size() == kWritableEnd);

    // The brick-protection contract: this is the entire point.
    REQUIRE(u16be_sum(img, kSumRangeStart, kSumRangeEnd) == kSumTarget);

    // Diffs actually applied.
    REQUIRE(img[0x024316] == 0x41);
    REQUIRE(img[0x024317] == 0xA0);
    REQUIRE(img[0x080000] == 0xDE);
    REQUIRE(img[0x080003] == 0xEF);

    // Trailer landmarks NOT clobbered (we copied them from base).
    for (std::uint32_t off = kTrailerReservedStart;
         off < kBalanceOffset; ++off) {
        REQUIRE(img[off] == rom[off]);
    }
}

TEST_CASE("build_image rejects bad input via validate",
          "[tune_export][build]") {
    auto rom = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x000000U, std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}}};
    auto r = build_image(rom, diffs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
}

TEST_CASE("emit_write_plan emits only the trailer when no changes",
          "[tune_export][plan]") {
    auto rom  = make_pre_balanced_rom();
    auto plan = emit_write_plan(rom, rom);  // identical input/base
    REQUIRE(plan.size() == 1);
    REQUIRE(plan[0].addr == 0x1FFF00U);
    REQUIRE(plan[0].tag == WriteBlock::Tag::BalanceWrite);
}

TEST_CASE("emit_write_plan skips unchanged blocks",
          "[tune_export][plan]") {
    auto base = make_pre_balanced_rom();
    std::vector<CalDiff> diffs{
        {0x024316U, std::vector<std::uint8_t>{0x41, 0xA0}}};
    auto built = build_image(base, diffs);
    REQUIRE(built.has_value());

    auto plan = emit_write_plan(*built, base);
    // Expect 2 blocks: the EB06 block containing 0x024316, plus
    // the trailer block (balance recomputed).
    REQUIRE(plan.size() == 2);
    REQUIRE(plan[0].tag == WriteBlock::Tag::CalChange);
    REQUIRE(plan[0].addr == (0x024316U & ~(kBlockSize - 1U)));  // 0x024300
    REQUIRE(plan[1].tag == WriteBlock::Tag::BalanceWrite);
    REQUIRE(plan[1].addr == 0x1FFF00U);
}

TEST_CASE("emit_write_plan places the trailer block LAST",
          "[tune_export][plan]") {
    auto base = make_pre_balanced_rom();
    // Force changes spread across many blocks
    std::vector<CalDiff> diffs{
        {0x008000U, std::vector<std::uint8_t>{0xAA}},
        {0x040000U, std::vector<std::uint8_t>{0xBB}},
        {0x100000U, std::vector<std::uint8_t>{0xCC}},
    };
    auto built = build_image(base, diffs);
    REQUIRE(built.has_value());

    auto plan = emit_write_plan(*built, base);
    REQUIRE_FALSE(plan.empty());
    // Last entry is the trailer
    REQUIRE(plan.back().addr == 0x1FFF00U);
    REQUIRE(plan.back().tag == WriteBlock::Tag::BalanceWrite);
    // All other entries are CalChange and in ascending address order
    for (std::size_t i = 1; i + 1U < plan.size(); ++i) {
        REQUIRE(plan[i - 1U].addr < plan[i].addr);
        REQUIRE(plan[i - 1U].tag == WriteBlock::Tag::CalChange);
    }
}

TEST_CASE("emit_write_plan returns empty on wrong-size input",
          "[tune_export][plan]") {
    std::vector<std::uint8_t> too_small(0x1000, 0u);
    auto good = make_pre_balanced_rom();
    REQUIRE(emit_write_plan(too_small, good).empty());
    REQUIRE(emit_write_plan(good, too_small).empty());
}

// ---------------------------------------------------------------------------
// Flash-ready checksum state + balance repair (task 47 §ROM-editing #4).
// These distinguish "flash-ready checksum" (the ECU brick-protection sum
// contract) from project integrity, and prove repair_balance re-tunes only
// the compensating word.
// ---------------------------------------------------------------------------

TEST_CASE("checksum_state classifies flash-ready vs needs-repair vs N/A",
          "[tune_export][checksum]") {
    auto pattern = make_pattern_rom();
    REQUIRE(checksum_state(pattern) == ChecksumState::NeedsRepair);

    auto balanced = make_pre_balanced_rom();
    REQUIRE(checksum_state(balanced) == ChecksumState::FlashReady);
    REQUIRE(flash_checksum(balanced) == kSumTarget);

    std::vector<std::uint8_t> const small(64, 0u);
    REQUIRE(checksum_state(small) == ChecksumState::NotApplicable);
}

TEST_CASE("repair_balance makes a ROM flash-ready and is idempotent",
          "[tune_export][checksum]") {
    auto rom = make_pattern_rom();
    REQUIRE(checksum_state(rom) == ChecksumState::NeedsRepair);

    REQUIRE(repair_balance(rom).has_value());
    REQUIRE(checksum_state(rom) == ChecksumState::FlashReady);
    REQUIRE(flash_checksum(rom) == kSumTarget);

    auto const before = rom;
    REQUIRE(repair_balance(rom).has_value());
    REQUIRE(rom == before); // idempotent — no bytes change
    REQUIRE(checksum_state(rom) == ChecksumState::FlashReady);
}

TEST_CASE("repair_balance preserves cal edits and only touches the balance cell",
          "[tune_export][checksum]") {
    auto rom = make_pre_balanced_rom();
    std::size_t const edit_addr = 0x100000; // inside the summed window
    rom[edit_addr] = static_cast<std::uint8_t>(rom[edit_addr] ^ 0xFFu);
    REQUIRE(checksum_state(rom) == ChecksumState::NeedsRepair);

    auto const edited = rom;
    REQUIRE(repair_balance(rom).has_value());
    REQUIRE(checksum_state(rom) == ChecksumState::FlashReady);
    REQUIRE(rom[edit_addr] == edited[edit_addr]); // cal edit survived

    bool only_balance_changed = true;
    for (std::size_t i = 0; i < rom.size(); ++i) {
        if (i == kBalanceOffset || i == kBalanceOffset + 1) {
            continue;
        }
        if (rom[i] != edited[i]) {
            only_balance_changed = false;
            break;
        }
    }
    REQUIRE(only_balance_changed);
}

TEST_CASE("repair_balance rejects a non-2MB image", "[tune_export][checksum]") {
    std::vector<std::uint8_t> small(1024, 0u);
    auto const r = repair_balance(small);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}
