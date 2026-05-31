// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::feature::codegen — IR-to-machine-code backends for custom features.
//
// Sits downstream of `st::feature::ir::Module` (the typed dataflow IR
// produced by lower(Graph)). Each ECU family has its own backend:
//
//   sh2a   — Renesas SH-2A (VA WRX et al.)
//   rh850  — Renesas RH850 (VB WRX et al.)
//
// This header is the public seam: the IBackend interface, the
// architecture-agnostic PatchObject that backends emit, the per-hook
// RamAllocator that backends share, and the select_backend() factory
// that picks the right backend by the loaded definition pack's
// platform.
//
// Coverage today: SH-2A is full (LoadConstant, LoadHookInput,
// CallPrimitive over the add_int/select primitive set, all wired
// through StoreHookOutput). RH850 covers all three IR shapes — the
// CallPrimitive slice currently supports add_int, subtract_int,
// and_bool, or_bool, not_bool with leaf operands (LoadConstant /
// LoadHookInput); nested primitives and the remaining primitive set
// (multiply/divide, compares, select, Float/FPU) land in follow-up
// bundles. Per docs/16 §"Architectural fit", we never need to *parse*
// SH-2A or RH850 — only emit.

#ifndef ST_FEATURE_CODEGEN_HPP
#define ST_FEATURE_CODEGEN_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/feature_ir.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::feature::codegen {

// The ECU instruction-set families this layer can target. `Unknown` is
// the "no backend selected" sentinel — `select_backend` returns it
// inside the Result error when the pack's platform field doesn't
// resolve.
enum class Arch : std::uint8_t {
    Unknown,
    Sh2a,
    Rh850,
};

[[nodiscard]] char const *arch_name(Arch a) noexcept;
[[nodiscard]] std::string_view arch_name_sv(Arch a) noexcept;

// One absolute-address byte allocation inside a hook's free_ram
// region. Backends emit one of these per scratch variable they need
// (intermediate SSA values, primitive temporaries). The RAM allocator
// guarantees no two claims overlap and stays within the hook's
// declared bounds.
struct RamClaim {
    std::size_t address{0};   // absolute ROM/RAM address
    std::size_t size{0};      // bytes
    std::size_t alignment{1}; // bytes the address must be a multiple of
};

// RamAllocator — bump allocator over a hook's free_ram region. Tracks
// the high-water cursor; each `claim` rounds up to alignment then
// reserves `size` bytes. Refuses on overflow past the region's bounds.
//
// Reusable across both backends — neither SH-2A nor RH850 need
// anything more sophisticated for v1.x (no reuse of freed slots —
// custom-feature lifetimes are bounded by one hook invocation so
// reset() between hooks is enough).
class RamAllocator {
public:
    // Construct over `[base, base + length)`. `length == 0` is a valid
    // "no scratch RAM available" allocator that refuses every claim.
    RamAllocator(std::size_t base, std::size_t length) noexcept;

    // Reserve `size` bytes, aligned to `alignment` (must be a power of
    // two; values <2 are treated as 1). Returns the claim on success,
    // OutOfRange on overflow, InvalidArgument on a non-power-of-two
    // alignment.
    [[nodiscard]] Result<RamClaim> claim(std::size_t size, std::size_t alignment = 1) noexcept;

    // Reset the cursor — useful between hook code-gen passes when each
    // hook owns its own RAM region in the same allocator instance.
    void reset() noexcept;

    [[nodiscard]] std::size_t base() const noexcept {
        return base_;
    }
    [[nodiscard]] std::size_t length() const noexcept {
        return length_;
    }
    [[nodiscard]] std::size_t used() const noexcept {
        return cursor_ - base_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return base_ + length_ - cursor_;
    }

private:
    std::size_t base_{0};
    std::size_t length_{0};
    std::size_t cursor_{0};
};

// One compiled hook — the bytes the patch loader will splice into the
// firmware at `splice_address`. Symbol identifies the hook by its
// pack-declared id (e.g. "after_fuel_calc") so the patch loader can
// resolve splice_address out of the current definition pack.
struct HookPatch {
    std::string symbol;
    std::size_t splice_address{0};
    std::vector<std::uint8_t> code;
    std::vector<RamClaim> ram_claims;
};

// Output of `IBackend::compile`. One PatchObject per compiled
// `.stmod` — holds the target arch and one HookPatch per distinct
// hook the user's graph wrote to. Generic enough that both backends
// can emit the same shape; downstream patch-insertion (free-RAM map,
// hook table, vector-table splicing — `src/feature_patch/` once it
// lands) consumes this without caring which backend produced it.
struct PatchObject {
    Arch arch{Arch::Unknown};
    std::vector<HookPatch> hooks;
};

