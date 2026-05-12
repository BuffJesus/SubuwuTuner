// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubaruTuner Authors

#ifndef ST_ROM_HPP
#define ST_ROM_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace st {

// In-memory representation of a raw ECU image.
//
// Subaru ECUs are big-endian, so the multi-byte read primitives default to
// BE. LE variants are provided for completeness (e.g. inspecting non-Subaru
// blobs or definition metadata stored in LE).
//
// Bounds are checked: every read returns Result<T> and reports OutOfRange on
// a bad offset. This keeps fuzz harnesses simple — pass random data, the worst
// outcome is an Error.
class Rom {
  public:
    // Default maximum size accepted by `from_file`. Subaru WRX ROMs are ~1.5 MB;
    // 64 MB is a generous ceiling that still rejects clearly-garbage inputs.
    static constexpr std::size_t kDefaultMaxBytes = 64ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static Result<Rom> from_file(std::filesystem::path const &path,
                                               std::size_t max_bytes = kDefaultMaxBytes);

    [[nodiscard]] static Rom from_bytes(std::vector<std::uint8_t> bytes) {
        return Rom{std::move(bytes)};
    }

    [[nodiscard]] std::span<std::uint8_t const> data() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t                   size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool                          empty() const noexcept { return bytes_.empty(); }

    [[nodiscard]] Result<std::span<std::uint8_t const>> slice(
        std::size_t offset, std::size_t length) const noexcept;

    [[nodiscard]] Result<std::uint8_t>  read_u8(std::size_t offset) const noexcept;
    [[nodiscard]] Result<std::uint16_t> read_u16_be(std::size_t offset) const noexcept;
    [[nodiscard]] Result<std::uint32_t> read_u32_be(std::size_t offset) const noexcept;
    [[nodiscard]] Result<std::uint16_t> read_u16_le(std::size_t offset) const noexcept;
    [[nodiscard]] Result<std::uint32_t> read_u32_le(std::size_t offset) const noexcept;

    // ASCII string read at a fixed offset. Stops at first NUL or `max_length`,
    // whichever comes first. Non-printable bytes (other than terminating NUL)
    // produce a ParseError.
    [[nodiscard]] Result<std::string> read_ascii(std::size_t offset, std::size_t max_length) const;

    // Whole-ROM CRC32 (IEEE 802.3). Used for project integrity tracking, not
    // ECU-specific OEM checksums (those go in st::flash later).
    [[nodiscard]] std::uint32_t crc32() const noexcept;

    // Heuristic ASCII scanner. Returns runs of printable ASCII at least
    // `min_length` characters long. Useful for spotting calibration IDs and
    // other embedded strings without knowing exact offsets.
    struct AsciiString {
        std::size_t offset;
        std::string text;
    };
    [[nodiscard]] std::vector<AsciiString> scan_ascii(std::size_t min_length = 4) const;

  private:
    explicit Rom(std::vector<std::uint8_t> bytes) : bytes_{std::move(bytes)} {}

    std::vector<std::uint8_t> bytes_;
};

} // namespace st

#endif // ST_ROM_HPP
