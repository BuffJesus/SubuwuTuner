// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// AP3 browser panel — file-vault GUI for the COBB AccessPort V3.
// CLI parity: `subuwutuner-cli ap3 {state, ls, pull, push, rm, backup}`,
// wrapped in a docking panel with one row per file. Each row has
// inline Pull/Remove buttons + a right-click context menu surfacing
// the composite verbs (Pull+Inspect, Pull+Import) that route .ptm
// pulls straight into the existing PTM modals. Toolbar buttons cover
// push + "Mirror to library".
//
// State + I/O are file-static so the panel is self-contained — AppState
// only carries the visibility bool. Most operations are still
// synchronous (a 70 KB pull completes in <1 s on a working AP, so a
// brief UI freeze is acceptable for v1). The Mirror-to-library verb
// is the exception — it walks every file in /maps/ + /datalog/ +
// /presets/ + /images/ + /settings + /backupcksum (~30 s on a full
// AP) and routes through `ets::Client::read_file_async` so the GUI
// stays frame-responsive throughout; progress + cancel are surfaced
// inline. See docs/41 for the worker-queue model.
//
// Per CLAUDE.md/docs/15 §12 trademark posture: the on-screen text uses
// "AccessPort" (the third-party product name, identifying the hardware
// being interoperated with), but code-side types and the underlying
// transport stay namespaced `Ap3` / `ap3`.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/config.hpp"
#include "st/core/result.hpp"
#include "st/devices/ets/client.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/library/filename_classifier.hpp"
#include "st/library/tune_index.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/ets_channel.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

namespace {

constexpr std::array<char const *, 4> kSubdirs{"/maps/", "/datalog/", "/presets/", "/images/"};

enum class StatusSeverity { None, Ok, Warn, Error };

// A4 — persistent status log entry. set_status() pushes one of these
// into the rolling history (capped) so users can review what just
// happened across multiple ops without staring at the panel during
// each one. `reveal_path`, when non-empty, renders an "Open in
// Explorer" link next to the entry; populated by pull/push/mirror
// completions whose result is a file or directory.
struct StatusEntry {
    StatusSeverity severity;
    std::string msg;
    std::filesystem::path reveal_path;
    std::chrono::system_clock::time_point when;
};

struct PanelState {
    std::unique_ptr<st::transport::IByteChannel> channel;
    std::optional<st::devices::ets::DeviceState> device_state;
    std::string status_msg;
    StatusSeverity status_severity{StatusSeverity::None};

    // A4 — rolling history of status updates. Capped at kStatusLogCap
    // (oldest dropped). Toggled by `show_status_log`; renders below
    // the inline status chip. Most-recent-first ordering — natural for
    // a "what just happened" log; matches the standard chat-style UX.
    std::deque<StatusEntry> status_log;
    bool show_status_log{false};

    // A2 — cached TuneIndex for the per-row "Tune" metadata column.
    // Lazy-loaded on first render of the file table; cached for the
    // session. nullopt + tune_index_load_attempted=true signals "tried
    // and failed / not present" — column degrades to "-" cells.
    std::optional<st::library::TuneIndex> tune_index;
    bool tune_index_load_attempted{false};
    // basename → entry-index map for O(1) lookup per row. Rebuilt
    // whenever the index loads. Empty when the index has no .ptm rows.
    std::map<std::string, std::size_t> tune_index_by_basename;

    std::string current_subdir{kSubdirs[0]};
    std::map<std::string, std::vector<st::devices::ets::FileInfo>> listings;
    std::map<std::string, std::string> listing_errors; // per-subdir last error
    // Per-subdir substring filter. Match is case-insensitive against the
    // basename only. Empty → show all rows. Per-subdir state so flipping
    // tabs doesn't clobber a useful filter on the previous tab.
    std::map<std::string, std::array<char, 128>> file_filter;
    // Device-tab singleton-file snapshots. Pulled on first Device-tab
    // open; cached for the session. RE2 confirmed /settings is plain
    // ASCII INI so we render parsed key=value rows; /backupcksum is a
    // 32-char ASCII MD5 (possibly with a trailing newline). Both are
    // small (≤ a few KB) so caching the raw text is cheap.
    std::optional<std::string> device_settings_text;
    std::optional<std::string> device_backupcksum;
    // T22 / S1 — live "currently flashed" answer from cmd 0x1a
    // OnRunTask(show_cur). Populated by query_currently_flashed on
    // first connect; cached for the session. Empty optional means
    // not-yet-queried or query failed (we render the library-side
    // lookup instead in that case).
    std::optional<std::string> live_currently_flashed;
    bool live_currently_flashed_attempted{false};
    std::string device_fetch_error;
    bool device_tab_fetched{false};

    bool allow_unpaired{false};
    // Per-frame: true when the user has the Device tab selected so the
    // shared toolbar + file-table below the tab bar skip rendering.
    // Reset at the start of every render_subdir_tabs call.
    bool device_tab_active{false};
    // Latched when any AP operation returns a daze-signature error
    // (TransportTimeout, LIBUSB_ERROR_PIPE). Renders the daze-recovery
    // card at the top of the panel until the user acknowledges or
    // resets the connection. Per docs/install.md + docs/34 §4.2, the
    // only known fix is physical unplug + replug.
    bool daze_suspected{false};

    // Cached "AccessPort detected" probe — refreshed at most once per
    // ~1 second so we don't libusb_get_device_list every frame.
    bool last_detect{false};
    std::chrono::steady_clock::time_point last_detect_at{};

    // A5 — "Mirror to library" async state. Drained per-frame by
    // tick_mirror(). Replaces the old synchronous backup_all() walk
    // with an enqueued batch of read_file_async ops that don't freeze
    // the UI. See docs/41 §6 for the worker-queue interaction model.
    struct MirrorOp {
        std::future<st::Result<std::vector<std::uint8_t>>> fut;
        std::filesystem::path dest;
        std::string ap_path; // for status only
    };
    struct MirrorState {
        bool in_progress{false};
        std::vector<MirrorOp> pending;
        std::filesystem::path root;
        std::size_t completed_files{0};
        std::size_t total_files{0};
        std::uint64_t total_bytes{0};
        std::size_t errors{0};
        std::shared_ptr<std::stop_source> stop_src;
        std::chrono::steady_clock::time_point started_at{};
    };
    MirrorState mirror;

