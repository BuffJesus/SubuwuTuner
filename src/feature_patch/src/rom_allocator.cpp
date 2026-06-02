// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/rom_allocator.hpp"

#include <string>

namespace st::feature_patch {

namespace {

[[nodiscard]] constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1U)) == 0U;
}

// Round `value` up to the next multiple of `align` (which must be a
// power of two). Caller is responsible for align validity.
[[nodiscard]] constexpr std::uint32_t round_up(std::uint32_t value, std::uint32_t align) noexcept {
    return (value + (align - 1U)) & ~(align - 1U);
}

} // namespace

RomAllocator::RomAllocator(std::vector<RomRegion> regions) : regions_(std::move(regions)) {
    cursors_.reserve(regions_.size());
    for (auto const &r : regions_) {
        cursors_.push_back(r.base);
    }
}

Result<RomClaim> RomAllocator::claim(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        return failure(ErrorCode::InvalidArgument, "RomAllocator::claim: size must be > 0");
    }
    std::size_t const align = alignment < 2U ? 1U : alignment;
    if (align != 1U && !is_power_of_two(align)) {
        return failure(ErrorCode::InvalidArgument,
                       "RomAllocator::claim: alignment must be a power of two");
    }
    auto const align_u32 = static_cast<std::uint32_t>(align);
    auto const size_u32 = static_cast<std::uint32_t>(size);

    if (regions_.empty()) {
        return failure(ErrorCode::OutOfRange, "RomAllocator::claim: no writable regions declared "
                                              "(pack needs at least one [[writable_region]] of "
                                              "kind = \"code\" for patch insertion)");
    }

    // First-fit: try each region in declaration order. Skip any that
    // can't accommodate after alignment + size; succeed on the first
    // that fits. The region order in the pack is meaningful — earlier
    // regions are preferred, which lets a pack author place a "small,
    // safe" region first (used for short patches) and reserve a
    // larger region as a fallback.
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        auto const &r = regions_[i];
        std::uint32_t const cursor = cursors_[i];
        std::uint32_t const region_end = r.base + r.length;

        // Round cursor up to alignment. Guard against overflow when
        // cursor is already near UINT32_MAX (pathological — but the
        // pack's authority over base addresses means we don't have to
        // trust them.)
        if (cursor + (align_u32 - 1U) < cursor) {
            continue; // overflow → can't align in this region
        }
        std::uint32_t const aligned = round_up(cursor, align_u32);
        if (aligned < cursor) {
            continue; // round-up wrapped
        }

        // Bounds check the claim.
        if (aligned + size_u32 < aligned) {
            continue; // size overflow
        }
        if (aligned + size_u32 > region_end) {
            continue; // doesn't fit
        }

        // Allocate.
        cursors_[i] = aligned + size_u32;
        RomClaim out{};
        out.address = aligned;
        out.size = size_u32;
        out.alignment = align_u32;
        out.region_name = r.name;
        return out;
    }

    std::string msg{"RomAllocator::claim: no region has "};
    msg.append(std::to_string(size));
    msg.append(" bytes free (with alignment ");
    msg.append(std::to_string(align));
    msg.append(") — tried ");
    msg.append(std::to_string(regions_.size()));
    msg.append(" region(s)");
    return failure(ErrorCode::OutOfRange, std::move(msg));
}

void RomAllocator::reset() noexcept {
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        cursors_[i] = regions_[i].base;
    }
}

std::uint32_t RomAllocator::total_used() const noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        sum += cursors_[i] - regions_[i].base;
    }
    return sum;
}

std::uint32_t RomAllocator::total_remaining() const noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        sum += regions_[i].base + regions_[i].length - cursors_[i];
    }
    return sum;
}

} // namespace st::feature_patch
