// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/inserter.hpp"

#include "st/feature_patch/splice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace fp = st::feature_patch;
namespace cg = st::feature::codegen;

namespace {

// Build a synthetic 64 KB ROM filled with `0xCC` so any byte that's
// changed by the inserter is visually obvious in test failures.
std::vector<std::uint8_t> make_synthetic_rom(std::size_t size = 0x10000) {
    return std::vector<std::uint8_t>(size, 0xCC);
}

// A 1-hook PatchObject with arbitrary code bytes and a splice
// address. arch is parameterized so we can test both ISAs from the
// same scaffold.
cg::PatchObject make_one_hook_patch(cg::Arch arch, std::size_t splice_addr,
                                    std::vector<std::uint8_t> code) {
    cg::HookPatch hp;
    hp.symbol = "test_hook";
    hp.splice_address = splice_addr;
    hp.code = std::move(code);
    cg::PatchObject p;
    p.arch = arch;
    p.hooks.push_back(std::move(hp));
    return p;
}

} // namespace

TEST_CASE("PatchInserter::apply rejects Unknown arch",
          "[feature_patch][inserter][error]") {
    auto patch = make_one_hook_patch(cg::Arch::Unknown, 0x1000, {0x00, 0x09});
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x8000, 0x100}}});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("PatchInserter::apply rejects empty writable regions",
          "[feature_patch][inserter][error]") {
    auto patch = make_one_hook_patch(cg::Arch::Sh2a, 0x1000, {0x00, 0x09});
    auto r = fp::apply(patch, make_synthetic_rom(), {});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("PatchInserter::apply writes SH-2A patch code + splice + manifest",
          "[feature_patch][inserter][sh2a]") {
    // Splice at 0x1000, target patch region at 0x8000. 0x7000 byte
    // gap is well past BRA range — but the inserter allocates the
    // patch at 0x8000 (the region's base), and the splice will need
    // long-form. To exercise short-form here, place the writable
    // region close to the splice address.
    auto patch = make_one_hook_patch(cg::Arch::Sh2a, 0x1000,
                                     {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE});
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x1100, 0x100}}});
    INFO("apply error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());

    auto const &out = *r;
    REQUIRE(out.bytes.size() == 0x10000);
    REQUIRE(out.tuner_tag == "SUBU");

    // Patch code lands at the region base (4-aligned, no prior claims).
    REQUIRE(out.bytes[0x1100] == 0xDE);
    REQUIRE(out.bytes[0x1101] == 0xAD);
    REQUIRE(out.bytes[0x1107] == 0xBE);

    // Splice at 0x1000: BRA disp12 to 0x1100 + NOP. PC=0x1000, target=0x1100,
    // signed_gap = 0x100 - 4 = 0xFC = 252. disp = 252/2 = 126 = 0x7E.
    // BRA encoding: 0xA000 | 0x07E = 0xA07E (BE: A0 7E). NOP: 00 09.
    REQUIRE(out.bytes[0x1000] == 0xA0);
    REQUIRE(out.bytes[0x1001] == 0x7E);
    REQUIRE(out.bytes[0x1002] == 0x00);
    REQUIRE(out.bytes[0x1003] == 0x09);

    // Manifest has two records — the patch code + the splice site.
    REQUIRE(out.manifest.size() == 2);
    bool found_code = false;
    bool found_splice = false;
    for (auto const &rec : out.manifest) {
        if (rec.target_address == 0x1100 && rec.length_bytes == 8)
            found_code = true;
        if (rec.target_address == 0x1000 && rec.length_bytes == 4)
            found_splice = true;
    }
    REQUIRE(found_code);
    REQUIRE(found_splice);

    // Manifest itself was written to ROM after the patch code (at the
    // next 4-aligned offset). Decoding the bytes there should reproduce
    // the manifest list.
    auto const manifest_byte_len =
        static_cast<std::ptrdiff_t>((out.manifest.size() + 1) * fp::kRecordSize);
    std::vector<std::uint8_t> manifest_slice(
        out.bytes.begin() + static_cast<std::ptrdiff_t>(out.manifest_address),
        out.bytes.begin() + static_cast<std::ptrdiff_t>(out.manifest_address) +
            manifest_byte_len);
    auto decoded = fp::decode(manifest_slice);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == out.manifest.size());
}

TEST_CASE("PatchInserter::apply writes RH850 patch code + splice + manifest",
          "[feature_patch][inserter][rh850]") {
    auto patch = make_one_hook_patch(cg::Arch::Rh850, 0x1000,
                                     {0x00, 0x10, 0x20, 0x30}); // 4 placeholder bytes
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x2000, 0x200}}});
    INFO("apply error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());

    auto const &out = *r;
    // Patch lands at 0x2000.
    REQUIRE(out.bytes[0x2000] == 0x00);
    REQUIRE(out.bytes[0x2001] == 0x10);
    REQUIRE(out.bytes[0x2002] == 0x20);
    REQUIRE(out.bytes[0x2003] == 0x30);

    // Splice at 0x1000: JR to 0x2000. disp22 = 0x2000 - 0x1000 = 0x1000.
    // hw1 = 0x0780 | ((0x1000 >> 16) & 0x3F) = 0x0780.
    // hw2 = 0x1000 & 0xFFFE = 0x1000.
    // LE on the wire: 80 07 00 10.
    REQUIRE(out.bytes[0x1000] == 0x80);
    REQUIRE(out.bytes[0x1001] == 0x07);
    REQUIRE(out.bytes[0x1002] == 0x00);
    REQUIRE(out.bytes[0x1003] == 0x10);
}

