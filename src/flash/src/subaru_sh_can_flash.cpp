// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash/subaru_sh_can_flash.hpp"

#include "st/core/error.hpp"

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
    return st::failure(st::ErrorCode::NotImplemented, kNotImplementedMessage);
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
