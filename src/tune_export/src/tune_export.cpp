// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/tune_export.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace st::tune_export {

namespace {

constexpr std::uint32_t kBalanceBlockStart =
    kBalanceOffset & ~(kBlockSize - 1U);  // 0x1FFF00

// Whether the half-open range [diff_start, diff_end) overlaps the
// half-open range [target_start, target_end).
constexpr bool
ranges_overlap(std::uint32_t a0, std::uint32_t a1,
               std::uint32_t b0, std::uint32_t b1) noexcept {
    return a0 < b1 && b0 < a1;
}

}  // namespace

std::uint16_t
u16be_sum(std::span<std::uint8_t const> bytes,
          std::uint32_t                 a,
          std::uint32_t                 e) noexcept {
    std::uint32_t acc = 0;
    // Walk u16 pairs; deliberately operate in mod-2^16 to match
    // the firmware's naive accumulator at ROM 0x3582.
    for (std::uint32_t i = a; i + 1U < e && i + 1U < bytes.size(); i += 2U) {
        acc = (acc + (static_cast<std::uint32_t>(bytes[i]) << 8) +
               static_cast<std::uint32_t>(bytes[i + 1U])) &
              0xFFFFU;
    }
    return static_cast<std::uint16_t>(acc);
}

std::uint16_t
compute_balance(std::span<std::uint8_t const> rom) noexcept {
    auto const s = u16be_sum(rom, kSumRangeStart, kSumRangeEnd);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(kSumTarget) -
         static_cast<std::uint32_t>(s)) &
        0xFFFFU);
}

std::vector<ValidationError>
validate(std::span<std::uint8_t const> base,
         std::span<CalDiff const>      diffs) {
    std::vector<ValidationError> errors;

    if (!base.empty() && base.size() != kWritableEnd) {
        errors.push_back({
            ValidationError::Kind::BaseSizeMismatch,
            static_cast<std::uint32_t>(base.size()),
            "base ROM size != 0x200000 bytes"});
        return errors;
    }

    for (auto const &d : diffs) {
        auto const d_end =
            d.offset + static_cast<std::uint32_t>(d.bytes.size());

        if (d.offset >= kWdtTrapStart || d_end > kWdtTrapStart) {
            errors.push_back({ValidationError::Kind::WdtTrapAddress,
                              d.offset, "diff at or past WDT trap"});
            continue;
        }
        if (d.offset < kFaciLockedEnd) {
            errors.push_back({ValidationError::Kind::FaciLockedAddress,
                              d.offset,
                              "diff in FACI-locked silent-drop range"});
            continue;
        }
        if (ranges_overlap(d.offset, d_end,
                           kBalanceOffset,
                           kBalanceOffset + kBalanceLength)) {
            errors.push_back({ValidationError::Kind::BalanceClobber,
                              d.offset,
                              "pipeline owns balance cell 0x1FFFFE"});
            continue;
        }
        if (ranges_overlap(d.offset, d_end,
                           kTrailerReservedStart,
                           kTrailerReservedEnd)) {
            errors.push_back({ValidationError::Kind::TrailerClobber,
                              d.offset,
                              "trailer landmarks must be preserved"});
            continue;
        }
        for (auto b : kBootIntegrityBytesMustBePreserved) {
            if (d.offset <= b && d_end > b) {
                errors.push_back({
                    ValidationError::Kind::BootIntegrityClobber, b,
                    "boot integrity check reads this byte"});
                break;
            }
        }
    }
    return errors;
}

Result<std::vector<std::uint8_t>>
build_image(std::span<std::uint8_t const> base,
            std::span<CalDiff const>      diffs) {
    auto errs = validate(base, diffs);
    if (!errs.empty()) {
        return failure(Error{ErrorCode::PolicyDenied, errs[0].explanation});
    }
    if (base.size() != kWritableEnd) {
        return failure(Error{ErrorCode::PolicyDenied,
                             "base ROM size != 0x200000 bytes"});
    }

    std::vector<std::uint8_t> img(base.begin(), base.end());
    for (auto const &d : diffs) {
        std::memcpy(img.data() + d.offset, d.bytes.data(), d.bytes.size());
    }

    // Recompute the balance from scratch. Zero the cell first so
    // any residual from the base ROM doesn't double-count.
    img[kBalanceOffset]      = 0U;
    img[kBalanceOffset + 1U] = 0U;
    auto const bal = compute_balance(img);
    img[kBalanceOffset]      = static_cast<std::uint8_t>((bal >> 8) & 0xFFU);
    img[kBalanceOffset + 1U] = static_cast<std::uint8_t>(bal & 0xFFU);

    return img;
}

