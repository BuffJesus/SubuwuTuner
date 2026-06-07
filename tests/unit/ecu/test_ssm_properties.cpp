// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Property-based tests for the SSM framing layer — ship blocker #11.
//
// Example-based coverage (test_ssm.cpp) exercises specific hand-written
// frames derived from the SSM wire-format spec and live bus capture.
// Property tests cover the encode/parse pair across the input space:
// any address set, any data size within spec → request frame is
// well-formed, response parse round-trips.
//
// Properties pinned here:
//
//   1. ssm_checksum(bytes) is the byte-wise sum mod 256, verified
//      against an independent reference.
//
//   2. build_a8_request produces a frame with the documented header
//      (80 10 F0), declared LEN matches actual payload length, and
//      the appended checksum verifies. (No way to "decode" a request
//      back, so the property is structural.)
//
//   3. A8 response round-trip: build a synthetic positive response
//      for the expected data bytes; parse_a8_response returns those
//      bytes intact.
//
//   4. B0 (single-byte write) round-trip: build_b0_request encodes
//      address + data byte; parse_b0_response on a synthetic echo
//      returns the echo byte.
//
//   5. B8 (block write) round-trip: same shape as B0 but with N
//      bytes; parse_b8_response returns the N-byte echo intact.
//
//   6. Address bounds: any address > 24-bit (`kMaxAddress`) is
//      rejected at build time.
//
//   7. Response parser on adversarial random bytes never crashes,
//      always returns a clean ParseError on malformed input.

#include "../_helpers/property.hpp"

#include "st/core/error.hpp"
#include "st/ecu/ssm.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace ssm = st::ecu::ssm;

namespace {

[[nodiscard]] std::uint8_t reference_checksum(std::span<std::uint8_t const> bytes) noexcept {
    std::uint32_t sum = 0;
    for (auto b : bytes) {
        sum += b;
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

// Build a synthetic positive A8 response frame for `data` bytes —
// matches what the ECU sends back when an A8 read succeeds.
//   80 F0 10 [LEN] E8 [data...] [CSUM]
//   LEN = 1 (RSP) + data.size()
//
// Init-list + insert avoids GCC 15's `-Werror=free-nonheap-object`
// false-positive on reserve+push_back chains (same pattern as
// `st::ecu::uds::build_wdbi_request` in src/ecu/src/uds.cpp:90).
[[nodiscard]] std::vector<std::uint8_t>
build_a8_response(std::span<std::uint8_t const> data) {
    std::vector<std::uint8_t> frame{
        ssm::kHeader,
        ssm::kSrcTool,
        ssm::kDestEcu,
        static_cast<std::uint8_t>(1U + data.size()),
        ssm::kRespReadByAddress,
    };
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(reference_checksum(frame));
    return frame;
}

// Build a synthetic positive B0 response: ECU echoes the written byte.
//   80 F0 10 02 F0 [data] [CSUM]
[[nodiscard]] std::vector<std::uint8_t> build_b0_response(std::uint8_t echo) {
    std::vector<std::uint8_t> frame{
        ssm::kHeader,
        ssm::kSrcTool,
        ssm::kDestEcu,
        0x02U, // LEN = 1 (RSP) + 1 (data)
        ssm::kRespWriteByAddress,
        echo,
    };
    frame.push_back(reference_checksum(frame));
    return frame;
}

// Build a synthetic positive B8 response: ECU echoes all N written bytes.
//   80 F0 10 [LEN] F8 [data...] [CSUM]
[[nodiscard]] std::vector<std::uint8_t>
build_b8_response(std::span<std::uint8_t const> echo) {
    std::vector<std::uint8_t> frame{
        ssm::kHeader,
        ssm::kSrcTool,
        ssm::kDestEcu,
        static_cast<std::uint8_t>(1U + echo.size()),
        ssm::kRespBlockWrite,
    };
    frame.insert(frame.end(), echo.begin(), echo.end());
    frame.push_back(reference_checksum(frame));
    return frame;
}

// Random vector of 24-bit-valid addresses.
[[nodiscard]] std::vector<std::uint32_t> random_addresses(st::test::PropertyRng &rng,
                                                          std::size_t lo, std::size_t hi) {
    auto const n = rng.size_in(lo, hi);
    std::vector<std::uint32_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(rng.uint32_in(0U, ssm::kMaxAddress));
    }
    return out;
}

} // namespace

TEST_CASE("property: ssm_checksum is byte-sum modulo 256",
          "[ecu][ssm][property][checksum]") {
    st::test::check_property(0x55AA'55AA'A55A'A55AULL, 500, [&](st::test::PropertyRng &rng) {
        auto const bytes = rng.bytes(0, 512);
        REQUIRE(ssm::ssm_checksum(bytes) == reference_checksum(bytes));
    });
}

