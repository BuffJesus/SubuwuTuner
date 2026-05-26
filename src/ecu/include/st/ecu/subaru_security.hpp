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

// Fehr-active L1 variant — for ECUs running a Fehr Tuning e-tune
// (CAL ID `LF79101P` on 2017 USDM WRX hardware). Fehr's tune patches
// the SA dispatcher's round-iteration AND substitutes its own L1
// round-key table at flash 0x074338, so neither `ssmcan1_key_stub`
// nor `ssmcan1_l1_cobb_tuned` produce a valid key for these ECUs —
// both return NRC 0x35 (invalidKey).
//
// Validated against the captured `cobb-uninstall-3 L1` pair
// (seed=0xB9A65C23 → key=0x13EF9295) AND against the user's live
// 2017 LF79101P on 2026-05-26 by reading the pairing token at
// 0x001FFFB0 via UDS RMBA + the SA preamble through this key
// function (`Flasher::set_security_key_fn(&ssmcan1_l1_fehr_active)`).
//
// Out-of-tree forks that need to support other e-tunes should copy
// the same pattern: extract the tune's 16 × u16 BE round-key table
// from offset 0x074338 of the decrypted plaintext and register a
// new function through `Flasher::set_security_key_fn`. The S-box at
// 0x074378 is invariant across all observed e-tunes.
//
// See `docs/23-security-access.md` § "Algorithm structure recovered"
// for the broader picture and `docs/17-data-distribution-policy.md`
// §7 for the in-tree-vs-pluggable posture (the Fehr variant's
// constants are shipped in-tree, same precedent as `kSaTableL1`).
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_l1_fehr_active(std::span<std::uint8_t const> seed);

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
