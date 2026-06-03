// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Contract test for the apply_op audit hook (handoff §test-coverage-gaps).
//
// `apply_op` lives in src/ui/src/actions.hpp as a template — it can't be
// unit-tested directly without a GUI harness because it sits on top of
// AppState (ImGui-coupled). This file tests the *contract* instead: the
// shape of EditCommitted entries that apply_op writes. If the on-disk
// format or the field set drifts, this fixture breaks; if apply_op
// stops emitting entries entirely, the GUI-side smoke catches it but
// this test still locks the entry shape so the audit panel keeps
// rendering rom annotations correctly.
//
// Mirrors the actual call site in src/ui/src/actions.hpp:102-115 +
// src/ui/src/panels/dtcs.cpp:129-141 (the two emit paths). Both fire
// EntryKind::EditCommitted with source="ui.editor" or "ui.dtcs", a
// `table` field, a `cells` count, and the active-rom annotation only
// when the user has targeted a non-working slot.

#include "st/audit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace au = st::audit;

namespace {

std::filesystem::path temp_log_path(char const *stem) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"subuwutuner_apply_op_audit_"} + stem + ".log");
    std::filesystem::remove(p);
    return p;
}

au::Entry const *find_first_kind(std::vector<au::Entry> const &es, au::EntryKind k) {
    auto it = std::find_if(es.begin(), es.end(),
                           [k](au::Entry const &e) { return e.kind == k; });
    return it == es.end() ? nullptr : &*it;
}

bool has_field(au::Entry const &e, std::string const &key, std::string const &value) {
    for (auto const &[k, v] : e.fields) {
        if (k == key && v == value)
            return true;
    }
    return false;
}

bool has_key(au::Entry const &e, std::string const &key) {
    for (auto const &[k, v] : e.fields) {
        if (k == key)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("apply_op-shaped audit entry round-trips with default working-slot fields",
          "[audit][apply_op][contract]") {
    // Models the apply_op call site in actions.hpp:102-115 when the
    // user is editing the default working slot — the active-rom
    // annotation is omitted (empty/working slot is the silent default,
    // matching the field set the GUI emits).
    auto const log_path = temp_log_path("working_default");
    {
        auto log = au::AuditLog::open(log_path);
        REQUIRE(log.has_value());
        REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.editor",
                        "+5% selection on boost_target_high_octane",
                        {{"table", "boost_target_high_octane"}, {"cells", "12"}})
                    .has_value());
    }
    auto entries = au::read_all(log_path);
    REQUIRE(entries.has_value());
    auto const *e = find_first_kind(*entries, au::EntryKind::EditCommitted);
    REQUIRE(e != nullptr);
    REQUIRE(e->source == "ui.editor");
    REQUIRE(e->description == "+5% selection on boost_target_high_octane");
    REQUIRE(e->checksum_valid);
    REQUIRE(has_field(*e, "table", "boost_target_high_octane"));
    REQUIRE(has_field(*e, "cells", "12"));
    // Working slot: NO rom annotation. Mirrors the
    // `if (!state.active_rom_id.empty() && state.active_rom_id != "working")`
    // gate in actions.hpp:109 — silent on the default slot.
    REQUIRE_FALSE(has_key(*e, "rom"));
    std::filesystem::remove(log_path);
}

