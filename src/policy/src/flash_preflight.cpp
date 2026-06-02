// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <st/policy/flash_preflight.hpp>

#include <cstdio>
#include <string>
#include <utility>

namespace st::policy {

DiagnosticReport Pipeline::run(PreflightContext const &ctx) const {
    DiagnosticReport report;
    for (auto const &v : validators_) {
        auto items = v(ctx);
        for (auto &d : items) {
            report.push(std::move(d));
        }
    }
    return report;
}

// ---------------------------------------------------------------------
// Built-in validator factories
// ---------------------------------------------------------------------

Validator make_ecu_id_match() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        // If either side is unspecified, we cannot make a positive
        // statement either way. Return empty; some other validator (or
        // host policy) is responsible for deciding whether unknown ECU
        // ID is itself a Blocker.
        if (!ctx.expected_ecu_id || !ctx.observed_ecu_id) {
            return {};
        }
        if (*ctx.expected_ecu_id == *ctx.observed_ecu_id) {
            return {};
        }
        std::string const msg = "ECU ID mismatch: source ROM expects '" + *ctx.expected_ecu_id +
                                "', connected ECU reports '" + *ctx.observed_ecu_id + "'.";
        return {Diagnostic{Severity::Blocker, kCatEcuIdMatch, msg}};
    };
}

Validator make_vin_match() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.expected_vin) {
            return {}; // Source ROM did not encode a VIN; nothing to check.
        }
        if (!ctx.observed_vin) {
            return {Diagnostic{
                Severity::Warning, kCatVinMatch,
                "Source ROM encodes a VIN but the connected ECU did not report one; "
                "cannot verify vehicle identity."}};
        }
        if (*ctx.expected_vin == *ctx.observed_vin) {
            return {};
        }
        return {Diagnostic{Severity::Blocker, kCatVinMatch,
                           "VIN mismatch: source ROM encodes '" + *ctx.expected_vin +
                               "', connected ECU reports '" + *ctx.observed_vin + "'."}};
    };
}

Validator make_battery_voltage_ok(double warn_below, double block_below) {
    return [warn_below, block_below](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.battery_voltage_v) {
            return {}; // Transport could not report; skip.
        }
        double const v = *ctx.battery_voltage_v;
        if (v < block_below) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Battery voltage %.2f V is below the safe-write threshold (%.2f V). "
                          "Connect a battery maintainer before continuing.",
                          v, block_below);
            return {Diagnostic{Severity::Blocker, kCatBatteryVoltage, buf}};
        }
        if (v < warn_below) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Battery voltage %.2f V is below the comfortable threshold (%.2f V). "
                          "A battery maintainer is recommended for long flash sessions.",
                          v, warn_below);
            return {Diagnostic{Severity::Warning, kCatBatteryVoltage, buf}};
        }
        return {};
    };
}

Validator make_ignition_on() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.ignition_on) {
            return {}; // Transport cannot report; skip.
        }
        if (!*ctx.ignition_on) {
            return {Diagnostic{Severity::Blocker, kCatIgnitionOn,
                               "Ignition is reported off. Turn the key to ON (engine not "
                               "running) before continuing."}};
        }
        return {};
    };
}

Validator make_checksum_known() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.checksum_strategy_known) {
            return {Diagnostic{Severity::Blocker, kCatChecksumKnown,
                               "Checksum strategy for the connected ECU family is not known. "
                               "Refusing to write — a successful write that cannot be "
                               "verified is a brick risk."}};
        }
        if (!*ctx.checksum_strategy_known) {
            return {Diagnostic{Severity::Blocker, kCatChecksumKnown,
                               "Checksum strategy for the connected ECU family is not known. "
                               "Refusing to write — a successful write that cannot be "
                               "verified is a brick risk."}};
        }
        return {};
    };
}

Validator make_backup_present() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.backup_present || !*ctx.backup_present) {
            return {Diagnostic{
                Severity::Blocker, kCatBackupPresent,
                "No verified backup of the current ECU contents is available. "
                "Create one via BackupStore::create() before writing — a write without a "
                "verified backup is irrecoverable on failure."}};
        }
        return {};
    };
}

Validator make_write_extent_sane(double warn_fraction, double block_fraction) {
    return [warn_fraction, block_fraction](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.source_rom_size || !ctx.bytes_to_write) {
            return {};
        }
        if (*ctx.source_rom_size == 0) {
            return {}; // Degenerate; let some other validator complain.
        }
        double const frac = static_cast<double>(*ctx.bytes_to_write) /
                            static_cast<double>(*ctx.source_rom_size);
        if (frac > block_fraction) {
            char buf[180];
            std::snprintf(buf, sizeof(buf),
                          "Plan writes %llu bytes against a %llu-byte ROM (%.0f%%). "
                          "Refusing — exceeds the ROM extent, indicates an addressing bug.",
                          static_cast<unsigned long long>(*ctx.bytes_to_write),
                          static_cast<unsigned long long>(*ctx.source_rom_size), frac * 100.0);
            return {Diagnostic{Severity::Blocker, kCatWriteExtent, buf}};
        }
        if (frac > warn_fraction) {
            char buf[180];
            std::snprintf(buf, sizeof(buf),
                          "Plan writes %llu bytes against a %llu-byte ROM (%.0f%%). "
                          "That is a large fraction of the ROM — confirm this is intentional.",
                          static_cast<unsigned long long>(*ctx.bytes_to_write),
                          static_cast<unsigned long long>(*ctx.source_rom_size), frac * 100.0);
            return {Diagnostic{Severity::Warning, kCatWriteExtent, buf}};
        }
        return {};
    };
}

Pipeline default_pipeline() {
    Pipeline p;
    p.add(make_ecu_id_match())
        .add(make_vin_match())
        .add(make_battery_voltage_ok())
        .add(make_ignition_on())
        .add(make_checksum_known())
        .add(make_backup_present())
        .add(make_write_extent_sane());
    return p;
}

} // namespace st::policy
