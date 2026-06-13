// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::Client — file-vault client for the COBB
// AccessPort V3 (USB-only).
//
// Built on top of an IByteChannel that owns the USB bulk endpoints,
// plus the wire codec in st::transport::ets and the VaultFileMetadata
// reader/writer in st::devices::ets. The client knows how to issue
// each command pair (setup + data for ReadFile / PutFile) and parse
// the response, but is itself I/O-agnostic — the channel handles
// fragmentation.
//
// Trademark posture: the type name and CLI surface use `Ap3` /
// `ap3`. Vendor / product names appear in docs/comments where they
// identify the third-party hardware being interoperated with, never
// as type names or CLI tokens. Per docs/15 §12.
//
// Spec: D:\Subuwu\specs\references\cobb-ap3-usb-protocol.md §6.

#ifndef ST_DEVICES_AP3_CLIENT_HPP
#define ST_DEVICES_AP3_CLIENT_HPP

#include "st/core/result.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/transport/byte_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::devices::ets {

// path_type byte for the file-vault tree (`/user/ap-user/`). Covers
// `/maps/`, `/datalog/`, `/presets/`, `/images/`, `/settings`,
// `/backupcksum`. Spec §6.5.
inline constexpr std::uint8_t kPathTypeUserFs = 0x02;

// Wire-level virtual filename the device serves for its persisted ROM-
// backup checksum pseudo-file. The literal stays on the wire (we have
// to send the bytes the firmware recognizes); call sites reference it
// through this constant so the OEM string lives in exactly one place
// in the public source.
inline constexpr std::string_view kVirtualBackupChecksumPath = "backupcksum";

// Captured state of a connected AP3, populated by query_state().
struct DeviceState {
    // Raw response bodies — parsing the inner Boost-archived strings
    // for serial / firmware / marriage is best-effort at the moment;
    // see notes on the parsed_* fields below. Callers that need the
    // raw bytes (e.g. round-trip diagnostics) can read these directly.
    std::vector<std::uint8_t> user_info_body;       // cmd 0x28 response
    std::vector<std::uint8_t> firmware_body;        // cmd 0x04 response
    std::vector<std::uint8_t> device_settings_body; // cmd 0x03 response
    // Three additional status-query responses, populated when the AP
    // firmware supports the cmd byte (v1.7.6.0-28785 confirmed via
    // RE8b CORRECTED dispatch-table extraction). A non-supporting
    // firmware emits a default-handler error packet for these; the
    // parser degrades to nullopt rather than failing query_state.
    std::vector<std::uint8_t> hardware_type_body;        // cmd 0x2e response
    std::vector<std::uint8_t> vehicle_manufacturer_body; // cmd 0x30 response
    std::vector<std::uint8_t> ap_manufacturer_body;      // cmd 0x31 response

    // Best-effort parses extracted from the raw bodies above. Empty
    // when the leading Boost-archive prefix is present but the
    // payload shape past it doesn't match what we can decode yet.
    //
    // The current spec is silent on the exact byte layout of these
    // response bodies past the 39-byte archive prefix; the
    // implementer-side decoder treats anything below as advisory.
    std::optional<std::string> ap_serial;
    std::optional<std::string> firmware_version;
    std::optional<std::string> vehicle_descriptor;
    // Parsed ASCII payloads from the three RE8b status cmds. Same
    // treatment as `firmware_version` (printable-ASCII span trim).
    std::optional<std::string> hardware_type;
    std::optional<std::string> vehicle_manufacturer;
    std::optional<std::string> ap_manufacturer;

    // Marriage state per spec §15. `std::nullopt` means
    // "unknown / not yet parsed" — distinct from `false`
    // ("Not Installed"). Default policy: the host-side caller refuses
    // to operate when the value is `false`; nullopt warns but
    // proceeds (since blocking on a TBD parse would brick the
    // capability for every user).
    std::optional<bool> vehicle_paired;
};