TEST_CASE("property: build_a8_request produces a well-formed frame",
          "[ecu][ssm][property][a8]") {
    st::test::check_property(0xA8A8'A8A8'A8A8'A8A8ULL, 300, [&](st::test::PropertyRng &rng) {
        // Multi-address read. SSM frame LEN is 1 byte so payload
        // (CMD + PAD + 3*N) must fit in 255 — capping N at 80
        // leaves headroom for the LEN byte. (Stricter cap than the
        // formal limit so the test never bumps the boundary; bounds
        // tests below exercise the cap directly.)
        auto const addrs = random_addresses(rng, 1, 80);
        auto const enc = ssm::build_a8_request(addrs);
        REQUIRE(enc.has_value());
        auto const &frame = *enc;

        // Structural invariants per docs/11 §SSM framing.
        REQUIRE(frame.size() >= 6); // header(3) + LEN + CMD + PAD + CSUM, before addrs
        REQUIRE(frame[0] == ssm::kHeader);
        REQUIRE(frame[1] == ssm::kDestEcu);
        REQUIRE(frame[2] == ssm::kSrcTool);
        std::size_t const declared_len = frame[3];
        // Frame total = 4 (header+LEN) + declared_len + 1 (CSUM)
        REQUIRE(frame.size() == 4U + declared_len + 1U);
        REQUIRE(frame[4] == ssm::kCmdReadByAddress);
        REQUIRE(frame[5] == ssm::kPadByte);
        // CSUM verifies.
        std::uint8_t const expected_chk =
            reference_checksum(std::span<std::uint8_t const>{frame.data(), frame.size() - 1});
        REQUIRE(frame.back() == expected_chk);
    });
}

TEST_CASE("property: A8 read round-trips through build/parse via synthetic response",
          "[ecu][ssm][property][a8][roundtrip]") {
    st::test::check_property(0xA800'A800'F00D'F00DULL, 300, [&](st::test::PropertyRng &rng) {
        auto const addrs = random_addresses(rng, 1, 80);
        auto const enc = ssm::build_a8_request(addrs);
        REQUIRE(enc.has_value());
        // Synthesize what the ECU would send back: one data byte per
        // requested address (the read is per-address).
        auto const data = rng.bytes(addrs.size(), addrs.size());
        auto const resp = build_a8_response(data);
        auto const parsed = ssm::parse_a8_response(resp, addrs.size());
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == data);
    });
}

TEST_CASE("property: B0 write round-trips through build/parse via synthetic response",
          "[ecu][ssm][property][b0][roundtrip]") {
    st::test::check_property(0xB000'B000'C0DE'C0DEULL, 400, [&](st::test::PropertyRng &rng) {
        auto const addr = rng.uint32_in(0U, ssm::kMaxAddress);
        auto const data = rng.byte();
        auto const enc = ssm::build_b0_request(addr, data);
        REQUIRE(enc.has_value());
        // Structural sanity: 80 dst src LEN B0 addr_hi addr_med addr_lo data CSUM = 10 bytes
        REQUIRE(enc->size() == 10);
        REQUIRE((*enc)[0] == ssm::kHeader);
        REQUIRE((*enc)[4] == ssm::kCmdWriteByAddress);
        // Address big-endian across the three address bytes.
        REQUIRE((*enc)[5] == static_cast<std::uint8_t>((addr >> 16U) & 0xFFU));
        REQUIRE((*enc)[6] == static_cast<std::uint8_t>((addr >> 8U) & 0xFFU));
        REQUIRE((*enc)[7] == static_cast<std::uint8_t>(addr & 0xFFU));
        REQUIRE((*enc)[8] == data);
        REQUIRE((*enc)[9] == reference_checksum(
                                 std::span<std::uint8_t const>{enc->data(), enc->size() - 1}));

        auto const resp = build_b0_response(data);
        auto const parsed = ssm::parse_b0_response(resp);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == data);
    });
}