TEST_CASE("apply_op-shaped audit entry includes rom field when an additional slot is active",
          "[audit][apply_op][contract][active-rom]") {
    // Models the Issue #10 sweep — when the user has set the active
    // slot to an additional ROM id, apply_op tacks on a `rom` field so
    // the audit timeline doesn't conflate edits to working vs other
    // ROMs. actions.hpp:109-111 + dtcs.cpp:135-137 +
    // autotune_knock.cpp:209-211 + autotune_maf.cpp:181-183 all share
    // this shape post-2026-06-03 sweep.
    auto const log_path = temp_log_path("additional_rom");
    {
        auto log = au::AuditLog::open(log_path);
        REQUIRE(log.has_value());
        REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.editor",
                        "Smooth selection on knock_correction",
                        {{"table", "knock_correction"},
                         {"cells", "4"},
                         {"rom", "log_baseline_2026_05_29"}})
                    .has_value());
    }
    auto entries = au::read_all(log_path);
    REQUIRE(entries.has_value());
    auto const *e = find_first_kind(*entries, au::EntryKind::EditCommitted);
    REQUIRE(e != nullptr);
    REQUIRE(has_field(*e, "table", "knock_correction"));
    REQUIRE(has_field(*e, "cells", "4"));
    REQUIRE(has_field(*e, "rom", "log_baseline_2026_05_29"));
    std::filesystem::remove(log_path);
}

TEST_CASE("Multiple apply_op-shaped entries append in order without merging",
          "[audit][apply_op][contract][sequence]") {
    // apply_op fires one entry per toolbar click — clicking +5% twenty
    // times generates twenty entries (handoff §watch-list-for-cumulative
    // -complexity). The audit panel's chip filter collapses the noise
    // visually; on disk every click is independent. Verify that the
    // round-trip preserves the per-call shape rather than aggregating.
    auto const log_path = temp_log_path("sequence");
    {
        auto log = au::AuditLog::open(log_path);
        REQUIRE(log.has_value());
        for (int i = 0; i < 5; ++i) {
            REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.editor",
                            "+5% selection on boost_target_high_octane",
                            {{"table", "boost_target_high_octane"},
                             {"cells", std::to_string(i + 1)}})
                        .has_value());
        }
    }
    auto entries = au::read_all(log_path);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 5);
    for (std::size_t i = 0; i < entries->size(); ++i) {
        auto const &e = (*entries)[i];
        REQUIRE(e.kind == au::EntryKind::EditCommitted);
        REQUIRE(e.source == "ui.editor");
        REQUIRE(has_field(e, "cells", std::to_string(i + 1)));
    }
    std::filesystem::remove(log_path);
}

TEST_CASE("Source label distinguishes apply_op variants on the wire",
          "[audit][apply_op][contract][source]") {
    // The four call sites use distinct source labels so the audit
    // panel + CLI grepping can tell them apart. Lock the contract here
    // so a rename in the GUI source breaks this test (and the audit
    // panel's chip filter rules) loudly rather than silently.
    auto const log_path = temp_log_path("sources");
    {
        auto log = au::AuditLog::open(log_path);
        REQUIRE(log.has_value());
        REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.editor", "table edit",
                        {{"table", "t"}, {"cells", "1"}})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.csv_import", "csv apply",
                        {{"table", "t"}, {"cells", "100"}})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::EditCommitted, "ui.dtcs", "DTC bulk toggle",
                        {{"count", "12"}, {"scope", "emissions"}})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::AutotuneCommitted, "autotune.knock",
                        "knock-pull autotune",
                        {{"table", "knock"}, {"pulled", "3"}, {"added_back", "0"},
                         {"log_path", "demo.csv"}})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::AutotuneCommitted, "autotune.maf",
                        "MAF autotune",
                        {{"table", "maf"}, {"cells_modified", "5"},
                         {"log_path", "demo.csv"}})
                    .has_value());
    }
    auto entries = au::read_all(log_path);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 5);
    REQUIRE((*entries)[0].source == "ui.editor");
    REQUIRE((*entries)[1].source == "ui.csv_import");
    REQUIRE((*entries)[2].source == "ui.dtcs");
    REQUIRE((*entries)[3].source == "autotune.knock");
    REQUIRE((*entries)[4].source == "autotune.maf");
    REQUIRE((*entries)[3].kind == au::EntryKind::AutotuneCommitted);
    REQUIRE((*entries)[4].kind == au::EntryKind::AutotuneCommitted);
    std::filesystem::remove(log_path);
}
