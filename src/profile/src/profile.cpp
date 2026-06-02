// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/profile.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace st::profile {

namespace {

template <typename T>
[[nodiscard]] std::optional<T> get(toml::table const &tbl, std::string_view key) {
    auto const node = tbl.get(key);
    if (node == nullptr)
        return std::nullopt;
    return node->value<T>();
}

void insert_string_if_set(toml::table &tbl, char const *key, std::string const &value) {
    if (!value.empty())
        tbl.insert(key, value);
}

} // namespace

Status save(VehicleProfile const &profile, std::filesystem::path const &path) {
    toml::table root;

    toml::table vp;
    vp.insert("schema_version", static_cast<std::int64_t>(profile.schema_version));
    vp.insert("id", profile.id);
    insert_string_if_set(vp, "display_name", profile.display_name);
    insert_string_if_set(vp, "vin", profile.vin);
    insert_string_if_set(vp, "year", profile.year);
    insert_string_if_set(vp, "make", profile.make);
    insert_string_if_set(vp, "model", profile.model);
    insert_string_if_set(vp, "transmission", profile.transmission);
    insert_string_if_set(vp, "transport_hint", profile.transport_hint);
    insert_string_if_set(vp, "created", profile.created_iso);
    insert_string_if_set(vp, "updated", profile.updated_iso);
    insert_string_if_set(vp, "notes", profile.notes);

    if (!profile.ecus.empty()) {
        toml::array ecu_arr;
        for (auto const &e : profile.ecus) {
            toml::table et;
            et.insert("role", e.role);
            insert_string_if_set(et, "cal_id", e.cal_id);
            insert_string_if_set(et, "ecu_part", e.ecu_part);
            insert_string_if_set(et, "pack_id", e.pack_id);
            insert_string_if_set(et, "notes", e.notes);
            ecu_arr.push_back(std::move(et));
        }
        vp.insert("ecus", std::move(ecu_arr));
    }

    if (profile.last_flash.has_value()) {
        auto const &lf = *profile.last_flash;
        toml::table lf_tbl;
        insert_string_if_set(lf_tbl, "timestamp", lf.timestamp_iso);
        insert_string_if_set(lf_tbl, "source_rom_path", lf.source_rom_path);
        if (lf.source_rom_crc32 != 0)
            lf_tbl.insert("source_rom_crc32",
                          static_cast<std::int64_t>(lf.source_rom_crc32));
        insert_string_if_set(lf_tbl, "target_rom_path", lf.target_rom_path);
        if (lf.target_rom_crc32 != 0)
            lf_tbl.insert("target_rom_crc32",
                          static_cast<std::int64_t>(lf.target_rom_crc32));
        insert_string_if_set(lf_tbl, "backup_path", lf.backup_path);
        insert_string_if_set(lf_tbl, "operator_notes", lf.operator_notes);
        vp.insert("last_flash", std::move(lf_tbl));
    }

    root.insert("vehicle_profile", std::move(vp));

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::string msg{"profile::save: cannot open "};
        msg.append(path.string());
        return failure(ErrorCode::IoFailure, std::move(msg));
    }
    out << root;
    return ok();
}

