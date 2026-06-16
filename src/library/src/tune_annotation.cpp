// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/tune_annotation.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace st::library {

namespace {

[[nodiscard]] char ascii_tolower(char c) noexcept {
    auto const u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') {
        return static_cast<char>(u + ('a' - 'A'));
    }
    return c;
}

[[nodiscard]] bool icontains(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) {
        return false;
    }
    if (hay.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (ascii_tolower(hay[i + j]) != ascii_tolower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(ascii_tolower(c));
    }
    return out;
}

} // namespace

bool annotation_vendor_matches(std::string_view vendor,
                                std::string_view tuner_display_name) noexcept {
    // A tuner's display_name is like "Fehr/DMann WRK1/2/3 ETune (user's car)";
    // the inventory's vendor field is the short form ("Fehr", "COBB",
    // "NexGen"). Either should imply the other on substring match
    // because the display_name embeds the short form. Symmetric containment.
    return icontains(tuner_display_name, vendor) ||
           icontains(vendor, tuner_display_name);
}

TuneAnnotation annotate_tune(TuneIndexEntry const &entry, Atlas const &atlas,
                              ApCrossRefInputs const &ap) {
    TuneAnnotation out;

    // Match vendor against the atlas tuner clusters. The atlas v1 carries
    // 9 clusters; we report the first cluster whose display_name contains
    // the entry.vendor (or vice-versa for the short-form atlas ids).
    if (!entry.vendor.empty()) {
        for (auto const &tu : atlas.tuners()) {
            if (annotation_vendor_matches(entry.vendor, tu.display_name) ||
                annotation_vendor_matches(entry.vendor, tu.id)) {
                out.tuner_family = tu.id;
                break;
            }
        }
    }

    // Cross-corpus signature flags. These are derived from the family
    // labels rather than computed from patches — that's the shallow
    // version. Deep per-table classification is the cipher-gated path
    // we deliberately don't run here.
    auto const vendor_lc = lower_ascii(entry.vendor);
    if (vendor_lc.find("fehr") != std::string::npos ||
        vendor_lc.find("dmann") != std::string::npos ||
        vendor_lc.find("felix") != std::string::npos) {
        out.fehr_family = true;
    }
    if (vendor_lc.find("cobb") != std::string::npos ||
        vendor_lc.find("nexgen") != std::string::npos) {
        out.cobb_nexgen = true;
    }
    if (vendor_lc.find("ntm") != std::string::npos ||
        icontains(entry.variant, "FA24") ||
        icontains(entry.variant, "basemap")) {
        out.ntm_swap_basemap = true;
    }

    // AP cross-reference. backupcksum compare is case-insensitive on
    // both sides — the AP-side and the index format both standardize
    // on lowercase per the J3 RE, but we don't assume.
    if (ap.backupcksum.has_value() && !ap.backupcksum->empty() &&
        !entry.md5.empty()) {
        auto const ap_md5 = lower_ascii(*ap.backupcksum);
        auto const tune_md5 = lower_ascii(entry.md5);
        if (ap_md5 == tune_md5) {
            out.currently_flashed = true;
        }
    }
    if (!ap.maps_filenames.empty()) {
        auto const slash = entry.path.find_last_of("/\\");
        std::string_view const basename =
            (slash == std::string_view::npos)
                ? std::string_view{entry.path}
                : std::string_view{entry.path}.substr(slash + 1);
        for (auto const &fn : ap.maps_filenames) {
            if (fn == basename) {
                out.on_ap = true;
                break;
            }
        }
    }

    return out;
}

} // namespace st::library
