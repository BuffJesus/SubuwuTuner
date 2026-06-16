// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::annotate_tune — derive at-a-glance badges for a TuneIndex
// entry. Composes:
//
//   - TuneIndexEntry (filename + vendor heuristic from inventory.py)
//   - st::library::Atlas (corpus-derived tuner clusters + safety pairs)
//   - Optional AP cross-reference inputs (currently-flashed MD5 +
//     /maps/ basenames the AP currently carries)
//
// Returns a TuneAnnotation with boolean facets. The library panel
// renders these as chips next to the row name. Pure read-only —
// no decrypt, no atlas mutation. Hardware-free, cipher-free, lane-
// neutral.
//
// NOTE: deep per-table annotations (does this tune touch the 50-
// table common-core? does it fire safety pair X?) require .ptm
// decrypt + atlas table-mapping and live behind the cipher build
// flag. annotate_tune returns shallow flags only; the deep path is
// out of scope for v1 of the Library panel.

#ifndef ST_LIBRARY_TUNE_ANNOTATION_HPP
#define ST_LIBRARY_TUNE_ANNOTATION_HPP

#include "st/library/atlas.hpp"
#include "st/library/tune_index.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace st::library {

// Optional AP cross-reference inputs. Caller passes nullopt / empty
// when the AP isn't connected; annotate_tune degrades gracefully —
// the AP-side facets just stay false.
struct ApCrossRefInputs {
    // /backupcksum value the AP exposes (per J3 RE: MD5 of plaintext
    // stock ROM). Lowercase hex, 32 chars. Empty / nullopt when not
    // fetched.
    std::optional<std::string> backupcksum;
    // Basenames of .ptm files in /maps/ on the connected AP. Empty
    // when no AP or /maps/ tab unvisited.
    std::vector<std::string> maps_filenames;
};

struct TuneAnnotation {
    // Cross-corpus tuner-family flag, derived from the entry's
    // `vendor` field against the atlas's tuner.display_name list. Empty
    // when the vendor doesn't match any known cluster.
    std::string tuner_family;
    // Specific signature flags from cross-corpus findings.
    bool fehr_family{false};       // intake AVCS retune signature
    bool cobb_nexgen{false};       // exhaust-only AVCS philosophy
    bool ntm_swap_basemap{false};  // FA24 swap basemap
    // AP cross-reference facets. False when no AP / no inputs.
    bool currently_flashed{false}; // MD5 matches /backupcksum
    bool on_ap{false};             // basename appears in /maps/
};

[[nodiscard]] TuneAnnotation annotate_tune(TuneIndexEntry const &entry,
                                           Atlas const &atlas,
                                           ApCrossRefInputs const &ap = {});

// Case-insensitive substring match used internally by annotate_tune.
// Exposed so test code can drive the same predicate against synthetic
// fixtures without depending on the full atlas.
[[nodiscard]] bool annotation_vendor_matches(std::string_view vendor,
                                              std::string_view tuner_display_name) noexcept;

} // namespace st::library

#endif // ST_LIBRARY_TUNE_ANNOTATION_HPP
