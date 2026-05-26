// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::subaru — Subaru-specific seed/key functions for UDS SecurityAccess.
//
// Each ECU generation has its own SecurityAccess algorithm:
//
// | Era          | Silicon | Algorithm name | Seed bytes | In-tree status         |
// |--------------|---------|----------------|------------|------------------------|
// | pre-2008     | SH7055  | "SSMK1"        | 4          | NotImplemented stub    |
// | 2008-~2017   | SH7058  | "SSMCAN1"      | 4          | L1 implemented in-tree |
// | 2018+        | RH850   | "CY1" (AES)    | 16         | NotImplemented stub    |
//
// Implementation provenance: analyst-mode reverse engineering of the
// plaintext flash images themselves (not from any GPL or closed-source
// implementation). The Gen-A 16-round Feistel + 5-bit S-box constants are
// byte-identical across every A-series ROM sampled. See
// docs/23-security-access.md § "Algorithm structure recovered (2026-05-24)"
// for the algorithm description and docs/17 §7 for the distribution-axis
// reasoning that lets this live in the public repo.

#ifndef ST_ECU_SUBARU_SECURITY_HPP
#define ST_ECU_SUBARU_SECURITY_HPP

#include "st/core/result.hpp"
#include "st/ecu/security_key.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::subaru {

// SSMCAN1 — Subaru CAN-era SecurityAccess (SH7058 ECUs, ~2008-2017,
// including the 2017 VA WRX reference hardware).
//
// Input:  4-byte seed from the `67 01 ...` requestSeed response,
//         positions [2..5] of the UDS frame as they appear on the wire.
// Output: 4-byte key to send via `27 02 ...` sendKey, again positions
//         [2..5] on the wire.
//
// Implemented in-tree at level 1 (sub-functions `27 01` / `27 02`) — the
// bootloader-unlock level used for ReadMemoryByAddress / RequestDownload
// flash operations. Levels 3 and 5 (deeper diagnostic / kernel routines,
// Gen-A.2 only) use a different per-level byte shuffle and a separate
// round-key table; they aren't on the critical path for a stock ROM dump
// and are deferred.
//
// Returns failure(InvalidArgument) if `seed.size() != 4`. Always succeeds
// for any 4-byte input; the ECU is the one that decides whether the key
// is correct.
//
// Historical note on the `_stub` suffix: this function was a
// NotImplemented stub through 2026-05-23. The implementation landed
// 2026-05-24; the symbol name is preserved so the `Flasher` default-
// security-key-fn callsite doesn't need to chase a rename.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_key_stub(std::span<std::uint8_t const> seed);

// DEPRECATED ALIAS (2026-05-24 PM). Originally introduced when we
// thought COBB-tuned ECUs ran a different L1 algorithm via a pointer
// swap. The analyst-side bootloader disassembly later showed COBB
// modifies the round-key TABLE BYTES in place at flash 0x074338, not
// the dispatch code — so `ssmcan1_key_stub` (now using the renamed
// `kSaTableL1` constants which ARE the bytes COBB writes) and this
// function are bit-identical. Routed through `ssmcan1_key_stub`.
//
// Kept for back-compat with the `--cobb-tuned` CLI flag and any out-
// of-tree callers; will be removed in a future cleanup pass.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_tuned(std::span<std::uint8_t const> seed);

// COBB-AccessPort-framework L1 variant — for ECUs that have been
// touched by the COBB AccessPort at any time, regardless of which
// specific tune (COBB OTS Stage 0/1/2, Fehr/DMann e-tune,
// NexGen-style etc.) is currently installed.  The COBB-AP install
// patches the SA dispatcher's round-iteration patch at flash
// 0xBE911 + 0xBE9C7..0xBE9CE AND substitutes its own L1 round-key
// table at flash 0x074338, so neither `ssmcan1_key_stub` nor
// `ssmcan1_l1_cobb_tuned` (which still use the FACTORY constants)
// produce a valid key for these ECUs — both return NRC 0x35
// (invalidKey).
//
// CROSS-VENDOR FINDING (2026-05-26 PM).  This path was originally
// introduced as `ssmcan1_l1_fehr_active` under the assumption it was
// Fehr-specific.  Install-sniff captures of COBB Stage 0/1/2 then
// showed the SAME L1 keys + SAME loop-reversal patch — they belong to
// the AP framework, not to any one tuner.  Renamed accordingly;
// `ssmcan1_l1_fehr_active` is now a pass-through alias.
//
// Use the tuner-tag region at flash 0x001FFFC0 to discriminate VENDOR
// at runtime: ASCII `"COBB"` for COBB OTS stages, `"W585"` for the
// Fehr/DMann e-tune, factory all-FF for stock.
//
// Validated against the captured `cobb-uninstall-3 L1` pair
// (seed=0xB9A65C23 → key=0x13EF9295) AND against the user's live
// 2017 LF79101P on 2026-05-26 by reading the pairing token at
// 0x001FFFB0 via UDS RMBA + the SA preamble through this key function.
// Cross-checked against COBB Stage 0 install bytes 2026-05-26 PM:
// byte-identical L1 constants.  See
// `Findings/calibration-deltas/install_roms_comparison.md` §"SA
// constants" for the data.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_ap(std::span<std::uint8_t const> seed);

