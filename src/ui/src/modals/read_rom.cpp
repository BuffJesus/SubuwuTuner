// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Read ROM from Car modal. Drives a background std::thread that opens
// the picked adapter (OBDX / J2534 / native / trace), runs the SSM or
// UDS read loop via st::flash::Flasher, and reports progress via shared
// std::atomic counters back to the UI thread. Worker is joined on
// state-transition out of Running so threads don't leak across frames.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/adapter_picker.hpp"
#include "widgets/widgets.hpp"

#include "st/core/result.hpp"
#include "st/ecu/security_key.hpp"
#include "st/ecu/ssm.hpp"
#include "st/flash.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace st::ui {

void render_read_rom_modal(AppState &state) {
    if (state.show_read_rom_modal) {
        ImGui::OpenPopup("\xEE\xA2\x96  Read ROM from Car##read_rom_modal");
        state.show_read_rom_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // FIX (was: SetNextWindowSize + AlwaysAutoResize). The combination
    // of AlwaysAutoResize + TextWrapped inside the modal created a
    // shrink loop — each frame the auto-resize measured the wrapped
    // text's content width (smaller than the natural width), shrank
    // the window, which forced TextWrapped to wrap tighter on the
    // next frame, repeat. Visible as the modal walking right-to-left
    // until it pinned at minimum widget width.
    //
    // SizeConstraints with a 560 width floor prevents the shrink
    // (TextWrapped's wrap width is bounded below) while letting
    // height grow with content. Matches the pattern render_about_modal
    // and render_shortcuts_modal already use successfully.
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 240.0f),
                                        ImVec2(560.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("\xEE\xA2\x96  Read ROM from Car##read_rom_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // ----- transition: Running → Done/Failed/Cancelled -----
    // Worker writes the terminal status, we join here so the std::thread
    // doesn't sit zombified across frames. Join is cheap once the worker
    // has already finished its loop.
    if (state.read_rom_worker.joinable() &&
        state.read_rom_state != AppState::ReadRomState::Running) {
        state.read_rom_worker.join();
    }

    auto const elapsed_str = [&] {
        auto const dur = std::chrono::steady_clock::now() - state.read_rom_start_time;
        auto const sec = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
        long long mm = sec / 60, ss = sec % 60;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lldm %02llds", mm, ss);
        return std::string{buf};
    };

    // --------- Idle: form entry ---------
    if (state.read_rom_state == AppState::ReadRomState::Idle) {
        ImGui::TextUnformatted("Pulls a ROM dump from the connected ECU via the");
        ImGui::TextUnformatted("OBDX/J2534/native adapter. Read-only — no ECU writes.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
        ImGui::TextWrapped("Before clicking Read: ignition must be in ACC or RUN "
                           "(engine off is fine), nothing else (e.g. COBB AccessPort) "
                           "plugged into the OBD-II port, OBDX in the port. ECU is "
                           "asleep with key out — it won't answer.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));

        bool const adapter_ready = render_adapter_picker(state.read_rom_adapter);
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::InputText("Base address (hex)", state.read_rom_base_addr_hex,
                         sizeof state.read_rom_base_addr_hex);
        ImGui::InputText("Size (hex)", state.read_rom_size_hex, sizeof state.read_rom_size_hex);
        ImGui::Combo("Protocol", &state.read_rom_protocol,
                     "SSM2 (pre-2016 K-Line + early CAN)\0"
                     "UDS / SSM4 (2016+ WRX, FA20DIT, FA24)\0\0");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Wire-level protocol used to talk to the ECU.\n"
                "\n"
                "SSM2: Subaru Select Monitor 2 — A8 ReadByAddress.\n"
                "  Used by older EJ Subarus on K-Line and the early\n"
                "  CAN-era cars (≤ 2015 model year). Maps to the\n"
                "  dealer's SSMIII software in Subaru parlance.\n"
                "\n"
                "UDS / SSM4: ISO 14229 — SID 0x23 ReadMemoryByAddress.\n"
                "  Used by 2016+ Subarus (VA WRX 2016-2021, VB WRX\n"
                "  2022+, FA20DIT, FA24, Hitachi-built ECUs). Maps\n"
                "  to the dealer's SSM4 software, which is documented\n"
                "  as 'Generic OBDII of the Subaru Select Monitor 4'\n"
                "  in Subaru's own technician reference.\n"
                "\n"
                "Wrong choice = silent timeout (ECU drops the unknown\n"
                "SID without responding). For a 2015 VA WRX, try both:\n"
                "Subaru transitioned dealer software at MY 2016.");
        }
        ImGui::InputInt("Max chunk size (bytes)", &state.read_rom_max_chunk, 16, 64);
        if (state.read_rom_max_chunk < 16)
            state.read_rom_max_chunk = 16;
        if (state.read_rom_max_chunk > 0x1000)
            state.read_rom_max_chunk = 0x1000;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bytes per request.\n"
                              "SSM: caps at 80 internally (single-frame limit);\n"
                              "     larger values are silently clamped.\n"
                              "UDS: 256 is the safe Subaru SH-2A default;\n"
                              "     some adapters tolerate up to 4096.");
        }
        ImGui::Checkbox("Verbose console output", &state.read_rom_verbose);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When on, every TX/RX frame is hex-dumped to the\n"
                              "spawned console as `[trace][obdx-tx]` / `[obdx-rx]`.\n"
                              "Useful while debugging silent timeouts — you can\n"
                              "see whether bytes are leaving the adapter and\n"
                              "whether the ECU is responding at all.");
        }
        ImGui::Checkbox("Authenticate (UDS SecurityAccess)", &state.read_rom_authenticate);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When on, performs UDS SID 0x27 seed/key exchange before\n"
                "the read loop. Real Subaru ECUs reject ReadMemoryByAddress\n"
                "with NRC 0x33 (securityAccessDenied) until SA succeeds.\n"
                "\n"
                "Shipped default (2026-05-24+): real Gen-A.2 L1 algorithm\n"
                "(SH7058 / SSMCAN1, ~2008-2017 Subarus) recovered analyst-\n"
                "side from the plaintext flash. The plug-in seam still\n"
                "exists — see src/ecu/include/st/ecu/security_key.hpp to\n"
                "swap in Gen-B (RH850/AES) or a vendor-supplied algorithm.\n"
                "\n"
                "SSM protocol path does not consult this checkbox — SSM\n"
                "does not gate ReadByAddress behind seed/key.");
        }

        // SA-variant picker — surfaces the CLI's --sa-variant flag in
        // the GUI. Factory ECUs work with the default; COBB-tuned ECUs
        // need the COBB-AP key tables (the AP-install patches the SA
        // round-key constants in flash, so the factory algorithm
        // returns NRC 0x35 invalidKey on those cars). Pick L3 if the
        // ECU also gates RMBA at SA level 3 — the dropdown bumps the
        // security_level alongside the key fn.
        ImGui::BeginDisabled(!state.read_rom_authenticate);
        constexpr char const *kSaVariantNames[] = {
            "Factory L1",
            "COBB-AP L1",
            "COBB-AP L3",
        };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("SA variant##read_rom_sa_variant", &state.read_rom_sa_variant_idx,
                     kSaVariantNames, IM_ARRAYSIZE(kSaVariantNames));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!state.read_rom_authenticate) {
                ImGui::SetTooltip("Turn on Authenticate to pick a SA variant.");
            } else {
                ImGui::SetTooltip(
                    "Which SecurityAccess key function to use.\n"
                    "\n"
                    "Factory L1 — stock SH7058 / SSMCAN1 round-key table.\n"
                    "  Use on factory ECUs. NRC 0x35 invalidKey on any\n"
                    "  ECU that's had a COBB AccessPort install (the AP\n"
                    "  patches the round-key constants in flash).\n"
                    "\n"
                    "COBB-AP L1 — the COBB-AccessPort-substituted L1\n"
                    "  round-key table (recovered 2026-05-26 from\n"
                    "  captured install logs). Use on COBB-tuned ECUs.\n"
                    "  Equivalent to CLI: --sa-variant fehr-active.\n"
                    "\n"
                    "COBB-AP L3 — same algorithm, deeper diagnostic\n"
                    "  level (0x03 instead of 0x01). Pick this when the\n"
                    "  ECU rejects L1 (NRC 0x35) on a COBB-AP install\n"
                    "  — some firmwares route RMBA past L3 specifically.\n"
                    "  Equivalent to CLI: --sa-variant fehr-active-l3.\n"
                    "\n"
                    "If you don't know: try Factory L1 first. NRC 0x35\n"
                    "→ switch to COBB-AP L1. Still 0x35 → COBB-AP L3.");
            }
        }

        // Pre-run errors (typically a parse failure from a previous click).
        if (!state.read_rom_error_msg.empty()) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.0f, kSpaceM));

        bool const start_disabled = !adapter_ready;
        ImGui::BeginDisabled(start_disabled);
        if (ImGui::Button("Read", ImVec2(120.0f, 0.0f))) {
            // ----- parse form inputs into runtime values -----
            auto parse_hex_u32 = [](char const *s, std::uint32_t &out) {
                if (s == nullptr || s[0] == '\0')
                    return false;
                char *end = nullptr;
                unsigned long v = std::strtoul(s, &end, 0); // base=0 picks 0x or decimal
                if (end == s || *end != '\0')
                    return false;
                if (v > 0xFFFFFFFFul)
                    return false;
                out = static_cast<std::uint32_t>(v);
                return true;
            };
            std::uint32_t base_addr = 0, size = 0;
            if (!parse_hex_u32(state.read_rom_base_addr_hex, base_addr)) {
                state.read_rom_error_msg = "Base address: parse failed (use 0xNNN or decimal).";
            } else if (!parse_hex_u32(state.read_rom_size_hex, size)) {
                state.read_rom_error_msg = "Size: parse failed (use 0xNNN or decimal).";
            } else if (size == 0) {
                state.read_rom_error_msg = "Size must be > 0.";
            } else {
                bool const trace_mode = adapter_is_trace_mode(state.read_rom_adapter);
                // Real-transport branch builds a spec for open_transport;
                // trace branch carries the file path (worker loads it).
                st::transport::TransportSpec spec =
                    trace_mode ? st::transport::TransportSpec{}
                               : adapter_picker_to_spec(state.read_rom_adapter);
                std::string trace_path_str =
                    trace_mode ? std::string{state.read_rom_adapter.trace_path} : std::string{};

                state.read_rom_error_msg.clear();
                state.read_rom_bytes_result.clear();
                state.read_rom_bytes_done = std::make_shared<std::atomic<std::uint32_t>>(0);
                state.read_rom_total_bytes = std::make_shared<std::atomic<std::uint32_t>>(size);
                state.read_rom_cancel = std::make_shared<std::atomic<bool>>(false);
                state.read_rom_state = AppState::ReadRomState::Running;
                state.read_rom_start_time = std::chrono::steady_clock::now();

                AppState *st_ptr = &state;
                int const max_chunk = state.read_rom_max_chunk;
                int const protocol = state.read_rom_protocol;
                bool const verbose = state.read_rom_verbose;
                bool const authenticate = state.read_rom_authenticate;
                // Resolve the SA variant dropdown selection into a key
                // function + security_level pair. The CLI's --sa-variant
                // pairs the same way (level 0x03 only for the L3
                // variant). Done on the UI thread so the worker doesn't
                // need to know about the dropdown index encoding.
                st::ecu::SecurityKeyFn sa_fn = &st::ecu::subaru::ssmcan1_key_stub;
                std::uint8_t security_level = 0x01;
                switch (state.read_rom_sa_variant_idx) {
                case 1:
                    sa_fn = &st::ecu::subaru::ssmcan1_l1_cobb_ap;
                    security_level = 0x01;
                    break;
                case 2:
                    sa_fn = &st::ecu::subaru::ssmcan1_l3_cobb_ap;
                    security_level = 0x03;
                    break;
                case 0:
                default:
                    // already set
                    break;
                }
                auto bytes_done_sp = state.read_rom_bytes_done;
                auto total_sp = state.read_rom_total_bytes;
                auto cancel_sp = state.read_rom_cancel;

                state.read_rom_worker = std::thread([st_ptr, spec, trace_mode, trace_path_str,
                                                     base_addr, size, max_chunk, protocol, verbose,
                                                     authenticate, sa_fn, security_level,
                                                     bytes_done_sp, total_sp,
                                                     cancel_sp]() mutable {
                    // Owning storage. Trace-mode keeps a stack MockTransport;
                    // real-mode owns the unique_ptr from the factory.
                    st::transport::MockTransport mock;
                    std::unique_ptr<st::transport::ITransport> owned;
                    st::transport::ITransport *chosen = nullptr;

                    // Flip the process-wide OBDX hex-trace toggle for the
                    // duration of the worker; restore it on every exit
                    // path via the RAII guard below so a backgrounded
                    // datalog can't inherit a stuck-on trace flag.
                    bool const previous_trace = st::transport::obdx::trace_enabled();
                    st::transport::obdx::set_trace_enabled(verbose);
                    struct TraceGuard {
                        bool restore_to;
                        ~TraceGuard() { st::transport::obdx::set_trace_enabled(restore_to); }
                    } const trace_guard{previous_trace};

                    if (verbose) {
                        char const *const proto_name = protocol == 0 ? "SSM" : "UDS";
                        std::fprintf(stderr,
                                     "[trace][read-rom] starting %s read: "
                                     "base=0x%08X size=0x%X max_chunk=%d trace_mode=%d\n",
                                     proto_name, base_addr, size, max_chunk,
                                     trace_mode ? 1 : 0);
                        std::fflush(stderr);
                    }

                    if (trace_mode) {
                        std::vector<st::transport::UdsTracePair> pairs;
                        std::string err;
                        if (!st::transport::parse_uds_trace(std::filesystem::path{trace_path_str},
                                                            pairs, err)) {
                            st_ptr->read_rom_error_msg = std::move(err);
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        if (auto s = mock.open({}); !s.has_value()) {
                            st_ptr->read_rom_error_msg =
                                "MockTransport open failed: " + std::string{s.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        for (auto &p : pairs) {
                            mock.expect_send_recv(std::move(p.request), std::move(p.response));
                        }
                        chosen = &mock;
                    } else {
                        auto transport_r = st::transport::open_transport(spec);
                        if (!transport_r.has_value()) {
                            st_ptr->read_rom_error_msg = "Adapter open failed: " +
                                                         std::string{transport_r.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        // The OBDX/Native paths are fully wired via Win32
                        // serial (CreateFileA on \\.\COMx) — a bad device
                        // path surfaces a real Win32 error. What's
                        // empirically unverified is the OBDX DVI codec +
                        // UDS handshake against real adapter firmware.
                        auto open_r = (*transport_r)->open({});
                        if (!open_r.has_value()) {
                            st_ptr->read_rom_error_msg = "Adapter link open failed: " +
                                                         std::string{open_r.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        owned = std::move(*transport_r);
                        chosen = owned.get();
                    }

                    // SSM-on-CAN strips the K-Line wrapper (per
                    // docs/13-transport.md). Real-hardware SSM mode opens
                    // the adapter on HS CAN (LinkConfig default
                    // CanIso15765), so the ECU expects bare `A8 00 + addrs`
                    // payloads. Trace mode replays K-Line-shaped fixtures
                    // — keep KLine framing there so existing trace files
                    // round-trip unchanged.
                    auto const ssm_framing = (!trace_mode && protocol == 0)
                                                 ? st::ecu::ssm::Framing::IsoTp
                                                 : st::ecu::ssm::Framing::KLine;
                    st::flash::Flasher flasher{*chosen, ssm_framing};
                    // Install the picked SA key function before the
                    // first SecurityAccess exchange. Skipped in trace
                    // mode (the trace replays the exact captured key
                    // bytes; an alternate fn would produce different
                    // wire bytes and fail the expect_send_recv).
                    if (!trace_mode && authenticate) {
                        flasher.set_security_key_fn(sa_fn);
                    }
                    auto const progress_cb =
                        [bytes_done_sp, total_sp](st::flash::Flasher::ReadProgress p) {
                            bytes_done_sp->store(p.bytes_done, std::memory_order_release);
                            total_sp->store(p.total_bytes, std::memory_order_release);
                        };
                    auto result = protocol == 0
                                      ? flasher.read_full_rom_ssm(
                                            base_addr, size, static_cast<std::uint32_t>(max_chunk),
                                            std::chrono::milliseconds{1500}, progress_cb,
                                            cancel_sp.get())
                                      : flasher.read_full_rom(
                                            base_addr, size, static_cast<std::uint32_t>(max_chunk),
                                            std::chrono::milliseconds{1500}, progress_cb,
                                            cancel_sp.get(),
                                            /*enter_diagnostic_session=*/!trace_mode,
                                            /*authenticate=*/authenticate && !trace_mode,
                                            security_level);
                    if (!result.has_value()) {
                        if (result.error().code() == st::ErrorCode::Cancelled) {
                            if (verbose) {
                                std::fprintf(stderr,
                                             "[trace][read-rom] cancelled by user\n");
                                std::fflush(stderr);
                            }
                            st_ptr->read_rom_state = AppState::ReadRomState::Cancelled;
                            return;
                        }
                        if (verbose) {
                            std::fprintf(stderr, "[trace][read-rom] FAILED: %s\n",
                                         std::string{result.error().message()}.c_str());
                            std::fflush(stderr);
                        }
                        st_ptr->read_rom_error_msg = std::string{result.error().message()};
                        st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                        return;
                    }
                    if (verbose) {
                        std::fprintf(stderr,
                                     "[trace][read-rom] done: %zu bytes received\n",
                                     result->size());
                        std::fflush(stderr);
                    }
                    st_ptr->read_rom_bytes_result = std::move(*result);
                    st_ptr->read_rom_state = AppState::ReadRomState::Done;
                });
            }
        }
        ImGui::EndDisabled();
        if (start_disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            char const *const which = state.read_rom_adapter.kind_idx == 0   ? "vendor DLL path"
                                      : state.read_rom_adapter.kind_idx == 3 ? "trace file path"
                                                                             : "device path";
            ImGui::SetTooltip("Fill in the %s field above.", which);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.read_rom_error_msg.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Running: progress bar + cancel ---------
    if (state.read_rom_state == AppState::ReadRomState::Running) {
        std::uint32_t const done = state.read_rom_bytes_done->load(std::memory_order_acquire);
        std::uint32_t const total = state.read_rom_total_bytes->load(std::memory_order_acquire);
        float const frac = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "0x%X / 0x%X  (%.1f%%)", done, total,
                      static_cast<double>(100.0f * frac));
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        ImGui::Text("Elapsed: %s", elapsed_str().c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        text_subtle("Reading is slow (10-30 KB/s typical on SSM).");
        text_subtle("A 2 MB ROM usually takes 4-15 minutes.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        if (ImGui::Button("Cancel read", ImVec2(160.0f, 0.0f))) {
            state.read_rom_cancel->store(true, std::memory_order_release);
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Done: save .bin + offer to open as project ---------
    if (state.read_rom_state == AppState::ReadRomState::Done) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
        ImGui::TextUnformatted("Read complete.");
        ImGui::PopStyleColor();
        ImGui::Text("Got %zu bytes in %s.", state.read_rom_bytes_result.size(),
                    elapsed_str().c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));

        if (ImGui::Button("Save .bin...", ImVec2(160.0f, 0.0f))) {
            NFD::UniquePath out_path;
            nfdfilteritem_t const filters[] = {{"ROM image", "bin,hex"}};
            // Suggest a name from base+size for traceability. Buffer sized
            // for the literal prefix + two hex strings up to 31 chars each
            // (the actual struct field widths) — GCC -Wformat-truncation
            // computes the worst-case as ~72 B, so 128 is comfortable.
            char default_name[128];
            std::snprintf(default_name, sizeof default_name, "rom_%s_%s.bin",
                          state.read_rom_base_addr_hex, state.read_rom_size_hex);
            nfdresult_t const r = NFD::SaveDialog(out_path, filters, 1, nullptr, default_name);
            if (r == NFD_OKAY) {
                std::filesystem::path target{out_path.get()};
                std::ofstream fh{target, std::ios::binary};
                if (!fh) {
                    state.read_rom_error_msg =
                        "Failed to open output file for write: " + target.string();
                } else {
                    fh.write(reinterpret_cast<char const *>(state.read_rom_bytes_result.data()),
                             static_cast<std::streamsize>(state.read_rom_bytes_result.size()));
                    if (!fh) {
                        state.read_rom_error_msg = "Write failed (disk full?).";
                    } else {
                        state.status_msg = "Saved " + target.string() +
                                           ". Use File \xE2\x86\x92 New Project\xE2\x80\xA6 to start tuning.";
                        // Reset to Idle and close the popup.
                        state.read_rom_bytes_result.clear();
                        state.read_rom_state = AppState::ReadRomState::Idle;
                        state.read_rom_error_msg.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
            } else if (r == NFD_ERROR) {
                state.read_rom_error_msg = std::string{"Save dialog error: "} + NFD::GetError();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f))) {
            state.read_rom_bytes_result.clear();
            state.read_rom_state = AppState::ReadRomState::Idle;
            ImGui::CloseCurrentPopup();
        }
        if (!state.read_rom_error_msg.empty()) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Failed / Cancelled: surface + back-to-Idle ---------
    if (state.read_rom_state == AppState::ReadRomState::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("Read failed.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    } else if (state.read_rom_state == AppState::ReadRomState::Cancelled) {
        ImGui::TextUnformatted("Read cancelled.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    }
    if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.read_rom_state = AppState::ReadRomState::Idle;
        state.read_rom_error_msg.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