std::vector<WriteBlock>
emit_write_plan(std::span<std::uint8_t const> built_image,
                std::span<std::uint8_t const> base_rom) {
    std::vector<WriteBlock> plan;
    if (built_image.size() != kWritableEnd ||
        base_rom.size() != kWritableEnd) {
        return plan;
    }

    auto const fill_block = [&](std::uint32_t addr,
                                WriteBlock::Tag tag) {
        WriteBlock blk;
        blk.addr = addr;
        std::copy_n(built_image.begin() + addr, kBlockSize,
                    blk.data.begin());
        blk.tag = tag;
        plan.push_back(std::move(blk));
    };

    for (std::uint32_t addr = kWritableStart; addr < kWritableEnd;
         addr += kBlockSize) {
        if (addr == kBalanceBlockStart) {
            continue;  // emit last
        }
        bool changed = false;
        for (std::uint32_t i = 0; i < kBlockSize; ++i) {
            if (built_image[addr + i] != base_rom[addr + i]) {
                changed = true;
                break;
            }
        }
        if (changed) {
            fill_block(addr, WriteBlock::Tag::CalChange);
        }
    }

    // Trailer block ALWAYS emitted last — the balance is recomputed
    // every build, so even if a tune happens to leave the trailer
    // bit-identical to base, the next tune-export run that DOES
    // change the balance would need this block, so we keep the
    // wire-plan shape stable. Tag distinguishes intent.
    fill_block(kBalanceBlockStart, WriteBlock::Tag::BalanceWrite);

    return plan;
}

std::string_view
to_string(ValidationError::Kind k) noexcept {
    switch (k) {
        case ValidationError::Kind::FaciLockedAddress:
            return "FaciLockedAddress";
        case ValidationError::Kind::WdtTrapAddress:
            return "WdtTrapAddress";
        case ValidationError::Kind::BalanceClobber:
            return "BalanceClobber";
        case ValidationError::Kind::TrailerClobber:
            return "TrailerClobber";
        case ValidationError::Kind::BootIntegrityClobber:
            return "BootIntegrityClobber";
        case ValidationError::Kind::BaseSizeMismatch:
            return "BaseSizeMismatch";
    }
    return "(unknown)";
}

std::string_view
to_string(ChecksumState s) noexcept {
    switch (s) {
        case ChecksumState::NotApplicable:
            return "NotApplicable";
        case ChecksumState::FlashReady:
            return "FlashReady";
        case ChecksumState::NeedsRepair:
            return "NeedsRepair";
    }
    return "(unknown)";
}

std::uint16_t
flash_checksum(std::span<std::uint8_t const> rom) noexcept {
    return u16be_sum(rom, kSumRangeStart, kSumRangeEnd);
}

ChecksumState
checksum_state(std::span<std::uint8_t const> rom) noexcept {
    // The 2 MB contract is only meaningful on a full 2 MB SH-2A image.
    if (rom.size() != kWritableEnd) {
        return ChecksumState::NotApplicable;
    }
    return flash_checksum(rom) == kSumTarget ? ChecksumState::FlashReady
                                             : ChecksumState::NeedsRepair;
}

st::Status
repair_balance(std::span<std::uint8_t> rom) noexcept {
    if (rom.size() != kWritableEnd) {
        return failure(Error{ErrorCode::InvalidArgument,
                             "repair_balance: image is not a 2 MB SH-2A ROM "
                             "(size != 0x200000); the 0x5AA5 contract does not "
                             "apply"});
    }
    // compute_balance sums the WHOLE window including the current balance
    // bytes, so zero them first (mirrors build_image) — otherwise the
    // result compensates against a stale balance and the sum drifts.
    rom[kBalanceOffset]      = 0U;
    rom[kBalanceOffset + 1U] = 0U;
    auto const bal           = compute_balance(rom);
    rom[kBalanceOffset]      = static_cast<std::uint8_t>((bal >> 8) & 0xFFU);
    rom[kBalanceOffset + 1U] = static_cast<std::uint8_t>(bal & 0xFFU);
    return {};
}

}  // namespace st::tune_export
