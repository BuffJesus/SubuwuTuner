// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Address-gate tests — v1.0 ship blocker #3 in docs/04-roadmap.md.
// `st::feature::codegen::gate_patch` is the security boundary that refuses
// to emit a PatchObject targeting flash addresses outside the loaded
// Definition's declared writable regions. See docs/16 §Safety #6 for the
// rationale, docs/11 §writable_region for the schema.
//
// These tests construct PatchObjects + Definitions directly rather than
// going through the SH-2A backend; the backend-integration coverage lives
// in test_sh2a.cpp (whose test packs now carry `[[writable_region]]`
// entries). The split keeps the gate's contract testable in isolation
// from any specific backend's emission behavior.

#include "st/defs.hpp"
#include "st/feature_codegen.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cg = st::feature::codegen;

namespace {

// Build a Definition with `n_regions` writable regions. Each region is
// 256 bytes wide, starting at 0x10000 + i * 0x1000 — disjoint, ordered.
st::Definition make_def_with_regions(std::size_t n_regions) {
    std::string toml = "[pack]\nschema_version = 1\nid = \"gate-test\"\nendianness = \"big\"\n";
    for (std::size_t i = 0; i < n_regions; ++i) {
        toml.append("\n[[writable_region]]\nname = \"r");
        toml.append(std::to_string(i));
        toml.append("\"\nkind = \"calibration\"\naddress = ");
        toml.append(std::to_string(0x10000 + i * 0x1000));
        toml.append("\nlength = 256\n");
    }
    auto def = st::Definition::from_toml_string(toml);
    REQUIRE(def.has_value());
    return std::move(*def);
}

// One-hook PatchObject at `splice_address` with `code_size` bytes of
// dummy content. Symbol is "test_hook" by default — the gate doesn't
// inspect it beyond echoing it in error messages.
cg::PatchObject make_patch(std::size_t splice_address, std::size_t code_size,
                           std::string_view symbol = "test_hook") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    cg::HookPatch hp;
    hp.symbol = std::string{symbol};
    hp.splice_address = splice_address;
    hp.code.assign(code_size, 0x00);
    p.hooks.push_back(std::move(hp));
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Vacuous-pass cases — no work to gate.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: empty patch passes against a pack with regions",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(1);
    cg::PatchObject p;
    REQUIRE(cg::gate_patch(p, def).has_value());
}

TEST_CASE("gate_patch: empty patch passes against a pack with NO regions (no work to gate)",
          "[feature_codegen][address_gate]") {
    // The fail-closed rule says "no regions declared, hooks present →
    // fail". An empty patch has nothing to fail on, so the gate is
    // vacuously satisfied — matches the "empty patch = no-op flash"
    // semantic the SH-2A backend produces for an empty module.
    auto const def = make_def_with_regions(0);
    cg::PatchObject p;
    REQUIRE(cg::gate_patch(p, def).has_value());
}

// ---------------------------------------------------------------------------
// Fail-closed: no declared regions + at least one hook → refuse.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: pack with no writable_region rejects any patch with hooks",
          "[feature_codegen][address_gate][fail_closed]") {
    auto const def = make_def_with_regions(0);
    auto const p = make_patch(0x10000, 20);
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    // The error message names the count of hooks and points to the
    // schema doc so the user can fix the pack.
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("no [[writable_region]]") != std::string::npos);
    REQUIRE(msg.find("docs/11") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Containment — happy path.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: hook fully inside a single region passes",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(1);
    // Region r0 = [0x10000, 0x10100). Hook at 0x10010 with 16 bytes
    // → [0x10010, 0x10020) ⊂ r0.
    auto const p = make_patch(0x10010, 16);
    REQUIRE(cg::gate_patch(p, def).has_value());
}

TEST_CASE("gate_patch: hook at the very start of a region passes",
          "[feature_codegen][address_gate][boundary]") {
    auto const def = make_def_with_regions(1);
    auto const p = make_patch(0x10000, 16); // splice == r0.address
    REQUIRE(cg::gate_patch(p, def).has_value());
}

TEST_CASE("gate_patch: hook ending exactly at region end passes",
          "[feature_codegen][address_gate][boundary]") {
    auto const def = make_def_with_regions(1);
    // r0 = [0x10000, 0x10100). Code = 16 bytes ending at 0x10100.
    auto const p = make_patch(0x100F0, 16);
    REQUIRE(cg::gate_patch(p, def).has_value());
}

// ---------------------------------------------------------------------------
// Containment — out-of-region.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: hook splice before any region is rejected",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(1);
    auto const p = make_patch(0x0F000, 16); // before r0
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("test_hook") != std::string::npos);
}

