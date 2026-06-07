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
    // Match the jurisdiction-step philosophy: lead with what the user
    // is actually deciding (small, fast), not procedural "this is
    // your first launch" framing. The bullets describe the *choice*
    // not the *setting name* — "jurisdiction policy" reads as jargon
    // until you know what it controls; "when the editor warns about
    // emissions edits" answers the question without requiring you to
    // already know the answer.
    ImGui::TextWrapped("Three quick prefs and you're in. None of them lock you into "
                       "anything — everything is changeable under Tools → Settings.");
    ImGui::Bullet();
    ImGui::TextUnformatted("When the editor warns about emissions-related edits");
    ImGui::Bullet();
    ImGui::TextUnformatted("Whether displays show °C / kPa / km/h or °F / psi / mph");
    ImGui::Bullet();
    ImGui::TextUnformatted("Dark or light interface");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("All defaults are picked for a typical project build. Press Esc or click "
                "Skip wizard to accept them and jump straight to the app.");
    (void)state; // step advance handled by footer
}

void draw_jurisdiction(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // Lead with what the choice DOES, in plain language. Without
    // this, "Jurisdiction profile" is the first piece of unexplained
    // jargon a new user sees. The reassurance ("you can change it
    // any time") lowers the stakes so the user doesn't agonize over
    // a default they'll never revisit.
    ImGui::TextWrapped("This only controls when the editor warns you about "
                       "emissions-related edits. Engine-safety violations always "
                       "block, regardless of which profile you pick.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Not sure? The default ('Motorsport-only') is the right pick "
                "for a track car or any non-street build. You can change it "
                "any time under Tools \xE2\x86\x92 Settings.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    using P = st::policy::Profile;
    struct Choice {
        P profile;
        char const *name;
        char const *blurb;
    };
    constexpr std::array<Choice, 4> choices = {{
        {P::MotorsportOnly, "Motorsport-only",
         "Track car or race-only build. No emissions warnings. Default."},
        {P::AlbertaCa, "Alberta, Canada",
         "Light-touch street use. Yellow badge on emissions edits, no flash-time gate."},
        {P::EuRoadworthy, "EU road-worthy",
         "Street car subject to EU annual inspection. Confirm on flash for emissions edits."},
        {P::CaliforniaUs, "California (USA)",
         "California / CARB jurisdiction. Confirm + reason required for emissions edits."},
    }};
    bool first = true;
    for (auto const &c : choices) {
        bool const selected = state.settings.default_policy_profile == c.profile;
        if (first && state.first_run_focused_step != state.first_run_step) {
            ImGui::SetKeyboardFocusHere();
        }
        first = false;
        if (ImGui::RadioButton(c.name, selected)) {
            state.settings.default_policy_profile = c.profile;
        }
        ImGui::SameLine();
        text_subtle(" — %s", c.blurb);
    }
}

void draw_units(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // Lead with what this is for — the old copy buried the user's
    // actual question ("does it change what gets flashed?") under
    // implementation detail. The bytes-flashed-are-identical note
    // is reassurance, not a header.
    ImGui::TextWrapped("This controls how numbers display in the editor. Bytes flashed to "
                       "the ECU are identical either way — the ROM's native scaling never "
                       "changes.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Not sure? Metric matches the ROM's internal scaling and is what most "
                "existing tuning docs use. Imperial is fine if you think in psi.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    using U = AppState::UnitSystem;
    if (state.first_run_focused_step != state.first_run_step) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::RadioButton("Metric (°C, kPa, km/h)",
                          state.first_run_units == U::Metric)) {
        state.first_run_units = U::Metric;
    }
    // Imperial conversion layer is not yet implemented (UnitSystem::Imperial
    // exists in app_state.hpp but no conversion logic reads it as of
    // 2026-06-07). Disable the choice with a tooltip so the wizard
    // doesn't offer a no-op pick — onboarding sets the user's
    // expectation that whatever they click takes effect.
    ImGui::BeginDisabled();
    bool imperial_selected_dummy = (state.first_run_units == U::Imperial);
    ImGui::RadioButton("Imperial (°F, psi, mph) — coming in a follow-up release",
                       imperial_selected_dummy);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Imperial display layer ships in a follow-up release. Pick "
                          "Metric for now; the toggle will be in Tools → Settings when "
                          "the conversion layer lands.");
    }
}

