// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::extended_did_datalog — protocol-level constants for an
// aftermarket-datalogger live request shape (SSM-extended DIDs over
// ISO-15765).
//
// Sourced clean-room from bus captures (one ignition-on capture of the
// polling tester) cross-referenced against 18 datalog CSV exports from
// the tester's own log directory. Per-byte signal layout is the wider
// of the two observed firmware revisions. The protocol (5 DIDs, ~25 Hz
// polling, ISO-TP-over-CAN-FD on 0x7E0/0x7E8) is fully confirmed; the
// per-byte signal mapping is high-confidence from CSV ↔ DID-payload
// width matching but pending a driving-capture ground-truth.
//
// Scope:
//   - Constants only. No I/O, no transport binding, no scaling math.
//   - The future live datalogger consumes these via a
//     "channel preset = wide_v1" knob; nothing that lands today claims
//     the live path is ready.
//
// Clean-room boundary: the upstream tester's firmware was never
// inspected. The DID set comes from on-the-wire request bytes; the
// byte layout comes from the tester's own CSV log directory. Both are
// bus-observable data, not a decompile.

#ifndef ST_ECU_EXTENDED_DID_DATALOG_HPP
#define ST_ECU_EXTENDED_DID_DATALOG_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace st::ecu::extended_did_datalog {

// ---- Protocol shape (confirmed from bus captures) ----------------------

// Tester (host) → ECU request CAN id on ISO-15765. Early-era testers
// use the standard OBD-II tester id (functional/physical depending on
// session state); the captures show physical 0x7E0 ↔ 0x7E8 framing.
inline constexpr std::uint32_t kRequestCanId = 0x7E0;
inline constexpr std::uint32_t kResponseCanId = 0x7E8;

// UDS Service ID — Read Data By Identifier. The datalog mode uses SID
// 0x22 exclusively (no SecurityAccess unlock during the datalog phase
// — the L3 latch persists from a prior install/uninstall session).
inline constexpr std::uint8_t kReadDataByIdentifier = 0x22;

// Median polling interval observed: 39 ms (~25 Hz). p95 = 91 ms,
// min = 8 ms. Worth matching for behavioural parity.
inline constexpr std::uint32_t kMedianPollIntervalMs = 39;

// The DID set the tester polls. Always exactly these 5, always in
// this order, packed into a single 13-byte tester request:
//   22 F3 00 F3 01 F3 02 F3 03 F3 04
inline constexpr std::array<std::uint16_t, 5> kDidSet{
    0xF300, 0xF301, 0xF302, 0xF303, 0xF304,
};

// Per-DID response payload width in bytes. Different firmware
// revisions of the upstream tester use different widths — the wider
// revision packs more signals. ECU response size = sum(payloads) +
// 2 bytes per DID id echo + 1 byte service-positive code (0x62) +
// 1 byte for the trailing SID echo.
struct DidPayload {
    std::uint16_t did;
    std::uint8_t  bytes;
};

// Narrow layout: 67 payload bytes / 78-byte ECU response.
inline constexpr std::array<DidPayload, 5> kDidPayloadsNarrow{{
    {0xF300, 22},
    {0xF301, 19},
    {0xF302, 10},
    {0xF303, 10},
    {0xF304,  6},
}};

// Wide layout: 75 payload bytes / 86-byte ECU response.
inline constexpr std::array<DidPayload, 5> kDidPayloadsWide{{
    {0xF300, 26},
    {0xF301, 20},
    {0xF302, 10},
    {0xF303, 10},
    {0xF304,  9},
}};

// Backward-compat alias — narrow widths. Old call sites that
// referenced kDidPayloads / kTotalPayloadBytes keep working. New code
// should use did_payloads(layout_version).
inline constexpr auto kDidPayloads = kDidPayloadsNarrow;
inline constexpr std::size_t kTotalPayloadBytes = 67;
inline constexpr std::size_t kTotalPayloadBytesNarrow = 67;
inline constexpr std::size_t kTotalPayloadBytesWide = 75;

