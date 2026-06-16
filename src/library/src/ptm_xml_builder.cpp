// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/ptm_xml_builder.hpp"

#include "st/core/error.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace st::library {

namespace {

constexpr std::size_t kMaxMetadataFieldLength = 128;

[[nodiscard]] bool is_allowed_metadata_char(char c) noexcept {
    auto const u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') {
        return true;
    }
    if (u >= 'a' && u <= 'z') {
        return true;
    }
    if (u >= '0' && u <= '9') {
        return true;
    }
    return c == '.' || c == '_' || c == '-' || c == ' ';
}

std::string xml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out.append("&amp;"); break;
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        case '"': out.append("&quot;"); break;
        case '\'': out.append("&apos;"); break;
        default: out.push_back(c);
        }
    }
    return out;
}

} // namespace

Status validate_ptm_metadata_field(std::string_view field_name,
                                    std::string_view value) {
    if (value.size() > kMaxMetadataFieldLength) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ptm metadata field '"} +
                           std::string{field_name} +
                           "' exceeds " +
                           std::to_string(kMaxMetadataFieldLength) +
                           "-char cap (" + std::to_string(value.size()) +
                           " chars given)");
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!is_allowed_metadata_char(value[i])) {
            auto const c = static_cast<unsigned char>(value[i]);
            char buf[16];
            (void)std::snprintf(buf, sizeof(buf), "0x%02X", c);
            return failure(
                ErrorCode::InvalidArgument,
                std::string{"ptm metadata field '"} + std::string{field_name} +
                    "' contains disallowed character " + buf + " at offset " +
                    std::to_string(i) +
                    " (allowed: A-Z a-z 0-9 . _ - space; rejects shell + XML "
                    "metachars to harden against downstream parsers)");
        }
    }
    return ok();
}

std::string
build_ptm_inner_xml(std::string_view vendor_id, std::string_view vehicle_id,
                    std::uint32_t lock_mask, std::string_view rom_sum,
                    std::string_view save_date,
                    std::vector<PtmExportPatch> const &patches) {
    std::string xml = "<PrivateData>";
    xml += "<vendorID>" + xml_escape(vendor_id) + "</vendorID>";
    xml += "<vehicleID>" + xml_escape(vehicle_id) + "</vehicleID>";
    xml += "<lock mask=\"" + std::to_string(lock_mask) + "\" />";
    xml += "<romSum value=\"" + xml_escape(rom_sum) + "\" />";
    xml += "<saveDateTime value=\"" + xml_escape(save_date) + "\" />";
    xml += "<patches>";
    for (auto const &p : patches) {
        xml += "<patch romOffset=\"" + std::to_string(p.rom_offset) +
               "\" ramOffset=\"" + std::to_string(p.ram_offset) +
               "\" length=\"" + std::to_string(p.length) + "\">" +
               p.bytes_b64 + "</patch>";
    }
    xml += "</patches></PrivateData>";
    return xml;
}

std::string
build_ptm_outer_xml(std::string_view vendor_id, std::string_view vehicle_id,
                    std::uint32_t lock_mask, std::string_view rom_sum,
                    std::string_view save_date) {
    std::string xml = "<PtmOuter>";
    xml += "<vendorID>" + xml_escape(vendor_id) + "</vendorID>";
    xml += "<vehicleID>" + xml_escape(vehicle_id) + "</vehicleID>";
    xml += "<lock mask=\"" + std::to_string(lock_mask) + "\" />";
    xml += "<romSum value=\"" + xml_escape(rom_sum) + "\" />";
    xml += "<saveDateTime value=\"" + xml_escape(save_date) + "\" />";
    xml += "</PtmOuter>";
    return xml;
}

} // namespace st::library
