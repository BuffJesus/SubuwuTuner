// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Forward-decl umbrella for every render_*_modal entry point. Per-modal
// companion symbols (PendingFlash, ShortcutRow/Group, build_pending_flash,
// run_maf_autotune_preview, modal_save_label, etc.) stay file-local to
// their modal .cpp once moved — they have no cross-file callers.

#ifndef ST_UI_MODALS_HPP
#define ST_UI_MODALS_HPP

#include "app_state.hpp"

namespace st::ui {

void render_unsaved_modal(AppState &state);
void render_csv_import_modal(AppState &state);
void render_maf_autotune_modal(AppState &state);
void render_kp_autotune_modal(AppState &state);
void render_shortcuts_modal(AppState &state);
void render_about_modal(AppState &state);
void render_new_project_modal(AppState &state);
void render_flash_modal(AppState &state);
void render_settings_modal(AppState &state);
void render_def_registry_modal(AppState &state);
void render_read_rom_modal(AppState &state);
void render_first_run_modal(AppState &state);
void render_help_modal(AppState &state);

} // namespace st::ui

#endif