// ---- Per-byte signal layout (wide-layout hypothesis) -------------------
//
// Each entry maps a (did, byte_offset_within_did) → named signal +
// storage shape + scale. Inferred from 18 CSV exports, validated
// against the 6 varying bytes in the engine-off bus capture.
// Confidence: high but unconfirmed-on-driving-data.
//
// Width values are post-aggregation (uint16 spans two consecutive
// byte_offsets; the second is implicit and not duplicated in this
// table). Endianness is big-endian per the SH-2A ECU's wire format.
//
// `scale` is the divisor used to recover engineering units:
//   value_eng = (raw / scale).
// For signed storage, sign-extend before dividing.

enum class SignalStorage : std::uint8_t {
    Uint8,
    Int8,
    Uint16,    // big-endian (standard SH-2A wire byte order)
    Int16,     // big-endian
    Uint16Le,  // little-endian — some values are packed LE on the wire
    Int16Le,   // little-endian
};

// Confidence tier for a signal's byte position. The RAM-address +
// signal-name join is high-confidence everywhere (catalog match
// scores ≥ 0.6, most are exact). The byte position within each DID
// is a separate axis: a handful of signals have been R²-verified
// against a real driving capture; the rest are hypothesized from CSV
// column order and may be wrong.
enum class Verification : std::uint8_t {
    // Position inferred from CSV column order — catalog name match is
    // exact, but the byte position is a best guess until R²-fit
    // against a real sniff confirms it. Use these as "this is where
    // the byte probably is" not "this is where the byte is."
    Hypothesized,
    // Byte position + storage shape + wire scale all confirmed via
    // R²-fit (≥0.95) against a real driving sniff. Use these as
    // ground truth.
    Verified,
};

struct SignalLayout {
    std::uint16_t       did;
    std::uint8_t        byte_offset;
    SignalStorage   storage;
    // LF79103P RAM address that gets read to populate this byte.
    // Sourced from joining (CSV column order × firmware live-signals
    // catalog name match). Unique per-layout-version; the same signal
    // name across layouts may resolve to different RAM addresses if
    // the underlying RAM moved between calibrations.
    std::uint32_t       ram_address;
    std::string_view    name;
    std::string_view    unit;
    // Raw → engineering-units expression. Variable `x` is the raw
    // value already widened to a 32-bit container (sign-extended for
    // Int8/Int16). Operators per the catalog: `+ - * / >> << & | (...)`.
    // No exponentiation, no function calls. Some firmware-pack
    // calibrations use floating-point literals — evaluate accordingly.
    // An evaluator that accepts this grammar will land alongside the
    // live datalogger (docs/32).
    //
    // For Verified entries this is the catalog (firmware-internal)
    // expression. The upstream tester applies a different `scale` /
    // `offset` on the wire — see those fields. For Hypothesized
    // entries the wire-side transform is unknown; the catalog
    // expression is what the firmware would compute internally.
    std::string_view    scaling;
    // Confidence tier — see Verification.
    Verification    verification{Verification::Hypothesized};
    // Wire-side scale + offset. Populated only for Verified entries;
    // zero/NaN for Hypothesized. value_engineering = raw * scale
    // + offset (decode straight from the wire). The catalog `scaling`
    // expression evaluates the same value from the ram_address. The
    // two often differ — the tester pre-transforms the RAM value
    // before packing into the F3xx payload.
    double              scale{0.0};
    double              offset{0.0};
    // Tester-internal monitor identifier from the cfg list (e.g.
    // "SSM_RPM", "RAM_COMM_FUEL_FINAL"). Stable across firmware
    // revisions where the same logical monitor exists — use this as
    // the cross-revision join key rather than `name` (which is the
    // user-facing label and can change). Empty for Hypothesized
    // entries whose cfg-row hasn't been confirmed.
    std::string_view    monitor_id;
    // True when the RAM address is catalog-authoritative (SSM_*
    // monitor family): the tester reads the OEM SSM-protocol RAM at
    // the listed address. False = candidate (RAM_* monitor family):
    // the tester may read the OEM RAM at this address or a tune-
    // patch RAM at an unknown address; the wire decode works
    // regardless, but a SSM-0xA8 fallback for this signal cannot
    // trust the ram_address without on-car verification.
    bool                ram_authoritative{false};
};

// Layout version tags. Different DID byte order, different signal
// sets, sometimes different RAM addresses for the same logical
// signal.
enum class DidLayoutVersion : std::uint8_t {
    Narrow,  // 31 signals across F300..F302
    Wide,  // 43 signals across F300..F304
};

