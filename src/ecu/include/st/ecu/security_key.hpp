// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::SecurityKeyFn — pluggable seed→key transform for UDS
// SecurityAccess (SID 0x27).
//
// The Flasher accepts a runtime-pluggable function: the caller (or a
// downstream fork) provides a function that takes the ECU's seed bytes
// and returns the matching key bytes. Defaults to
// `subaru_ssmcan1_key_stub`, which (as of 2026-05-24) is the real Gen-A.2
// L1 implementation — algorithm + constants recovered from the plaintext
// flash images of every available A-series CID via the analyst-mode RE
// workflow in docs/15. Not derived from RomRaider / ECUFlash / nisprog /
// Atlas / any commercial implementation. See docs/23-security-access.md
// for the algorithm description and docs/17 §7 for the distribution-axis
// reasoning. The plug-in seam is still the right architectural shape —
// future Gen-B (AES) work and any user-supplied Gen-A overrides slot in
// through the same setter.
//
// The architecture is:
//
//   ECU                          Flasher                   user-supplied fn
//   ───                          ───────                   ────────────────
//   ──[seed]──>  request_seed
//                send_key(key)<─────────────  key = fn(seed)
//   ──[ack]───>
//
// Downstream forks may still replace the default with their own clean-room
// derivation, a vendor SDK they've licensed, or a Gen-B AES implementation
// they've integrated — `Flasher::set_security_key_fn` is the seam.

#ifndef ST_ECU_SECURITY_KEY_HPP
#define ST_ECU_SECURITY_KEY_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace st::ecu {

// Pluggable seed → key transform. Returns failure() if the algorithm
// can't compute a key for this seed (wrong length, unsupported variant,
// stub not configured, etc.). Otherwise returns the key bytes the Flasher
// will send to the ECU via UDS SecurityAccess sub-function 0x02.
//
// Contract:
// * Pure function — must not depend on globals, threads, or I/O.
// * Deterministic — same seed must always produce the same key (the ECU
//   side is deterministic too; non-deterministic keys are always rejected
//   on the first try).
// * Bounded runtime — ~ms at most. The ECU has a session-level timeout
//   (Subaru's is typically 5s) so a key function blocking on I/O risks
//   missing the window.
using SecurityKeyFn =
    std::function<Result<std::vector<std::uint8_t>>(std::span<std::uint8_t const> seed)>;

} // namespace st::ecu

#endif // ST_ECU_SECURITY_KEY_HPP
