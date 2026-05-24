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
