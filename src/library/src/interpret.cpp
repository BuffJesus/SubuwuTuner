// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/interpret.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace st::library {

namespace {

// Case-insensitive substring match. Table-name lookups don't care
// about casing in the def-packs we ship (some are "Target Throttle",
// others "TARGET_THROTTLE") and a contains-test is more robust than a
// strict equality.
[[nodiscard]] bool icontains(std::string const &hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    auto const tolower_byte = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (tolower_byte(static_cast<unsigned char>(hay[i + j])) !=
                tolower_byte(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Convenience: does any hit's name match?
[[nodiscard]] bool any_hit_named(std::vector<TableHit> const &hits,
                                 std::string_view needle) {
    for (auto const &h : hits) {
        if (icontains(h.table_name, needle)) {
            return true;
        }
    }
    return false;
}

// Count hits whose name contains `needle` — used to detect "16
// variants of Target Throttle" patterns.
[[nodiscard]] std::size_t count_hits_named(std::vector<TableHit> const &hits,
                                           std::string_view needle) {
    std::size_t n = 0;
    for (auto const &h : hits) {
        if (icontains(h.table_name, needle)) {
            ++n;
        }
    }
    return n;
}

// Average rounded to nearest whole byte. Empty -> 0.
[[nodiscard]] std::uint64_t avg_bytes_in_hits_named(
    std::vector<TableHit> const &hits, std::string_view needle) {
    std::uint64_t total = 0;
    std::size_t count = 0;
    for (auto const &h : hits) {
        if (icontains(h.table_name, needle)) {
            total += h.total_bytes;
            ++count;
        }
    }
    return count == 0 ? 0 : total / count;
}

// HPFP cluster anchor offsets — see MEMORY.md
// `project_hpfp_0x49ba0_resolved.md` (2026-06-09 Ghidra). The 16-elem
// uint8 1-D map at 0x49B98, byte indices 8..11 land at 0x49BA0..0x49BA3,
// and NTM tunes 5 bytes at 0x49B9E..0x49BA2 — so a rom_offset anywhere
// in the [0x49B98, 0x49BA8) window is "HPFP cluster" territory.
constexpr std::uint32_t kHpfpClusterStart = 0x49B98U;
constexpr std::uint32_t kHpfpClusterEnd = 0x49BA8U;

// MAF Sensor Scale BigSF distinguisher — RE wave 2026-06-12 PM. A
// patch landing at rom 0x30F64 with length 256 (full 128-cell × u16
// table) is the byte that distinguishes BigSF from Redline/SF.
constexpr std::uint32_t kMafSensorScaleRom = 0x30F64U;
constexpr std::uint32_t kMafSensorScaleLen = 256U;

[[nodiscard]] bool any_patch_in_hpfp_cluster(DecodedPtm const &decoded) {
    for (auto const &p : decoded.patches) {
        if (p.rom_offset >= kHpfpClusterStart && p.rom_offset < kHpfpClusterEnd) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool any_patch_at_maf_sensor_scale(DecodedPtm const &decoded) {
    for (auto const &p : decoded.patches) {
        if (p.rom_offset == kMafSensorScaleRom && p.length == kMafSensorScaleLen) {
            return true;
        }
    }
    return false;
}

// "Effective-state" matcher. A .ptm may carry multiple `<patch>` entries
// at the same (rom_offset, ram_offset) — they're applied sequentially
// and the LAST one wins on the working ROM. For diff prose we care
// about the effective state, so pair A's last-at-key against B's last-
// at-key by walking from the end. Earlier `find_matching_b` returned
// the FIRST match, which mis-aligned every duplicate-key pair in a
// Fehr-class tune (~1000 spurious pairs across WRK2/WRK3) and surfaced
// noisy "uniform -8 delta" prose where the real change is per-uint16-cell.
[[nodiscard]] PatchEntry const *
find_matching_b_effective(PatchEntry const &a, DecodedPtm const &b) {
    for (auto it = b.patches.rbegin(); it != b.patches.rend(); ++it) {
        if (it->rom_offset == a.rom_offset && it->ram_offset == a.ram_offset) {
            return &*it;
        }
    }
    return nullptr;
}

// Per-cell uniform-delta detector. Treats bytes as cells of width 1
// (uint8), 2 (uint16 BE), or 2 (uint16 LE) and reports the largest
// "all differing cells have the same delta" subset. Returns the
// dominant signed delta + the count of cells that match it. A
// patch with len 476 where 112 of 238 uint16 BE cells shifted by
// +512 and the rest are identical reports (delta=512, matching=112,
// total=238, width=2, big_endian=true).
struct CellDelta {
    int delta{0};
    std::size_t matching_cells{0};
    std::size_t total_cells{0};
    std::size_t width{1};
    bool big_endian{false};
};

[[nodiscard]] CellDelta best_cell_delta(std::vector<std::uint8_t> const &a,
                                         std::vector<std::uint8_t> const &b) {
    CellDelta best;
    if (a.size() != b.size() || a.empty()) {
        return best;
    }
    auto const try_width = [&](std::size_t width, bool be) {
        if (a.size() % width != 0) {
            return;
        }
        std::size_t const n = a.size() / width;
        // Wrap-aware reinterpretation threshold: any unsigned delta whose
        // magnitude exceeds half the cell's value range is almost
        // certainly a signed cell that crossed zero (ignition timing,
        // signed knock retard, signed torque trim, etc.). Re-cast the
        // delta into signed-int space so the prose reads "-16 raw" instead
        // of the unsigned "+65520 raw" that the firmware-canonical bytes
        // produce. Pure cosmetic in unsigned-table cases (no cells exceed
        // the threshold there); load-bearing in signed-table cases (the
        // wrong sign was the original WRK2→WRK3 defect).
        int const range = (width == 1) ? 0x100 : 0x10000;
        int const half_range = range / 2;
        std::map<int, std::size_t> bin;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t av = 0, bv = 0;
            for (std::size_t k = 0; k < width; ++k) {
                std::size_t const idx = i * width + (be ? k : (width - 1 - k));
                av = (av << 8) | a[idx];
                bv = (bv << 8) | b[idx];
            }
            int d = static_cast<int>(bv) - static_cast<int>(av);
            if (d > half_range) {
                d -= range;
            } else if (d < -half_range) {
                d += range;
            }
            if (d != 0) {
                ++bin[d];
            }
        }
        for (auto const &[d, count] : bin) {
            // Coverage in byte-equivalent so width=1 and width=2 are
            // on the same scale. Tie-break: prefer wider semantic view.
            std::uint64_t const cov = count * width;
            std::uint64_t const best_cov = best.matching_cells * best.width;
            bool take = false;
            if (cov > best_cov) {
                take = true;
            } else if (cov == best_cov && width > best.width) {
                take = true;
            }
            if (take) {
                best.delta = d;
                best.matching_cells = count;
                best.total_cells = n;
                best.width = width;
                best.big_endian = be;
            }
        }
    };
    try_width(1, false); // uint8 — byte-equivalent
    try_width(2, true);  // uint16 BE
    try_width(2, false); // uint16 LE
    return best;
}

} // namespace

// Atlas-driven anchor lookup: walk the decoded patches and emit any
// anchor prose whose ROM range covers a patch. Each anchor fires at
// most once even when multiple patches land inside it. Returns the
// set of triggered anchor IDs so the caller can avoid re-firing the
// hardcoded fallbacks.
std::vector<std::string> emit_atlas_anchor_prose(
    DecodedPtm const &decoded, Atlas const &atlas,
    std::vector<std::string> &out) {
    std::vector<std::string> fired_ids;
    for (auto const &p : decoded.patches) {
        auto const *anchor = atlas.anchor_for_offset(p.rom_offset);
        if (anchor == nullptr) {
            continue;
        }
        bool already = false;
        for (auto const &id : fired_ids) {
            if (id == anchor->id) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        fired_ids.push_back(anchor->id);
        out.push_back(anchor->prose);
    }
    return fired_ids;
}

bool anchor_fired(std::vector<std::string> const &fired, std::string_view id) {
    for (auto const &s : fired) {
        if (s == id) {
            return true;
        }
    }
    return false;
}

// Atlas-driven safety pair detection. A pair fires when the patches
// touch ANY of the LHS patterns but NONE of the RHS patterns (or vice
// versa). Pattern match is a case-insensitive substring against the
// hit's table_name. Returns one prose sentence per violated pair.
void emit_safety_pair_prose(std::vector<TableHit> const &top_tables,
                            Atlas const &atlas,
                            std::vector<std::string> &out) {
    if (top_tables.empty()) {
        return;
    }
    auto const matches_any = [&](std::vector<std::string> const &patterns) {
        for (auto const &pat : patterns) {
            if (pat.empty()) {
                continue;
            }
            for (auto const &h : top_tables) {
                if (icontains(h.table_name, pat)) {
                    return true;
                }
            }
        }
        return false;
    };
    for (auto const &pair : atlas.safety_pairs()) {
        bool const has_lhs = matches_any(pair.lhs_patterns);
        bool const has_rhs = matches_any(pair.rhs_patterns);
        if (has_lhs != has_rhs) {
            // One-sided: violation.
            char buf[512];
            (void)std::snprintf(
                buf, sizeof(buf),
                "Safety-pair flag (%s, %s): %s",
                pair.severity.c_str(), pair.id.c_str(),
                pair.rationale.empty() ? pair.title.c_str()
                                       : pair.rationale.c_str());
            out.emplace_back(buf);
        }
    }
}

std::vector<std::string>
interpret_inspect(DecodedPtm const &decoded,
                  std::vector<TableHit> const &top_tables,
                  Atlas const *atlas) {
    std::vector<std::string> out;

    auto const total = decoded.patches.size();

    // Rule: total patch count thresholds. Always fires regardless of
    // def-pack availability. These thresholds are calibrated against
    // the corpus snapshot in MEMORY.md — Stage0 carries ~2,600
    // patches; Fehr WRK3 ~5,000+; single-feature mods (boost-up,
    // wastegate-only) sit well under 1,000.
    if (total < 1000 && total > 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Lightweight basemap or single-feature mod (%zu patches).",
                      total);
        out.emplace_back(buf);
    } else if (total >= 5000) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Full custom tune (%zu patches) — Fehr-class footprint.",
                      total);
        out.emplace_back(buf);
    } else if (total >= 1000) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Mid-weight retune (%zu patches).",
                      total);
        out.emplace_back(buf);
    }

