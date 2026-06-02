// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Live gauge cluster panel — docs/32-live-datalogger.md. Renders a
// grid of headline-value + ImPlot mini-line gauges fed by a
// LiveBuffer SPSC ring. v1 ships a synthetic demo data generator
// so the panel is visually complete + clickable before the
// real-transport session wiring lands; the LiveBuffer + sampling
// path are unchanged between demo + live modes.
//
// Toolbar: Start Demo / Stop / Clear. The "Start Demo" path spawns
// a worker thread that injects synthetic RPM / MAP / TPS / IAT
// samples at gauge_demo_hz Hz. The "Stop" path signals the worker
// and joins; idempotent. "Clear" zeroes the buffer (rebuilds).
//
// Real-transport sessions will land as a follow-up — same panel,
// new toolbar button ("Connect…") that opens an adapter picker
// modal and constructs a real LogSession instead of the demo
// thread. The LiveBuffer + render path stays put.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/log/live_buffer.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace st::ui {

namespace {

// Four demo channels. Order matches the LiveBuffer's channel indices
// when the demo session creates the buffer. When real-transport mode
// lands these become pack-driven from [[log_channel]] entries.
inline constexpr AppState::GaugeMeta kDemoChannels[] = {
    {"RPM", "rpm", 6800.0, 5500.0, 0.0, 600.0, 7200.0},
    {"MAP", "kPa", 250.0, 200.0, 0.0, 0.0, 300.0},
    {"TPS", "%", 100.0, 90.0, 0.0, 0.0, 100.0},
    {"IAT", "°C", 70.0, 60.0, 0.0, -20.0, 90.0},
};
inline constexpr std::size_t kDemoChannelCount =
    sizeof(kDemoChannels) / sizeof(kDemoChannels[0]);

// Inject synthetic samples at `hz` until stop is requested. Each
// channel follows its own simple model:
//   RPM — sine wave around 3500, ±2500 amplitude
//   MAP — partial echo of RPM (boost responds to RPM-ish input)
//   TPS — square-ish steps every ~2 s (driver inputs)
//   IAT — slow drift around 25 °C with ±5 jitter
void run_demo_session(st::log::LiveBuffer *buffer, std::atomic<bool> &stop, int hz) {
    if (buffer == nullptr || hz <= 0)
        return;
    auto const period_us = std::chrono::microseconds{1'000'000 / hz};
    auto const start_time = std::chrono::steady_clock::now();
    std::mt19937 rng{0xC0FFEEU};
    std::uniform_real_distribution<double> jitter{-1.0, 1.0};
    double tps_target = 15.0;
    auto last_tps_step = start_time;
    while (!stop.load(std::memory_order_acquire)) {
        auto const now = std::chrono::steady_clock::now();
        auto const t_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time).count();
        double const t_s = static_cast<double>(t_ns) / 1.0e9;

        // RPM — wandering sine.
        double const rpm = 3500.0 + 2500.0 * std::sin(t_s * 0.7) +
                           500.0 * std::sin(t_s * 3.1) + 50.0 * jitter(rng);
        buffer->push(0, t_ns, rpm);

        // MAP — coupled to RPM with damping. Roughly: idle = 30 kPa,
        // boost peaks ~220 at high RPM.
        double const map = 30.0 + (rpm - 600.0) * 0.029 + 5.0 * jitter(rng);
        buffer->push(1, t_ns, std::clamp(map, 0.0, 280.0));

        // TPS — step every ~2 seconds to a new random target,
        // approached exponentially in between.
        if (now - last_tps_step > std::chrono::seconds{2}) {
            std::uniform_real_distribution<double> step{0.0, 100.0};
            tps_target = step(rng);
            last_tps_step = now;
        }
        static double tps_actual = 15.0;
        tps_actual += (tps_target - tps_actual) * 0.08 + 0.5 * jitter(rng);
        buffer->push(2, t_ns, std::clamp(tps_actual, 0.0, 100.0));

        // IAT — slow drift around 25 °C.
        double const iat = 25.0 + 5.0 * std::sin(t_s * 0.05) + 1.0 * jitter(rng);
        buffer->push(3, t_ns, iat);

        std::this_thread::sleep_for(period_us);
    }
}

void stop_demo(AppState &state) {
    if (!state.gauge_demo_running.load(std::memory_order_acquire))
        return;
    state.gauge_demo_stop.store(true, std::memory_order_release);
    if (state.gauge_demo_thread.joinable()) {
        state.gauge_demo_thread.join();
    }
    state.gauge_demo_running.store(false, std::memory_order_release);
    state.gauge_demo_stop.store(false, std::memory_order_release);
}

