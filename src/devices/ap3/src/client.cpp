// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/client.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/devices/ap3/file_info.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/cobb_ap_packet.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::devices::ap3 {

namespace {

// Strip a leading '/' from an AP-vault path. For cmd 0x22 (PutFile)
// and cmd 0x25 (RemoveFile) the AP firmware expects the request
// FileInfo2.path to be the full destination relative to the vault
// root WITHOUT a leading slash — e.g. "maps/test.ptm", not
// "/maps/test.ptm". Verified against `cmd22_putfile_setup_request_known_good.bin`
// and `cmd25_removefile_request_known_good.bin`.
std::string strip_leading_slash(std::string_view s) {
    if (!s.empty() && s.front() == '/') {
        return std::string{s.substr(1)};
    }
    return std::string{s};
}

// Diagnostic fallback for cmd 0x21 ReadFile DATA — off by default.
// The strict path (receive_packet_body) reads exactly wire_len bytes
// from the response header. On 2026-06-12 the live AP advertised
// wire_len = 5500 for a 58 KB .ptm pull and then never sent the body
// (see findings/handoffs/HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation.md).
// The reference Python tool that DID pull the same file end-to-end on
// 2026-06-10 (findings/for-dan/ap3-toolkit/ap_pull_state.py) uses an
// idle-driven accumulator instead: 512-byte reads in a loop, stopping
// after 800ms of bus silence. ST_AP3_READFILE_DRAIN_MODE=1 swaps in
// that strategy for the cmd 0x21 step only — the implementer can A/B
// against the strict path when live hardware is available.
bool readfile_drain_mode_enabled() noexcept {
    static int cached = -1;
    if (cached == -1) {
        char const *v = std::getenv("ST_AP3_READFILE_DRAIN_MODE");
        cached = (v != nullptr && std::string{v} == "1") ? 1 : 0;
    }
    return cached == 1;
}

} // namespace

// Per spec §6.13 (revised 2026-06-11 PM): cmd 0x28 (UserInfo) response
// body shape is:
//
//   [0..29]    30-byte FileInfo2-style prefix
//   [30]       u8 archive flag = 0x08
//   [31..34]   u32 LE tracking ID = 1
//   [35..38]   u32 LE string length
//   [39..]     ASCII string of that many bytes, 6 newline-separated
//              fields in order: firmware / product code / serial /
//              install state / vehicle / trailing 5-digit.
UserInfoFields parse_user_info_body(std::span<std::uint8_t const> body) {
    UserInfoFields out;
    constexpr std::size_t kFraming = 30U + 1U + 4U + 4U; // prefix + flag + tracking + len
    if (body.size() < kFraming) {
        return out;
    }
    // The 30-byte FileInfo2 prefix is the leading magic + 3-byte
    // archive-config; validate the leading 27 bytes of magic, the
    // analyst spec doesn't require strict validation of the trailing
    // 3 bytes.
    if (!std::equal(kFileInfo2Prefix.begin(), kFileInfo2Prefix.begin() + 27,
                    body.begin())) {
        return out;
    }
    std::size_t off = 30U;
    // [30] = 0x08 archive flag (skip)
    ++off;
    // [31..34] u32 LE tracking ID (skip)
    off += 4U;
    // [35..38] u32 LE string length
    std::uint32_t const str_len = static_cast<std::uint32_t>(body[off]) |
                                  (static_cast<std::uint32_t>(body[off + 1]) << 8) |
                                  (static_cast<std::uint32_t>(body[off + 2]) << 16) |
                                  (static_cast<std::uint32_t>(body[off + 3]) << 24);
    off += 4U;
    if (off + str_len > body.size()) {
        return out;
    }
    if (str_len > 4096U) {
        // Sanity cap — a malformed response shouldn't allocate huge.
        return out;
    }
    std::string_view const blob{reinterpret_cast<char const *>(body.data() + off),
                                static_cast<std::size_t>(str_len)};
    // Split on '\n' into up to 6 fields.
    std::array<std::string_view, 6> fields{};
    std::size_t field_idx = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= blob.size() && field_idx < fields.size(); ++i) {
        if (i == blob.size() || blob[i] == '\n') {
            fields[field_idx++] = blob.substr(start, i - start);
            start = i + 1;
        }
    }
    auto to_string_opt = [](std::string_view sv) -> std::optional<std::string> {
        if (sv.empty()) {
            return std::nullopt;
        }
        return std::string{sv};
    };
    out.firmware_version = to_string_opt(fields[0]);
    out.ap_product_code = to_string_opt(fields[1]);
    out.ap_serial = to_string_opt(fields[2]);
    if (!fields[3].empty()) {
        out.married = (fields[3] == "Installed");
    }
    out.vehicle = to_string_opt(fields[4]);
    return out;
}

