// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ap3::FileInfo — wire codec for the FileInfo2 record
// (and the single-string carrier) exchanged by the AP3's modern
// file-vault commands.
//
// REQUEST shape (host → AP) is a Boost.Serialization binary archive
// with a 35-byte common envelope (27-byte magic + 8-byte format /
// objcount tail). What follows depends on the cmd byte:
//
//   * cmd 0x20 ReadFile setup, 0x22 PutFile setup, 0x25 RemoveFile —
//     carry a FileInfo2: 2-byte class registration + u32 LE name_len
//     + name + 27-byte metadata (u32 LE mtime + u32 LE size + 19 zero
//     bytes) + u32 LE path_len + path. Field order is
//     `name → metadata → path`.
//
//   * cmd 0x21 GetFile (step 2), 0x26 ListFiles — carry a single
//     Boost-archived UTF-8 string: u32 LE str_len + str bytes.
//
// RESPONSE shape from the AP is structurally different (uses a
// 30-byte prefix, no class registration, varied per-cmd footers); the
// response decoders below pin those shapes separately from the
// request encoders.
//
// The earlier "uleb128 / 30-byte-prefix / name-path-metadata" layout
// in spec §7 was an artifact of synthesizing a fixture from prose
// rather than capturing wire bytes — see the 2026-06-11 PM analyst
// handoff and `specs/fixtures/ap3/REQUEST_FIXTURES_README.md`. The
// fixtures `cmd20_*` / `cmd22_*` / `cmd25_*` / `cmd26_*` are now the
// source of truth for the request format.
//
// Spec: D:\Subuwu\specs\references\cobb-ap3-usb-protocol.md §7 and
// the §12.2/§12.3 test vectors.

