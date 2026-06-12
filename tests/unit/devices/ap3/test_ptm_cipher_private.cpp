// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Private-fixture regression test for the full real-tune `.ptm` cipher
// chain. Locked down 2026-06-12 PM after live-validating the AES custom
// CTR rewrite (commit 97bd2dd) against the user's married AccessPort
// pulling /maps/Stage1 91 v401.ptm.
//
// Why a separate file: the synthetic fixtures in test_ptm_cipher.cpp
// exercise each layer with hand-crafted vectors. This file exercises
// the FULL 4-layer chain against a real COBB-emitted .ptm (which lives
// off-tree under fixtures/private/ap3_ptm_samples/ per docs/17 — Path
// B distribution posture means owned-content .ptm samples never enter
// the public repo).
//
// Catch2 [.private] hidden tag — the suite self-skips when the file
// isn't staged. Public CI sees zero failures; analyst-side or
// developer-with-local-fixture invocations run it via:
//
//   st_unit_tests "[.private]"

#include "st/devices/ap3/ptm_cipher.hpp"
#include "st/library/patch_decoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_all(std::filesystem::path const &p) {
    std::ifstream in{p, std::ios::binary | std::ios::ate};
    if (!in) {
        return {};
    }
    auto const sz = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char *>(bytes.data()), sz);
    return bytes;
}

std::filesystem::path private_ptm_path() {
    // CWD-relative is acceptable here: the [.private] tag means we self-
    // skip when the file isn't there, and the analyst-side `ctest` /
    // `st_unit_tests` invocation runs from project root by convention.
    return std::filesystem::path{"fixtures/private/ap3_ptm_samples/"} /
           "cobb_stage1_91_v401_lf79103p.ptm";
}

} // namespace

#if defined(ST_AP3_HAVE_CIPHER)

TEST_CASE("ap3 cipher private: real COBB Stage1 .ptm decrypts and decodes",
          "[.private][devices][ap3][cipher]") {
    auto const path = private_ptm_path();
    if (!std::filesystem::exists(path)) {
        SKIP("private .ptm fixture not staged (see "
             "fixtures/private/ap3_ptm_samples/ + commit 97bd2dd)");
    }
    auto const ptm_bytes = read_all(path);
    REQUIRE(ptm_bytes.size() == 58005); // pinned at the captured file's size

    // Full 4-layer decrypt — XTEA-CBC outer + base64 + custom-CTR
    // AES-256 + bzip2 inflate. Any layer regression breaks this.
    auto contents = st::devices::ap3::cipher::decrypt_ptm(ptm_bytes);
    REQUIRE(contents.has_value());
    auto const &xml = contents->private_data_xml;
    REQUIRE_FALSE(xml.empty());
    // The COBB Stage1 91 v401 PrivateData root + identity fields. If
    // any of these change, the cipher or the file changed.
    REQUIRE(xml.starts_with("<PrivateData>"));
    REQUIRE(xml.find("<vendorID>COBB</vendorID>") != std::string::npos);
    REQUIRE(xml.find("<vehicleID>SUBA_US_WRXM_CF_17_E</vehicleID>") !=
            std::string::npos);
    REQUIRE(xml.find("<romSum value=\"812800817\" />") != std::string::npos);
    REQUIRE(xml.ends_with("</PrivateData>"));

    // Patch decoder runs cleanly on the real PrivateData. 4244 is the
    // exact captured patch count; if the decoder drops or duplicates
    // entries the count changes and this trips.
    auto decoded = st::library::decode_ptm_xml(xml);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->patches.size() == 4244);
    REQUIRE(decoded->metadata.vendor_id == "COBB");
    REQUIRE(decoded->metadata.vehicle_id == "SUBA_US_WRXM_CF_17_E");
    REQUIRE(decoded->metadata.lock_mask == 0);
    REQUIRE(decoded->metadata.rom_sum == "812800817");

    // Duplicate-rom_offset invariant — this Stage1 tune has 78
    // rom_offsets with two <patch> entries each (clear-then-set byte
    // pairs in the COBB cal-write strategy). The decoder MUST preserve
    // all of them; if it silently dedupes, ptm import would lose the
    // "set" half of each pair.
    std::size_t duplicate_offsets = 0;
    std::map<std::uint32_t, std::size_t> seen;
    for (auto const &p : decoded->patches) {
        if (++seen[p.rom_offset] == 2) {
            ++duplicate_offsets;
        }
    }
    REQUIRE(duplicate_offsets == 78);
}

#endif // ST_AP3_HAVE_CIPHER
