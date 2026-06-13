// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::flash::SubaruShCanFlash — port of the AP-side Subaru ECU flash
// protocol (libFlashSubaru's `ecu::subaru::SH_CAN_Flash`) into a
// SubuwuTuner-native class, so eventually the orchestrator can flash
// ECUs directly via the OBD-II port without APManager + COBB AP as
// middleware.
//
// Reference architecture: `docs/37-subaru-flash-protocol.md` (clean-
// room sequence + signature summary). Spec draft:
// `findings/re-2026-06-12-pm/RE5_subaru_flash_spec_draft.md`.
// **ζ1 reframe** (`RE_wave6_findings.md`): Subaru's wire protocol is
// SSM byte 0xa5 + vtable-dispatched byte vectors, NOT UDS
// RoutineControl. The `Flasher::ecu_*` primitive set is generic-UDS-
// only and won't compose here; Tier B needs a parallel
// `Flasher::subaru_*` (or private SubaruShCanFlash) primitive layer
// that calls a `PacketExchange_ISO` equivalent. See docs/37 §"Wire
// protocol — ζ1 reframe" for the implementation roadmap.
//
// Two-step arming:
//   * Build flag `ST_ENABLE_SUBARU_ECU_FLASH` — OFF by default.
//     When OFF, every method returns PolicyDenied with a docs/37
//     pointer. The class still compiles and links so callers don't
//     need #ifdef guards.
//   * Runtime gate at the CLI / GUI surface (separate flag) is the
//     planned Tier-B addition once the bench rig validates the
//     sequence end-to-end.
//
// Tier shipped today: A (skeleton). Methods return NotImplemented
// when the build flag is ON; PolicyDenied when OFF. Tier B fills in
// the UDS sequence body per `docs/37`; Tier C is the bench-rig
// validation gate.

#ifndef ST_FLASH_SUBARU_SH_CAN_FLASH_HPP
#define ST_FLASH_SUBARU_SH_CAN_FLASH_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace st::transport {
class ITransport;
} // namespace st::transport

