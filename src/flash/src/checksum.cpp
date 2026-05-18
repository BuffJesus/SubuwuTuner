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
        case ChecksumKind::None:       return "none";
        case ChecksumKind::SubaruStd:  return "subaru_std";
        case ChecksumKind::SubaruAlt:  return "subaru_alt";
        case ChecksumKind::SubaruAlt2: return "subaru_alt2";
    }
    return "unknown";
}

std::optional<ChecksumKind> parse_checksum_kind(std::string_view s) noexcept {
    if (s == "none")        return ChecksumKind::None;
    if (s == "subaru_std")  return ChecksumKind::SubaruStd;
    if (s == "subaru_alt")  return ChecksumKind::SubaruAlt;
    if (s == "subaru_alt2") return ChecksumKind::SubaruAlt2;
    return std::nullopt;
}

ChecksumKind checksum_kind_from_pack(
    std::string_view checksum_type_field) noexcept {
    if (checksum_type_field.empty()) return ChecksumKind::None;
    auto const k = parse_checksum_kind(checksum_type_field);
    return k.has_value() ? *k : ChecksumKind::None;
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

// Family of Subaru repair stubs. Each surfaces a NotImplemented
// with a citation pointer to where the algorithm lives in public
// references so the next implementer doesn't have to re-find it.
// When the algorithm lands, this class gets its `repair()` filled
// in + the citation comment removed.
template <ChecksumKind Kind, char const *CitationRef>
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
inline constexpr char kStdCitation[]  =
    "RomRaider's `src/main/java/com/romraider/maps/checksum/"
    "ChecksumSTD.java` (open-source GPL reference — implement "
    "fresh from the algorithm shape per clean-room rules in "
    "docs/15)";
inline constexpr char kAltCitation[]  =
    "RomRaider's `ChecksumALT.java` (older 16-bit Subaru family)";
inline constexpr char kAlt2Citation[] =
    "RomRaider's `ChecksumALT2.java` (some 32-bit Subaru ROMs — "
    "release notes mention SH-2A platforms)";

} // namespace

std::unique_ptr<IChecksumRepair> make_checksum_repair(ChecksumKind kind) {
    switch (kind) {
        case ChecksumKind::None:
            return std::make_unique<NoneRepair>();
        case ChecksumKind::SubaruStd:
            return std::make_unique<
                SubaruRepairStub<ChecksumKind::SubaruStd, kStdCitation>>();
        case ChecksumKind::SubaruAlt:
            return std::make_unique<
                SubaruRepairStub<ChecksumKind::SubaruAlt, kAltCitation>>();
        case ChecksumKind::SubaruAlt2:
            return std::make_unique<
                SubaruRepairStub<ChecksumKind::SubaruAlt2, kAlt2Citation>>();
    }
    // Unreachable per the exhaustive switch; defensive None fallback.
    return std::make_unique<NoneRepair>();
}

st::Status apply_checksum_repair(std::span<std::uint8_t> rom_bytes,
                                   Definition const &     def) noexcept {
    auto const kind   = checksum_kind_from_pack(def.pack().checksum_type);
    auto       repair = make_checksum_repair(kind);
    return repair->repair(rom_bytes);
}

} // namespace st::flash
