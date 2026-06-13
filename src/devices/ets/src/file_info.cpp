// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/file_info.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string>
#include <vector>

namespace st::devices::ets {

namespace detail {

std::vector<std::uint8_t> encode_uleb128(std::uint64_t value) {
    std::vector<std::uint8_t> out;
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
    return out;
}

Result<UlebRead> read_uleb128(std::span<std::uint8_t const> bytes) {
    std::uint64_t value = 0;
    std::size_t shift = 0;
    std::size_t i = 0;
    for (; i < bytes.size(); ++i) {
        std::uint8_t const b = bytes[i];
        // 10-byte upper bound covers u64; anything longer is malformed.
        if (shift >= 64) {
            return st::failure(st::ErrorCode::ParseError,
                               "ap3::read_uleb128: overflow past 64 bits");
        }
        value |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        shift += 7;
        if ((b & 0x80U) == 0) {
            UlebRead r{};
            r.value = value;
            r.bytes_consumed = i + 1;
            return r;
        }
    }
    return st::failure(st::ErrorCode::UnexpectedEof,
                       "ap3::read_uleb128: ran off the end of the buffer");
}

} // namespace detail

namespace {

void append_le_u32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (unsigned i = 0; i < 4U; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8U)) & 0xFFU));
    }
}

Result<std::uint32_t> read_le_u32(std::span<std::uint8_t const> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::read_le_u32: truncated buffer");
    }
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
        v |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return v;
}

} // namespace

Result<std::vector<std::uint8_t>> encode_file_info(FileInfo const &info) {
    if (info.mtime > 0xFFFFFFFFULL) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "ap3::encode_file_info: mtime overflows u32 (post-2106)");
    }
    if (info.size > 0xFFFFFFFFULL) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "ap3::encode_file_info: size overflows u32 (>4 GiB) — "
                           "AP firmware reads the metadata size field as u32 LE");
    }
    std::vector<std::uint8_t> out;
    out.reserve(kBoostEnvelope.size() + kFileInfo2ClassReg.size() + 4 + info.name.size() +
                kMetadataSize + 4 + info.path.size());
    // 35-byte envelope + 2-byte class registration. Field order
    // inside the FileInfo2 is `name → metadata → path` (NOT
    // name/path/metadata as the older spec §7 prose described).
    out.insert(out.end(), kBoostEnvelope.begin(), kBoostEnvelope.end());
    out.insert(out.end(), kFileInfo2ClassReg.begin(), kFileInfo2ClassReg.end());
    // u32 LE name_len + name bytes.
    append_le_u32(out, static_cast<std::uint32_t>(info.name.size()));
    out.insert(out.end(), info.name.begin(), info.name.end());
    // 27-byte metadata: u32 LE mtime + u32 LE size + 19 zero bytes.
    append_le_u32(out, static_cast<std::uint32_t>(info.mtime));
    append_le_u32(out, static_cast<std::uint32_t>(info.size));
    out.insert(out.end(), 19U, std::uint8_t{0});
    // u32 LE path_len + path bytes.
    append_le_u32(out, static_cast<std::uint32_t>(info.path.size()));
    out.insert(out.end(), info.path.begin(), info.path.end());
    return out;
}

