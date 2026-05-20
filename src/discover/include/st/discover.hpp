// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_DISCOVER_HPP
#define ST_DISCOVER_HPP

#include "st/can.hpp"
#include "st/core/result.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st::discover {

// Per-byte-position classification inferred from a baseline window.
// See docs/14 § "Discovery workflow" for the rationale.
enum class ByteClass : std::uint8_t {
    Stable, // takes a small set of values (default ≤ 4)
    Cyclic, // deterministic counter / rolling sequence
    Noisy,  // many distinct values, no obvious pattern (analog signal)
};

// Baseline knowledge of one CAN id on one bus.
struct IdBaseline {
    can::BusId bus{can::BusId::Hs};
    std::uint32_t can_id{0};
    bool extended{false};
    std::uint8_t dlc{0};

    std::array<ByteClass, 8> classes{};
    // For Stable bytes: the set of values seen during the baseline window.
    // For Cyclic / Noisy bytes: empty.
    std::array<std::vector<std::uint8_t>, 8> stable_values;
    // For all bytes: the most-common observed value (the "mode"). Used
    // for the `before` field of Change events and for .cdb output.
    std::array<std::uint8_t, 8> mode_value{};

    double freq_hz{0.0}; // samples / baseline_duration
    std::size_t samples{0};
};

// Aggregates per-CAN-id baselines over a captured window. Use:
//
//     BaselineModel m;
//     for (frame in baseline_frames) m.observe(frame);
//     m.finalize(baseline_duration);
//     auto const* b = m.find(bus, can_id);
//
// or, equivalently, the `build_baseline` convenience below.
class BaselineModel {
public:
    struct Options {
        // A byte position is Stable iff it takes at most this many
        // distinct values during the baseline window.
        std::size_t stable_max_distinct{4};
        // Below this many baseline samples for a given id, we never
        // classify any byte as Noisy — we don't have enough evidence.
        // (The byte is reported as Stable with whatever values it took.)
        std::size_t min_samples_for_noisy{8};
        // Cyclic detector: a byte is Cyclic iff at least this fraction
        // of consecutive deltas equal the modal non-zero delta. Allows
        // strict-counter and wrapping-counter to both qualify.
        double cyclic_modal_delta_fraction{0.75};
    };

    BaselineModel();
    explicit BaselineModel(Options opts);

    BaselineModel(BaselineModel const &) = delete;
    BaselineModel &operator=(BaselineModel const &) = delete;
    BaselineModel(BaselineModel &&) = default;
    BaselineModel &operator=(BaselineModel &&) = default;

    // Feed one frame from the baseline window. After `finalize` has been
    // called, additional `observe` calls are silently ignored.
    void observe(can::Frame const &f);

    // Convert the in-progress builders into the final per-id baselines.
    // `duration` is used to compute each id's frequency in Hz.
    void finalize(std::chrono::nanoseconds duration);

    [[nodiscard]] bool finalized() const noexcept {
        return finalized_;
    }
    [[nodiscard]] Options const &options() const noexcept {
        return opts_;
    }
    [[nodiscard]] std::vector<IdBaseline> const &entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] IdBaseline const *find(can::BusId bus, std::uint32_t can_id) const noexcept;

private:
    Options opts_;
    bool finalized_{false};
    std::vector<IdBaseline> entries_;

    struct Builder {
        can::BusId bus{can::BusId::Hs};
        bool extended{false};
        std::uint8_t dlc{0};
        std::array<std::vector<std::uint8_t>, 8> samples;
        std::size_t frames{0};
    };
    // Key = (static_cast<uint64_t>(bus) << 32) | can_id.
    std::unordered_map<std::uint64_t, Builder> builders_;
};

// One discovery event emitted during the watch phase.
struct DiscoveryEvent {
    enum class Kind : std::uint8_t {
        NewId,  // ID not present in baseline
        Change, // one or more Stable bytes of a known ID deviated
    };

    Kind kind{Kind::Change};
    std::int64_t timestamp_ns{0};
    can::BusId bus{can::BusId::Hs};
    std::uint32_t can_id{0};
    bool extended{false};

