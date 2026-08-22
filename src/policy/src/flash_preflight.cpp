// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <st/policy/flash_preflight.hpp>

#include <cstdio>
#include <limits>
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

Validator make_ecu_identity_known() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.expected_ecu_id || ctx.expected_ecu_id->empty() ||
            !ctx.observed_ecu_id || ctx.observed_ecu_id->empty()) {
            return {Diagnostic{
                Severity::Blocker, kCatEcuIdentityKnown,
                "The expected and observed ECU calibration IDs are both required "
                "before a write. Identify the exact ECU and definition first."}};
        }
        return {};
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

Validator make_ecu_communication_safe() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (!ctx.ecu_communication_state ||
            *ctx.ecu_communication_state == EcuCommunicationState::Unknown ||
            *ctx.ecu_communication_state == EcuCommunicationState::Reachable) {
            return {};
        }

        if (*ctx.ecu_communication_state == EcuCommunicationState::SilentBootFailure) {
            return {Diagnostic{
                Severity::Blocker, kCatEcuCommunication,
                "The ECU is silent after power-cycle: no CAN/UDS response was observed. "
                "Refusing blind OBD writes; preserve logs and use the exact external "
                "boot/JTAG recovery path for this ECU."}};
        }

        return {Diagnostic{
            Severity::Blocker, kCatEcuCommunication,
            "The ECU was identified, but programming was rejected. Refusing repeated "
            "blind retries; preserve the stock image and obtain a documented recovery "
            "procedure for this exact ECU."}};
    };
}

namespace {

Validator make_required_proof(std::optional<bool> PreflightContext::*field,
                              char const *category,
                              char const *label,
                              char const *next_action) {
    return [field, category, label, next_action](PreflightContext const &ctx)
               -> std::vector<Diagnostic> {
        auto const &value = ctx.*field;
        if (!value || !*value) {
            return {Diagnostic{Severity::Blocker, category,
                               std::string{label} + " has not been positively verified. " +
                                   next_action}};
        }
        return {};
    };
}

} // namespace

Validator make_definition_match_verified() {
    return make_required_proof(&PreflightContext::definition_match_verified,
                                kCatDefinitionMatch, "Definition match",
                                "Refusing to write until the exact CID and definition pack "
                                "are confirmed.");
}

Validator make_source_image_verified() {
    return make_required_proof(&PreflightContext::source_image_verified,
                                kCatSourceImage, "Source image",
                                "Preserve and verify the source image before continuing.");
}

Validator make_recovery_image_present() {
    return make_required_proof(&PreflightContext::recovery_image_present,
                                kCatRecoveryImage, "Recovery image",
                                "Provide an exact-CID stock or otherwise approved recovery "
                                "image.");
}

Validator make_journal_safe() {
    return make_required_proof(&PreflightContext::journal_safe, kCatJournalSafety,
                                "Flash journal", "Resolve or explicitly resume the interrupted "
                                "flash journal before starting another write.");
}