#ifndef ST_DEVICES_AP3_FILE_INFO_HPP
#define ST_DEVICES_AP3_FILE_INFO_HPP

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::devices::ap3 {

// 27-byte serialization::archive magic that opens every Boost-archived
// body the AP3 produces or accepts (spec §12.3). All FileInfo2 bodies
// AND the cmd 0x28 probe body begin with these bytes; what follows
// differs by carrier (see kFileInfo2Prefix and kCmd28ProbeBody below).
inline constexpr std::array<std::uint8_t, 27> kBoostArchivePrefix{{
    0x16, 0x00, 0x00, 0x00, 0x73, 0x65, 0x72, 0x69, // u32 LE length=22 "seri"
    0x61, 0x6C, 0x69, 0x7A, 0x61, 0x74, 0x69, 0x6F, // "alizatio"
    0x6E, 0x3A, 0x3A, 0x61, 0x72, 0x63, 0x68, 0x69, // "n::archi"
    0x76, 0x65, 0x03,                               // "ve" + version=3
}};

// 30-byte response-side prefix used by AP-emitted bodies (cmd 0x28
// UserInfo response per spec §6.13, vector<FileInfo2> response from
// cmd 0x26). 27-byte magic + 3 archive-config zero bytes. This is NOT
// what host-side requests use — they use kBoostEnvelope below.
inline constexpr std::array<std::uint8_t, 30> kFileInfo2Prefix{{
    0x16, 0x00, 0x00, 0x00, 0x73, 0x65, 0x72, 0x69,
    0x61, 0x6C, 0x69, 0x7A, 0x61, 0x74, 0x69, 0x6F,
    0x6E, 0x3A, 0x3A, 0x61, 0x72, 0x63, 0x68, 0x69,
    0x76, 0x65, 0x03,
    0x00, 0x00, 0x00, // archive-config tail for FileInfo2 response carriers
}};

// 35-byte request-side envelope that opens every host→AP body for the
// file-vault commands (cmd 0x20 / 0x21 / 0x22 / 0x25 / 0x26). 27-byte
// magic + 5-byte Boost format constants `03 04 04 04 08` + u32 LE
// objcount = 1. Pinned against the analyst's captured-known-good
// request fixtures (`specs/fixtures/ap3/cmd2*_request_known_good.bin`).
inline constexpr std::array<std::uint8_t, 35> kBoostEnvelope{{
    0x16, 0x00, 0x00, 0x00, 0x73, 0x65, 0x72, 0x69,
    0x61, 0x6C, 0x69, 0x7A, 0x61, 0x74, 0x69, 0x6F,
    0x6E, 0x3A, 0x3A, 0x61, 0x72, 0x63, 0x68, 0x69,
    0x76, 0x65, 0x03,
    0x04, 0x04, 0x04, 0x08,             // Boost format constants
    0x01, 0x00, 0x00, 0x00,             // u32 LE objcount = 1
}};

// 2-byte FileInfo2 class registration sentinel that follows the
// kBoostEnvelope for cmd 0x20 / 0x22 / 0x25 (FileInfo2-bearing
// requests). The string-bearing carriers (cmd 0x21, cmd 0x26) skip
// this and proceed directly to the u32 LE string length.
inline constexpr std::array<std::uint8_t, 2> kFileInfo2ClassReg{{
    0x00, 0x01,
}};

// 39-byte body the cmd 0x28 (UserInfo) host probe sends — 27-byte
// magic + 12-byte library-version / class-tracking tail captured
// verbatim from spec §12.2. This shape carries no FileInfo2 record;
// it is the empty-body probe envelope. Bit-identical reconstruction
// of §12.2 is asserted in tests/unit/transport/test_cobb_ap_packet.cpp
// against the fixture at fixtures/ap3/packet_cmd28_userinfo.bin.
inline constexpr std::array<std::uint8_t, 39> kCmd28ProbeBody{{
    0x16, 0x00, 0x00, 0x00, 0x73, 0x65, 0x72, 0x69,
    0x61, 0x6C, 0x69, 0x7A, 0x61, 0x74, 0x69, 0x6F,
    0x6E, 0x3A, 0x3A, 0x61, 0x72, 0x63, 0x68, 0x69,
    0x76, 0x65, 0x03,
    0x04, 0x04, 0x04, 0x08, 0x01, 0x00, 0x00, 0x00, // cmd 0x28-specific tail
    0x00, 0x00, 0x00, 0x00                          //  (per spec §12.2)
}};

// FileInfo2 metadata footer in REQUEST shape is a 27-byte block:
// u32 LE mtime (Unix epoch seconds), u32 LE size (byte count), 19
// trailing zero bytes. The earlier u64-mtime + u64-size + 11-flag
// layout from spec §7 was incorrect — the AP firmware reads the
// metadata as (u32 mtime, u32 size, 19 opaque), so any modern
// timestamp's high 32 bits would leak into the "size" field.
inline constexpr std::size_t kMetadataSize = 27;

struct FileInfo {
    std::string name;
    std::string path;
    std::uint64_t mtime{0};
    std::uint64_t size{0};
    std::array<std::uint8_t, 11> flags{};
};

// Encode a single FileInfo2 record wrapped in the 35-byte Boost
// envelope and 2-byte class registration. Emits the complete body for
// cmd 0x20 / 0x22 / 0x25 — the caller hands this directly to
// encode_packet(cmd, body); there is NO leading path_type byte (that
// was the spec-§7 misread the request-fixture handoff corrected).
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_file_info(FileInfo const &info);

// Encode a single Boost-archived UTF-8 string body. Used by cmd 0x21
// (GetFile step 2: session-identifier string) and cmd 0x26 (ListFiles:
// directory name like "maps" / "presets" / "datalog" / "images").
// Shape: 35-byte envelope + u32 LE str_len + str bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_string_body(std::string_view s);

// Decode a single FileInfo2 record from a REQUEST-shaped body (35-byte
// envelope + 2-byte class reg + name + 27-byte metadata + path).
// Symmetric to encode_file_info; useful for the test round-trip.
// Response-shape FileInfo2 records (from cmd 0x20 setup ACKs and
// similar) use a different prefix; for those, see
// decode_setup_ack_response_shape below.

// Decode the cmd 0x20 setup-ACK response body. The AP firmware's
// echo has a different layout than the request: 30-byte FileInfo2
// prefix, then a 7-byte fixed block `08 01 00 00 00 00 01`, then
// FileInfo2 fields (u32 LE name_len + name + u32 LE mtime + u32 LE
// size + 19-byte flags + u32 LE path_len + path).
//
// Empirically decoded live 2026-06-12 — see
// findings/handoffs/HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation.md
// for the byte-level decode. The AP returns name="." and size=0 for
// files in subdirectories (a documented firmware path-resolution bug);
// callers check `name == "."` to fail fast before a degenerate cmd 0x21
// hang.
[[nodiscard]] Result<FileInfo>
decode_setup_ack_response_shape(std::span<std::uint8_t const> body);
[[nodiscard]] Result<FileInfo> decode_file_info(std::span<std::uint8_t const> body);

// Decode the ListFiles (cmd 0x26) RESPONSE body: 30-byte prefix +
// 13-byte carrier framing + count FileInfo2 records with
// per-first-record-extra metadata bytes. Pinned against the canonical
// /presets/ capture at fixtures/ap3/fileinfo2_list_response_known_good.bin.
[[nodiscard]] Result<std::vector<FileInfo>>
decode_file_info_list(std::span<std::uint8_t const> body);

namespace detail {

// uleb128 little-endian encoding/decoding. The AP3 spec calls this
// out for the per-string lengths inside FileInfo2 (§7). Exposed for
// testing.
[[nodiscard]] std::vector<std::uint8_t> encode_uleb128(std::uint64_t value);

struct UlebRead {
    std::uint64_t value{0};
    std::size_t bytes_consumed{0};
};

[[nodiscard]] Result<UlebRead> read_uleb128(std::span<std::uint8_t const> bytes);

} // namespace detail

} // namespace st::devices::ap3

#endif // ST_DEVICES_AP3_FILE_INFO_HPP
