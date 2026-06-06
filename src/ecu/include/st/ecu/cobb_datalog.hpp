// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::cobb_datalog — protocol-level constants for the Cobb AccessPort's
// live datalog request shape.
//
// Sourced clean-room from analyst-side captures: bus-check.log (one
// ignition-on capture of the AP polling the ECU) cross-referenced against
// 18 datalog CSV exports from the AP's own log directory. Per-byte signal
// layout is the AP firmware v1.7.6.0 "CCF Gen3" preset. The protocol
// (5 DIDs, ~25 Hz polling, ISO-TP-over-CAN-FD on 0x7E0/0x7E8) is
// fully-confirmed; the per-byte signal mapping is high-confidence from
// CSV ↔ DID-payload width matching but pending a driving-capture
// ground-truth (analyst handoff §"Open" — needs one fresh on-car
// bus-check with the engine running).
//
// Scope:
//   - Constants only. No I/O, no transport binding, no scaling math.
//   - The future live datalogger consumes these via a
//     "channel preset = cobb_ap_v1_7_6_0" knob; nothing that lands
//     today claims the live path is ready.
//
// Clean-room boundary: AP firmware itself was never inspected. The DID
// set comes from on-the-wire request bytes; the byte layout comes from
// the user's own AP log directory (CSV outputs the AP wrote). Both are
// the user's data, not a decompile.

#ifndef ST_ECU_COBB_DATALOG_HPP
#define ST_ECU_COBB_DATALOG_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace st::ecu::cobb_datalog {

// ---- Protocol shape (confirmed from bus-check.log) ----------------------

// Tester (host) → ECU request CAN id on ISO-15765. AP v1.7.4.2-era
// uses the standard OBD-II tester id (functional/physical depending on
// session state); the captures show physical 0x7E0 ↔ 0x7E8 framing.
inline constexpr std::uint32_t kRequestCanId = 0x7E0;
inline constexpr std::uint32_t kResponseCanId = 0x7E8;

// UDS Service ID — Read Data By Identifier. Cobb's datalog mode uses
// SID 0x22 exclusively (no SecurityAccess unlock during the datalog
// phase — the L3 latch persists from a prior install/uninstall session
// per the analyst's lazy-verify hypothesis).
inline constexpr std::uint8_t kReadDataByIdentifier = 0x22;

// Median polling interval observed in bus-check.log: 39 ms (~25 Hz).
// p95 = 91 ms, min = 8 ms. Worth matching for behavioural parity.
inline constexpr std::uint32_t kMedianPollIntervalMs = 39;

// The DID set the AP polls. Always exactly these 5, always in this
// order, packed into a single 13-byte tester request:
//   22 F3 00 F3 01 F3 02 F3 03 F3 04
inline constexpr std::array<std::uint16_t, 5> kDidSet{
    0xF300, 0xF301, 0xF302, 0xF303, 0xF304,
};

// Per-DID response payload width in bytes (sum = 67). Total ECU
// response is 78 bytes (67 payload + per-DID id bytes + service code).
struct DidPayload {
    std::uint16_t did;
    std::uint8_t  bytes;
};
inline constexpr std::array<DidPayload, 5> kDidPayloads{{
    {0xF300, 22},
    {0xF301, 19},
    {0xF302, 10},
    {0xF303, 10},
    {0xF304,  6},
}};

inline constexpr std::size_t kTotalPayloadBytes = 67;

// ---- Per-byte signal layout (AP v1.7.6.0 "CCF Gen3" hypothesis) ---------
//
// Each entry maps a (did, byte_offset_within_did) → named signal +
// storage shape + scale. Inferred from 18 AP CSV exports, validated
// against the 6 varying bytes in the engine-off bus-check capture.
// Confidence: high but unconfirmed-on-driving-data.
//
// Width values are post-aggregation (uint16 spans two consecutive
// byte_offsets; the second is implicit and not duplicated in this
// table). Endianness is big-endian per the SH-2A ECU's wire format.
//
// `scale` is the divisor used to recover engineering units:
//   value_eng = (raw / scale).
// For signed storage, sign-extend before dividing.

enum class CobbSignalStorage : std::uint8_t {
    Uint8,
    Int8,
    Uint16,    // big-endian (standard SH-2A wire byte order)
    Int16,     // big-endian
    Uint16Le,  // little-endian — AP packs some values LE on the wire
    Int16Le,   // little-endian
};