// Pure-virtual backend interface. One concrete impl per target ISA.
// Backends are stateless across calls — `compile` reads the IR and
// produces a PatchObject; any per-call state is local.
class IBackend {
public:
    IBackend() = default;
    virtual ~IBackend() = default;
    IBackend(IBackend const &) = delete;
    IBackend &operator=(IBackend const &) = delete;
    IBackend(IBackend &&) noexcept = delete;
    IBackend &operator=(IBackend &&) noexcept = delete;

    [[nodiscard]] virtual Arch arch() const noexcept = 0;

    // Compile a lowered IR module into a target-architecture
    // PatchObject. The Definition supplies hook splice addresses,
    // free_ram regions, and primitive signatures. Backends return
    // NotImplemented for any IR shape they don't yet cover (see the
    // per-backend coverage notes in the file banner).
    [[nodiscard]] virtual Result<PatchObject> compile(ir::Module const &m,
                                                      Definition const &def) = 0;
};

// SH-2A backend for VA WRX (et al.). Full coverage today: LoadConstant,
// LoadHookInput, CallPrimitive over the add_int/select primitive set,
// all wired through StoreHookOutput.
class Sh2aBackend final : public IBackend {
public:
    [[nodiscard]] Arch arch() const noexcept override {
        return Arch::Sh2a;
    }
    [[nodiscard]] Result<PatchObject> compile(ir::Module const &m, Definition const &def) override;
};

// RH850 backend for VB WRX (et al.). Partial coverage today: all three
// IR shapes are wired — LoadConstant→Store, LoadHookInput→Store, and
// CallPrimitive→Store. The CallPrimitive slice currently recognizes
// add_int, subtract_int, and_bool, or_bool, and not_bool with leaf
// operands; multiply/divide, Int compares, select, Float/FPU
// primitives, and nested CallPrimitive operands all still return
// NotImplemented.
class Rh850Backend final : public IBackend {
public:
    [[nodiscard]] Arch arch() const noexcept override {
        return Arch::Rh850;
    }
    [[nodiscard]] Result<PatchObject> compile(ir::Module const &m, Definition const &def) override;
};

// Public layout constants for the SH-2A emission shapes the codegen
// produces today. Exposed so the patch-insertion layer and tests can
// reference offsets without re-hardcoding.
//
// LoadConstant → StoreHookOutput (`kStoreSequenceSize`):
//   5 instructions + 1 instruction of pool-alignment padding +
//   2 longwords of literal pool = 20 bytes.
//
// LoadHookInput → StoreHookOutput (`kLoadStoreSequenceSize`):
//   6 instructions (no pad — already 4-aligned) + 2 longwords of
//   literal pool = 20 bytes. Same total size, different shape.
//
// Both shapes terminate with RTS + NOP (delay slot) and place the
// 4-byte-aligned literal pool last.
namespace sh2a {
inline constexpr std::size_t kStoreSequenceSize = 20;
inline constexpr std::size_t kStoreLiteralPoolOffset = 12;
inline constexpr std::size_t kStoreLiteralStride = 4;

inline constexpr std::size_t kLoadStoreSequenceSize = 20;
inline constexpr std::size_t kLoadStoreLiteralPoolOffset = 12;
inline constexpr std::size_t kLoadStoreLiteralStride = 4;
} // namespace sh2a