    // F4 — async marriage-backup pull state. One pull in flight at a
    // time per panel. fut.valid()==false means idle. dest_when_done is
    // populated at start so the polling loop knows where to write the
    // result. Errors get stamped into status_msg via the usual chip.
    struct BackupPullState {
        std::future<st::Result<std::vector<std::uint8_t>>> fut;
        std::filesystem::path dest;
        std::chrono::steady_clock::time_point started_at{};
    };
    BackupPullState backup_pull;
};

PanelState &panel() {
    static PanelState s;
    return s;
}

// A4 — cap chosen so the log fits comfortably on screen + bounds
// memory growth across long sessions. Drop-oldest behaviour.
constexpr std::size_t kStatusLogCap = 64;

void push_status_entry(PanelState &p, StatusSeverity sev, std::string msg,
                       std::filesystem::path reveal = {}) {
    StatusEntry e{sev, std::move(msg), std::move(reveal),
                  std::chrono::system_clock::now()};
    p.status_log.push_front(std::move(e));
    while (p.status_log.size() > kStatusLogCap) {
        p.status_log.pop_back();
    }
}

void set_status(PanelState &p, StatusSeverity sev, std::string msg) {
    push_status_entry(p, sev, msg);
    p.status_msg = std::move(msg);
    p.status_severity = sev;
}

// A4 — variant for ops whose result is a file or directory. Both the
// inline chip and the log entry get the reveal path attached so an
// "Open in Explorer" link renders next to either.
void set_status_with_path(PanelState &p, StatusSeverity sev,
                          std::string msg, std::filesystem::path reveal) {
    push_status_entry(p, sev, msg, reveal);
    p.status_msg = std::move(msg);
    p.status_severity = sev;
}

// A4 — best-effort cross-platform "show this in the file manager".
// Windows: `explorer /select,<path>` highlights the file/dir.
// POSIX: `xdg-open <dir>` opens the containing folder (no per-file
// selection without per-DE-specific shims; close enough).
void reveal_in_explorer(std::filesystem::path const &p) {
    if (p.empty()) {
        return;
    }
    // Defensive: reject paths containing characters that could break
    // the shell-quoted form below. mirror_root_for already sanitizes
    // the per-AP serial; filenames from the AP listing in practice are
    // basenames without these chars (the AP firmware itself disallows
    // most via the upload path), but belt-and-suspenders here.
    auto const str = p.string();
    if (str.find('"') != std::string::npos ||
        str.find('\n') != std::string::npos ||
        str.find('\r') != std::string::npos) {
        return;
    }
#ifdef _WIN32
    // Quote the path; the comma after /select must be unescaped.
    std::string cmd = "explorer /select,\"" + str + "\"";
    std::system(cmd.c_str());
#else
    auto const dir = std::filesystem::is_directory(p) ? p : p.parent_path();
    std::string cmd = "xdg-open \"" + dir.string() + "\" &";
    std::system(cmd.c_str());
#endif
}

// Daze-signature detection. Per docs/install.md "Troubleshooting" and
// docs/34 §4.2, a wedged AP firmware presents as one of:
//   - LIBUSB_ERROR_TIMEOUT (bulk OUT hangs past the 5 s deadline)
//   - LIBUSB_ERROR_PIPE (bulk IN gets a stall)
// Both map to st::ErrorCode::TransportTimeout in our translation
// layer; we also string-sniff for the libusb literals as belt-and-
// suspenders against translation drift.
[[nodiscard]] bool error_looks_like_daze(st::Error const &err) {
    if (err.code() == st::ErrorCode::TransportTimeout) {
        return true;
    }
    auto const msg = err.to_string();
    return msg.find("TIMEOUT") != std::string::npos ||
           msg.find("PIPE") != std::string::npos ||
           msg.find("timeout") != std::string::npos;
}

// Call sites: every AP op that can fail with a daze signature passes
// the error through here. Latches `daze_suspected` once set; cleared
// by the recovery card's button or a successful reconnect.
void note_possible_daze(PanelState &p, st::Error const &err) {
    if (error_looks_like_daze(err)) {
        p.daze_suspected = true;
    }
}

void clear_status(PanelState &p) {
    p.status_msg.clear();
    p.status_severity = StatusSeverity::None;
}

// A4 — chip color picker, factored so both the inline status and the
// history log entries render with the same severity → color mapping.
void render_status_chip(StatusSeverity sev, char const *msg) {
    switch (sev) {
    case StatusSeverity::Ok:
        chip(msg, chip_fg_ok(), chip_bg_ok());
        break;
    case StatusSeverity::Warn:
        chip(msg, chip_fg_caution(), chip_bg_caution());
        break;
    case StatusSeverity::Error:
        chip(msg, chip_fg_danger(), chip_bg_danger());
        break;
    case StatusSeverity::None:
        text_subtle("%s", msg);
        break;
    }
}

void render_status(PanelState &p) {
    // Most recent entry first, then a "Show log" toggle that expands a
    // scrollable history of prior ops + per-row "Open in Explorer".
    bool const has_chip = !p.status_msg.empty();
    if (has_chip) {
        render_status_chip(p.status_severity, p.status_msg.c_str());
        // Most recent entry's reveal path lives at the head of the log
        // when set_status_with_path populated it; render an inline
        // Open-in-Explorer link next to the chip when present.
        if (!p.status_log.empty() && !p.status_log.front().reveal_path.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open in Explorer##ap3_open_recent")) {
                reveal_in_explorer(p.status_log.front().reveal_path);
            }
        }
    }
    if (p.status_log.empty()) {
        return;
    }
    if (has_chip) {
        ImGui::SameLine();
    }
    char const *toggle_label = p.show_status_log ? "Hide log##ap3_log_toggle"
                                                  : "Show log##ap3_log_toggle";
    if (ImGui::SmallButton(toggle_label)) {
        p.show_status_log = !p.show_status_log;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rolling history of the last %zu AP operations.",
                          kStatusLogCap);
    }
    if (!p.show_status_log) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (ImGui::BeginChild("##ap3_status_log",
                          ImVec2(0.0f, 180.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::size_t row = 0;
        for (auto const &entry : p.status_log) {
            ImGui::PushID(static_cast<int>(row++));
            std::time_t const t =
                std::chrono::system_clock::to_time_t(entry.when);
            char ts[16];
            std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
            text_subtle("%s", ts);
            ImGui::SameLine();
            render_status_chip(entry.severity, entry.msg.c_str());
            if (!entry.reveal_path.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open in Explorer")) {
                    reveal_in_explorer(entry.reveal_path);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// Cheap presence probe, refreshed at ~1 Hz. Surfaces the Connect button
// state and (downstream) the welcome-panel hint.
bool ets_detected(PanelState &p) {
    auto const now = std::chrono::steady_clock::now();
    if (now - p.last_detect_at > std::chrono::seconds{1}) {
        p.last_detect = st::transport::ets::detect_present();
        p.last_detect_at = now;
    }
    return p.last_detect;
}

void disconnect(PanelState &p) {
    // T14 — fire cmd 0x05 as a NAND barrier before tearing the
    // channel down so any pending UBIFS journal entries from this
    // session commit to NAND. APManager mirrors exactly this pattern.
    // Best-effort: a failure here doesn't block the disconnect, and
    // the channel will be reset right after regardless. Only runs
    // when the workflow flag is on (the helper returns PolicyDenied
    // without touching the wire otherwise).
    if (p.channel != nullptr) {
        st::devices::ets::Client client{*p.channel};
        (void)client.nand_barrier_best_effort();
    }
    p.device_state.reset();
    p.channel.reset();
    p.listings.clear();
    p.listing_errors.clear();
    // Drop Device-tab cache on disconnect so the next session's first
    // visit re-pulls /settings + /backupcksum fresh. If the user
    // replaced the AP between sessions the cached blob would be stale.
    p.device_settings_text.reset();
    p.device_backupcksum.reset();
    p.device_fetch_error.clear();
    p.device_tab_fetched = false;
    p.live_currently_flashed.reset();
    p.live_currently_flashed_attempted = false;
}

void connect(PanelState &p) {
    clear_status(p);
    // Clear daze flag on connect — if the user reached the Connect
    // button, the AP enumerated (libusb_get_device_list found it), so
    // we're starting clean. A failed channel-open below will re-set
    // the flag if the new error matches the daze signature.
    p.daze_suspected = false;
    auto channel = st::transport::ets::open_channel();
    if (!channel.has_value()) {
        note_possible_daze(p, channel.error());
        set_status(p, StatusSeverity::Error, channel.error().to_string());
        return;
    }
    p.channel = std::move(*channel);
    st::devices::ets::Client client{*p.channel};
    auto state = client.query_state();
    if (!state.has_value()) {
        note_possible_daze(p, state.error());
        set_status(p, StatusSeverity::Error, "query_state: " + state.error().to_string());
        p.channel.reset();
        return;
    }
    p.device_state = std::move(*state);
    // T22 — once per connect, ask the AP what tune is currently
    // flashed (cmd 0x1a OnRunTask show_cur). Best-effort: a failure
    // doesn't block the rest of the connect — we just fall back to
    // the library-side md5 cross-ref in the connect pane.
    p.live_currently_flashed.reset();
    p.live_currently_flashed_attempted = true;
    if (auto flashed = client.query_currently_flashed();
        flashed.has_value()) {
        p.live_currently_flashed = std::move(*flashed);
    }
    set_status(p, StatusSeverity::Ok, "Connected.");
    // Eager-fetch the default subdir's listing so the panel doesn't
    // open empty.
    auto records = client.ls(p.current_subdir);
    if (records.has_value()) {
        p.listings[p.current_subdir] = std::move(*records);
        p.listing_errors.erase(p.current_subdir);
    } else {
        note_possible_daze(p, records.error());
        p.listing_errors[p.current_subdir] = records.error().to_string();
    }
}

// Forward-decl: A5's helper. Defined further down next to the mirror
// state machine; A3 (immediately below) anchors the per-AP cache at
// the same root so composite verbs and full mirrors agree on layout.
[[nodiscard]] std::filesystem::path mirror_root_for(PanelState const &p);
[[nodiscard]] std::filesystem::path audit_log_path_for(PanelState const &p);
void audit_log_append(PanelState const &p, std::string_view action,
                       std::string_view ap_path, std::uint64_t bytes_or_zero);
struct AuditRow;
[[nodiscard]] std::vector<AuditRow>
audit_log_tail(PanelState const &p, std::size_t max_rows);

// F4 — backup-ROM-pull lifecycle helpers.
[[nodiscard]] std::filesystem::path backup_pull_dest_for(PanelState const &p);
void start_backup_pull(PanelState &p);
void tick_backup_pull(PanelState &p);

// A3 — composite verbs (Pull + Inspect / Pull + Import). Pulls the
// file straight into the per-AP mirror cache under
// `library_root/ap-mirror/<serial>/<subdir>/<basename>` so the source
// path persists for the downstream modal + is automatically scannable
// by the library inventory tool. Returns the local destination path on
// success, or empty path on any error (status is set internally).
[[nodiscard]] std::filesystem::path
pull_into_mirror_cache(PanelState &p,
                       st::devices::ets::FileInfo const &rec) {
    if (p.channel == nullptr) {
        return {};
    }
    auto root = mirror_root_for(p);
    std::string sub_name = p.current_subdir;
    if (!sub_name.empty() && sub_name.front() == '/') {
        sub_name.erase(0, 1);
    }
    if (!sub_name.empty() && sub_name.back() == '/') {
        sub_name.pop_back();
    }
    auto dest = root / sub_name / rec.name;
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        set_status(p, StatusSeverity::Error,
                   "Couldn't create mirror dir " +
                       dest.parent_path().string() + ": " + ec.message());
        return {};
    }
    set_status(p, StatusSeverity::Ok,
               "Pulling " + rec.name + " → " + dest.string() + " …");
    st::devices::ets::Client client{*p.channel};
    auto bytes = client.read_file(rec.path + rec.name);
    if (!bytes.has_value()) {
        note_possible_daze(p, bytes.error());
        set_status(p, StatusSeverity::Error,
                   "Pull failed: " + bytes.error().to_string());
        return {};
    }
    std::ofstream o{dest, std::ios::binary};
    if (!o) {
        set_status(p, StatusSeverity::Error,
                   "Pull succeeded but cannot open " + dest.string() +
                       " for write.");
        return {};
    }
    o.write(reinterpret_cast<char const *>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    return dest;
}

void pull_and_inspect(PanelState &p, AppState &state,
                       st::devices::ets::FileInfo const &rec) {
    auto const dest = pull_into_mirror_cache(p, rec);
    if (dest.empty()) {
        return;
    }
    std::snprintf(state.ptm_inspect_ptm_path,
                  sizeof state.ptm_inspect_ptm_path, "%s",
                  dest.string().c_str());
    state.show_ptm_inspect_modal = true;
    set_status_with_path(p, StatusSeverity::Ok,
                         "Pulled + opened in Inspect: " + dest.string(), dest);
}

void pull_and_import(PanelState &p, AppState &state,
                      st::devices::ets::FileInfo const &rec) {
    auto const dest = pull_into_mirror_cache(p, rec);
    if (dest.empty()) {
        return;
    }
    std::snprintf(state.ptm_import_ptm_path,
                  sizeof state.ptm_import_ptm_path, "%s",
                  dest.string().c_str());
    state.show_ptm_import_modal = true;
    set_status_with_path(p, StatusSeverity::Ok,
                         "Pulled + opened in Import: " + dest.string(), dest);
}

void refresh_subdir(PanelState &p, std::string const &subdir) {
    if (p.channel == nullptr) {
        return;
    }
    st::devices::ets::Client client{*p.channel};
    auto records = client.ls(subdir);
    if (records.has_value()) {
        p.listings[subdir] = std::move(*records);
        p.listing_errors.erase(subdir);
    } else {
        note_possible_daze(p, records.error());
        p.listing_errors[subdir] = records.error().to_string();
    }
}

void pull_file(PanelState &p, st::devices::ets::FileInfo const &rec) {
    if (p.channel == nullptr) {
        return;
    }
    nfdu8filteritem_t const filters[] = {{"AccessPort tune (.ptm)", "ptm"},
                                         {"Any file", "*"}};
    NFD::UniquePathU8 out;
    nfdresult_t const r = NFD::SaveDialog(out, filters, 2, nullptr, rec.name.c_str());
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || out == nullptr) {
        set_status(p, StatusSeverity::Error, std::string{"File dialog: "} + NFD::GetError());
        return;
    }
    set_status(p, StatusSeverity::Ok,
               "Pulling " + rec.name + " (this may take a few seconds for large tunes)...");
    st::devices::ets::Client client{*p.channel};
    auto bytes = client.read_file(rec.path + rec.name);
    if (!bytes.has_value()) {
        note_possible_daze(p, bytes.error());
        // Translate the underlying error into an actionable hint.
        // TransportTimeout on cmd 0x21 is the most common failure on
        // the AP — usually a path-encoding bug or a daze recovery
        // window — so flag that case specifically. The full daze
        // recovery card now renders at the top of the panel via
        // note_possible_daze above; the status hint stays as a
        // shorter inline pointer.
        std::string msg = "Pull failed: " + bytes.error().to_string();
        auto const code = bytes.error().code();
        if (code == st::ErrorCode::TransportTimeout) {
            msg += "\n  See the daze-recovery card at the top of the panel.";
        } else if (code == st::ErrorCode::PolicyDenied) {
            msg += "\n  Hint: the cmd is on the spec §6.0 block list. This is"
                   " a SubuwuTuner-side refusal, not the AP.";
        }
        set_status(p, StatusSeverity::Error, std::move(msg));
        return;
    }
    std::ofstream o{std::filesystem::path{out.get()}, std::ios::binary};
    if (!o) {
        set_status(p, StatusSeverity::Error,
                   "Pull succeeded but cannot open output file at " +
                       std::string{out.get()} +
                       "\n  Hint: check the directory exists and isn't "
                       "read-only.");
        return;
    }
    o.write(reinterpret_cast<char const *>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    set_status_with_path(
        p, StatusSeverity::Ok,
        "Pulled " + std::to_string(bytes->size()) + " bytes -> " +
            std::string{out.get()},
        std::filesystem::path{out.get()});
}

void push_file(PanelState &p) {
    if (p.channel == nullptr) {
        return;
    }
    nfdu8filteritem_t const filters[] = {{"AccessPort tune (.ptm)", "ptm"},
                                         {"Any file", "*"}};
    NFD::UniquePathU8 in;
    nfdresult_t const r = NFD::OpenDialog(in, filters, 2);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || in == nullptr) {
        set_status(p, StatusSeverity::Error, std::string{"File dialog: "} + NFD::GetError());
        return;
    }
    std::filesystem::path local{in.get()};
    std::ifstream f{local, std::ios::binary | std::ios::ate};
    if (!f) {
        set_status(p, StatusSeverity::Error, "push: couldn't open " + local.string());
        return;
    }
    auto const size = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char *>(bytes.data()), size);
    f.close();
    std::string const ap_path = p.current_subdir + local.filename().string();
    st::devices::ets::Client client{*p.channel};
    auto status = client.write_file(ap_path, bytes, static_cast<std::uint64_t>(std::time(nullptr)));
    if (!status.has_value()) {
        note_possible_daze(p, status.error());
        set_status(p, StatusSeverity::Error, "push: " + status.error().to_string());
        return;
    }
    set_status(p, StatusSeverity::Ok,
               "Pushed " + std::to_string(bytes.size()) + " bytes → AP:" + ap_path);
    audit_log_append(p, "push", ap_path, bytes.size());
    refresh_subdir(p, p.current_subdir);
}

void remove_file(PanelState &p, st::devices::ets::FileInfo const &rec) {
    if (p.channel == nullptr) {
        return;
    }
    st::devices::ets::Client client{*p.channel};
    auto status = client.remove_file(rec.path + rec.name);
    if (!status.has_value()) {
        note_possible_daze(p, status.error());
        set_status(p, StatusSeverity::Error, "rm: " + status.error().to_string());
        return;
    }
    set_status(p, StatusSeverity::Ok, "Removed AP:" + rec.path + rec.name);
    audit_log_append(p, "remove", rec.path + rec.name, 0);
    refresh_subdir(p, p.current_subdir);
}

// A5 — anchor mirror destination under the configured library_root so
// the pulled tree is automatically scannable by the library inventory
// tool. Subdir is `ap-mirror/<serial>/`, isolated per-AP so a user with
// two APs doesn't end up with collision-prone unified mirrors. Serial
// missing → fall back to "unknown-ap" (still useful, just less precise).
[[nodiscard]] std::filesystem::path mirror_root_for(PanelState const &p) {
    std::filesystem::path const lib = st::config::library_root();
    std::string serial = "unknown-ap";
    if (p.device_state.has_value() && p.device_state->ap_serial.has_value() &&
        !p.device_state->ap_serial->empty()) {
        serial = *p.device_state->ap_serial;
        // Defensive sanitize: strip path-hostile chars even though
        // SUB-prefixed serials are pure alphanumeric in practice.
        for (auto &c : serial) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
    }
    return lib / "ap-mirror" / serial;
}

// S3 — per-AP audit log. Each row is a tab-separated record of
// (timestamp, action, ap_path, bytes_or_dash) appended to a file
// under `<library_root>/ap-logs/<serial>.tsv` so a user with two APs
// gets independent histories. Cheap to read back: a few hundred
// rows total per AP in practice, even for an active user.
[[nodiscard]] std::filesystem::path audit_log_path_for(PanelState const &p) {
    std::filesystem::path const lib = st::config::library_root();
    std::string serial = "unknown-ap";
    if (p.device_state.has_value() && p.device_state->ap_serial.has_value() &&
        !p.device_state->ap_serial->empty()) {
        serial = *p.device_state->ap_serial;
        for (auto &c : serial) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
    }
    return lib / "ap-logs" / (serial + ".tsv");
}

// Append a single TSV row. Best-effort — failures (no disk, no
// permissions) don't propagate; the user sees nothing in the audit
// section in that case but the AP operation itself still succeeded.
void audit_log_append(PanelState const &p, std::string_view action,
                       std::string_view ap_path,
                       std::uint64_t bytes_or_zero) {
    auto const path = audit_log_path_for(p);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return;
    }
    std::ofstream out{path, std::ios::app | std::ios::binary};
    if (!out) {
        return;
    }
    std::time_t const now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S",
                   std::localtime(&now));
    out << ts << '\t' << action << '\t' << ap_path << '\t';
    if (bytes_or_zero > 0) {
        out << bytes_or_zero;
    } else {
        out << '-';
    }
    out << '\n';
}

// Read the most recent N audit rows. Returns oldest-first within
// the slice but the slice itself is the tail of the log. Empty
// vector means no log yet or read failure.
struct AuditRow {
    std::string ts;
    std::string action;
    std::string ap_path;
    std::string bytes;
};
[[nodiscard]] std::filesystem::path backup_pull_dest_for(PanelState const &p) {
    // Anchor under <library_root>/ap-backups/<serial>_<YYYYmmdd-HHMMSS>_enc.rom
    // so a user with two APs gets independent backup files and the
    // timestamp lets them keep multiple snapshots without overwrites.
    std::filesystem::path const lib = st::config::library_root();
    std::string serial = "unknown-ap";
    if (p.device_state.has_value() && p.device_state->ap_serial.has_value() &&
        !p.device_state->ap_serial->empty()) {
        serial = *p.device_state->ap_serial;
        for (auto &c : serial) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
    }
    std::time_t const now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&now));
    return lib / "ap-backups" /
            (serial + "_" + ts + "_enc.rom");
}

