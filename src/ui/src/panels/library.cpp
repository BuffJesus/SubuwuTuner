// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tune Library panel — primary surface for the user's .ptm tune
// collection. Reads `library_index.toml` (produced by
// `tools/library_inventory/inventory.py`), surfaces vendor / stage /
// variant per tune, and gates further actions on the AccessPort
// browser's connection state (currently-flashed cross-reference,
// Pull-and-import combo, etc.). Per the analyst's 2026-06-13 strategic
// reply (`findings/handoffs/HANDOFF-from-analyst-2026-06-13-ap-browser-decisions.md`)
// this becomes the primary tune-surface; the AccessPort Browser
// recedes to a filesystem-y detail view.
//
// v1 scope (this skeleton):
// - Load TuneIndex from default-discovery path on first render
// - Header strip: count, scan-source, agreement chip vs project
// - Filter input + vendor facet
// - Tune-card list (basic — vendor / stage / variant / size / md5)
// - "Re-scan library" button (re-runs the load)
// - Empty state when no index found
//
// v2+ (NOT in this skeleton — wired in follow-up commits):
// - /backupcksum cross-reference once `st::config::Paths::library_root`
//   lands (Phase 1 of the library posture work)
// - Atlas (st::library::Atlas) annotations: common-core / safety-pair
//   detection per tune
// - Per-card actions (Inspect, Diff, Pull from AP, Import as project)
// - Threading via the worker queue (analyst Strategic Q C answer = C2)
//
// File-static panel state — pattern matches ets_browser.cpp.

#include "panels/panels.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/core/shell_safe.hpp"
#include "st/library/atlas.hpp"
#include "st/library/tune_annotation.hpp"
#include "st/library/tune_index.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace st::ui {

namespace {

struct PanelState {
    std::optional<st::library::TuneIndex> index;
    std::string load_error;      // empty when index loaded cleanly OR not attempted
    bool load_attempted{false};  // distinguishes "not loaded yet" from "load failed"

    // Filters.
    std::array<char, 128> filter_buf{};
    std::string vendor_filter; // empty = "Any vendor"
    // L3 — additional facets, settable from the facet sidebar. Empty
    // string means "any". The sidebar's collapsing header + clickable
    // items mutate these in place; apply_filters() composes them with
    // the substring + vendor predicates.
    std::string stage_filter;
    std::string fuel_filter;
    // L3 — sidebar visibility toggle. Default on; user can hide it for
    // a wider table on narrow viewports.
    bool show_facet_sidebar{true};
    // L1 — view-mode toggle. Default rows (preserves existing layout +
    // sort + column widths). Cards mode renders a flow-wrapped grid of
    // tune cards with the same per-row context menu + L2 pinning.
    bool show_as_cards{false};

    // Atlas — loaded lazily on first render via the same default-path
    // discovery as the CLI's `tuner-atlas` subcommand. Empty on
    // failure (no badges rendered) so the panel degrades cleanly.
    std::optional<st::library::Atlas> atlas;
    bool atlas_load_attempted{false};
};

PanelState &panel() {
    static PanelState s;
    return s;
}

void load_index(PanelState &p) {
    p.load_attempted = true;
    auto const path = st::library::default_index_path();
    if (!path.has_value()) {
        p.index.reset();
        p.load_error =
            "No library_index.toml found. Run "
            "`python tools/library_inventory/inventory.py` to scan your "
            ".ptm files, or set ST_LIBRARY_INDEX to an existing index path.";
        return;
    }
    auto loaded = st::library::TuneIndex::load_from_file(*path);
    if (!loaded.has_value()) {
        p.index.reset();
        p.load_error = "Load failed: " + loaded.error().to_string();
        return;
    }
    p.index = std::move(*loaded);
    p.load_error.clear();
}

// Lazy-load the atlas the first time the panel renders. Failure is
// silent — annotate_tune is robust to no atlas and just emits no
// badges. The atlas path uses the same env-override + walk pattern
// as the CLI `tuner-atlas` subcommand: ST_TUNER_ATLAS env var,
// otherwise fixtures/tuner_atlas/tuner_atlas.toml discovered up the
// cwd chain.
void load_atlas_lazy(PanelState &p) {
    if (p.atlas_load_attempted) {
        return;
    }
    p.atlas_load_attempted = true;
    if (char const *override_path = std::getenv("ST_TUNER_ATLAS")) {
        if (auto r = st::library::Atlas::load_from_file(override_path);
            r.has_value()) {
            p.atlas = std::move(*r);
            return;
        }
    }
    std::filesystem::path cwd = std::filesystem::current_path();
    for (int hops = 0; hops < 6; ++hops) {
        auto const candidate =
            cwd / "fixtures" / "tuner_atlas" / "tuner_atlas.toml";
        if (std::filesystem::exists(candidate)) {
            if (auto r = st::library::Atlas::load_from_file(candidate);
                r.has_value()) {
                p.atlas = std::move(*r);
                return;
            }
        }
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) {
            break;
        }
        cwd = cwd.parent_path();
    }
}

// Distinct vendor list across the loaded entries — used by the
// facet selector. Order: alphabetical, with "(unknown)" sentinel
// at the end for entries whose filename heuristic didn't infer.
std::vector<std::string> distinct_vendors(st::library::TuneIndex const &idx) {
    std::vector<std::string> out;
    bool saw_unknown = false;
    for (auto const &e : idx.ptm_entries()) {
        if (e.vendor.empty()) {
            saw_unknown = true;
            continue;
        }
        if (std::find(out.begin(), out.end(), e.vendor) == out.end()) {
            out.push_back(e.vendor);
        }
    }
    std::sort(out.begin(), out.end());
    if (saw_unknown) {
        out.emplace_back("(unknown)");
    }
    return out;
}

