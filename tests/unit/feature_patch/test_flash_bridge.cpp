// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/flash_bridge.hpp"

#include "st/feature_patch/inserter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace fp = st::feature_patch;
namespace cg = st::feature::codegen;

namespace {

std::vector<std::uint8_t> make_filled_rom(std::size_t size, std::uint8_t fill) {
    return std::vector<std::uint8_t>(size, fill);
}

cg::PatchObject one_hook(cg::Arch arch, std::size_t splice_addr,
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

TEST_CASE("build_flash_plan rejects size mismatch",
          "[feature_patch][flash_bridge][error]") {
    auto src = make_filled_rom(0x1000, 0xFF);
    fp::PatchedRom p;
    p.bytes = make_filled_rom(0x2000, 0xFF);
    auto r = fp::build_flash_plan(src, p);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_flash_plan rejects zero sector size",
          "[feature_patch][flash_bridge][error]") {
    auto src = make_filled_rom(0x1000, 0xFF);
    fp::PatchedRom p;
    p.bytes = src;
    auto r = fp::build_flash_plan(src, p, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("build_flash_plan emits no writes for an unchanged ROM",
          "[feature_patch][flash_bridge]") {
    auto src = make_filled_rom(0x4000, 0xAB);
    fp::PatchedRom p;
    p.bytes = src; // identical
    auto r = fp::build_flash_plan(src, p, 0x1000);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.empty());
}

TEST_CASE("build_flash_plan emits one SectorWrite per differing sector",
          "[feature_patch][flash_bridge]") {
    auto src = make_filled_rom(0x4000, 0xFF);
    fp::PatchedRom p;
    p.bytes = src;

    // Change one byte in sector 0 (0x0000..0x0FFF) and one in sector 2
    // (0x2000..0x2FFF). Sector 1 + 3 untouched.
    p.bytes[0x0010] = 0xDE;
    p.bytes[0x2100] = 0xAD;

    auto r = fp::build_flash_plan(src, p, 0x1000);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 2);

    // First write: sector 0 (addr 0x0000, length 0x1000).
    REQUIRE(r->writes[0].sector.address == 0x0000);
    REQUIRE(r->writes[0].sector.length == 0x1000);
    REQUIRE(r->writes[0].data.size() == 0x1000);
    REQUIRE(r->writes[0].data[0x0010] == 0xDE);

    // Second write: sector 2 (addr 0x2000, length 0x1000).
    REQUIRE(r->writes[1].sector.address == 0x2000);
    REQUIRE(r->writes[1].sector.length == 0x1000);
    REQUIRE(r->writes[1].data[0x0100] == 0xAD);
}

TEST_CASE("build_flash_plan respects base_address offset",
          "[feature_patch][flash_bridge]") {
    // Buffer represents flash at 0x80000..0x80fff. Edit byte at
    // local offset 0x100 → ECU address 0x80100, sector 0x80000.
    auto src = make_filled_rom(0x2000, 0xFF);
    fp::PatchedRom p;
    p.bytes = src;
    p.bytes[0x100] = 0xCA;
    p.bytes[0x1080] = 0xFE;

    auto r = fp::build_flash_plan(src, p, 0x1000, 0x80000);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 2);
    REQUIRE(r->writes[0].sector.address == 0x80000);
    REQUIRE(r->writes[0].data[0x100] == 0xCA);
    REQUIRE(r->writes[1].sector.address == 0x81000);
    REQUIRE(r->writes[1].data[0x80] == 0xFE);
}

TEST_CASE("build_flash_plan end-to-end after PatchInserter::apply",
          "[feature_patch][flash_bridge][end-to-end]") {
    // Full pipeline: compile a synthetic 1-hook PatchObject, apply
    // it to a 64KB ROM, build a FlashPlan from source vs patched,
    // confirm the plan covers exactly the sectors that the inserter
    // touched.
    auto src = make_filled_rom(0x10000, 0xFF);

    auto patch = one_hook(cg::Arch::Sh2a, 0x1000,
                          {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE});
    auto patched = fp::apply(patch, src, {{fp::RomRegion{"code", 0x1100, 0x100}}});
    INFO("apply error: "
         << (patched.has_value() ? std::string{"(none)"} : patched.error().to_string()));
    REQUIRE(patched.has_value());

    auto plan = fp::build_flash_plan(src, *patched, 0x1000);
    REQUIRE(plan.has_value());

    // Both the splice (0x1000) and the patch code + manifest (0x1100+)
    // land in the same 0x1000-sized sector at 0x1000. Single write.
    REQUIRE(plan->writes.size() == 1);
    REQUIRE(plan->writes[0].sector.address == 0x1000);
    REQUIRE(plan->writes[0].sector.length == 0x1000);

    // Confirm the splice bytes survived through to the plan: BRA disp12
    // = 0x7E at the splice address → 0xA07E in BE, NOP = 0x0009.
    REQUIRE(plan->writes[0].data[0] == 0xA0);
    REQUIRE(plan->writes[0].data[1] == 0x7E);
    REQUIRE(plan->writes[0].data[2] == 0x00);
    REQUIRE(plan->writes[0].data[3] == 0x09);
}
