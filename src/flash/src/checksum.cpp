// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash/checksum.hpp"

#include "st/core/error.hpp"
#include "st/defs.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace st::flash {

char const *checksum_kind_name(ChecksumKind k) noexcept {
    switch (k) {
    case ChecksumKind::None:
        return "none";
    case ChecksumKind::SubaruStd:
        return "subaru_std";
    case ChecksumKind::SubaruAlt:
        return "subaru_alt";
    case ChecksumKind::SubaruAlt2:
        return "subaru_alt2";
    }
    return "unknown";
}

std::optional<ChecksumKind> parse_checksum_kind(std::string_view s) noexcept {
    if (s == "none")
        return ChecksumKind::None;
    if (s == "subaru_std")
        return ChecksumKind::SubaruStd;
    if (s == "subaru_alt")
        return ChecksumKind::SubaruAlt;
    if (s == "subaru_alt2")
        return ChecksumKind::SubaruAlt2;
    return std::nullopt;
}

ChecksumKind checksum_kind_from_pack(std::string_view checksum_type_field) noexcept {
    if (checksum_type_field.empty())
        return ChecksumKind::None;
    auto const k = parse_checksum_kind(checksum_type_field);
    return k.has_value() ? *k : ChecksumKind::None;
}

std::string_view SubaruStdConfig::validate() const noexcept {
    if (cal_end <= cal_start)
        return "cal_end must exceed cal_start";
    if ((sum_slot & 0x3u) != 0)
        return "sum_slot must be 4-byte aligned";
    if ((complement_slot & 0x3u) != 0)
        return "complement_slot must be 4-byte aligned";
    if (sum_slot == complement_slot)
        return "sum_slot and complement_slot must differ";
    // Slots may sit inside OR outside the cal region. The default
    // SH-2A 2 MB layout actually puts them in the bootloader region
    // at 0x100/0x104 (outside the 0x10000-start cal region). Only
    // detect a partial overlap (one slot byte inside cal, one
    // outside), which would imply a misconfigured cal_start/cal_end.
    auto const overlaps_partial = [&](std::uint32_t off) {
        bool const start_in =
            (off >= cal_start) && (off < cal_end);
        bool const end_in =
            (off + 4 > cal_start) && (off + 4 <= cal_end);
        // Both in or both out is fine. Partial overlap isn't.
        return start_in != end_in;
    };
    if (overlaps_partial(sum_slot))
        return "sum_slot partially overlaps the cal region";
    if (overlaps_partial(complement_slot))
        return "complement_slot partially overlaps the cal region";
    return {};
}

namespace {

// No-op repair — pack didn't declare a kind, or explicitly said
// "no checksum here." Always succeeds without touching bytes.
class NoneRepair final : public IChecksumRepair {
public:
    [[nodiscard]] st::Status repair(std::span<std::uint8_t>) noexcept override {
        return ok();
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "none";
    }
};

// SubaruStd — sum-of-16-bit-words mod 2^32 over the cal region,
// excluding the slot bytes themselves. The lower-address slot stores
// the sum (BE); the upper stores ~sum (BE) so a boot-time
// integrity check that verifies sum+complement == 0xFFFFFFFF holds.
//
// Implementation is fresh from the documented algorithm shape per
// the clean-room rules in docs/15 — no source lifted from RomRaider
// or any other reference. Algorithm shape (public knowledge from
// Subaru tuning literature):
//   1. Sum word[i] = (rom[2i]<<8) | rom[2i+1], for i covering
//      cal_start..cal_end in word steps, SKIPPING the 4-byte sum
//      slot and the 4-byte complement slot.
//   2. The accumulated 32-bit sum is the value written at
//      sum_slot (BE: high byte first).
//   3. The bitwise complement of that sum is written at
//      complement_slot (BE).
//
// **Validation status**: matches the documented algorithm shape;
// has NOT yet been byte-validated against a known-good stock Subaru
// ROM. Until that validation runs, downstream callers should
// treat the output as "computed per spec" but not "guaranteed to
// match what the ECU's bootloader will accept." See bench-rig
// validation gate in docs/04 Phase 4.
class SubaruStdRepair final : public IChecksumRepair {
public:
    explicit SubaruStdRepair(SubaruStdConfig cfg) noexcept : cfg_{cfg} {}