TEST_CASE("PatchInserter::apply surfaces ROM allocator exhaustion",
          "[feature_patch][inserter][error][exhaustion]") {
    // 64-byte writable region, but the hook needs 128.
    auto patch = make_one_hook_patch(cg::Arch::Sh2a, 0x1000,
                                     std::vector<std::uint8_t>(128, 0xAA));
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x1100, 0x40}}});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
    auto const &msg = r.error().to_string();
    REQUIRE(msg.find("test_hook") != std::string::npos);
    REQUIRE(msg.find("128") != std::string::npos);
}

TEST_CASE("PatchInserter::apply uses long-form splice for far targets",
          "[feature_patch][inserter][long-form]") {
    // SH-2A: splice at 0x1000, region at 0x8000 — 0x7000 gap, past
    // BRA's ±~4 KB. Long-form 12-byte sequence takes over (MOV.L +
    // JMP @R0 + delay-slot NOP + pad NOP + inline 4-byte literal).
    auto patch = make_one_hook_patch(cg::Arch::Sh2a, 0x1000, {0x00, 0x09});
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x8000, 0x100}}});
    INFO("apply error: "
         << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    // Splice bytes at 0x1000: MOV.L @(1,PC), R0 = 0xD001 (BE: D0 01).
    REQUIRE(r->bytes[0x1000] == 0xD0);
    REQUIRE(r->bytes[0x1001] == 0x01);
    // JMP @R0 = 0x402B (BE: 40 2B).
    REQUIRE(r->bytes[0x1002] == 0x40);
    REQUIRE(r->bytes[0x1003] == 0x2B);
    // Inline literal at splice+8 = patch_address = 0x8000.
    REQUIRE(r->bytes[0x1008] == 0x00);
    REQUIRE(r->bytes[0x1009] == 0x00);
    REQUIRE(r->bytes[0x100A] == 0x80);
    REQUIRE(r->bytes[0x100B] == 0x00);
}

TEST_CASE("PatchInserter::apply handles multi-hook patches",
          "[feature_patch][inserter][multi-hook]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    {
        cg::HookPatch h1;
        h1.symbol = "hook_a";
        h1.splice_address = 0x1000;
        h1.code = {0xAA, 0xAA, 0xBB, 0xBB};
        p.hooks.push_back(std::move(h1));
    }
    {
        cg::HookPatch h2;
        h2.symbol = "hook_b";
        h2.splice_address = 0x1100;
        h2.code = {0xCC, 0xCC, 0xDD, 0xDD, 0xEE, 0xEE, 0xFF, 0xFF};
        p.hooks.push_back(std::move(h2));
    }

    auto r = fp::apply(p, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x1200, 0x100}}});
    INFO("apply error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    auto const &out = *r;

    // Hook A: 4 bytes at 0x1200.
    REQUIRE(out.bytes[0x1200] == 0xAA);
    REQUIRE(out.bytes[0x1203] == 0xBB);
    // Hook B: 8 bytes at 0x1204 (4-aligned after hook A).
    REQUIRE(out.bytes[0x1204] == 0xCC);
    REQUIRE(out.bytes[0x120B] == 0xFF);

    // 2 hooks × 2 records each (code + splice) = 4 manifest entries.
    REQUIRE(out.manifest.size() == 4);
}

TEST_CASE("PatchInserter::apply skips zero-byte hooks benignly",
          "[feature_patch][inserter][edge]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    cg::HookPatch h;
    h.symbol = "empty";
    h.splice_address = 0x1000;
    // h.code is empty — backend may emit this for dead-store cases.
    p.hooks.push_back(std::move(h));

    auto r = fp::apply(p, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x2000, 0x100}}});
    REQUIRE(r.has_value());
    // No patch code, no splice — manifest has only the trailer record
    // for the empty bookkeeping list.
    REQUIRE(r->manifest.empty());
    // ROM bytes unchanged at splice + region except for the manifest
    // bytes (which are zero — encode of empty records is just the
    // terminator).
    REQUIRE(r->bytes[0x1000] == 0xCC); // splice site untouched
    REQUIRE(r->bytes[0x2000] == 0x00); // manifest terminator landed here
}

TEST_CASE("PatchInserter::apply respects custom tuner_tag",
          "[feature_patch][inserter][tuner_tag]") {
    auto patch = make_one_hook_patch(cg::Arch::Sh2a, 0x1000, {0xAB, 0xCD});
    auto r = fp::apply(patch, make_synthetic_rom(),
                       {{fp::RomRegion{"code", 0x1100, 0x100}}}, "TEST");
    REQUIRE(r.has_value());
    REQUIRE(r->tuner_tag == "TEST");
}