void start_backup_pull(PanelState &p) {
    if (p.channel == nullptr || p.backup_pull.fut.valid()) {
        return;
    }
    p.backup_pull.dest = backup_pull_dest_for(p);
    p.backup_pull.started_at = std::chrono::steady_clock::now();
    // Run pull_marriage_backup on a dedicated worker thread so the
    // GUI keeps repainting during the multi-step expose_user dance
    // (which can take a few seconds while the AP shells out to
    // rom_encrypter). The Client uses a single channel — we hold the
    // panel's channel pointer for the duration.
    auto *channel = p.channel.get();
    p.backup_pull.fut = std::async(std::launch::async,
        [channel]() -> st::Result<std::vector<std::uint8_t>> {
            st::devices::ets::Client client{*channel};
            return client.pull_marriage_backup();
        });
    set_status(p, StatusSeverity::Ok,
                "Pulling backup ROM (this can take a few seconds)...");
}

void tick_backup_pull(PanelState &p) {
    if (!p.backup_pull.fut.valid()) {
        return;
    }
    if (p.backup_pull.fut.wait_for(std::chrono::milliseconds{0}) !=
        std::future_status::ready) {
        return;
    }
    auto result = p.backup_pull.fut.get();
    if (!result.has_value()) {
        note_possible_daze(p, result.error());
        set_status(p, StatusSeverity::Error,
                    "Backup pull failed: " + result.error().to_string());
        return;
    }
    auto const &bytes = *result;
    auto const &dest = p.backup_pull.dest;
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        set_status(p, StatusSeverity::Error,
                    "Couldn't create backup dir " +
                        dest.parent_path().string() + ": " + ec.message());
        return;
    }
    std::ofstream out{dest, std::ios::binary};
    if (!out) {
        set_status(p, StatusSeverity::Error,
                    "Couldn't open " + dest.string() + " for write");
        return;
    }
    out.write(reinterpret_cast<char const *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    out.close();
    audit_log_append(p, "backup-pull", dest.string(), bytes.size());
    set_status_with_path(p, StatusSeverity::Ok,
                          "Saved backup ROM (" +
                              std::to_string(bytes.size()) +
                              " bytes encrypted) → " + dest.string(),
                          dest);
}