    // Anchor-driven prose. Prefer atlas anchors when an atlas is
    // available — the atlas TOML carries the authoritative prose so
    // future updates ship as data, not code. When no atlas is passed
    // (legacy callers), fall back to the hardcoded anchors below.
    std::vector<std::string> fired_anchors;
    if (atlas != nullptr) {
        fired_anchors = emit_atlas_anchor_prose(decoded, *atlas, out);
    }

    // Hardcoded fallback: MAF Sensor Scale.
    if (!anchor_fired(fired_anchors, "maf_sensor_scale") &&
        any_patch_at_maf_sensor_scale(decoded)) {
        out.push_back(
            "Patches MAF Sensor Scale at rom 0x30F64 (256B) — intake "
            "recalibration (SF, BigSF, or aftermarket MAF housing).");
    }

    // Hardcoded fallback: HPFP cluster. Anchor-offset based — see
    // `project_hpfp_0x49ba0_resolved.md` + the FA24 lobe-phase note in
    // MEMORY.md. Fires without a def-pack.
    if (!anchor_fired(fired_anchors, "hpfp_cluster") &&
        any_patch_in_hpfp_cluster(decoded)) {
        out.push_back(
            "Patches HPFP cluster (rom 0x49B98..0x49BA8) — FA24 "
            "cam-lobe phase compensation.");
    }

