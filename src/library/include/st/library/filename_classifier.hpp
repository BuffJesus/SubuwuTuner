// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::classify_ptm_filename — best-effort vendor / stage /
// fuel-grade / variant inference from a .ptm basename. C++ port of
// `tools/library_inventory/inventory.py:parse_filename_hints`, so the
// GUI's AP browser can label every .ptm in the file vault without
// requiring the user to have run the inventory step.
//
// F2 (Tune Family browser) wedge: surface the family / variant info
// the user can read from filenames anyway, in a parsed shape the UI
// can use for grouping + filter chips. Every field is "weak signal"
// — a match means consistent with the label, not a guarantee.

#ifndef ST_LIBRARY_FILENAME_CLASSIFIER_HPP
#define ST_LIBRARY_FILENAME_CLASSIFIER_HPP

#include <string>
#include <string_view>

namespace st::library {

struct PtmFilenameClassification {
    std::string vendor;     // "COBB" / "NexGen" / "Fehr" / "NTM" / "Felix" / "EpifanSoft"
    std::string stage;      // "Stage0" / "Stage1" / "Stage2" / "Stage3"
    std::string fuel_grade; // "91" / "93" / "94" / "E85" / "E50" / "E30" / "Race"
    // " / "-joined run of variant tokens. Possible tokens: WRK<n>,
    // v<NNN>, Redline, SF, BigSF, Economy, Valet, HWG, LWG. Order
    // follows their occurrence in the filename.
    std::string variant;
};

[[nodiscard]] PtmFilenameClassification
classify_ptm_filename(std::string_view name);

} // namespace st::library

#endif // ST_LIBRARY_FILENAME_CLASSIFIER_HPP
