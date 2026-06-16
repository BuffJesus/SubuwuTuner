// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/subaru_atlas.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace st::ecu::subaru_atlas {

namespace {

// Bench capture from LF79002P (FA20DIT SH-2A 2 MB) donor ECU,
// 2026-06-16. SHA of donor ROM:
//   5fb404e70beb912224e8b141be8cc2be20cda1965d129002305aef5c116d0d11
// Wire sequence sent: AA (single byte). Wire sequence received:
// EA <104 B>. The 104 B body is stored here.
constexpr std::array<std::uint8_t, kFlashInformationBodyBytes>
    kReferenceBodyLf79002p = {
        0xA3, 0x10, 0x0F, 0xB0, 0x29, 0xB0, 0x40, 0x07, 0x41, 0x80, 0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

} // namespace

std::span<std::uint8_t const> reference_flash_information_body_lf79002p() noexcept {
    return {kReferenceBodyLf79002p.data(), kReferenceBodyLf79002p.size()};
}

std::string AtlasFlashInformationResponse::hex_dump() const {
    std::string out;
    out.reserve(raw.size() * 3U);
    char buf[4];
    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", raw[i]);
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(buf[0]);
        out.push_back(buf[1]);
    }
    return out;
}

bool AtlasFlashInformationResponse::matches_reference_lf79002p() const noexcept {
    if (raw.size() != kReferenceBodyLf79002p.size()) {
        return false;
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != kReferenceBodyLf79002p[i]) {
            return false;
        }
    }
    return true;
}

std::array<std::uint8_t, kDeviceIdPrefixBytes>
AtlasFlashInformationResponse::device_id_prefix() const noexcept {
    std::array<std::uint8_t, kDeviceIdPrefixBytes> out{};
    std::size_t const n = std::min(raw.size(), kDeviceIdPrefixBytes);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = raw[i];
    }
    return out;
}

bool AtlasFlashInformationResponse::has_lf79002p_primary_prefix() const noexcept {
    if (raw.size() < kDeviceIdPrefixBytes) {
        return false;
    }
    for (std::size_t i = 0; i < kDeviceIdPrefixBytes; ++i) {
        if (raw[i] != kDeviceIdPrefixLf79002pPrimary[i]) {
            return false;
        }
    }
    return true;
}

Result<AtlasFlashInformationResponse>
parse_flash_information_body(std::span<std::uint8_t const> body) {
    if (body.size() != kFlashInformationBodyBytes) {
        return failure(ErrorCode::InvalidArgument,
                       "atlas FlashInformation body must be 104 B");
    }

    AtlasFlashInformationResponse r;
    r.raw.assign(body.begin(), body.end());

    r.status_flag = body[0];
    r.field_a = static_cast<std::uint16_t>((body[1] << 8) | body[2]);
    r.field_b = static_cast<std::uint32_t>(body[3]) << 24 |
                static_cast<std::uint32_t>(body[4]) << 16 |
                static_cast<std::uint32_t>(body[5]) << 8 |
                static_cast<std::uint32_t>(body[6]);
    r.field_c = body[7];
    r.field_d = body[8];
    r.marker_pair_0[0] = body[9];
    r.marker_pair_0[1] = body[10];
    r.marker_offset_40 = body[40];
    r.marker_offset_48 = body[48];
    r.marker_offset_55 = body[55];

    return r;
}

Result<AtlasFlashInformationResponse>
parse_flash_information_frame(std::span<std::uint8_t const> frame) {
    if (frame.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "atlas FlashInformation frame is empty");
    }

    if (frame[0] == 0x7FU) {
        std::uint8_t nrc = frame.size() >= 3 ? frame[2] : 0U;
        char msg[64];
        std::snprintf(msg, sizeof(msg),
                      "atlas FlashInformation NRC 0x%02X", nrc);
        return failure(ErrorCode::EcuRejected, msg);
    }

    if (frame[0] != kAckAtlasProprietary) {
        char msg[80];
        std::snprintf(msg, sizeof(msg),
                      "atlas FlashInformation expected ACK 0xEA, got 0x%02X",
                      frame[0]);
        return failure(ErrorCode::InvalidArgument, msg);
    }

    if (frame.size() != kFlashInformationBodyBytes + 1U) {
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "atlas FlashInformation frame must be 105 B (1 ACK + 104), got %zu",
                      frame.size());
        return failure(ErrorCode::InvalidArgument, msg);
    }

    return parse_flash_information_body(frame.subspan(1));
}

} // namespace st::ecu::subaru_atlas
