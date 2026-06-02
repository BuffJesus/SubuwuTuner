// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/splice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace fp = st::feature_patch;

TEST_CASE("rh850_jr_disp22 returns the signed byte displacement",
          "[feature_patch][splice_rh850][jr]") {
    // Forward jump of 16 bytes: target = PC + 16. disp22 = 16.
    REQUIRE(fp::rh850_jr_disp22(0x10000, 0x10010).value() == 16);

    // Branch to self: disp22 = 0 (single-instruction infinite loop).
    REQUIRE(fp::rh850_jr_disp22(0x10000, 0x10000).value() == 0);

    // Backward jump of 8 bytes: target = PC - 8. disp22 = -8.
    REQUIRE(fp::rh850_jr_disp22(0x10000, 0x0FFF8).value() == -8);
}

TEST_CASE("rh850_jr_disp22 returns nullopt for unaligned displacements",
          "[feature_patch][splice_rh850][jr][error]") {
    // Odd target relative to even splice: signed_gap = 1, odd → nullopt.
    REQUIRE_FALSE(fp::rh850_jr_disp22(0x10000, 0x10001).has_value());
    REQUIRE_FALSE(fp::rh850_jr_disp22(0x10000, 0x10003).has_value());
}

TEST_CASE("rh850_jr_disp22 returns nullopt at the range boundaries",
          "[feature_patch][splice_rh850][jr][boundary]") {
    // Forward max: +2097150 bytes — last in-range.
    REQUIRE(fp::rh850_jr_disp22(0x10000, 0x10000 + 2097150).value() == 2097150);
    // One step past: +2097152, falls out (next even value).
    REQUIRE_FALSE(fp::rh850_jr_disp22(0x10000, 0x10000 + 2097152).has_value());

    // Backward min: -2097152.
    REQUIRE(fp::rh850_jr_disp22(0x10000 + 0x300000U, (0x10000 + 0x300000U) - 2097152U)
                .value() == -2097152);
    // One step past: -2097154, out of range.
    REQUIRE_FALSE(fp::rh850_jr_disp22(0x10000 + 0x300000U,
                                      (0x10000 + 0x300000U) - 2097154U)
                      .has_value());
}

TEST_CASE("emit_rh850_splice short form emits JR for in-range targets",
          "[feature_patch][splice_rh850][emit]") {
    // splice at 0x00010000, patch at 0x00010010 (16 bytes forward).
    // disp22 = 16. Encoding (little-endian on the wire):
    //   hw1 = 0x0780 | ((16 >> 16) & 0x3F) = 0x0780
    //   hw2 = 16 & 0xFFFE = 0x0010
    // LE bytes: 80 07 10 00
    auto r = fp::emit_rh850_splice(0x00010000, 0x00010010);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Rh850ShortJr);
    REQUIRE(r->splice_address == 0x00010000);
    REQUIRE(r->patch_address == 0x00010010);
    REQUIRE(r->bytes.size() == 4);
    REQUIRE(r->bytes[0] == 0x80);
    REQUIRE(r->bytes[1] == 0x07);
    REQUIRE(r->bytes[2] == 0x10);
    REQUIRE(r->bytes[3] == 0x00);
}

TEST_CASE("emit_rh850_splice handles backward branches",
          "[feature_patch][splice_rh850][emit]") {
    // splice at 0x00020000, patch at 0x0001FFF0 (16 bytes back).
    // disp22 = -16 = 0xFFFFFFF0 (in 32-bit two's-complement).
    // hw1 high 6 bits of disp22[21:16] = 0x3F (all bits set, since
    //   -16 sign-extends as all-1 above the low byte). 0x0780 | 0x3F = 0x07BF.
    // hw2 = 0xFFF0 & 0xFFFE = 0xFFF0.
    auto r = fp::emit_rh850_splice(0x00020000, 0x0001FFF0);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Rh850ShortJr);
    REQUIRE(r->bytes[0] == 0xBF);
    REQUIRE(r->bytes[1] == 0x07);
    REQUIRE(r->bytes[2] == 0xF0);
    REQUIRE(r->bytes[3] == 0xFF);
}

TEST_CASE("emit_rh850_splice branch-to-self is valid and stable",
          "[feature_patch][splice_rh850][emit][edge]") {
    // disp22 = 0 → JR jumps to itself (infinite loop). hw1 = 0x0780,
    // hw2 = 0x0000.
    auto r = fp::emit_rh850_splice(0x00010000, 0x00010000);
    REQUIRE(r.has_value());
    REQUIRE(r->form == fp::SpliceForm::Rh850ShortJr);
    REQUIRE(r->bytes[0] == 0x80);
    REQUIRE(r->bytes[1] == 0x07);
    REQUIRE(r->bytes[2] == 0x00);
    REQUIRE(r->bytes[3] == 0x00);
}

TEST_CASE("emit_rh850_splice surfaces NotImplemented for out-of-range targets",
          "[feature_patch][splice_rh850][emit][long-form]") {
    // 4 MB forward — past JR's ±2 MB reach.
    auto r = fp::emit_rh850_splice(0x00010000, 0x00410000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
    auto const &msg = r.error().to_string();
    REQUIRE(msg.find("displacement") != std::string::npos);
    REQUIRE(msg.find("JR range") != std::string::npos);
    REQUIRE(msg.find("long-form") != std::string::npos);
}

TEST_CASE("emit_rh850_splice rejects unaligned splice or patch addresses",
          "[feature_patch][splice_rh850][emit][error]") {
    auto r1 = fp::emit_rh850_splice(0x00010001, 0x00010010);
    REQUIRE_FALSE(r1.has_value());
    REQUIRE(r1.error().code() == st::ErrorCode::InvalidArgument);

    auto r2 = fp::emit_rh850_splice(0x00010000, 0x00010011);
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("emit_rh850_splice exercises the JR boundary cases",
          "[feature_patch][splice_rh850][emit][boundary]") {
    // Forward max: +2097150 bytes — last in-range target.
    auto r_max = fp::emit_rh850_splice(0x00010000, 0x00010000 + 2097150);
    REQUIRE(r_max.has_value());
    REQUIRE(r_max->form == fp::SpliceForm::Rh850ShortJr);

    // 2 bytes past: long form (NotImplemented in this slice).
    auto r_past = fp::emit_rh850_splice(0x00010000, 0x00010000 + 2097152);
    REQUIRE_FALSE(r_past.has_value());
    REQUIRE(r_past.error().code() == st::ErrorCode::NotImplemented);
}
