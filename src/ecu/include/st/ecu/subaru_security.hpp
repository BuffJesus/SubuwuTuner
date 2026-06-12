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
// Implementation provenance: clean-room reverse engineering of the
// plaintext flash images themselves (not from any GPL or closed-source
// implementation). The Gen-A 16-round Feistel + 5-bit S-box constants are
// byte-identical across every A-series ROM sampled. See
// docs/23-security-access.md for the algorithm description and docs/17 §7
// for the distribution-axis reasoning that lets this live in the public
// repo.

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

// Aftermarket-framework L1 variant — for ECUs that have been touched
// by a common aftermarket flasher at any time, regardless of which
// specific tune is currently installed. The aftermarket install
// patches the SA dispatcher's round-iteration patch at flash
// 0xBE911 + 0xBE9C7..0xBE9CE AND substitutes its own L1 round-key
// table at flash 0x074338, so `ssmcan1_key_stub` (which uses the
// FACTORY constants) returns NRC 0x35 (invalidKey) on these ECUs.
//
// Use the tuner-tag region at flash 0x001FFFC0 to discriminate VENDOR
// at runtime: 4-byte ASCII vendor tag, factory all-FF for stock.
//
// Validated by computing the SA key for several independently captured
// (seed, key) pairs (random-match probability 2^-32 each) and
// confirming byte-exact matches, plus a live-hardware confirmation:
// reading the pairing token at flash 0x001FFFB0 over UDS RMBA after
// the SA preamble produced exactly the expected bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_aftermarket(std::span<std::uint8_t const> seed);

// Aftermarket-framework L3 variant — Gen-A.2 SecurityAccess
// sub-function `27 03` (RequestSeed L3) / `27 04` (SendKey L3) for
// any aftermarket-installed tune on this CID family. The same
// dispatcher reversed-iteration patch that flips L1's direction also
// flips L3 (the patched loop is shared across all SA levels), so this
// function's structure mirrors `ssmcan1_l1_aftermarket` — forward
// Feistel + final wordswap — differing only by:
//   * Round-key table (`kSaTableL35Aftermarket` at flash 0x074358).
//   * The factory dispatcher inserts a per-level byte permutation on
//     the wire seed before the Feistel and on the wire key after,
//     which this function applies internally.
//
// Validated against an independently captured L3 pair
// (seed=0x4ADFFE07 → key=0x24243A06). Cross-checked across multiple
// aftermarket installs: byte-identical L35 constants. Joint match
// probability 2^-32 from offline validation; one live-hardware test
// shot would push to 2^-64 (queued in the hardware-gated private
// test tree).
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l3_aftermarket(std::span<std::uint8_t const> seed);

// COBB-active L1 — for ECUs in the COBB-installed-tune state. The
// round-key table differs from `ssmcan1_l1_aftermarket` (which targets
// the broader Fehr-active framework) but the SA-dispatcher reversed-
// iteration patch + forward-Feistel direction is shared.
//
// Two flavors per RE5b extraction (findings/re-2026-06-12-pm/
// cobb_sa_keys_extracted.md): `_cobb_flash` is the canonical COBB
// install variant ("COBB Flash" mode); `_cobb_maf_sd` is the MAF-
// based Speed-Density variant used when COBB targets an ECU without
// MAP-sensor data configured. Both algorithms are byte-identical
// except for the round-key table; the choice is driven by which
// install state the user's ECU is in (CID + tuner-tag region byte
// content disambiguate).
//
// Cross-validation: SSM-IV factory keys at the same trampoline
// family decode byte-identical to `kFeistelRoundKeysL35` (the
// implementer's independently-recovered factory table), so the
// extraction methodology is verified. Live bench-rig validation
// against an actual COBB-installed ECU pending (docs/28 Phase 5.5).
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_flash(std::span<std::uint8_t const> seed);
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_maf_sd(std::span<std::uint8_t const> seed);

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
