// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// ROM allocator — draws fixed-size byte regions from one or more
// declared writable ROM areas. The patch-insertion layer's analog of
// RamAllocator (which lives in st::feature_codegen for per-hook
// scratch RAM): here the regions come from `[[writable_region]] kind
// = "code"` pack declarations, and the claims hold the patch's code
// bytes + the on-ROM manifest.
//
// Multi-region: a pack may declare several non-contiguous writable
// regions (e.g. one in the high-cal scalar block + one at the tail
// of the primary cal). claim() walks regions in declaration order
// and first-fits; this matches what `gate_patch` expects (every
// emitted byte falls within one declared region, not spanning).
//
// Bump-allocator inside each region — no reuse of freed slots. Patch
// insertion is one-shot per `apply()` call; a fresh allocator runs
// for each call. reset() exists for callers that want to re-run
// against the same regions without re-constructing.

#pragma once

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace st::feature_patch {

// One declared writable region. `name` mirrors the pack's
// `[[writable_region]].name` so diagnostics can name the region the
// allocator drew from (or refused for). `base` + `length` together
// define the inclusive byte range `[base, base + length)`.
struct RomRegion {
    std::string name;
    std::uint32_t base{0};
    std::uint32_t length{0};
};

// One successful allocation. `address` is absolute (not relative to
// any region's base); the inserter writes the patch bytes there.
// `region_name` carries the declaration the allocator drew from for
// downstream logging.
struct RomClaim {
    std::uint32_t address{0};
    std::uint32_t size{0};
    std::uint32_t alignment{1};
    std::string region_name;
};

// RomAllocator — first-fit bump allocator across N writable regions.
// Each region has its own cursor; claim() walks regions in
// declaration order until one accommodates the request.
class RomAllocator {
public:
    // Construct over a list of regions. An empty list is a valid
    // "no writable space" allocator that refuses every claim with
    // OutOfRange + a descriptive message.
    explicit RomAllocator(std::vector<RomRegion> regions);

    // Reserve `size` bytes, aligned to `alignment` (must be a power
    // of two; values < 2 are treated as 1). Default alignment is 4 —
    // both SH-2A and RH850 instructions are at minimum 2-byte and
    // commonly 4-byte; aligning patch code to 4 covers both. Returns
    // OutOfRange when no region has room; InvalidArgument on a
    // non-power-of-two alignment or zero size.
    [[nodiscard]] Result<RomClaim> claim(std::size_t size, std::size_t alignment = 4);

    // Reset all region cursors to their bases. Useful between
    // hypothetical re-runs against the same regions (not used by
    // the inserter's normal one-shot apply path).
    void reset() noexcept;

    [[nodiscard]] std::size_t region_count() const noexcept {
        return regions_.size();
    }
    [[nodiscard]] RomRegion const &region(std::size_t idx) const {
        return regions_.at(idx);
    }
    [[nodiscard]] std::uint32_t used_in_region(std::size_t idx) const {
        return cursors_.at(idx) - regions_.at(idx).base;
    }
    [[nodiscard]] std::uint32_t remaining_in_region(std::size_t idx) const {
        return regions_.at(idx).base + regions_.at(idx).length - cursors_.at(idx);
    }
    [[nodiscard]] std::uint32_t total_used() const noexcept;
    [[nodiscard]] std::uint32_t total_remaining() const noexcept;

private:
    std::vector<RomRegion> regions_;
    std::vector<std::uint32_t> cursors_; // one cursor per region
};

} // namespace st::feature_patch