Result<std::vector<std::uint8_t>> encode_string_body(std::string_view s) {
    std::vector<std::uint8_t> out;
    out.reserve(kBoostEnvelope.size() + 4 + s.size());
    out.insert(out.end(), kBoostEnvelope.begin(), kBoostEnvelope.end());
    append_le_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

Result<FileInfo> decode_file_info(std::span<std::uint8_t const> body) {
    constexpr std::size_t kHeaderSize = kBoostEnvelope.size() + kFileInfo2ClassReg.size();
    if (body.size() < kHeaderSize + 4U + kMetadataSize + 4U) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::decode_file_info: shorter than minimum FileInfo2 body");
    }
    if (!std::equal(kBoostEnvelope.begin(), kBoostEnvelope.end(), body.begin())) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_file_info: 35-byte request envelope mismatch");
    }
    if (body[kBoostEnvelope.size()] != kFileInfo2ClassReg[0] ||
        body[kBoostEnvelope.size() + 1] != kFileInfo2ClassReg[1]) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::decode_file_info: class registration mismatch");
    }
    std::size_t off = kHeaderSize;
    // u32 LE name_len + name bytes.
    auto name_len = read_le_u32(body, off);
    if (!name_len.has_value()) {
        return st::failure(std::move(name_len).error());
    }
    off += 4U;
    if (off + *name_len > body.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::decode_file_info: name truncated");
    }
    std::string name(reinterpret_cast<char const *>(body.data() + off),
                     static_cast<std::size_t>(*name_len));
    off += *name_len;
    // 27-byte metadata.
    if (off + kMetadataSize > body.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::decode_file_info: metadata truncated");
    }
    auto mtime = read_le_u32(body, off);
    if (!mtime.has_value()) {
        return st::failure(std::move(mtime).error());
    }
    auto size = read_le_u32(body, off + 4U);
    if (!size.has_value()) {
        return st::failure(std::move(size).error());
    }
    off += kMetadataSize;
    // u32 LE path_len + path bytes.
    auto path_len = read_le_u32(body, off);
    if (!path_len.has_value()) {
        return st::failure(std::move(path_len).error());
    }
    off += 4U;
    if (off + *path_len > body.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::decode_file_info: path truncated");
    }
    std::string path(reinterpret_cast<char const *>(body.data() + off),
                     static_cast<std::size_t>(*path_len));
    off += *path_len;

    FileInfo info{};
    info.name = std::move(name);
    info.path = std::move(path);
    info.mtime = *mtime;
    info.size = *size;
    // The 19-byte metadata tail is opaque; surface the first 11 in
    // the legacy flags slot for diagnostic dumps, leave the rest
    // unreported.
    std::memcpy(info.flags.data(),
                body.data() + kHeaderSize + 4U + *name_len + 8U,
                info.flags.size());
    return info;
}