    // Atlas-driven safety pair detection (only when both an atlas and
    // a def-pack are supplied — we need table names to match patterns).
    if (atlas != nullptr) {
        emit_safety_pair_prose(top_tables, *atlas, out);
    }

    // Below here, rules depend on the def-pack to resolve table
    // names. If no def-pack was passed, skip them cleanly — the
    // anchor + count rules above carry the section on their own.
    if (top_tables.empty()) {
        return out;
    }

    // Rule: Target Throttle variants. The Fehr/Stage tune family
    // touches many "Target Throttle <variant>" tables; ≥4 variants
    // is the threshold beyond which "Stage-tier retune" stops being
    // a noisy claim.
    auto const tt_count = count_hits_named(top_tables, "Target Throttle");
    auto const tt_avg = avg_bytes_in_hits_named(top_tables, "Target Throttle");
    if (tt_count >= 4) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Heavy on Target Throttle (%zu variants × ~%lluB each) — "
                      "Stage-tier throttle remap.",
                      tt_count,
                      static_cast<unsigned long long>(tt_avg));
        out.emplace_back(buf);
    }

    // Rule: Wastegate Duty Initial+Maximum — the boost-up signature.
    bool const has_wg_init =
        any_hit_named(top_tables, "Wastegate Duty Initial");
    bool const has_wg_max =
        any_hit_named(top_tables, "Wastegate Duty Maximum");
    if (has_wg_init && has_wg_max) {
        out.push_back(
            "Wastegate Duty Initial + Maximum both touched — boost-up "
            "signature.");
    }

    // Rule: AVCS philosophy. The Intake/Exhaust pair is a useful
    // tuner-author distinguisher (Fehr-style independent vs
    // COBB/NexGen "exhaust-only" consensus).
    bool const has_avcs_intake =
        any_hit_named(top_tables, "AVCS Intake") ||
        any_hit_named(top_tables, "Intake AVCS") ||
        any_hit_named(top_tables, "Intake Cam") ||
        any_hit_named(top_tables, "AVCS - Intake");
    bool const has_avcs_exhaust =
        any_hit_named(top_tables, "AVCS Exhaust") ||
        any_hit_named(top_tables, "Exhaust AVCS") ||
        any_hit_named(top_tables, "Exhaust Cam") ||
        any_hit_named(top_tables, "AVCS - Exhaust");
    // Cross-corpus finding (see findings/tuning-knowledge-2026-06-13):
    // - COBB / NexGen tune Exhaust cam targets and SKIP Intake.
    // - Fehr / Felix is the only family that touches Intake. They may
    //   touch Intake alone or Intake+Exhaust together; either is the
    //   Fehr signature.
    if (has_avcs_intake) {
        out.push_back(
            "AVCS Intake cam targets retuned — Fehr-family signature "
            "(rare across the COBB / NexGen corpus).");
    } else if (has_avcs_exhaust) {
        out.push_back(
            "AVCS Exhaust retuned without Intake — COBB / NexGen "
            "consensus AVCS philosophy.");
    }

    // Rule: timing aggression. Knock thresholds + DAA in the same
    // tune is the classic "push timing harder" combination.
    bool const has_knock =
        any_hit_named(top_tables, "Knock Threshold");
    bool const has_daa =
        any_hit_named(top_tables, "Dynamic Advance Adder") ||
        any_hit_named(top_tables, "Dynamic Advance");
    if (has_knock && has_daa) {
        out.push_back(
            "Knock thresholds + Dynamic Advance Adder co-edited — "
            "timing aggression layer.");
    }

    return out;
}