void start_demo(AppState &state) {
    if (state.gauge_demo_running.load(std::memory_order_acquire))
        return;
    if (state.gauge_buffer == nullptr) {
        // Capacity sized for ~60 seconds at 100 Hz, rounded up to the
        // next power of two by LiveBuffer's constructor.
        state.gauge_buffer = std::make_unique<st::log::LiveBuffer>(
            kDemoChannelCount, 8192);
    }
    state.gauge_demo_stop.store(false, std::memory_order_release);
    state.gauge_demo_running.store(true, std::memory_order_release);
    auto *buf = state.gauge_buffer.get();
    int const hz = state.gauge_demo_hz;
    state.gauge_demo_thread = std::thread{[buf, &stop = state.gauge_demo_stop, hz] {
        run_demo_session(buf, stop, hz);
    }};
    state.gauge_status_msg = "Demo session running.";
}

// Pick a color for the threshold band based on whether a value is
// above the warn line. Bands shade lightly inside the gauge's
// y-range; the line plot draws on top.
ImVec4 band_color_warn() noexcept {
    return ImVec4(0.95f, 0.75f, 0.30f, 0.18f); // amber, low alpha
}
ImVec4 band_color_redline() noexcept {
    return ImVec4(0.95f, 0.30f, 0.30f, 0.22f); // red, low alpha
}

// One gauge. Caller provides the cell rect (so the panel can grid them
// however it likes); this just paints inside it.
void render_one_gauge(AppState &state, std::size_t channel_idx,
                      AppState::GaugeMeta const &meta, float cell_height) {
    ImGui::PushID(static_cast<int>(channel_idx));

    // Header row: channel name (large) + units (subtle).
    ImGui::TextUnformatted(meta.name);
    ImGui::SameLine();
    text_subtle("(%s)", meta.units);

    // Headline value — the latest sample. Large font would be nice;
    // for v1 use the default font at increased size via no-op.
    auto const latest = state.gauge_buffer ? state.gauge_buffer->latest(channel_idx)
                                           : std::nullopt;
    if (latest.has_value()) {
        // Color the headline by zone — red if above redline, amber if
        // above warn_above (and warn_above > 0), default otherwise.
        ImVec4 headline_color = chip_fg_ok();
        if (meta.redline > 0.0 && latest->value >= meta.redline) {
            headline_color = chip_fg_danger();
        } else if (meta.warn_above > 0.0 && latest->value >= meta.warn_above) {
            headline_color = chip_fg_caution();
        } else if (meta.warn_below > 0.0 && latest->value <= meta.warn_below) {
            headline_color = chip_fg_caution();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, headline_color);
        ImGui::Text("%.1f", latest->value);
        ImGui::PopStyleColor();
    } else {
        text_subtle("(no data yet)");
    }

    // Mini-line — recent N seconds at the demo rate.
    float const plot_height = std::max(40.0f, cell_height - 70.0f);
    auto const samples_to_show = static_cast<std::size_t>(
        std::lround(static_cast<double>(state.gauge_demo_hz) *
                    static_cast<double>(state.gauge_history_seconds)));
    auto snapshot = state.gauge_buffer
                        ? state.gauge_buffer->snapshot(channel_idx, samples_to_show)
                        : std::vector<st::log::LiveBuffer::Sample>{};

    char plot_id[32];
    std::snprintf(plot_id, sizeof plot_id, "##gauge_plot_%zu", channel_idx);
    if (ImPlot::BeginPlot(plot_id, ImVec2(-1.0f, plot_height),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend |
                              ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus |
                              ImPlotFlags_NoBoxSelect | ImPlotFlags_NoFrame)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisLimits(ImAxis_Y1, meta.display_min, meta.display_max,
                                ImPlotCond_Always);
        if (!snapshot.empty()) {
            // X axis: relative seconds from oldest sample.
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(snapshot.size());
            ys.reserve(snapshot.size());
            auto const t0 = snapshot.front().timestamp_ns;
            for (auto const &s : snapshot) {
                xs.push_back(static_cast<double>(s.timestamp_ns - t0) / 1.0e9);
                ys.push_back(s.value);
            }
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, xs.back(), ImPlotCond_Always);

            // Threshold shading bands (drawn under the line). ImPlot
            // styling uses ImPlotSpec per-item — FillColor on the
            // shaded region, LineColor on the trace.
            if (meta.redline > 0.0 && meta.redline < meta.display_max) {
                double const top = meta.display_max;
                double const bot = meta.redline;
                double xs_band[2] = {0.0, xs.back()};
                double tops[2] = {top, top};
                double bots[2] = {bot, bot};
                ImPlotSpec band_spec;
                band_spec.FillColor = band_color_redline();
                ImPlot::PlotShaded("##red_band", xs_band, bots, tops, 2, band_spec);
            }
            if (meta.warn_above > 0.0 && meta.warn_above < meta.display_max) {
                double const top = (meta.redline > 0.0) ? meta.redline : meta.display_max;
                double const bot = meta.warn_above;
                double xs_band[2] = {0.0, xs.back()};
                double tops[2] = {top, top};
                double bots[2] = {bot, bot};
                ImPlotSpec band_spec;
                band_spec.FillColor = band_color_warn();
                ImPlot::PlotShaded("##warn_above_band", xs_band, bots, tops, 2, band_spec);
            }
            if (meta.warn_below > 0.0 && meta.warn_below > meta.display_min) {
                double const top = meta.warn_below;
                double const bot = meta.display_min;
                double xs_band[2] = {0.0, xs.back()};
                double tops[2] = {top, top};
                double bots[2] = {bot, bot};
                ImPlotSpec band_spec;
                band_spec.FillColor = band_color_warn();
                ImPlot::PlotShaded("##warn_below_band", xs_band, bots, tops, 2, band_spec);
            }

            // The trace itself — purple accent per the project theme.
            ImPlotSpec trace_spec;
            trace_spec.LineColor = ImVec4(0.55f, 0.35f, 0.85f, 1.0f);
            trace_spec.LineWeight = 1.5f;
            ImPlot::PlotLine("##trace", xs.data(), ys.data(),
                             static_cast<int>(xs.size()), trace_spec);
        }
        ImPlot::EndPlot();
    }

    // Footer: range + redline.
    if (meta.redline > 0.0) {
        text_subtle("range %g–%g    redline >%g", meta.display_min, meta.display_max,
                    meta.redline);
    } else {
        text_subtle("range %g–%g", meta.display_min, meta.display_max);
    }

    ImGui::PopID();
}

} // namespace

