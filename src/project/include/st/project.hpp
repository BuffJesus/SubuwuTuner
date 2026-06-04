// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_PROJECT_HPP
#define ST_PROJECT_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/policy.hpp"
#include "st/rom.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace st {

// A SubuwuTuner project on disk: a directory containing project.toml plus a
// source ROM, a working ROM (the one with edits applied), and a reference to
// a definition pack (either a path inside the project or a path outside).
//
// Layout:
//
//     mytune.stune/
//     ├── project.toml         (metadata + paths)
//     ├── source.bin           (the original ROM, never modified)
//     ├── working.bin          (the editable copy)
//     └── definitions/         (optional: a copy of the def pack, otherwise
//                              project.toml references an external path)
//
// The Project type owns the loaded source ROM, working ROM, and Definition.
// Editing the working ROM in memory is done via st::edit + st::defs as
// usual; save_working_rom() persists the in-memory bytes back to disk.
//
// Additional ROMs (analyst Issue #10 — multi-ROM project model, read
// slice). A project can optionally carry extra ROM files under [[rom]]
// entries in project.toml; these are typically alternate calibrations
// the user wants to compare against source/working without standing
// up a second project. v1 ships READ-ONLY: the Compare panel can
// pick from them, but per-ROM edit::History isolation is still future
// work. Schema stays at v1 because the [[rom]] array is additive —
// projects without it round-trip unchanged.

class Project {
public:
    static constexpr int kSchemaVersion = 1;

    struct AdditionalRom {
        std::string id;                // slug used in TOML + UI ids
        std::string display_name;
        std::filesystem::path path_rel;
        std::string notes;
        std::uint32_t crc32{0};
        Rom rom{Rom::from_bytes({})};
        // Per-ROM edit history (Issue #10 phase 3). Persisted at
        // <project>/histories/<id>.toml, loaded on Project::open.
        // Pre-2026-06-04 projects stored it at <project>/<id>.edits.toml
        // at the root; the loader still falls back to that path so
        // legacy projects open without manual migration. Empty for a
        // freshly-registered ROM until the user edits it through the
        // GUI's active-ROM flow.
        edit::History history;
    };

    [[nodiscard]] static Result<Project> create(std::filesystem::path const &project_dir,
                                                std::filesystem::path const &source_rom_path,
                                                std::filesystem::path const &definition_path,
                                                std::string display_name);

    [[nodiscard]] static Result<Project> open(std::filesystem::path const &project_dir);

    // Persist the in-memory working ROM bytes to working.bin and update
    // project.toml's `working_rom.crc32`. Does NOT touch source.bin or the
    // definition pack.
    [[nodiscard]] Status save_working_rom();

    // Rewrite project.toml from the current in-memory state. Useful after
    // changing display_name or notes.
    [[nodiscard]] Status save_metadata() const;

    // ---- Accessors -----------------------------------------------------

    [[nodiscard]] std::filesystem::path const &dir() const noexcept {
        return dir_;
    }
    [[nodiscard]] std::string const &display_name() const noexcept {
        return display_name_;
    }
    [[nodiscard]] std::string const &notes() const noexcept {
        return notes_;
    }
    void set_notes(std::string n) noexcept {
        notes_ = std::move(n);
    }
    void set_display_name(std::string n) noexcept {
        display_name_ = std::move(n);
    }

    // Active jurisdiction profile (see docs/06-legal-ethics.md). Defaults
    // to `MotorsportOnly` on a freshly-created project, matching the doc's
    // first-run-default. Persisted in `project.toml` as `policy_profile =
    // "..."`. Drives flash-time linting and (eventually) the GUI badge.
    [[nodiscard]] policy::Profile policy_profile() const noexcept {
        return policy_profile_;
    }
    void set_policy_profile(policy::Profile p) noexcept {
        policy_profile_ = p;
    }