// Confidence tier for a signal's byte position. The RAM-address +
// signal-name join is high-confidence everywhere (analyst's catalog
// match scores ≥ 0.6, most are exact). The byte position within
// each DID is a separate axis: a handful of signals have been
// R²-verified against a real driving capture; the rest are
// hypothesized from CSV column order and may be wrong.
enum class CobbVerification : std::uint8_t {
    // Position inferred from AP CSV column order — catalog name match
    // is exact, but the byte position is a best guess until R²-fit
    // against a real sniff confirms it. Use these as "this is where
    // the byte probably is" not "this is where the byte is."
    Hypothesized,
    // Byte position + storage shape + COBB wire scale all confirmed
    // via R²-fit (≥0.95) against the analyst's dmann driving sniff.
    // Use these as ground truth.
    Verified,
};

struct CobbSignalLayout {
    std::uint16_t       did;
    std::uint8_t        byte_offset;
    CobbSignalStorage   storage;
    // LF79103P RAM address the AP reads to populate this byte. Sourced
    // from the analyst's join of (CSV column order × firmware live-
    // signals catalog name match). Unique per-firmware-version; the
    // same signal name across firmwares may resolve to different RAM
    // addresses if the underlying RAM moved between calibrations.
    std::uint32_t       ram_address;
    std::string_view    name;
    std::string_view    unit;
    // Raw → engineering-units expression. Variable `x` is the raw
    // value already widened to a 32-bit container (sign-extended for
    // Int8/Int16). Operators per the catalog: `+ - * / >> << & | (...)`.
    // No exponentiation, no function calls. Some firmware-pack
    // calibrations use floating-point literals — evaluate accordingly.
    // Expressions are pass-through analyst-side; an evaluator that
    // accepts this grammar will land alongside the live datalogger
    // (docs/32).
    //
    // For Verified entries this is the catalog (firmware-internal)
    // expression. The AP applies a different `cobb_scale` /
    // `cobb_offset` on the wire — see those fields. For Hypothesized
    // entries the wire-side transform is unknown; the catalog
    // expression is what the firmware would compute internally.
    std::string_view    scaling;
    // Confidence tier — see CobbVerification.
    CobbVerification    verification{CobbVerification::Hypothesized};
    // Wire-side scale + offset. Populated only for Verified entries;
    // zero/NaN for Hypothesized. value_engineering = raw * cobb_scale
    // + cobb_offset (decode straight from the wire). The catalog
    // `scaling` expression evaluates the same value from the
    // ram_address. The two often differ — the AP pre-transforms the
    // RAM value before packing into the F3xx payload.
    double              cobb_scale{0.0};
    double              cobb_offset{0.0};
};

// AP firmware version tags. Pre-/post-CCF-Gen2-to-Gen3 transition.
// Different DID byte order, different signal sets, sometimes different
// RAM addresses for the same logical signal.
enum class CobbApFirmware : std::uint8_t {
    V1_7_4_2_CCF_Gen2,  // 31 signals across F300..F302
    V1_7_6_0_CCF_Gen3,  // 43 signals across F300..F304
};

// Per-firmware signal layouts.
//
// v1.7.4.2 (CCF Gen2): 31 signals, 100% catalog-mapped to LF79103P
//   RAM addresses (analyst's high-confidence join).
//
// v1.7.6.0 (CCF Gen3): 43 signals across F300..F304 (88% of the AP's
//   49-signal gauge set). Six signals overflowed the 67-byte budget
//   in the analyst's inference (SD VE Est MAF, TD Boost Error Ext,
//   TGV Map Ratio, Throttle Pos, Vehicle Speed, Wastegate Duty) —
//   pending an on-car sniff to confirm whether they live in an
//   unseen F305 DID.
std::span<CobbSignalLayout const> ap_v1_7_4_2_layout() noexcept;
std::span<CobbSignalLayout const> ap_v1_7_6_0_layout() noexcept;
std::span<CobbSignalLayout const> ap_layout(CobbApFirmware fw) noexcept;

// Look up the signal at (did, byte_offset) within a specific firmware
// layout. Returns nullptr if the position is not in that layout.
[[nodiscard]] CobbSignalLayout const *
find_signal(CobbApFirmware fw, std::uint16_t did,
            std::uint8_t byte_offset) noexcept;

// Backwards-compat overload — defaults to v1.7.6.0 (CCF Gen3), the
// firmware version most users of recent APs will be running.
[[nodiscard]] CobbSignalLayout const *
find_signal(std::uint16_t did, std::uint8_t byte_offset) noexcept;

} // namespace st::ecu::cobb_datalog

#endif // ST_ECU_COBB_DATALOG_HPP
