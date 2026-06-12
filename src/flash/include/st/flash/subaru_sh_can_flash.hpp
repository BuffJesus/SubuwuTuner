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

enum class SubaruEcuFamily : std::uint8_t {
    SH7058 = 0, // SSM-III era — older SH platform (2008–2014)
    SH2A   = 1, // SSM-IV era — LF79103P et al (2015–2021 VA WRX, FA20DIT)
    Rh850  = 2, // SSM-V / VI era — VB WRX (2022+)
};

enum class SubaruInitVariant : std::uint8_t {
    Factory     = 0, // OEM stock SA round keys
    CobbFlash   = 1, // COBB-installed tune ("COBB Flash" mode)
    CobbMafSd   = 2, // COBB MAF-based Speed-Density variant
    Aftermarket = 3, // Third-party / Fehr-active aftermarket framework
};

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