struct ClientConfig {
    // Per-call timeout for IByteChannel reads/writes on metadata
    // commands (state / ls / setup ACKs etc). Small responses; 2s is
    // generous.
    std::chrono::milliseconds io_timeout{std::chrono::milliseconds{2000}};
    // Wider timeout for cmd 0x21 ReadFile DATA responses, since the
    // AP can take noticeably longer to push the body of a large
    // (50+ KB) tune file than the 2s metadata budget allows. 30s is
    // ~2x what APManager observably uses, with headroom for slow
    // flash reads on the AP side.
    std::chrono::milliseconds file_data_io_timeout{std::chrono::milliseconds{30000}};
};

// cmd 0x1f OnCapabilities response, decoded per ζ5: the AP firmware
// returns a Boost-archived u32 capability-flag word + the raw body
// bytes (preserved verbatim so the caller can re-decode if the
// schema changes in a future firmware revision).
struct ApCapabilities {
    // The decoded capability bits. On v1.7.6.0-28785 this is the
    // constant 0x000001BB (443) — 7 bits set: 0, 1, 3, 4, 5, 7, 8.
    // The per-bit semantics are TBD pending a Frida probe of
    // APManager-side reads; until then treat the value as an opaque
    // firmware-version fingerprint.
    std::uint32_t flags{0};
    // Raw response body so the caller can sanity-check the parse or
    // re-decode against a future schema. The decoder treats the
    // last 4 bytes of `raw_body` as the u32 in little-endian — if a
    // future Boost archive layout puts the integer elsewhere, the
    // bytes are still here to re-extract.
    std::vector<std::uint8_t> raw_body;
};

class Client {
public:
    Client(st::transport::IByteChannel &channel, ClientConfig cfg = {}) noexcept
        : channel_{&channel}, cfg_{cfg} {}

    // cmd 0x28 + 0x04 + 0x03 — gather serial, firmware, settings.
    [[nodiscard]] Result<DeviceState> query_state();

    // cmd 0x26 — list a subdirectory under /user/ap-user/, e.g.
    // "/maps/", "/datalog/", "/presets/", "/images/".
    [[nodiscard]] Result<std::vector<FileInfo>> ls(std::string_view subdir);

    // cmd 0x20 setup + 0x21 data — read a file by its absolute path
    // under /user/ap-user/. e.g. "/maps/Stage0.ptm". Returns the
    // file's raw bytes.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_file(std::string_view path);

    // cmd 0x22 setup + 0x23 data — write a file to the AP. `path` is
    // absolute under /user/ap-user/. `mtime_unix_secs` is the
    // intended mtime the AP should record; defaults to 0.
    [[nodiscard]] Status write_file(std::string_view path,
                                    std::span<std::uint8_t const> data,
                                    std::uint64_t mtime_unix_secs = 0);

    // cmd 0x25 — remove a file by absolute path.
    [[nodiscard]] Status remove_file(std::string_view path);

    // Diagnostic cmds — env-var gated, off by default.
    // ST_ETS_ENABLE_DIAGNOSTIC_CMDS=1 unlocks both methods; without it
    // they return PolicyDenied without touching the wire. The
    // firmware-version uncertainty (RE wave 3 dispatch table extracted
    // from binary v1.7.6.0-28785) means these are NOT guaranteed safe
    // on other AP builds.
    //
    // remount_user_filesystem (cmd 0x05) bypasses the codec-level
    // block list internally — useful as a "flush filesystem caches"
    // step after a bulk-write batch. Per RE wave 3 corrected dispatch
    // table this maps to ap::Filesystem::remountUser, not reboot.
    //
    // get_capabilities (cmd 0x1f) maps to OnCapabilities per the
    // corrected dispatch table; ζ5 pinned the response schema as a
    // Boost-archived u32 capability flag word. On firmware
    // v1.7.6.0-28785 the value is the constant 0x1BB (443) per the
    // literal pool at `libap_comms.so:0x24228`, with 7 bits set:
    // 0 / 1 / 3 / 4 / 5 / 7 / 8. The bit→meaning table is TBD; the
    // u32 is useful today as a firmware-version fingerprint.
    [[nodiscard]] Status remount_user_filesystem();
    [[nodiscard]] Result<ApCapabilities> get_capabilities();

private:
    [[nodiscard]] Status send_packet(std::uint8_t type, std::span<std::uint8_t const> body);
    [[nodiscard]] Result<std::vector<std::uint8_t>> receive_packet_body();
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_exact(std::size_t n);
    // Idle-driven accumulator for cmd 0x21 DATA when the strict
    // wire_len approach fails (see ST_ETS_READFILE_DRAIN_MODE in
    // client.cpp). Reads in 512-byte chunks until `idle_threshold`
    // has elapsed without new data, capped at `file_data_io_timeout`.
    // Returns the body (header + CRC stripped); accepts a zero CRC
    // trailer per the §3 sentinel rule. Validates the leading wire
    // header (sync magic + reserved byte + type).
    [[nodiscard]] Result<std::vector<std::uint8_t>> receive_packet_body_drain_mode();