std::optional<bool>
parse_marriage_from_device_settings(std::span<std::uint8_t const> body) {
    // PLACEHOLDER — analyst RE task per spec §15. The cmd 0x03
    // DeviceSettings response is a Boost-archived blob with an
    // Installed / Not Installed marker somewhere in the payload;
    // exact byte offset is the open question.
    //
    // Drop-in shape, when the offset lands:
    //
    //   constexpr std::size_t kMarriageByteOffset = 0xXX;
    //   constexpr std::uint8_t kInstalled = 0xYY;
    //   constexpr std::uint8_t kNotInstalled = 0xZZ;
    //   if (body.size() <= kMarriageByteOffset) return std::nullopt;
    //   auto const b = body[kMarriageByteOffset];
    //   if (b == kInstalled)    return true;
    //   if (b == kNotInstalled) return false;
    //   return std::nullopt;
    //
    // Until then, every caller falls back to the cmd 0x28 ASCII
    // payload (parse_user_info_body) — which is reliable on current
    // firmware. Returning nullopt here keeps the cmd 0x28 result
    // load-bearing without poisoning it.
    (void)body;
    return std::nullopt;
}

SplitPath split_ap_path(std::string_view absolute_path) {
    SplitPath out;
    auto slash = absolute_path.rfind('/');
    if (slash == std::string_view::npos) {
        // No slash at all — treat the whole thing as a bare name in
        // root. Unusual; the AP path tree is rooted, so callers should
        // always pass "/<…>" but we tolerate the bare form.
        out.name = std::string{absolute_path};
        out.path = "/";
        return out;
    }
    out.path = std::string{absolute_path.substr(0, slash + 1)}; // include the slash
    if (out.path.empty()) {
        out.path = "/";
    }
    out.name = std::string{absolute_path.substr(slash + 1)};
    return out;
}

Status Client::send_packet(std::uint8_t type, std::span<std::uint8_t const> body) {
    auto packet = st::transport::ap3::encode_packet(type, body);
    if (!packet.has_value()) {
        return st::failure(std::move(packet).error());
    }
    return channel_->write_bytes(*packet);
}

Result<std::vector<std::uint8_t>> Client::read_exact(std::size_t n) {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    auto const deadline = std::chrono::steady_clock::now() + cfg_.io_timeout;
    while (out.size() < n) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return st::failure(st::ErrorCode::TransportTimeout,
                               "ap3::read_exact: deadline exceeded with " +
                                   std::to_string(out.size()) + "/" + std::to_string(n) +
                                   " bytes");
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto chunk = channel_->read_bytes(n - out.size(), remaining);
        if (!chunk.has_value()) {
            return st::failure(std::move(chunk).error());
        }
        if (chunk->empty()) {
            // Channel returned empty inside the deadline — try again
            // until we time out. Real USB endpoints can briefly stall.
            continue;
        }
        out.insert(out.end(), chunk->begin(), chunk->end());
    }
    return out;
}