// L3 — distinct stage list. Matches the vendor pattern: alpha-sorted
// with "(unknown)" sentinel at the end when any entry has an empty
// stage field. Used by render_facet_sidebar.
[[maybe_unused]] std::vector<std::string>
distinct_stages(st::library::TuneIndex const &idx) {
    std::vector<std::string> out;
    bool saw_unknown = false;
    for (auto const &e : idx.ptm_entries()) {
        if (e.stage.empty()) {
            saw_unknown = true;
            continue;
        }
        if (std::find(out.begin(), out.end(), e.stage) == out.end()) {
            out.push_back(e.stage);
        }
    }
    std::sort(out.begin(), out.end());
    if (saw_unknown) {
        out.emplace_back("(unknown)");
    }
    return out;
}

// L3 — distinct fuel-grade list.
[[maybe_unused]] std::vector<std::string>
distinct_fuels(st::library::TuneIndex const &idx) {
    std::vector<std::string> out;
    bool saw_unknown = false;
    for (auto const &e : idx.ptm_entries()) {
        if (e.fuel_grade.empty()) {
            saw_unknown = true;
            continue;
        }
        if (std::find(out.begin(), out.end(), e.fuel_grade) == out.end()) {
            out.push_back(e.fuel_grade);
        }
    }
    std::sort(out.begin(), out.end());
    if (saw_unknown) {
        out.emplace_back("(unknown)");
    }
    return out;
}

// L3 — shared facet predicate. `(unknown)` sentinel matches the empty-
// string entry path; otherwise exact-match. Empty filter is "any".
[[nodiscard]] bool facet_matches(std::string_view filter,
                                  std::string_view entry_value) {
    if (filter.empty()) {
        return true;
    }
    if (filter == "(unknown)") {
        return entry_value.empty();
    }
    return filter == entry_value;
}

// Filter resolution: substring AND vendor-facet AND L3 stage / fuel
// facets. Returns indices into idx.ptm_entries() so the rendering loop
// can render in stable order.
std::vector<std::size_t> apply_filters(st::library::TuneIndex const &idx,
                                        std::string_view name_needle,
                                        std::string_view vendor_filter,
                                        std::string_view stage_filter = {},
                                        std::string_view fuel_filter = {}) {
    auto const by_name = idx.filter_by_name(name_needle);
    if (vendor_filter.empty() && stage_filter.empty() &&
        fuel_filter.empty()) {
        return by_name;
    }
    std::vector<std::size_t> out;
    out.reserve(by_name.size());
    for (auto i : by_name) {
        auto const &e = idx.ptm_entries()[i];
        if (!facet_matches(vendor_filter, e.vendor)) {
            continue;
        }
        if (!facet_matches(stage_filter, e.stage)) {
            continue;
        }
        if (!facet_matches(fuel_filter, e.fuel_grade)) {
            continue;
        }
        out.push_back(i);
    }
    return out;
}

// L3 — count tunes per facet value, used by the sidebar to surface a
// per-row count badge ("Fehr (12)"). Skips entries already filtered
// out by the OTHER active facets so the user sees a live "how many
// match if I click this" value, not the corpus-wide total.
[[nodiscard, maybe_unused]] std::size_t
count_for_facet(st::library::TuneIndex const &idx,
                std::string_view name_needle,
                std::string_view candidate_vendor,
                std::string_view candidate_stage,
                std::string_view candidate_fuel) {
    auto const by_name = idx.filter_by_name(name_needle);
    std::size_t n = 0;
    for (auto i : by_name) {
        auto const &e = idx.ptm_entries()[i];
        if (!facet_matches(candidate_vendor, e.vendor)) {
            continue;
        }
        if (!facet_matches(candidate_stage, e.stage)) {
            continue;
        }
        if (!facet_matches(candidate_fuel, e.fuel_grade)) {
            continue;
        }
        ++n;
    }
    return n;
}

void render_empty_state_text(PanelState &p) {
    if (p.load_error.empty()) {
        render_empty_state(
            "Library not loaded yet",
            "Click 'Scan library' below to load library_index.toml. The index "
            "is produced by tools/library_inventory/inventory.py — run that "
            "first to scan your local .ptm collection.");
    } else {
        render_empty_state("Library load failed", p.load_error.c_str());
    }
}

