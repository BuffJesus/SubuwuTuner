// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Bottom status bar — project name, dirty/saved chip, jurisdiction
// profile chip (click to swap), DTCs-off chip, edit counter, transient
// status message in the middle, "N bytes changed" delta right-aligned.
// Rendered over the dockspace via a fixed-position window pinned to
// the viewport.

#include "panels/panels.hpp"

#include "actions.hpp" // set_active_view_rom
#include "app_state.hpp"
#include "persistence.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/policy.hpp"
#include "st/profile.hpp"

#include <imgui.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace st::ui {

void render_status_bar(AppState &state) {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    if (state.project.has_value()) {
        bool const dirty = state.dirty;

        // Left cluster: project name → status chip → history position.
        ImGui::TextUnformatted(state.project->display_name().c_str());
        // Hover the name to see the on-disk path and which definition
        // pack the project is bound to. Two projects with the same id
        // (different copies, different forks) read the same in the
        // name line; the path is the unambiguous handle, and the pack
        // disambiguates ECU variant.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nPack: %s", state.project->dir().string().c_str(),
                              state.project->definition().pack().id.c_str());
        }

        ImGui::SameLine();
        // MDL2 prefix icons:
        //   E70F Edit (pencil)   — unsaved edits
        //   E930 Completed       — saved / clean (filled circle + check)
        if (dirty) {
            chip("\xEE\x9C\x8F  Unsaved edits", chip_fg_warn(), chip_bg_warn());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("In-memory edits have not been written to disk.\n"
                                  "Ctrl+S to save the .stune project.");
            }
        } else {
            // After a successful save this session, swap the generic
            // "Clean" reading for "Saved <relative time>" — at-a-glance
            // freshness. Before the first save (project just opened),
            // fall back to "Clean" since the project on disk is still
            // saved, we just don't have a session-local timestamp.
            if (state.last_save_iso.has_value()) {
                auto const rel = format_relative_time(*state.last_save_iso);
                char buf[64];
                if (rel.empty()) {
                    std::snprintf(buf, sizeof buf, "\xEE\xA4\xB0  Saved");
                } else {
                    std::snprintf(buf, sizeof buf, "\xEE\xA4\xB0  Saved %s", rel.c_str());
                }
                chip(buf, chip_fg_muted(), chip_bg_muted());
                if (ImGui::IsItemHovered()) {
                    // Human-friendly absolute timestamp — strip the
                    // ISO 'T' separator and trailing 'Z', append "UTC".
                    // "2026-05-29T14:23:51Z" → "2026-05-29 14:23:51 UTC".
                    std::string when = *state.last_save_iso;
                    if (auto const t = when.find('T'); t != std::string::npos) {
                        when[t] = ' ';
                    }
                    if (!when.empty() && when.back() == 'Z') {
                        when.pop_back();
                    }
                    ImGui::SetTooltip("Last save: %s UTC.\nAll edits are on disk.",
                                      when.c_str());
                }
            } else {
                chip("\xEE\xA4\xB0  Clean", chip_fg_muted(), chip_bg_muted());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("All edits are saved to disk.");
                }
            }
        }

        // Jurisdiction profile chip — disabled-muted in motorsport-only
        // (the default, silent gate), accent for any other profile so the
        // user notices when they're under a real regulatory posture. Click
        // to open a chooser. See docs/06-legal-ethics.md.
        ImGui::SameLine();
        {
            auto const profile = state.project->policy_profile();
            auto const profile_str = std::string{st::policy::profile_name(profile)};
            bool const is_default = profile == st::policy::Profile::MotorsportOnly;
            // Leading E72E Lock — jurisdiction profile gates flash-time
            // policy. Trailing ▾ keeps the menu-trigger affordance.
            auto const chip_label = "\xEE\x9C\xAE  " + profile_str + "  \xE2\x96\xBE";
            if (is_default) {
                chip(chip_label.c_str(), chip_fg_muted(), chip_bg_muted());
            } else {
                chip(chip_label.c_str(), chip_fg_accent(), chip_bg_accent());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Active jurisdiction profile (project.toml).\n"
                                  "Drives the EmissionsLinter at flash time — emissions-\n"
                                  "flagged edits may require Confirm / Confirm+Reason\n"
                                  "under stricter profiles. Engine-safety violations\n"
                                  "always block, every profile.\n"
                                  "Click to change.");
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                ImGui::OpenPopup("##profile_chooser");
            }
            if (ImGui::BeginPopup("##profile_chooser")) {
                text_subtle("Jurisdiction profile (this project)");
                ImGui::Separator();
                auto pick = [&](st::policy::Profile p, char const *label, char const *desc) {
                    bool const is_current = (profile == p);
                    bool const is_saved_default = (state.settings.default_policy_profile == p);
                    char row[64];
                    if (is_saved_default) {
                        std::snprintf(row, sizeof row, "%s  (default)", label);
                    } else {
                        std::snprintf(row, sizeof row, "%s", label);
                    }
                    if (ImGui::Selectable(row, is_current)) {
                        state.project->set_policy_profile(p);
                        if (auto s = state.project->save_metadata(); !s.has_value()) {
                            state.status_msg = "Profile save failed: " + s.error().to_string();
                        } else {
                            state.status_msg = std::string{"Profile: "} + label;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", desc);
                    }
                };
                pick(st::policy::Profile::MotorsportOnly, "motorsport-only",
                     "Silent on save and on flash. No emissions warnings.\n"
                     "Engine-safety violations still block.");
                pick(st::policy::Profile::AlbertaCa, "alberta-ca",
                     "Yellow badge on emissions-flagged edits.\n"
                     "No flash-time prompt. Engine-safety still blocks.");
                pick(st::policy::Profile::EuRoadworthy, "eu-roadworthy",
                     "Warning on save, confirmation on flash for emissions\n"
                     "edits. Engine-safety still blocks.");
                pick(st::policy::Profile::CaliforniaUs, "california-us",
                     "Confirm + free-text reason on save AND on flash for\n"
                     "emissions edits. Engine-safety still blocks.");
                ImGui::Separator();
                if (ImGui::MenuItem("Save current as default for new projects")) {
                    state.settings.default_policy_profile = profile;
                    save_settings(state.settings);
                    state.status_msg = std::string{"Default profile: "} +
                                       std::string{st::policy::profile_name(profile)};
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Persist the project's current profile in\n"
                                      "settings.txt as the default for future projects.\n"
                                      "The CLI's `project-new` still defaults to\n"
                                      "motorsport-only — this setting is GUI-only for now.");
                }
                ImGui::EndPopup();
            }
        }

        // VehicleProfile chip — surfaces the active profile so the
        // user doesn't have to dive into Settings to remember which
        // one is current. Click → popup combo lists every profile
        // under default_profile_dir() and switches active_vehicle_
        // profile_id on selection (persisted via save_settings).
        // No profile set → "Profile: none" with a Settings hint.
        ImGui::SameLine();
        {
            std::string const &active_id = state.settings.active_vehicle_profile_id;
            std::string chip_label;
            chip_label.reserve(40);
            // E77B Contact — small person glyph reads as "vehicle/owner"
            // in this vocabulary. Trailing ▾ keeps the menu affordance.
            chip_label.append("\xEE\x9D\xBB  Profile: ");
            chip_label.append(active_id.empty() ? "none" : active_id);
            chip_label.append("  \xE2\x96\xBE");
            if (active_id.empty()) {
                chip(chip_label.c_str(), chip_fg_muted(), chip_bg_muted());
            } else {
                chip(chip_label.c_str(), chip_fg_accent(), chip_bg_accent());
            }
            if (ImGui::IsItemHovered()) {
                if (active_id.empty()) {
                    ImGui::SetTooltip("No active vehicle profile.\n"
                                      "Profiles capture vehicle identity (VIN,\n"
                                      "year/make/model, ECU cal id) for\n"
                                      "context-aware Flash + Read ROM dialogs.\n"
                                      "Click to pick one, or create via the\n"
                                      "CLI `profile import`.");
                } else {
                    ImGui::SetTooltip("Active vehicle profile: %s\n"
                                      "Stored at default_profile_dir().\n"
                                      "Click to switch.",
                                      active_id.c_str());
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                ImGui::OpenPopup("##vehicle_profile_chooser");
            }
            if (ImGui::BeginPopup("##vehicle_profile_chooser")) {
                text_subtle("Vehicle profile");
                ImGui::Separator();
                // List profiles on each popup open. The directory is
                // tiny in practice (a user typically has 1-5 cars), so
                // re-reading on each popup is cheaper than a cache +
                // invalidation dance. Errors fall through to a quiet
                // empty list so the popup still renders.
                auto const profile_dir = st::profile::default_profile_dir();
                auto profiles_result = st::profile::list(profile_dir);
                if (profiles_result.has_value() && !profiles_result->empty()) {
                    // "(none)" — explicit unset, mirrors the policy
                    // profile chooser's same-shape interaction.
                    if (ImGui::Selectable("(none)", active_id.empty())) {
                        state.settings.active_vehicle_profile_id.clear();
                        save_settings(state.settings);
                        state.refresh_audit_identity();
                        state.status_msg = "Profile: none";
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    for (auto const &p : *profiles_result) {
                        bool const is_current = (p.id == active_id);
                        char row[128];
                        if (p.display_name.empty()) {
                            std::snprintf(row, sizeof row, "%s", p.id.c_str());
                        } else {
                            std::snprintf(row, sizeof row, "%s (%s)",
                                          p.display_name.c_str(), p.id.c_str());
                        }
                        if (ImGui::Selectable(row, is_current)) {
                            state.settings.active_vehicle_profile_id = p.id;
                            save_settings(state.settings);
                            state.refresh_audit_identity();
                            state.status_msg = std::string{"Profile: "} + p.id;
                            ImGui::CloseCurrentPopup();
                        }
                        if (ImGui::IsItemHovered()) {
                            std::string tip;
                            tip.reserve(128);
                            if (!p.year.empty() || !p.make.empty() || !p.model.empty()) {
                                if (!p.year.empty()) {
                                    tip.append(p.year);
                                    tip.append(" ");
                                }
                                if (!p.make.empty()) {
                                    tip.append(p.make);
                                    tip.append(" ");
                                }
                                if (!p.model.empty()) {
                                    tip.append(p.model);
                                }
                                tip.append("\n");
                            }
                            if (!p.vin.empty()) {
                                tip.append("VIN: ");
                                tip.append(p.vin);
                            }
                            if (!tip.empty()) {
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                        }
                    }
                } else {
                    text_subtle("No profiles found.");
                    text_subtle("Create one via the CLI:");
                    text_subtle("  subuwutuner-cli profile import <FILE>");
                    text_subtle("Dir: %s", profile_dir.string().c_str());
                }
                ImGui::EndPopup();
            }
        }

        // Active-ROM chip — only renders when the active slot is NOT
        // working (the default). Surfaces "you are looking at a non-
        // editable ROM" so the user doesn't wonder why the edit
        // toolbar greyed out. Accent color since it represents a
        // non-default posture. Click resets to working in one step.
        if (!state.viewing_working_rom()) {
            ImGui::SameLine();
            std::string const id = state.active_rom_id;
            // E946 Info icon — neutral "you should know this" cue,
            // matches the chip vocabulary already in use.
            std::string const chip_label = "\xEE\xA5\x86  ROM: " + id;
            chip(chip_label.c_str(), chip_fg_accent(), chip_bg_accent());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reading from the '%s' ROM (read-only).\n"
                                  "Edit affordances are disabled while this\n"
                                  "slot is active.\n"
                                  "Click to switch back to the working ROM.",
                                  id.c_str());
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                set_active_view_rom(state, "working");
            }
        }

        // DTCs-disabled chip — only present when the pack declares DTCs
        // AND at least one is currently disabled. Walks the pack each
        // frame (a few hundred byte reads at worst, dwarfed by the data
        // grid's per-cell scaling work). Click to focus the DTCs panel.
        {
            auto const &def = state.project->definition();
            if (!def.dtcs().empty()) {
                auto const &rom = state.project->working_rom();
                std::size_t disabled = 0;
                std::size_t emissions_off = 0;
                for (auto const &d : def.dtcs()) {
                    auto const *bm = def.find_dtc_bitmap(d.bitmap_id);
                    if (bm == nullptr)
                        continue;
                    auto const en = st::is_dtc_enabled(rom, *bm, d);
                    if (en.has_value() && !*en) {
                        ++disabled;
                        if (d.emissions_relevant)
                            ++emissions_off;
                    }
                }
                if (disabled > 0) {
                    char buf[64];
                    // E7BA Warning — DTCs disabled is a soft warning.
                    std::snprintf(buf, sizeof buf, "\xEE\x9E\xBA  %zu DTC off", disabled);
                    ImGui::SameLine();
                    chip(buf, chip_fg_muted(), chip_bg_muted());
                    if (ImGui::IsItemHovered()) {
                        if (emissions_off > 0) {
                            ImGui::SetTooltip("%zu DTC(s) disabled in this working ROM\n"
                                              "(%zu emissions-flagged).\n"
                                              "See the DTCs panel to toggle.",
                                              disabled, emissions_off);
                        } else {
                            ImGui::SetTooltip("%zu DTC(s) disabled in this working ROM.\n"
                                              "See the DTCs panel to toggle.",
                                              disabled);
                        }
                    }
                }
            }
        }

        ImGui::SameLine();
        // Edits counter reflects the active slot (Issue #10 phase 3).
        // Source: always 0/0 (no history). Additional ROMs and
        // working: their respective per-ROM history.
        text_subtle("edits %zu / %zu", state.project->active_history().cursor(),
                    state.project->active_history().size());

        // Middle cluster: transient status message. save_project sets
        // this to "Saved."; edit-op errors land here too. Previously
        // the with-project branch never showed it, so Ctrl+S
        // succeeded silently — now it gives the user feedback.
        if (!state.status_msg.empty()) {
            ImGui::SameLine(0.0f, 24.0f);
            text_subtle("\xE2\x80\x94 %s", state.status_msg.c_str());
        }

        // Right cluster: "N bytes changed" delta, right-aligned. The raw
        // CRC pair used to live here; users glanced at two 8-digit hex
        // values whose only useful read was "did anything change." The
        // delta-count answers that question directly. Full CRCs move to
        // the hover tooltip for the rare diff-debug case. When the
        // working ROM matches source byte-for-byte (clean project, or
        // user undid every edit) the line is hidden entirely — the
        // "Clean" / "Saved Xm ago" chip on the left already conveys it.
        auto const src_data = state.project->source_rom().data();
        auto const work_data = state.project->working_rom().data();
        std::size_t bytes_changed = 0;
        if (src_data.size() == work_data.size()) {
            for (std::size_t i = 0; i < src_data.size(); ++i) {
                if (src_data[i] != work_data[i]) {
                    ++bytes_changed;
                }
            }
        } else {
            // Size mismatch — shouldn't happen in normal flow (Project
            // guarantees both buffers are the source size), but if it
            // does we'd rather show "size mismatch" than silently lie
            // about the delta count.
            bytes_changed = SIZE_MAX;
        }
        if (bytes_changed > 0) {
            char delta_buf[64];
            if (bytes_changed == SIZE_MAX) {
                std::snprintf(delta_buf, sizeof delta_buf, "size mismatch");
            } else if (bytes_changed == 1) {
                std::snprintf(delta_buf, sizeof delta_buf, "1 byte changed");
            } else {
                std::snprintf(delta_buf, sizeof delta_buf, "%zu bytes changed", bytes_changed);
            }
            float const delta_w = ImGui::CalcTextSize(delta_buf).x;
            float const right_x =
                ImGui::GetWindowContentRegionMax().x - delta_w - ImGui::GetStyle().FramePadding.x;
            if (right_x > ImGui::GetCursorPosX()) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(right_x);
                text_subtle("%s", delta_buf);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s between source and working ROM.\n"
                        "source CRC32  0x%08X\nworking CRC32 0x%08X",
                        delta_buf, state.project->source_crc32_at_create(),
                        state.project->working_rom().crc32());
                }
            }
        }
    } else {
        text_subtle("No project loaded. %s",
                    state.status_msg.empty() ? "" : state.status_msg.c_str());
    }
    ImGui::End();
}

} // namespace st::ui