[[nodiscard]] std::vector<AuditRow>
audit_log_tail(PanelState const &p, std::size_t max_rows) {
    std::vector<AuditRow> rows;
    auto const path = audit_log_path_for(p);
    std::ifstream in{path};
    if (!in) {
        return rows;
    }
    std::string line;
    while (std::getline(in, line)) {
        AuditRow row;
        auto const t1 = line.find('\t');
        if (t1 == std::string::npos) {
            continue;
        }
        auto const t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) {
            continue;
        }
        auto const t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) {
            continue;
        }
        row.ts = line.substr(0, t1);
        row.action = line.substr(t1 + 1, t2 - t1 - 1);
        row.ap_path = line.substr(t2 + 1, t3 - t2 - 1);
        row.bytes = line.substr(t3 + 1);
        rows.push_back(std::move(row));
    }
    if (rows.size() > max_rows) {
        rows.erase(rows.begin(),
                    rows.begin() +
                        static_cast<std::ptrdiff_t>(rows.size() - max_rows));
    }
    return rows;
}

// A5 — kick off the mirror. Enumerates files via sync `ls` (4 quick
// listings on the worker), then enqueues `read_file_async` for every
// hit + the /settings + /backupcksum singletons. tick_mirror() drains
// completion per-frame. The worker queue serializes the actual reads
// at the protocol level (one bulk-pipe pair), but the GUI loop stays
// frame-responsive throughout — cancel + status updates remain live.
void start_mirror_to_library(PanelState &p) {
    if (p.channel == nullptr || p.mirror.in_progress) {
        return;
    }
    auto root = mirror_root_for(p);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        set_status(p, StatusSeverity::Error,
                   "Couldn't create mirror dir " + root.string() + ": " +
                       ec.message());
        return;
    }
    p.mirror.root = root;
    p.mirror.in_progress = true;
    p.mirror.completed_files = 0;
    p.mirror.total_files = 0;
    p.mirror.total_bytes = 0;
    p.mirror.errors = 0;
    p.mirror.pending.clear();
    p.mirror.stop_src = std::make_shared<std::stop_source>();
    p.mirror.started_at = std::chrono::steady_clock::now();

    st::devices::ets::Client client{*p.channel};
    auto stop_tok = p.mirror.stop_src->get_token();

    // Enumerate subdirs synchronously — each ls is <100 ms and we
    // need the file list to know what to enqueue. Worker queue
    // serializes the lse against any later async reads.
    for (auto sub : kSubdirs) {
        auto records = client.ls(sub);
        if (!records.has_value()) {
            ++p.mirror.errors;
            continue;
        }
        std::string sub_name{sub + 1};
        if (!sub_name.empty() && sub_name.back() == '/') {
            sub_name.pop_back();
        }
        auto sub_dir = root / sub_name;
        std::filesystem::create_directories(sub_dir, ec);
        for (auto const &rec : *records) {
            auto ap_path = rec.path + rec.name;
            p.mirror.pending.push_back(PanelState::MirrorOp{
                client.read_file_async(ap_path, stop_tok),
                sub_dir / rec.name,
                ap_path,
            });
        }
    }
    // /settings + /backupcksum singletons.
    for (auto const *singleton : {"settings", "backupcksum"}) {
        std::string ap_path = std::string{"/"} + singleton;
        p.mirror.pending.push_back(PanelState::MirrorOp{
            client.read_file_async(ap_path, stop_tok),
            root / singleton,
            ap_path,
        });
    }
    p.mirror.total_files = p.mirror.pending.size();
    set_status(p, StatusSeverity::Ok,
               "Mirroring " + std::to_string(p.mirror.total_files) +
                   " files → " + root.string() + " …");
}

// A5 — frame-tick drain. For each pending op that's ready, write its
// bytes to disk + count + drop. Stops the op stream when stop_token
// fires; the worker still completes the in-flight protocol exchange
// (single-stream pipe means we can't interrupt mid-packet without
// dazing the AP) but skips queued ops. Errors don't halt the batch —
// we count them and move on.
void tick_mirror(PanelState &p) {
    if (!p.mirror.in_progress) {
        return;
    }
    std::vector<PanelState::MirrorOp> remaining;
    remaining.reserve(p.mirror.pending.size());
    for (auto &op : p.mirror.pending) {
        // Cheap "is it done yet" peek — wait_for(0) returns immediately.
        if (op.fut.wait_for(std::chrono::milliseconds{0}) !=
            std::future_status::ready) {
            remaining.push_back(std::move(op));
            continue;
        }
        auto bytes = op.fut.get();
        if (!bytes.has_value()) {
            ++p.mirror.errors;
            ++p.mirror.completed_files;
            note_possible_daze(p, bytes.error());
            continue;
        }
        std::ofstream o{op.dest, std::ios::binary};
        if (!o) {
            ++p.mirror.errors;
            ++p.mirror.completed_files;
            continue;
        }
        o.write(reinterpret_cast<char const *>(bytes->data()),
                static_cast<std::streamsize>(bytes->size()));
        p.mirror.total_bytes += bytes->size();
        ++p.mirror.completed_files;
    }
    p.mirror.pending = std::move(remaining);

    if (p.mirror.pending.empty()) {
        bool const cancelled = p.mirror.stop_src &&
                               p.mirror.stop_src->stop_requested();
        auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - p.mirror.started_at).count();
        std::string msg;
        if (cancelled) {
            msg = "Mirror cancelled after " +
                  std::to_string(p.mirror.completed_files) + "/" +
                  std::to_string(p.mirror.total_files) + " files (" +
                  std::to_string(p.mirror.total_bytes) + " bytes, " +
                  std::to_string(elapsed) + "s) → " + p.mirror.root.string();
        } else {
            msg = "Mirrored " +
                  std::to_string(p.mirror.completed_files -
                                 p.mirror.errors) +
                  " files (" + std::to_string(p.mirror.total_bytes) +
                  " bytes, " + std::to_string(elapsed) + "s) → " +
                  p.mirror.root.string();
            if (p.mirror.errors > 0) {
                msg += "  [" + std::to_string(p.mirror.errors) +
                       " errors skipped]";
            }
            msg += "\n  Re-run `python tools/library_inventory/inventory.py` "
                   "to add the mirrored .ptm files to your library_index.toml.";
        }
        set_status_with_path(
            p,
            cancelled || p.mirror.errors > 0 ? StatusSeverity::Warn
                                              : StatusSeverity::Ok,
            std::move(msg), p.mirror.root);
        p.mirror.in_progress = false;
        p.mirror.stop_src.reset();
    }
}

void cancel_mirror(PanelState &p) {
    if (!p.mirror.in_progress || !p.mirror.stop_src) {
        return;
    }
    p.mirror.stop_src->request_stop();
}

