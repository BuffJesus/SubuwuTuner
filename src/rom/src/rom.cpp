// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/rom.hpp"

#include "st/core/crc32.hpp"
#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace st {

namespace {

// Subaru CIDs and most embedded strings are within printable ASCII. Treat
// 0x20..0x7E as printable.
constexpr bool is_printable_ascii(std::uint8_t b) noexcept {
    return b >= 0x20U && b <= 0x7EU;
}

} // namespace

Result<Rom> Rom::from_file(std::filesystem::path const &path, std::size_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return failure(ErrorCode::FileNotFound, path.string());
    }
    if (std::filesystem::is_directory(path, ec) || ec) {
        return failure(ErrorCode::InvalidArgument,
                       "expected a file, got a directory: " + path.string());
    }

    auto const size = std::filesystem::file_size(path, ec);
    if (ec) {
        return failure(ErrorCode::IoFailure, "could not stat: " + path.string());
    }
    if (size > max_bytes) {
        return failure(ErrorCode::OutOfRange,
                       "file exceeds max_bytes (" + std::to_string(size) + " > "
                           + std::to_string(max_bytes) + ")");
    }

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::PermissionDenied, "could not open: " + path.string());
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
            return failure(ErrorCode::UnexpectedEof,
                           "short read on: " + path.string());
        }
    }

    return Rom{std::move(bytes)};
}

Result<std::span<std::uint8_t const>> Rom::slice(std::size_t offset,
                                                 std::size_t length) const noexcept {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
        return failure(ErrorCode::OutOfRange);
    }
    return std::span<std::uint8_t const>{bytes_.data() + offset, length};
}

Result<std::uint8_t> Rom::read_u8(std::size_t offset) const noexcept {
    if (offset >= bytes_.size()) {
        return failure(ErrorCode::OutOfRange);
    }
    return bytes_[offset];
}

Result<std::uint16_t> Rom::read_u16_be(std::size_t offset) const noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 2) {
        return failure(ErrorCode::OutOfRange);
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[offset]) << 8U)
                                      | static_cast<std::uint16_t>(bytes_[offset + 1]));
}

Result<std::uint32_t> Rom::read_u32_be(std::size_t offset) const noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 4) {
        return failure(ErrorCode::OutOfRange);
    }
    return (static_cast<std::uint32_t>(bytes_[offset]) << 24U)
         | (static_cast<std::uint32_t>(bytes_[offset + 1]) << 16U)
         | (static_cast<std::uint32_t>(bytes_[offset + 2]) << 8U)
         | (static_cast<std::uint32_t>(bytes_[offset + 3]));
}

Result<std::uint16_t> Rom::read_u16_le(std::size_t offset) const noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 2) {
        return failure(ErrorCode::OutOfRange);
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[offset + 1]) << 8U)
                                      | static_cast<std::uint16_t>(bytes_[offset]));
}

Result<std::uint32_t> Rom::read_u32_le(std::size_t offset) const noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 4) {
        return failure(ErrorCode::OutOfRange);
    }
    return (static_cast<std::uint32_t>(bytes_[offset + 3]) << 24U)
         | (static_cast<std::uint32_t>(bytes_[offset + 2]) << 16U)
         | (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8U)
         | (static_cast<std::uint32_t>(bytes_[offset]));
}

Result<std::string> Rom::read_ascii(std::size_t offset, std::size_t max_length) const {
    if (offset > bytes_.size()) {
        return failure(ErrorCode::OutOfRange);
    }
    auto const available = bytes_.size() - offset;
    auto const limit     = max_length < available ? max_length : available;

    std::string out;
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        auto const b = bytes_[offset + i];
        if (b == 0) {
            break;
        }
        if (!is_printable_ascii(b)) {
            return failure(ErrorCode::ParseError,
                           "non-printable byte at offset " + std::to_string(offset + i));
        }
        out.push_back(static_cast<char>(b));
    }
    return out;
}

Status Rom::write_u8(std::size_t offset, std::uint8_t value) noexcept {
    if (offset >= bytes_.size()) {
        return failure(ErrorCode::OutOfRange);
    }
    bytes_[offset] = value;
    return ok();
}

Status Rom::write_u16_be(std::size_t offset, std::uint16_t value) noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 2) {
        return failure(ErrorCode::OutOfRange);
    }
    bytes_[offset]     = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes_[offset + 1] = static_cast<std::uint8_t>(value & 0xFFU);
    return ok();
}

Status Rom::write_u16_le(std::size_t offset, std::uint16_t value) noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 2) {
        return failure(ErrorCode::OutOfRange);
    }
    bytes_[offset]     = static_cast<std::uint8_t>(value & 0xFFU);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    return ok();
}

Status Rom::write_u32_be(std::size_t offset, std::uint32_t value) noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 4) {
        return failure(ErrorCode::OutOfRange);
    }
    bytes_[offset]     = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes_[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes_[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
    return ok();
}

Status Rom::write_u32_le(std::size_t offset, std::uint32_t value) noexcept {
    if (offset > bytes_.size() || bytes_.size() - offset < 4) {
        return failure(ErrorCode::OutOfRange);
    }
    bytes_[offset]     = static_cast<std::uint8_t>(value & 0xFFU);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes_[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes_[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    return ok();
}

std::uint32_t Rom::crc32() const noexcept {
    return st::crc32(std::span<std::uint8_t const>{bytes_});
}

std::vector<Rom::AsciiString> Rom::scan_ascii(std::size_t min_length) const {
    std::vector<AsciiString> out;
    std::size_t              run_start = 0;
    std::size_t              run_len   = 0;

    auto const flush = [&]() {
        if (run_len >= min_length) {
            AsciiString s;
            s.offset = run_start;
            s.text.assign(reinterpret_cast<char const *>(bytes_.data() + run_start), run_len);
            out.push_back(std::move(s));
        }
        run_len = 0;
    };

    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (is_printable_ascii(bytes_[i])) {
            if (run_len == 0) {
                run_start = i;
            }
            ++run_len;
        } else {
            flush();
        }
    }
    flush();
    return out;
}

} // namespace st
