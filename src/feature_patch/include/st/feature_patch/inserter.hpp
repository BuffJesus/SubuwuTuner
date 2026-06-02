// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Patch inserter — bridges the compiled PatchObject (from
// st::feature_codegen) and a flashable PatchedRom (consumed by
// st::flash::plan). Glues the three preceding slices together:
//
//   1. RomAllocator picks a free ROM region for each hook's code.
//   2. splice_{sh2a,rh850} emits the BRA / JR bytes at each
//      hook.splice_address jumping to the allocated patch address.
//   3. ManifestRecord encodes the (addr, length) of every patched
//      region for the uninstall path.
//
// `apply()` produces a PatchedRom from a PatchObject + the source
// ROM bytes + the pack's declared writable code regions.
//
// Scope of this first slice:
//   - Short-form splices only (per `splice.hpp`). Long-form lands
//     when the underlying splice emitter learns it.
//   - Manifest is appended to the first writable region (no
//     pack-supplied manifest_region override yet).
//   - No checksum recompute — the patched ROM ships with stale
//     checksum bytes; st::checksum::recompute is called separately
//     by the flash orchestrator (per docs/30 §Checksum recompute).
//   - No "active bank" gate on RH850 — bank state is a runtime
//     property the flash orchestrator owns. The inserter trusts
//     the regions it's handed.

#pragma once

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/feature_codegen.hpp"
#include "st/feature_patch/manifest.hpp"
#include "st/feature_patch/rom_allocator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace st::feature_patch {

struct PatchedRom {
    std::vector<std::uint8_t> bytes;       // patched ROM image
    std::vector<ManifestRecord> manifest;  // every record written
    std::uint32_t manifest_address{0};     // where the manifest lives in ROM
    std::string tuner_tag;                 // 4-byte ASCII; "SUBU" default
};

// Apply a compiled patch to a source ROM. Returns a PatchedRom
// containing the modified bytes + manifest + tuner-tag metadata.
//
// Algorithm:
//   1. Copy source_rom into the output buffer.
//   2. Build a RomAllocator from `writable_code_regions`. Each hook's
//      code claims a region (4-byte aligned) and is written there.
//   3. For each hook, emit a splice at `hook.splice_address` jumping
//      to the allocated patch address. Splice bytes are written into
//      the output buffer.
//   4. Build manifest records for every written byte range (patch
//      code + splice site + manifest itself).
//   5. Encode the manifest and write it into the first writable
//      region at the cursor's current position.
//
// Errors:
//   InvalidArgument  — patch.arch is Unknown; writable_code_regions
//                      empty; source_rom too small for the declared
//                      regions.
//   OutOfRange       — ROM allocator exhaustion (no region has space
//                      for a hook's code or for the manifest).
//   NotImplemented   — long-form splice required (current splice
//                      emitters only do short-form).
//   ParseError       — manifest encode failure (shouldn't happen
//                      with our own data, surfaced for completeness).
[[nodiscard]] Result<PatchedRom>
apply(st::feature::codegen::PatchObject const &patch,
      std::vector<std::uint8_t> source_rom,
      std::vector<RomRegion> writable_code_regions,
      std::string tuner_tag = "SUBU");

} // namespace st::feature_patch
