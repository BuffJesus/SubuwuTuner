// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/bzip2_decompress.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#ifdef ST_ETS_HAVE_CIPHER

extern "C" {
#include "bzlib.h"
}

namespace st::devices::ets::cipher {

Result<std::string> bzip2_decompress(std::span<std::uint8_t const> compressed) {
    // Validate the bzip2 magic first — fail fast on AES output that
    // wasn't actually a bzip2 stream (most often: wrong key, wrong
    // nonce, or layer 2/3 plumbing bug). Cheap check, big diagnostic
    // win.
    if (compressed.size() < 3U || compressed[0] != 'B' ||
        compressed[1] != 'Z' || compressed[2] != 'h') {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 bzip2: input does not start with 'BZh' magic "
                           "(layer 3 AES output didn't produce a valid bzip2 "
                           "stream — check key / nonce / earlier layers)");
    }
    // Real `.ptm` payloads inflate ~4x. Start with 16x as headroom
    // and grow once if BZ_OUTBUFF_FULL fires.
    unsigned int out_size = static_cast<unsigned int>(compressed.size() * 16U);
    if (out_size < 64U) {
        out_size = 64U;
    }
    std::vector<char> out(out_size);

    auto *input_buf = const_cast<char *>(
        reinterpret_cast<char const *>(compressed.data()));
    int rc = BZ2_bzBuffToBuffDecompress(
        out.data(), &out_size, input_buf,
        static_cast<unsigned int>(compressed.size()),
        0,  // small=0 → fast path, more memory
        0); // verbosity=0

    if (rc == BZ_OUTBUFF_FULL) {
        out_size = static_cast<unsigned int>(compressed.size() * 64U);
        out.assign(out_size, 0);
        rc = BZ2_bzBuffToBuffDecompress(
            out.data(), &out_size, input_buf,
            static_cast<unsigned int>(compressed.size()),
            0, 0);
    }

    if (rc != BZ_OK) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 bzip2: decompress failed, rc=" +
                               std::to_string(rc));
    }
    return std::string(out.data(), out_size);
}

#ifdef ST_ETS_HAVE_PTM_REWRITE

Result<std::vector<std::uint8_t>>
bzip2_compress(std::span<std::uint8_t const> plaintext) {
    // Block-size 4 per spec §13.2 verification methodology — every
    // reference .ptm sample compresses at level 4. Different levels
    // produce different RLE/MTF/Huffman boundaries; for byte-identical
    // round-trip against the canonical fixture, level 4 is required.
    constexpr int kBlockSize100K = 4;
    constexpr int kVerbosity = 0;
    constexpr int kWorkFactor = 30; // bzip2 1.0.x default

    // Worst-case output for bzip2 is input_size * 1.01 + 600 per the
    // BZ2_bzBuffToBuffCompress contract. Round up to input + 1 KB
    // headroom to be safe.
    unsigned int out_size = static_cast<unsigned int>(plaintext.size() + 1024U);
    std::vector<std::uint8_t> out(out_size);

    auto *input_buf = const_cast<char *>(
        reinterpret_cast<char const *>(plaintext.data()));
    int rc = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char *>(out.data()), &out_size, input_buf,
        static_cast<unsigned int>(plaintext.size()),
        kBlockSize100K, kVerbosity, kWorkFactor);
    if (rc != BZ_OK) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 bzip2: compress failed, rc=" +
                               std::to_string(rc));
    }
    out.resize(out_size);
    return out;
}

#else // !ST_ETS_HAVE_PTM_REWRITE

Result<std::vector<std::uint8_t>>
bzip2_compress(std::span<std::uint8_t const> /*plaintext*/) {
    return st::failure(st::ErrorCode::NotImplemented,
                       "ap3 bzip2_compress: gated by ST_ENABLE_COBB_AP_PTM_REWRITE "
                       "(separate opt-in from the read-only cipher path). "
                       "Configure with -DST_ENABLE_COBB_AP_PTM_REWRITE=ON to enable. "
                       "See docs/34-cobb-ap-as-tune-vault.md.");
}

#endif // ST_ETS_HAVE_PTM_REWRITE

} // namespace st::devices::ets::cipher

#else // !ST_ETS_HAVE_CIPHER

namespace st::devices::ets::cipher {

namespace {

st::Error policy_denied_bzip() {
    return st::Error{st::ErrorCode::PolicyDenied,
                     "ap3: bzip2 layer 4 is excluded from this build. "
                     "Configure with -DST_ENABLE_COBB_AP_CIPHER=ON and pass "
                     "--enable-cobb-ap-cipher at runtime. See "
                     "docs/34-cobb-ap-as-tune-vault.md."};
}

} // namespace

Result<std::string> bzip2_decompress(std::span<std::uint8_t const> /*compressed*/) {
    return st::failure(policy_denied_bzip());
}

Result<std::vector<std::uint8_t>>
bzip2_compress(std::span<std::uint8_t const> /*plaintext*/) {
    return st::failure(policy_denied_bzip());
}

} // namespace st::devices::ets::cipher

#endif // ST_ETS_HAVE_CIPHER