TEST_CASE("gate_patch: hook splice after all regions is rejected",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(1);
    auto const p = make_patch(0x20000, 16);
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("gate_patch: hook starts in region but code overflows the end is rejected",
          "[feature_codegen][address_gate][boundary]") {
    auto const def = make_def_with_regions(1);
    // r0 = [0x10000, 0x10100). Splice at 0x100F0 + 32-byte code spans
    // past the region end (0x10110 > 0x10100). Refused.
    auto const p = make_patch(0x100F0, 32);
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Containment — spanning two regions.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: hook spanning two adjacent regions is rejected",
          "[feature_codegen][address_gate]") {
    // Two regions: r0 = [0x10000, 0x10100), r1 = [0x10100, 0x10200).
    // They cover [0x10000, 0x10200) as a union, but the gate refuses a
    // hook that crosses the boundary — bridging two declared-safe zones
    // is exactly the foot-gun the gate exists to prevent.
    std::string toml = "[pack]\nschema_version = 1\nid = \"adj\"\nendianness = \"big\"\n";
    toml.append("\n[[writable_region]]\nname=\"a\"\nkind=\"calibration\"\naddress="
                "65536\nlength=256\n");
    toml.append("\n[[writable_region]]\nname=\"b\"\nkind=\"calibration\"\naddress="
                "65792\nlength=256\n");
    auto def = st::Definition::from_toml_string(toml);
    REQUIRE(def.has_value());

    // Splice at 0x100F8 + 16 bytes = [0x100F8, 0x10108): crosses
    // r0/r1 boundary at 0x10100.
    auto const p = make_patch(0x100F8, 16);
    auto const r = cg::gate_patch(p, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Multi-region: hook fits in any single region passes.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: hook fits in a non-first region of a multi-region pack",
          "[feature_codegen][address_gate]") {
    // 3 regions at 0x10000 / 0x11000 / 0x12000, each 256 bytes wide.
    // Hook at 0x12040 + 16 bytes → fits in r2.
    auto const def = make_def_with_regions(3);
    auto const p = make_patch(0x12040, 16);
    REQUIRE(cg::gate_patch(p, def).has_value());
}

TEST_CASE("gate_patch: hook in the inter-region gap is rejected",
          "[feature_codegen][address_gate]") {
    // 3 regions at 0x10000 / 0x11000 / 0x12000, each 256 bytes wide.
    // Gaps: [0x10100, 0x11000), [0x11100, 0x12000), [0x12100, ...).
    // Hook at 0x10800 falls in the first gap.
    auto const def = make_def_with_regions(3);
    auto const p = make_patch(0x10800, 16);
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Multi-hook: report every violation.
// ---------------------------------------------------------------------------

TEST_CASE("gate_patch: multi-hook patch reports every out-of-region hook",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(1); // r0 = [0x10000, 0x10100)

    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    // hook1: inside r0 → passes
    p.hooks.push_back({"hook_ok", 0x10010, std::vector<std::uint8_t>(16, 0x00), {}});
    // hook2: way outside → fails
    p.hooks.push_back({"hook_bad_a", 0x99999, std::vector<std::uint8_t>(16, 0x00), {}});
    // hook3: overflows past r0's end → fails
    p.hooks.push_back({"hook_bad_b", 0x100F0, std::vector<std::uint8_t>(32, 0x00), {}});

    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    std::string const msg{r.error().message()};
    REQUIRE(msg.find("hook_bad_a") != std::string::npos);
    REQUIRE(msg.find("hook_bad_b") != std::string::npos);
    REQUIRE(msg.find("hook_ok") == std::string::npos); // passing hooks not in message
    REQUIRE(msg.find("2 hook(s)") != std::string::npos);
}

TEST_CASE("gate_patch: multi-hook all-in-regions passes",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(2);

    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    p.hooks.push_back({"hook_a", 0x10010, std::vector<std::uint8_t>(16, 0x00), {}});
    p.hooks.push_back({"hook_b", 0x11020, std::vector<std::uint8_t>(16, 0x00), {}});

    REQUIRE(cg::gate_patch(p, def).has_value());
}

// ---------------------------------------------------------------------------
// Error message shape — surfaces the available regions for the user.
// ---------------------------------------------------------------------------

TEST_CASE("WritableRegion parser rejects a negative address with a clear message",
          "[defs][writable_region][parse]") {
    // Without the explicit negative guard, this would surface as
    // "address + length overflows the address space" — confusing.
    std::string const toml =
        "[pack]\nschema_version=1\nid=\"neg\"\nendianness=\"big\"\n"
        "[[writable_region]]\nname=\"r\"\nkind=\"calibration\"\n"
        "address=-1\nlength=16\n";
    auto const def = st::Definition::from_toml_string(toml);
    REQUIRE_FALSE(def.has_value());
    std::string const msg{def.error().message()};
    REQUIRE(msg.find("non-negative") != std::string::npos);
}

TEST_CASE("gate_patch: rejection message lists the available regions",
          "[feature_codegen][address_gate]") {
    auto const def = make_def_with_regions(2);
    auto const p = make_patch(0xFFFFF, 16); // way out
    auto const r = cg::gate_patch(p, def);
    REQUIRE_FALSE(r.has_value());
    std::string const msg{r.error().message()};
    // Both region names appear in the help block so the user can fix
    // the splice address by reference.
    REQUIRE(msg.find("'r0'") != std::string::npos);
    REQUIRE(msg.find("'r1'") != std::string::npos);
    REQUIRE(msg.find("Available regions") != std::string::npos);
}