// RH850 layout constants for the LoadConstant→StoreHookOutput slice.
// Mirrors the sh2a namespace shape so the patch-insertion layer can
// query either backend's emission size without backend-specific code.
//
// Shape: 5 × 32-bit Format VI/VII instructions + 1 × 16-bit Format I
// JMP + 1 × 16-bit NOP pad = 24 bytes. See src/feature_codegen/src/
// rh850.hpp for the per-byte breakdown.
//
// CAUTION (mirrored from rh850.hpp): RH850 codegen has NOT been
// validated against a real VB WRX ECU as of v1.x. Treat any PatchObject
// with `arch == Rh850` as best-effort until the bench rig signs off.
namespace rh850 {
inline constexpr std::size_t kStoreSequenceSize = 24;
inline constexpr std::size_t kJmpOffset = 20;

// LoadHookInput → StoreHookOutput slice. 28 bytes — 6 × 32-bit
// instructions (MOVHI+MOVEA pair for source addr, LD.W, MOVHI+MOVEA
// pair for dest addr, ST.W) + 2 × 16-bit (JMP [lp], NOP pad). The
// extra 4 bytes vs kStoreSequenceSize come from materializing the
// source address (separate register so the LD.W doesn't clobber it).
// JMP lands at offset 24.
inline constexpr std::size_t kLoadStoreSequenceSize = 28;
inline constexpr std::size_t kLoadStoreJmpOffset = 24;

// CallPrimitive binary leaf-operand slice — used by add_int,
// subtract_int, and_bool, or_bool (anything that's a Format-I 16-bit
// reg-reg op over two materialized operands). Patch size depends on
// operand kinds: each Constant operand needs MOVHI+MOVEA (8 bytes);
// each HookInputPointer operand needs MOVHI+MOVEA + LD.W (12 bytes).
// The rest is fixed: 2-byte op + 2-byte alignment NOP + 8-byte MOVHI+
// MOVEA for destination addr + 4-byte ST.W + 2-byte JMP + 2-byte tail
// NOP = 20 bytes overhead. `not_bool` is unary but XORs against a
// loaded constant 1, so it lands on the same size envelope.
//
//   kBinaryIntArithSizeCC : both operands Constant
//   kBinaryIntArithSizeCH : op1 Constant, op2 HookInputPointer
//   kBinaryIntArithSizeHC : op1 HookInputPointer, op2 Constant
//   kBinaryIntArithSizeHH : both HookInputPointer
//
// All four are 4-aligned.
inline constexpr std::size_t kBinaryIntArithSizeCC = 36;
inline constexpr std::size_t kBinaryIntArithSizeCH = 40;
inline constexpr std::size_t kBinaryIntArithSizeHC = 40;
inline constexpr std::size_t kBinaryIntArithSizeHH = 44;
} // namespace rh850

// Address gate per docs/16 §Safety #6 + docs/04 ship blocker #3.
//
// Verifies that every byte each HookPatch in `p` would write to flash
// falls inside one (single) declared `[[writable_region]]` of `def`.
// The byte range checked is `[splice_address, splice_address + code.size())`.
// A hook is rejected if it spans two regions — even if the union of
// regions covers the range, a single contiguous region must contain it.
// (Spanning would imply the patch silently bridges two declared-safe
// zones, which is exactly the foot-gun the gate exists to prevent.)
//
// Fails closed: a Definition with zero `[[writable_region]]` entries
// rejects every patch. Codegen against such a pack cannot emit anything.
// This is a security boundary, not a sanity check; silent fail-open
// would let a buggy or malicious graph reach `st::flash` with an
// arbitrary target address through the same pipeline table edits use.
//
// `kind` is informational for v1.0 — the gate checks containment
// regardless of kind. Per-kind policy (e.g. production-fleet profiles
// refusing patches into `code` regions) is a future bundle.
//
// Returns ok() if every hook fits; InvalidArgument with a multi-line
// message naming the offending hook(s), the offending byte range, and
// the available regions otherwise. The check is total — the message
// reports every violation, not just the first.
[[nodiscard]] Status gate_patch(PatchObject const &p, Definition const &def);

// Pick the right backend by the loaded pack's platform field. v1.0
// recognizes "VA" → Sh2a, "VB" → Rh850; anything else is
// UnsupportedVersion (it's the closest existing ErrorCode — we're
// declining the pack, not failing on a malformed one). Returns a
// fresh heap-allocated backend; ownership transfers to the caller.
[[nodiscard]] Result<std::unique_ptr<IBackend>> select_backend(Definition const &def);

// Same as above but driven by the platform string directly. Used by
// tests and any caller that already has the string in hand.
[[nodiscard]] Result<std::unique_ptr<IBackend>> select_backend(std::string_view platform);

// PatchObject ↔ TOML round-trip. `patch_to_toml` emits a single
// top-level `[patch]` table plus one `[[patch.hook]]` per hook, each
// with its `[[patch.hook.ram_claim]]` nested array. Code bytes go in
// as one uppercase hex string with no separators. `patch_from_toml`
// scans a TOML text for `[patch]` and reconstructs the object; absent
// table → `std::nullopt`; malformed → ParseError.
//
// Designed for distribution: a `.stmod` file is a TOML document
// holding `[graph]` + `[[node]]` + `[[edge]]` for the source graph
// AND `[patch]` for the compiled bytes. The graph loader
// (`feature::from_toml`) ignores `[patch]`; this loader ignores
// everything outside it. Callers parse both to roundtrip the whole
// file.
[[nodiscard]] std::string patch_to_toml(PatchObject const &p);
[[nodiscard]] Result<std::optional<PatchObject>> patch_from_toml(std::string_view text);

} // namespace st::feature::codegen

#endif // ST_FEATURE_CODEGEN_HPP