// Render the header strip: total count, scan source, filter input,
// vendor facet, agreement chip vs the loaded project.
void render_header(PanelState &p, AppState &state) {
    if (!p.index.has_value()) {
        text_subtle("No library index loaded.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Scan library##library_load")) {
            load_index(p);
        }
        return;
    }
    auto const &idx = *p.index;
    char hdr[160];
    std::snprintf(hdr, sizeof hdr,
                  "%zu tunes in library  \xC2\xB7  %zu .stune projects",
                  idx.ptm_entries().size(), idx.stune_entries().size());
    ImGui::TextUnformatted(hdr);
    if (!idx.source_path().empty()) {
        ImGui::SameLine();
        text_subtle("\xC2\xB7  %s", idx.source_path().string().c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Re-read TOML##library_rescan")) {
        load_index(p);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-read library_index.toml from disk.\n"
                          "Fast (<1 ms). Use 'Full rescan' below if\n"
                          "you've added new .ptm files since the\n"
                          "index was last written.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Full rescan\xE2\x80\xA6##library_full_rescan")) {
        // Look for the inventory script via env override first; then
        // walk up from cwd. Same discovery posture as
        // default_index_path + the analyst's tuner-atlas CLI default-
        // path walk.
        std::filesystem::path script_path;
        if (char const *raw = std::getenv("ST_LIBRARY_INVENTORY_PYTHON");
            raw != nullptr && raw[0] != '\0') {
            script_path = raw;
        } else {
            std::error_code ec;
            auto walk = std::filesystem::current_path(ec);
            for (int hops = 0; hops < 6 && !walk.empty(); ++hops) {
                auto candidate =
                    walk / "tools" / "library_inventory" / "inventory.py";
                if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                    script_path = candidate;
                    break;
                }
                auto const parent = walk.parent_path();
                if (parent == walk) {
                    break;
                }
                walk = parent;
            }
        }
        if (script_path.empty()) {
            enqueue_toast(state, ToastKind::Warn,
                          "Full rescan: couldn't find inventory.py. Set "
                          "ST_LIBRARY_INVENTORY_PYTHON or run from a repo "
                          "checkout. Falling back to re-read TOML.");
            load_index(p);
        } else {
            // Run inventory.py and wait. The script reads cwd-relative
            // default scan paths and writes library_index.toml + the
            // summary into --out-dir. Aim it at the same dir the
            // existing index lives in (when known) or cwd otherwise.
            std::filesystem::path out_dir;
            if (p.index.has_value() && !p.index->source_path().empty()) {
                out_dir = p.index->source_path().parent_path();
            } else {
                std::error_code ec;
                out_dir = std::filesystem::current_path(ec);
            }
            std::string cmd = "python \"";
            cmd += script_path.string();
            cmd += "\" --quiet --out-dir \"";
            cmd += out_dir.string();
            cmd += "\"";
            int const rc = std::system(cmd.c_str());
            if (rc != 0) {
                enqueue_toast(
                    state, ToastKind::Warn,
                    "Full rescan: inventory.py exited with code " +
                        std::to_string(rc) +
                        ". Python on PATH? Falling back to re-read TOML.");
            } else {
                enqueue_toast(state, ToastKind::Success,
                              "Library rescan complete.");
            }
            // Reload either way — even on failure the existing TOML
            // is still valid.
            load_index(p);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Re-run tools/library_inventory/inventory.py to walk the\n"
            "scan dirs from scratch — picks up new .ptm files. Blocks\n"
            "the UI for ~5 s on a 55-tune library. Needs python on PATH\n"
            "(override via ST_LIBRARY_INVENTORY_PYTHON env var).");
    }

    // Agreement chip — L5: compare AP's machine vehicle_id (cmd 0x28
    // field[5], e.g. "SUBA_US_WRXM_CF_17_F") against the project's
    // [ptm_metadata].vehicle_id. Four states:
    //  - Green "AP matches project" — same vehicle code, push safe
    //  - Red "Vehicle mismatch — push blocked" — different codes
    //  - Grey "AP not connected" — no snapshot
    //  - Grey "No project" — no project loaded
    ImGui::SameLine();
    {
        auto const snap = ets_status_snapshot();
        bool const have_ap = snap.has_value() && !snap->vehicle_id.empty();
        bool const have_proj = state.ptm_imported_vehicle.has_value() &&
                                 !state.ptm_imported_vehicle->empty();
        if (have_ap && have_proj) {
            if (snap->vehicle_id == *state.ptm_imported_vehicle) {
                chip("AP matches project", chip_fg_ok(), chip_bg_ok());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("AP and project both target %s.\n"
                                      "Push is safe.",
                                      snap->vehicle_id.c_str());
                }
            } else {
                chip("Vehicle mismatch — push blocked",
                     chip_fg_danger(), chip_bg_danger());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "AP vehicle: %s\nProject vehicle: %s\n\n"
                        "Pushing this project's tunes to the AP would\n"
                        "target a different ECU than the AP is married to.\n"
                        "Push is blocked.",
                        snap->vehicle_id.c_str(),
                        state.ptm_imported_vehicle->c_str());
                }
            }
        } else if (!have_ap && have_proj) {
            chip("AP not connected", chip_fg_muted(), chip_bg_muted());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Connect an AP for the agreement check.");
            }
        } else if (have_ap && !have_proj) {
            chip("No project", chip_fg_muted(), chip_bg_muted());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open a project to see the agreement state.");
            }
        } else {
            chip("No project", chip_fg_muted(), chip_bg_muted());
        }
    }

    // Filter row.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##library_filter", "Filter by name\xE2\x80\xA6",
                             p.filter_buf.data(), p.filter_buf.size());
    ImGui::SameLine();
    // Vendor facet dropdown — built from the live distinct list so
    // tunes from a vendor we haven't seen yet show up automatically.
    auto const vendors = distinct_vendors(idx);
    std::string const &cur = p.vendor_filter;
    char const *combo_preview = cur.empty() ? "Any vendor" : cur.c_str();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##library_vendor", combo_preview)) {
        if (ImGui::Selectable("Any vendor", cur.empty())) {
            p.vendor_filter.clear();
        }
        for (auto const &v : vendors) {
            bool const sel = (cur == v);
            if (ImGui::Selectable(v.c_str(), sel)) {
                p.vendor_filter = v;
            }
        }
        ImGui::EndCombo();
    }
    // L3 — facet sidebar visibility toggle. The sidebar adds Stage +
    // Fuel facets next to Vendor and surfaces per-facet counts; the
    // header-row Vendor dropdown stays for users who prefer the
    // compact pull-down.
    ImGui::SameLine();
    char const *facet_label = p.show_facet_sidebar
                                  ? "Hide facets##library_facets_toggle"
                                  : "Show facets##library_facets_toggle";
    if (ImGui::SmallButton(facet_label)) {
        p.show_facet_sidebar = !p.show_facet_sidebar;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle the Vendor / Stage / Fuel facet sidebar.\n"
                          "Hidden by default on narrow viewports to give\n"
                          "the entries table more room.");
    }
    // L1 — Rows / Cards view toggle. Cards view is denser visually,
    // wraps to fit the viewport, and works well with the L2 pinned-
    // flashed tune (the flashed card gets an accent border). Rows
    // remain the default to preserve existing user habits.
    ImGui::SameLine();
    char const *view_label = p.show_as_cards
                                  ? "Rows view##library_view_toggle"
                                  : "Cards view##library_view_toggle";
    if (ImGui::SmallButton(view_label)) {
        p.show_as_cards = !p.show_as_cards;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Switch between flat rows (sortable table) and\n"
                          "a flow-wrapped card grid. Both surface the same\n"
                          "context menu + L2 currently-flashed pin.");
    }
}