std::vector<std::string>
interpret_diff(DecodedPtm const &a, DecodedPtm const &b,
               TuneDiffResult const &result, Atlas const *atlas) {
    (void)atlas; // reserved for future atlas-driven diff narratives
    std::vector<std::string> out;

    // Direction-of-change for the per-table buckets. The TuneDiffResult's
    // per-table view already aggregates a_only / b_only / changed at
    // shared per table, so we can synthesize "B adds the X retune"
    // ((a_only=0, b_only>0) and "B drops the X retune"
    // (a_only>0, b_only=0) cheaply.
    //
    // Threshold: ignore tables with under 16 affected bytes — that's
    // typical noise (single scalar). We're looking for narrative-
    // worthy edits, not every single-byte difference.
    constexpr std::uint64_t kMinTableBytes = 16;

    std::vector<TableDiff const *> b_adds;   // a_only=0, b_only>0
    std::vector<TableDiff const *> b_drops;  // a_only>0, b_only=0
    std::vector<TableDiff const *> b_changes;
    for (auto const &td : result.by_table) {
        std::uint64_t const td_total =
            td.a_only_bytes + td.b_only_bytes + td.changed_bytes;
        if (td_total < kMinTableBytes) {
            continue;
        }
        if (td.b_only_patches > 0 && td.a_only_patches == 0 &&
            td.changed_at_shared == 0) {
            b_adds.push_back(&td);
        } else if (td.a_only_patches > 0 && td.b_only_patches == 0 &&
                   td.changed_at_shared == 0) {
            b_drops.push_back(&td);
        } else if (td.changed_at_shared > 0 || td.b_only_patches > 0 ||
                   td.a_only_patches > 0) {
            b_changes.push_back(&td);
        }
    }

    // Headline rule: tunes are structurally identical.
    if (result.a_only_patches == 0 && result.b_only_patches == 0 &&
        result.changed_at_shared == 0) {
        out.push_back(
            "B is structurally identical to A — no patches differ.");
        return out;
    }

    // Headline rule: convergence direction. If B's footprint is
    // markedly smaller than A's (B drops a bunch of A's retunes
    // without adding much), call it "converging toward stock".
    if (!b_drops.empty() && b_adds.empty() && b_changes.empty() &&
        result.b_only_patches == 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "B drops %zu of A's retunes without adding new ones — "
                      "converging toward base.",
                      b_drops.size());
        out.emplace_back(buf);
    }

    // Rule: per-cell uniform-delta detection on shared patches. Walk
    // A's patches deduped to effective-state (last-at-key wins), find
    // the matching B effective patch, and detect the dominant uniform
    // delta across uint8 / uint16 BE / uint16 LE cell views. Bin by
    // (delta, width, endianness); the largest bin becomes prose if it
    // covers ≥3 patches AND ≥16 matching cells (filters single-scalar
    // noise without losing real wide-table deltas like the WRK2→WRK3
    // wastegate +2 LE = +512 BE across 112 of 238 uint16 cells).
    struct Bucket {
        std::uint64_t matching_cells{0};
        std::uint64_t total_cells{0};
        std::uint32_t patches{0};
    };
    std::map<std::tuple<int, std::size_t, bool>, Bucket> bins;

    // Dedupe A's patches to last-occurrence at each (rom, ram). A
    // Fehr-class .ptm has ~1000 duplicate-key patches; without dedup
    // the prior implementation paired the wrong A-instance against B's
    // first match and reported noise like "uniform -8 across 67 cells".
    std::map<std::pair<std::uint32_t, std::int32_t>, PatchEntry const *> a_last;
    for (auto const &pa : a.patches) {
        a_last[{pa.rom_offset, pa.ram_offset}] = &pa;
    }
    for (auto const &[key, pa_ptr] : a_last) {
        auto const &pa = *pa_ptr;
        auto const *pb = find_matching_b_effective(pa, b);
        if (pb == nullptr || pa.bytes == pb->bytes) {
            continue;
        }
        auto const cd = best_cell_delta(pa.bytes, pb->bytes);
        if (cd.matching_cells == 0) {
            continue;
        }
        auto &bucket = bins[{cd.delta, cd.width, cd.big_endian}];
        bucket.matching_cells += cd.matching_cells;
        bucket.total_cells += cd.total_cells;
        bucket.patches += 1;
    }

    // Pick the dominant bucket. Tie-break: prefer the WIDER cell view.
    // For a uint16 table tuned by +2 LE per cell, the byte view sees
    // 2× as many "matching cells" (every low-byte +2) and would win on
    // count, but the uint16 LE / BE reading is more semantically
    // useful (matches how a tuner thinks about the cal value). When a
    // wider view's matching_cells is at least floor(narrow / width),
    // prefer it.
    auto const *top_key =
        static_cast<std::tuple<int, std::size_t, bool> const *>(nullptr);
    Bucket top_bucket;
    for (auto const &[k, v] : bins) {
        std::size_t const k_width = std::get<1>(k);
        std::size_t const top_width =
            top_key == nullptr ? 0 : std::get<1>(*top_key);
        // Normalize coverage to byte-equivalent so width-1 and width-2
        // bins compare on the same scale.
        std::uint64_t const v_cov = v.matching_cells * k_width;
        std::uint64_t const top_cov =
            top_bucket.matching_cells * (top_width == 0 ? 1 : top_width);
        bool take = false;
        if (v_cov > top_cov) {
            take = true;
        } else if (v_cov == top_cov && k_width > top_width) {
            take = true; // tie on coverage → prefer wider semantic view
        }
        if (take) {
            top_key = &k;
            top_bucket = v;
        }
    }
    if (top_key != nullptr && top_bucket.patches >= 1 &&
        top_bucket.matching_cells >= 16) {
        int const delta = std::get<0>(*top_key);
        std::size_t const width = std::get<1>(*top_key);
        bool const be = std::get<2>(*top_key);
        char const *sign = delta > 0 ? "+" : "";
        char const *unit =
            width == 1 ? "byte" : (be ? "uint16 BE" : "uint16 LE");
        char buf[320];
        if (top_bucket.matching_cells == top_bucket.total_cells) {
            (void)std::snprintf(buf, sizeof(buf),
                                "B applies a uniform %s%d %s delta across all "
                                "%llu cells in %u patch%s — uniform scaler bump.",
                                sign, delta, unit,
                                static_cast<unsigned long long>(
                                    top_bucket.matching_cells),
                                top_bucket.patches,
                                top_bucket.patches == 1 ? "" : "es");
        } else {
            (void)std::snprintf(buf, sizeof(buf),
                                "B shifts %llu of %llu %s cells by %s%d in %u "
                                "patch%s — looks like a targeted retune within "
                                "a wider table.",
                                static_cast<unsigned long long>(
                                    top_bucket.matching_cells),
                                static_cast<unsigned long long>(
                                    top_bucket.total_cells),
                                unit, sign, delta, top_bucket.patches,
                                top_bucket.patches == 1 ? "" : "es");
        }
        out.emplace_back(buf);
    }

    // Rule: B adds an Intake-AVCS layer (Fehr-style) on top of A.
    auto const matches_avcs_intake = [](TableDiff const *td) {
        return icontains(td->table_name, "AVCS Intake") ||
               icontains(td->table_name, "Intake AVCS") ||
               icontains(td->table_name, "Intake Cam") ||
               icontains(td->table_name, "AVCS - Intake");
    };
    auto const matches_avcs_exhaust = [](TableDiff const *td) {
        return icontains(td->table_name, "AVCS Exhaust") ||
               icontains(td->table_name, "Exhaust AVCS") ||
               icontains(td->table_name, "Exhaust Cam") ||
               icontains(td->table_name, "AVCS - Exhaust");
    };
    bool const b_adds_intake_avcs =
        std::any_of(b_adds.begin(), b_adds.end(), matches_avcs_intake);
    bool const b_drops_intake_avcs =
        std::any_of(b_drops.begin(), b_drops.end(), matches_avcs_intake);
    bool const b_touches_exhaust_avcs =
        std::any_of(b_adds.begin(), b_adds.end(), matches_avcs_exhaust) ||
        std::any_of(b_changes.begin(), b_changes.end(), matches_avcs_exhaust);
    if (b_adds_intake_avcs) {
        out.push_back(
            "B adds an Intake AVCS retune A lacked — moving toward "
            "Fehr-style independent AVCS.");
    }
    if (b_drops_intake_avcs && b_touches_exhaust_avcs) {
        out.push_back(
            "B drops Intake AVCS while keeping Exhaust — converging "
            "back toward COBB / NexGen style.");
    }

    // Rule: Requested Torque revert/add — visible signature for
    // drivability retunes.
    auto const has_rt_in = [](std::vector<TableDiff const *> const &v) {
        return std::any_of(v.begin(), v.end(), [](TableDiff const *td) {
            return icontains(td->table_name, "Requested Torque");
        });
    };
    if (has_rt_in(b_drops) && !has_rt_in(b_adds)) {
        std::size_t n = 0;
        std::uint64_t bytes = 0;
        for (auto const *td : b_drops) {
            if (icontains(td->table_name, "Requested Torque")) {
                ++n;
                bytes += td->a_only_bytes;
            }
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "B reverts the Requested Torque edits A carried (%zu "
                      "tables, %lluB).",
                      n,
                      static_cast<unsigned long long>(bytes));
        out.emplace_back(buf);
    } else if (has_rt_in(b_adds) && !has_rt_in(b_drops)) {
        out.push_back(
            "B adds Requested Torque edits A lacked — drivability / "
            "torque-shaping pass.");
    }

    // Rule: B adds wastegate edits (boost-up) on top of A.
    bool const b_adds_wg = std::any_of(
        b_adds.begin(), b_adds.end(), [](TableDiff const *td) {
            return icontains(td->table_name, "Wastegate Duty");
        });
    if (b_adds_wg) {
        out.push_back(
            "B adds Wastegate Duty edits A didn't have — boost-up "
            "layered on top.");
    }

    // Rule: B adds Target Throttle edits — Stage-tier addition.
    std::size_t tt_adds = 0;
    for (auto const *td : b_adds) {
        if (icontains(td->table_name, "Target Throttle")) {
            ++tt_adds;
        }
    }
    if (tt_adds >= 4) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "B adds %zu Target Throttle variants A lacked — "
                      "Stage-tier throttle remap layered on.",
                      tt_adds);
        out.emplace_back(buf);
    }

    return out;
}

} // namespace st::library