    // Lazy session warmup. The analyst's reference toolkit always
    // sends cmd 0x28 (UserInfo) before any file-vault command, and
    // APManager (per Frida) does the same. The hypothesis is that the
    // AP firmware tracks per-session state and rejects file-vault
    // commands until UserInfo has been read at least once. This guard
    // fires query_state() the first time ls/read_file/write_file/
    // remove_file is called and caches the result; subsequent calls
    // are no-ops. See findings/handoffs/HANDOFF-from-analyst-2026-06-11-
    // dispatcher-default-likely-stale-in.md hypothesis #2.
    [[nodiscard]] Status ensure_session_warmup();

    st::transport::IByteChannel *channel_;
    ClientConfig cfg_;
    bool session_warmed_up_ = false;
};

// Helper: split an absolute "/dir/.../file" path into the
// (name, path) pair the AP expects in VaultFileMetadata records. `/maps/Foo.ptm`
// → name="Foo.ptm", path="/maps/". `/backupcksum` → name="backupcksum",
// path="/".
struct SplitPath {
    std::string name;
    std::string path;
};
[[nodiscard]] SplitPath split_ap_path(std::string_view absolute_path);

// Parsed cmd 0x28 (UserInfo) response payload — the 6 newline-separated
// fields the AP firmware emits per spec §6.13.
struct UserInfoFields {
    std::optional<std::string> firmware_version;
    std::optional<std::string> ap_product_code;
    std::optional<std::string> ap_serial;
    std::optional<bool> vehicle_paired;
    std::optional<std::string> vehicle;
};

// Decode the cmd 0x28 (UserInfo) response body. Returns a partially-
// populated struct on best-effort parse failure (e.g. truncated /
// shape mismatch — every field is std::optional, all nullopt means
// nothing extractable). Per spec §6.13 — see also
// `fixtures/ap3/cmd28_userinfo_response.bin` for the canonical example.
[[nodiscard]] UserInfoFields parse_user_info_body(std::span<std::uint8_t const> body);

// Extract the marriage flag from the cmd 0x03 DeviceSettings response
// body. Per RE2 (findings/re-2026-06-12-pm/), the AP's persistent
// settings store is `/root/settings`, a plain ASCII INI with no
// marriage field — marriage state lives ONLY in the cmd 0x28
// UserInfo response (already parsed by `parse_user_info_body`). The
// cmd 0x03 binary layout could not be decoded without fresh live
// captures, and analysts confirmed no marriage byte is hiding there.
//
// This hook is retained as a forward-compat seam: a future firmware
// revision might add a marriage byte to cmd 0x03 (carrier-flagged or
// otherwise), in which case the body becomes a one-line edit and the
// `query_state` fallback already wired here picks up the new signal.
// For now it returns `std::nullopt` unconditionally — callers MUST
// keep treating `parse_user_info_body` as authoritative.
[[nodiscard]] std::optional<bool>
parse_marriage_from_device_settings(std::span<std::uint8_t const> body);

} // namespace st::devices::ets

#endif // ST_DEVICES_AP3_CLIENT_HPP
