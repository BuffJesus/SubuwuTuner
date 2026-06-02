// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// VehicleProfile — top-level domain object holding the identity +
// last-known state of one ECU the user tunes. Anchor for multi-ROM
// project models (Issue #10), live tuning (docs/19), and the
// AuditLog (Issue #8 — entries reference a profile by id). Closes
// analyst Issue #7.
//
// Persistence: each VehicleProfile is a single `.stprofile` TOML
// file. A user's profile collection lives in their config dir under
// `profiles/` so multiple cars / customer ECUs can co-exist; the
// active profile is just a path the GUI / CLI references.
//
// v1 ships the library + CLI verbs (show / import / export / list);
// GUI integration (profile picker in Settings + Flash modal
// defaulting from the active profile) is the next slice.

#ifndef ST_PROFILE_HPP
#define ST_PROFILE_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace st::profile {

inline constexpr int kVehicleProfileSchemaVersion = 1;

// Per-ECU identity. A vehicle with multiple ECUs (e.g. engine + TCU)
// gets one EcuProfile per ECU. v1 ships single-engine-ECU profiles;
// the vector shape is forward-compat for the multi-ECU case.
struct EcuProfile {
    // Pack-defined identifier — "engine", "tcm", "bcm", etc. Free
    // string in v1; future versions may constrain to a known set.
    std::string role;
    // ECU calibration ID as reported by SSM 0xA8 or UDS ReadDataByIdentifier
    // 0xF188. Stable per cal flash; changes when the user reflashes.
    std::string cal_id;
    // ECU hardware part number (e.g. "22765-AS80U"). Stable across
    // reflashes; identifies the silicon variant.
    std::string ecu_part;
    // Pack id that's been validated against this ECU (one of
    // Definition::pack().id). Empty when no pack has been picked yet.
    std::string pack_id;
    // Free-form notes.
    std::string notes;
};

// Last-known state of a successful flash against this vehicle.
// Optional — fresh profiles have none.
struct LastFlash {
    std::string timestamp_iso;       // ISO-8601 UTC
    std::string source_rom_path;     // absolute or relative
    std::uint32_t source_rom_crc32{0};
    std::string target_rom_path;
    std::uint32_t target_rom_crc32{0};
    std::string backup_path;         // .stbackup written before the flash
    std::string operator_notes;      // free-form, what changed in this tune
};

struct VehicleProfile {
    int schema_version{kVehicleProfileSchemaVersion};
    std::string id;                  // short slug, used as filename
    std::string display_name;        // human-readable
    std::string vin;                 // optional — many users tune ECU-only
    std::string year;                // "2017"
    std::string make;                // "Subaru"
    std::string model;               // "WRX"
    std::string transmission;        // "manual" / "auto"
    std::string transport_hint;      // "obdx COM5" / "j2534 OBDX Pro VX" / etc.
    std::vector<EcuProfile> ecus;
    std::optional<LastFlash> last_flash;
    std::string created_iso;
    std::string updated_iso;
    std::string notes;
};

// Save a profile to disk as TOML. Caller picks the path (typically
// `<config>/profiles/<id>.stprofile`).
[[nodiscard]] Status save(VehicleProfile const &profile,
                          std::filesystem::path const &path);

// Load a profile from disk. Returns ParseError on malformed TOML or
// missing required fields.
[[nodiscard]] Result<VehicleProfile> load(std::filesystem::path const &path);

// Scan a directory for `.stprofile` files. Returns sorted by id.
// Missing/empty dir returns an empty vector (not an error).
[[nodiscard]] Result<std::vector<VehicleProfile>>
list(std::filesystem::path const &dir);

// Sensible default config-dir location for the active profile
// registry. On Windows: %APPDATA%/SubuwuTuner/profiles. On Linux:
// ~/.config/subuwutuner/profiles. Caller can override.
[[nodiscard]] std::filesystem::path default_profile_dir();

} // namespace st::profile

#endif // ST_PROFILE_HPP
