// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the 0xB6 bulk-reflash wire cipher. The whole TU is gated on
// ST_ENABLE_BULK_REFLASH_CIPHER so the file compiles cleanly in default
// (capability-off) builds — when the macro is undefined the file becomes
// a single Catch2 sentinel test that confirms the build is operating in
// the public/default configuration.

#include "st/ecu/bulk_reflash_cipher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#ifdef ST_ENABLE_BULK_REFLASH_CIPHER

namespace {

// Known plaintext <-> ciphertext vectors recovered analyst-side.
struct Vector {
    std::uint32_t plaintext;
    std::uint32_t ciphertext;
    char const *note;
};

constexpr std::array<Vector, 7> kVectors = {{
    {0xFFFFFFFFU, 0x941478D7U, "erased flash all-FF"},
    {0x00000000U, 0x98A49DE7U, "all-zero block"},
    {0x5555FFFFU, 0xF135FA11U, "offset 0x00"},
    {0x0504FFFFU, 0xFB73BC27U, "offset 0x10"},
    {0x33333932U, 0x122E4E98U, "offset 0x14"},
    {0x302D3832U, 0xD99F6C2DU, "offset 0x18"},
    {0x4A322A00U, 0xDA7690CAU, "offset 0x1C"},
}};

}  // namespace

TEST_CASE("bulk_reflash_cipher: decrypt round-trip matches known vectors",
          "[ecu][bulk_reflash_cipher]") {
    using namespace st::ecu::bulk_reflash;
    for (auto const &v : kVectors) {
        INFO("vector: " << v.note);
        REQUIRE(feistel_block(v.ciphertext, kRoundKeysDecrypt) == v.plaintext);
    }
}

TEST_CASE("bulk_reflash_cipher: encrypt round-trip matches known vectors",
          "[ecu][bulk_reflash_cipher]") {
    using namespace st::ecu::bulk_reflash;
    for (auto const &v : kVectors) {
        INFO("vector: " << v.note);
        REQUIRE(feistel_block(v.plaintext, kRoundKeysEncrypt) == v.ciphertext);
    }
}

TEST_CASE("bulk_reflash_cipher: encrypt then decrypt is identity",
          "[ecu][bulk_reflash_cipher]") {
    using namespace st::ecu::bulk_reflash;
    // Drive a representative spread of inputs through encrypt then decrypt
    // and verify we get the original back. Includes the canonical vectors
    // plus a few synthetic values to widen coverage.
    std::vector<std::uint32_t> const inputs = {
        0x00000000U, 0xFFFFFFFFU, 0xDEADBEEFU, 0x12345678U,
        0xAABBCCDDU, 0x55555555U, 0xAAAAAAAAU, 0x80000001U,
    };
    for (auto const x : inputs) {
        INFO("input: " << std::hex << x);
        std::uint32_t const wire = feistel_block(x, kRoundKeysEncrypt);
        std::uint32_t const back = feistel_block(wire, kRoundKeysDecrypt);
        REQUIRE(back == x);
    }
}

TEST_CASE("bulk_reflash_cipher: byte-buffer ECB encrypt matches block-by-block",
          "[ecu][bulk_reflash_cipher]") {
    using namespace st::ecu::bulk_reflash;
    // Two 4-byte blocks worth of plaintext.
    std::array<std::uint8_t, 8> plaintext = {0x55, 0x55, 0xFF, 0xFF, 0x05, 0x04, 0xFF, 0xFF};
    auto const encrypted = encrypt_bytes(plaintext);
    REQUIRE(encrypted.size() == plaintext.size());
    // Block 0 should match kVectors[2] (0x5555FFFF -> 0xF135FA11).
    REQUIRE(encrypted[0] == 0xF1);
    REQUIRE(encrypted[1] == 0x35);
    REQUIRE(encrypted[2] == 0xFA);
    REQUIRE(encrypted[3] == 0x11);
    // Block 1 should match kVectors[3] (0x0504FFFF -> 0xFB73BC27).
    REQUIRE(encrypted[4] == 0xFB);
    REQUIRE(encrypted[5] == 0x73);
    REQUIRE(encrypted[6] == 0xBC);
    REQUIRE(encrypted[7] == 0x27);

    // Round-trip through decrypt_bytes.
    auto const recovered = decrypt_bytes(encrypted);
    REQUIRE(recovered.size() == plaintext.size());
    REQUIRE(std::equal(recovered.begin(), recovered.end(), plaintext.begin()));
}

TEST_CASE("bulk_reflash_cipher: arming is opt-in and idempotent",
          "[ecu][bulk_reflash_cipher]") {
    using namespace st::ecu::bulk_reflash;
    // Note: arming is process-global and persists across test cases, so we
    // can't test the unarmed-default state here (other tests may have run
    // first). We just verify the arm() call doesn't throw and that
    // is_armed() reports true afterwards.
    arm();
    REQUIRE(is_armed());
    // Idempotent: a second arm() doesn't unset.
    arm();
    REQUIRE(is_armed());
}

#else

TEST_CASE("bulk_reflash_cipher: not compiled in (default public-build config)",
          "[ecu][bulk_reflash_cipher][gated]") {
    // Sentinel test — when the build configures -DST_ENABLE_BULK_REFLASH_CIPHER=OFF
    // (the default for public builds), there's nothing to test. This case
    // exists only so the file produces at least one Catch2 entry in either
    // configuration, which keeps the tests/CMakeLists.txt entry valid.
    SUCCEED("bulk-reflash cipher disabled at build time; no cipher symbols to exercise");
}

#endif  // ST_ENABLE_BULK_REFLASH_CIPHER
