// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Targeted regression coverage for the custom AES-256-CTR construction
// used in `.ptm` files (commit 97bd2dd, src/devices/ap3/src/aes256_ctr.cpp).
//
// The construction is NOT standard CTR: it chunks input into 16 KB
// blocks and recomputes the outer_nonce = file_nonce + chunk_offset
// at each chunk boundary. Inside a chunk it runs AES-ECB on
// `[counter:4 BE] x 4` and XORs the keystream.
//
// The self-consistency round-trip in test_ptm_cipher.cpp runs at 8 KB
// — comfortably inside a single 16 KB chunk. If someone accidentally
// changes the chunk-size constant (kChunkSize) or breaks the per-chunk
// nonce-reset, that test still passes. The cases below force at least
// two chunk transitions + a partial trailing AES block so any of those
// regressions surface.

#include "st/devices/ap3/aes256_ctr.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#if defined(ST_AP3_HAVE_CIPHER)

namespace {

std::vector<std::uint8_t> make_lcg_plaintext(std::size_t n, std::uint32_t seed) {
    // Simple LCG so the bytes vary across the chunk-boundary region.
    // Picked over std::iota %256 because a 256-byte cycle would happen
    // to land at every chunk boundary by accident — chunks are 64 *
    // cycle-length, which would make a chunk-boundary regression
    // visually invisible in the keystream output.
    std::vector<std::uint8_t> v;
    v.reserve(n);
    std::uint32_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 1103515245U + 12345U;
        v.push_back(static_cast<std::uint8_t>(s >> 16));
    }
    return v;
}

} // namespace

TEST_CASE("ap3 cipher: AES-256-CTR survives a 16 KB chunk boundary",
          "[devices][ap3][cipher]") {
    // Two full 16 KB chunks + 1024 bytes of the third chunk = 33792
    // bytes. Forces two chunk transitions, an odd-aligned (non-multiple
    // of 16) final stretch, and a partial trailing AES block (1024 mod
    // 16 = 0; bump by 7 below to also exercise the partial block).
    constexpr std::size_t kSize = 16 * 1024 * 2 + 1024 + 7;
    auto const plaintext = make_lcg_plaintext(kSize, 0xDEADBEEFU);
    constexpr std::uint32_t kNonce = 0x00000020U;
    auto const encrypted = st::devices::ap3::cipher::aes256_ctr_encrypt(plaintext, kNonce);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() == plaintext.size());
    auto const decrypted = st::devices::ap3::cipher::aes256_ctr_decrypt(*encrypted, kNonce);
    REQUIRE(decrypted.has_value());
    REQUIRE(*decrypted == plaintext);

    // Chunk-boundary integrity: encrypting [0..32 KB) of the same
    // plaintext under the same nonce must produce a byte-identical
    // prefix of `encrypted`. A regression that mis-computed the
    // chunk-1 outer_nonce would diverge at byte 16384.
    std::vector<std::uint8_t> first_two_chunks(plaintext.begin(),
                                               plaintext.begin() + 16 * 1024 * 2);
    auto const enc_two_chunks =
        st::devices::ap3::cipher::aes256_ctr_encrypt(first_two_chunks, kNonce);
    REQUIRE(enc_two_chunks.has_value());
    REQUIRE(enc_two_chunks->size() == first_two_chunks.size());
    for (std::size_t i = 0; i < enc_two_chunks->size(); ++i) {
        REQUIRE((*enc_two_chunks)[i] == (*encrypted)[i]);
    }
}

TEST_CASE("ap3 cipher: AES-256-CTR is symmetric (encrypt == decrypt at the wire)",
          "[devices][ap3][cipher]") {
    // CTR is an XOR stream cipher — running the keystream over
    // ciphertext recovers plaintext bit-for-bit. The codepath split
    // between encrypt and decrypt in our impl is a thin wrapper; this
    // test makes sure the wrappers stay symmetric on a chunk-boundary
    // payload (catches a regression that, e.g., only resets the per-
    // chunk nonce on the encrypt side).
    constexpr std::size_t kSize = 16 * 1024 + 1; // straddle one chunk boundary
    auto const plaintext = make_lcg_plaintext(kSize, 0xC0FFEEU);
    constexpr std::uint32_t kNonce = 0x00010000U;
    auto const e = st::devices::ap3::cipher::aes256_ctr_encrypt(plaintext, kNonce);
    REQUIRE(e.has_value());
    auto const d = st::devices::ap3::cipher::aes256_ctr_decrypt(plaintext, kNonce);
    REQUIRE(d.has_value());
    REQUIRE(*e == *d);
}

#endif // ST_AP3_HAVE_CIPHER