namespace st::flash {

// Subaru silicon family / SSM-era axis. Picks the SA round-key
// width (16-bit vs 32-bit per round-key), the F function's bit
// shuffling rules, and the wire seed/key byte width. Per
// docs/38-subaru-sa-variants.md §"The variant matrix".
//
// Numeric values aligned to the reference architecture's
// `e_FlashInitTypes` enum per ζ3 (the constructor arg to
// `SH_CAN_Flash` takes this as a 0..5 index, validated against the
// 6-value `cmp #6` upper bound in the disasm). The mapping is:
//   0 = SSM-II            (factory) → SH7055
//   1 = SSM-III           (factory) → SH7058
//   2 = SSM-III COBB                → SH7058 with CobbFlash variant
//   3 = SSM-IV            (factory) → SH2A
//   4 = SSM-IV  COBB                → SH2A with CobbFlash variant
//   5 = SSM-VI COBB                 → Rh850Vi with CobbFlash variant
// Our (family, variant) split is more granular — the helper
// `subaru_flash_init_type(family, variant)` below maps to the wire
// value when a Tier-B implementation needs to send it.
enum class SubaruEcuFamily : std::uint8_t {
    SH7055 = 0, // SSM-II era — pre-2008 (no aftermarket fleet on file)
    SH7058 = 1, // SSM-III era — older SH platform (2008–2014)
    SH2A   = 2, // SSM-IV era — LF79103P et al (2015–2021 VA WRX, FA20DIT)
    Rh850V = 3, // SSM-V era — VB WRX (2022+, initial RH850 ECUs)
    Rh850Vi = 4, // SSM-VI era — newer RH850 ECUs (2024+)
};

// Install-state axis. Picks the round-key table + S-box + Feistel
// direction together. Not every (family, variant) combo is real —
// see docs/38 §"The variant matrix" for the cells on file.
enum class SubaruInitVariant : std::uint8_t {
    Factory     = 0, // OEM stock SA round keys
    CobbFlash   = 1, // COBB-installed tune ("COBB Flash" mode)
    CobbMafSd   = 2, // COBB MAF-based Speed-Density variant
    Aftermarket = 3, // Third-party / aftermarket-active aftermarket framework
    EcuTek      = 4, // EcuTek-installed tune — factory tables + patched S-box
    SsmvFactory = 5, // SSM-V factory — reversed L35 round-key set
};

// `e_FlashInitTypes` per ζ3 — 6-value enum the reference
// architecture's `SH_CAN_Flash` constructor takes to pick the SA
// Init class internally. The implementer-side (family, variant)
// split is more granular; this enum is the wire-equivalent value
// the Tier-B SubaruShCanFlash will pass when constructing the
// reference-architecture SH_CAN_Flash. Values from the disasm's
// `cmp #6` upper bound + class taxonomy.
enum class SubaruFlashInitType : std::uint8_t {
    SSMII             = 0,
    SSMIII_Factory    = 1,
    SSMIII_COBB       = 2,
    SSMIV_Factory     = 3,
    SSMIV_COBB        = 4,
    SSMVI_COBB        = 5,
    Unknown           = 0xFF, // sentinel for combos with no wire mapping yet
};

// Map (family, variant) → wire SubaruFlashInitType. Returns
// SubaruFlashInitType::Unknown for combos the disasm enum doesn't
// cover (e.g. SSM-V factory, EcuTek, MAF-SD-as-distinct-from-CF).
[[nodiscard]] constexpr SubaruFlashInitType
subaru_flash_init_type(SubaruEcuFamily family,
                        SubaruInitVariant variant) noexcept;

// `e_ChallengeMode` per ζ3 — binary mode select inside
// `GetDecryptKeys`. Mode 1 takes the Direct path (the SA seed flows
// straight into the key derivation); Mode 0 takes the Construct
// path (the seed is composed with internal state before
// derivation). Subaru picks the mode per init class; the Tier-B
// SubaruShCanFlash will need to pass the right value when invoking
// the SA exchange.
enum class SubaruChallengeMode : std::uint8_t {
    Construct = 0, // composed-seed path
    Direct    = 1, // raw-seed path
};

// `SetFMATSMode` payload per ζ3 — 1-byte mode-select packet. FMATS
// likely stands for "Field MAintenance and Test Switch" — the
// programming-mode gate Subaru's field-service tooling uses. The
// per-mode byte values aren't disclosed in the analyst writeup;
// when Tier-B observes them on the bench rig, populate this enum
// (Unknown today).
enum class SubaruFmatsMode : std::uint8_t {
    Unknown = 0xFF, // placeholder; bench-rig observation fills in real values
};

// True when the (family, variant) combination is present in
// docs/38's matrix. Callers can use this to refuse impossible
// combinations early (e.g. EcuTek targeting RH850 isn't on file).
// Returns false for combos that have no known SA implementation;
// returns true for combos a Tier-B SubaruShCanFlash would need to
// support. Tier-A implementations always return PolicyDenied or
// NotImplemented regardless of this flag.
[[nodiscard]] constexpr bool
subaru_variant_supported(SubaruEcuFamily family,
                          SubaruInitVariant variant) noexcept {
    switch (family) {
    case SubaruEcuFamily::SH7055:
        // SSM-II: factory only on file. No aftermarket fleet
        // historically targets SSM-II — the install-base is too
        // small and the engine families are pre-modern-tuning.
        return variant == SubaruInitVariant::Factory;
    case SubaruEcuFamily::SH7058:
    case SubaruEcuFamily::SH2A:
        // SSM-III + SSM-IV: full variant set on file.
        return variant == SubaruInitVariant::Factory ||
               variant == SubaruInitVariant::CobbFlash ||
               variant == SubaruInitVariant::CobbMafSd ||
               variant == SubaruInitVariant::Aftermarket ||
               variant == SubaruInitVariant::EcuTek;
    case SubaruEcuFamily::Rh850V:
        // SSM-V: factory + the COBB pair + aftermarket. EcuTek
        // hasn't shipped Gen-B keys yet (per docs/38).
        return variant == SubaruInitVariant::Factory ||
               variant == SubaruInitVariant::SsmvFactory ||
               variant == SubaruInitVariant::CobbFlash ||
               variant == SubaruInitVariant::CobbMafSd ||
               variant == SubaruInitVariant::Aftermarket;
    case SubaruEcuFamily::Rh850Vi:
        // SSM-VI: COBB + aftermarket. No factory-key recovery on
        // file yet (would need a stock 2024+ ROM dump).
        return variant == SubaruInitVariant::CobbFlash ||
               variant == SubaruInitVariant::CobbMafSd ||
               variant == SubaruInitVariant::Aftermarket;
    }
    return false;
}

constexpr SubaruFlashInitType
subaru_flash_init_type(SubaruEcuFamily family,
                        SubaruInitVariant variant) noexcept {
    // Only the 6 cells the disasm enum covers map to a real wire
    // value. Everything else returns Unknown — Tier B will refuse
    // these combos at the SH_CAN_Flash construction site.
    switch (family) {
    case SubaruEcuFamily::SH7055:
        if (variant == SubaruInitVariant::Factory)
            return SubaruFlashInitType::SSMII;
        break;
    case SubaruEcuFamily::SH7058:
        if (variant == SubaruInitVariant::Factory)
            return SubaruFlashInitType::SSMIII_Factory;
        if (variant == SubaruInitVariant::CobbFlash ||
            variant == SubaruInitVariant::CobbMafSd)
            return SubaruFlashInitType::SSMIII_COBB;
        break;
    case SubaruEcuFamily::SH2A:
        if (variant == SubaruInitVariant::Factory)
            return SubaruFlashInitType::SSMIV_Factory;
        if (variant == SubaruInitVariant::CobbFlash ||
            variant == SubaruInitVariant::CobbMafSd)
            return SubaruFlashInitType::SSMIV_COBB;
        break;
    case SubaruEcuFamily::Rh850V:
    case SubaruEcuFamily::Rh850Vi:
        if (variant == SubaruInitVariant::CobbFlash ||
            variant == SubaruInitVariant::CobbMafSd)
            return SubaruFlashInitType::SSMVI_COBB;
        break;
    }
    return SubaruFlashInitType::Unknown;
}

struct SubaruEcuFlashParams {
    SubaruEcuFamily family{SubaruEcuFamily::SH2A};
    SubaruInitVariant variant{SubaruInitVariant::Factory};
    std::chrono::milliseconds tester_present_interval{2000};
    std::chrono::milliseconds flash_chunk_timeout{30000};
};

class SubaruShCanFlash {
public:
    SubaruShCanFlash(st::transport::ITransport &channel,
                     SubaruEcuFlashParams params) noexcept;