TEST_CASE("property: B8 block write round-trips through build/parse via synthetic response",
          "[ecu][ssm][property][b8][roundtrip]") {
    st::test::check_property(0xB888'B888'BEEF'BEEFULL, 300, [&](st::test::PropertyRng &rng) {
        auto const addr = rng.uint32_in(0U, ssm::kMaxAddress);
        // N >= 1; cap below the formal max so address+N-1 still fits 24 bits.
        // We also need addr + N - 1 <= kMaxAddress; pick conservatively.
        auto const max_n = std::min<std::size_t>(ssm::kMaxBlockWriteBytes,
                                                 (ssm::kMaxAddress - addr) + 1);
        auto const n = rng.size_in(1, std::min<std::size_t>(max_n, 100));
        auto const data = rng.bytes(n, n);
        auto const enc = ssm::build_b8_request(addr, data);
        REQUIRE(enc.has_value());

        // Structural: 4 (header+LEN) + 1 (CMD) + 3 (addr) + N + 1 (CSUM)
        REQUIRE(enc->size() == 9U + n);
        REQUIRE((*enc)[4] == ssm::kCmdBlockWrite);

        auto const resp = build_b8_response(data);
        auto const parsed = ssm::parse_b8_response(resp, n);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == data);
    });
}

TEST_CASE("property: build_*_request rejects address > 24-bit",
          "[ecu][ssm][property][bounds]") {
    st::test::check_property(0x2400'2400'BAD0'BAD0ULL, 200, [&](st::test::PropertyRng &rng) {
        // Pick an address with at least one bit set above 24-bit.
        auto const high = rng.uint32_in(1U, 0xFFU); // bits 24-31
        auto const low = rng.uint32_in(0U, ssm::kMaxAddress);
        auto const addr = (high << 24U) | low;
        REQUIRE(addr > ssm::kMaxAddress);

        // A8: build with that single oversize address.
        auto const a8 = ssm::build_a8_request(std::array<std::uint32_t, 1>{addr});
        REQUIRE_FALSE(a8.has_value());
        REQUIRE(a8.error().code() == st::ErrorCode::InvalidArgument);

        // B0: same oversize.
        auto const b0 = ssm::build_b0_request(addr, 0x42U);
        REQUIRE_FALSE(b0.has_value());
        REQUIRE(b0.error().code() == st::ErrorCode::InvalidArgument);

        // B8: same.
        std::array<std::uint8_t, 1> dummy{0x42U};
        auto const b8 = ssm::build_b8_request(addr, dummy);
        REQUIRE_FALSE(b8.has_value());
        REQUIRE(b8.error().code() == st::ErrorCode::InvalidArgument);
    });
}

TEST_CASE("property: parse_a8_response rejects checksum corruption",
          "[ecu][ssm][property][a8][checksum]") {
    st::test::check_property(0xC0DE'C0DE'BAD0'BAD0ULL, 400, [&](st::test::PropertyRng &rng) {
        auto const n = rng.size_in(1, 80);
        auto const data = rng.bytes(n, n);
        auto resp = build_a8_response(data);

        // Flip the checksum to anything ≠ original.
        std::uint8_t delta = 0;
        while (delta == 0U) {
            delta = rng.byte();
        }
        resp.back() ^= delta;

        auto const parsed = ssm::parse_a8_response(resp, n);
        REQUIRE_FALSE(parsed.has_value());
        // SSM parsers distinguish structural errors (ParseError) from
        // checksum mismatches (BadChecksum). A flipped CSUM byte
        // surfaces as BadChecksum specifically; pinning that here
        // catches a regression where the parser conflates them.
        REQUIRE(parsed.error().code() == st::ErrorCode::BadChecksum);
    });
}

TEST_CASE("property: parse_*_response on random bytes never crashes",
          "[ecu][ssm][property][robustness]") {
    // Adversarial fuzz: random buffers across the three SSM parsers.
    // The parsers must return a clean error on malformed input,
    // never abort / throw / read past the input span. Acceptable
    // error codes are ParseError (structural) and BadChecksum
    // (frame structure ok, checksum byte didn't verify).
    auto const acceptable = [](st::ErrorCode c) {
        return c == st::ErrorCode::ParseError || c == st::ErrorCode::BadChecksum;
    };
    st::test::check_property(0x600D'F00D'BAD0'F00DULL, 2000, [&](st::test::PropertyRng &rng) {
        auto const bytes = rng.bytes(0, 64);
        // Pick a parser at random + a plausible expected_n.
        std::uint32_t const which = rng.uint32_in(0, 2);
        auto const expected_n = rng.size_in(0, 32);
        switch (which) {
        case 0: {
            auto const r = ssm::parse_a8_response(bytes, expected_n);
            if (!r.has_value()) {
                REQUIRE(acceptable(r.error().code()));
            }
            break;
        }
        case 1: {
            auto const r = ssm::parse_b0_response(bytes);
            if (!r.has_value()) {
                REQUIRE(acceptable(r.error().code()));
            }
            break;
        }
        case 2: {
            auto const r = ssm::parse_b8_response(bytes, expected_n);
            if (!r.has_value()) {
                REQUIRE(acceptable(r.error().code()));
            }
            break;
        }
        }
    });
}