// Renders the daze-recovery card when an AP op has tripped the
// daze-signature detector. Visually prominent (danger-color border +
// wrapped explanation + two action buttons) because the only known
// recovery requires physical unplug + replug. Idempotent — call from
// both connected and disconnected render paths.
void render_daze_recovery_card(PanelState &p) {
    if (!p.daze_suspected) {
        return;
    }
    bool const detected = ets_detected(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // Use BeginChild with a danger-tinted border so the card stands
    // out from the rest of the panel without needing a full popup.
    ImGui::PushStyleColor(ImGuiCol_Border, chip_fg_danger());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
    if (ImGui::BeginChild("##ets_daze_card", ImVec2(0.0f, 140.0f), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(chip_fg_danger(), "  AccessPort appears wedged");
        ImGui::TextWrapped(
            "An operation hit a timeout / pipe error, which usually means the "
            "AP firmware is wedged. Per docs/34 §4.2 the firmware permanently "
            "stalls on any malformed body until physical unplug + replug — "
            "libusb_clear_halt and libusb_reset_device do not recover it. "
            "This is a firmware-side problem, not a SubuwuTuner-side one.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        // Reconnect button — auto-tries connect() when the device is
        // detected. Greyed when no device enumerates so the user has
        // a clear "still unplugged" signal.
        if (!detected) {
            ImGui::BeginDisabled();
        }
        push_primary_button_colors();
        if (ImGui::Button("Reset connection##ets_daze_reset",
                          ImVec2(200.0f, 0.0f))) {
            disconnect(p);
            p.daze_suspected = false;
            if (ets_detected(p)) {
                connect(p);
            }
        }
        pop_primary_button_colors();
        if (!detected) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Dismiss##ets_daze_dismiss")) {
            p.daze_suspected = false;
        }
        ImGui::SameLine();
        if (detected) {
            chip("AP enumerates", chip_fg_ok(), chip_bg_ok());
        } else {
            chip("AP not enumerated — replug now", chip_fg_caution(),
                 chip_bg_caution());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
}

void render_disconnect_state(PanelState &p) {
    render_daze_recovery_card(p);
    bool const detected = ets_detected(p);
    if (detected) {
        chip("AccessPort detected", chip_fg_accent(), chip_bg_accent());
    } else {
        chip("No AccessPort enumerated", chip_fg_muted(), chip_bg_muted());
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    push_primary_button_colors();
    if (ImGui::Button("Connect##ap3_connect", ImVec2(180.0f, 32.0f))) {
        connect(p);
    }
    pop_primary_button_colors();
    if (ImGui::IsItemHovered() && !detected) {
        ImGui::SetTooltip(
            "No matching device found on USB. Plug in the AccessPort\n"
            "and rebind it to WinUSB via Zadig (Windows) — see\n"
            "docs/install.md → \"USB hardware setup\".");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    render_status(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("File vault, datalog pulls, and AP backups — opaque on the .ptm side.");
    text_subtle("Cipher introspection requires the gated build flag (see docs/34).");
}

void render_device_header(PanelState &p, AppState const &state) {
    auto const &s = *p.device_state;
    auto field = [](char const *label, std::string const &v) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(140.0f);
        ImGui::TextUnformatted(v.empty() ? "(unparsed)" : v.c_str());
    };
    field("Serial", s.ap_serial.value_or(""));
    field("Firmware", s.firmware_version.value_or(""));
    field("Vehicle", s.vehicle_descriptor.value_or(""));
    // RE8b CORRECTED status probes (cmd 0x2e / 0x30 / 0x31). Older
    // firmware may not implement these and the parsers degrade to
    // nullopt — only show the rows when the AP returned printable
    // ASCII, so an unsupported firmware doesn't pollute the header
    // with three "(unparsed)" lines.
    if (s.hardware_type.has_value()) {
        field("Hardware", *s.hardware_type);
    }
    if (s.vehicle_manufacturer.has_value()) {
        field("Veh. mfr.", *s.vehicle_manufacturer);
    }
    if (s.ap_manufacturer.has_value()) {
        field("AP mfr.", *s.ap_manufacturer);
    }

    ImGui::TextDisabled("Marriage");
    ImGui::SameLine(140.0f);
    if (s.vehicle_paired.has_value()) {
        if (*s.vehicle_paired) {
            chip("Installed", chip_fg_ok(), chip_bg_ok());
        } else {
            chip("Not Installed", chip_fg_danger(), chip_bg_danger());
        }
    } else {
        chip("unparsed", chip_fg_caution(), chip_bg_caution());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "The protocol spec doesn't pin down the byte offset of the\n"
                "Installed/Not Installed marker in the settings blob yet.\n"
                "Operations proceed with a warning. See docs/34.");
        }
    }

    // Agreement chip — L5: AP↔project vehicle match. Pulls AP's
    // machine vehicle_id (cmd 0x28 field[5]) + project's
    // [ptm_metadata].vehicle_id. Green when both present + match,
    // red when both present + differ, grey when either side
    // unknown.
    if (s.vehicle_id.has_value() && !s.vehicle_id->empty() &&
        state.ptm_imported_vehicle.has_value() &&
        !state.ptm_imported_vehicle->empty()) {
        ImGui::TextDisabled("Project match");
        ImGui::SameLine(140.0f);
        if (*s.vehicle_id == *state.ptm_imported_vehicle) {
            chip("AP matches project", chip_fg_ok(), chip_bg_ok());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("AP and project both target %s.\n"
                                  "Push is safe.",
                                  s.vehicle_id->c_str());
            }
        } else {
            chip("Vehicle mismatch — push blocked",
                 chip_fg_danger(), chip_bg_danger());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "AP vehicle: %s\nProject vehicle: %s\n\n"
                    "Push is blocked — a different ECU is married\n"
                    "to this AP than the project's source ROM targets.",
                    s.vehicle_id->c_str(),
                    state.ptm_imported_vehicle->c_str());
            }
        }
    }

    // T22 — live "currently flashed" answer from cmd 0x1a OnRunTask
    // show_cur. Authoritative when present (the AP knows what it
    // last flashed) and takes precedence over the library-side md5
    // cross-ref below. Renders only when the query produced a
    // value; a missing optional means the query wasn't run or
    // failed — we fall through to the library lookup.
    if (p.live_currently_flashed.has_value() &&
        !p.live_currently_flashed->empty()) {
        ImGui::TextDisabled("Currently flashed");
        ImGui::SameLine(140.0f);
        // The AP returns the full vault path
        // ("/user/ap-user/maps/<name>.ptm"); trim to the basename for
        // the headline display and keep the full path in the tooltip.
        std::string const &full = *p.live_currently_flashed;
        auto const slash = full.find_last_of('/');
        std::string const basename = (slash == std::string::npos)
                                          ? full
                                          : full.substr(slash + 1);
        ImGui::TextUnformatted(basename.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Reported by the AP itself via cmd 0x1a show_cur.\n"
                "Full path on AP: %s", full.c_str());
        }
        // Run the F2 classifier on the basename for a quick family /
        // variant cue right next to the headline.
        auto const cls = st::library::classify_ptm_filename(basename);
        std::string detail;
        auto append = [&detail](std::string const &tok) {
            if (tok.empty()) {
                return;
            }
            if (!detail.empty()) {
                detail += " \xC2\xB7 ";
            }
            detail += tok;
        };
        append(cls.vendor);
        append(cls.stage);
        append(cls.fuel_grade);
        append(cls.variant);
        if (!detail.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", detail.c_str());
        }
    }
    // Library-side cross-reference. Pinned to /backupcksum cached on
    // Device-tab open, matched against the loaded TuneIndex via
    // library_status_snapshot. Stays useful even when the show_cur
    // path above succeeded — they answer subtly different questions
    // (show_cur = last tunerflash; backupcksum = MD5 of the stock
    // ROM, which the library cross-refs to identify the unmodified
    // ROM family).
    else if (p.device_backupcksum.has_value()) {
        ImGui::TextDisabled("Currently flashed");
        ImGui::SameLine(140.0f);
        std::string md5{*p.device_backupcksum};
        while (!md5.empty() && (md5.back() == '\n' || md5.back() == '\r' ||
                                 md5.back() == ' ' || md5.back() == '\t')) {
            md5.pop_back();
        }
        auto const lib = library_status_snapshot();
        bool matched = false;
        if (lib.has_value() && !lib->currently_flashed_name.empty()) {
            ImGui::TextUnformatted(lib->currently_flashed_name.c_str());
            std::string detail;
            if (!lib->currently_flashed_vendor.empty()) {
                detail = lib->currently_flashed_vendor;
            }
            if (!lib->currently_flashed_stage.empty()) {
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += lib->currently_flashed_stage;
            }
            if (!lib->currently_flashed_fuel.empty()) {
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += lib->currently_flashed_fuel + " oct";
            }
            if (!lib->currently_flashed_variant.empty()) {
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += lib->currently_flashed_variant;
            }
            if (!detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", detail.c_str());
            }
            matched = true;
        }
        if (!matched) {
            std::string label = "unknown tune (MD5 ";
            label += md5.substr(0, 12);
            label += "\xE2\x80\xA6)"; // …
            ImGui::TextDisabled("%s", label.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "/backupcksum doesn't match any .ptm in the loaded\n"
                    "library_index.toml. Either no library is loaded,\n"
                    "the currently-flashed tune isn't in it, or this is\n"
                    "a custom tune with no library entry.\n\nFull MD5: %s",
                    md5.c_str());
            }
        }
    }

    // Build-mode chips — D2/D3 posture from the strategic decisions
    // handoff. The user shouldn't have to remember whether the build
    // they're running has cipher enabled; the header just tells them.
    // Both flags are compile-time, surfaced as ON/OFF via #ifdef so
    // we don't carry runtime state for a build-time decision.
    ImGui::TextDisabled("Build flags");
    ImGui::SameLine(140.0f);
#ifdef ST_AP3_HAVE_CIPHER
    chip("Cipher: ON", chip_fg_ok(), chip_bg_ok());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Built with -DST_ENABLE_COBB_AP_CIPHER=ON.\n"
                          ".ptm read path active (inspect / diff / import).");
    }
#else
    chip("Cipher: OFF", chip_fg_muted(), chip_bg_muted());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Rebuild with -DST_ENABLE_COBB_AP_CIPHER=ON for `.ptm`\n"
            "decryption support (inspect / diff / import).\n"
            "See docs/34-cobb-ap-as-tune-vault.md.");
    }
#endif
    ImGui::SameLine();
#ifdef ST_AP3_HAVE_PTM_REWRITE
    chip("PTM rewrite: ON", chip_fg_ok(), chip_bg_ok());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Built with -DST_ENABLE_COBB_AP_PTM_REWRITE=ON.\n"
                          "`.ptm` write/re-pack path active (ptm export).");
    }
#else
    chip("PTM rewrite: OFF", chip_fg_muted(), chip_bg_muted());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Rebuild with -DST_ENABLE_COBB_AP_PTM_REWRITE=ON for the\n"
            "`.ptm` write path (ptm export). Read path is independent\n"
            "and gated by the Cipher flag.");
    }
#endif
    ImGui::SameLine();
#ifdef ST_HAVE_AP_WORKFLOW
    chip("Workflow: ON", chip_fg_ok(), chip_bg_ok());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Built with -DST_ENABLE_COBB_AP_WORKFLOW=ON.\n"
            "AP push / pull / Save-and-Push surface active.");
    }
#else
    chip("Workflow: OFF", chip_fg_muted(), chip_bg_muted());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Rebuild with -DST_ENABLE_COBB_AP_WORKFLOW=ON to enable\n"
            "push / pull / Save-and-Push. Distribution default is OFF.\n"
            "See docs/34-cobb-ap-as-tune-vault.md §gating.");
    }
