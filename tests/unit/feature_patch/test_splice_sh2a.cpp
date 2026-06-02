// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/splice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace fp = st::feature_patch;

TEST_CASE("sh2a_bra_disp12 returns the signed halfword displacement",
          "[feature_patch][splice_sh2a][bra]") {
    // Forward jump of 8 bytes from PC: target = PC + 8.
    // BRA target = PC + 4 + disp*2 ⇒ disp = (8 - 4) / 2 = 2.
    REQUIRE(fp::sh2a_bra_disp12(0x10000, 0x10008).value() == 2);

    // Branch to self: PC -> PC. disp = (0 - 4) / 2 = -2 (infinite loop).
    REQUIRE(fp::sh2a_bra_disp12(0x10000, 0x10000).value() == -2);

    // Backward jump of 4 bytes: target = PC - 4. disp = (-4 - 4) / 2 = -4.
    REQUIRE(fp::sh2a_bra_disp12(0x10000, 0x0FFFC).value() == -4);
}

TEST_CASE("sh2a_bra_disp12 returns nullopt for unaligned addresses",
          "[feature_patch][splice_sh2a][bra][error]") {
    // Odd target: signed_gap = 7 - 4 = 3, not divisible by 2.
    REQUIRE_FALSE(fp::sh2a_bra_disp12(0x10000, 0x10007).has_value());
    // Odd splice + even target: signed_gap = 8 - 4 = 4, divisible by 2,
    // but BRA from an odd address is itself invalid (the encoder
    // doesn't catch this — the splice wrapper does).
    // Confirmed via emit_sh2a_splice's separate alignment check.
}

TEST_CASE("sh2a_bra_disp12 returns nullopt at the range boundaries",
          "[feature_patch][splice_sh2a][bra][boundary]") {
    // Forward max: disp = 2047 → target = PC + 4 + 4094 = PC + 4098.
    REQUIRE(fp::sh2a_bra_disp12(0x10000, 0x10000 + 4098).value() == 2047);
    // One beyond max: target = PC + 4100, disp = 2048, doesn't fit in 12-bit signed.
    REQUIRE_FALSE(fp::sh2a_bra_disp12(0x10000, 0x10000 + 4100).has_value());

    // Backward min: disp = -2048 → target = PC + 4 - 4096 = PC - 4092.
    REQUIRE(fp::sh2a_bra_disp12(0x10000, 0x10000 - 4092).value() == -2048);
    // One beyond min: target = PC - 4094, disp = -2049, doesn't fit.
    REQUIRE_FALSE(fp::sh2a_bra_disp12(0x10000, 0x10000 - 4094).has_value());
}

TEST_CASE("emit_sh2a_splice short form emits BRA + NOP for in-range targets",
          "[feature_patch][splice_sh2a][emit]") {
    // splice at 0x00010000, patch at 0x00010008 (8 bytes forward).
    // BRA disp = 2 → encoding 0xA002. NOP = 0x0009.
    // Big-endian byte sequence: A0 02 00 09.
    auto r = fp::emit_sh2a_splice(0x00010000, 0x00010008);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Sh2aShortBra);
    REQUIRE(r->splice_address == 0x00010000);
    REQUIRE(r->patch_address == 0x00010008);
    REQUIRE(r->bytes.size() == 4);
    REQUIRE(r->bytes[0] == 0xA0);
    REQUIRE(r->bytes[1] == 0x02);
    REQUIRE(r->bytes[2] == 0x00);
    REQUIRE(r->bytes[3] == 0x09);
}

TEST_CASE("emit_sh2a_splice handles backward branches",
          "[feature_patch][splice_sh2a][emit]") {
    // splice at 0x00010000, patch at 0x0000FFFC (4 bytes back).
    // BRA disp = -4 → encoding 0xA000 | (0xFFC & 0xFFF) = 0xAFFC.
    auto r = fp::emit_sh2a_splice(0x00010000, 0x0000FFFC);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Sh2aShortBra);
    REQUIRE(r->bytes[0] == 0xAF);
    REQUIRE(r->bytes[1] == 0xFC);
    REQUIRE(r->bytes[2] == 0x00); // NOP delay slot
    REQUIRE(r->bytes[3] == 0x09);
}

TEST_CASE("emit_sh2a_splice surfaces NotImplemented for out-of-range targets",
          "[feature_patch][splice_sh2a][emit][long-form]") {
    // 64 KB forward — well past BRA's 4 KB reach.
    auto r = fp::emit_sh2a_splice(0x00010000, 0x00020000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
    auto const &msg = r.error().to_string();
    // Error should name the actual displacement + the BRA range.
    REQUIRE(msg.find("displacement") != std::string::npos);
    REQUIRE(msg.find("BRA range") != std::string::npos);
    REQUIRE(msg.find("long-form") != std::string::npos);
}

TEST_CASE("emit_sh2a_splice rejects unaligned splice or patch addresses",
          "[feature_patch][splice_sh2a][emit][error]") {
    auto r1 = fp::emit_sh2a_splice(0x00010001, 0x00010008);
    REQUIRE_FALSE(r1.has_value());
    REQUIRE(r1.error().code() == st::ErrorCode::InvalidArgument);

    auto r2 = fp::emit_sh2a_splice(0x00010000, 0x00010009);
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("emit_sh2a_splice handles BRA range boundaries",
          "[feature_patch][splice_sh2a][emit][boundary]") {
    // Forward max: 4098 bytes ahead — last in-range target.
    auto r_max = fp::emit_sh2a_splice(0x00010000, 0x00010000 + 4098);
    REQUIRE(r_max.has_value());
    REQUIRE(r_max->form == fp::SpliceForm::Sh2aShortBra);
    // Encoding: 0xA000 | (2047 & 0xFFF) = 0xA7FF.
    REQUIRE(r_max->bytes[0] == 0xA7);
    REQUIRE(r_max->bytes[1] == 0xFF);

    // 2 bytes past: long form (NotImplemented in this slice).
    auto r_past = fp::emit_sh2a_splice(0x00010000, 0x00010000 + 4100);
    REQUIRE_FALSE(r_past.has_value());
    REQUIRE(r_past.error().code() == st::ErrorCode::NotImplemented);

    // Backward min: 4092 bytes back — last in-range target.
    auto r_min = fp::emit_sh2a_splice(0x00010000, 0x00010000 - 4092);
    REQUIRE(r_min.has_value());
    // Encoding: 0xA000 | (-2048 & 0xFFF) = 0xA000 | 0x800 = 0xA800.
    REQUIRE(r_min->bytes[0] == 0xA8);
    REQUIRE(r_min->bytes[1] == 0x00);
}

TEST_CASE("emit_sh2a_splice branch-to-self produces a tight loop",
          "[feature_patch][splice_sh2a][emit][edge]") {
    // Edge but defensible: BRA -2 (target = PC + 4 + (-2)*2 = PC) makes
    // an infinite loop. The splice helper allows it; the caller is
    // responsible for not requesting nonsense.
    auto r = fp::emit_sh2a_splice(0x00010000, 0x00010000);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Sh2aShortBra);
    // disp = -2 → encoding 0xA000 | (0xFFE & 0xFFF) = 0xAFFE.
    REQUIRE(r->bytes[0] == 0xAF);
    REQUIRE(r->bytes[1] == 0xFE);
}