Result<std::vector<std::uint8_t>> Client::receive_packet_body_drain_mode() {
    // Mirrors ap_pull_state.py do_query(): 512-byte reads in a loop,
    // tracking the time of the last non-empty chunk. Stops when the
    // bus has been idle for `kIdleThreshold` AND we have at least one
    // complete wire frame's worth of bytes (header + CRC = 11 bytes).
    // Hard-caps at cfg_.file_data_io_timeout. Does NOT trust wire_len
    // from the response header — that's the whole point of this path.
    constexpr std::size_t kChunkSize = 512U;
    constexpr auto kIdleThreshold = std::chrono::milliseconds{800};
    constexpr auto kPerCallTimeout = std::chrono::milliseconds{500};
    auto const deadline = std::chrono::steady_clock::now() + cfg_.file_data_io_timeout;
    auto last_data = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> raw;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto chunk_timeout = std::min(remaining, kPerCallTimeout);
        auto chunk = channel_->read_bytes(kChunkSize, chunk_timeout);
        if (!chunk.has_value()) {
            return st::failure(std::move(chunk).error());
        }
        if (!chunk->empty()) {
            raw.insert(raw.end(), chunk->begin(), chunk->end());
            last_data = std::chrono::steady_clock::now();
            continue;
        }
        // Empty read = no bytes in this 500ms window. If we have any
        // accumulated data and the idle window has passed, stop.
        if (!raw.empty()) {
            auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_data);
            if (idle_for >= kIdleThreshold) {
                break;
            }
        }
    }
    if (raw.size() < st::transport::ap3::kFixedOverhead) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::receive_packet_body_drain_mode: got " +
                               std::to_string(raw.size()) +
                               " bytes; need >= " +
                               std::to_string(st::transport::ap3::kFixedOverhead));
    }
    // Validate the leading wire header. Don't trust its wire_len for
    // size — the field is empirically wrong for large cmd 0x21
    // responses — but it MUST be a syntactically valid header so we
    // know we're not staring at random bus data.
    if (raw[0] != st::transport::ap3::kSyncByte0 ||
        raw[1] != st::transport::ap3::kSyncByte1 ||
        raw[5] != 0x00U) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::receive_packet_body_drain_mode: invalid sync header");
    }
    auto const response_type = raw[6];
    if (response_type == static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Error)) {
        // Forward the error frame body just like receive_packet_body does.
        std::string msg{"ap3: device returned error frame"};
        std::size_t const body_end =
            raw.size() >= st::transport::ap3::kCrcSize
                ? raw.size() - st::transport::ap3::kCrcSize
                : raw.size();
        for (std::size_t i = st::transport::ap3::kHeaderSize; i < body_end; ++i) {
            char buf[4];
            (void)std::snprintf(buf, sizeof(buf), "%02X", raw[i]);
            msg += buf;
        }
        return st::failure(st::ErrorCode::TransportNack, std::move(msg));
    }
    // Strip header + CRC trailer. Accept zero-CRC per §3 sentinel
    // (we already don't checksum-verify in drain mode — the wire_len
    // is untrusted, so a CRC over the wrong byte range would be too).
    std::vector<std::uint8_t> body;
    body.reserve(raw.size() - st::transport::ap3::kFixedOverhead);
    body.insert(body.end(),
                raw.begin() + st::transport::ap3::kHeaderSize,
                raw.end() - st::transport::ap3::kCrcSize);
    return body;
}