#endif

    // S1 — Maps row with family-grouped count. Cheap: walks the
    // already-cached /maps/ listing through the F2 classifier. Only
    // renders once the /maps/ tab has been visited at least once and
    // a listing was populated; before that we'd be printing "0" for
    // a tab the user hasn't asked about yet.
    auto const maps_it = p.listings.find(std::string{"/maps/"});
    if (maps_it != p.listings.end() && !maps_it->second.empty()) {
        std::map<std::string, std::size_t> by_vendor;
        std::size_t total_ptm = 0;
        for (auto const &rec : maps_it->second) {
            if (rec.name.size() < 4) {
                continue;
            }
            // is_ptm_file does case-insensitive .ptm tail check; we
            // mirror its predicate inline so we don't pull it across
            // a forward-decl just for this row.
            auto const tail = rec.name.substr(rec.name.size() - 4);
            std::string lc;
            lc.reserve(tail.size());
            for (char c : tail) {
                lc.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            if (lc != ".ptm") {
                continue;
            }
            ++total_ptm;
            auto const cls = st::library::classify_ptm_filename(rec.name);
            std::string const &bucket =
                cls.vendor.empty() ? std::string{"other"} : cls.vendor;
            by_vendor[bucket]++;
        }
        if (total_ptm > 0) {
            ImGui::TextDisabled("Maps");
            ImGui::SameLine(140.0f);
            ImGui::Text("%zu", total_ptm);
            // Render the family breakdown as a deemphasized aside on
            // the same line. Order: descending count. Cap at the top 5
            // buckets + "+N more" so a heavy AP doesn't overflow the
            // header.
            std::vector<std::pair<std::string, std::size_t>> sorted{
                by_vendor.begin(), by_vendor.end()};
            std::sort(sorted.begin(), sorted.end(),
                       [](auto const &a, auto const &b) {
                           return a.second > b.second;
                       });
            std::string breakdown;
            std::size_t shown = 0;
            for (auto const &kv : sorted) {
                if (shown >= 5) {
                    break;
                }
                if (!breakdown.empty()) {
                    breakdown += ", ";
                }
                breakdown += kv.first;
                breakdown += ": ";
                breakdown += std::to_string(kv.second);
                ++shown;
            }
            if (sorted.size() > 5) {
                breakdown += ", +";
                breakdown += std::to_string(sorted.size() - 5);
                breakdown += " more";
            }
            if (!breakdown.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", breakdown.c_str());
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (ImGui::SmallButton("Disconnect##ap3_disconnect")) {
        disconnect(p);
        clear_status(p);
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh state##ap3_refresh_state")) {
        st::devices::ets::Client client{*p.channel};
        auto fresh = client.query_state();
        if (fresh.has_value()) {
            p.device_state = std::move(*fresh);
        } else {
            note_possible_daze(p, fresh.error());
            set_status(p, StatusSeverity::Error, "query_state: " + fresh.error().to_string());
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Allow unmarried##ap3_allow_unpaired", &p.allow_unpaired);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When ON, suppresses the warning emitted when the marriage\n"
            "state can't be confirmed Installed. Forward-compatible with\n"
            "a future spec extension that gates by default.");
    }
    ImGui::SameLine();
    // F4 — pull marriage backup ROM via the T18 USB dance. Disabled
    // while a pull is in flight (one channel, one operation).
    bool const pull_in_flight = p.backup_pull.fut.valid();
    if (pull_in_flight) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("Pull backup ROM\xE2\x80\xA6##ap3_pull_backup")) {
        start_backup_pull(p);
    }
    if (pull_in_flight) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "USB-pull the AP's marriage backup ROM via cmd 0x1a expose_user\n"
            "+ cmd 0x20/0x21 (T18 dance). Saves the encrypted _enc.rom under\n"
            "<library>/ap-backups/. Decrypt happens offline.");
    }

    // S3 — per-AP audit log section. Collapsible: lazy load on
    // first expand so we don't read the file every frame. Capped at
    // 100 most-recent rows; users wanting the full history open the
    // log file in their editor via the "Reveal log" button.
    if (ImGui::CollapsingHeader("Audit log##ap3_audit_log")) {
        auto const rows = audit_log_tail(p, 100);
        if (rows.empty()) {
            ImGui::TextDisabled("No push / remove actions recorded yet for "
                                "this AP.");
        } else {
            if (ImGui::BeginTable("##ap3_audit_table", 4,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, 200.0f))) {
                ImGui::TableSetupColumn("When",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         150.0f);
                ImGui::TableSetupColumn("Action",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         70.0f);
                ImGui::TableSetupColumn(
                    "AP path", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Bytes",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         80.0f);
                ImGui::TableHeadersRow();
                // Render newest at the top so the user sees what they
                // just did first.
                for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(it->ts.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(it->action.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(it->ap_path.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(it->bytes.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Reveal log##ap3_audit_reveal")) {
            reveal_in_explorer(audit_log_path_for(p));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Open %s in the file manager.",
                audit_log_path_for(p).string().c_str());
        }
    }
}

// Pull /settings + /backupcksum from the AP into the panel-state cache.
// These are the two filesystem singletons (NOT under any subdir) that
// the Backup-all command already knows how to handle. Cache stays for
// the session; refresh-on-demand button in the Device tab re-fetches.
void fetch_device_singletons(PanelState &p) {
    if (p.channel == nullptr) {
        return;
    }
    p.device_settings_text.reset();
    p.device_backupcksum.reset();
    p.device_fetch_error.clear();
    p.device_tab_fetched = true;
    st::devices::ets::Client client{*p.channel};
    auto settings = client.read_file("/settings");
    if (settings.has_value()) {
        p.device_settings_text = std::string{
            reinterpret_cast<char const *>(settings->data()), settings->size()};
    } else {
        note_possible_daze(p, settings.error());
        p.device_fetch_error = "settings: " + settings.error().to_string();
    }
    auto cksum = client.read_file("/backupcksum");
    if (cksum.has_value()) {
        std::string s{reinterpret_cast<char const *>(cksum->data()), cksum->size()};
        // Strip trailing newline if present — the AP emits "<md5>\n"
        // per RE so the displayed value is the 32-char hex only.
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        p.device_backupcksum = std::move(s);
    } else {
        if (!p.device_fetch_error.empty()) {
            p.device_fetch_error += "; ";
        }
        p.device_fetch_error += "backupcksum: " + cksum.error().to_string();
    }
}

void render_device_tab(PanelState &p) {
    if (!p.device_tab_fetched) {
        fetch_device_singletons(p);
    }
    if (ImGui::SmallButton("Refresh##ap3_device_refresh")) {
        fetch_device_singletons(p);
    }
    ImGui::SameLine();
    text_subtle("/settings + /backupcksum — read-only, pulled on first visit.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // /backupcksum row — single MD5 string. This is the AP's claim
    // about what's currently flashed on the ECU; a future library
    // cross-reference (see HANDOFF-2026-06-13-ap-browser-ux Tier 1 #2)
    // matches it against locally-known tunes by hash.
    if (ImGui::CollapsingHeader("/backupcksum (currently-flashed identity)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (p.device_backupcksum.has_value()) {
            ImGui::TextDisabled("MD5");
            ImGui::SameLine(120.0f);
            ImGui::TextUnformatted(p.device_backupcksum->c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "The AP's MD5 of the currently-installed ROM. Library\n"
                    "matching against this hash is the next planned\n"
                    "browser feature — see the 2026-06-13 UX handoff.");
            }
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
            // Affordance: copy MD5 to clipboard for manual library matching
            // until the auto-match feature lands.
            if (ImGui::SmallButton("Copy MD5##ap3_copy_backupcksum")) {
                ImGui::SetClipboardText(p.device_backupcksum->c_str());
                set_status(p, StatusSeverity::Ok, "MD5 copied to clipboard.");
            }
        } else if (p.device_fetch_error.find("backupcksum") != std::string::npos) {
            chip("read failed", chip_fg_danger(), chip_bg_danger());
        } else {
            ImGui::TextDisabled("(not fetched yet)");
        }
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // /settings — plain ASCII INI per RE2 (no [section] headers, just
    // key=value lines). Render as a 2-column read-only table for the
    // typical debugging-use case ("what does the AP think about itself
    // right now"). Lines without '=' fall to a single-column "Comment"
    // row so we don't accidentally swallow non-key lines.
    if (ImGui::CollapsingHeader("/settings (device INI)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (p.device_settings_text.has_value()) {
            if (ImGui::BeginTable("##ap3_settings_tbl", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 240.0f))) {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                std::string_view const text{*p.device_settings_text};
                std::size_t row_idx = 0;
                std::size_t pos = 0;
                while (pos < text.size()) {
                    auto end = text.find('\n', pos);
                    if (end == std::string_view::npos) {
                        end = text.size();
                    }
                    std::string_view line = text.substr(pos, end - pos);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1);
                    }
                    pos = end + 1;
                    if (line.empty()) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(row_idx++));
                    auto const eq = line.find('=');
                    if (eq == std::string_view::npos) {
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("\xE2\x80\x94"); // em-dash
                        ImGui::TableSetColumnIndex(1);
                        std::string raw{line};
                        ImGui::TextUnformatted(raw.c_str());
                    } else {
                        std::string key{line.substr(0, eq)};
                        std::string value{line.substr(eq + 1)};
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(key.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(value.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        } else if (p.device_fetch_error.find("settings") != std::string::npos) {
            chip("read failed", chip_fg_danger(), chip_bg_danger());
        } else {
            ImGui::TextDisabled("(not fetched yet)");
        }
    }

    if (!p.device_fetch_error.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        chip(p.device_fetch_error.c_str(), chip_fg_danger(), chip_bg_danger());
    }
}

// A6 — render one chip-style tab button. Active state highlights with
// the accent palette; inactive uses the muted chip palette so the
// active selection reads at a glance. Right-side count badge shows
// the cached listing size (or "?" when unfetched yet).
void render_subdir_chip([[maybe_unused]] PanelState &p, char const *label,
                         bool active, char const *count_text,
                         std::function<void()> on_click) {
    ImVec4 const bg = active ? chip_bg_accent() : chip_bg_muted();
    ImVec4 const fg = active ? chip_fg_accent() : chip_fg_muted();
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    std::string display{label};
    if (count_text != nullptr && count_text[0] != '\0') {
        display += "  ";
        display += count_text;
    }
    if (ImGui::Button(display.c_str())) {
        on_click();
    }
    ImGui::PopStyleColor(4);
}

void render_subdir_tabs(PanelState &p) {
    // Reset per-frame; the Device chip click handler below sets it
    // true when Device is the active selection, which gates the
    // shared toolbar + file-table render outside this call site.
    p.device_tab_active = false;
    // current_subdir == kDeviceSubdirSentinel is the "Device" selection
    // (string chosen so it can never collide with a real /<x>/ subdir).
    constexpr char const *kDeviceSentinel = "::device::";
    bool const device_active = p.current_subdir == kDeviceSentinel;
    for (std::size_t i = 0; i < kSubdirs.size(); ++i) {
        char const *subdir = kSubdirs[i];
        bool const active = !device_active && p.current_subdir == subdir;
        // Compute the count badge — "n" when listing cached, "?" when
        // unfetched, "!" when last fetch errored.
        char count_buf[16];
        if (auto it = p.listings.find(subdir); it != p.listings.end()) {
            std::snprintf(count_buf, sizeof count_buf, "(%zu)",
                          it->second.size());
        } else if (p.listing_errors.find(subdir) != p.listing_errors.end()) {
            std::snprintf(count_buf, sizeof count_buf, "(!)");
        } else {
            std::snprintf(count_buf, sizeof count_buf, "(?)");
        }
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        render_subdir_chip(p, subdir, active, count_buf, [&]() {
            p.current_subdir = subdir;
            // Lazy fetch on first visit.
            if (p.listings.find(p.current_subdir) == p.listings.end() &&
                p.listing_errors.find(p.current_subdir) ==
                    p.listing_errors.end()) {
                refresh_subdir(p, p.current_subdir);
            }
        });
        ImGui::PopID();
    }
    // Device chip — surfaces the two filesystem singletons that aren't
    // under any of the canonical subdirs. Toggled via current_subdir =
    // kDeviceSentinel so the existing "current_subdir != X" guards on
    // refresh / read paths can't accidentally fire against it.
    ImGui::SameLine();
    ImGui::PushID("device_chip");
    render_subdir_chip(p, "Device", device_active, "", [&]() {
        p.current_subdir = kDeviceSentinel;
    });
    ImGui::PopID();
    if (p.current_subdir == kDeviceSentinel) {
        p.device_tab_active = true;
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        render_device_tab(p);
    }
}

// Format a raw byte count as a short human-readable string (e.g.
// "57.3 KB", "1.2 MB"). Single-decimal precision is the sweet spot
// for the tune-size range we typically see (1 KB - 2 MB).
void format_size(std::uint64_t bytes, char *out, std::size_t cap) {
    if (bytes < 1024) {
        std::snprintf(out, cap, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(out, cap, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(out, cap, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(out, cap, "%.2f GB", static_cast<double>(bytes) /
                                              (1024.0 * 1024.0 * 1024.0));
    }
}

// Heuristic: rows whose name ends in ".ptm" can be inspected / imported
// downstream via the existing ptm_inspect + ptm_import modals.
[[nodiscard]] bool is_ptm_file(std::string_view name) {
    return name.size() >= 4 &&
           name.substr(name.size() - 4) == ".ptm";
}

// A2 — lazy library-index load. Best-effort: failure (no index, parse
// error, schema mismatch) leaves `tune_index` empty + the row column
// degrades silently to "-". Per-frame guard is cheap (the load_attempted
// bool short-circuits after the first call).
void load_tune_index_lazy(PanelState &p) {
    if (p.tune_index_load_attempted) {
        return;
    }
    p.tune_index_load_attempted = true;
    auto const path = st::library::default_index_path();
    if (!path.has_value()) {
        return;
    }
    auto loaded = st::library::TuneIndex::load_from_file(*path);
    if (!loaded.has_value()) {
        return;
    }
    p.tune_index = std::move(*loaded);
    p.tune_index_by_basename.clear();
    auto const &entries = p.tune_index->ptm_entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto const &full = entries[i].path;
        auto const slash = full.find_last_of("/\\");
        std::string base = (slash == std::string::npos)
                                ? full
                                : full.substr(slash + 1);
        p.tune_index_by_basename.emplace(std::move(base), i);
    }
}

// A2 — find the library entry for a given AP-side row by basename. Path
// lives only on the local side; the AP listing carries just the name
// + path under /maps/ etc. Returns nullptr when the row doesn't match
// any indexed tune (or no index loaded).
[[nodiscard]] st::library::TuneIndexEntry const *
lookup_tune_for_row(PanelState const &p, std::string_view ap_basename) {
    if (!p.tune_index.has_value()) {
        return nullptr;
    }
    auto it = p.tune_index_by_basename.find(std::string{ap_basename});
    if (it == p.tune_index_by_basename.end()) {
        return nullptr;
    }
    return &p.tune_index->ptm_entries()[it->second];
}

void render_file_table(PanelState &p, AppState &state) {
    auto err_it = p.listing_errors.find(p.current_subdir);
    if (err_it != p.listing_errors.end()) {
        chip(("ls failed: " + err_it->second).c_str(), chip_fg_danger(), chip_bg_danger());
        return;
    }
    auto it = p.listings.find(p.current_subdir);
    if (it == p.listings.end() || it->second.empty()) {
        text_subtle("(empty)");
        return;
    }
    auto const &records = it->second;
    // Per-subdir filter input. Stored in the panel state so flipping
    // tabs and coming back doesn't clobber the filter. Substring match
    // is case-insensitive against the basename only.
    auto &filter_buf = p.file_filter[p.current_subdir];
    if (filter_buf[0] == '\0' && filter_buf.size() > 0) {
        // First touch — zero-init the buffer. Map's default-construct
        // gives us a value-initialized array but defensive-belt for the
        // tab-creation race.
        filter_buf[0] = '\0';
    }
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##ap3_file_filter", "Filter (substring)\xE2\x80\xA6",
                             filter_buf.data(), filter_buf.size());
    std::string_view const filter{filter_buf.data()};
    // A2 — load the library index lazily before rendering so the
    // "Tune" column lookup is cheap once the user scrolls or sorts.
    load_tune_index_lazy(p);
    if (ImGui::BeginTable("##ap3_files", 5,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                             ImGuiTableFlags_SizingStretchProp |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                         ImVec2(0, 280.0f))) {
        ImGui::TableSetupColumn("Name",
                                ImGuiTableColumnFlags_WidthStretch |
                                    ImGuiTableColumnFlags_DefaultSort,
                                0.0f, 0);
        ImGui::TableSetupColumn("Tune",
                                ImGuiTableColumnFlags_WidthFixed, 220.0f, 4);
        ImGui::TableSetupColumn("Size",
                                ImGuiTableColumnFlags_WidthFixed, 90.0f, 1);
        ImGui::TableSetupColumn("Modified",
                                ImGuiTableColumnFlags_WidthFixed, 180.0f, 2);
        ImGui::TableSetupColumn("Actions",
                                ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_NoSort,
                                160.0f, 3);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Build a view-only index permutation so we don't mutate the
        // underlying listings vector (AP-canonical, also read by
        // start_mirror_to_library). Filter first, then sort.
        std::vector<std::size_t> visible;
        visible.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (filter.empty() || icontains(records[i].name, filter)) {
                visible.push_back(i);
            }
        }
        if (auto const *specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0) {
            auto const &spec = specs->Specs[0];
            bool const asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::sort(visible.begin(), visible.end(),
                      [&](std::size_t a, std::size_t b) {
                          auto const &ra = records[a];
                          auto const &rb = records[b];
                          int cmp = 0;
                          switch (spec.ColumnUserID) {
                          case 0: // Name
                              cmp = ra.name.compare(rb.name);
                              break;
                          case 1: // Size
                              cmp = (ra.size < rb.size)   ? -1
                                    : (ra.size > rb.size) ? 1
                                                          : 0;
                              break;
                          case 2: // Modified
                              cmp = (ra.mtime < rb.mtime)   ? -1
                                    : (ra.mtime > rb.mtime) ? 1
                                                            : 0;
                              break;
                          default:
                              cmp = 0;
                              break;
                          }
                          return asc ? (cmp < 0) : (cmp > 0);
                      });
        }
        for (std::size_t vi : visible) {
            auto const &rec = records[vi];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(vi));
            ImGui::TableSetColumnIndex(0);
            // Make the name cell a selectable target so a right-click
            // anywhere on it opens the row context menu. (Selectable
            // doesn't reserve persistent selection state — we just use
            // it as a hit region.)
            ImGui::Selectable(rec.name.c_str(), false,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap);
            bool const ptm = is_ptm_file(rec.name);
            if (ImGui::BeginPopupContextItem("##ap3_row_ctx")) {
                if (!ptm) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Pull + Inspect\xE2\x80\xA6")) {
                    pull_and_inspect(p, state, rec);
                }
                if (ImGui::MenuItem("Pull + Import\xE2\x80\xA6")) {
                    pull_and_import(p, state, rec);
                }
                if (!ptm) {
                    ImGui::EndDisabled();
                }
                if (!ptm && ImGui::IsItemHovered(
                                ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Inspect + Import only apply to .ptm tunes.");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Pull (choose location)\xE2\x80\xA6")) {
                    pull_file(p, rec);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from AP\xE2\x80\xA6")) {
                    ImGui::OpenPopup("##ap3_rm_confirm");
                }
                ImGui::EndPopup();
            }
            // A2 + F2 — Tune column. Lookup order:
            //   1. Library index basename match → renders the
            //      cataloged vendor / stage / variant + path / md5
            //      tooltip (canonical source).
            //   2. Filename heuristic via st::library::classify_ptm_filename
            //      → renders the inferred fields with a tooltip noting
            //      "(filename heuristic)" so the user knows the data
            //      isn't from a cataloged tune.
            //   3. Em-dash for anything else.
            // The fallback means every .ptm — including fresh pushes
            // that haven't been re-inventoried — surfaces enough
            // identifier text to scan the list at a glance.
            ImGui::TableSetColumnIndex(1);
            if (ptm) {
                auto const *match = lookup_tune_for_row(p, rec.name);
                auto append = [](std::string &label, std::string const &s) {
                    if (s.empty()) {
                        return;
                    }
                    if (!label.empty()) {
                        label += " \xE2\x80\xA2 "; // bullet
                    }
                    label += s;
                };
                if (match != nullptr) {
                    std::string label;
                    append(label, match->vendor);
                    append(label, match->stage);
                    append(label, match->variant);
                    if (label.empty()) {
                        label = "(library hit)";
                    }
                    ImGui::TextUnformatted(label.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Library match by basename:\n%s\nmd5: %s",
                                          match->path.c_str(),
                                          match->md5.c_str());
                    }
                } else {
                    auto const inferred =
                        st::library::classify_ptm_filename(rec.name);
                    std::string label;
                    append(label, inferred.vendor);
                    append(label, inferred.stage);
                    append(label, inferred.fuel_grade);
                    append(label, inferred.variant);
                    if (label.empty()) {
                        ImGui::TextDisabled("\xE2\x80\x94");
                    } else {
                        ImGui::TextDisabled("%s", label.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Inferred from filename (no library entry "
                                "yet).\nRun the library inventory tool to "
                                "promote this row to a cataloged match.");
                        }
                    }
                }
            } else {
                ImGui::TextDisabled("\xE2\x80\x94");
            }
            ImGui::TableSetColumnIndex(2);
            char size_buf[32];
            format_size(rec.size, size_buf, sizeof size_buf);
            ImGui::TextUnformatted(size_buf);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%llu bytes", static_cast<unsigned long long>(rec.size));
            }
            ImGui::TableSetColumnIndex(3);
            if (rec.mtime != 0) {
                std::time_t t = static_cast<std::time_t>(rec.mtime);
                char buf[32];
                std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
                ImGui::TextUnformatted(buf);
            } else {
                ImGui::TextDisabled("\xE2\x80\x94");
            }
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Pull")) {
                pull_file(p, rec);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                ImGui::OpenPopup("##ap3_rm_confirm");
            }
            if (ImGui::BeginPopup("##ap3_rm_confirm")) {
                ImGui::Text("Remove %s%s from the AP?", rec.path.c_str(), rec.name.c_str());
                ImGui::TextDisabled("This is irreversible — the file is deleted on the device.");
                ImGui::Dummy(ImVec2(0.0f, kSpaceS));
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                push_primary_button_colors();
                if (ImGui::Button("Remove")) {
                    remove_file(p, rec);
                    ImGui::CloseCurrentPopup();
                }
                pop_primary_button_colors();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        // Footer status: filtered count vs total. Only render when the
        // filter is active so unfiltered views aren't cluttered.
        if (!filter.empty()) {
            text_subtle("Showing %zu of %zu.", visible.size(), records.size());
        }
    }
}

void render_toolbar(PanelState &p) {
    if (ImGui::Button("Push file\xE2\x80\xA6##ap3_push")) {
        push_file(p);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Upload a local file into the currently-selected\n"
                          "subdirectory on the AP.");
    }
    ImGui::SameLine();
    if (p.mirror.in_progress) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Mirror to library\xE2\x80\xA6##ap3_mirror")) {
        start_mirror_to_library(p);
    }
    if (p.mirror.in_progress) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        std::filesystem::path const dest = mirror_root_for(p);
        ImGui::SetTooltip("Pull every /maps, /datalog, /presets, /images file\n"
                          "+ /settings + /backupcksum into your library:\n"
                          "  %s\n"
                          "Async — the panel stays responsive. Re-run the\n"
                          "library inventory tool afterwards to index the\n"
                          "new .ptm files.",
                          dest.string().c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh##ap3_refresh_list")) {
        refresh_subdir(p, p.current_subdir);
    }
}

// A5 — mirror progress card. Renders an ImGui progress bar with the
// current file count + cancel button while a mirror is running.
// Idempotent and cheap — call from render_connected_state every frame.
void render_mirror_progress(PanelState &p) {
    if (!p.mirror.in_progress) {
        return;
    }
    auto const frac = p.mirror.total_files == 0
                          ? 0.0f
                          : static_cast<float>(p.mirror.completed_files) /
                                static_cast<float>(p.mirror.total_files);
    char overlay[96];
    std::snprintf(overlay, sizeof overlay, "%zu / %zu files",
                  p.mirror.completed_files, p.mirror.total_files);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
    if (ImGui::SmallButton("Cancel##ap3_mirror_cancel")) {
        cancel_mirror(p);
    }
    ImGui::SameLine();
    text_subtle("Mirror target: %s", p.mirror.root.string().c_str());
}

void render_connected_state(PanelState &p, AppState &state) {
    // Drain the async mirror queue first so progress + status reflect
    // the latest frame's worker output before we render anything.
    tick_mirror(p);
    tick_backup_pull(p);
    render_daze_recovery_card(p);
    render_device_header(p, state);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    render_subdir_tabs(p);
    // Toolbar + file table apply to filesystem-subdir tabs only —
    // the Device tab renders its own content inside its TabItem block.
    if (!p.device_tab_active) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        render_toolbar(p);
        if (p.mirror.in_progress) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
            render_mirror_progress(p);
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        render_file_table(p, state);
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    render_status(p);
}

} // namespace

