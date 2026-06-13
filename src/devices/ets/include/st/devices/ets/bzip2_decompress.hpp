// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::cipher::bzip2_decompress — layer 4 of the `.ptm`
// cipher chain per spec §13. The output of AES-256-CTR decryption
// (layer 3) is a bzip2-compressed stream; this function inverts it
// to recover the inner `<PrivateData>` XML.
//
// Build-flag gated. When `ST_ETS_HAVE_CIPHER` is undefined every
// function returns `PolicyDenied`. When defined the implementation
// uses bzip2 1.0.8's decompress sources (BSD-like license) lifted
// from upstream — see `THIRD_PARTY_NOTICES.md` and
// `specs/cobb-ap3-tier3-dep-survey.md`. The compress side is
// stubbed out; only decompress is wired.

#ifndef ST_DEVICES_AP3_BZIP2_DECOMPRESS_HPP
#define ST_DEVICES_AP3_BZIP2_DECOMPRESS_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::devices::ets::cipher {

// Decompress a bzip2 stream (must start with the `BZh` magic per
// the bzip2 file format). Returns the inflated plaintext as a
// string — the layer 4 output of a `.ptm` is XML.
[[nodiscard]] Result<std::string>
bzip2_decompress(std::span<std::uint8_t const> compressed);

// Compress a buffer with bzip2 at block-size 4 (per spec §13 — every
// reference .ptm sample uses level 4). Returns the compressed stream
// including the `BZh` magic header so the result is a complete
// standalone bzip2 file. Round-trips byte-identical against the
// canonical layer-4 fixture when called as `bzip2_compress(input)`.
//
// Gated by ST_ETS_HAVE_PTM_REWRITE (which requires
// ST_ENABLE_COBB_AP_PTM_REWRITE=ON at configure time). When undefined
// — either because ST_ETS_HAVE_CIPHER is off OR because the rewrite
// gate hasn't been opted into — this function returns NotImplemented
// with a pointer at docs/34. The cipher OFF build returns
// PolicyDenied like everything else.
[[nodiscard]] Result<std::vector<std::uint8_t>>
bzip2_compress(std::span<std::uint8_t const> plaintext);

} // namespace st::devices::ets::cipher

#endif // ST_DEVICES_AP3_BZIP2_DECOMPRESS_HPP
