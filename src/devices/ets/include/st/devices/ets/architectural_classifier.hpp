// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::Layer — architectural classification for the
// rom_offset of a `.ptm` patch record per `docs/35-tuner-overlay-architecture.md`.
//
// Patches in a tune cluster into well-known architectural regions
// (OEM tables, dead-fill tuner additions, UDS dispatch retargets,
// code patches in the primary/secondary code regions, etc.). The
// inspect modal, future tune-diff view, and validation logic all
// need to bucket patches by these regions so the UX shows "this tune
// makes 47 calibration edits + 3 code patches" instead of just a
// flat list of addresses.
//
// Independent of the Tier 3 cipher gating — operates on decrypted
// patch records (rom_offset + length), not on `.ptm` bytes.

#ifndef ST_DEVICES_AP3_ARCHITECTURAL_CLASSIFIER_HPP
#define ST_DEVICES_AP3_ARCHITECTURAL_CLASSIFIER_HPP

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace st::devices::ets {

// Architectural regions per `docs/35-tuner-overlay-architecture.md`.
// Order is the canonical enum index used by `summarize`'s tally.
enum class Layer : std::uint8_t {
    HeaderBootSecurity = 0,    // OEM ROM header / boot / secureboot stub
    EarlyCalTables,            // Calibration before main block
    TunerAddition,             // Layer 2 — dead-fill region (0x078bc..0x10000 on LF79103P)
    MainCalibration,           // Layer 1 — OEM calibration tables
    AlignmentPad,              // Pre-security alignment
    SecurityChecksumApproach,  // Cal-checksum + security-prep region
    UdsDispatchRetarget,       // UDS F3xx logger plumbing (subset of Layer 3)
    RomTail,                   // Last few bytes of ROM
    PrimaryCode,               // Layer 3 — main code region (0x60000..0x80000 on LF79103P)
    SecondaryCode,             // Layer 3 — UDS handler region (0xC0000..0xD0000 on LF79103P)
    Uncharacterized,           // Fallback for addresses outside any registered range
};

// Display label for a layer. Stable text the UI can use directly.
[[nodiscard]] std::string_view layer_label(Layer layer) noexcept;

// Per-platform region map. ECU calibration IDs vary in their region
// addresses; the loader fills this struct per CID and hands it to the
// classifier. The default LF79103P map (the user's current daily
// driver) is exposed below; other CIDs come online as their address
// ranges are documented.
struct LayerMap {
    struct Range {
        std::uint32_t start;
        std::uint32_t end_exclusive;
        Layer layer;
    };
    std::vector<Range> ranges;
};

// Default LF79103P region map per `docs/35-tuner-overlay-architecture.md`
// §"Per-ISA region addresses". Constructed once on first call and
// shared via static reference — safe to address-compare.
[[nodiscard]] LayerMap const &default_lf79103p_layer_map();

// Classify a single rom_offset. Falls back to `Layer::Uncharacterized`
// when no range matches.
[[nodiscard]] Layer classify(LayerMap const &map, std::uint32_t rom_offset) noexcept;

// One classified patch record.
struct PatchClassification {
    std::uint32_t rom_offset;
    std::uint32_t length;
    Layer layer;
};

// Classify a list of patches. Returns one entry per input, preserving
// order. Returns an empty vector if the two input spans have different
// sizes (caller bug; the spec keeps this as a soft failure to keep the
// classifier signature uniform).
[[nodiscard]] std::vector<PatchClassification>
classify_patches(LayerMap const &map,
                 std::span<std::uint32_t const> rom_offsets,
                 std::span<std::uint32_t const> lengths);

// Aggregated counts for the inspect modal's summary row.
struct LayerSummary {
    Layer layer;
    std::uint32_t patch_count;
    std::uint64_t total_bytes;
};

// Aggregate patch-by-patch classifications into one row per layer.
// Result is sorted by `total_bytes` descending (the biggest-impact
// region first), with zero-patch layers omitted. Stable across runs.
[[nodiscard]] std::vector<LayerSummary>
summarize(std::span<PatchClassification const> classifications);

} // namespace st::devices::ets

#endif // ST_DEVICES_AP3_ARCHITECTURAL_CLASSIFIER_HPP
