// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/can.hpp"
#include "st/discover.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace can_ns = st::can;
namespace discover_ns = st::discover;

namespace {

can_ns::Frame make(std::int64_t ts_ns, std::uint32_t id,
                   std::initializer_list<std::uint8_t> bytes) {
    can_ns::Frame f;
    f.timestamp_ns = ts_ns;
    f.bus = can_ns::BusId::Hs;
    f.id = id;
    f.dlc = static_cast<std::uint8_t>(bytes.size());
    std::uint8_t i = 0;
    for (auto b : bytes) {
        f.data[i++] = b;
    }
    return f;
}

} // namespace

// =====================================================================
// BaselineModel — classification
// =====================================================================

TEST_CASE("Baseline classifies a byte that never changes as Stable", "[discover][baseline]") {
    std::vector<can_ns::Frame> frames;
    for (int i = 0; i < 50; ++i) {
        frames.push_back(make(i * 1'000'000, 0x140, {0x42, 0x00}));
    }
    auto const model = discover_ns::build_baseline(frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x140);
    REQUIRE(id != nullptr);
    REQUIRE(id->classes[0] == discover_ns::ByteClass::Stable);
    REQUIRE(id->classes[1] == discover_ns::ByteClass::Stable);
    REQUIRE(id->stable_values[0] == std::vector<std::uint8_t>{0x42});
    REQUIRE(id->mode_value[0] == 0x42);
}

TEST_CASE("Baseline classifies a small-set byte as Stable", "[discover][baseline]") {
    // Byte 0 alternates between three values — within the default
    // stable_max_distinct of 4 -> Stable.
    std::vector<std::uint8_t> const sequence{0x10, 0x20, 0x30, 0x10, 0x20,
                                             0x30, 0x10, 0x20, 0x30, 0x10};
    std::vector<can_ns::Frame> frames;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        frames.push_back(make(static_cast<std::int64_t>(i * 1'000'000), 0x200, {sequence[i]}));
    }
    auto const model = discover_ns::build_baseline(frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x200);
    REQUIRE(id != nullptr);
    REQUIRE(id->classes[0] == discover_ns::ByteClass::Stable);
    REQUIRE(id->stable_values[0].size() == 3);
}

TEST_CASE("Baseline classifies a strictly-incrementing byte as Cyclic", "[discover][baseline]") {
    // Byte 0 increments 0..15 (16 distinct values, all delta=+1). Way
    // beyond the Stable distinct threshold; Cyclic should win.
    std::vector<can_ns::Frame> frames;
    for (int i = 0; i < 32; ++i) {
        auto const v = static_cast<std::uint8_t>(i & 0x0F);
        frames.push_back(make(i * 1'000'000, 0x300, {v}));
    }
    auto const model = discover_ns::build_baseline(frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x300);
    REQUIRE(id != nullptr);
    REQUIRE(id->classes[0] == discover_ns::ByteClass::Cyclic);
    REQUIRE(id->stable_values[0].empty());
}

TEST_CASE("Baseline classifies an unstructured byte as Noisy", "[discover][baseline]") {
    // A made-up sequence with no clear pattern AND many distinct values.
    std::vector<std::uint8_t> const seq{0x10, 0xFF, 0x22, 0x05, 0xAB, 0x71, 0x14, 0xEE,
                                        0x30, 0x9F, 0x42, 0x55, 0x07, 0x83, 0x91, 0xC0};
    std::vector<can_ns::Frame> frames;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        frames.push_back(make(static_cast<std::int64_t>(i * 1'000'000), 0x400, {seq[i]}));
    }
    auto const model = discover_ns::build_baseline(frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x400);
    REQUIRE(id != nullptr);
    REQUIRE(id->classes[0] == discover_ns::ByteClass::Noisy);
}

TEST_CASE("Baseline computes frequency from the capture duration", "[discover][baseline]") {
    // 11 frames spanning 100 ms — that's 11 samples in a 100 ms window,
    // i.e., 110 Hz when measured strictly from the first-to-last span.
    std::vector<can_ns::Frame> frames;
    for (int i = 0; i < 11; ++i) {
        frames.push_back(make(i * 10'000'000, 0x100, {0x00}));
    }
    auto const model = discover_ns::build_baseline(frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x100);
    REQUIRE(id != nullptr);
    REQUIRE(id->samples == 11);
    REQUIRE(id->freq_hz > 109.0);
    REQUIRE(id->freq_hz < 111.0);
}

