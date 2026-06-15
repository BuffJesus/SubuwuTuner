// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Pins the heuristic patterns the GUI's AP browser uses to label
// every .ptm with family / variant / fuel-grade. Mirrors the seven
// canonical filename shapes that show up across the user's AP
// corpus (per docs/40 and the F2 wedge handoff).

#include "st/library/filename_classifier.hpp"

#include <catch2/catch_test_macros.hpp>

using st::library::classify_ptm_filename;

TEST_CASE("classify_ptm_filename: bare COBB Stage1", "[library][classifier]") {
    auto const c = classify_ptm_filename("Stage1 91 v401.ptm");
    CHECK(c.vendor == "COBB");
    CHECK(c.stage == "Stage1");
    CHECK(c.fuel_grade == "91");
    CHECK(c.variant == "v401");
}

TEST_CASE("classify_ptm_filename: NexGen Stage2 + BigSF",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("NexGen Stage2 BigSF 93 v401.ptm");
    CHECK(c.vendor == "NexGen");
    CHECK(c.stage == "Stage2");
    CHECK(c.fuel_grade == "93");
    // BigSF comes before v401 in the variant string because the
    // implementation prepends pattern hits (WRK*/v*) before literals.
    CHECK(c.variant.find("BigSF") != std::string::npos);
    CHECK(c.variant.find("v401") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: Fehr WRK lineage", "[library][classifier]") {
    auto const c = classify_ptm_filename(
        "Fehr_DMann CAN 93 ETune (Reflash-WRK3).ptm");
    // DMann should resolve to Fehr (DMann = tuner, Fehr = owner)
    CHECK(c.vendor == "Fehr");
    CHECK(c.fuel_grade == "93");
    CHECK(c.variant.find("WRK3") != std::string::npos);
    CHECK(c.variant.find("ETune") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: Felix-on-FA24 SubuwuTuner-built",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("Felix-on-FA24_v2.ptm");
    CHECK(c.vendor == "Felix");
    CHECK(c.variant.find("v2") != std::string::npos); // Wait — v2 is 1 digit
}

TEST_CASE("classify_ptm_filename: NexGen Stage2 +SF +Redline equivalent",
          "[library][classifier]") {
    auto const c =
        classify_ptm_filename("NexGen Stage2 91 Redline v401.ptm");
    CHECK(c.vendor == "NexGen");
    CHECK(c.stage == "Stage2");
    CHECK(c.fuel_grade == "91");
    CHECK(c.variant.find("Redline") != std::string::npos);
    CHECK(c.variant.find("v401") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: wastegate-mode token (HWG / LWG)",
          "[library][classifier]") {
    auto const high =
        classify_ptm_filename("Stage1 HWG 93 v401.ptm");
    CHECK(high.vendor == "COBB");
    CHECK(high.stage == "Stage1");
    CHECK(high.variant.find("HWG") != std::string::npos);

    auto const low = classify_ptm_filename("Stage1 LWG 93 v401.ptm");
    CHECK(low.variant.find("LWG") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: Valet / Economy modes",
          "[library][classifier]") {
    auto const valet = classify_ptm_filename("Stage0 Valet 91.ptm");
    CHECK(valet.stage == "Stage0");
    CHECK(valet.variant.find("Valet") != std::string::npos);

    auto const eco = classify_ptm_filename("Stage1 Economy 93.ptm");
    CHECK(eco.variant.find("Economy") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: unknown vendor leaves vendor empty",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("Vehicle Anti-Theft Mode.ptm");
    CHECK(c.vendor.empty());
    CHECK(c.stage.empty());
}

TEST_CASE("classify_ptm_filename: E85 fuel grade", "[library][classifier]") {
    auto const c = classify_ptm_filename("Stage2 E85 v401.ptm");
    CHECK(c.fuel_grade == "E85");
}

TEST_CASE("classify_ptm_filename: empty input is safe",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("");
    CHECK(c.vendor.empty());
    CHECK(c.stage.empty());
    CHECK(c.fuel_grade.empty());
    CHECK(c.variant.empty());
}

TEST_CASE("classify_ptm_filename: case-insensitive matching",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("nexgen STAGE1 91.PTM");
    CHECK(c.vendor == "NexGen");
    // Stage canonicalizes to "Stage1" regardless of input case.
    CHECK(c.stage == "Stage1");
    CHECK(c.fuel_grade == "91");
}

TEST_CASE("classify_ptm_filename: ethanol blend fuel grades",
          "[library][classifier]") {
    auto const e30 = classify_ptm_filename("Stage2 E30 v401.ptm");
    CHECK(e30.fuel_grade == "E30");
    auto const e50 = classify_ptm_filename("Stage2 E50 v401.ptm");
    CHECK(e50.fuel_grade == "E50");
    auto const race = classify_ptm_filename("Stage3 Race v401.ptm");
    CHECK(race.fuel_grade == "Race");
}

TEST_CASE("classify_ptm_filename: 94 octane (Canadian fuel)",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("Stage1 94 v401.ptm");
    CHECK(c.fuel_grade == "94");
}

TEST_CASE("classify_ptm_filename: COBB token without bare Stage",
          "[library][classifier]") {
    // Some COBB OTS .ptm files include the COBB token explicitly but
    // not a bare Stage<N> (e.g. accessory configs, anti-theft).
    auto const c = classify_ptm_filename("COBB Accessory Config v2.ptm");
    CHECK(c.vendor == "COBB");
    CHECK(c.stage.empty());
}

TEST_CASE("classify_ptm_filename: multi-variant Stage1+SF+BigSF chain",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("Stage1+SF+BigSF 93 v401.ptm");
    CHECK(c.stage == "Stage1");
    CHECK(c.fuel_grade == "93");
    // BigSF should match before bare SF in the variant string since
    // BigSF appears earlier in the literal table and string position
    // — but both should appear since the literal scan covers both.
    CHECK(c.variant.find("SF") != std::string::npos);
    CHECK(c.variant.find("BigSF") != std::string::npos);
    CHECK(c.variant.find("v401") != std::string::npos);
}

TEST_CASE("classify_ptm_filename: WRK lineage with high WRK number",
          "[library][classifier]") {
    auto const c = classify_ptm_filename("Fehr_WRK12.ptm");
    CHECK(c.vendor == "Fehr");
    CHECK(c.variant.find("WRK12") != std::string::npos);
}