Result<std::vector<std::uint8_t>> Client::receive_packet_body() {
    auto header_bytes = read_exact(st::transport::ap3::kHeaderSize);
    if (!header_bytes.has_value()) {
        return st::failure(std::move(header_bytes).error());
    }
    auto header = st::transport::ap3::decode_header(*header_bytes);
    if (!header.has_value()) {
        return st::failure(std::move(header).error());
    }
    auto rest = read_exact(header->body_size + st::transport::ap3::kCrcSize);
    if (!rest.has_value()) {
        return st::failure(std::move(rest).error());
    }
    std::vector<std::uint8_t> full;
    full.reserve(header_bytes->size() + rest->size());
    full.insert(full.end(), header_bytes->begin(), header_bytes->end());
    full.insert(full.end(), rest->begin(), rest->end());

    // Extract body either from strict CRC verification (the common case)
    // or from the zero-CRC-sentinel fallback. The AP firmware empirically
    // emits 0x00000000 in the CRC trailer for cmd 0x21 (ReadFile DATA)
    // responses — confirmed against a real `/backupcksum` pull on
    // 2026-06-12, spec §5's strict CRC notwithstanding. The hypothesis is
    // that the firmware skips CRC computation for file-data responses
    // because the file content typically carries its own integrity (.ptm
    // has XTEA-CBC + AES + bzip2 magic; /backupcksum is an MD5 ASCII
    // string the user reads). Accept zero-CRC as a sentinel rather than
    // refusing the body — but ONLY on exact `00 00 00 00`, so any other
    // mismatch still surfaces as BadChecksum.
    std::span<std::uint8_t const> body_span;
    auto body_view = st::transport::ap3::decode_packet_body(full);
    if (body_view.has_value()) {
        body_span = *body_view;
    } else if (body_view.error().code() == st::ErrorCode::BadChecksum &&
               full.size() >= st::transport::ap3::kCrcSize) {
        std::size_t const crc_off = full.size() - st::transport::ap3::kCrcSize;
        bool const all_zero =
            full[crc_off] == 0 && full[crc_off + 1] == 0 &&
            full[crc_off + 2] == 0 && full[crc_off + 3] == 0;
        if (!all_zero) {
            return st::failure(std::move(body_view).error());
        }
        std::fprintf(stderr,
                     "ap3: warning — AP emitted zero CRC trailer for response "
                     "(type=0x%02X, body=%u bytes); accepting body unchecked "
                     "(firmware skips CRC for cmd 0x21 file-data responses; "
                     "see receive_packet_body comment for rationale).\n",
                     header->type, header->body_size);
        body_span = std::span<std::uint8_t const>{full.data() + st::transport::ap3::kHeaderSize,
                                                  header->body_size};
    } else {
        return st::failure(std::move(body_view).error());
    }

    if (header->type == static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Error)) {
        std::string msg{"ap3: device returned error frame"};
        if (!body_span.empty()) {
            msg += " body=";
            for (auto b : body_span) {
                char buf[4];
                (void)std::snprintf(buf, sizeof(buf), "%02X", b);
                msg += buf;
            }
        }
        return st::failure(st::ErrorCode::TransportNack, std::move(msg));
    }
    return std::vector<std::uint8_t>{body_span.begin(), body_span.end()};
}

Status Client::ensure_session_warmup() {
    if (session_warmed_up_) {
        return st::ok();
    }
    auto state = query_state();
    if (!state.has_value()) {
        return st::failure(std::move(state).error());
    }
    session_warmed_up_ = true;
    return st::ok();
}