// Right-click context menu body for a single tune row. Sets state
// fields that the modal-open handlers / dispatch code consume. Most
// items are always shown (greyed when conditions don't allow) so the
// user has a stable mental map of what's possible per row.
void render_row_context_menu(AppState &state,
                              st::library::TuneIndexEntry const &e) {
    // Mark the targeted row so any handler that fires after the
    // EndPopup can read which row was clicked. This is the source of
    // truth for "what path did the user act on"; modal opens set
    // their own per-modal field from here.
    state.library_action_target = e.path;

    // 1. Inspect — opens the ptm_inspect modal with the row's path.
    //    Cipher-gated via build flag; on-OFF builds, render greyed +
    //    explanatory tooltip (D2/D3 posture per strategic decisions).
#ifdef ST_ETS_HAVE_CIPHER
    bool const cipher_on = true;
#else
    bool const cipher_on = false;
#endif
    if (!cipher_on) {
        ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Inspect\xE2\x80\xA6")) {
        std::snprintf(state.ptm_inspect_ptm_path,
                      sizeof state.ptm_inspect_ptm_path, "%s",
                      e.path.c_str());
        state.show_ptm_inspect_modal = true;
    }
    if (!cipher_on) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (cipher_on) {
            ImGui::SetTooltip("Open this tune in the Inspect modal —\n"
                              "vendor / vehicle / patch count / layer\n"
                              "breakdown without importing.");
        } else {
            ImGui::SetTooltip(
                "Inspect needs the cipher build flag. Rebuild with\n"
                "-DST_ENABLE_COBB_AP_CIPHER=ON.");
        }
    }

    // 2. Diff against — two-step:
    //    First click on row A: "Set as diff source" → stash left_path.
    //    Subsequent click on row B: "Diff against <basename of A>" →
    //                                open ptm_diff modal with both.
    //    Same row clicked twice: cancel the picking state.
    if (state.library_diff_picking_right &&
        state.library_diff_left_path != e.path) {
        auto const &lp = state.library_diff_left_path;
        auto const slash = lp.find_last_of("/\\");
        std::string const left_basename =
            (slash == std::string::npos) ? lp : lp.substr(slash + 1);
        std::string const label = "Diff against \"" + left_basename + "\"";
        if (ImGui::MenuItem(label.c_str())) {
            std::snprintf(state.ptm_diff_a_path,
                          sizeof state.ptm_diff_a_path, "%s",
                          state.library_diff_left_path.c_str());
            std::snprintf(state.ptm_diff_b_path,
                          sizeof state.ptm_diff_b_path, "%s",
                          e.path.c_str());
            state.show_ptm_diff_modal = true;
            state.library_diff_picking_right = false;
            state.library_diff_left_path.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open the ptm_diff modal with the chosen\n"
                              "left + right paths pre-populated.");
        }
        if (ImGui::MenuItem("Cancel diff picking")) {
            state.library_diff_picking_right = false;
            state.library_diff_left_path.clear();
        }
    } else if (state.library_diff_picking_right &&
               state.library_diff_left_path == e.path) {
        // Same row — let the user cancel without picking a partner.
        if (ImGui::MenuItem("Cancel diff picking")) {
            state.library_diff_picking_right = false;
            state.library_diff_left_path.clear();
        }
    } else {
        if (ImGui::MenuItem("Diff against\xE2\x80\xA6")) {
            state.library_diff_left_path = e.path;
            state.library_diff_picking_right = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Set this tune as the diff source. Right-click another\n"
                "row to pick the second tune and open the diff modal.");
        }
    }

    // 3. Import as project — opens the ptm_import modal pre-filled.
    //    User picks the rest (base ROM, def pack, output dir).
    if (ImGui::MenuItem("Import as project\xE2\x80\xA6")) {
        std::snprintf(state.ptm_import_ptm_path,
                      sizeof state.ptm_import_ptm_path, "%s",
                      e.path.c_str());
        state.show_ptm_import_modal = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Open the ptm-import wizard with this .ptm pre-selected.\n"
            "You'll still pick the base ROM, def pack, and output dir.");
    }

    ImGui::Separator();

