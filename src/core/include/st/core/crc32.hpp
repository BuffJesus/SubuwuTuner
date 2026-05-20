// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_CRC32_HPP
#define ST_CORE_CRC32_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace st {

namespace detail {

inline constexpr auto kCrc32Table = []() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}();

} // namespace detail

// Streaming CRC32 (IEEE 802.3). The "final" value xors the running state with
// 0xFFFFFFFF, which is the convention used by zlib, gzip, PNG, and the canonical
// "123456789" → 0xCBF43926 test vector.
class Crc32 {
public:
    constexpr void update(std::span<std::uint8_t const> data) noexcept {
        std::uint32_t c = state_;
        for (auto const b : data) {
            c = detail::kCrc32Table[(c ^ b) & 0xFFU] ^ (c >> 8U);
        }
        state_ = c;
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return state_ ^ 0xFFFFFFFFU;
    }

    constexpr void reset() noexcept {
        state_ = 0xFFFFFFFFU;
    }

private:
    std::uint32_t state_{0xFFFFFFFFU};
};

[[nodiscard]] constexpr std::uint32_t crc32(std::span<std::uint8_t const> data) noexcept {
    Crc32 c;
    c.update(data);
    return c.value();
}

} // namespace st

#endif // ST_CORE_CRC32_HPP
