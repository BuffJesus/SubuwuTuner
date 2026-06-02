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

// Static "wizard is open" flag — survives across frames while the
// wizard's main window is rendering. Toggle on when state.show_first_run_wizard
// fires; toggle off on Skip / Finish / window-X / Escape.
static bool g_wizard_open = false;

void render_first_run_modal(AppState &state) {
    // ImGui::BeginPopupModal at this call site reliably opens the
    // popup stack (dim background draws, IsPopupOpen=1) but the
    // popup's window content never renders — Z-order pathology
    // specific to this site. Reproduced across multiple shape
    // variants (NoSavedSettings on/off, SetNextWindowViewport on/off,
    // ASCII-only ID, fixed size, forced opaque bg). A plain
    // ImGui::Begin works at the same site with the same content. So
    // skip popup-modal entirely: render a dim overlay window
    // underneath, then the wizard window on top. Mimics modal UX
    // (background dimmed, wizard focused) without the broken popup
    // path. Tracked further in handoff notes; this is the practical
    // fix that ships the wizard.
    if (state.show_first_run_wizard) {
        g_wizard_open = true;
        state.show_first_run_wizard = false;
    }
    if (!g_wizard_open) {
        return;
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();

    // Dim background overlay covering the main viewport. Sits in
    // front of the dockspace + every panel so it dims them AND
    // absorbs clicks (anything outside the wizard rect goes here and
    // does nothing). Critically, DO NOT add NoBringToFrontOnFocus —
    // that's the dockspace pattern, which pins the window at the
    // back of the Z stack. We want the opposite.
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.40f);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##wizard_dim_overlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoNavFocus |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
        // Empty body — the window's purpose is the dim background fill.
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Wizard window itself — centered, focused, opaque.
    ImVec2 const center = vp->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowViewport(vp->ID);
    bool open = true;
    if (!ImGui::Begin("\xEE\x9D\xA8  Welcome to SubuwuTuner##first_run", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!open) {
        // Window-X = Skip wizard.
        state.settings.first_run_complete = true;
        save_settings(state.settings);
        g_wizard_open = false;
        ImGui::End();
        enqueue_toast(state, ToastKind::Info,
                      "Wizard skipped — defaults kept. Reopen via Help → Welcome wizard.");
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
    if (ImGui::Button("Skip wizard")) {
        state.settings.first_run_complete = true;
        save_settings(state.settings);
        g_wizard_open = false;
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
            g_wizard_open = false;
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

    ImGui::End();
}

} // namespace st::ui
