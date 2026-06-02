// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/flash_bridge.hpp"

#include <string>

namespace st::feature_patch {

Result<st::flash::FlashPlan> build_flash_plan(std::vector<std::uint8_t> const &source_rom,
                                              PatchedRom const &patched_rom,
                                              std::uint32_t sector_size,
                                              std::uint32_t base_address) {
    if (source_rom.size() != patched_rom.bytes.size()) {
        std::string msg{"build_flash_plan: source_rom size "};
        msg.append(std::to_string(source_rom.size()));
        msg.append(" != patched_rom.bytes size ");
        msg.append(std::to_string(patched_rom.bytes.size()));
        msg.append(" — the two buffers must describe the same flash region");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    if (sector_size == 0) {
        return failure(ErrorCode::InvalidArgument, "build_flash_plan: sector_size must be > 0");
    }

    auto const sectors = st::flash::Flasher::compute_delta(source_rom, patched_rom.bytes,
                                                           sector_size, base_address);

    st::flash::FlashPlan plan;
    plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite w;
        w.sector = s;
        auto const local_off = s.address - base_address;
        w.data.assign(patched_rom.bytes.begin() + static_cast<std::ptrdiff_t>(local_off),
                      patched_rom.bytes.begin() +
                          static_cast<std::ptrdiff_t>(local_off + s.length));
        plan.writes.push_back(std::move(w));
    }
    return plan;
}

} // namespace st::feature_patch
