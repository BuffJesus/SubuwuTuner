// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/splice.hpp"

#include <cstdint>
#include <string>

namespace st::feature_patch {

namespace {

// SH-2A BRA disp12 encoder. 1010 dddd dddd dddd; target = PC + 4 + disp*2
// where PC is the BRA instruction's address. Mirrors the equivalent
// helper in src/feature_codegen/src/sh2a.hpp (private to that target).
// Local copy here so feature_patch doesn't depend on feature_codegen
// for ISA encoder primitives; BRA's encoding is silicon-stable.
[[nodiscard]] constexpr std::uint16_t enc_bra(std::int16_t disp) noexcept {
    return static_cast<std::uint16_t>(0xA000U |
                                      (static_cast<std::uint16_t>(disp) & 0x0FFFU));
}

// SH-2A NOP. Encoding 0x0009. Fills delay slots so the splice has no
// side effect beyond the branch itself.
[[nodiscard]] constexpr std::uint16_t enc_nop() noexcept {
    return 0x0009U;
}

// SH-2A is big-endian on the wire. Write a u16 in BE order.
void emit_be16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

} // namespace

std::optional<std::int16_t> sh2a_bra_disp12(std::uint32_t splice_address,
                                            std::uint32_t patch_address) noexcept {
    // target = splice + 4 + disp*2  ⇒  disp = (target - splice - 4) / 2
    auto const signed_gap = static_cast<std::int64_t>(patch_address) -
                            static_cast<std::int64_t>(splice_address) - 4;
    if ((signed_gap % 2) != 0) {
        return std::nullopt; // unaligned — SH-2A targets must be 2-aligned
    }
    auto const halfword_disp = signed_gap / 2;
    // 12-bit signed range: [-2048, 2047].
    if (halfword_disp < -2048 || halfword_disp > 2047) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(halfword_disp);
}

Result<SpliceBytes> emit_sh2a_splice(std::uint32_t splice_address,
                                     std::uint32_t patch_address) {
    if ((splice_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_sh2a_splice: splice_address must be 2-aligned "
                       "(SH-2A is a 16-bit fixed-instruction ISA)");
    }
    if ((patch_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_sh2a_splice: patch_address must be 2-aligned");
    }

    auto const disp = sh2a_bra_disp12(splice_address, patch_address);
    if (disp.has_value()) {
        SpliceBytes out{};
        out.form = SpliceForm::Sh2aShortBra;
        out.splice_address = splice_address;
        out.patch_address = patch_address;
        out.bytes.reserve(4);
        emit_be16(out.bytes, enc_bra(*disp));
        emit_be16(out.bytes, enc_nop()); // delay slot
        return out;
    }

    // Long form needs the literal-pool + JMP @R0 pattern + the
    // displaced-instruction analyzer. Lands in the next bundle; in
    // the meantime, surface a precise error so a caller knows exactly
    // why the splice can't be emitted yet.
    auto const signed_gap = static_cast<std::int64_t>(patch_address) -
                            static_cast<std::int64_t>(splice_address) - 4;
    std::string msg{"emit_sh2a_splice: splice→patch displacement "};
    msg.append(std::to_string(signed_gap));
    msg.append(" bytes is outside BRA range [");
    msg.append(std::to_string(kSh2aBraMinByteOffset));
    msg.append(", ");
    msg.append(std::to_string(kSh2aBraMaxByteOffset));
    msg.append("] — long-form (MOV.L + JMP @Rn + literal pool) "
               "not yet implemented in this slice");
    return failure(ErrorCode::NotImplemented, std::move(msg));
}

} // namespace st::feature_patch
