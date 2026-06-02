// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// First-run wizard — analyst Issue #13. Auto-opens on a fresh
// install (settings.first_run_complete == false). Five steps:
//
//   Welcome      → brand + brief intro, "Get started" button
//   Jurisdiction → pick policy profile (defaults MotorsportOnly)
//   Units        → metric / imperial (informational v1; reserved for
//                  per-axis display)
//   Theme        → Dark / Light
//   Demo         → opens fixtures/demo.stune in a fresh project
//                  window, or "Skip" to land on the empty welcome panel
//
// On Finish the modal writes settings (theme, profile,
// first_run_complete=true) so subsequent launches go straight to the
// main UI. Help → Welcome wizard re-opens the modal manually;
// `subuwutuner-gui --reset-config` clears the flag on disk so the
// next launch shows the wizard again.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "panels/panels.hpp" // enqueue_toast
#include "persistence.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/policy.hpp"

#include <imgui.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace st::ui {

namespace {

constexpr int kStepCount = 5;
constexpr char const *kStepNames[kStepCount] = {"Welcome", "Jurisdiction", "Units", "Theme",
                                                "Demo"};

void draw_step_indicator(int current_step) {
    // Five small dots, current one filled with accent color.
    ImVec4 const accent = accent_for(Theme::Dark).base; // accent is theme-stable
    ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_muted());
    for (int i = 0; i < kStepCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        if (i == current_step) {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted("\xE2\x97\x8F"); // ●
            ImGui::PopStyleColor();
        } else if (i < current_step) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
            ImGui::TextUnformatted("\xE2\x9C\x93"); // ✓
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted("\xE2\x97\x8B"); // ○
        }
        if (i + 1 < kStepCount) {
            ImGui::SameLine();
            ImGui::TextUnformatted("\xE2\x80\x94"); // —
        }
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextUnformatted(kStepNames[current_step]);
    ImGui::Separator();
}

void draw_welcome(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_centered("SubuwuTuner", 2.0f);
    text_centered_subtle("A free, open-source Subaru ECU tuning suite");
    ImGui::Dummy(ImVec2(0.0f, kSpaceL));
    ImGui::TextWrapped("This is your first launch. In the next few steps you'll pick a few "
                       "defaults so the editor matches how you tune:");
    ImGui::Bullet();
    ImGui::TextUnformatted("Jurisdiction policy — controls emissions-edit warnings.");
    ImGui::Bullet();
    ImGui::TextUnformatted("Unit system — metric or imperial for display.");
    ImGui::Bullet();
    ImGui::TextUnformatted("Theme — Dark or Light.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("Defaults are tuner-friendly. You can change everything later under "
                "Tools → Settings.");
    (void)state; // step advance handled by footer
}

