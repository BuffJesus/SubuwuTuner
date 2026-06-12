// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/architectural_classifier.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace st::devices::ap3 {

LayerMap const &default_lf79103p_layer_map() {
    static LayerMap const map = [] {
        LayerMap m;
        // Address ranges per docs/35-tuner-overlay-architecture.md
        // §"Per-ISA region addresses".
        m.ranges = {
            {0x00000U, 0x06000U, Layer::HeaderBootSecurity},
            {0x06000U, 0x078BCU, Layer::EarlyCalTables},
            {0x078BCU, 0x10000U, Layer::TunerAddition},
            {0x12000U, 0x1EFC38U, Layer::MainCalibration},
            {0x1EFC38U, 0x1F0000U, Layer::AlignmentPad},
            {0x1F0000U, 0x1FF02CU, Layer::SecurityChecksumApproach},
            {0x1FF02CU, 0x1FFFE0U, Layer::UdsDispatchRetarget},
            {0x1FFFE0U, 0x200000U, Layer::RomTail},
            {0x60000U, 0x80000U, Layer::PrimaryCode},
            {0xC0000U, 0xD0000U, Layer::SecondaryCode},
        };
        return m;
    }();
    return map;
}

Layer classify(LayerMap const &map, std::uint32_t rom_offset) noexcept {
    // Most-specific (smallest containing range) wins. The default
    // LF79103P map has overlapping ranges by design — PrimaryCode
    // (0x60000..0x80000) and SecondaryCode (0xC0000..0xD0000) both
    // sit inside MainCalibration (0x12000..0x1EFC38) per docs/35.
    // A first-match scan would always return MainCalibration for
    // those addresses, but the spec's example tests pin that code
    // regions outrank cal regions. Picking the smallest enclosing
    // range gives the right answer without forcing per-map authors
    // to hand-order overlap-safely.
    Layer best = Layer::Uncharacterized;
    std::uint64_t best_size = static_cast<std::uint64_t>(-1);
    for (auto const &range : map.ranges) {
        if (range.start <= rom_offset && rom_offset < range.end_exclusive) {
            std::uint64_t const size =
                static_cast<std::uint64_t>(range.end_exclusive) - range.start;
            if (size < best_size) {
                best_size = size;
                best = range.layer;
            }
        }
    }
    return best;
}

std::vector<PatchClassification>
classify_patches(LayerMap const &map,
                 std::span<std::uint32_t const> rom_offsets,
                 std::span<std::uint32_t const> lengths) {
    if (rom_offsets.size() != lengths.size()) {
        return {};
    }
    std::vector<PatchClassification> out;
    out.reserve(rom_offsets.size());
    for (std::size_t i = 0; i < rom_offsets.size(); ++i) {
        out.push_back({rom_offsets[i], lengths[i], classify(map, rom_offsets[i])});
    }
    return out;
}

std::vector<LayerSummary>
summarize(std::span<PatchClassification const> classifications) {
    // 11 entries: one slot per declared `Layer` enumerator. If the
    // enum grows, this array (and the layer_label switch below) need
    // matching updates.
    constexpr std::size_t kLayerCount = 11U;
    std::array<LayerSummary, kLayerCount> totals{};
    for (std::size_t i = 0; i < totals.size(); ++i) {
        totals[i].layer = static_cast<Layer>(i);
        totals[i].patch_count = 0;
        totals[i].total_bytes = 0;
    }
    for (auto const &c : classifications) {
        auto const idx = static_cast<std::size_t>(c.layer);
        if (idx >= totals.size()) {
            continue; // defensive — would only fire on a corrupted enum value
        }
        totals[idx].patch_count += 1U;
        totals[idx].total_bytes += static_cast<std::uint64_t>(c.length);
    }
    std::vector<LayerSummary> result;
    result.reserve(totals.size());
    for (auto const &row : totals) {
        if (row.patch_count > 0) {
            result.push_back(row);
        }
    }
    std::sort(result.begin(), result.end(),
              [](LayerSummary const &a, LayerSummary const &b) {
                  return a.total_bytes > b.total_bytes;
              });
    return result;
}

std::string_view layer_label(Layer layer) noexcept {
    switch (layer) {
    case Layer::HeaderBootSecurity:
        return "Header / boot / security";
    case Layer::EarlyCalTables:
        return "Early calibration tables";
    case Layer::TunerAddition:
        return "Tuner addition (dead-fill)";
    case Layer::MainCalibration:
        return "Main calibration (OEM tables)";
    case Layer::AlignmentPad:
        return "Alignment pad";
    case Layer::SecurityChecksumApproach:
        return "Security / checksum approach";
    case Layer::UdsDispatchRetarget:
        return "UDS dispatch retarget";
    case Layer::RomTail:
        return "ROM tail";
    case Layer::PrimaryCode:
        return "Primary code region";
    case Layer::SecondaryCode:
        return "Secondary code region";
    case Layer::Uncharacterized:
        return "Uncharacterized";
    }
    return "?";
}

} // namespace st::devices::ap3