Result<DeviceState> Client::query_state() {
    DeviceState state{};

    // cmd 0x28 UserInfo — the host-side probe body is the bare
    // Boost-archive header (§12.2 test vector).
    {
        std::vector<std::uint8_t> probe{kCmd28ProbeBody.begin(), kCmd28ProbeBody.end()};
        if (auto s = send_packet(0x28U, probe); !s.has_value()) {
            return st::failure(std::move(s).error());
        }
        auto body = receive_packet_body();
        if (!body.has_value()) {
            return st::failure(std::move(body).error());
        }
        state.user_info_body = std::move(*body);
        // Spec §6.13 (revised): the cmd 0x28 response carries all of
        // firmware version / AP serial / vehicle / marriage in one
        // ASCII payload split on '\n'. Parse it deterministically; the
        // older try_extract_leading_string heuristic is no longer needed.
        auto const fields = parse_user_info_body(state.user_info_body);
        state.firmware_version = fields.firmware_version;
        state.ap_serial = fields.ap_serial;
        state.vehicle_descriptor = fields.vehicle;
        state.married = fields.married;
    }

    // cmd 0x04 firmware version — body-less request. The response is a
    // plain ASCII string (no archive header per §6.14). Populated as a
    // backup if the cmd 0x28 parse failed (e.g. response shape drift).
    {
        if (auto s = send_packet(0x04U, std::span<std::uint8_t const>{}); !s.has_value()) {
            return st::failure(std::move(s).error());
        }
        auto body = receive_packet_body();
        if (!body.has_value()) {
            return st::failure(std::move(body).error());
        }
        state.firmware_body = std::move(*body);
        if (!state.firmware_version.has_value()) {
            std::string s(reinterpret_cast<char const *>(state.firmware_body.data()),
                          state.firmware_body.size());
            while (!s.empty() && (static_cast<unsigned char>(s.back()) < 0x20 ||
                                  static_cast<unsigned char>(s.back()) >= 0x7F)) {
                s.pop_back();
            }
            if (!s.empty() &&
                std::all_of(s.begin(), s.end(),
                            [](unsigned char c) { return c >= 0x20 && c < 0x7F; })) {
                state.firmware_version = s;
            }
        }
    }

    // cmd 0x03 DeviceSettings. Empty body on host side.
    {
        if (auto s = send_packet(0x03U, std::span<std::uint8_t const>{}); !s.has_value()) {
            return st::failure(std::move(s).error());
        }
        auto body = receive_packet_body();
        if (!body.has_value()) {
            return st::failure(std::move(body).error());
        }
        state.device_settings_body = std::move(*body);
        // Marriage state is primarily sourced from the cmd 0x28 ASCII
        // payload (spec §6.13). The cmd 0x03 DeviceSettings blob is
        // also examined here as a fallback: if cmd 0x28 yielded a
        // marriage flag we keep it (authoritative); if it didn't, the
        // device-settings hook gets a chance. Today the hook always
        // returns nullopt (analyst RE task — spec §15 open question);
        // when the offset lands the override-vs-fallback decision is
        // already wired.
        auto const dev_marriage =
            parse_marriage_from_device_settings(state.device_settings_body);
        if (!state.married.has_value() && dev_marriage.has_value()) {
            state.married = dev_marriage;
        }
    }

    // A successful query_state() satisfies the cmd 0x28 session-warmup
    // prereq for file-vault commands; mark the flag so subsequent
    // ls/read/write/remove calls don't re-probe.
    session_warmed_up_ = true;
    return state;
}

