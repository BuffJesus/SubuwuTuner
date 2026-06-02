// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/inserter.hpp"

#include "st/feature_patch/splice.hpp"

#include <cstring>
#include <string>

namespace st::feature_patch {

namespace {

// Write `src.size()` bytes from `src` at `addr` in `dst`. Returns
// ParseError if the write would extend past dst.size().
[[nodiscard]] Status write_bytes(std::vector<std::uint8_t> &dst, std::uint32_t addr,
                                 std::vector<std::uint8_t> const &src) {
    if (static_cast<std::uint64_t>(addr) + src.size() > dst.size()) {
        std::string msg{"feature_patch::apply: write at 0x"};
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", addr);
        msg.append(buf);
        msg.append(" length ");
        msg.append(std::to_string(src.size()));
        msg.append(" extends past source_rom size ");
        msg.append(std::to_string(dst.size()));
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    std::memcpy(dst.data() + addr, src.data(), src.size());
    return ok();
}

// Pick the right short-form splice for the patch's arch.
[[nodiscard]] Result<SpliceBytes> emit_splice(st::feature::codegen::Arch arch,
                                              std::uint32_t splice_address,
                                              std::uint32_t patch_address) {
    switch (arch) {
    case st::feature::codegen::Arch::Sh2a:
        return emit_sh2a_splice(splice_address, patch_address);
    case st::feature::codegen::Arch::Rh850:
        return emit_rh850_splice(splice_address, patch_address);
    case st::feature::codegen::Arch::Unknown:
    default:
        return failure(ErrorCode::InvalidArgument,
                       "feature_patch::apply: PatchObject.arch is Unknown — "
                       "the backend that produced it didn't set the arch field");
    }
}

} // namespace

Result<PatchedRom> apply(st::feature::codegen::PatchObject const &patch,
                         std::vector<std::uint8_t> source_rom,
                         std::vector<RomRegion> writable_code_regions,
                         std::string tuner_tag) {
    if (patch.arch == st::feature::codegen::Arch::Unknown) {
        return failure(ErrorCode::InvalidArgument,
                       "feature_patch::apply: PatchObject.arch is Unknown");
    }
    if (writable_code_regions.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "feature_patch::apply: at least one writable code region "
                       "is required (a pack without [[writable_region]] kind = "
                       "\"code\" cannot accept compiled patches)");
    }

    PatchedRom out{};
    out.bytes = std::move(source_rom);
    out.tuner_tag = std::move(tuner_tag);

    // First writable region carries the manifest. Record its name
    // (for the manifest_address echo) before the allocator consumes
    // the region.
    auto const manifest_region_base = writable_code_regions.front().base;

    RomAllocator alloc{std::move(writable_code_regions)};

    // Per-hook: allocate ROM for the code, write code, emit splice,
    // record manifest entries.
    std::vector<ManifestRecord> records;
    records.reserve(patch.hooks.size() * 2);

    for (auto const &hook : patch.hooks) {
        if (hook.code.empty()) {
            continue; // benign — backend may emit zero-byte hooks for dead stores
        }
        auto claim = alloc.claim(hook.code.size(), 4);
        if (!claim.has_value()) {
            std::string msg{"feature_patch::apply: ROM allocator exhausted for "
                            "hook '"};
            msg.append(hook.symbol);
            msg.append("' (need ");
            msg.append(std::to_string(hook.code.size()));
            msg.append(" bytes) — ");
            msg.append(claim.error().to_string());
            return failure(ErrorCode::OutOfRange, std::move(msg));
        }

        if (auto s = write_bytes(out.bytes, claim->address, hook.code); !s.has_value()) {
            return failure(s.error());
        }
        records.push_back({claim->address, static_cast<std::uint8_t>(ManifestType::NormalPatch),
                           0x01, static_cast<std::uint16_t>(hook.code.size())});

        // Splice from hook.splice_address to claim->address.
        auto splice = emit_splice(patch.arch, static_cast<std::uint32_t>(hook.splice_address),
                                  claim->address);
        if (!splice.has_value()) {
            std::string msg{"feature_patch::apply: splice emit failed for hook '"};
            msg.append(hook.symbol);
            msg.append("' at splice_address 0x");
            char buf[16];
            std::snprintf(buf, sizeof buf, "%08zX", hook.splice_address);
            msg.append(buf);
            msg.append(": ");
            msg.append(splice.error().to_string());
            return failure(splice.error().code(), std::move(msg));
        }
        if (auto s = write_bytes(out.bytes, splice->splice_address, splice->bytes);
            !s.has_value()) {
            return failure(s.error());
        }
        records.push_back({splice->splice_address,
                           static_cast<std::uint8_t>(ManifestType::NormalPatch), 0x01,
                           static_cast<std::uint16_t>(splice->bytes.size())});
    }

    // Encode + write the manifest. The manifest itself takes ROM
    // space, so allocate from the same allocator (will land at the
    // cursor of whichever region has room). Encoded manifest size is
    // (records.size() + 1) * 8.
    auto const encoded_manifest = encode(records);
    auto manifest_claim = alloc.claim(encoded_manifest.size(), 4);
    if (!manifest_claim.has_value()) {
        std::string msg{"feature_patch::apply: ROM allocator exhausted for "
                        "manifest (need "};
        msg.append(std::to_string(encoded_manifest.size()));
        msg.append(" bytes) — ");
        msg.append(manifest_claim.error().to_string());
        return failure(ErrorCode::OutOfRange, std::move(msg));
    }
    if (auto s = write_bytes(out.bytes, manifest_claim->address, encoded_manifest);
        !s.has_value()) {
        return failure(s.error());
    }
    // Hint at where future tools should look for the manifest: the
    // first region's base. We record the actual placement address
    // separately for the uninstaller.
    out.manifest_address = manifest_claim->address;
    (void)manifest_region_base; // reserved for the pack-supplied-manifest_region path

    // The records array reflects what was actually patched (not the
    // manifest's own records — those are bookkeeping).
    out.manifest = std::move(records);

    return out;
}

} // namespace st::feature_patch