// RETAINED ALIAS (2026-05-26 PM).  Pass-through to
// `ssmcan1_l1_cobb_ap` — same constants, same algorithm.  Kept under
// the original name so the CLI's `--sa-variant fehr-active[-l1]` flag,
// the gitignored hardware-validation tests in `tests/private/`, and
// any out-of-tree callers continue to work without source churn.  New
// in-tree code should prefer `ssmcan1_l1_cobb_ap`.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_fehr_active(std::span<std::uint8_t const> seed);

// COBB-AccessPort-framework L3 variant — Gen-A.2 SecurityAccess
// sub-function `27 03` (RequestSeed L3) / `27 04` (SendKey L3) for any
// COBB-AP-installed tune on this CID family.  The same dispatcher
// reversed-iteration patch that flips L1's direction also flips L3
// (the patched loop is shared across all SA levels), so this
// function's structure mirrors `ssmcan1_l1_cobb_ap` — forward Feistel
// + final wordswap — differing only by:
//   * Round-key table (`kSaTableL35CobbAp` at flash 0x074358).
//   * The factory dispatcher inserts a per-level byte permutation on
//     the wire seed before the Feistel and on the wire key after,
//     which this function applies internally.
//
// Validated against the captured Fehr-active L3 pair (seed=0x4ADFFE07
// → key=0x24243A06).  Cross-checked against COBB Stage 0 install
// bytes 2026-05-26 PM: byte-identical L35 constants.  Joint match
// probability 2^-32 from offline validation; one live-hardware test
// shot would push to 2^-64 (queued in `tests/private/` under tag
// `[.fehr-live][l3-token]`).
//
// See `Findings/calibration-deltas/l3_cipher_recovered.md` for the full
// derivation and `Findings/calibration-deltas/install_roms_comparison.md`
// for the COBB-vs-Fehr cross-vendor identity.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l3_cobb_ap(std::span<std::uint8_t const> seed);

// RETAINED ALIAS (2026-05-26 PM).  Pass-through to
// `ssmcan1_l3_cobb_ap` — same constants, same algorithm.  Kept under
// the original name so the CLI's `--sa-variant fehr-active-l3` flag
// and gitignored private hardware tests continue to work without
// source churn.  New in-tree code should prefer `ssmcan1_l3_cobb_ap`.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l3_fehr_active(std::span<std::uint8_t const> seed);

// SSMK1 — pre-2008 K-Line SecurityAccess. Algorithm not yet recovered
// (no K-Line capture rig in hand). Returns failure(NotImplemented) on
// every input.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmk1_key_stub(std::span<std::uint8_t const> seed);

// 2018+ AES-128 ECB SecurityAccess for RH850-silicon ECUs (Gen-B,
// `LHBxxx` CIDs). The algorithm + three universal master keys are known
// analyst-side; in-tree implementation deferred pending an AES primitive
// choice. Returns failure(NotImplemented) on every input.
[[nodiscard]] Result<std::vector<std::uint8_t>>
cy1_aes_key_stub(std::span<std::uint8_t const> seed);

// -----------------------------------------------------------------------------
// internal:: — exposed for unit tests; not part of the public API.
//
// These wrap the file-local implementation primitives so test_subaru_security
// can exercise the F function, the per-direction Feistel, and the level-1
// round-key constants without dragging the implementation back into a public
// header. Subject to change without notice; do not call from production code.
// -----------------------------------------------------------------------------
namespace internal {

std::uint16_t test_only_F(std::uint16_t x, std::uint16_t k) noexcept;

std::uint32_t
test_only_feistel_forward(std::uint32_t state,
                          std::span<std::uint16_t const, 16> rk) noexcept;

std::uint32_t
test_only_feistel_inverse(std::uint32_t state,
                          std::span<std::uint16_t const, 16> rk) noexcept;

std::span<std::uint16_t const, 16> test_only_round_keys_l1() noexcept;

} // namespace internal

} // namespace st::ecu::subaru

#endif // ST_ECU_SUBARU_SECURITY_HPP