Result<std::vector<FileInfo>> Client::ls(std::string_view subdir) {
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // cmd 0x26 ListFiles carries a single Boost-archived UTF-8 string
    // (directory name like "maps" / "presets" / "datalog" / "images")
    // — NOT a FileInfo2 record. The AP firmware accepts bare names
    // without leading or trailing slashes; normalise here so callers
    // can still pass `/maps/` or `maps`.
    std::string dir{subdir};
    while (!dir.empty() && dir.front() == '/') {
        dir.erase(0, 1);
    }
    while (!dir.empty() && dir.back() == '/') {
        dir.pop_back();
    }
    auto body = encode_string_body(dir);
    if (!body.has_value()) {
        return st::failure(std::move(body).error());
    }
    if (auto s = send_packet(0x26U, *body); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    auto resp = receive_packet_body();
    if (!resp.has_value()) {
        return st::failure(std::move(resp).error());
    }
    return decode_file_info_list(*resp);
}

Result<std::vector<std::uint8_t>> Client::read_file(std::string_view path) {
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // ReadFile setup (cmd 0x20) expects:
    //   - FileInfo2.name = basename ("test.ptm", "backupcksum")
    //   - FileInfo2.path = directory with trailing slash and leading
    //     slash ("/maps/"); EXCEPT for root-level pseudo-files like
    //     /backupcksum, where path = name = bare token.
    // Empirical layout pinned against `cmd20_*_request_known_good.bin`.
    // cmd 0x20 ReadFile setup expects:
    //   FileInfo2.name = basename (e.g. "Stage1.ptm")
    //   FileInfo2.path = full relative path with NO leading slash
    //                    (e.g. "maps/Stage1.ptm" or "backupcksum")
    // Live-confirmed 2026-06-12 against the reference Python tool's
    // successful pull of 59 tunes from /maps/. The earlier convention
    // (name=basename, path="/maps/") produced a degenerate setup ACK
    // (name=".", size=0) and cmd 0x21 returned zero body bytes.
    auto split = split_ap_path(path);
    FileInfo info{};
    info.name = std::move(split.name);
    info.path = strip_leading_slash(path); // full relative path

    // Step 1: cmd 0x20 setup. Response carries the actual size in a
    // FileInfo2 echo; we discard the host-echoed name/path and trust
    // the size field for the step-2 read.
    auto setup_body = encode_file_info(info);
    if (!setup_body.has_value()) {
        return st::failure(std::move(setup_body).error());
    }
    if (auto s = send_packet(0x20U, *setup_body); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    auto setup_resp = receive_packet_body();
    if (!setup_resp.has_value()) {
        return st::failure(std::move(setup_resp).error());
    }
    if (setup_resp->empty()) {
        return st::failure(st::ErrorCode::ProtocolError,
                           "ap3::read_file: empty setup ACK body");
    }
    // The AP's setup ACK uses a DIFFERENT layout than our request —
    // see decode_setup_ack_response_shape for the byte-level decode.
    // Try the response-shape decoder; if it succeeds AND reports
    // name="." with size=0, the AP firmware's path lookup degenerated
    // (live-confirmed for files in /maps/, /presets/, /datalog/ as
    // of 2026-06-12; analyst investigation pending — see
    // findings/handoffs/HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation.md).
    // Fail fast in that case rather than burning a 30-second cmd 0x21
    // timeout for nothing.
    // Decode the setup ACK to extract reported_size (response shape is
    // different from the request — see decode_setup_ack_response_shape).
    // A degenerate ACK (name=".", size=0) historically meant the AP's
    // path-lookup failed because we were sending path="/maps/" (directory)
    // instead of path="maps/X.ptm" (full relative); the cmd 0x20 setup
    // path field convention is now fixed and ACKs decode cleanly. The
    // fast-fail check is kept as a defensive guard in case other
    // degenerate request shapes surface in the future.
    std::uint64_t reported_size = 0;
    if (auto ack_info = decode_setup_ack_response_shape(*setup_resp);
        ack_info.has_value()) {
        if (ack_info->name == "." && ack_info->size == 0) {
            return st::failure(
                st::ErrorCode::TransportNack,
                "ap3::read_file: AP setup ACK reported a degenerate path "
                "lookup (name=\".\", size=0) for '" +
                    std::string{path} +
                    "'. Verify the path encoding — cmd 0x20 wants "
                    "FileInfo2.path = full relative path with no leading "
                    "slash (e.g. \"maps/X.ptm\"), not the directory.");
        }
        reported_size = ack_info->size;
    } else if (auto echoed_info = decode_file_info(*setup_resp);
               echoed_info.has_value()) {
        reported_size = echoed_info->size;
    }

    // Step 2: cmd 0x21 data. Body is a Boost-archived UTF-8 string —
    // the AP uses it as an opaque session identifier and does not
    // write to it. Pass the AP path; the analyst tool's choice of a
    // synthetic Windows-local path is cosmetic.
    auto data_req = encode_string_body(path);
    if (!data_req.has_value()) {
        return st::failure(std::move(data_req).error());
    }
    if (auto s = send_packet(0x21U, *data_req); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // cmd 0x21 file data can run to tens of KB on a real tune (~58 KB
    // for a typical Stage1) and the AP can take several seconds to
    // push the body through bulk-in. Swap to the wider file-data
    // timeout for just this receive; restore the metadata default
    // after so any error-path subsequent reads use the small budget.
    //
    // Two strategies, selected by env var:
    //   - default (strict): receive_packet_body trusts wire_len from
    //     the response header and read_exact's that many body bytes.
    //   - ST_AP3_READFILE_DRAIN_MODE=1: receive_packet_body_drain_mode
    //     ignores wire_len and accumulates 512-byte chunks until 800ms
    //     idle. Mirrors ap_pull_state.py do_query(); use this when the
    //     strict path advertises wire_len that doesn't match the actual
    //     body size (per the 2026-06-12 cmd 0x21 large-file truncation
    //     observation).
    auto const saved_timeout = cfg_.io_timeout;
    cfg_.io_timeout = cfg_.file_data_io_timeout;
    auto file_resp = readfile_drain_mode_enabled()
                         ? receive_packet_body_drain_mode()
                         : receive_packet_body();
    cfg_.io_timeout = saved_timeout;
    if (!file_resp.has_value()) {
        return st::failure(std::move(file_resp).error());
    }
    // The AP reports the size in step-1; verify the step-2 body
    // matches when we have a reported size. A size mismatch is a
    // protocol error worth surfacing rather than silently truncating.
    if (reported_size != 0 && file_resp->size() != reported_size) {
        return st::failure(st::ErrorCode::ProtocolError,
                           "ap3::read_file: body size " + std::to_string(file_resp->size()) +
                               " mismatched reported size " + std::to_string(reported_size));
    }
    return std::move(*file_resp);
}

Status Client::write_file(std::string_view path, std::span<std::uint8_t const> data,
                          std::uint64_t mtime_unix_secs) {
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return s;
    }
    // PutFile setup (cmd 0x22) expects:
    //   - FileInfo2.name = basename
    //   - FileInfo2.path = full destination WITHOUT leading slash
    //     ("maps/test.ptm"). Different convention from ReadFile.
    // Empirical layout pinned against `cmd22_putfile_setup_request_known_good.bin`.
    auto split = split_ap_path(path);
    FileInfo info{};
    info.name = std::move(split.name);
    info.path = strip_leading_slash(path);
    info.mtime = mtime_unix_secs;
    info.size = data.size();

    // Step 1: cmd 0x22 setup.
    auto setup_body = encode_file_info(info);
    if (!setup_body.has_value()) {
        return st::failure(std::move(setup_body).error());
    }
    if (auto s = send_packet(0x22U, *setup_body); !s.has_value()) {
        return s;
    }
    auto setup_ack = receive_packet_body();
    if (!setup_ack.has_value()) {
        return st::failure(std::move(setup_ack).error());
    }

    // Step 2: cmd 0x23 data. Body IS the raw file content.
    if (auto s = send_packet(0x23U, data); !s.has_value()) {
        return s;
    }
    auto commit_ack = receive_packet_body();
    if (!commit_ack.has_value()) {
        return st::failure(std::move(commit_ack).error());
    }
    return st::ok();
}

Status Client::remove_file(std::string_view path) {
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return s;
    }
    // RemoveFile (cmd 0x25) uses the same path-without-leading-slash
    // convention as PutFile. Pinned against
    // `cmd25_removefile_request_known_good.bin`.
    auto split = split_ap_path(path);
    FileInfo info{};
    info.name = std::move(split.name);
    info.path = strip_leading_slash(path);
    auto body = encode_file_info(info);
    if (!body.has_value()) {
        return st::failure(std::move(body).error());
    }
    if (auto s = send_packet(0x25U, *body); !s.has_value()) {
        return s;
    }
    auto ack = receive_packet_body();
    if (!ack.has_value()) {
        return st::failure(std::move(ack).error());
    }
    return st::ok();
}

} // namespace st::devices::ap3
