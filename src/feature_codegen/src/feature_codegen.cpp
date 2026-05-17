// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_codegen.hpp"

#include "st/core/error.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace st::feature::codegen {

namespace {

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

} // namespace

char const *arch_name(Arch a) noexcept {
    switch (a) {
        case Arch::Sh2a:    return "sh2a";
        case Arch::Rh850:   return "rh850";
        case Arch::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view arch_name_sv(Arch a) noexcept { return arch_name(a); }

// ---- RamAllocator -------------------------------------------------------

RamAllocator::RamAllocator(std::size_t base, std::size_t length) noexcept
    : base_(base), length_(length), cursor_(base) {}

Result<RamClaim> RamAllocator::claim(std::size_t size,
                                      std::size_t alignment) noexcept {
    if (size == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "RamAllocator::claim: size must be > 0");
    }
    // Normalize alignment: <2 means "no alignment" → 1. Otherwise it
    // must be a power of two so cursor rounding has a single-step
    // closed form.
    std::size_t const align = alignment < 2U ? 1U : alignment;
    if (align != 1U && !is_power_of_two(align)) {
        return failure(ErrorCode::InvalidArgument,
                       "RamAllocator::claim: alignment must be a power of two");
    }

    // Round cursor up to `align`. Overflow-guarded — if cursor + (align - 1)
    // would wrap past length_, refuse before we compute the masked address.
    std::size_t const mask     = align - 1U;
    std::size_t const slack    = (align - (cursor_ & mask)) & mask;
    if (slack > length_ || cursor_ + slack < cursor_) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: alignment overflow");
    }
    std::size_t const aligned = cursor_ + slack;

    // Bounds check the claim itself.
    if (aligned + size < aligned) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: size overflow");
    }
    if (aligned + size > base_ + length_) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: region exhausted");
    }

    RamClaim out{};
    out.address   = aligned;
    out.size      = size;
    out.alignment = align;
    cursor_       = aligned + size;
    return out;
}

void RamAllocator::reset() noexcept { cursor_ = base_; }

// ---- Backend stubs ------------------------------------------------------

Result<PatchObject> Sh2aBackend::compile(ir::Module const & /*m*/,
                                          Definition const & /*def*/) {
    // Stub — instruction emission lands in a follow-up bundle.
    // Returning NotImplemented (rather than ok with an empty
    // PatchObject) so any caller that wires this up before the impl
    // is ready surfaces visibly instead of silently producing an
    // empty patch.
    return failure(ErrorCode::NotImplemented,
                   "SH-2A backend: instruction emission not yet implemented");
}

Result<PatchObject> Rh850Backend::compile(ir::Module const & /*m*/,
                                           Definition const & /*def*/) {
    return failure(ErrorCode::NotImplemented,
                   "RH850 backend: instruction emission not yet implemented");
}

// ---- Backend selection --------------------------------------------------

namespace {

[[nodiscard]] bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char const ca = a[i] >= 'a' && a[i] <= 'z'
                            ? static_cast<char>(a[i] - 32)
                            : a[i];
        char const cb = b[i] >= 'a' && b[i] <= 'z'
                            ? static_cast<char>(b[i] - 32)
                            : b[i];
        if (ca != cb) return false;
    }
    return true;
}

} // namespace

Result<std::unique_ptr<IBackend>> select_backend(std::string_view platform) {
    if (ieq(platform, "VA")) {
        return std::unique_ptr<IBackend>{new Sh2aBackend{}};
    }
    if (ieq(platform, "VB")) {
        return std::unique_ptr<IBackend>{new Rh850Backend{}};
    }
    std::string msg{"select_backend: no codegen backend for platform '"};
    msg.append(platform);
    msg.append("' (recognized: VA → sh2a, VB → rh850)");
    return failure(ErrorCode::UnsupportedVersion, std::move(msg));
}

Result<std::unique_ptr<IBackend>> select_backend(Definition const &def) {
    return select_backend(def.pack().platform);
}

} // namespace st::feature::codegen
