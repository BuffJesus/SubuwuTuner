// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/table_mapping.hpp"

#include "st/defs.hpp"
#include "st/library/patch_decoder.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace st::library {

std::vector<TableRange> compute_table_ranges(st::Definition const &def) {
    std::vector<TableRange> out;
    out.reserve(def.tables().size());
    for (auto const &t : def.tables()) {
        auto axis_len = [&](std::optional<std::string> const &name) -> std::size_t {
            if (!name.has_value() || name->empty()) {
                return 1;
            }
            auto const *a = def.find_axis(*name);
            return a == nullptr ? 1 : a->length;
        };
        std::size_t const cols = axis_len(t.axis_x);
        std::size_t const rows = t.dimensions >= 2 ? axis_len(t.axis_y) : 1;
        std::size_t const depth = t.dimensions >= 3 ? axis_len(t.axis_z) : 1;
        std::size_t const step = st::byte_size(t.data_type);
        std::size_t const bytes = cols * rows * depth * step;
        if (bytes == 0) {
            continue;
        }
        TableRange r;
        r.id = t.id;
        r.name = t.name;
        r.category = t.category;
        r.start = t.address;
        r.end_exclusive = t.address + bytes;
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(),
              [](TableRange const &x, TableRange const &y) { return x.start < y.start; });
    return out;
}

TableRange const *find_table_for(std::vector<TableRange> const &ranges,
                                 std::size_t rom_offset) noexcept {
    TableRange const *best = nullptr;
    for (auto const &r : ranges) {
        if (rom_offset < r.start || rom_offset >= r.end_exclusive) {
            continue;
        }
        if (best == nullptr ||
            (r.end_exclusive - r.start) < (best->end_exclusive - best->start)) {
            best = &r;
        }
    }
    return best;
}

std::vector<TableHit> aggregate_table_hits(DecodedPtm const &decoded,
                                           std::vector<TableRange> const &ranges) {
    std::map<std::string, TableHit> by_id;
    for (auto const &p : decoded.patches) {
        auto const *t = find_table_for(ranges, p.rom_offset);
        if (t == nullptr) {
            continue;
        }
        auto &hit = by_id[t->id];
        if (hit.table_id.empty()) {
            hit.table_id = t->id;
            hit.table_name = t->name;
        }
        ++hit.patch_count;
        hit.total_bytes += p.length;
    }
    std::vector<TableHit> out;
    out.reserve(by_id.size());
    for (auto &[id, hit] : by_id) {
        out.push_back(std::move(hit));
    }
    std::sort(out.begin(), out.end(),
              [](TableHit const &a, TableHit const &b) {
                  if (a.patch_count != b.patch_count) {
                      return a.patch_count > b.patch_count;
                  }
                  return a.total_bytes > b.total_bytes;
              });
    return out;
}

} // namespace st::library