Result<std::vector<FileInfo>>
decode_file_info_list(std::span<std::uint8_t const> body) {
    // Vector<FileInfo2> wire format per spec §7 (revised 2026-06-11 PM).
    // Critically DIFFERENT from the single-FileInfo2 layout that
    // decode_file_info handles:
    //
    //   [0..29]    30-byte FileInfo2 prefix (same magic+config)
    //   [30]       u8 archive flag = 0x08
    //   [31..34]   u32 LE tracking ID = 1
    //   [35..38]   u32 LE = 0 (per-archive sentinel)
    //   [39..42]   u32 LE record count
    //   per-record (count times):
    //     [u32 LE flag/prefix]   — Boost tracking, accept any value
    //     [u32 LE name_len]      — NOT uleb128 in this carrier
    //     [name bytes]
    //     [27 bytes metadata]    — mtime u64 LE + size u64 LE + 11 flags
    //     [u32 LE path_len]
    //     [path bytes]
    //
    // The field order is `name → metadata → path` here, NOT
    // `name → path → metadata` as in the single-record case. And
    // string lengths are u32 LE, NOT uleb128. Spec §7's §"Two
    // critical differences" subsection makes this explicit; first
    // observed 2026-06-11 against a captured cmd 0x26 response for
    // /presets/ (3 records).
    if (body.size() < kFileInfo2Prefix.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3 decode_file_info_list: shorter than archive prefix");
    }
    if (!std::equal(kBoostArchivePrefix.begin(), kBoostArchivePrefix.end(), body.begin())) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 decode_file_info_list: 'serialization::archive' magic mismatch");
    }
    std::size_t off = kFileInfo2Prefix.size();
    // Carrier framing between prefix and first record per the canonical
    // 3-record /presets/ capture (`fileinfo2_list_response_known_good.bin`):
    //
    //   [30]      u8 archive flag (= 0x08)
    //   [31]      u8 (= 0x01; first-class-tracking marker)
    //   [32..33]  u16 (= 0)
    //   [34..37]  u32 BE record count
    //   [38..41]  u32 BE (= 0; per-archive sentinel)
    //   [42]      u8 first-record-occurrence flag (= 0x01 for record 0)
    //   [43..]    records
    //
    // 13-byte total carrier framing. The analyst's spec §7 prose
    // labels these as "u32 LE" at adjacent offsets — empirically they
    // are u32 BE and the offsets shift by one byte. The record count
    // appears at body[34..37] as u32 BE (= 3 in the canonical fixture).
    // String-length prefixes INSIDE each record ARE u32 LE.
    constexpr std::size_t kCarrierFraming = 13U; // body[30..42] inclusive
    if (off + kCarrierFraming > body.size()) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3 decode_file_info_list: short of record-count header");
    }
    // u32 BE record count at body[34..37] = body[off+4..off+7].
    std::uint32_t const count =
        (static_cast<std::uint32_t>(body[off + 4]) << 24) |
        (static_cast<std::uint32_t>(body[off + 5]) << 16) |
        (static_cast<std::uint32_t>(body[off + 6]) << 8) |
        static_cast<std::uint32_t>(body[off + 7]);
    off += kCarrierFraming;
    // Sanity-cap count so a corrupted u32 (~4 billion) can't
    // allocate-bomb us. 1 million records is well above what any
    // AP could legitimately store. The canonical /presets/ capture
    // has 3 records — typical real values are O(10)-O(100).
    if (count > 1'000'000U) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 decode_file_info_list: implausible record count " +
                               std::to_string(count));
    }

    std::vector<FileInfo> records;
    records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        // Per-record layout from the canonical /presets/ capture:
        //   [u32 LE name_len][name][metadata][u32 LE path_len][path]
        //
        // Metadata size is FIRST-RECORD DEPENDENT (an artefact of
        // Boost.Serialization's class-tracking: the first instance of
        // a class in the archive carries 2 extra trailing bytes for
        // the class-version marker):
        //   record 0:   27 bytes (= 4 mtime + 4 size + 17 flags + 2 class-tracking)
        //   records 1+: 25 bytes (= 4 mtime + 4 size + 17 flags)
        //
        // Verified empirically against `fileinfo2_list_response_known_good.bin`
        // (3 records totaling 308 body bytes after stripping wire header).
        std::size_t const metadata_size = (i == 0) ? 27U : 25U;
        auto name_len = read_le_u32(body, off);
        if (!name_len.has_value()) {
            return st::failure(std::move(name_len).error());
        }
        off += 4U;
        if (off + name_len.value() > body.size()) {
            return st::failure(st::ErrorCode::UnexpectedEof,
                               "ap3 decode_file_info_list: name truncated");
        }
        std::string name(reinterpret_cast<char const *>(body.data() + off),
                         static_cast<std::size_t>(*name_len));
        off += *name_len;
        // Metadata: u32 LE mtime + u32 LE size + variable trailing
        // flags/padding (17 bytes flags for all records; record 0
        // additionally carries 2 bytes of class-tracking-version at
        // the very end).
        if (off + metadata_size > body.size()) {
            return st::failure(st::ErrorCode::UnexpectedEof,
                               "ap3 decode_file_info_list: metadata truncated");
        }
        std::uint64_t const mtime =
            static_cast<std::uint64_t>(body[off]) |
            (static_cast<std::uint64_t>(body[off + 1]) << 8U) |
            (static_cast<std::uint64_t>(body[off + 2]) << 16U) |
            (static_cast<std::uint64_t>(body[off + 3]) << 24U);
        std::uint64_t const size =
            static_cast<std::uint64_t>(body[off + 4]) |
            (static_cast<std::uint64_t>(body[off + 5]) << 8U) |
            (static_cast<std::uint64_t>(body[off + 6]) << 16U) |
            (static_cast<std::uint64_t>(body[off + 7]) << 24U);
        std::array<std::uint8_t, 11> flags{};
        std::memcpy(flags.data(), body.data() + off + 8U, 11U);
        off += metadata_size;
        // [u32 LE path_len]
        auto path_len = read_le_u32(body, off);
        if (!path_len.has_value()) {
            return st::failure(std::move(path_len).error());
        }
        off += 4U;
        if (off + path_len.value() > body.size()) {
            return st::failure(st::ErrorCode::UnexpectedEof,
                               "ap3 decode_file_info_list: path truncated");
        }
        std::string path(reinterpret_cast<char const *>(body.data() + off),
                         static_cast<std::size_t>(*path_len));
        off += *path_len;

        FileInfo info{};
        info.name = std::move(name);
        info.path = std::move(path);
        info.mtime = mtime;
        info.size = size;
        info.flags = flags;
        records.push_back(std::move(info));
    }
    return records;
}

