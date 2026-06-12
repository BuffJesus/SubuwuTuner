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
void render_ptm_import_modal(AppState &state);
void render_ptm_inspect_modal(AppState &state);
void render_ptm_export_modal(AppState &state);
void render_ptm_diff_modal(AppState &state);
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
void render_fa24_swap_modal(AppState &state);

// Public guard used by Welcome card + Tools menu entry to decide
// whether the FA24-swap workflow card should be enabled. Returns
// true when the loaded project's pack declares all required tables
// for the workflow. See modals/fa24_swap.cpp for the table list.
[[nodiscard]] bool pack_supports_fa24_swap(AppState const &state);

// Returns true when the project's active-ROM history has at least
// one edit tagged "fa24_swap" at the head of the applied range —
// i.e. the FA24-swap workflow ran and hasn't been undone past its
// first edit. Drives the status-bar badge and the welcome-card's
// "already applied" indicator.
[[nodiscard]] bool fa24_swap_active(AppState const &state);

// Atomically undo every contiguous "fa24_swap"-tagged edit at the
// head of the active history, restoring the project state to before
// the workflow ran. Used by the status-bar badge's Revert All. Noop
// when there's no project, no tagged edits, or the head edit is
// untagged (user added a manual edit on top — they need to undo
// that one first to expose the workflow batch).
void revert_fa24_swap(AppState &state);

// TGV + EGR delete workflow. Same shape as the FA24 trio above
// (render + pack support + active + revert). Per analyst's Task A-F
// delivery 2026-06-09; encodes the L1 + DTC bit-clear delete strategy
// jurisdiction-gated at modal entry.
void render_tgv_egr_delete_modal(AppState &state);
[[nodiscard]] bool pack_supports_tgv_egr_delete(AppState const &state);
[[nodiscard]] bool tgv_egr_delete_active(AppState const &state);
void revert_tgv_egr_delete(AppState &state);

} // namespace st::ui

#endif