#ifdef ST_HAVE_AP_WORKFLOW
    // 4. Push to AP — disabled-with-tooltip for v1. Wiring needs the
    //    C2 worker-thread refactor (strategic decisions handoff) so a
    //    push initiated from Library doesn't fight the AP browser's
    //    open channel. Pre-stage the menu item so the user sees it's
    //    coming + understands why it's not live yet.
    //
    // Gated on the AP workflow flag — in the default-OFF distribution
    // build the menu entry is omitted entirely (no "coming soon"
    // surface for a feature whose dependency is itself omitted).
    ImGui::BeginDisabled();
    if (ImGui::MenuItem("Push to AP")) {
        // intentionally unreachable until C2 lands
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Push this tune to the connected AP's /maps/.\n"
            "Awaiting the worker-thread refactor (strategic Q C2)\n"
            "so library + browser don't fight for the channel.\n"
            "Today: open the AccessPort Browser panel and use its\n"
            "Push File\xE2\x80\xA6 button.");
    }
#endif

    // 5. Reveal in Explorer — OS-native file browser jump. Validated
    //    through shell_safe_path so a maliciously-crafted library
    //    entry can't break out via shell metachars when we shell out
    //    to explorer/open/xdg-open.
    if (ImGui::MenuItem("Reveal in file browser")) {
        if (auto status =
                st::validate_shell_safe_path("library entry path", e.path);
            !status.has_value()) {
            enqueue_toast(state, ToastKind::Warn,
                          std::string{"Refused reveal: "} +
                              status.error().to_string());
        } else {
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path{e.path}, ec)) {
                enqueue_toast(state, ToastKind::Warn,
                              std::string{"File no longer exists on disk: "} +
                                  e.path);
            } else {
                std::string cmd;
#if defined(_WIN32)
                cmd = "explorer /select,\"" + e.path + "\"";
#elif defined(__APPLE__)
                cmd = "open -R \"" + e.path + "\"";
#else
                cmd = "xdg-open \"" +
                      std::filesystem::path{e.path}.parent_path().string() +
                      "\"";
#endif
                (void)std::system(cmd.c_str());
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the file browser to this .ptm's folder\n"
                          "with the file highlighted.");
    }

    // 6. Copy MD5 — pure UX win. Useful for hand-pasting into a
    //    `subuwutuner-cli ets pull /backupcksum` + library_inventory
    //    --query comparison.
    if (!e.md5.empty()) {
        if (ImGui::MenuItem("Copy MD5")) {
            ImGui::SetClipboardText(e.md5.c_str());
            enqueue_toast(state, ToastKind::Success,
                          "MD5 copied to clipboard.");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Copy this tune's MD5 hex to the clipboard.\n"
                              "Matches the AP's /backupcksum format —\n"
                              "useful for shell-side cross-reference.");
        }
    }
}

// L3 — render one collapsing facet group. Each item is a clickable
// selectable that toggles the corresponding filter; clicking the
// currently-active value clears it (i.e. "back to Any"). Count badge
// next to each item shows how many tunes would match if this facet
// alone were active alongside the current name needle. The active row
// is highlighted via the standard Selectable selection visual.
void render_facet_group(char const *label,
                         std::vector<std::string> const &values,
                         std::string &current_filter,
                         std::function<std::size_t(std::string const &)> count_of) {
    if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    // "Any" row sits at the top so the user always has a single-click
    // path back to no filter.
    {
        bool const active = current_filter.empty();
        if (ImGui::Selectable("Any", active)) {
            current_filter.clear();
        }
    }
    for (auto const &v : values) {
        ImGui::PushID(v.c_str());
        bool const active = (current_filter == v);
        std::string row = v;
        row += "  (";
        row += std::to_string(count_of(v));
        row += ")";
        if (ImGui::Selectable(row.c_str(), active)) {
            // Toggle behaviour: clicking the active row clears it.
            if (active) {
                current_filter.clear();
            } else {
                current_filter = v;
            }
        }
        ImGui::PopID();
    }
}

void render_facet_sidebar(PanelState &p) {
    if (!p.show_facet_sidebar || !p.index.has_value()) {
        return;
    }
    auto const &idx = *p.index;
    std::string_view const name_needle{p.filter_buf.data()};
    if (ImGui::BeginChild("##library_facets", ImVec2(220.0f, 360.0f), true)) {
        ImGui::TextUnformatted("Facets");
        ImGui::SameLine();
        // Compact "clear all" affordance — saves a 3-click walk through
        // each open group when the user wants to reset.
        if (ImGui::SmallButton("Clear##library_facets_clear")) {
            p.vendor_filter.clear();
            p.stage_filter.clear();
            p.fuel_filter.clear();
        }
        ImGui::Separator();

        auto const vendors = distinct_vendors(idx);
        render_facet_group("Vendor", vendors, p.vendor_filter,
            [&](std::string const &v) {
                return count_for_facet(idx, name_needle, v,
                                        p.stage_filter, p.fuel_filter);
            });

        auto const stages = distinct_stages(idx);
        render_facet_group("Stage", stages, p.stage_filter,
            [&](std::string const &s) {
                return count_for_facet(idx, name_needle, p.vendor_filter,
                                        s, p.fuel_filter);
            });

        auto const fuels = distinct_fuels(idx);
        render_facet_group("Fuel", fuels, p.fuel_filter,
            [&](std::string const &f) {
                return count_for_facet(idx, name_needle, p.vendor_filter,
                                        p.stage_filter, f);
            });
    }
    ImGui::EndChild();
}