Result<VehicleProfile> load(std::filesystem::path const &path) {
    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (toml::parse_error const &e) {
        std::string msg{"profile::load: TOML parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    auto const vp = parsed["vehicle_profile"].as_table();
    if (vp == nullptr) {
        return failure(ErrorCode::ParseError,
                       "profile::load: missing [vehicle_profile] table");
    }

    VehicleProfile p;
    p.schema_version = static_cast<int>(
        get<std::int64_t>(*vp, "schema_version").value_or(kVehicleProfileSchemaVersion));
    if (p.schema_version > kVehicleProfileSchemaVersion) {
        std::string msg{"profile::load: schema_version "};
        msg.append(std::to_string(p.schema_version));
        msg.append(" newer than supported ");
        msg.append(std::to_string(kVehicleProfileSchemaVersion));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    p.id = get<std::string>(*vp, "id").value_or(std::string{});
    if (p.id.empty()) {
        return failure(ErrorCode::ParseError,
                       "profile::load: [vehicle_profile] missing required `id`");
    }
    p.display_name = get<std::string>(*vp, "display_name").value_or(std::string{});
    p.vin = get<std::string>(*vp, "vin").value_or(std::string{});
    p.year = get<std::string>(*vp, "year").value_or(std::string{});
    p.make = get<std::string>(*vp, "make").value_or(std::string{});
    p.model = get<std::string>(*vp, "model").value_or(std::string{});
    p.transmission = get<std::string>(*vp, "transmission").value_or(std::string{});
    p.transport_hint = get<std::string>(*vp, "transport_hint").value_or(std::string{});
    p.created_iso = get<std::string>(*vp, "created").value_or(std::string{});
    p.updated_iso = get<std::string>(*vp, "updated").value_or(std::string{});
    p.notes = get<std::string>(*vp, "notes").value_or(std::string{});

    if (auto const ecu_arr = (*vp)["ecus"].as_array(); ecu_arr != nullptr) {
        for (auto const &node : *ecu_arr) {
            auto const et = node.as_table();
            if (et == nullptr)
                continue;
            EcuProfile e;
            e.role = get<std::string>(*et, "role").value_or(std::string{});
            e.cal_id = get<std::string>(*et, "cal_id").value_or(std::string{});
            e.ecu_part = get<std::string>(*et, "ecu_part").value_or(std::string{});
            e.pack_id = get<std::string>(*et, "pack_id").value_or(std::string{});
            e.notes = get<std::string>(*et, "notes").value_or(std::string{});
            p.ecus.push_back(std::move(e));
        }
    }

    if (auto const lf = (*vp)["last_flash"].as_table(); lf != nullptr) {
        LastFlash f;
        f.timestamp_iso = get<std::string>(*lf, "timestamp").value_or(std::string{});
        f.source_rom_path = get<std::string>(*lf, "source_rom_path").value_or(std::string{});
        f.source_rom_crc32 = static_cast<std::uint32_t>(
            get<std::int64_t>(*lf, "source_rom_crc32").value_or(0));
        f.target_rom_path = get<std::string>(*lf, "target_rom_path").value_or(std::string{});
        f.target_rom_crc32 = static_cast<std::uint32_t>(
            get<std::int64_t>(*lf, "target_rom_crc32").value_or(0));
        f.backup_path = get<std::string>(*lf, "backup_path").value_or(std::string{});
        f.operator_notes = get<std::string>(*lf, "operator_notes").value_or(std::string{});
        p.last_flash = std::move(f);
    }

    return p;
}

Result<std::vector<VehicleProfile>> list(std::filesystem::path const &dir) {
    std::vector<VehicleProfile> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return out; // empty dir is fine
    }
    for (auto const &entry : std::filesystem::directory_iterator{dir, ec}) {
        if (entry.path().extension() != ".stprofile")
            continue;
        auto p = load(entry.path());
        if (!p.has_value()) {
            // Skip malformed profiles rather than failing the whole list —
            // a single bad file shouldn't hide the rest from the picker.
            continue;
        }
        out.push_back(std::move(*p));
    }
    std::sort(out.begin(), out.end(),
              [](VehicleProfile const &a, VehicleProfile const &b) { return a.id < b.id; });
    return out;
}

std::filesystem::path default_profile_dir() {
#if defined(_WIN32)
    char const *appdata = std::getenv("APPDATA");
    if (appdata != nullptr) {
        return std::filesystem::path{appdata} / "SubuwuTuner" / "profiles";
    }
    return std::filesystem::path{"profiles"};
#else
    char const *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path{xdg} / "subuwutuner" / "profiles";
    }
    char const *home = std::getenv("HOME");
    if (home != nullptr) {
        return std::filesystem::path{home} / ".config" / "subuwutuner" / "profiles";
    }
    return std::filesystem::path{"profiles"};
#endif
}

} // namespace st::profile