    // Connect + handshake — UDS DiagnosticSessionControl (0x10) into
    // extended/programming session and SecurityAccess (0x27) with the
    // round-key table the variant selects.
    [[nodiscard]] Status open();

    // Enter the Subaru-specific programming-mode routine
    // (RoutineControl 0x31 0x01 with routine_id 0xff00 per RE5).
    [[nodiscard]] Status enable_flash_mode();

    // Erase a logical block by index. Implemented via RoutineControl
    // 0x31 0x01 routine_id 0xff01 with the block index as the option
    // record. Block sizes are family-specific (1 KB on SH-2A, varies
    // elsewhere); callers consult the def pack for the boundary list.
    [[nodiscard]] Status erase_block(std::uint32_t block_idx);

    // Program a single block. Internally: RequestDownload (0x34) →
    // TransferData (0x36) chunks → RequestTransferExit (0x37).
    [[nodiscard]] Status flash_block(std::uint32_t block_idx,
                                     std::span<std::uint8_t const> bytes);

    // Ask the ECU to compute + return its own checksum over an
    // inclusive address range. RoutineControl 0x31 0x01 with the
    // platform-specific checksum routine ID. Distinct from the host-
    // side `st::flash::checksum::IChecksumRepair` because this is
    // POST-flash verification against the ECU's own view.
    [[nodiscard]] Result<std::uint32_t>
    checksum(std::uint32_t start, std::uint32_t end);

    // Read a contiguous address range via ReadMemoryByAddress (0x23)
    // chunked per the platform's response-buffer ceiling. Distinct
    // from `Flasher::read_full_rom` because this lives inside the
    // SubaruShCanFlash session lifecycle — opens / authenticates /
    // closes in one call.
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    dump_range(std::uint32_t start, std::uint32_t end);

    // Exit programming session — sends DiagnosticSessionControl
    // back to defaultSession (0x10 0x01), then ECUReset (0x11).
    [[nodiscard]] Status close();

    // Composite flash flow: open → enable_flash_mode → erase all
    // blocks the image touches → flash all blocks → checksum →
    // close. Progress callback fires after each block; `on_progress`
    // runs on the caller's thread.
    using ProgressFn =
        std::function<void(std::uint32_t bytes_done, std::uint32_t total)>;
    [[nodiscard]] Status flash_full_rom(std::span<std::uint8_t const> image,
                                        ProgressFn on_progress = nullptr);

private:
    st::transport::ITransport *channel_;
    SubaruEcuFlashParams params_;
};

} // namespace st::flash

#endif // ST_FLASH_SUBARU_SH_CAN_FLASH_HPP