void render_gauge_cluster_panel(AppState &state) {
    if (!state.show_gauge_cluster_panel) {
        // If the user closes the window while a demo is running,
        // stop the worker so we don't leak the thread.
        if (state.gauge_demo_running.load(std::memory_order_acquire)) {
            stop_demo(state);
        }
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Gauge Cluster###Gauge Cluster (Preview)",
                      &state.show_gauge_cluster_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Live datalog gauges. v1 ships with a synthetic demo data feed; "
                "real-transport hookup lands as a follow-up. See docs/32.");
    ImGui::Separator();

    // Toolbar.
    bool const running = state.gauge_demo_running.load(std::memory_order_acquire);
    if (!running) {
        if (ImGui::Button("\xEE\x9C\xB7  Start Demo")) { // E737 Play
            start_demo(state);
        }
    } else {
        if (ImGui::Button("\xEE\x9D\x84  Stop")) { // E71A Stop
            stop_demo(state);
            state.gauge_status_msg = "Demo stopped.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x9D\xA4  Clear")) { // E74D Trash
        // Stop, drop the buffer (next start re-creates), clear status.
        stop_demo(state);
        state.gauge_buffer.reset();
        state.gauge_status_msg = "Buffer cleared.";
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("History (s)", &state.gauge_history_seconds, 5.0f, 120.0f, "%.0f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderInt("Rate (Hz)", &state.gauge_demo_hz, 10, 200)) {
        // Rate only takes effect on next session start; no live retune
        // in v1 (the demo worker captures hz by value).
    }
    if (!state.gauge_status_msg.empty()) {
        ImGui::SameLine();
        text_subtle("%s", state.gauge_status_msg.c_str());
    }

    ImGui::Separator();

    // Grid layout. Pick columns based on available width:
    //   < 700 px → 1 column, < 1300 px → 2 columns, else 3.
    float const avail_w = ImGui::GetContentRegionAvail().x;
    int const cols = (avail_w < 700.0f) ? 1 : (avail_w < 1300.0f) ? 2 : 3;
    float const cell_height = 200.0f;

    if (ImGui::BeginTable("##gauge_grid", cols, ImGuiTableFlags_SizingStretchSame)) {
        for (std::size_t i = 0; i < kDemoChannelCount; ++i) {
            ImGui::TableNextColumn();
            ImGui::BeginChild(ImGui::GetID(static_cast<int>(i)),
                              ImVec2(0.0f, cell_height), true);
            render_one_gauge(state, i, kDemoChannels[i], cell_height);
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace st::ui
