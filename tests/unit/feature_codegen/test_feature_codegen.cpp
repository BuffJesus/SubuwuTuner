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