Result<FileInfo>
decode_setup_ack_response_shape(std::span<std::uint8_t const> body) {
    // Layout (see HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation
    // for the byte-level derivation):
    //   [0..29]   30-byte FileInfo2 prefix (same as kFileInfo2Prefix)
    //   [30..36]  7-byte fixed block 08 01 00 00 00 00 01
    //   [37..40]  u32 LE name_len
    //   [41..]    name
    //   next 4    u32 LE mtime
    //   next 4    u32 LE size
    //   next 19   metadata flags (zero)
    //   next 4    u32 LE path_len
    //   next N    path
    constexpr std::size_t kPrefixLen = kFileInfo2Prefix.size(); // 30
    constexpr std::size_t kFixedBlockLen = 7;
    constexpr std::size_t kMinHeader = kPrefixLen + kFixedBlockLen + 4U;
    if (body.size() < kMinHeader) {
        return failure(ErrorCode::UnexpectedEof,
                       "ap3 decode_setup_ack_response_shape: body too short");
    }
    if (!std::equal(kFileInfo2Prefix.begin(), kFileInfo2Prefix.begin() + 27,
                    body.begin())) {
        return failure(ErrorCode::ParseError,
                       "ap3 decode_setup_ack_response_shape: prefix magic mismatch");
    }
    std::size_t off = kPrefixLen + kFixedBlockLen;

    auto read_u32_le = [&](std::size_t at) -> Result<std::uint32_t> {
        if (at + 4U > body.size()) {
            return failure(ErrorCode::UnexpectedEof,
                           "ap3 decode_setup_ack_response_shape: truncated u32");
        }
        return static_cast<std::uint32_t>(body[at]) |
               (static_cast<std::uint32_t>(body[at + 1]) << 8U) |
               (static_cast<std::uint32_t>(body[at + 2]) << 16U) |
               (static_cast<std::uint32_t>(body[at + 3]) << 24U);
    };

    auto name_len = read_u32_le(off);
    if (!name_len.has_value()) {
        return failure(name_len.error());
    }
    off += 4U;
    if (*name_len > 4096U || off + *name_len > body.size()) {
        return failure(ErrorCode::ParseError,
                       "ap3 decode_setup_ack_response_shape: name_len " +
                           std::to_string(*name_len) + " implausible");
    }
    FileInfo out;
    out.name.assign(reinterpret_cast<char const *>(body.data() + off), *name_len);
    off += *name_len;

    auto mtime = read_u32_le(off);
    if (!mtime.has_value()) {
        return failure(mtime.error());
    }
    out.mtime = *mtime;
    off += 4U;

    auto size = read_u32_le(off);
    if (!size.has_value()) {
        return failure(size.error());
    }
    out.size = *size;
    off += 4U;

    // 19-byte metadata flags block.
    constexpr std::size_t kFlagsLen = 19U;
    if (off + kFlagsLen > body.size()) {
        return failure(ErrorCode::UnexpectedEof,
                       "ap3 decode_setup_ack_response_shape: flags truncated");
    }
    // Copy the first 11 bytes of the 19-byte block into FileInfo.flags
    // for parity with the request-shape decoder; the trailing 8 bytes
    // are observed to be zero in every capture so far.
    std::memcpy(out.flags.data(), body.data() + off, out.flags.size());
    off += kFlagsLen;

    auto path_len = read_u32_le(off);
    if (!path_len.has_value()) {
        return failure(path_len.error());
    }
    off += 4U;
    if (*path_len > 4096U || off + *path_len > body.size()) {
        return failure(ErrorCode::ParseError,
                       "ap3 decode_setup_ack_response_shape: path_len " +
                           std::to_string(*path_len) + " implausible");
    }
    out.path.assign(reinterpret_cast<char const *>(body.data() + off), *path_len);
    return out;
}

} // namespace st::devices::ets
