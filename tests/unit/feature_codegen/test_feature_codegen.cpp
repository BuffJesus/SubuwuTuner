// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <catch2/catch_test_macros.hpp>

#include "st/feature_codegen.hpp"
#include "st/feature_ir.hpp"

#include <memory>

namespace cg = st::feature::codegen;

// ---- RamAllocator -------------------------------------------------------

TEST_CASE("RamAllocator: empty region refuses every claim",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 0};
    REQUIRE(a.length() == 0);
    REQUIRE(a.remaining() == 0);
    auto r = a.claim(1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("RamAllocator: zero-size claim is invalid",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 256};
    auto r = a.claim(0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("RamAllocator: non-power-of-two alignment is invalid",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 256};
    auto r = a.claim(4, 3);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("RamAllocator: sequential claims bump the cursor",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 64};

    auto a1 = a.claim(4);
    REQUIRE(a1.has_value());
    REQUIRE(a1->address == 0x40000000);
    REQUIRE(a1->size == 4);
    REQUIRE(a.used() == 4);

    auto a2 = a.claim(8);
    REQUIRE(a2.has_value());
    REQUIRE(a2->address == 0x40000004);
    REQUIRE(a.used() == 12);
    REQUIRE(a.remaining() == 52);
}

TEST_CASE("RamAllocator: claims round up to alignment",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000001, 64};

    auto a1 = a.claim(1);  // un-aligned start, no requested alignment
    REQUIRE(a1.has_value());
    REQUIRE(a1->address == 0x40000001);

    // Now ask for an 8-byte-aligned 4-byte claim. cursor sits at 0x40000002.
    // Next 8-aligned address is 0x40000008.
    auto a2 = a.claim(4, 8);
    REQUIRE(a2.has_value());
    REQUIRE(a2->address == 0x40000008);
    REQUIRE(a2->alignment == 8);

    auto a3 = a.claim(2, 4);  // cursor 0x4000000C, already 4-aligned
    REQUIRE(a3.has_value());
    REQUIRE(a3->address == 0x4000000C);
}

TEST_CASE("RamAllocator: refuses on region exhaustion",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 16};

    auto a1 = a.claim(8);
    REQUIRE(a1.has_value());
    auto a2 = a.claim(8);  // exactly fits — should succeed
    REQUIRE(a2.has_value());
    REQUIRE(a.remaining() == 0);

    auto a3 = a.claim(1);  // one byte over — refuses
    REQUIRE_FALSE(a3.has_value());
    REQUIRE(a3.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("RamAllocator: reset moves the cursor back to base",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000000, 16};
    REQUIRE(a.claim(8).has_value());
    REQUIRE(a.used() == 8);
    a.reset();
    REQUIRE(a.used() == 0);
    REQUIRE(a.remaining() == 16);
    auto a2 = a.claim(16);  // full region available again
    REQUIRE(a2.has_value());
    REQUIRE(a2->address == 0x40000000);
}

TEST_CASE("RamAllocator: alignment-1 means no alignment",
          "[feature_codegen][ram_allocator]") {
    cg::RamAllocator a{0x40000003, 32};
    // Even though base is odd, alignment=1 (and 0, which we treat as 1)
    // should not bump the cursor.
    auto a1 = a.claim(1, 1);
    REQUIRE(a1.has_value());
    REQUIRE(a1->address == 0x40000003);
    auto a2 = a.claim(1, 0);
    REQUIRE(a2.has_value());
    REQUIRE(a2->address == 0x40000004);
}

// ---- arch_name ----------------------------------------------------------

TEST_CASE("arch_name renders each Arch", "[feature_codegen][arch]") {
    REQUIRE(std::string_view{cg::arch_name(cg::Arch::Sh2a)} == "sh2a");
    REQUIRE(std::string_view{cg::arch_name(cg::Arch::Rh850)} == "rh850");
    REQUIRE(std::string_view{cg::arch_name(cg::Arch::Unknown)} == "unknown");
}

// ---- Backend selection --------------------------------------------------

TEST_CASE("select_backend(VA) returns an SH-2A backend",
          "[feature_codegen][select]") {
    auto b = cg::select_backend("VA");
    REQUIRE(b.has_value());
    REQUIRE((*b)->arch() == cg::Arch::Sh2a);
}

TEST_CASE("select_backend(VB) returns an RH850 backend",
          "[feature_codegen][select]") {
    auto b = cg::select_backend("VB");
    REQUIRE(b.has_value());
    REQUIRE((*b)->arch() == cg::Arch::Rh850);
}

TEST_CASE("select_backend is case-insensitive on the platform name",
          "[feature_codegen][select]") {
    REQUIRE(cg::select_backend("va").has_value());
    REQUIRE(cg::select_backend("vb").has_value());
    REQUIRE(cg::select_backend("Va").has_value());
}

