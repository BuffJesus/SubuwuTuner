// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::SecurityKeyFn — pluggable seed→key transform for UDS
// SecurityAccess (SID 0x27).
//
// Why a plug-in instead of a built-in algorithm:
//
// Every Subaru SH7058 / Hitachi seed/key algorithm we surveyed (RomRaider,
// ECUFlash, james-portman/subaru-ecu-flashing, LibSSM2, fenugrec/nisprog
// reverse-engineering docs) lives behind a GPL-3 license — and the actual
// algorithm tables are the "secret sauce" that distinguishes them from a
// generic XOR cipher. Lifting those tables into this Apache-2.0 codebase
// would be a license violation; paraphrasing the algorithm shape from a
// GPL-3 source is the same problem in different clothing
// (CLAUDE.md "training-data knowledge is a channel too").
//
// Rather than block hardware progress on that legal puzzle, the Flasher
// accepts a runtime-pluggable function: the caller (or a downstream fork)
// provides a function that takes the ECU's seed bytes and returns the
// matching key bytes. Defaults to `subaru_ssmcan1_key_stub` (which
// returns NotImplemented with a pointer to this file).
//
// The architecture is:
//
//   ECU                          Flasher                   user-supplied fn
//   ───                          ───────                   ────────────────
//   ──[seed]──>  request_seed
//                send_key(key)<─────────────  key = fn(seed)
//   ──[ack]───>
//
// SubuwuTuner ships clean transport + UDS plumbing + integration glue;
// the key-derivation function is the only piece that must come from a
// license-compatible source.

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
