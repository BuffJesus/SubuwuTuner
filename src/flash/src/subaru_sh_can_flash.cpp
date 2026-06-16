// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash/subaru_sh_can_flash.hpp"

#include "st/core/error.hpp"

#if defined(ST_SUBARU_ECU_FLASH_ENABLED)
#include "st/ecu/ssm.hpp"
#endif

namespace st::flash {

namespace {

#if !defined(ST_SUBARU_ECU_FLASH_ENABLED)
constexpr char const *kGateOffMessage =
    "st::flash::SubaruShCanFlash: build flag ST_ENABLE_SUBARU_ECU_FLASH is "
    "OFF (default). Direct ECU flashing via OBD-II without APManager is a "
    "two-step opt-in; see docs/37-subaru-flash-protocol.md.";
#endif

#if defined(ST_SUBARU_ECU_FLASH_ENABLED)
constexpr char const *kNotImplementedMessage =
    "st::flash::SubaruShCanFlash: Tier A skeleton only — the UDS sequence "
    "body is unimplemented pending bench-rig validation. See docs/37 + "
    "findings/re-2026-06-12-pm/RE5_subaru_flash_spec_draft.md.";

// SSM command byte for EnableFlashMode, per ζ1 reframe
// (findings/re-2026-06-12-pm/RE_wave6_findings.md). The reference
// architecture's vmethod call is `vmethod(this, 0xa5, 5)` — the
// 0xA5 byte is the SSM cmd, the 5 is the vmethod's internal
// timeout/retry parameter (NOT a wire byte).
constexpr std::uint8_t kSsmEnableFlashMode = 0xA5;
#endif

} // namespace

SubaruShCanFlash::SubaruShCanFlash(st::transport::ITransport &channel,
                                   SubaruEcuFlashParams params) noexcept
    : channel_{&channel}, params_{params} {}

#if defined(ST_SUBARU_ECU_FLASH_ENABLED)

Status SubaruShCanFlash::open() {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Status SubaruShCanFlash::enable_flash_mode() {
    // ζ1 reframe (docs/37 §"Wire protocol"): Subaru's EnableFlashMode is
    // NOT UDS DSC 0x10 0x02 or RoutineControl 0xFF00. It's SSM cmd byte
    // 0xA5 sent through the inherited PacketExchange_ISO vmethod. The
    // payload is empty — the "5" in the reference disasm
    // `mov r2, #5; mov r1, #0xa5; blx vmethod` is a vmethod-internal
    // timeout/retry knob, not a wire byte.
    //
    // The SsmClient::send_byte_command primitive sits exactly at the
    // PacketExchange_ISO layer; on IsoTp framing it emits the bare
    // `A5` byte over the configured CAN ID. The response ACK is
    // `0xE5` (0xA5 | 0x40) per standard SSM convention; the wrapper
    // strips that and surfaces any remaining body bytes (empty for
    // EnableFlashMode positive).
    //
    // VA/VB Subaru chassis (every 2008+ FA/EJ/FB ECU) run CAN ISO-TP;
    // K-Line is pre-2008 only. The Tier-B path is CAN-only for now;
    // K-Line targets would need a separate framing pick here.
    st::ecu::ssm::SsmClient ssm{*channel_, st::ecu::ssm::Framing::IsoTp};
    auto const r = ssm.send_byte_command(
        kSsmEnableFlashMode, std::span<std::uint8_t const>{},
        params_.flash_chunk_timeout);
    if (!r.has_value()) {
        return st::failure(r.error());
    }
    return st::ok();
}

Status SubaruShCanFlash::erase_block(std::uint32_t /*block_idx*/) {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Status SubaruShCanFlash::flash_block(std::uint32_t /*block_idx*/,
                                     std::span<std::uint8_t const> /*bytes*/) {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Result<std::uint32_t> SubaruShCanFlash::checksum(std::uint32_t /*start*/,
                                                  std::uint32_t /*end*/) {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Result<std::vector<std::uint8_t>>
SubaruShCanFlash::dump_range(std::uint32_t /*start*/, std::uint32_t /*end*/) {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Status SubaruShCanFlash::close() {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

Status SubaruShCanFlash::flash_full_rom(
    std::span<std::uint8_t const> /*image*/, ProgressFn /*on_progress*/) {
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
}

#else // ST_SUBARU_ECU_FLASH_ENABLED

Status SubaruShCanFlash::open() {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Status SubaruShCanFlash::enable_flash_mode() {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Status SubaruShCanFlash::erase_block(std::uint32_t /*block_idx*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Status SubaruShCanFlash::flash_block(std::uint32_t /*block_idx*/,
                                     std::span<std::uint8_t const> /*bytes*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Result<std::uint32_t> SubaruShCanFlash::checksum(std::uint32_t /*start*/,
                                                  std::uint32_t /*end*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Result<std::vector<std::uint8_t>>
SubaruShCanFlash::dump_range(std::uint32_t /*start*/, std::uint32_t /*end*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Status SubaruShCanFlash::close() {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Status SubaruShCanFlash::flash_full_rom(
    std::span<std::uint8_t const> /*image*/, ProgressFn /*on_progress*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

#endif // ST_SUBARU_ECU_FLASH_ENABLED

} // namespace st::flash