// L1 — Cards view. Flow-wraps tune cards in a multi-column grid; each
// card mirrors the table-row info in a denser, more scannable visual.
// L2 pinning lives here too — the flashed tune renders first with an
// accent-tinted border + ★ prefix. Right-click any card to get the
// same context menu the row view exposes.
void render_entries_cards(AppState &state, PanelState &p,
                           st::library::TuneIndex const &idx,
                           st::library::ApCrossRefInputs const &ap_inputs,
                           std::vector<std::size_t> const &render_order,
                           std::size_t flashed_idx) {
    constexpr float kCardW = 280.0f;
    constexpr float kCardH = 160.0f;
    constexpr float kGap = 8.0f;
    float const avail = ImGui::GetContentRegionAvail().x;
    int const cols = std::max(
        1, static_cast<int>((avail + kGap) / (kCardW + kGap)));
    int col = 0;
    std::size_t row_seq = 0;
    for (auto i : render_order) {
        auto const &e = idx.ptm_entries()[i];
        bool const is_flashed = (i == flashed_idx);
        if (col > 0) {
            ImGui::SameLine(0.0f, kGap);
        }
        ImGui::PushID(static_cast<int>(row_seq++));
        if (is_flashed) {
            ImGui::PushStyleColor(ImGuiCol_Border, chip_fg_accent());
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        }
        if (ImGui::BeginChild("##library_card", ImVec2(kCardW, kCardH), true)) {
            // Top row: ★ pin + vendor chip (or "(unknown)" disabled).
            if (is_flashed) {
                chip("\xE2\x98\x85 Flashed", chip_fg_ok(), chip_bg_ok());
                ImGui::SameLine();
            }
            if (!e.vendor.empty()) {
                chip(e.vendor.c_str(), chip_fg_muted(), chip_bg_muted());
            } else {
                ImGui::TextDisabled("(unknown vendor)");
            }
            // Name — basename (full path is in the tooltip).
            auto const slash = e.path.find_last_of("/\\");
            std::string const basename =
                (slash == std::string::npos) ? e.path : e.path.substr(slash + 1);
            ImGui::Spacing();
            ImGui::TextWrapped("%s", basename.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", e.path.c_str());
            }
            // Stage / fuel / variant — one compact line.
            std::string sv;
            if (!e.stage.empty()) {
                sv = e.stage;
            }
            if (!e.fuel_grade.empty()) {
                if (!sv.empty()) {
                    sv += " / ";
                }
                sv += e.fuel_grade + " oct";
            }
            if (!e.variant.empty()) {
                if (!sv.empty()) {
                    sv += " / ";
                }
                sv += e.variant;
            }
            if (!sv.empty()) {
                text_subtle("%s", sv.c_str());
            }
            // Atlas + AP badges. Inline draw_chip mirrors the table
            // path (kept local to avoid threading the lambda through
            // two call sites).
            if (p.atlas.has_value()) {
                auto const ann =
                    st::library::annotate_tune(e, *p.atlas, ap_inputs);
                bool first = true;
                auto const draw_chip = [&](char const *label,
                                            ImVec4 const &fg,
                                            ImVec4 const &bg) {
                    if (!first) {
                        ImGui::SameLine(0, 4);
                    }
                    chip(label, fg, bg);
                    first = false;
                };
                if (ann.on_ap && !is_flashed) {
                    draw_chip("On AP", chip_fg_caution(), chip_bg_caution());
                }
                if (ann.fehr_family) {
                    draw_chip("Fehr", chip_fg_muted(), chip_bg_muted());
                }
                if (ann.cobb_nexgen) {
                    draw_chip("COBB/NexGen", chip_fg_muted(), chip_bg_muted());
                }
                if (ann.ntm_swap_basemap) {
                    draw_chip("FA24 swap", chip_fg_muted(), chip_bg_muted());
                }
            }
            // MD5 prefix + size at the bottom — bookkeeping info but
            // useful for matching against /backupcksum at-a-glance.
            char meta[80];
            std::snprintf(meta, sizeof meta, "md5:%.8s\xE2\x80\xA6  %llu KB",
                          e.md5.c_str(),
                          e.size / 1024ULL);
            text_subtle("%s", meta);
        }
        ImGui::EndChild();
        // Card-level context menu — right-click anywhere inside the
        // bordered child opens the same menu the table row provides.
        if (ImGui::BeginPopupContextItem("##library_card_ctx")) {
            render_row_context_menu(state, e);
            ImGui::EndPopup();
        }
        if (is_flashed) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        col = (col + 1) % cols;
        if (col == 0) {
            ImGui::Spacing();
        }
    }
}