// =====================================================================
// ChangeDetector — events
// =====================================================================

TEST_CASE("ChangeDetector emits a NewId event for an unseen ID", "[discover][change]") {
    // Baseline only has 0x100. Watch sees 0x200 — emit NewId.
    std::vector<can_ns::Frame> baseline_frames;
    for (int i = 0; i < 10; ++i) {
        baseline_frames.push_back(make(i * 1'000'000, 0x100, {0x00}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);

    std::vector<can_ns::Frame> watch_frames;
    watch_frames.push_back(make(1'000'000'000, 0x200, {0xAB}));
    watch_frames.push_back(make(1'001'000'000, 0x200, {0xCD})); // 2nd sighting — silent
    watch_frames.push_back(make(2'000'000'000, 0x100, {0x00})); // known + stable: silent

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == discover_ns::DiscoveryEvent::Kind::NewId);
    REQUIRE(events[0].can_id == 0x200);
}

TEST_CASE("ChangeDetector flags a stable-byte deviation", "[discover][change]") {
    // Baseline: byte 0 always 0x00. Watch: byte 0 becomes 0x01.
    std::vector<can_ns::Frame> baseline_frames;
    for (int i = 0; i < 10; ++i) {
        baseline_frames.push_back(make(i * 1'000'000, 0x140, {0x00, 0x00}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);

    std::vector<can_ns::Frame> watch_frames;
    watch_frames.push_back(make(1'000'000'000, 0x140, {0x01, 0x00}));

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == discover_ns::DiscoveryEvent::Kind::Change);
    REQUIRE(events[0].can_id == 0x140);
    REQUIRE(events[0].changed_byte_indices == std::vector<std::uint8_t>{0});
    REQUIRE(events[0].before[0] == 0x00);
    REQUIRE(events[0].after[0] == 0x01);
}

TEST_CASE("ChangeDetector ignores a cyclic byte at watch time", "[discover][change]") {
    // Baseline byte 0: a counter 0..15 incrementing each frame.
    // Watch byte 0: a continuation of that counter — must NOT fire.
    std::vector<can_ns::Frame> baseline_frames;
    for (int i = 0; i < 32; ++i) {
        auto const v = static_cast<std::uint8_t>(i & 0x0F);
        baseline_frames.push_back(make(i * 1'000'000, 0x300, {v, 0x00}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);

    std::vector<can_ns::Frame> watch_frames;
    for (int i = 0; i < 32; ++i) {
        auto const v = static_cast<std::uint8_t>((i + 5) & 0x0F);
        watch_frames.push_back(make(1'000'000'000 + i * 1'000'000, 0x300, {v, 0x00}));
    }

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.empty());
}

TEST_CASE("ChangeDetector coalesces multi-byte changes in one frame", "[discover][change]") {
    // Baseline: bytes 0+1 always 0x00. Watch: both flip in the same
    // frame. One event with two byte indices.
    std::vector<can_ns::Frame> baseline_frames;
    for (int i = 0; i < 10; ++i) {
        baseline_frames.push_back(make(i * 1'000'000, 0x150, {0x00, 0x00, 0x00}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);

    std::vector<can_ns::Frame> watch_frames;
    watch_frames.push_back(make(1'000'000'000, 0x150, {0xAA, 0xBB, 0x00}));

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].changed_byte_indices == std::vector<std::uint8_t>{0, 1});
    REQUIRE(events[0].after[0] == 0xAA);
    REQUIRE(events[0].after[1] == 0xBB);
}

TEST_CASE("ChangeDetector debounces - suppresses repeats within the window",
          "[discover][change][debounce]") {
    // Baseline byte 0 always 0x00. Watch flips it three times quickly;
    // only the first event must fire (others fall inside the default
    // 500 ms debounce window).
    std::vector<can_ns::Frame> baseline_frames;
    for (int i = 0; i < 10; ++i) {
        baseline_frames.push_back(make(i * 1'000'000, 0x140, {0x00}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);

    std::vector<can_ns::Frame> watch_frames;
    watch_frames.push_back(make(1'000'000'000, 0x140, {0x01})); // event
    watch_frames.push_back(make(1'100'000'000, 0x140, {0x02})); // debounced
    watch_frames.push_back(make(1'300'000'000, 0x140, {0x03})); // debounced
    watch_frames.push_back(make(1'600'000'000, 0x140, {0x04})); // past 500ms — event

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].after[0] == 0x01);
    REQUIRE(events[1].after[0] == 0x04);
}

TEST_CASE("ChangeDetector emits a NewId only on first sighting", "[discover][change]") {
    // Baseline has nothing. Watch sees the same new id three times —
    // only the first fires.
    discover_ns::BaselineModel model;
    model.finalize(std::chrono::nanoseconds{1'000'000'000});

    std::vector<can_ns::Frame> watch_frames;
    watch_frames.push_back(make(0, 0x999, {0xAA}));
    watch_frames.push_back(make(600'000'000, 0x999, {0xBB}));   // past debounce
    watch_frames.push_back(make(1'200'000'000, 0x999, {0xCC})); // past debounce

    auto const events = discover_ns::detect_changes(model, watch_frames);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == discover_ns::DiscoveryEvent::Kind::NewId);
}

TEST_CASE("ChangeDetector with flag_noisy_deviations on flags noisy deviations",
          "[discover][change][noisy]") {
    // Baseline byte 0: noisy (16 distinct values, no pattern). Mode
    // value works out to whatever happens to recur most.
    std::vector<std::uint8_t> const seq{0x10, 0xFF, 0x22, 0x05, 0xAB, 0x71, 0x14, 0xEE,
                                        0x30, 0x9F, 0x42, 0x55, 0x07, 0x83, 0x91, 0xC0};
    std::vector<can_ns::Frame> baseline_frames;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        baseline_frames.push_back(make(static_cast<std::int64_t>(i * 1'000'000), 0x400, {seq[i]}));
    }
    auto const model = discover_ns::build_baseline(baseline_frames);
    auto const *id = model.find(can_ns::BusId::Hs, 0x400);
    REQUIRE(id != nullptr);
    REQUIRE(id->classes[0] == discover_ns::ByteClass::Noisy);

    // Default options — noisy bytes do not fire.
    auto const off = discover_ns::detect_changes(
        model, std::vector<can_ns::Frame>{make(
                   1'000'000'000, 0x400, {static_cast<std::uint8_t>(id->mode_value[0] + 1U)})});
    REQUIRE(off.empty());

    // With flag_noisy_deviations = true, a non-mode value fires.
    discover_ns::ChangeDetector::Options opts;
    opts.flag_noisy_deviations = true;
    auto const on = discover_ns::detect_changes(
        model,
        std::vector<can_ns::Frame>{
            make(1'000'000'000, 0x400, {static_cast<std::uint8_t>(id->mode_value[0] + 1U)})},
        opts);
    REQUIRE(on.size() == 1);
}

// =====================================================================
// .cdb bundle round-trip
// =====================================================================

namespace {

// Build a small, hand-tuned baseline: one id at 0x140 with byte 0
// stable=={0x00}, byte 1 cyclic, byte 2 stable=={0x10,0x20}, byte 3
// noisy. 32 frames over 100 ms -> 320 Hz nominally.
discover_ns::BaselineModel small_baseline() {
    std::vector<can_ns::Frame> frames;
    for (int i = 0; i < 32; ++i) {
        auto const cyc = static_cast<std::uint8_t>(i & 0x0F);
        auto const stab2 = static_cast<std::uint8_t>((i & 1) ? 0x20 : 0x10);
        auto const noisy = static_cast<std::uint8_t>((i * 73 + 17) & 0xFF);
        frames.push_back(
            make(static_cast<std::int64_t>(i * 3'125'000), 0x140, {0x00, cyc, stab2, noisy}));
    }
    return discover_ns::build_baseline(frames);
}

} // namespace

TEST_CASE("Bundle round-trips through write_cdb_string + parse_cdb_string", "[discover][cdb]") {
    discover_ns::Bundle b;
    b.captured_at = "2026-05-12T00:00:00Z";
    b.bus_label = "hs";
    b.baseline_ns = 100'000'000;

    auto const model = small_baseline();
    b.baseline_entries = model.entries();

    // One Change event and one NewId event.
    discover_ns::DiscoveryEvent change;
    change.kind = discover_ns::DiscoveryEvent::Kind::Change;
    change.timestamp_ns = 1'000'000'000;
    change.can_id = 0x140;
    change.bus = can_ns::BusId::Hs;
    change.changed_byte_indices = {0, 2};
    change.before[0] = 0x00;
    change.before[2] = 0x10;
    change.after[0] = 0x42;
    change.after[2] = 0xFF;
    change.description = "left blinker on";
    b.events.push_back(change);

    discover_ns::DiscoveryEvent new_id;
    new_id.kind = discover_ns::DiscoveryEvent::Kind::NewId;
    new_id.timestamp_ns = 2'000'000'000;
    new_id.can_id = 0x215;
    new_id.bus = can_ns::BusId::Hs;
    new_id.after = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    new_id.description = "first sighting after engine start";
    b.events.push_back(new_id);

    auto const text = discover_ns::write_cdb_string(b);
    REQUIRE(text.has_value());

    auto const round = discover_ns::parse_cdb_string(*text);
    REQUIRE(round.has_value());
    REQUIRE(round->schema_version == b.schema_version);
    REQUIRE(round->captured_at == b.captured_at);
    REQUIRE(round->bus_label == b.bus_label);
    REQUIRE(round->baseline_ns == b.baseline_ns);
    REQUIRE(round->baseline_entries.size() == b.baseline_entries.size());

    // Spot-check the baseline entry's classifications survived.
    auto const &back = round->baseline_entries[0];
    REQUIRE(back.can_id == b.baseline_entries[0].can_id);
    REQUIRE(back.dlc == b.baseline_entries[0].dlc);
    REQUIRE(back.samples == b.baseline_entries[0].samples);
    REQUIRE(back.classes == b.baseline_entries[0].classes);
    REQUIRE(back.mode_value == b.baseline_entries[0].mode_value);
    for (std::size_t i = 0; i < 8; ++i) {
        REQUIRE(back.stable_values[i] == b.baseline_entries[0].stable_values[i]);
    }

    REQUIRE(round->events.size() == 2);
    auto const &ec = round->events[0];
    REQUIRE(ec.kind == discover_ns::DiscoveryEvent::Kind::Change);
    REQUIRE(ec.can_id == 0x140);
    REQUIRE(ec.changed_byte_indices == std::vector<std::uint8_t>{0, 2});
    REQUIRE(ec.before[0] == 0x00);
    REQUIRE(ec.before[2] == 0x10);
    REQUIRE(ec.after[0] == 0x42);
    REQUIRE(ec.after[2] == 0xFF);
    REQUIRE(ec.description == "left blinker on");

    auto const &en = round->events[1];
    REQUIRE(en.kind == discover_ns::DiscoveryEvent::Kind::NewId);
    REQUIRE(en.can_id == 0x215);
    REQUIRE(en.after[0] == 0x80);
    REQUIRE(en.description == "first sighting after engine start");
}

TEST_CASE("Bundle round-trips via the filesystem (read_cdb / write_cdb)", "[discover][cdb][file]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     ("st_cdb_test_" + std::to_string(std::random_device{}()) + ".cdb");

    discover_ns::Bundle b;
    b.bus_label = "ms";
    b.baseline_ns = 5'000'000'000;
    b.baseline_entries = small_baseline().entries();

    REQUIRE(discover_ns::write_cdb(tmp, b).has_value());
    auto const r = discover_ns::read_cdb(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->bus_label == "ms");
    REQUIRE(r->baseline_ns == b.baseline_ns);
    REQUIRE(r->baseline_entries.size() == b.baseline_entries.size());

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("parse_cdb_string rejects an unknown event kind", "[discover][cdb][error]") {
    constexpr std::string_view text = "schema_version = 1\n"
                                      "baseline_ns = 0\n"
                                      "[baseline]\n"
                                      "id = []\n"
                                      "[[event]]\n"
                                      "kind = \"asdf\"\n"
                                      "timestamp_ns = 0\n"
                                      "can_id = 1\n";
    auto const r = discover_ns::parse_cdb_string(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_cdb_string rejects a malformed change event", "[discover][cdb][error]") {
    // bytes / before / after length mismatch.
    constexpr std::string_view text = "schema_version = 1\n"
                                      "baseline_ns = 0\n"
                                      "[baseline]\n"
                                      "id = []\n"
                                      "[[event]]\n"
                                      "kind = \"change\"\n"
                                      "timestamp_ns = 0\n"
                                      "can_id = 1\n"
                                      "bytes = [0, 1]\n"
                                      "before = [0]\n"
                                      "after = [1]\n";
    auto const r = discover_ns::parse_cdb_string(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("read_cdb errors clearly on a missing file", "[discover][cdb][file][error]") {
    auto const r = discover_ns::read_cdb("/no/such/path/should/not.exist.cdb");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}
