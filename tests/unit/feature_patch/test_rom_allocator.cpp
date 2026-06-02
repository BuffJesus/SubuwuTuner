// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/rom_allocator.hpp"

#include <catch2/catch_test_macros.hpp>

namespace fp = st::feature_patch;

TEST_CASE("RomAllocator with empty region list refuses every claim",
          "[feature_patch][rom_allocator][empty]") {
    fp::RomAllocator alloc{{}};
    REQUIRE(alloc.region_count() == 0);
    REQUIRE(alloc.total_used() == 0);
    REQUIRE(alloc.total_remaining() == 0);

    auto r = alloc.claim(16);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("RomAllocator allocates sequentially from a single region",
          "[feature_patch][rom_allocator]") {
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010000, 0x1000}}};
    REQUIRE(alloc.region_count() == 1);

    auto a = alloc.claim(64);
    REQUIRE(a.has_value());
    REQUIRE(a->address == 0x00010000);
    REQUIRE(a->size == 64);
    REQUIRE(a->region_name == "code");

    auto b = alloc.claim(128);
    REQUIRE(b.has_value());
    REQUIRE(b->address == 0x00010040); // 0x10000 + 64

    auto c = alloc.claim(256);
    REQUIRE(c.has_value());
    REQUIRE(c->address == 0x000100C0); // 0x10040 + 128

    REQUIRE(alloc.used_in_region(0) == 64 + 128 + 256);
    REQUIRE(alloc.remaining_in_region(0) == 0x1000 - (64 + 128 + 256));
}

TEST_CASE("RomAllocator respects alignment",
          "[feature_patch][rom_allocator][alignment]") {
    // Base 0x10001 — deliberately odd to test alignment rounding.
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010001, 0x1000}}};

    // Default alignment is 4 — 0x10001 rounds up to 0x10004.
    auto a = alloc.claim(16);
    REQUIRE(a.has_value());
    REQUIRE(a->address == 0x00010004);
    REQUIRE(a->alignment == 4);

    // Explicit alignment 16 — next claim rounds from 0x10014 to 0x10020.
    auto b = alloc.claim(32, 16);
    REQUIRE(b.has_value());
    REQUIRE(b->address == 0x00010020);
    REQUIRE(b->alignment == 16);
}

TEST_CASE("RomAllocator alignment must be a power of two",
          "[feature_patch][rom_allocator][error]") {
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010000, 0x1000}}};

    auto r = alloc.claim(16, 3);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);

    auto r6 = alloc.claim(16, 6);
    REQUIRE_FALSE(r6.has_value());
    REQUIRE(r6.error().code() == st::ErrorCode::InvalidArgument);

    // Power-of-two alignments work
    REQUIRE(alloc.claim(16, 1).has_value());
    REQUIRE(alloc.claim(16, 2).has_value());
    REQUIRE(alloc.claim(16, 4).has_value());
    REQUIRE(alloc.claim(16, 8).has_value());
}

TEST_CASE("RomAllocator rejects zero-size claims",
          "[feature_patch][rom_allocator][error]") {
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010000, 0x1000}}};

    auto r = alloc.claim(0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("RomAllocator reports OutOfRange when a single region exhausts",
          "[feature_patch][rom_allocator][exhaustion]") {
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010000, 0x100}}};

    REQUIRE(alloc.claim(0x80).has_value());
    REQUIRE(alloc.claim(0x80).has_value());

    auto over = alloc.claim(1);
    REQUIRE_FALSE(over.has_value());
    REQUIRE(over.error().code() == st::ErrorCode::OutOfRange);

    REQUIRE(alloc.remaining_in_region(0) == 0);
    REQUIRE(alloc.total_remaining() == 0);
}