// One row per filtered tune. v1 is a flat table; v2 promotes to
// card-style rendering with atlas badges.
void render_entries(AppState &state, PanelState &p) {
    if (!p.index.has_value()) {
        return;
    }
    auto const &idx = *p.index;
    auto const visible = apply_filters(idx,
                                        std::string_view{p.filter_buf.data()},
                                        p.vendor_filter, p.stage_filter,
                                        p.fuel_filter);
    if (visible.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        text_subtle("No tunes match the current filter.");
        return;
    }
    // Pull AP snapshot once per frame for AP cross-reference badges.
    // Empty / no-AP degrades cleanly: the badges just stay off.
    st::library::ApCrossRefInputs ap_inputs;
    if (auto const snap = ets_status_snapshot(); snap.has_value()) {
        ap_inputs.backupcksum = snap->device_backupcksum;
        ap_inputs.maps_filenames = snap->ap_maps_filenames;
    }

    // L2: pin currently-flashed to top. Find which (if any) visible
    // row matches the AP's /backupcksum and pull its index to the
    // front of the render order. Distinctive treatment (★ + accent
    // background) is applied in the row body below.
    std::vector<std::size_t> render_order;
    render_order.reserve(visible.size());
    std::size_t flashed_idx = static_cast<std::size_t>(-1);
    if (ap_inputs.backupcksum.has_value() &&
        !ap_inputs.backupcksum->empty()) {
        std::string ap_md5 = *ap_inputs.backupcksum;
        for (auto &c : ap_md5) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (auto i : visible) {
            auto const &e = idx.ptm_entries()[i];
            std::string entry_md5 = e.md5;
            for (auto &c : entry_md5) {
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            if (entry_md5 == ap_md5) {
                flashed_idx = i;
                break;
            }
        }
    }
    if (flashed_idx != static_cast<std::size_t>(-1)) {
        render_order.push_back(flashed_idx);
        for (auto i : visible) {
            if (i != flashed_idx) {
                render_order.push_back(i);
            }
        }
    } else {
        render_order = visible;
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (p.show_as_cards) {
        // L1 — cards view path. Reuses the same render_order + ap_inputs
        // so L2 pinning + AP cross-reference render identically.
        render_entries_cards(state, p, idx, ap_inputs, render_order,
                              flashed_idx);
        return;
    }
    if (ImGui::BeginTable("##library_table", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 360.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Stage / Variant", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Badges", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("MD5", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto i : render_order) {
            auto const &e = idx.ptm_entries()[i];
            bool const is_flashed = (i == flashed_idx);
            ImGui::TableNextRow();
            if (is_flashed) {
                // Subtle accent background for the pinned row — pulls
                // the eye without screaming.
                ImVec4 const bg = chip_bg_ok();
                ImU32 const bg_u = ImGui::GetColorU32(
                    ImVec4{bg.x, bg.y, bg.z, 0.18f});
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg_u);
            }
            ImGui::PushID(static_cast<int>(i));
            // Name column — basename of the path. Star prefix on the
            // pinned currently-flashed row.
            ImGui::TableSetColumnIndex(0);
            if (is_flashed) {
                ImGui::TextColored(chip_fg_ok(), "\xE2\x98\x85 "); // ★
                ImGui::SameLine(0, 0);
            }
            auto const slash = e.path.find_last_of("/\\");
            std::string_view const basename =
                (slash == std::string::npos) ? std::string_view{e.path}
                                              : std::string_view{e.path}.substr(slash + 1);
            ImGui::TextUnformatted(basename.data(),
                                    basename.data() + basename.size());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", e.path.c_str());
            }
            // Right-click context menu. ImGui binds the popup to the
            // most-recently-drawn item (the name text). PushID up at
            // the top of the row keeps per-row IDs distinct so each
            // row's popup is independent. PopupContextItem fires on
            // right-click anywhere over the name cell — most-natural
            // target for the user's right-click impulse.
            if (ImGui::BeginPopupContextItem("##library_row_ctx")) {
                render_row_context_menu(state, e);
                ImGui::EndPopup();
            }
            // Vendor.
            ImGui::TableSetColumnIndex(1);
            if (e.vendor.empty()) {
                ImGui::TextDisabled("(unknown)");
            } else {
                ImGui::TextUnformatted(e.vendor.c_str());
            }
            // Stage / variant — combined readable label.
            ImGui::TableSetColumnIndex(2);
            std::string sv;
            if (!e.stage.empty()) {
                sv = e.stage;
            }
            if (!e.fuel_grade.empty()) {
                if (!sv.empty()) {
                    sv += " / ";
                }
                sv += e.fuel_grade + " oct";
            }
            if (!e.variant.empty()) {
                if (!sv.empty()) {
                    sv += " / ";
                }
                sv += e.variant;
            }
            if (sv.empty()) {
                ImGui::TextDisabled("\xE2\x80\x94");
            } else {
                ImGui::TextUnformatted(sv.c_str());
            }
            // Badges — atlas-derived tuner-family + AP cross-reference.
            // Atlas may be nullopt (no fixture / file missing); in that
            // case tuner_family stays empty and we render only the AP
            // facets. Order matters: "Flashed" first (most actionable),
            // then "On AP", then tuner-family chips.
            ImGui::TableSetColumnIndex(3);
            if (p.atlas.has_value()) {
                auto const ann =
                    st::library::annotate_tune(e, *p.atlas, ap_inputs);
                bool first = true;
                auto const draw_chip = [&](char const *label,
                                            ImVec4 const &fg,
                                            ImVec4 const &bg) {
                    if (!first) {
                        ImGui::SameLine(0, 4);
                    }
                    chip(label, fg, bg);
                    first = false;
                };
                if (ann.currently_flashed) {
                    draw_chip("Flashed", chip_fg_ok(), chip_bg_ok());
                }
                if (ann.on_ap) {
                    draw_chip("On AP", chip_fg_caution(), chip_bg_caution());
                }
                if (ann.fehr_family) {
                    draw_chip("Fehr", chip_fg_muted(), chip_bg_muted());
                }
                if (ann.cobb_nexgen) {
                    draw_chip("COBB/NexGen", chip_fg_muted(), chip_bg_muted());
                }
                if (ann.ntm_swap_basemap) {
                    draw_chip("FA24 swap", chip_fg_muted(), chip_bg_muted());
                }
                if (first) {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
            } else {
                ImGui::TextDisabled("(no atlas)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("tuner_atlas.toml not found.\n"
                                      "Set ST_TUNER_ATLAS or run from a build that\n"
                                      "carries fixtures/tuner_atlas/.");
                }
            }
            // Size (KB).
            ImGui::TableSetColumnIndex(4);
            char size_buf[24];
            if (e.size < 1024) {
                std::snprintf(size_buf, sizeof size_buf, "%llu B",
                              static_cast<unsigned long long>(e.size));
            } else if (e.size < 1024ULL * 1024ULL) {
                std::snprintf(size_buf, sizeof size_buf, "%.1f KB",
                              static_cast<double>(e.size) / 1024.0);
            } else {
                std::snprintf(size_buf, sizeof size_buf, "%.1f MB",
                              static_cast<double>(e.size) / (1024.0 * 1024.0));
            }
            ImGui::TextUnformatted(size_buf);
            // MD5.
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(e.md5.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("MD5 of the .ptm file bytes — matches the\n"
                                  "AP's /backupcksum format. Click 'Copy MD5'\n"
                                  "(future) or copy from this tooltip to grep\n"
                                  "the index manually.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Phase 2: per-row actions (Inspect / Diff / Pull from AP / Import as project), "
                "atlas annotations, and /backupcksum cross-reference land in follow-up commits.");
}

} // namespace

void render_library_panel(AppState &state) {
    if (!state.show_library_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Tune Library", &state.show_library_panel)) {
        ImGui::End();
        return;
    }
    track_help_context(state, AppState::HelpContext::Library);
    auto &p = panel();
    // First-visit: try to load the default index. Surfaces an empty
    // state on failure rather than blocking until user clicks Scan.
    if (!p.load_attempted) {
        load_index(p);
    }
    // Lazy-load the atlas the first time the panel renders. Failure
    // is silent — annotate_tune is robust to no atlas; the panel just
    // shows "(no atlas)" in the Badges column.
    load_atlas_lazy(p);
    render_header(p, state);
    ImGui::Separator();
    if (!p.index.has_value()) {
        render_empty_state_text(p);
    } else if (p.index->ptm_entries().empty()) {
        render_empty_state(
            "Empty library",
            "library_index.toml loaded but no .ptm entries. Add .ptm files to "
            "the scanned directories and re-run "
            "tools/library_inventory/inventory.py.");
    } else if (p.show_facet_sidebar) {
        // L3 — split layout: facet sidebar on the left, entries pane
        // on the right. BeginChild gives the sidebar a bordered box;
        // BeginGroup lets the existing render_entries' own ScrollY-
        // enabled BeginTable layout work without nesting issues.
        render_facet_sidebar(p);
        ImGui::SameLine();
        ImGui::BeginGroup();
        render_entries(state, p);
        ImGui::EndGroup();
    } else {
        render_entries(state, p);
    }
    // Persistent indicator of the diff-picking state — surfaces even
    // when the user has moved the mouse away from the source row.
    // Without this, the two-step diff flow's left-side selection is
    // invisible after the popup closes.
    if (state.library_diff_picking_right &&
        !state.library_diff_left_path.empty()) {
        auto const &lp = state.library_diff_left_path;
        auto const slash = lp.find_last_of("/\\");
        std::string const basename =
            (slash == std::string::npos) ? lp : lp.substr(slash + 1);
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        chip(("Diff source: " + basename).c_str(), chip_fg_accent(),
             chip_bg_accent());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##library_diff_cancel")) {
            state.library_diff_picking_right = false;
            state.library_diff_left_path.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cancel the in-progress diff selection.");
        }
    }
    ImGui::End();
}

std::optional<LibraryStatusSnapshot> library_status_snapshot() {
    auto const &p = panel();
    if (!p.index.has_value()) {
        return std::nullopt;
    }
    LibraryStatusSnapshot out;
    out.ptm_count = p.index->ptm_entries().size();
    // stune_count from the already-loaded TuneIndex — the loader's
    // `stune_entries()` accessor is the canonical source. (Earlier
    // draft of this comment said the loader didn't expose it; it
    // does.) Inventory.py's `[[stune]]` rows feed straight through.
    out.stune_count = p.index->stune_entries().size();
    // currently_flashed_md5: cross-ref against ets_status_snapshot's
    // device_backupcksum. Empty when no AP or no match. Atlas-side
    // annotate_tune does the full per-row badge; this is the cheap
    // single-row lookup for the status-bar surface.
    if (auto const ap_snap = ets_status_snapshot();
        ap_snap.has_value() && ap_snap->device_backupcksum.has_value()) {
        auto const *match =
            p.index->lookup_by_md5(*ap_snap->device_backupcksum);
        if (match != nullptr) {
            out.currently_flashed_md5 = match->md5;
            // Basename of the matched .ptm so the AP browser's "Currently
            // flashed" row can render without re-walking the path.
            auto const slash = match->path.find_last_of("/\\");
            out.currently_flashed_name =
                (slash == std::string::npos) ? match->path
                                              : match->path.substr(slash + 1);
            out.currently_flashed_vendor = match->vendor;
            out.currently_flashed_stage = match->stage;
            out.currently_flashed_variant = match->variant;
            out.currently_flashed_fuel = match->fuel_grade;
        }
    }
    return out;
}

} // namespace st::ui