bool ap3_browser_should_hint(AppState const & /*state*/) {
#ifndef ST_HAVE_AP_WORKFLOW
    // Workflow surface disabled in this build — no welcome-card hint.
    return false;
#else
    return panel().last_detect && panel().channel == nullptr;
#endif
}

void render_ap3_browser_panel(AppState &state) {
    if (!state.show_ap3_browser_panel) {
        return;
    }
#ifndef ST_HAVE_AP_WORKFLOW
    // Workflow surface disabled. Render a small notice in place of the
    // file vault, so a command-palette / menubar entry that survives
    // the gate (it shouldn't, but defense-in-depth) still produces a
    // legible UI rather than an empty window.
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("AccessPort", &state.show_ap3_browser_panel)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped(
        "The AccessPort workflow surface is disabled in this build. "
        "Reconfigure with -DST_ENABLE_COBB_AP_WORKFLOW=ON to enable "
        "AP browsing, push / pull, and Save & Push.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "See docs/34-cobb-ap-as-tune-vault.md for the gating rationale.");
    ImGui::End();
    return;
#else
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("AccessPort", &state.show_ap3_browser_panel)) {
        ImGui::End();
        return;
    }
#endif
    // F1 routing: when the AP browser is the focused window, F1 should
    // land on docs/34-cobb-ap-as-tune-vault (the public capability +
    // workflow doc for this surface). track_help_context only updates
    // when the panel is actually focused, so we don't clobber a
    // different panel's routing if the user just has the AP browser
    // open in a background tab.
    track_help_context(state, AppState::HelpContext::AccessPortBrowser);
    auto &p = panel();
    // Cheap presence probe (~1 Hz) so the welcome card + the
    // disconnect view header reflect plug/unplug without manual
    // refresh.
    (void)ets_detected(p);

    if (p.channel == nullptr) {
        render_disconnect_state(p);
    } else {
        render_connected_state(p, state);
    }
    ImGui::End();
}