TEST_CASE("RomAllocator first-fits across multiple regions",
          "[feature_patch][rom_allocator][multi-region]") {
    // Two regions: a small "early-cal" region (256 B) and a larger
    // "tail-cal" region (4 KB). First-fit means small claims land
    // in the early-cal region until it's full.
    fp::RomAllocator alloc{{
        fp::RomRegion{"early-cal", 0x00010000, 0x100},
        fp::RomRegion{"tail-cal", 0x00080000, 0x1000},
    }};

    auto a = alloc.claim(64);
    REQUIRE(a.has_value());
    REQUIRE(a->address == 0x00010000);
    REQUIRE(a->region_name == "early-cal");

    auto b = alloc.claim(128);
    REQUIRE(b.has_value());
    REQUIRE(b->address == 0x00010040);
    REQUIRE(b->region_name == "early-cal");

    // Next 128 doesn't fit in early-cal (only 64 left); spills to tail-cal.
    auto c = alloc.claim(128);
    REQUIRE(c.has_value());
    REQUIRE(c->address == 0x00080000);
    REQUIRE(c->region_name == "tail-cal");

    // Small 32 fits back in early-cal (64 remaining).
    auto d = alloc.claim(32);
    REQUIRE(d.has_value());
    REQUIRE(d->address == 0x000100C0);
    REQUIRE(d->region_name == "early-cal");
}

TEST_CASE("RomAllocator with a too-large claim refuses across all regions",
          "[feature_patch][rom_allocator][multi-region]") {
    fp::RomAllocator alloc{{
        fp::RomRegion{"r0", 0x00010000, 0x100},
        fp::RomRegion{"r1", 0x00020000, 0x200},
    }};

    auto r = alloc.claim(0x400); // bigger than either region
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
    // Error message should name the region count for diagnosis.
    auto const &msg = r.error().to_string();
    REQUIRE(msg.find("2 region") != std::string::npos);
}

TEST_CASE("RomAllocator::reset returns cursors to bases",
          "[feature_patch][rom_allocator]") {
    fp::RomAllocator alloc{{fp::RomRegion{"code", 0x00010000, 0x1000}}};

    REQUIRE(alloc.claim(0x800).has_value());
    REQUIRE(alloc.used_in_region(0) == 0x800);

    alloc.reset();
    REQUIRE(alloc.used_in_region(0) == 0);
    REQUIRE(alloc.remaining_in_region(0) == 0x1000);

    // Re-allocating after reset starts at base again.
    auto a = alloc.claim(16);
    REQUIRE(a.has_value());
    REQUIRE(a->address == 0x00010000);
}

TEST_CASE("RomAllocator total_used + total_remaining sum across regions",
          "[feature_patch][rom_allocator][totals]") {
    fp::RomAllocator alloc{{
        fp::RomRegion{"r0", 0x00010000, 0x100},
        fp::RomRegion{"r1", 0x00020000, 0x200},
    }};

    REQUIRE(alloc.total_used() == 0);
    REQUIRE(alloc.total_remaining() == 0x100 + 0x200);

    REQUIRE(alloc.claim(0x80).has_value()); // → r0
    REQUIRE(alloc.total_used() == 0x80);

    REQUIRE(alloc.claim(0x180).has_value()); // → r1 (spills past r0's 0x80 remaining)
    REQUIRE(alloc.total_used() == 0x80 + 0x180);
    REQUIRE(alloc.total_remaining() == (0x100 - 0x80) + (0x200 - 0x180));
}

TEST_CASE("RomAllocator alignment skip-doesn't-fit falls through to next region",
          "[feature_patch][rom_allocator][multi-region][alignment]") {
    // First region's base is unaligned for a 16-byte alignment request,
    // and after alignment only 8 bytes remain (less than the requested
    // 16). Second region is aligned and fits.
    fp::RomAllocator alloc{{
        fp::RomRegion{"r0", 0x00010000, 0x18}, // 24 bytes
        fp::RomRegion{"r1", 0x00020000, 0x100},
    }};

    // Make r0's cursor 0x10010 (8 bytes used).
    REQUIRE(alloc.claim(8).has_value());
    // Now 16 bytes left in r0 with no alignment work; but next claim
    // wants alignment 32 — 0x10010 rounds to 0x10020, which is past
    // r0's end (0x10018). Falls through to r1.
    auto b = alloc.claim(16, 32);
    REQUIRE(b.has_value());
    REQUIRE(b->region_name == "r1");
    REQUIRE(b->address == 0x00020000);
}
