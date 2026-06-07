// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash/boot_signatures.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace st::flash {

namespace {

// Read a big-endian u16 at offset `off` from `image`. Caller has
// already bounds-checked.
[[nodiscard]] std::uint16_t
read_u16_be(std::span<std::uint8_t const> image, std::uint32_t off) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(image[off]) << 8) |
        static_cast<std::uint16_t>(image[off + 1]));
}

} // namespace

BootSignatureReport
verify_boot_signatures_sh2a_2mb(std::span<std::uint8_t const> image) noexcept {
    BootSignatureReport r;
    // The highest offset we touch is kBootSigBOffset + 1 (the BE u16
    // ends one byte past its start). Bail before any of the reads
    // could over-run the buffer.
    if (image.size() < static_cast<std::size_t>(kBootSigBOffset) + 2u) {
        r.failure = BootSignatureFailure::TooSmall;
        return r;
    }

    r.sig_a_actual = read_u16_be(image, kBootSigAOffset);
    r.sig_b_actual = read_u16_be(image, kBootSigBOffset);
    r.pair_at_6c = read_u16_be(image, kBootPairLowOffset);
    r.pair_at_6010 = read_u16_be(image, kBootPairHighOffset);

    // First failure wins (in offset order). All four actual values
    // are populated regardless, so a CLI / GUI can still surface
    // every signature for the user.
    if (r.sig_a_actual != kBootSigAExpected) {
        r.failure = BootSignatureFailure::SigA;
    } else if (r.pair_at_6c != r.pair_at_6010) {
        r.failure = BootSignatureFailure::PairMismatch;
    } else if (r.sig_b_actual != kBootSigBExpected) {
        r.failure = BootSignatureFailure::SigB;
    }
    // Default-constructed r.failure is Ok; leave it.
    return r;
}

std::string_view
boot_signature_failure_name(BootSignatureFailure f) noexcept {
    switch (f) {
    case BootSignatureFailure::Ok:
        return "ok";
    case BootSignatureFailure::TooSmall:
        return "too_small";
    case BootSignatureFailure::SigA:
        return "sig_a_mismatch";
    case BootSignatureFailure::SigB:
        return "sig_b_mismatch";
    case BootSignatureFailure::PairMismatch:
        return "pair_mismatch";
    }
    return "unknown";
}

} // namespace st::flash