std::optional<EtsStatusSnapshot> ets_status_snapshot() {
    auto const &p = panel();
    if (p.channel == nullptr || !p.device_state.has_value()) {
        return std::nullopt;
    }
    auto const &s = *p.device_state;
    EtsStatusSnapshot out;
    out.vehicle_descriptor = s.vehicle_descriptor.value_or("");
    out.vehicle_id = s.vehicle_id.value_or("");
    out.hardware_type = s.hardware_type.value_or("");
    out.vehicle_manufacturer = s.vehicle_manufacturer.value_or("");
    out.ap_manufacturer = s.ap_manufacturer.value_or("");
    out.paired_known = s.vehicle_paired.has_value();
    out.vehicle_paired = s.vehicle_paired.value_or(false);
    // Device-tab cache: /backupcksum is the MD5 of the plaintext stock
    // ROM (per J3 RE). Library panel cross-refs this against
    // TuneIndex::lookup_by_md5. Trim trailing whitespace / newline.
    if (p.device_backupcksum.has_value()) {
        std::string md5 = *p.device_backupcksum;
        while (!md5.empty() && (md5.back() == '\n' || md5.back() == '\r' ||
                                 md5.back() == ' ' || md5.back() == '\t')) {
            md5.pop_back();
        }
        if (!md5.empty()) {
            out.device_backupcksum = std::move(md5);
        }
    }
    // /maps/ basenames — the Library panel uses set-membership to flag
    // rows as "on AP". Reading from the cached listing avoids re-issuing
    // cmd 0x26 every frame.
    auto const it = p.listings.find(std::string{"/maps/"});
    if (it != p.listings.end()) {
        out.ap_maps_filenames.reserve(it->second.size());
        for (auto const &fi : it->second) {
            out.ap_maps_filenames.push_back(fi.name);
        }
    }
    return out;
}

bool ets_panel_has_channel() {
    return panel().channel != nullptr;
}

EtsPushResult ets_panel_push_to_maps(std::string filename,
                                      std::vector<std::uint8_t> bytes) {
    EtsPushResult out;
#ifndef ST_HAVE_AP_WORKFLOW
    (void)filename;
    (void)bytes;
    out.error =
        "AP workflow surface is off in this build. Reconfigure with "
        "-DST_ENABLE_COBB_AP_WORKFLOW=ON to enable Save & Push.";
    return out;
#else
    auto &p = panel();
    if (p.channel == nullptr) {
        out.error =
            "No AccessPort channel open. Connect to an AP first via the "
            "AccessPort Browser panel, then retry Save & Push.";
        return out;
    }
    if (filename.empty()) {
        out.error = "Refusing to push: empty filename.";
        return out;
    }
    // Mirror push_file's path-building convention (concatenate the
    // current /maps/ subdir + basename). Save & Push always targets
    // /maps/ regardless of which tab the user has open in the browser.
    std::string const ap_path = std::string{"/maps/"} + filename;
    st::devices::ets::Client client{*p.channel};
    auto status = client.write_file(
        ap_path, bytes, static_cast<std::uint64_t>(std::time(nullptr)));
    if (!status.has_value()) {
        note_possible_daze(p, status.error());
        out.error = status.error().to_string();
        return out;
    }
    // Refresh the /maps/ listing so the new entry shows immediately if
    // the user pivots to the AP browser to verify.
    refresh_subdir(p, std::string{"/maps/"});
    audit_log_append(p, "push", ap_path, bytes.size());
    out.ok = true;
    out.ap_path = ap_path;
    return out;
#endif
}

} // namespace st::ui
