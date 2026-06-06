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
#include <limits>
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

// Per-DID response payload width in bytes. Different AP firmware
// versions use different widths — v1.7.6.0 (CCF Gen3) widened the
// payload to fit more signals. ECU response size = sum(payloads) +
// 2 bytes per DID id echo + 1 byte service-positive code (0x62) +
// 1 byte for the trailing SID echo.
struct DidPayload {
    std::uint16_t did;
    std::uint8_t  bytes;
};

// v1.7.4.2 (CCF Gen2): 67 payload bytes / 78-byte ECU response.
// Confirmed from `bus-check.log`.
inline constexpr std::array<DidPayload, 5> kDidPayloadsV1_7_4_2{{
    {0xF300, 22},
    {0xF301, 19},
    {0xF302, 10},
    {0xF303, 10},
    {0xF304,  6},
}};

// v1.7.6.0 (CCF Gen3): 75 payload bytes / 86-byte ECU response.
// Confirmed from the dmann-sniff-20260528-181747 driving capture.
inline constexpr std::array<DidPayload, 5> kDidPayloadsV1_7_6_0{{
    {0xF300, 26},
    {0xF301, 20},
    {0xF302, 10},
    {0xF303, 10},
    {0xF304,  9},
}};

// Backward-compat alias — v1.7.4.2 widths. Old call sites that
// referenced kDidPayloads / kTotalPayloadBytes (anchored to bus-check
// .log) keep working. New code should use did_payloads(firmware).
inline constexpr auto kDidPayloads = kDidPayloadsV1_7_4_2;
inline constexpr std::size_t kTotalPayloadBytes = 67;
inline constexpr std::size_t kTotalPayloadBytesV1_7_4_2 = 67;
inline constexpr std::size_t kTotalPayloadBytesV1_7_6_0 = 75;

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
    // AP-internal monitor identifier from the cfg list (e.g.
    // "SSM_RPM", "RAM_COMM_FUEL_FINAL"). Stable across AP firmware
    // versions where the same logical monitor exists — use this as
    // the cross-firmware join key rather than `name` (which is the
    // user-facing label and can change). Empty for Hypothesized
    // entries whose cfg-row hasn't been confirmed.
    std::string_view    monitor_id;
    // True when the RAM address is catalog-authoritative (SSM_*
    // monitor family): COBB reads the OEM SSM-protocol RAM at the
    // listed address. False = candidate (RAM_* monitor family):
    // COBB may read the OEM RAM at this address or a COBB-tune-patch
    // RAM at an unknown address; the wire decode works regardless,
    // but a SSM-0xA8 fallback for this signal cannot trust the
    // ram_address without on-car verification.
    bool                ram_authoritative{false};
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
// v1.7.4.2 (CCF Gen2): 31 signals across F300..F302; 67-byte response.
//   100% catalog-mapped to LF79103P RAM addresses (high-confidence
//   join). All entries Hypothesized — R²-fit was run against a
//   v1.7.6.0 sniff only.
//
// v1.7.6.0 (CCF Gen3): 44 signals across F300..F304; 75-byte response.
//   12 entries R²-verified ≥ 0.95 against the dmann driving sniff
//   (Verified); the remaining 32 are CSV-column-order inferences
//   (Hypothesized) and many disagree with the Verified positions at
//   the same DID byte offset.
//
// Naming reference (see findings/.../COBB_MONITOR_IDS.md):
//   SSM_*  — OEM Subaru SSM-protocol RAM reads. RAM address is
//            authoritative from the firmware live-signals catalog.
//   RAM_*  — COBB-defined RAM reads, often from a Cobb tune-patch
//            region. RAM addresses for these are candidate-only —
//            Cobb may read OEM RAM or a Cobb-patch RAM at a different
//            address. Verified Cobb-monitor entries here may carry a
//            ram_address that's a best guess.
//
// Cross-CID portability story (analyst 2026-06-06 per_cid_packs/):
//   - Byte positions + wire scales (factor/offset) are assumed
//     portable across A-series CIDs — Cobb's AP firmware writes
//     one response template regardless of which A-series CID
//     answers. This assumption holds for the 7 LF75404H / LF75404S
//     / LF79103P / LF9C102P / LF9D012H / LF9G003T / LF9L000E packs
//     the analyst generated.
//   - RAM addresses are LF79103P-specific. Every `ram_address`
//     in this file refers to the LF79103P live-signals catalog.
//     For other CIDs, look up the same `monitor_id` in that CID's
//     pack at `findings/.../per_cid_packs/cobb_pids_<CID>.toml`.
//   - LF75600H is the one A-series CID where 3 of the 12 signals
//     have no high-confidence catalog match (per-CID README) —
//     don't assume the LF79103P pack ports cleanly there.
std::span<CobbSignalLayout const> ap_v1_7_4_2_layout() noexcept;
std::span<CobbSignalLayout const> ap_v1_7_6_0_layout() noexcept;
std::span<CobbSignalLayout const> ap_layout(CobbApFirmware fw) noexcept;

// Per-firmware DID payload widths (5 entries for the 5 polled DIDs).
[[nodiscard]] std::span<DidPayload const>
did_payloads(CobbApFirmware fw) noexcept;

// Per-firmware total payload byte count (sum of the 5 DID widths).
[[nodiscard]] std::size_t total_payload_bytes(CobbApFirmware fw) noexcept;

// Look up the signal at (did, byte_offset) within a specific firmware
// layout. Returns nullptr if the position is not in that layout.
[[nodiscard]] CobbSignalLayout const *
find_signal(CobbApFirmware fw, std::uint16_t did,
            std::uint8_t byte_offset) noexcept;

// Backwards-compat overload — defaults to v1.7.6.0 (CCF Gen3), the
// firmware version most users of recent APs will be running.
[[nodiscard]] CobbSignalLayout const *
find_signal(std::uint16_t did, std::uint8_t byte_offset) noexcept;

// Decode the raw bytes of a single DID payload into the engineering
// value for one signal, applying the wire-side cobb_scale + cobb_offset.
//
// `did_payload` is the DID's payload region of the ECU response — for
// example, F301's 20 bytes in a v1.7.6.0 response. Caller is responsible
// for slicing the right DID out of the multi-frame response per the
// per-firmware widths (see did_payloads()).
//
// Returns NaN when `layout.byte_offset` (+ storage width) is out of
// range, or when `cobb_scale == 0.0` (Hypothesized entry — no wire-side
// transform is available, callers should evaluate the catalog `scaling`
// expression instead).
//
// Designed to mirror the analyst's demo_decode_sniff.py end-to-end
// closing-loop demo. Producing a Verified-row value from sniffed bytes
// is the smallest concrete "this data is useful" capability for the
// const-data layouts.
[[nodiscard]] double
decode_signal(CobbSignalLayout const &layout,
              std::span<std::uint8_t const> did_payload) noexcept;

} // namespace st::ecu::cobb_datalog

#endif // ST_ECU_COBB_DATALOG_HPP