// Per-revision signal layouts.
//
// Narrow: 31 signals across F300..F302; 67-byte response. 100%
//   catalog-mapped to LF79103P RAM addresses (high-confidence join).
//   All entries Hypothesized — R²-fit was run against a wide-layout
//   sniff only.
//
// Wide: 44 signals across F300..F304; 75-byte response. 12 entries
//   R²-verified ≥ 0.95 against a real driving sniff (Verified); the
//   remaining 32 are CSV-column-order inferences (Hypothesized) and
//   many disagree with the Verified positions at the same DID byte
//   offset.
//
// Naming convention:
//   SSM_*  — OEM Subaru SSM-protocol RAM reads. RAM address is
//            authoritative from the firmware live-signals catalog.
//   RAM_*  — tester-defined RAM reads, often from an aftermarket
//            tune-patch region. RAM addresses for these are
//            candidate-only — the tester may read OEM RAM or a
//            tune-patch RAM at a different address. Verified RAM_*
//            entries here may carry a ram_address that's a best
//            guess.
//
// Provenance — two confidence axes, distinct sources:
//
//   1. BYTE POSITIONS + WIRE SCALES were R²-verified against a real
//      car sniff. With proper sniff/CSV alignment (RPM anchor
//      R²=0.9999) the tester packs raw SSM values directly with
//      canonical scales (RPM is x/5.12). Aftermarket-tester firmware
//      generally uses one response template per layout-version
//      across the A-series, so these positions + scales are assumed
//      portable across the A-series family. `decode_signal()` relies
//      only on these and works on any CID running the same layout
//      version.
//
//   2. RAM ADDRESSES were sourced from the LF79103P live-signals
//      catalog (the LF79 variant most heavily catalogued). The same
//      monitor_id resolves to per-CID RAM addresses for other
//      A-series CIDs. These RAM addresses matter only for the
//      SSM-A8 fallback path (read RAM directly via OEM SSM instead
//      of decoding the wire response) — not implemented as of this
//      commit.
std::span<SignalLayout const> v_narrow_layout() noexcept;
std::span<SignalLayout const> v_wide_layout() noexcept;
std::span<SignalLayout const> ap_layout(DidLayoutVersion fw) noexcept;

// Per-firmware DID payload widths (5 entries for the 5 polled DIDs).
[[nodiscard]] std::span<DidPayload const>
did_payloads(DidLayoutVersion fw) noexcept;

// Per-firmware total payload byte count (sum of the 5 DID widths).
[[nodiscard]] std::size_t total_payload_bytes(DidLayoutVersion fw) noexcept;

// Look up the signal at (did, byte_offset) within a specific firmware
// layout. Returns nullptr if the position is not in that layout.
[[nodiscard]] SignalLayout const *
find_signal(DidLayoutVersion fw, std::uint16_t did,
            std::uint8_t byte_offset) noexcept;

// Backwards-compat overload — defaults to the wide layout (the more
// common recent revision).
[[nodiscard]] SignalLayout const *
find_signal(std::uint16_t did, std::uint8_t byte_offset) noexcept;

// Decode the raw bytes of a single DID payload into the engineering
// value for one signal, applying the wire-side scale + offset.
//
// `did_payload` is the DID's payload region of the ECU response — for
// example, F301's 20 bytes in a wide-layout response. Caller is
// responsible for slicing the right DID out of the multi-frame
// response per the per-layout widths (see did_payloads()).
//
// Returns NaN when `layout.byte_offset` (+ storage width) is out of
// range, or when `scale == 0.0` (Hypothesized entry — no wire-side
// transform is available, callers should evaluate the catalog
// `scaling` expression instead).
//
// CID portability: decode_signal depends only on the byte position +
// storage + wire scale axes. Those were R²-verified against a real
// car sniff; aftermarket testers generally use one response template
// per layout-version across the A-series, so the same decode works
// on any A-series CID running the same layout. The ram_address axis
// (LF79103P catalog) is not consulted.
[[nodiscard]] double
decode_signal(SignalLayout const &layout,
              std::span<std::uint8_t const> did_payload) noexcept;

} // namespace st::ecu::extended_did_datalog

#endif // ST_ECU_EXTENDED_DID_DATALOG_HPP