    // Aftermarket-vendor programming-handheld serial number for the ECU
    // this project targets, if any. Persisted in `[security_access]
    // handheld_serial = "..."`.
    //
    // Why this lives in the project rather than user-prefs: when an ECU's
    // SecurityAccess constants have been replaced by an aftermarket tuning
    // install, recovery of the replacement constants may need the handheld's
    // serial as an input (open question — see the per-handheld vs.
    // static-per-firmware×CALID hypothesis worked through in the off-tree
    // SA-alteration analysis). Keeping the field at project scope means a
    // single user with multiple cars / handhelds can carry per-car serials
    // without re-entering them.
    //
    // Empty string ⇒ stock ECU or unknown handheld; no derivation will use
    // it. Format is whatever the vendor prints on the device — we do not
    // validate, normalize, or transmit it.
    [[nodiscard]] std::string const &handheld_serial() const noexcept {
        return handheld_serial_;
    }
    void set_handheld_serial(std::string s) noexcept {
        handheld_serial_ = std::move(s);
    }

    [[nodiscard]] Rom const &source_rom() const noexcept {
        return source_;
    }
    [[nodiscard]] Rom const &working_rom() const noexcept {
        return working_;
    }
    [[nodiscard]] Rom &working_rom() noexcept {
        return working_;
    }
    [[nodiscard]] Definition const &definition() const noexcept {
        return def_;
    }

    [[nodiscard]] edit::History const &history() const noexcept {
        return history_;
    }
    [[nodiscard]] edit::History &history() noexcept {
        return history_;
    }

    [[nodiscard]] std::uint32_t source_crc32_at_create() const noexcept {
        return source_crc32_;
    }

    // Active-ROM identifier (Issue #10 — multi-ROM read-side foundation).
    // Empty string means "the working slot is active" (the default
    // behavior every project has had since v1). The reserved tokens
    // "working" and "source" address the built-in slots; any other
    // non-empty value must match an id in additional_roms().
    //
    // Persisted in project.toml as [project] active_rom_id = "...".
    // Round-trips through Project::open / save_metadata. Read-side CLI
    // verbs honor it as their default when their own --rom flag is
    // absent. Edit-side routing through this slot is future work.
    [[nodiscard]] std::string const &active_rom_id() const noexcept {
        return active_rom_id_;
    }

    // Set the active ROM id. Validates against the built-in slot names
    // and the current additional_roms() list. Empty string and the
    // tokens "working" and "source" always succeed; any other value
    // must match an additional id or InvalidArgument is returned.
    // Caller persists the change via save_metadata().
    [[nodiscard]] Status set_active_rom_id(std::string_view id);

    // Resolve an id to a ROM pointer for read-side consumers. Accepts
    // the same vocabulary as set_active_rom_id ("" → working_rom,
    // "working" → working_rom, "source" → source_rom, additional id →
    // its loaded Rom). Returns nullptr for unknown ids.
    [[nodiscard]] Rom const *find_rom_by_id(std::string_view id) const noexcept;

    // Edit history for the currently-active slot (Issue #10 phase 3).
    // "" / "working" / "source" → the project's working-rom history.
    // Source has no editable history of its own; this returns the
    // working history so display surfaces (history panel, status-bar
    // edit counter) stay populated. Edit-side call sites pair this
    // with active_rom_mut(), which DOES return nullptr for source,
    // so writes still gate off correctly.
    // An additional ROM's id → that additional's history.
    // Unknown id (stale active_rom_id) → working history fallback,
    // matching find_rom_by_id's behavior for the read side.
    [[nodiscard]] edit::History const &active_history() const noexcept;
    [[nodiscard]] edit::History &active_history() noexcept;

    // Writable ROM pointer for the currently-active editable slot.
    // "" / "working" → &working_ (edits target this slot in the v1
    // model and still do for the project's default slot).
    // Additional id → &(additional_roms_[i].rom). Source / unknown id
    // → nullptr. Callers MUST null-check before writing — a null
    // return is the contract that means "the active slot is not
    // editable right now" (currently only true for source).
    [[nodiscard]] Rom *active_rom_mut() noexcept;

    // Persist the currently-active ROM's bytes + its edit history.
    // Working: writes working.bin + edits.toml (matches the v1
    // behavior of save_working_rom — that method is still available
    // and now delegates here when working is active).
    // Additional id: writes the additional's bytes to its
    // path_rel + history to histories/<id>.toml (legacy
    // <id>.edits.toml at the root is removed after a successful
    // write). Also updates the additional's recorded crc32 in
    // project.toml via save_metadata.
    // Source: no-op (immutable). Returns InvalidArgument if the
    // active id is set to an unknown slug.
    [[nodiscard]] Status save_active_rom();