Validator make_write_regions_safe() {
    return [](PreflightContext const &ctx) -> std::vector<Diagnostic> {
        if (ctx.planned_write_ranges.empty()) {
            return {};
        }
        if (ctx.approved_write_regions.empty()) {
            return {Diagnostic{
                Severity::Blocker, kCatWriteRegions,
                "The flash plan contains writes but no approved flash write regions "
                "were supplied. Refusing to write outside an explicit address "
                "allow-list."}};
        }

        for (auto const &planned : ctx.planned_write_ranges) {
            if (planned.length == 0 ||
                planned.address > std::numeric_limits<std::uint64_t>::max() - planned.length) {
                return {Diagnostic{
                    Severity::Blocker, kCatWriteRegions,
                    "The flash plan contains an invalid or overflowing write range. "
                    "Refusing to emit a plan with an unsafe address extent."}};
            }

            auto const planned_end = planned.address + planned.length;
            bool contained = false;
            for (auto const &region : ctx.approved_write_regions) {
                if (region.length == 0 ||
                    region.address > std::numeric_limits<std::uint64_t>::max() - region.length) {
                    continue;
                }
                auto const region_end = region.address + region.length;
                if (planned.address >= region.address && planned_end <= region_end) {
                    contained = true;
                    break;
                }
            }
            if (!contained) {
                return {Diagnostic{
                    Severity::Blocker, kCatWriteRegions,
                    "The flash plan contains a range outside every approved flash write "
                    "region, or spans a region boundary. Refusing the write."}};
            }
        }
        return {};
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
        .add(make_ecu_identity_known())
        .add(make_vin_match())
        .add(make_ecu_communication_safe())
        .add(make_definition_match_verified())
        .add(make_source_image_verified())
        .add(make_recovery_image_present())
        .add(make_journal_safe())
        .add(make_write_regions_safe())
        .add(make_battery_voltage_ok())
        .add(make_ignition_on())
        .add(make_checksum_known())
        .add(make_backup_present())
        .add(make_write_extent_sane());
    return p;
}

std::vector<ValidatorDescription> available_validators() {
    return {
        {kCatEcuIdMatch, "EcuIdMatch",
         "Refuses if the ECU reports a calibration id different from the "
         "source ROM's pack metadata.",
         "", "expected_ecu_id or observed_ecu_id unset", ""},
        {kCatEcuIdentityKnown, "EcuIdentityKnown",
         "Requires both expected and observed calibration IDs before a write.",
         "", "", "expected_ecu_id or observed_ecu_id unset/empty"},
        {kCatVinMatch, "VinMatch",
         "Warns if the host has an expected VIN but the ECU did not report "
         "one; blocks on a hard mismatch.",
         "", "expected_vin or observed_vin unset", ""},
        {kCatEcuCommunication, "EcuCommunicationSafe",
         "Refuses blind writes after a programming rejection or silent boot "
         "failure; directs the operator to documented recovery.",
         "", "ecu_communication_state unset, Unknown, or Reachable", ""},
        {kCatDefinitionMatch, "DefinitionMatchVerified",
         "Requires positive confirmation that the exact definition matches the ECU.",
         "", "", "definition_match_verified unset or false"},
        {kCatSourceImage, "SourceImageVerified",
         "Requires a preserved and verified source image for the write.",
         "", "", "source_image_verified unset or false"},
        {kCatRecoveryImage, "RecoveryImagePresent",
         "Requires an exact-CID stock or approved recovery image.",
         "", "", "recovery_image_present unset or false"},
        {kCatJournalSafety, "JournalSafe",
         "Refuses a new write while an interrupted flash journal is unresolved.",
         "", "", "journal_safe unset or false"},
        {kCatWriteRegions, "WriteRegionsSafe",
         "Requires every planned write range to fit inside one approved flash write region.",
         "", "planned_write_ranges empty or all ranges contained", ""},
        {kCatBatteryVoltage, "BatteryVoltageOk",
         "Refuses below the engine-cranking-safe floor; warns below the "
         "resting-target floor.",
         "block_below=11.5 V, warn_below=12.0 V",
         "battery_voltage_v not reported by transport", ""},
        {kCatIgnitionOn, "IgnitionOn",
         "Refuses when the transport reports ignition off (no engine "
         "control while writing = unsafe).",
         "", "ignition_on not reported by transport", ""},
        {kCatChecksumKnown, "ChecksumKnown",
         "Refuses if the host doesn't know how to recompute the ECU's "
         "checksum (post-write verify would fail).",
         "", "checksum_strategy_known unset", ""},
        {kCatBackupPresent, "BackupPresent",
         "Refuses without a fresh verified backup of the connected ECU.",
         "", "backup_present unset", ""},
        {kCatWriteExtent, "WriteExtentSane",
         "Warns if the plan would write more than 80% of the ROM; blocks "
         "if it exceeds 100% (impossible without an addressing bug).",
         "warn_fraction=0.80, block_fraction=1.00",
         "bytes_to_write or source_rom_size unset", ""},
    };
}

} // namespace st::policy
