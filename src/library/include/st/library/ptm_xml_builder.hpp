// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::build_ptm_inner_xml / build_ptm_outer_xml — shared
// XML construction for the two halves of a .ptm cipher round-trip.
// Used by both `subuwutuner-cli ptm export` and the GUI Export modal.
//
// The inner `<PrivateData>` XML is what gets bzip2-compressed +
// AES-encrypted into the layer-3 ciphertext. The outer envelope XML
// is what gets XTEA-encrypted into the final .ptm; encrypt_ptm
// injects `<encData>BASE64</encData>` into the outer before the last
// closing tag, so any outer that includes a valid root element works.

#ifndef ST_LIBRARY_PTM_XML_BUILDER_HPP
#define ST_LIBRARY_PTM_XML_BUILDER_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace st::library {

// Defensive validator for `.ptm` metadata fields (vendorId / vehicleId /
// romSum / saveDateTime). XML-escaping in `build_ptm_*_xml` keeps the
// output valid as XML, but the consuming AP firmware's BusyBox shell
// scripts extract these fields via `sed` + `eval` (see analyst writeup
// at `findings/tuning-knowledge-2026-06-13/cmd22-path-re/shell_injection_sinks.md`)
// — XML safety is necessary but not sufficient. We additionally
// restrict the character set to alphanumeric + a small safe-punctuation
// set so accidental shell metachars in user-supplied metadata can't
// chain through to the AP's parser.
//
// Restricted character set: ASCII [A-Z a-z 0-9 . _ - space]. Length
// capped at 128 chars. Anything else returns InvalidArgument with a
// description naming the bad character.
//
// This is defense-in-depth — the COBB firmware should sanitize its own
// inputs too, and we'd report the underlying issue to COBB through a
// responsible-disclosure path. Belt + suspenders.
[[nodiscard]] Status validate_ptm_metadata_field(std::string_view field_name,
                                                  std::string_view value);

// One patch as it appears in the inner XML. bytes_b64 is the base64-
// encoded replacement-bytes string from the project's ptm_patches.toml
// (which is itself the original .ptm's per-patch payload, preserved
// raw for round-trip).
struct PtmExportPatch {
    std::uint32_t rom_offset{0};
    std::int32_t ram_offset{0};
    std::uint32_t length{0};
    std::string bytes_b64;
};

[[nodiscard]] std::string
build_ptm_inner_xml(std::string_view vendor_id, std::string_view vehicle_id,
                    std::uint32_t lock_mask, std::string_view rom_sum,
                    std::string_view save_date,
                    std::vector<PtmExportPatch> const &patches);

[[nodiscard]] std::string
build_ptm_outer_xml(std::string_view vendor_id, std::string_view vehicle_id,
                    std::uint32_t lock_mask, std::string_view rom_sum,
                    std::string_view save_date);

} // namespace st::library

#endif // ST_LIBRARY_PTM_XML_BUILDER_HPP
