// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// COBB CAN Gateway — accessory device, distinct from the AccessPort.
// Sits inline between the OBD-II port and the ECU, exposing a second
// CAN bus the AP can talk to. The Gateway speaks an AES-128 encrypted
// wire protocol; this header carries the key constant the Gateway's
// firmware uses for "AES mode 1" (the only mode RE'd so far).
//
// Provenance: extracted from `libGatewayComms.so:0xa158` trampoline
// (the same 3-instruction pc-relative pattern as the COBB AP SA
// key fetchers RE'd in `findings/re-2026-06-12-pm/
// cobb_sa_keys_extracted.md`). Bytes 16-31 of the loaded blob are
// zero — confirming AES-128 (not AES-256) per RE wave 5 §ε4.
//
// SubuwuTuner does NOT officially support the CAN Gateway accessory.
// This constant lands for posterity so a future implementer porting
// Gateway-protocol support has the key ready; no Gateway-specific
// code paths today.

#ifndef ST_ECU_COBB_GATEWAY_HPP
#define ST_ECU_COBB_GATEWAY_HPP

#include <array>
#include <cstdint>

namespace st::ecu::cobb_gateway {

// AES-128 key for the Gateway's "mode 1" encrypted wire protocol.
// Pinned against the live extraction; any future Gateway firmware
// revision that rotates this key will surface as a decrypt-side
// MAC failure long before the wrong key gets near hardware.
inline constexpr std::array<std::uint8_t, 16> kCobbCanGatewayAesKey = {
    0xf1, 0x03, 0xa6, 0x35, 0x2a, 0x5a, 0x45, 0xd1,
    0x21, 0x7b, 0xa2, 0x46, 0xfd, 0xda, 0xdf, 0x18,
};

} // namespace st::ecu::cobb_gateway

#endif // ST_ECU_COBB_GATEWAY_HPP
