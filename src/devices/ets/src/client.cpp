// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/client.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/ets_packet.hpp"

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

namespace st::devices::ets {

namespace {

// Strip a leading '/' from an AP-vault path. For cmd 0x22 (PutFile)
// and cmd 0x25 (RemoveFile) the AP firmware expects the request
// VaultFileMetadata.path to be the full destination relative to the vault
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
// from the response header. Kept as defensive code: an early
// 2026-06-12 PM bug (cmd 0x20 path field set to the directory rather
// than the full relative path — fixed in ef70018) caused the AP to
// answer cmd 0x21 with wire_len > 0 + zero body bytes; the resolved
// failure-mode analysis is in
// findings/handoffs/HANDOFF-to-analyst-2026-06-12-cmd21-large-file-truncation.md.
// ST_ETS_READFILE_DRAIN_MODE=1 swaps in an idle-driven accumulator
// (512-byte reads, stop after 800 ms of silence) for the cmd 0x21
// step only — a one-knob escape hatch if a future firmware revision
// regresses the wire_len reporting.
bool readfile_drain_mode_enabled() noexcept {
    static int cached = -1;
    if (cached == -1) {
        char const *v = std::getenv("ST_ETS_READFILE_DRAIN_MODE");
        cached = (v != nullptr && std::string{v} == "1") ? 1 : 0;
    }
    return cached == 1;
}

// Diagnostic-cmd gate per workstream G. When the env-var is set,
// `Client::remount_user_filesystem` (cmd 0x05) + `Client::get_capabilities`
// (cmd 0x1f) become callable. Cmd 0x05 needs a transport-block-list
// bypass (it's on `is_blocked_command`'s defense-in-depth list — see
// dispatch_table_CORRECTED.md: actually OnFilesystem::remountUser, not
// reboot, but field reports of dazes keep it blocked by default). Cmd
// 0x1f is already in the safe range; only the env-var gate stops the
// API from being called.
bool ap3_diagnostic_cmds_enabled() noexcept {
    // Re-read on each call so tests can flip the env-var between
    // cases — unlike `readfile_drain_mode_enabled()` which caches
    // (its value is meant to be set once at process start). The
    // hot-path cost of a single getenv per gated call is negligible
    // (these are not in the inner loop of any throughput-sensitive
    // path).
    char const *v = std::getenv("ST_ETS_ENABLE_DIAGNOSTIC_CMDS");
    return v != nullptr && std::string{v} == "1";
}

// Build + ship a packet bypassing the codec-level block list. Only
// callable from within Client and only after the surrounding caller
// has cleared the appropriate gate (diagnostic env-var for cmd 0x05
// stand-alone calls, or the ST_HAVE_AP_WORKFLOW build flag for the
// NAND-barrier path inside write_file_impl / remove_file_impl).
// Mirrors `st::transport::ets::encode_packet` minus the
// `is_blocked_command` check.
inline Status send_packet_bypassing_block_list(
    st::transport::IByteChannel &channel, std::uint8_t type,
    std::span<std::uint8_t const> body) {
    using namespace st::transport::ets;
    std::uint32_t const wire_len =
        static_cast<std::uint32_t>(body.size() + kCrcSize);
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + body.size() + kCrcSize);
    out.push_back(kSyncByte0);
    out.push_back(kSyncByte1);
    out.push_back(static_cast<std::uint8_t>((wire_len >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((wire_len >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(wire_len & 0xFFU));
    out.push_back(0x00U); // reserved
    out.push_back(type);
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t const crc = packet_crc(out);
    out.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return channel.write_bytes(out);
}

} // namespace

// Per spec §6.13 (revised 2026-06-11 PM): cmd 0x28 (UserInfo) response
// body shape is:
//
//   [0..29]    30-byte VaultFileMetadata-style prefix
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
    // The 30-byte VaultFileMetadata prefix is the leading magic + 3-byte
    // archive-config; validate the leading 27 bytes of magic, the
    // analyst spec doesn't require strict validation of the trailing
    // 3 bytes.
    if (!std::equal(kVaultFileMetadataPrefix.begin(), kVaultFileMetadataPrefix.begin() + 27,
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
        out.vehicle_paired = (fields[3] == "Installed");
    }
    out.vehicle = to_string_opt(fields[4]);
    return out;
}

// Heuristic extractor for the machine-readable vehicle code embedded
// in the cmd 0x03 DeviceSettings body (Boost-archived binary struct
// with multiple typed fields). Empirically the code appears as a
// u32 LE length prefix followed by N bytes of uppercase-ASCII
// matching the .ptm PrivateData XML `<vehicleID>` format
// ("SUBA_US_WRXM_CF_17_F", "SUBA_JP_..." etc.). Scans the body for
// the first plausible length-prefixed ASCII run of 8..40 chars,
// uppercase/digits/underscore only, that contains "_US_" / "_JP_" /
// "_EU_" / "_AU_". Returns nullopt on no plausible match.
std::optional<std::string>
parse_vehicle_id_from_device_settings(std::span<std::uint8_t const> body) {
    auto const is_market_marker = [](std::string_view s) {
        return s.find("_US_") != std::string_view::npos ||
               s.find("_JP_") != std::string_view::npos ||
               s.find("_EU_") != std::string_view::npos ||
               s.find("_AU_") != std::string_view::npos;
    };
    auto const is_id_char = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    if (body.size() < 8) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i + 4 < body.size(); ++i) {
        std::uint32_t const len = static_cast<std::uint32_t>(body[i]) |
                                  (static_cast<std::uint32_t>(body[i + 1]) << 8) |
                                  (static_cast<std::uint32_t>(body[i + 2]) << 16) |
                                  (static_cast<std::uint32_t>(body[i + 3]) << 24);
        if (len < 8 || len > 40) {
            continue;
        }
        if (i + 4 + len > body.size()) {
            continue;
        }
        bool all_id_chars = true;
        for (std::size_t j = 0; j < len; ++j) {
            if (!is_id_char(body[i + 4 + j])) {
                all_id_chars = false;
                break;
            }
        }
        if (!all_id_chars) {
            continue;
        }
        std::string_view const candidate{
            reinterpret_cast<char const *>(body.data() + i + 4), len};
        if (is_market_marker(candidate)) {
            return std::string{candidate};
        }
    }
    return std::nullopt;
}

std::optional<bool>
parse_marriage_from_device_settings(std::span<std::uint8_t const> body) {
    // Per RE2 (findings/re-2026-06-12-pm/): the AP stores persistent
    // settings in `/root/settings` as plain ASCII INI with no
    // marriage field. cmd 0x28 is the only source. This hook stays
    // as a forward-compat seam — if a future firmware revision adds
    // a marriage byte to cmd 0x03, the body becomes a one-liner and
    // query_state's fallback ordering picks it up automatically.
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

Client::Client(st::transport::IByteChannel &channel, ClientConfig cfg)
    : channel_{&channel}, cfg_{cfg} {
    // Spin the worker thread immediately. Every public verb routes
    // work to it via the queue; the constructor must complete
    // before any verb call so the queue + cv + flag are all
    // initialized when the worker starts pulling.
    worker_ = std::thread{[this]() { worker_loop(); }};
}

Client::~Client() {
    // Signal stop + wake the worker so it can drain. Join under
    // RAII semantics — if the worker is wedged on a USB read the
    // join will block, but the channel destructor (which runs after
    // this) is what cancels in-flight I/O. For the existing tests
    // (LoopbackByteChannel) the worker exits promptly because all
    // ops are queue-poll/run/repeat.
    {
        std::lock_guard<std::mutex> lock{queue_mu_};
        stop_worker_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Client::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{queue_mu_};
            queue_cv_.wait(lock, [this]() {
                return stop_worker_ || !queue_.empty();
            });
            if (stop_worker_ && queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // Task body runs without the lock held — channel I/O is
        // potentially seconds-long and would block submitters.
        task();
    }
}

Status Client::send_packet(std::uint8_t type, std::span<std::uint8_t const> body) {
    auto packet = st::transport::ets::encode_packet(type, body);
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
    if (raw.size() < st::transport::ets::kFixedOverhead) {
        return st::failure(st::ErrorCode::UnexpectedEof,
                           "ap3::receive_packet_body_drain_mode: got " +
                               std::to_string(raw.size()) +
                               " bytes; need >= " +
                               std::to_string(st::transport::ets::kFixedOverhead));
    }
    // Validate the leading wire header. Don't trust its wire_len for
    // size — the field is empirically wrong for large cmd 0x21
    // responses — but it MUST be a syntactically valid header so we
    // know we're not staring at random bus data.
    if (raw[0] != st::transport::ets::kSyncByte0 ||
        raw[1] != st::transport::ets::kSyncByte1 ||
        raw[5] != 0x00U) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::receive_packet_body_drain_mode: invalid sync header");
    }
    auto const response_type = raw[6];
    if (response_type == static_cast<std::uint8_t>(st::transport::ets::ResponseType::Error)) {
        // Forward the error frame body just like receive_packet_body does.
        std::string msg{"ap3: device returned error frame"};
        std::size_t const body_end =
            raw.size() >= st::transport::ets::kCrcSize
                ? raw.size() - st::transport::ets::kCrcSize
                : raw.size();
        for (std::size_t i = st::transport::ets::kHeaderSize; i < body_end; ++i) {
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
    body.reserve(raw.size() - st::transport::ets::kFixedOverhead);
    body.insert(body.end(),
                raw.begin() + st::transport::ets::kHeaderSize,
                raw.end() - st::transport::ets::kCrcSize);
    return body;
}

Result<std::vector<std::uint8_t>> Client::receive_packet_body() {
    auto header_bytes = read_exact(st::transport::ets::kHeaderSize);
    if (!header_bytes.has_value()) {
        return st::failure(std::move(header_bytes).error());
    }
    auto header = st::transport::ets::decode_header(*header_bytes);
    if (!header.has_value()) {
        return st::failure(std::move(header).error());
    }
    auto rest = read_exact(header->body_size + st::transport::ets::kCrcSize);
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
    auto body_view = st::transport::ets::decode_packet_body(full);
    if (body_view.has_value()) {
        body_span = *body_view;
    } else if (body_view.error().code() == st::ErrorCode::BadChecksum &&
               full.size() >= st::transport::ets::kCrcSize) {
        std::size_t const crc_off = full.size() - st::transport::ets::kCrcSize;
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
        body_span = std::span<std::uint8_t const>{full.data() + st::transport::ets::kHeaderSize,
                                                  header->body_size};
    } else {
        return st::failure(std::move(body_view).error());
    }

    if (header->type == static_cast<std::uint8_t>(st::transport::ets::ResponseType::Error)) {
        std::string msg{"ap3: device returned error frame"};
        if (!body_span.empty()) {
            msg += " body=";
            for (auto b : body_span) {
                char buf[4];
                (void)std::snprintf(buf, sizeof(buf), "%02X", b);
                msg += buf;
            }
            // Per RE wave 4 default-handler decode: when the body is
            // a short ASCII decimal string, it's the dispatched cmd
            // byte encoded as `itoa(cmd)`. Bodies "32" / "38" / "40"
            // observed in §4.2 daze reports decode to cmds 50 / 56 /
            // 64 — all in the out-of-range default-handler band. The
            // decode helps users understand WHICH cmd the firmware
            // rejected.
            bool all_ascii_digits = true;
            int cmd_decoded = 0;
            for (auto b : body_span) {
                if (b < '0' || b > '9') {
                    all_ascii_digits = false;
                    break;
                }
                cmd_decoded = cmd_decoded * 10 + (b - '0');
            }
            if (all_ascii_digits && !body_span.empty() &&
                body_span.size() <= 3 && cmd_decoded <= 255) {
                char buf[96];
                (void)std::snprintf(buf, sizeof(buf),
                                    " (default-handler error response — cmd "
                                    "0x%02X is not implemented in this "
                                    "firmware or was sent with wrong args)",
                                    cmd_decoded);
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
        state.vehicle_paired = fields.vehicle_paired;
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
        if (!state.vehicle_paired.has_value() && dev_marriage.has_value()) {
            state.vehicle_paired = dev_marriage;
        }
        // Extract the machine vehicle_id from the same body. Embedded
        // as a u32 LE length prefix + uppercase-ASCII run; heuristic
        // match against market markers (_US_ / _JP_ / _EU_ / _AU_).
        state.vehicle_id =
            parse_vehicle_id_from_device_settings(state.device_settings_body);
    }

    // Three additional status probes per RE8b CORRECTED dispatch
    // table (firmware v1.7.6.0-28785): cmd 0x2e OnHardwareType, cmd
    // 0x30 OnGetVehicleManufacturer, cmd 0x31 OnGetApManufacturer.
    // All three are read-only — empty host body, ASCII-string
    // response — same shape as cmd 0x04. A firmware that doesn't
    // implement them emits a default-handler error packet; we treat
    // any non-printable-ASCII response as nullopt so query_state
    // still succeeds on older builds. Run AFTER the warmup trio so
    // session_warmed_up_ already covers the file-vault prereq.
    // Boost binary archive prefix on these read-only status responses:
    //
    //   bytes 0..3   : u32 LE  = 22 (length of "serialization::archive")
    //   bytes 4..25  : ASCII   = "serialization::archive"
    //   bytes 26..34 : 9 bytes : archive version + flags (03 04 04 04 08 01 00 00 00 observed)
    //
    // Past that 35-byte prefix the payload is either:
    //   - a length-prefixed ASCII string: u32 LE length + N chars (cmd 0x30,
    //     cmd 0x31 — VehicleManufacturer / ApManufacturer)
    //   - a bare u32 LE value (cmd 0x2e — HardwareType enum)
    //
    // Confirmed on firmware v1.7.6.0-28785 against SUB0484551 (2026-06-13).
    // Older firmware may emit a default-handler error packet instead; we
    // degrade to nullopt rather than failing query_state.
    static constexpr std::size_t kBoostArchivePrefixLen = 35;
    auto probe_ascii_string = [&](std::uint8_t cmd,
                                   std::vector<std::uint8_t> &out_body,
                                   std::optional<std::string> &out_parsed) -> Status {
        if (auto s = send_packet(cmd, std::span<std::uint8_t const>{}); !s.has_value()) {
            return st::failure(std::move(s).error());
        }
        auto body = receive_packet_body();
        if (!body.has_value()) {
            return st::failure(std::move(body).error());
        }
        out_body = std::move(*body);
        // Past prefix: u32 LE length + N ASCII chars.
        if (out_body.size() < kBoostArchivePrefixLen + 4) {
            return st::ok(); // degraded — leave nullopt
        }
        std::size_t off = kBoostArchivePrefixLen;
        auto const len =
            static_cast<std::uint32_t>(out_body[off]) |
            (static_cast<std::uint32_t>(out_body[off + 1]) << 8) |
            (static_cast<std::uint32_t>(out_body[off + 2]) << 16) |
            (static_cast<std::uint32_t>(out_body[off + 3]) << 24);
        off += 4;
        if (len == 0 || len > 1024 ||
            off + static_cast<std::size_t>(len) > out_body.size()) {
            return st::ok(); // implausible — leave nullopt
        }
        std::string s(reinterpret_cast<char const *>(out_body.data() + off), len);
        if (std::all_of(s.begin(), s.end(),
                        [](unsigned char c) { return c >= 0x20 && c < 0x7F; })) {
            out_parsed = std::move(s);
        }
        return st::ok();
    };
    auto probe_u32 = [&](std::uint8_t cmd,
                          std::vector<std::uint8_t> &out_body,
                          std::optional<std::string> &out_parsed) -> Status {
        if (auto s = send_packet(cmd, std::span<std::uint8_t const>{}); !s.has_value()) {
            return st::failure(std::move(s).error());
        }
        auto body = receive_packet_body();
        if (!body.has_value()) {
            return st::failure(std::move(body).error());
        }
        out_body = std::move(*body);
        if (out_body.size() < kBoostArchivePrefixLen + 4) {
            return st::ok();
        }
        std::size_t off = kBoostArchivePrefixLen;
        auto const val =
            static_cast<std::uint32_t>(out_body[off]) |
            (static_cast<std::uint32_t>(out_body[off + 1]) << 8) |
            (static_cast<std::uint32_t>(out_body[off + 2]) << 16) |
            (static_cast<std::uint32_t>(out_body[off + 3]) << 24);
        // Render as decimal — the enum semantics need Frida confirmation
        // (RE wave-6 hardware-type vs e_CpuTypes; see project memory),
        // so we surface the raw value rather than a guessed label.
        out_parsed = std::to_string(val);
        return st::ok();
    };
    if (auto s = probe_u32(0x2eU, state.hardware_type_body,
                            state.hardware_type);
        !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    if (auto s = probe_ascii_string(0x30U, state.vehicle_manufacturer_body,
                                     state.vehicle_manufacturer);
        !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    if (auto s = probe_ascii_string(0x31U, state.ap_manufacturer_body,
                                     state.ap_manufacturer);
        !s.has_value()) {
        return st::failure(std::move(s).error());
    }

    // A successful query_state() satisfies the cmd 0x28 session-warmup
    // prereq for file-vault commands; mark the flag so subsequent
    // ls/read/write/remove calls don't re-probe.
    session_warmed_up_ = true;
    return state;
}

Result<std::vector<FileInfo>> Client::ls_impl(std::string_view subdir,
                                                std::stop_token const &token) {
    if (token.stop_requested()) {
        return st::failure(ErrorCode::Cancelled,
                            "ap3::ls: cancelled before send");
    }
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // cmd 0x26 ListFiles carries a single Boost-archived UTF-8 string
    // (directory name like "maps" / "presets" / "datalog" / "images")
    // — NOT a VaultFileMetadata record. The AP firmware accepts bare names
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

Result<std::vector<FileInfo>> Client::ls(std::string_view subdir) {
    return ls_async(subdir).get();
}

std::future<Result<std::vector<FileInfo>>>
Client::ls_async(std::string_view subdir, std::stop_token token) {
    // Capture by value — std::string_view doesn't own its bytes, and
    // by the time the worker runs the caller's stack frame may be gone.
    std::string subdir_owned{subdir};
    return enqueue([this, dir = std::move(subdir_owned),
                    tok = std::move(token)]() {
        return ls_impl(dir, tok);
    });
}

Result<std::vector<std::uint8_t>> Client::read_file(std::string_view path) {
    return read_file_async(path).get();
}

std::future<Result<std::vector<std::uint8_t>>>
Client::read_file_async(std::string_view path, std::stop_token token) {
    std::string path_owned{path};
    return enqueue([this, p = std::move(path_owned),
                    tok = std::move(token)]() {
        return read_file_impl(p, tok);
    });
}

Result<std::vector<std::uint8_t>>
Client::read_file_impl(std::string_view path, std::stop_token const &token) {
    if (token.stop_requested()) {
        return st::failure(ErrorCode::Cancelled,
                            "ap3::read_file: cancelled before send");
    }
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // ReadFile setup (cmd 0x20) expects:
    //   - VaultFileMetadata.name = basename ("test.ptm", "backupcksum")
    //   - VaultFileMetadata.path = directory with trailing slash and leading
    //     slash ("/maps/"); EXCEPT for root-level pseudo-files like
    //     /backupcksum, where path = name = bare token.
    // Empirical layout pinned against `cmd20_*_request_known_good.bin`.
    // cmd 0x20 ReadFile setup expects:
    //   VaultFileMetadata.name = basename (e.g. "tune.ptm")
    //   VaultFileMetadata.path = full relative path with NO leading slash
    //                    (e.g. "maps/tune.ptm" or "backupcksum")
    // Live-confirmed 2026-06-12 against the reference Python tool's
    // successful pull of 59 tunes from /maps/. The earlier convention
    // (name=basename, path="/maps/") produced a degenerate setup ACK
    // (name=".", size=0) and cmd 0x21 returned zero body bytes.
    auto split = split_ap_path(path);
    FileInfo info{};
    info.name = std::move(split.name);
    info.path = strip_leading_slash(path); // full relative path

    // Step 1: cmd 0x20 setup. Response carries the actual size in a
    // VaultFileMetadata echo; we discard the host-echoed name/path and trust
    // the size field for the step-2 read.
    auto setup_body = encode_vault_file_metadata(info);
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
    // A degenerate ACK (name=".", size=0) historically meant the AP's
    // path-lookup failed because we sent path="/maps/" (directory)
    // instead of path="maps/X.ptm" (full relative); the cmd 0x20 setup
    // path convention is now fixed (ef70018) and ACKs decode cleanly
    // on the user's live AP. The fast-fail check stays as a defensive
    // guard against future degenerate request shapes — the alternative
    // is a 30-second cmd 0x21 timeout for nothing.
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
                    "VaultFileMetadata.path = full relative path with no leading "
                    "slash (e.g. \"maps/X.ptm\"), not the directory.");
        }
        reported_size = ack_info->size;
    } else if (auto echoed_info = decode_vault_file_metadata(*setup_resp);
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
    // for a typical retail) and the AP can take several seconds to
    // push the body through bulk-in. Swap to the wider file-data
    // timeout for just this receive; restore the metadata default
    // after so any error-path subsequent reads use the small budget.
    //
    // Two strategies, selected by env var:
    //   - default (strict): receive_packet_body trusts wire_len from
    //     the response header and read_exact's that many body bytes.
    //   - ST_ETS_READFILE_DRAIN_MODE=1: receive_packet_body_drain_mode
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

Status Client::write_file(std::string_view path,
                           std::span<std::uint8_t const> data,
                           std::uint64_t mtime_unix_secs) {
    // Copy data into the future-owned vector so the worker can outlive
    // the caller's data span. The async path takes ownership directly.
    std::vector<std::uint8_t> owned{data.begin(), data.end()};
    return write_file_async(path, std::move(owned), mtime_unix_secs).get();
}

std::future<Status>
Client::write_file_async(std::string_view path,
                          std::vector<std::uint8_t> data,
                          std::uint64_t mtime_unix_secs,
                          std::stop_token token) {
    std::string path_owned{path};
    return enqueue(
        [this, p = std::move(path_owned), bytes = std::move(data),
         mtime_unix_secs, tok = std::move(token)]() {
            return write_file_impl(p, bytes, mtime_unix_secs, tok);
        });
}

Status Client::write_file_impl(std::string_view path,
                                std::span<std::uint8_t const> data,
                                std::uint64_t mtime_unix_secs,
                                std::stop_token const &token) {
    // Cancellation contract takes precedence over the workflow gate —
    // a stopped token always returns Cancelled, never PolicyDenied,
    // so callers (and tests) can rely on the same shutdown semantics
    // in both build modes.
    if (token.stop_requested()) {
        return st::failure(ErrorCode::Cancelled,
                            "ap3::write_file: cancelled before send");
    }
#ifndef ST_HAVE_AP_WORKFLOW
    (void)path;
    (void)data;
    (void)mtime_unix_secs;
    // Workflow gate: write surface is OFF in the default distribution
    // build. PutFile (cmd 0x22) is a mutating wire command per
    // HANDOFF-from-analyst-2026-06-14-edit-export-push-ptm.md §CRITICAL.
    return st::failure(
        ErrorCode::PolicyDenied,
        "ap3::write_file: AP workflow surface is off in this build. "
        "Reconfigure with -DST_ENABLE_COBB_AP_WORKFLOW=ON to enable.");
#else
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return s;
    }
    // PutFile setup (cmd 0x22) expects:
    //   - VaultFileMetadata.name = basename
    //   - VaultFileMetadata.path = full destination WITHOUT leading slash
    //     ("maps/test.ptm"). Different convention from ReadFile.
    // Empirical layout pinned against `cmd22_putfile_setup_request_known_good.bin`.
    auto split = split_ap_path(path);
    FileInfo info{};
    info.name = std::move(split.name);
    info.path = strip_leading_slash(path);
    info.mtime = mtime_unix_secs;
    info.size = data.size();

    // Step 1: cmd 0x22 setup.
    auto setup_body = encode_vault_file_metadata(info);
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

    // Step 3: cmd 0x05 NAND barrier. Per
    // findings/re-2026-06-14/cmd_05_remountuser.md + T30 step 9:
    // remountUser commits the UBIFS journal so the just-written file
    // survives a power-cycle. Skipping it risks the write disappearing
    // on next mount. Bypasses the codec-level block list because cmd
    // 0x05 IS on the defense-in-depth list — the unguarded helper is
    // intentional here, the workflow flag is the outer gate.
    if (auto s = send_packet_bypassing_block_list(
            *channel_, 0x05U, std::span<std::uint8_t const>{});
        !s.has_value()) {
        return s;
    }
    auto barrier_ack = receive_packet_body();
    if (!barrier_ack.has_value()) {
        return st::failure(std::move(barrier_ack).error());
    }
    return st::ok();
#endif
}

Status Client::remove_file(std::string_view path) {
    return remove_file_async(path).get();
}

std::future<Status>
Client::remove_file_async(std::string_view path, std::stop_token token) {
    std::string path_owned{path};
    return enqueue([this, p = std::move(path_owned),
                    tok = std::move(token)]() {
        return remove_file_impl(p, tok);
    });
}

Status Client::remove_file_impl(std::string_view path,
                                  std::stop_token const &token) {
    if (token.stop_requested()) {
        return st::failure(ErrorCode::Cancelled,
                            "ap3::remove_file: cancelled before send");
    }
#ifndef ST_HAVE_AP_WORKFLOW
    (void)path;
    return st::failure(
        ErrorCode::PolicyDenied,
        "ap3::remove_file: AP workflow surface is off in this build. "
        "Reconfigure with -DST_ENABLE_COBB_AP_WORKFLOW=ON to enable.");
#else
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
    auto body = encode_vault_file_metadata(info);
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
    // NAND barrier — same rationale as write_file_impl. A delete that
    // doesn't survive a power-cycle is worse than no delete (the file
    // ghosts back).
    if (auto s = send_packet_bypassing_block_list(
            *channel_, 0x05U, std::span<std::uint8_t const>{});
        !s.has_value()) {
        return s;
    }
    auto barrier_ack = receive_packet_body();
    if (!barrier_ack.has_value()) {
        return st::failure(std::move(barrier_ack).error());
    }
    return st::ok();
#endif
}

Status Client::nand_barrier_best_effort() {
#ifndef ST_HAVE_AP_WORKFLOW
    return st::failure(
        ErrorCode::PolicyDenied,
        "ap3::nand_barrier_best_effort: AP workflow off; not touching wire.");
#else
    // No warmup, no env-var gate. The caller knows this is the
    // disconnect path so we send cmd 0x05 unconditionally and let any
    // error propagate. Bypasses the codec-level block list because
    // cmd 0x05 IS on the defense-in-depth list — the workflow flag is
    // the outer gate.
    if (auto s = send_packet_bypassing_block_list(
            *channel_, 0x05U, std::span<std::uint8_t const>{});
        !s.has_value()) {
        return s;
    }
    auto ack = receive_packet_body();
    if (!ack.has_value()) {
        return st::failure(std::move(ack).error());
    }
    return st::ok();
#endif
}

Result<std::string> Client::query_currently_flashed() {
#ifndef ST_HAVE_AP_WORKFLOW
    return st::failure(
        ErrorCode::PolicyDenied,
        "ap3::query_currently_flashed (cmd 0x1a show_cur): AP workflow "
        "surface is off in this build.");
#else
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(s.error());
    }
    // Body = Boost-archived vector<string>{"show_cur no_comms"} per
    // findings/re-2026-06-14/cmd_1a_onruntask_format.md: 35-byte
    // envelope + u32 LE vector_size (=1) + u32 LE str_len + bytes.
    auto body = encode_runtask_body("show_cur no_comms");
    if (!body.has_value()) {
        return st::failure(body.error());
    }
    if (auto s = send_packet(0x1aU, *body); !s.has_value()) {
        return st::failure(s.error());
    }
    // Read chunked response packets until we see the terminator. The
    // firmware emits one packet per stdout line of the show_cur shell
    // script; the final packet body is the 17-byte literal
    // "[RUNTASK_FINISH]" (with optional trailing newline). We accept
    // both the 16-byte bare form and the 17-byte form for robustness
    // across firmware revisions.
    constexpr std::string_view kTerminator16{"[RUNTASK_FINISH]"};
    std::string accumulated;
    // Cap the accumulator at 64 KB to avoid an unbounded read if the
    // AP's firmware regresses and never emits the terminator.
    constexpr std::size_t kAccumulatorCap = 64 * 1024;
    while (accumulated.size() < kAccumulatorCap) {
        auto chunk = receive_packet_body();
        if (!chunk.has_value()) {
            return st::failure(chunk.error());
        }
        // Convert bytes to string for terminator check.
        std::string_view chunk_sv{
            reinterpret_cast<char const *>(chunk->data()), chunk->size()};
        // Trim trailing newline for the terminator compare so both
        // "[RUNTASK_FINISH]" and "[RUNTASK_FINISH]\n" match.
        std::string_view trimmed = chunk_sv;
        while (!trimmed.empty() &&
               (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.remove_suffix(1);
        }
        if (trimmed == kTerminator16) {
            break;
        }
        accumulated.append(chunk_sv);
    }
    // Parse the `reflash=` line.
    std::string_view view{accumulated};
    while (!view.empty()) {
        auto const nl = view.find('\n');
        std::string_view line = (nl == std::string_view::npos)
                                     ? view
                                     : view.substr(0, nl);
        // Trim CR.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        constexpr std::string_view kPrefix{"reflash="};
        if (line.size() > kPrefix.size() &&
            line.substr(0, kPrefix.size()) == kPrefix) {
            std::string value{line.substr(kPrefix.size())};
            // Trim trailing whitespace.
            while (!value.empty() &&
                   (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return value;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        view.remove_prefix(nl + 1);
    }
    return st::failure(ErrorCode::ParseError,
                        "ap3::query_currently_flashed: show_cur response "
                        "did not contain a `reflash=` line.");
#endif
}

Result<std::vector<std::uint8_t>> Client::pull_marriage_backup() {
#ifndef ST_HAVE_AP_WORKFLOW
    return st::failure(
        ErrorCode::PolicyDenied,
        "ap3::pull_marriage_backup: AP workflow surface is off in this "
        "build.");
#else
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(s.error());
    }
    // Step 1: list /dump/ — file may already be staged from a previous
    // expose_user invocation.
    auto find_enc_rom =
        [this]() -> Result<std::optional<std::string>> {
        auto entries = ls("/dump/");
        if (!entries.has_value()) {
            return st::failure(entries.error());
        }
        for (auto const &e : *entries) {
            // _enc.rom is the canonical suffix; calid prefix varies by
            // VIN (e.g. SUBA_US_WRXM_CF_17_E_84551_enc.rom).
            constexpr std::string_view kSuffix{"_enc.rom"};
            if (e.name.size() >= kSuffix.size() &&
                e.name.compare(e.name.size() - kSuffix.size(),
                                kSuffix.size(), kSuffix) == 0) {
                return std::optional<std::string>{e.name};
            }
        }
        return std::optional<std::string>{};
    };
    auto found = find_enc_rom();
    if (!found.has_value()) {
        return st::failure(found.error());
    }
    // Step 2: if not already there, trigger expose_user.
    if (!found->has_value()) {
        auto body = encode_runtask_body("expose_user");
        if (!body.has_value()) {
            return st::failure(body.error());
        }
        if (auto s = send_packet(0x1aU, *body); !s.has_value()) {
            return st::failure(s.error());
        }
        // Drain chunks until [RUNTASK_FINISH]. Same loop shape as
        // query_currently_flashed; we discard the stdout body since
        // expose_user's output is just status messages (the rom file
        // it produces is fetched separately via cmd 0x21).
        constexpr std::string_view kTerminator16{"[RUNTASK_FINISH]"};
        constexpr std::size_t kDrainCap = 256 * 1024;
        std::size_t total = 0;
        for (;;) {
            auto chunk = receive_packet_body();
            if (!chunk.has_value()) {
                return st::failure(chunk.error());
            }
            total += chunk->size();
            if (total > kDrainCap) {
                return st::failure(
                    ErrorCode::ParseError,
                    "ap3::pull_marriage_backup: expose_user output cap "
                    "exceeded; terminator never observed.");
            }
            std::string_view chunk_sv{
                reinterpret_cast<char const *>(chunk->data()),
                chunk->size()};
            std::string_view trimmed = chunk_sv;
            while (!trimmed.empty() && (trimmed.back() == '\n' ||
                                          trimmed.back() == '\r')) {
                trimmed.remove_suffix(1);
            }
            if (trimmed == kTerminator16) {
                break;
            }
        }
        // Step 3: re-list /dump/ to pick up the newly-staged filename.
        found = find_enc_rom();
        if (!found.has_value()) {
            return st::failure(found.error());
        }
        if (!found->has_value()) {
            return st::failure(
                ErrorCode::ParseError,
                "ap3::pull_marriage_backup: expose_user completed but no "
                "_enc.rom appeared in /dump/. AP_STATE may not be "
                "Installed/Recovery.");
        }
    }
    // Step 4: pull the file via the standard ReadFile pipeline.
    std::string const ap_path = std::string{"/dump/"} + **found;
    return read_file(ap_path);
#endif
}

Status Client::remount_user_filesystem() {
    if (!ap3_diagnostic_cmds_enabled()) {
        return st::failure(
            st::ErrorCode::PolicyDenied,
            "ap3::remount_user_filesystem (cmd 0x05): diagnostic cmds are off "
            "by default — set ST_ETS_ENABLE_DIAGNOSTIC_CMDS=1 to enable. See "
            "RE wave 4 + dispatch_table_CORRECTED.md; the firmware-version "
            "uncertainty applies — live behavior may vary across AP builds.");
    }
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return s;
    }
    if (auto s = send_packet_bypassing_block_list(
            *channel_, 0x05U, std::span<std::uint8_t const>{});
        !s.has_value()) {
        return s;
    }
    auto ack = receive_packet_body();
    if (!ack.has_value()) {
        return st::failure(std::move(ack).error());
    }
    return st::ok();
}

Result<ApCapabilities> Client::get_capabilities() {
    if (!ap3_diagnostic_cmds_enabled()) {
        return st::failure(
            st::ErrorCode::PolicyDenied,
            "ap3::get_capabilities (cmd 0x1f): diagnostic cmds are off by "
            "default — set ST_ETS_ENABLE_DIAGNOSTIC_CMDS=1 to enable. ζ5 "
            "pinned the response schema as a Boost-archived u32; on "
            "v1.7.6.0-28785 the value is 0x1BB (firmware-version fingerprint).");
    }
    if (auto s = ensure_session_warmup(); !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    // Cmd 0x1f is in the safe range (not on is_blocked_command), so
    // the normal send_packet path is fine. The env-var gates because
    // the per-bit semantics of the returned flag word are TBD; the
    // raw u32 is currently useful only as a fingerprint.
    if (auto s = send_packet(0x1fU, std::span<std::uint8_t const>{});
        !s.has_value()) {
        return st::failure(std::move(s).error());
    }
    auto body = receive_packet_body();
    if (!body.has_value()) {
        return st::failure(std::move(body).error());
    }
    ApCapabilities out;
    out.raw_body = std::move(*body);
    // Decode the u32 capability word as little-endian from the last
    // 4 bytes of the body. ζ5: the analyst pinned the schema as
    // Boost-archived but didn't disclose the exact archive layout;
    // the last 4 bytes is a defensive position that works on the
    // v1.7.6.0-28785 reference + would still extract a u32 from any
    // Boost variant that places integers at the payload tail. Short
    // bodies degrade to flags=0 — caller can inspect raw_body.
    if (out.raw_body.size() >= 4) {
        std::size_t const n = out.raw_body.size();
        out.flags = static_cast<std::uint32_t>(out.raw_body[n - 4]) |
                    (static_cast<std::uint32_t>(out.raw_body[n - 3]) << 8) |
                    (static_cast<std::uint32_t>(out.raw_body[n - 2]) << 16) |
                    (static_cast<std::uint32_t>(out.raw_body[n - 1]) << 24);
    }
    return out;
}

} // namespace st::devices::ets
