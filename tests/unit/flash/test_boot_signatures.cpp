// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash/boot_signatures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace fl = st::flash;

namespace {

// Build a synthetic 2 MB image with the three boot signatures
// pre-populated correctly, so individual tests can mutate one
// signature at a time to provoke a specific failure.
std::vector<std::uint8_t> good_image() {
    std::vector<std::uint8_t> img(0x200000u, 0u);
    // Sig A at 0x6000 = 0x5555
    img[fl::kBootSigAOffset + 0] = 0x55;
    img[fl::kBootSigAOffset + 1] = 0x55;
    // Sig B at 0x1FFFF2 = 0xAAAA
    img[fl::kBootSigBOffset + 0] = 0xAA;
    img[fl::kBootSigBOffset + 1] = 0xAA;
    // Pair: both 0x6C and 0x6010 read the same value (0x0504 per
    // the analyst's measurement of the user's live ROM)
    img[fl::kBootPairLowOffset + 0] = 0x05;
    img[fl::kBootPairLowOffset + 1] = 0x04;
    img[fl::kBootPairHighOffset + 0] = 0x05;
    img[fl::kBootPairHighOffset + 1] = 0x04;
    return img;
}

} // namespace

TEST_CASE("verify_boot_signatures_sh2a_2mb accepts a well-formed image",
          "[flash][boot_signatures]") {
    auto img = good_image();
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE(r.ok());
    REQUIRE(r.failure == fl::BootSignatureFailure::Ok);
    REQUIRE(r.sig_a_actual == fl::kBootSigAExpected);
    REQUIRE(r.sig_b_actual == fl::kBootSigBExpected);
    REQUIRE(r.pair_at_6c == r.pair_at_6010);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb rejects an image too small to hold the signatures",
          "[flash][boot_signatures]") {
    // Any size below kBootSigBOffset+2 should fail with TooSmall
    // before touching the (out-of-bounds) signature bytes.
    std::vector<std::uint8_t> img(0x100000u, 0xFF);
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.failure == fl::BootSignatureFailure::TooSmall);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb flags SigA mismatch first",
          "[flash][boot_signatures]") {
    auto img = good_image();
    img[fl::kBootSigAOffset] = 0x12;
    img[fl::kBootSigAOffset + 1] = 0x34;
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.failure == fl::BootSignatureFailure::SigA);
    REQUIRE(r.sig_a_actual == 0x1234u);
    // Other fields are still populated for diagnostics.
    REQUIRE(r.sig_b_actual == fl::kBootSigBExpected);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb flags SigB mismatch",
          "[flash][boot_signatures]") {
    auto img = good_image();
    img[fl::kBootSigBOffset] = 0x55;
    img[fl::kBootSigBOffset + 1] = 0x55;
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.failure == fl::BootSignatureFailure::SigB);
    REQUIRE(r.sig_b_actual == 0x5555u);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb flags PairMismatch",
          "[flash][boot_signatures]") {
    auto img = good_image();
    // Diverge the high pair so it no longer matches the low pair.
    img[fl::kBootPairHighOffset] = 0xDE;
    img[fl::kBootPairHighOffset + 1] = 0xAD;
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.failure == fl::BootSignatureFailure::PairMismatch);
    REQUIRE(r.pair_at_6c == 0x0504u);
    REQUIRE(r.pair_at_6010 == 0xDEADu);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb fails in offset order — SigA before SigB",
          "[flash][boot_signatures]") {
    // Break both SigA and SigB; the report should flag SigA (lower
    // offset wins) so the user sees the first thing to fix.
    auto img = good_image();
    img[fl::kBootSigAOffset] = 0x00;
    img[fl::kBootSigAOffset + 1] = 0x00;
    img[fl::kBootSigBOffset] = 0x00;
    img[fl::kBootSigBOffset + 1] = 0x00;
    auto const r = fl::verify_boot_signatures_sh2a_2mb(img);
    REQUIRE(r.failure == fl::BootSignatureFailure::SigA);
}

TEST_CASE("boot_signature_failure_name covers every enumerator",
          "[flash][boot_signatures]") {
    REQUIRE(fl::boot_signature_failure_name(fl::BootSignatureFailure::Ok) == "ok");
    REQUIRE(fl::boot_signature_failure_name(fl::BootSignatureFailure::TooSmall) ==
            "too_small");
    REQUIRE(fl::boot_signature_failure_name(fl::BootSignatureFailure::SigA) ==
            "sig_a_mismatch");
    REQUIRE(fl::boot_signature_failure_name(fl::BootSignatureFailure::SigB) ==
            "sig_b_mismatch");
    REQUIRE(fl::boot_signature_failure_name(fl::BootSignatureFailure::PairMismatch) ==
            "pair_mismatch");
}

// Opt-in end-to-end checks against locally-staged reference ROMs.
// Both paths are configurable via environment variables so the
// public source tree carries no hardcoded user-specific filenames;
// the tests skip cleanly when the env var is unset or the file
// is absent.
//
//   SUBUWU_TEST_LIVE_ROM    — full path to an aftermarket-installed
//                             2 MB SH-2A reference ROM
//   SUBUWU_TEST_STOCK_ROM   — full path to a factory-virgin 2 MB
//                             SH-2A reference ROM

namespace {

std::vector<std::uint8_t> load_2mb_rom(std::filesystem::path const &p) {
    std::ifstream in{p, std::ios::binary};
    REQUIRE(in.is_open());
    std::vector<std::uint8_t> bytes{(std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() == 0x200000u);
    return bytes;
}

std::optional<std::filesystem::path> env_rom_path(char const *var) {
    char const *raw = std::getenv(var);
    if (raw == nullptr || raw[0] == '\0') return std::nullopt;
    std::filesystem::path p{raw};
    if (!std::filesystem::exists(p)) return std::nullopt;
    return p;
}

} // namespace

TEST_CASE("verify_boot_signatures_sh2a_2mb passes against an aftermarket-installed reference ROM",
          "[flash][boot_signatures][rom]") {
    auto const path = env_rom_path("SUBUWU_TEST_LIVE_ROM");
    if (!path) {
        WARN("SUBUWU_TEST_LIVE_ROM unset or file absent — skipping.");
        return;
    }
    auto const rom = load_2mb_rom(*path);
    auto const r = fl::verify_boot_signatures_sh2a_2mb(rom);
    REQUIRE(r.ok());
    // Spot-check the paired-vector value documented for the
    // reference image in APP_CHECKSUM_VERIFICATION.md (0x0504 at
    // both 0x6C and 0x6010).
    REQUIRE(r.pair_at_6c == 0x0504u);
    REQUIRE(r.pair_at_6010 == 0x0504u);
}

TEST_CASE("verify_boot_signatures_sh2a_2mb passes against a factory-virgin reference ROM",
          "[flash][boot_signatures][rom]") {
    auto const path = env_rom_path("SUBUWU_TEST_STOCK_ROM");
    if (!path) {
        WARN("SUBUWU_TEST_STOCK_ROM unset or file absent — skipping.");
        return;
    }
    auto const rom = load_2mb_rom(*path);
    auto const r = fl::verify_boot_signatures_sh2a_2mb(rom);
    REQUIRE(r.ok());
}
