// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ecu::subaru — Subaru-specific seed/key stub functions.
//
// Each ECU generation has its own SecurityAccess algorithm:
//
// | Era          | Silicon | Algorithm name | Seed bytes | Notes                    |
// |--------------|---------|----------------|------------|--------------------------|
// | pre-2008     | SH7055  | "SSMK1"        | 4          | K-Line only              |
// | 2008–~2017   | SH7058  | "SSMCAN1"      | 4          | CAN-era, our VA WRX      |
// | 2018+        | RH850   | "CY1" (AES)    | 16         | AES-CBC; UnlockECU has it|
//
// All three stubs in this header return failure(NotImplemented). Hooking
// in a real implementation is a `Flasher::set_security_key_fn(...)` call
// at runtime — see security_key.hpp for the rationale.

#ifndef ST_ECU_SUBARU_SECURITY_HPP
#define ST_ECU_SUBARU_SECURITY_HPP

#include "st/core/result.hpp"
#include "st/ecu/security_key.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::subaru {

// SSMCAN1 — Subaru CAN-era SecurityAccess (SH7058 ECUs, ~2008–2017).
// Used by the 2017 VA WRX our reference hardware targets.
//
// Input:  4-byte seed from `27 01` requestSeed response.
// Output: 4-byte key to send via `27 02` sendKey.
//
// **NOT IMPLEMENTED.** The algorithm is publicly documented (fenugrec/
// nisprog reverse-engineering notes) but every plain-code implementation
// is GPL-3 licensed and would contaminate our Apache 2.0 codebase. To
// enable Read ROM on a security-locked Subaru ECU:
//
// 1. Source the SSMCAN1 algorithm + tables from a license-compatible
//    reference (or write a clean-room implementation from first
//    principles + your own ECU traces).
// 2. Wrap it in a `st::ecu::SecurityKeyFn` lambda.
// 3. Pass to `Flasher::set_security_key_fn` before calling
//    `read_full_rom(authenticate=true)`.
//
// Returns failure(NotImplemented, ...) on every input.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmcan1_key_stub(std::span<std::uint8_t const> seed);

// SSMK1 — pre-2008 K-Line SecurityAccess. Same legal status as SSMCAN1.
// Returns failure(NotImplemented, ...) on every input.
[[nodiscard]] Result<std::vector<std::uint8_t>>
ssmk1_key_stub(std::span<std::uint8_t const> seed);

// 2018+ AES-based SecurityAccess for RH850-silicon ECUs. The algorithm
// IS available under a license-compatible MIT (jglim/UnlockECU's
// SubaruSecurityAccess2018CY1.cs) so this stub could be replaced with a
// real implementation. Currently NotImplemented to keep the surface
// consistent and to defer the AES dependency choice.
[[nodiscard]] Result<std::vector<std::uint8_t>>
cy1_aes_key_stub(std::span<std::uint8_t const> seed);

} // namespace st::ecu::subaru

#endif // ST_ECU_SUBARU_SECURITY_HPP
