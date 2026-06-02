// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Bridge between a PatchedRom (from PatchInserter::apply) and a
// FlashPlan (consumed by Flasher::execute). Step 6 of 6 in the
// patch-insertion design from docs/30 §Module shape.
//
// Mechanism: compute the byte-level delta between the source ROM
// and the patched ROM via Flasher::compute_delta, then materialize
// SectorWrite entries holding the patched bytes at each differing
// sector. Returns a FlashPlan that can flow straight into the flash
// orchestrator.
//
// No transport calls; pure function over the two ROM buffers + the
// pack-supplied sector size. The orchestrator's
// `evaluate_plan_policy` runs before any wire activity — it's the
// caller's responsibility to invoke it on the returned plan.

#pragma once

#include "st/flash.hpp"
#include "st/core/result.hpp"
#include "st/feature_patch/inserter.hpp"

#include <cstdint>
#include <vector>

namespace st::feature_patch {

// Build a FlashPlan from a source ROM + a PatchedRom output. Walks
// the two buffers in `sector_size`-aligned chunks; emits a
// `SectorWrite` per chunk whose bytes differ. `base_address`
// translates byte offsets in the buffer into ECU flash addresses
// (offset 0 in the buffer = `base_address` on the ECU).
//
// The returned plan uses sensible defaults: programming session,
// verify-after-write enabled, bus-silenced. Callers that need to
// flip data_format (e.g. for the optional 0xB6 bulk-transfer
// cipher) do so on the returned plan before passing to execute().
//
// If `patched_rom.bytes.size() != source_rom.size()`, returns
// InvalidArgument — the source must come from the same flash region
// the patched output describes.
[[nodiscard]] Result<st::flash::FlashPlan>
build_flash_plan(std::vector<std::uint8_t> const &source_rom,
                 PatchedRom const &patched_rom,
                 std::uint32_t sector_size = 0x1000,
                 std::uint32_t base_address = 0);

} // namespace st::feature_patch