TEST_CASE("select_backend refuses an unknown platform",
          "[feature_codegen][select]") {
    auto b = cg::select_backend("EJ20T");
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("select_backend refuses an empty platform string",
          "[feature_codegen][select]") {
    auto b = cg::select_backend("");
    REQUIRE_FALSE(b.has_value());
}

// ---- Stub backends ------------------------------------------------------

TEST_CASE("Sh2aBackend reports its arch",
          "[feature_codegen][sh2a]") {
    cg::Sh2aBackend backend;
    REQUIRE(backend.arch() == cg::Arch::Sh2a);
}

TEST_CASE("Rh850Backend::compile returns NotImplemented",
          "[feature_codegen][stub][rh850]") {
    cg::Rh850Backend backend;
    REQUIRE(backend.arch() == cg::Arch::Rh850);

    st::feature::ir::Module m;
    st::Definition          def;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

// ---- PatchObject ↔ TOML round-trip --------------------------------

TEST_CASE("patch_from_toml: empty text returns nullopt",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml("");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
}

TEST_CASE("patch_from_toml: text without [patch] section returns nullopt",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[graph]
schema_version = 1
)toml");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
}

TEST_CASE("patch_to_toml: empty patch round-trips through patch_from_toml",
          "[feature_codegen][patch_toml]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    auto const text = cg::patch_to_toml(p);
    auto r = cg::patch_from_toml(text);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->arch == cg::Arch::Sh2a);
    REQUIRE((*r)->hooks.empty());
}

TEST_CASE("patch_to_toml: single-hook patch round-trips",
          "[feature_codegen][patch_toml]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    cg::HookPatch h;
    h.symbol         = "after_fuel_calc";
    h.splice_address = 0x000ABCD0;
    h.code           = {0xD0, 0x03, 0xD1, 0x04, 0x31, 0x0C, 0xFF};
    h.ram_claims.push_back(cg::RamClaim{0x40000000, 4, 4});
    h.ram_claims.push_back(cg::RamClaim{0x40000004, 8, 4});
    p.hooks.push_back(std::move(h));

    auto const text = cg::patch_to_toml(p);
    auto r = cg::patch_from_toml(text);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());

    cg::PatchObject const &q = **r;
    REQUIRE(q.arch == cg::Arch::Sh2a);
    REQUIRE(q.hooks.size() == 1);
    REQUIRE(q.hooks[0].symbol == "after_fuel_calc");
    REQUIRE(q.hooks[0].splice_address == 0x000ABCD0);
    REQUIRE(q.hooks[0].code == p.hooks[0].code);
    REQUIRE(q.hooks[0].ram_claims.size() == 2);
    REQUIRE(q.hooks[0].ram_claims[0].address == 0x40000000);
    REQUIRE(q.hooks[0].ram_claims[0].size == 4);
    REQUIRE(q.hooks[0].ram_claims[0].alignment == 4);
    REQUIRE(q.hooks[0].ram_claims[1].address == 0x40000004);
    REQUIRE(q.hooks[0].ram_claims[1].size == 8);
}

TEST_CASE("patch_to_toml: multi-hook patch keeps hook order",
          "[feature_codegen][patch_toml]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Rh850;
    cg::HookPatch h1;
    h1.symbol = "hook_a"; h1.splice_address = 0x100; h1.code = {0x01, 0x02};
    cg::HookPatch h2;
    h2.symbol = "hook_b"; h2.splice_address = 0x200; h2.code = {0x03, 0x04};
    p.hooks.push_back(std::move(h1));
    p.hooks.push_back(std::move(h2));

    auto r = cg::patch_from_toml(cg::patch_to_toml(p));
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->arch == cg::Arch::Rh850);
    REQUIRE((*r)->hooks.size() == 2);
    REQUIRE((*r)->hooks[0].symbol == "hook_a");
    REQUIRE((*r)->hooks[1].symbol == "hook_b");
}

TEST_CASE("patch_from_toml: malformed TOML returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml("[patch\nbroken");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: missing arch returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
)toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: unknown arch returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
arch = "v850"
)toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: odd-length code hex returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
arch = "sh2a"
[[patch.hook]]
symbol = "x"
splice_address = 0x100
code = "ABC"
)toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: non-hex chars in code returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
arch = "sh2a"
[[patch.hook]]
symbol = "x"
splice_address = 0x100
code = "XXYY"
)toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: hook without splice_address returns ParseError",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
arch = "sh2a"
[[patch.hook]]
symbol = "x"
code = ""
)toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch_from_toml: accepts lowercase hex too",
          "[feature_codegen][patch_toml]") {
    auto r = cg::patch_from_toml(R"toml(
[patch]
arch = "sh2a"
[[patch.hook]]
symbol = "x"
splice_address = 0x100
code = "deadbeef"
)toml");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->hooks[0].code
            == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("patch_from_toml: ignores graph sections in a bundled .stmod",
          "[feature_codegen][patch_toml]") {
    // Simulates a .stmod file holding both source graph and compiled
    // patch. The graph loader (feature::from_toml) will read the
    // [graph]/[[node]]/[[edge]] side and ignore [patch]; this loader
    // does the reverse.
    auto r = cg::patch_from_toml(R"toml(
[graph]
schema_version = 1

[[node]]
id   = 1
kind = "primitive.add_int"
pins = []

[patch]
arch = "sh2a"

[[patch.hook]]
symbol = "after_fuel_calc"
splice_address = 0xABCD0
code = "DEADBEEF"
)toml");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->arch == cg::Arch::Sh2a);
    REQUIRE((*r)->hooks.size() == 1);
    REQUIRE((*r)->hooks[0].symbol == "after_fuel_calc");
}

TEST_CASE("patch_to_toml: escapes backslash and double-quote in symbol",
          "[feature_codegen][patch_toml]") {
    cg::PatchObject p;
    p.arch = cg::Arch::Sh2a;
    cg::HookPatch h;
    h.symbol = R"(weird"\name)";
    h.splice_address = 0x100;
    p.hooks.push_back(std::move(h));

    auto r = cg::patch_from_toml(cg::patch_to_toml(p));
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->hooks[0].symbol == R"(weird"\name)");
}
