// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::PatchEntry / PtmMetadata / DecodedPtm — structured
// representation of a decrypted .ptm PrivateData document.
//
// The decoder takes the inner <PrivateData> XML returned by
// `st::devices::ap3::cipher::decrypt_ptm` and parses it into the
// structures below. Consumers (ptm inspect / ptm list-patches /
// ptm diff / ptm import) build their feature surfaces on top.
//
// Spec: specs/cobb-ap3-patch-decoder-spec.md (clean-room algorithm).

#ifndef ST_LIBRARY_PATCH_DECODER_HPP
#define ST_LIBRARY_PATCH_DECODER_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::library {

struct PatchEntry {
    std::uint32_t rom_offset{0};
    std::int32_t ram_offset{0};
    std::uint32_t length{0};
    std::vector<std::uint8_t> bytes;
};

struct PtmMetadata {
    std::string vendor_id;
    std::string vehicle_id;
    std::uint32_t lock_mask{0};
    std::string rom_sum;
    std::string save_date_time;
};

struct DecodedPtm {
    PtmMetadata metadata;
    std::vector<PatchEntry> patches;
};

// Reasonable ceiling for a single <patch> length attribute. Real
// tunes have per-patch sizes from 1 byte to ~few KB. 1 MB is well
// above any legitimate edit; values past this are almost certainly
// malformed input, and accepting them would let an attacker request
// huge allocations from a parser caller.
inline constexpr std::uint32_t kMaxPatchLengthBytes = 1U << 20;

// Decode an inner <PrivateData> XML document (as produced by
// `decrypt_ptm`) into a structured patch list. Strict: any malformed
// input — unparseable XML, missing required field, non-integer
// attribute, base64 mismatch, declared-length vs decoded-length
// mismatch — returns `ParseError` with a descriptive message.
//
// Whitespace inside element text content is trimmed before base64
// decode. The decoder does NOT enforce ROM-size bounds or overlap
// invariants — those belong with the per-target classifier; this
// stage's job is faithful translation only.
[[nodiscard]] Result<DecodedPtm> decode_ptm_xml(std::string_view xml);

} // namespace st::library

#endif // ST_LIBRARY_PATCH_DECODER_HPP