void draw_theme(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // Two-sided lead lets the user self-identify ("which one am I?")
    // rather than forcing a binary they haven't been given context
    // for. Light got no description before while Dark did — fixed.
    ImGui::TextWrapped("Dark for indoor / nighttime work, light for sunny garages and "
                       "outdoor dyno days.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Not sure? Dark is the default and matches most tuning environments. "
                "View → Theme switches any time.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    if (state.first_run_focused_step != state.first_run_step) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::RadioButton("Dark (default — easier on the eyes in a tuning bay)",
                          state.settings.theme == Theme::Dark)) {
        state.settings.theme = Theme::Dark;
        apply_theme(state.settings.theme);
    }
    if (ImGui::RadioButton("Light (better for sunlight or printed screenshots)",
                          state.settings.theme == Theme::Light)) {
        state.settings.theme = Theme::Light;
        apply_theme(state.settings.theme);
    }
}

void draw_demo(AppState &state) {
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // "Sample project to poke around in" reads as more inviting than
    // "bundled demo project." Surface the actual use-case ("haven't
    // pulled a ROM yet") so the user knows when this is for them.
    ImGui::TextWrapped("All set. Want a sample project to poke around in? The demo loads "
                       "a synthetic ROM + pack — every table editable, no real ECU needed. "
                       "Useful if you haven't pulled a ROM yet.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Not sure? Yes is a good pick. The demo is a 30-second poke around the "
                "editor; close it any time without saving.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    if (state.first_run_focused_step != state.first_run_step) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Checkbox("Open the demo project on Finish", &state.first_run_offer_demo);
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("Always available later: File → Open Recent → demo.stune.");
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
    state.help_context = AppState::HelpContext::FirstRunWizard;

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
    // Skip-wizard helper — called from window-X, the Skip button, and
    // an Esc keypress. Always: mark first_run_complete, persist, close
    // the wizard, toast the user with how to reopen it. Centralized so
    // the three escape hatches stay in lockstep; the previous shape
    // duplicated the body across window-X and the Skip button, which
    // drifted (the Esc affordance the welcome step promises in copy
    // had no wiring at all).
    auto skip_wizard = [&]() {
        state.settings.first_run_complete = true;
        save_settings(state.settings);
        g_wizard_open = false;
        state.first_run_focused_step = -1;
        enqueue_toast(state, ToastKind::Info,
                      "Wizard skipped — defaults kept. Reopen via Help → Welcome wizard.");
    };
    if (!open) {
        // Window-X = Skip wizard.
        skip_wizard();
        ImGui::End();
        return;
    }
    // Esc = Skip wizard. The welcome step's copy promises this; the
    // wiring used to be missing. IsKeyPressed reads global keypresses
    // and the wizard owns focus while it's rendered (SetNextWindowFocus
    // every frame + dim overlay absorbing clicks), so it's safe to
    // trigger here without scoping to the wizard window specifically.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        skip_wizard();
        ImGui::End();
        return;
    }

    draw_step_indicator(state.first_run_step);

    // Snapshot step BEFORE the draw_* call so the per-step focus
    // helpers above can compare against state.first_run_focused_step
    // and refresh focus on the frame the user lands on a new step.
    int const step_at_render = state.first_run_step;
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
        skip_wizard();
    }
    ImGui::SameLine();

    push_primary_button_colors();
    if (!at_last) {
        // Step 0 (welcome) has no in-body interactive widget so the
        // Next button gets focus instead — Enter then advances the
        // wizard immediately, which is the gesture a returning user
        // expects on a screen they've read once.
        if (at_first && state.first_run_focused_step != state.first_run_step) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::Button("Next \xE2\x86\x92", ImVec2(120.0f, 0.0f))) {
            ++state.first_run_step;
        }
    } else {
        if (ImGui::Button("\xEE\x9C\xBE  Finish", ImVec2(140.0f, 0.0f))) {
            state.settings.first_run_complete = true;
            save_settings(state.settings);
            g_wizard_open = false;
            state.first_run_focused_step = -1;
            if (state.first_run_offer_demo) {
                // resolve_demo_project_path needs argv[0]; the wizard
                // has no access to it here. main() does the resolve at
                // startup and caches the result on AppState — use that
                // instead of re-resolving with nullptr (which always
                // returns nullopt).
                if (state.demo_project_path.has_value()) {
                    state.try_open_project(*state.demo_project_path);
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
    // Mark the step as focused once per landing — must run after the
    // step body + Next button so the inside-step SetKeyboardFocusHere
    // calls have already fired this frame against the current step
    // value. `step_at_render` pins the focus to whatever was drawn
    // even if Back/Next changed first_run_step during the footer.
    state.first_run_focused_step = step_at_render;

    ImGui::End();
}

} // namespace st::ui
