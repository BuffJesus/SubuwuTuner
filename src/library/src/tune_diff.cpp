// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/tune_diff.hpp"

#include <algorithm>
#include <map>

namespace st::library {

TuneDiffResult
compute_tune_diff(DecodedPtm const &a, DecodedPtm const &b,
                  std::vector<TableRange> const &table_ranges) {
    TuneDiffResult r{};
    r.a_total_patches = static_cast<std::uint32_t>(a.patches.size());
    r.b_total_patches = static_cast<std::uint32_t>(b.patches.size());

    std::map<std::uint32_t, std::size_t> a_by_offset;
    std::map<std::uint32_t, std::size_t> b_by_offset;
    for (std::size_t i = 0; i < a.patches.size(); ++i) {
        a_by_offset[a.patches[i].rom_offset] = i;
    }
    for (std::size_t i = 0; i < b.patches.size(); ++i) {
        b_by_offset[b.patches[i].rom_offset] = i;
    }

    auto const &map = st::devices::ap3::default_lf79103p_layer_map();
    std::map<st::devices::ap3::Layer, LayerDiff> per_layer;
    auto layer_bucket = [&](st::devices::ap3::Layer L) -> LayerDiff & {
        auto it = per_layer.find(L);
        if (it == per_layer.end()) {
            it = per_layer.emplace(L, LayerDiff{L, 0, 0, 0, 0, 0, 0, 0}).first;
        }
        return it->second;
    };

    std::map<std::string, TableDiff> per_table;
    auto table_bucket = [&](std::uint32_t offset) -> TableDiff * {
        if (table_ranges.empty()) {
            return nullptr;
        }
        auto const *t = find_table_for(table_ranges, offset);
        if (t == nullptr) {
            return nullptr;
        }
        auto it = per_table.find(t->id);
        if (it == per_table.end()) {
            it = per_table.emplace(t->id, TableDiff{t->id, t->name, 0, 0, 0, 0, 0, 0, 0}).first;
        }
        return &it->second;
    };

    for (auto const &p : a.patches) {
        auto const L = st::devices::ap3::classify(map, p.rom_offset);
        auto &bucket = layer_bucket(L);
        auto *tb = table_bucket(p.rom_offset);
        auto it = b_by_offset.find(p.rom_offset);
        if (it == b_by_offset.end()) {
            ++r.a_only_patches;
            r.a_only_bytes += p.length;
            ++bucket.a_only_patches;
            bucket.a_only_bytes += p.length;
            if (tb != nullptr) {
                ++tb->a_only_patches;
                tb->a_only_bytes += p.length;
            }
        } else {
            ++r.shared_patches;
            ++bucket.shared_patches;
            if (tb != nullptr) {
                ++tb->shared_patches;
            }
            auto const &pb = b.patches[it->second];
            if (p.bytes != pb.bytes) {
                ++r.changed_at_shared;
                ++bucket.changed_at_shared;
                std::size_t const min_n = std::min(p.bytes.size(), pb.bytes.size());
                std::uint64_t diff_bytes = 0;
                for (std::size_t i = 0; i < min_n; ++i) {
                    if (p.bytes[i] != pb.bytes[i]) {
                        ++diff_bytes;
                    }
                }
                diff_bytes += p.bytes.size() > min_n ? p.bytes.size() - min_n : 0;
                diff_bytes += pb.bytes.size() > min_n ? pb.bytes.size() - min_n : 0;
                r.changed_bytes += diff_bytes;
                bucket.changed_bytes += diff_bytes;
                if (tb != nullptr) {
                    ++tb->changed_at_shared;
                    tb->changed_bytes += diff_bytes;
                }
            }
        }
    }
    for (auto const &p : b.patches) {
        if (a_by_offset.find(p.rom_offset) != a_by_offset.end()) {
            continue;
        }
        auto const L = st::devices::ap3::classify(map, p.rom_offset);
        auto &bucket = layer_bucket(L);
        auto *tb = table_bucket(p.rom_offset);
        ++r.b_only_patches;
        r.b_only_bytes += p.length;
        ++bucket.b_only_patches;
        bucket.b_only_bytes += p.length;
        if (tb != nullptr) {
            ++tb->b_only_patches;
            tb->b_only_bytes += p.length;
        }
    }

    for (auto const &[layer, ld] : per_layer) {
        r.by_layer.push_back(ld);
    }
    std::sort(r.by_layer.begin(), r.by_layer.end(),
              [](LayerDiff const &x, LayerDiff const &y) {
                  return (x.a_only_bytes + x.b_only_bytes + x.changed_bytes) >
                         (y.a_only_bytes + y.b_only_bytes + y.changed_bytes);
              });

    for (auto const &[id, td] : per_table) {
        r.by_table.push_back(td);
    }
    std::sort(r.by_table.begin(), r.by_table.end(),
              [](TableDiff const &x, TableDiff const &y) {
                  return (x.a_only_bytes + x.b_only_bytes + x.changed_bytes) >
                         (y.a_only_bytes + y.b_only_bytes + y.changed_bytes);
              });
    return r;
}

} // namespace st::library