    // For `Change` events: which byte positions deviated, the baseline
    // mode values, and the current frame's values. `changed_byte_indices`
    // is empty for `NewId` events.
    std::vector<std::uint8_t> changed_byte_indices;
    std::array<std::uint8_t, 8> before{};
    std::array<std::uint8_t, 8> after{};

    // User-typed label captured at the time the event fired (e.g.,
    // "left blinker on"). Empty when the event was produced by the
    // detector with no labeling step yet — the .cdb writer/reader
    // round-trips it either way.
    std::string description;
};

// Stateful detector: feed one frame at a time from the watch phase.
// Emits at most one event per call.
//
// Debounce: after any event fires, subsequent events from any id are
// suppressed for `opts.debounce_ns` worth of wall-clock-in-the-trace
// (using each frame's own timestamp_ns). The debounce window starts
// from the event's timestamp.
class ChangeDetector {
public:
    struct Options {
        // Frames within this nanosecond window of the most recent event
        // are not allowed to fire another event. 500 ms default.
        std::int64_t debounce_ns{500'000'000};
        // Off by default — analog noise dominates this signal class.
        bool flag_noisy_deviations{false};
    };

    explicit ChangeDetector(BaselineModel const &baseline);
    ChangeDetector(BaselineModel const &baseline, Options opts);

    [[nodiscard]] std::optional<DiscoveryEvent> observe(can::Frame const &f);

    [[nodiscard]] std::uint64_t events_emitted() const noexcept {
        return events_count_;
    }

private:
    BaselineModel const *baseline_;
    Options opts_;
    std::int64_t last_event_ns_{std::numeric_limits<std::int64_t>::min()};
    std::uint64_t events_count_{0};
    // IDs we've already reported as "new" during watch — suppresses
    // repeated NewId emissions for the same id.
    std::unordered_set<std::uint64_t> reported_new_ids_;
};

// Convenience: build a baseline from a span of frames in one call. The
// duration is computed from the first and last frame's timestamps.
[[nodiscard]] BaselineModel build_baseline(std::span<can::Frame const> frames,
                                           BaselineModel::Options opts = BaselineModel::Options{});

// Convenience: drive a ChangeDetector across a span of watch-phase
// frames and collect every event it emits.
[[nodiscard]] std::vector<DiscoveryEvent>
detect_changes(BaselineModel const &baseline, std::span<can::Frame const> watch_frames,
               ChangeDetector::Options opts = ChangeDetector::Options{});

// =====================================================================
// .cdb (CAN Discovery Bundle) file format — TOML
// =====================================================================
//
// The durable artifact of a discovery session: the per-id baseline (so a
// later session can pick up without re-baselining) plus the stream of
// labeled events. Schema documented in docs/14.
//
// Round-trip stable: write_cdb(b) → read_cdb returns a Bundle whose
// fields all match `b`. Times use int64 nanoseconds throughout — wall-
// clock context lives in the opaque `captured_at` string.
struct Bundle {
    int schema_version{1};
    // Free-form ISO-8601 timestamp of when the capture started, e.g.
    // "2026-05-11T12:34:56.789Z". Opaque to the reader/writer.
    std::string captured_at;
    // Free-form bus label — "hs", "ms", "powertrain", etc. For multi-
    // bus sessions, emit one .cdb per bus (and set `bus_label`
    // accordingly).
    std::string bus_label;
    // Baseline window length in nanoseconds — preserved exactly so
    // frequency calculations stay reproducible.
    std::int64_t baseline_ns{0};
    std::vector<IdBaseline> baseline_entries;
    std::vector<DiscoveryEvent> events;
};

[[nodiscard]] Result<std::string> write_cdb_string(Bundle const &b);
[[nodiscard]] Result<Bundle> parse_cdb_string(std::string_view text);

[[nodiscard]] Result<Bundle> read_cdb(std::filesystem::path const &path);
[[nodiscard]] Status write_cdb(std::filesystem::path const &path, Bundle const &b);

} // namespace st::discover

#endif // ST_DISCOVER_HPP