void draw_jurisdiction(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("Choose a jurisdiction profile. This controls when the editor warns or "
                       "blocks edits to emissions-related tables. SubuwuTuner is "
                       "jurisdiction-neutral by design — pick what matches your situation.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    using P = st::policy::Profile;
    struct Choice {
        P profile;
        char const *name;
        char const *blurb;
    };
    constexpr std::array<Choice, 4> choices = {{
        {P::MotorsportOnly, "Motorsport-only",
         "No emissions equipment in scope. Track car, race-only build. Default."},
        {P::AlbertaCa, "Alberta, Canada",
         "No provincial emissions inspection; minimal federal enforcement against "
         "individual modifiers."},
        {P::EuRoadworthy, "EU road-worthy",
         "Stricter — warns on any emissions table edit. For street cars subject to "
         "EU annual inspection."},
        {P::CaliforniaUs, "California (USA)",
         "Strictest — blocks emissions edits by default. CARB jurisdiction."},
    }};
    for (auto const &c : choices) {
        bool const selected = state.settings.default_policy_profile == c.profile;
        if (ImGui::RadioButton(c.name, selected)) {
            state.settings.default_policy_profile = c.profile;
        }
        ImGui::SameLine();
        text_subtle(" — %s", c.blurb);
    }
}

void draw_units(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("Pick a unit system for display. Engineering values stored in the ROM "
                       "stay in their native scaling regardless — this only affects how the "
                       "editor labels them.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    using U = AppState::UnitSystem;
    if (ImGui::RadioButton("Metric (°C, kPa, km/h)",
                          state.first_run_units == U::Metric)) {
        state.first_run_units = U::Metric;
    }
    if (ImGui::RadioButton("Imperial (°F, psi, mph)",
                          state.first_run_units == U::Imperial)) {
        state.first_run_units = U::Imperial;
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("v1 ships with metric throughout; the imperial conversion layer lands in a "
                "follow-up. Pick what you'd want it to be when it does.");
}

void draw_theme(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("Light or dark? Toggleable later under View → Theme.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    if (ImGui::RadioButton("Dark (default — easier on the eyes in a tuning bay)",
                          state.settings.theme == Theme::Dark)) {
        state.settings.theme = Theme::Dark;
        apply_theme(state.settings.theme);
    }
    if (ImGui::RadioButton("Light", state.settings.theme == Theme::Light)) {
        state.settings.theme = Theme::Light;
        apply_theme(state.settings.theme);
    }
}

void draw_demo(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("All set. Want to open the bundled demo project? It loads a small "
                       "synthetic ROM + pack so you can poke around the table editor without "
                       "needing a real ECU dump.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Checkbox("Open the demo project on Finish", &state.first_run_offer_demo);
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("You can always open it later via File → Open Recent → demo.stune.");
}

} // namespace

void render_first_run_modal(AppState &state) {
    if (state.show_first_run_wizard) {
        ImGui::OpenPopup("\xEE\x9D\xA8  Welcome to SubuwuTuner##first_run");
        state.show_first_run_wizard = false;
    }
    // Pin the popup to the main viewport so multi-viewport mode
    // doesn't detach it into its own OS window (which can land hidden
    // behind the main window — the screenshot bug from 2026-06-01
    // night: input blocked, no visible modal, popup detached + hidden).
    // Set BEFORE position so SetNextWindowPos applies in main-viewport
    // coordinates, not OS-screen coordinates.
    ImGuiViewport const *main_vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(main_vp->ID);
    ImVec2 const center = main_vp->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(580.0f, 380.0f),
                                        ImVec2(580.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("\xEE\x9D\xA8  Welcome to SubuwuTuner##first_run", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    draw_step_indicator(state.first_run_step);

    switch (state.first_run_step) {
    case 0:
        draw_welcome(state);
        break;
    case 1:
        draw_jurisdiction(state);
        break;
    case 2:
        draw_units(state);
        break;
    case 3:
        draw_theme(state);
        break;
    case 4:
        draw_demo(state);
        break;
    default:
        ImGui::TextUnformatted("(invalid step — please restart the wizard)");
        break;
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceL));
    ImGui::Separator();

    // Footer: Back / Next / Finish + Skip.
    bool const at_first = state.first_run_step == 0;
    bool const at_last = state.first_run_step == kStepCount - 1;

    if (!at_first) {
        if (ImGui::Button("Back", ImVec2(120.0f, 0.0f))) {
            --state.first_run_step;
        }
        ImGui::SameLine();
    }
    // Skip wizard — useful for users who want to dive straight in.
    if (ImGui::Button("Skip wizard")) {
        state.settings.first_run_complete = true;
        save_settings(state.settings);
        ImGui::CloseCurrentPopup();
        enqueue_toast(state, ToastKind::Info,
                      "Wizard skipped — defaults kept. Reopen via Help → Welcome wizard.");
    }
    ImGui::SameLine();

    push_primary_button_colors();
    if (!at_last) {
        if (ImGui::Button("Next \xE2\x86\x92", ImVec2(120.0f, 0.0f))) {
            ++state.first_run_step;
        }
    } else {
        if (ImGui::Button("\xEE\x9C\xBE  Finish", ImVec2(140.0f, 0.0f))) {
            state.settings.first_run_complete = true;
            save_settings(state.settings);
            ImGui::CloseCurrentPopup();
            if (state.first_run_offer_demo) {
                if (auto const demo = resolve_demo_project_path(nullptr); demo.has_value()) {
                    state.try_open_project(*demo);
                } else {
                    enqueue_toast(state, ToastKind::Info,
                                  "Demo project not found alongside the binary. Use File → "
                                  "Open to point at your own .stune project.");
                }
            } else {
                enqueue_toast(state, ToastKind::Info,
                              "Welcome aboard! Use File → New project to create a tune.");
            }
        }
    }
    pop_primary_button_colors();

    ImGui::EndPopup();
}

} // namespace st::ui
