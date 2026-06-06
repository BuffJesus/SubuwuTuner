// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_POLICY_FLASH_PREFLIGHT_HPP
#define ST_POLICY_FLASH_PREFLIGHT_HPP

#include "st/core/diagnostic.hpp"
#include "st/policy.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// =====================================================================
// st::policy::FlashPreflight — composable pre-flight validator pipeline
// =====================================================================
//
// Before `st::flash::Flasher::execute()` runs against a real ECU, the
// host runs a pipeline of cheap validators that confirm: the connected
// ECU is the one the user thinks it is, the bytes about to be written
// match that ECU, the electrical environment is safe to begin a write,
// the checksum strategy is known so the recompute step can complete,
// and a verified backup exists.
//
// A validator is a free function (or functor) that takes a
// `PreflightContext` and returns zero or more `Diagnostic`s. A Pipeline
// collects validators and runs them in order; the resulting
// `DiagnosticReport` is the single value callers use to decide.
//
// All validators run regardless of earlier failures — collecting every
// blocker in one pass produces a more useful UI than "fix one error at
// a time". For very expensive validators (uncommon today), a future
// `RunMode::FailFast` could short-circuit; not implemented in this
// slice.
//
// Independent of `st::policy::Action` ordering: the preflight pipeline
// always produces Diagnostics. A Diagnostic of `Severity::Blocker`
// always refuses, regardless of jurisdiction profile. Jurisdiction
// profiles influence which validators fire (e.g. emissions-relevant
// edits in `EuRoadworthy` add a `Warning`, but engine_safety_critical
// edits always produce a `Blocker` regardless of profile).

namespace st::policy {

// Inputs to every preflight validator. Construct one per flash attempt
// from the application state (ECU live readout, project pack, vehicle
// profile). Fields are intentionally optional — validators ignore
// fields they do not consume, so missing data widens the surface of
// what the pipeline can NOT prove rather than throwing.
struct PreflightContext {
    // What we expect the connected ECU to identify as, from the source
    // ROM's pack metadata. When unset, the EcuIdMatch validator is a
    // no-op (treat as "user has not stated an expectation").
    std::optional<std::string> expected_ecu_id{};
    // What the connected ECU just responded with on its identification
    // request. Unset means the host has not yet read the ECU (or could
    // not). The EcuIdMatch validator surfaces a Blocker iff both fields
    // are set AND differ.
    std::optional<std::string> observed_ecu_id{};

    std::optional<std::string> expected_vin{};
    std::optional<std::string> observed_vin{};

    // Battery voltage reported by the transport device, in volts. When
    // unset, BatteryVoltageOk is a no-op. The default thresholds (warn
    // < 12.0 V, blocker < 11.5 V) live on the validator factory.
    std::optional<double> battery_voltage_v{};

    // Whether the ignition is reported on. False -> Blocker.
    // Some transports cannot report this; leave unset to skip.
    std::optional<bool> ignition_on{};

    // Whether the host has a known checksum strategy for the connected
    // ECU's family. Unset -> Blocker (refuse to write without a way to
    // verify after).
    std::optional<bool> checksum_strategy_known{};

    // Whether a fresh verified backup exists for the connected ECU.
    // Unset -> Blocker. The host fills this from `BackupStore::list`
    // (filtered by ecu_id + age).
    std::optional<bool> backup_present{};

    // The currently active jurisdiction profile. Validators consult
    // this for profile-conditioned advisories.
    Profile profile{Profile::MotorsportOnly};

    // The size of the source ROM in bytes. Used by validators that
    // sanity-check writes against the address space (e.g. refuse to
    // queue writes past the ROM end).
    std::optional<std::uint64_t> source_rom_size{};

    // Cumulative byte count the plan would write. Validators may
    // surface a Warning if extreme (e.g. > 80% of ROM) and a Blocker
    // if obviously wrong (e.g. > 100% of ROM).
    std::optional<std::uint64_t> bytes_to_write{};
};

using Validator = std::function<std::vector<Diagnostic>(PreflightContext const &)>;

// =====================================================================
// Pipeline
// =====================================================================

class Pipeline {
public:
    Pipeline() = default;

    Pipeline &add(Validator v) {
        validators_.push_back(std::move(v));
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return validators_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return validators_.empty();
    }

    // Run every validator against `ctx` and collect their Diagnostics
    // into a single `DiagnosticReport`. Order is preserved: validators
    // earlier in the pipeline contribute earlier diagnostics.
    [[nodiscard]] DiagnosticReport run(PreflightContext const &ctx) const;

private:
    std::vector<Validator> validators_{};
};

// =====================================================================
// Built-in validator factories
// =====================================================================
//
// Each factory returns a Validator closure with the relevant thresholds
// pre-bound. Defaults are designed to be conservative (refuse rather
// than warn when in doubt); callers can override per-deployment.

[[nodiscard]] Validator make_ecu_id_match();
[[nodiscard]] Validator make_vin_match(); // Warning-tier when expected set + observed unset.

// Battery thresholds in volts. `warn_below` defaults to 12.0 V
// (resting); `block_below` defaults to 11.5 V (engine cranking can dip
// this far transiently — the validator still refuses, the host can
// surface a "wait for battery to settle" hint).
[[nodiscard]] Validator make_battery_voltage_ok(double warn_below = 12.0,
                                                double block_below = 11.5);
[[nodiscard]] Validator make_ignition_on();
[[nodiscard]] Validator make_checksum_known();
[[nodiscard]] Validator make_backup_present();

// Sanity-checks `bytes_to_write` against `source_rom_size`.
// Defaults: warn if writes exceed 80% of ROM; block if writes exceed
// 100% (impossible without an addressing bug).
[[nodiscard]] Validator make_write_extent_sane(double warn_fraction = 0.80,
                                               double block_fraction = 1.00);

// =====================================================================
// Convenience: assemble all built-in validators with default thresholds.
// =====================================================================
//
// Use this in production to get the canonical pipeline. Tests typically
// build pipelines from scratch to focus on one validator at a time.
[[nodiscard]] Pipeline default_pipeline();

// Stable category strings used in Diagnostic::category(). Round-trip
// safe; UI/CLI consumers can group on these.
inline constexpr char kCatEcuIdMatch[] = "ecu_id_match";
inline constexpr char kCatVinMatch[] = "vin_match";
inline constexpr char kCatBatteryVoltage[] = "battery_voltage";
inline constexpr char kCatIgnitionOn[] = "ignition_on";
inline constexpr char kCatChecksumKnown[] = "checksum_known";
inline constexpr char kCatBackupPresent[] = "backup_present";
inline constexpr char kCatWriteExtent[] = "write_extent";

// Static-description record for each built-in validator. Used by the
// `subuwutuner-cli list-validators` verb and the GUI's "what does the
// flash gate actually check?" affordance — so a user staring at a
// Blocker can see the full validator inventory + when each fires.
struct ValidatorDescription {
    std::string_view category;            // matches kCat* constants
    std::string_view name;                // human-readable label
    std::string_view description;         // 1-line summary
    std::string_view default_thresholds;  // empty when not numerically gated
    std::string_view skip_when;           // condition under which the validator no-ops
};

// Returns the static inventory of validators that `default_pipeline()`
// assembles. Order matches the pipeline order so callers can render
// "run sequence" verbatim.
[[nodiscard]] std::vector<ValidatorDescription> available_validators();

} // namespace st::policy

#endif // ST_POLICY_FLASH_PREFLIGHT_HPP
