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

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace st::library {

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