    [[nodiscard]] st::Status
    repair(std::span<std::uint8_t> rom) noexcept override {
        if (auto const v = cfg_.validate(); !v.empty()) {
            return failure(ErrorCode::InvalidArgument,
                           std::string{"flash::subaru_std::repair: "} +
                               std::string{v});
        }
        if (rom.size() < cfg_.cal_end) {
            return failure(ErrorCode::InvalidArgument,
                           "flash::subaru_std::repair: ROM is too small for "
                           "the configured cal region");
        }
        // Cal region must be word-aligned for a 16-bit walk. The
        // standard layouts (boot ends at 0x10000) satisfy this, but
        // defensive-check anyway.
        if ((cfg_.cal_start & 0x1u) != 0 || (cfg_.cal_end & 0x1u) != 0) {
            return failure(ErrorCode::InvalidArgument,
                           "flash::subaru_std::repair: cal region bounds "
                           "must be 2-byte aligned");
        }
        std::uint32_t sum = 0;
        std::uint32_t const ss = cfg_.sum_slot;
        std::uint32_t const cs = cfg_.complement_slot;
        for (std::uint32_t off = cfg_.cal_start; off + 2 <= cfg_.cal_end;
             off += 2) {
            // Skip the 4 bytes of either slot.
            bool const in_sum_slot = (off >= ss && off < ss + 4);
            bool const in_complement_slot = (off >= cs && off < cs + 4);
            if (in_sum_slot || in_complement_slot)
                continue;
            std::uint32_t const word =
                (static_cast<std::uint32_t>(rom[off]) << 8) |
                static_cast<std::uint32_t>(rom[off + 1]);
            sum += word;
        }
        // Write the sum (BE) at sum_slot and ~sum (BE) at complement_slot.
        // Idempotent: if the existing bytes already match, the writes
        // produce no functional change.
        std::uint32_t const inverted = ~sum;
        rom[ss + 0] = static_cast<std::uint8_t>((sum >> 24) & 0xFFu);
        rom[ss + 1] = static_cast<std::uint8_t>((sum >> 16) & 0xFFu);
        rom[ss + 2] = static_cast<std::uint8_t>((sum >> 8) & 0xFFu);
        rom[ss + 3] = static_cast<std::uint8_t>((sum) & 0xFFu);
        rom[cs + 0] = static_cast<std::uint8_t>((inverted >> 24) & 0xFFu);
        rom[cs + 1] = static_cast<std::uint8_t>((inverted >> 16) & 0xFFu);
        rom[cs + 2] = static_cast<std::uint8_t>((inverted >> 8) & 0xFFu);
        rom[cs + 3] = static_cast<std::uint8_t>((inverted) & 0xFFu);
        return ok();
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "subaru_std";
    }

private:
    SubaruStdConfig cfg_;
};

// Stub family for the not-yet-implemented Subaru variants. Each
// surfaces a NotImplemented with a citation pointer to where the
// algorithm lives in public references so the next implementer
// doesn't have to re-find it.
template<ChecksumKind Kind, char const *CitationRef>
class SubaruRepairStub final : public IChecksumRepair {
public:
    [[nodiscard]] st::Status repair(std::span<std::uint8_t>) noexcept override {
        std::string msg{"flash::"};
        msg.append(name());
        msg.append("::repair: algorithm not yet implemented. See ");
        msg.append(CitationRef);
        msg.append(" for the reference + bench-validate against a "
                   "known-good stock ROM before enabling.");
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return checksum_kind_name(Kind);
    }
};

// Citation strings — referenced by the stubs above. Stored at
// namespace scope so a non-type template parameter can take their
// addresses without ODR / static-init-order surprises.
inline constexpr char kAltCitation[] =
    "RomRaider's `ChecksumALT.java` (older 16-bit Subaru family)";
inline constexpr char kAlt2Citation[] =
    "RomRaider's `ChecksumALT2.java` (some 32-bit Subaru ROMs — "
    "release notes mention SH-2A platforms)";

} // namespace

std::unique_ptr<IChecksumRepair>
make_subaru_std_repair(SubaruStdConfig const &cfg) {
    return std::make_unique<SubaruStdRepair>(cfg);
}

std::unique_ptr<IChecksumRepair> make_checksum_repair(ChecksumKind kind) {
    switch (kind) {
    case ChecksumKind::None:
        return std::make_unique<NoneRepair>();
    case ChecksumKind::SubaruStd:
        return std::make_unique<SubaruStdRepair>(SubaruStdConfig{});
    case ChecksumKind::SubaruAlt:
        return std::make_unique<SubaruRepairStub<ChecksumKind::SubaruAlt, kAltCitation>>();
    case ChecksumKind::SubaruAlt2:
        return std::make_unique<SubaruRepairStub<ChecksumKind::SubaruAlt2, kAlt2Citation>>();
    }
    // Unreachable per the exhaustive switch; defensive None fallback.
    return std::make_unique<NoneRepair>();
}

st::Status apply_checksum_repair(std::span<std::uint8_t> rom_bytes,
                                 Definition const &def) noexcept {
    auto const kind = checksum_kind_from_pack(def.pack().checksum_type);
    auto repair = make_checksum_repair(kind);
    return repair->repair(rom_bytes);
}

} // namespace st::flash