    // Persist every dirty slot (working + any additional with a non-
    // empty history). The GUI save flow calls this so the user
    // doesn't have to manually re-activate each slot before Ctrl+S.
    // Stops at the first failure and reports it; partially-saved
    // state is left on disk (matches the v1 save_working_rom error
    // path). Always emits save_metadata at the end to refresh the
    // recorded crc32 for each saved slot.
    [[nodiscard]] Status save_all();

    // Read-only access to extra ROMs declared in project.toml as
    // [[rom]] entries (Issue #10 read slice). Empty when project.toml
    // declares none. Returned vector is sized once on Project::open;
    // adding ROMs at runtime is a follow-up.
    [[nodiscard]] std::vector<AdditionalRom> const &additional_roms() const noexcept {
        return additional_roms_;
    }

    // Warnings collected from [[rom]] parsing when an entry was
    // dropped — missing file, missing id, etc. Surfaces user-visible
    // signal for "the project.toml declares this ROM but it didn't
    // load" without making the whole open() fail.
    [[nodiscard]] std::vector<std::string> const &additional_rom_warnings() const noexcept {
        return additional_rom_warnings_;
    }

    // Add an additional ROM to the in-memory project. Caller must
    // have already populated `entry.rom` (the loaded bytes) and
    // `entry.path_rel` (relative to dir()). Does NOT copy the file —
    // the caller is responsible for ensuring the path is reachable.
    // Save with save_metadata() to persist the [[rom]] entry.
    // Returns InvalidArgument when id is empty or already in use.
    [[nodiscard]] Status add_additional_rom(AdditionalRom entry);

private:
    Project() = default;

    std::filesystem::path dir_;
    std::string display_name_;
    std::string notes_;
    std::string created_;
    std::uint32_t source_crc32_{0};
    policy::Profile policy_profile_{policy::Profile::MotorsportOnly};
    std::string handheld_serial_;

    std::filesystem::path source_rel_{"source.bin"};
    std::filesystem::path working_rel_{"working.bin"};
    std::filesystem::path def_rel_;

    Rom source_{Rom::from_bytes({})};
    Rom working_{Rom::from_bytes({})};
    Definition def_;
    edit::History history_;
    std::vector<AdditionalRom> additional_roms_;
    std::vector<std::string> additional_rom_warnings_;
    std::string active_rom_id_; // "" = working slot (the v1 default)
};

// CSV bulk-edit parser. Shared between the CLI's `project-edit-csv` and
// the GUI's File > Import CSV path so identity-header validation,
// bounds checking, and comment/blank-line handling stay single-source.
//
// CSV format (matches what `project-export-csv` emits):
//
//   # pack_id = "..."   (optional — warning on mismatch)
//   # table   = "..."   (optional — HARD ERROR on mismatch)
//   row,col,value       (an optional non-numeric header row is tolerated)
//   0,0,12.5
//   ...
//
// `#` introduces a line comment; blank/comment-only lines are skipped.
// Identity headers live inside `#` comments so the file still
// round-trips through generic CSV tools.
struct EditCsvCell {
    std::size_t row;
    std::size_t col;
    double value;
};

struct EditCsvWarning {
    std::string message;
};

struct EditCsvParseResult {
    std::vector<EditCsvCell> cells;
    std::vector<EditCsvWarning> warnings;
};

struct EditCsvParseOptions {
    // If non-empty, the CSV's `# pack_id = "..."` header is compared
    // against this. Mismatch emits a warning (not an error) — pack
    // IDs can drift across versions while scaling/addresses still
    // align well enough to apply the edit.
    std::string_view expected_pack_id;

    // If non-empty, the CSV's `# table = "..."` header is compared
    // against this. Mismatch is a hard error — applying a CSV made
    // for a different table would corrupt the working ROM.
    std::string_view expected_table_id;

    // Bounds-check parsed (row,col) against the target grid. Zero
    // means "skip the check" (caller validates).
    std::size_t table_rows{0};
    std::size_t table_cols{0};
};

[[nodiscard]] Result<EditCsvParseResult> parse_edit_csv(std::string_view text,
                                                        EditCsvParseOptions const &opts);

} // namespace st

#endif // ST_PROJECT_HPP
