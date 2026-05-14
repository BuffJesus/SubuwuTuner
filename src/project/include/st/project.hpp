// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_PROJECT_HPP
#define ST_PROJECT_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/policy.hpp"
#include "st/rom.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

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

class Project {
  public:
    static constexpr int kSchemaVersion = 1;

    [[nodiscard]] static Result<Project> create(std::filesystem::path const &project_dir,
                                                std::filesystem::path const &source_rom_path,
                                                std::filesystem::path const &definition_path,
                                                std::string                  display_name);

    [[nodiscard]] static Result<Project> open(std::filesystem::path const &project_dir);

    // Persist the in-memory working ROM bytes to working.bin and update
    // project.toml's `working_rom.crc32`. Does NOT touch source.bin or the
    // definition pack.
    [[nodiscard]] Status save_working_rom();

    // Rewrite project.toml from the current in-memory state. Useful after
    // changing display_name or notes.
    [[nodiscard]] Status save_metadata() const;

    // ---- Accessors -----------------------------------------------------

    [[nodiscard]] std::filesystem::path const &dir() const noexcept { return dir_; }
    [[nodiscard]] std::string const &          display_name() const noexcept {
        return display_name_;
    }
    [[nodiscard]] std::string const &notes() const noexcept { return notes_; }
    void                             set_notes(std::string n) noexcept { notes_ = std::move(n); }
    void                             set_display_name(std::string n) noexcept {
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

    [[nodiscard]] Rom const &       source_rom() const noexcept { return source_; }
    [[nodiscard]] Rom const &       working_rom() const noexcept { return working_; }
    [[nodiscard]] Rom &             working_rom() noexcept { return working_; }
    [[nodiscard]] Definition const &definition() const noexcept { return def_; }

    [[nodiscard]] edit::History const &history() const noexcept { return history_; }
    [[nodiscard]] edit::History &      history() noexcept { return history_; }

    [[nodiscard]] std::uint32_t source_crc32_at_create() const noexcept {
        return source_crc32_;
    }

  private:
    Project() = default;

    std::filesystem::path dir_;
    std::string           display_name_;
    std::string           notes_;
    std::string           created_;
    std::uint32_t         source_crc32_{0};
    policy::Profile       policy_profile_{policy::Profile::MotorsportOnly};

    std::filesystem::path source_rel_{"source.bin"};
    std::filesystem::path working_rel_{"working.bin"};
    std::filesystem::path def_rel_;

    Rom           source_{Rom::from_bytes({})};
    Rom           working_{Rom::from_bytes({})};
    Definition    def_;
    edit::History history_;
};

} // namespace st

#endif // ST_PROJECT_HPP
