// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/live_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace lb = st::log;

TEST_CASE("LiveBuffer rounds capacity up to a power of two",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 100};
    REQUIRE(buf.per_channel_capacity() == 128); // 100 → 128

    lb::LiveBuffer buf2{1, 256};
    REQUIRE(buf2.per_channel_capacity() == 256); // already power of 2

    lb::LiveBuffer buf3{1, 1};
    REQUIRE(buf3.per_channel_capacity() == 2); // floor at 2

    lb::LiveBuffer buf4{1, 0};
    REQUIRE(buf4.per_channel_capacity() == 2); // floor at 2
}

TEST_CASE("LiveBuffer latest returns nullopt before any push",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{3, 8};
    REQUIRE_FALSE(buf.latest(0).has_value());
    REQUIRE_FALSE(buf.latest(1).has_value());
    REQUIRE_FALSE(buf.latest(2).has_value());
    REQUIRE(buf.pushed_count(0) == 0);
}

TEST_CASE("LiveBuffer push/latest round-trip a single sample",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 8};
    buf.push(0, 1000, 3.14);

    auto last = buf.latest(0);
    REQUIRE(last.has_value());
    REQUIRE(last->timestamp_ns == 1000);
    REQUIRE(last->value == 3.14);
    REQUIRE(buf.pushed_count(0) == 1);
}

TEST_CASE("LiveBuffer snapshot returns samples in chronological order",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 8};
    for (int i = 0; i < 5; ++i) {
        buf.push(0, i * 100, static_cast<double>(i));
    }

    auto snap = buf.snapshot(0, 5);
    REQUIRE(snap.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        REQUIRE(snap[i].timestamp_ns == static_cast<std::int64_t>(i * 100));
        REQUIRE(snap[i].value == static_cast<double>(i));
    }
}

TEST_CASE("LiveBuffer snapshot caps at the number of pushed samples",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 16};
    buf.push(0, 100, 1.0);
    buf.push(0, 200, 2.0);

    auto snap = buf.snapshot(0, 10); // asked for 10, only 2 pushed
    REQUIRE(snap.size() == 2);
    REQUIRE(snap[0].value == 1.0);
    REQUIRE(snap[1].value == 2.0);
}

TEST_CASE("LiveBuffer snapshot caps at ring capacity",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 4}; // capacity 4
    for (int i = 0; i < 10; ++i) {
        buf.push(0, i * 100, static_cast<double>(i));
    }
    REQUIRE(buf.pushed_count(0) == 10);

    auto snap = buf.snapshot(0, 100); // asked for 100, only 4 fit
    REQUIRE(snap.size() == 4);
    // Most recent 4 samples = i = 6, 7, 8, 9.
    REQUIRE(snap[0].value == 6.0);
    REQUIRE(snap[1].value == 7.0);
    REQUIRE(snap[2].value == 8.0);
    REQUIRE(snap[3].value == 9.0);
}

TEST_CASE("LiveBuffer snapshot of partial recent range",
          "[log][live_buffer]") {
    lb::LiveBuffer buf{1, 16};
    for (int i = 0; i < 10; ++i) {
        buf.push(0, i * 100, static_cast<double>(i));
    }
    auto snap = buf.snapshot(0, 3); // last 3 only
    REQUIRE(snap.size() == 3);
    REQUIRE(snap[0].value == 7.0);
    REQUIRE(snap[1].value == 8.0);
    REQUIRE(snap[2].value == 9.0);
}

TEST_CASE("LiveBuffer wrap-around behaves correctly",
          "[log][live_buffer][wrap]") {
    lb::LiveBuffer buf{1, 8}; // capacity 8

    // Push exactly capacity-many samples → all fit, no wrap.
    for (int i = 0; i < 8; ++i) {
        buf.push(0, i, static_cast<double>(i));
    }
    auto snap1 = buf.snapshot(0, 8);
    REQUIRE(snap1.size() == 8);
    REQUIRE(snap1[0].value == 0.0);
    REQUIRE(snap1[7].value == 7.0);

    // Push 4 more — wraps. Slots [0..3] now hold values 8..11.
    for (int i = 8; i < 12; ++i) {
        buf.push(0, i, static_cast<double>(i));
    }
    auto snap2 = buf.snapshot(0, 8);
    REQUIRE(snap2.size() == 8);
    // Most-recent 8 are values 4..11 in order.
    for (std::size_t i = 0; i < 8; ++i) {
        REQUIRE(snap2[i].value == static_cast<double>(i + 4));
    }

    REQUIRE(buf.pushed_count(0) == 12);
}

TEST_CASE("LiveBuffer keeps channels independent",
          "[log][live_buffer][multi-channel]") {
    lb::LiveBuffer buf{3, 4};
    buf.push(0, 100, 1.0);
    buf.push(1, 200, 2.0);
    buf.push(1, 300, 3.0);
    buf.push(2, 400, 4.0);
    buf.push(2, 500, 5.0);
    buf.push(2, 600, 6.0);

    REQUIRE(buf.pushed_count(0) == 1);
    REQUIRE(buf.pushed_count(1) == 2);
    REQUIRE(buf.pushed_count(2) == 3);

    REQUIRE(buf.latest(0)->value == 1.0);
    REQUIRE(buf.latest(1)->value == 3.0);
    REQUIRE(buf.latest(2)->value == 6.0);

    auto snap1 = buf.snapshot(1, 10);
    REQUIRE(snap1.size() == 2);
    REQUIRE(snap1[0].value == 2.0);
    REQUIRE(snap1[1].value == 3.0);
}

TEST_CASE("LiveBuffer out-of-range channel is benign",
          "[log][live_buffer][edge]") {
    lb::LiveBuffer buf{2, 8};
    buf.push(5, 100, 1.0); // out of range — no-op
    REQUIRE_FALSE(buf.latest(5).has_value());
    REQUIRE(buf.snapshot(5, 10).empty());
    REQUIRE(buf.pushed_count(5) == 0);
}

TEST_CASE("LiveBuffer snapshot with count==0 returns empty",
          "[log][live_buffer][edge]") {
    lb::LiveBuffer buf{1, 8};
    buf.push(0, 100, 1.0);
    REQUIRE(buf.snapshot(0, 0).empty());
}

TEST_CASE("LiveBuffer zero-channel construction is degenerate but safe",
          "[log][live_buffer][edge]") {
    lb::LiveBuffer buf{0, 8};
    REQUIRE(buf.channel_count() == 0);
    buf.push(0, 100, 1.0); // no-op, no crash
    REQUIRE_FALSE(buf.latest(0).has_value());
    REQUIRE(buf.snapshot(0, 10).empty());
}

TEST_CASE("LiveBuffer concurrent producer + consumer holds monotonic ordering",
          "[log][live_buffer][concurrency]") {
    // SPSC discipline: one producer thread writes samples for 200ms;
    // one consumer thread snapshots every 5ms. Assert (a) the
    // consumer's latest() never goes backwards in timestamp_ns, (b)
    // pushed_count grows monotonically, (c) no crashes.
    lb::LiveBuffer buf{1, 1024};
    std::atomic<bool> stop{false};

    std::thread producer{[&] {
        std::int64_t ts = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            buf.push(0, ts, static_cast<double>(ts));
            ++ts;
            // No sleep — write as fast as possible to stress the ring.
        }
    }};

    std::int64_t last_seen_ts = -1;
    std::uint64_t last_seen_count = 0;
    auto const start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds{200}) {
        auto sample = buf.latest(0);
        if (sample.has_value()) {
            // Newer-or-equal timestamps only.
            REQUIRE(sample->timestamp_ns >= last_seen_ts);
            last_seen_ts = sample->timestamp_ns;
        }
        auto const count = buf.pushed_count(0);
        REQUIRE(count >= last_seen_count);
        last_seen_count = count;
        // Snapshots stay within capacity bounds.
        auto snap = buf.snapshot(0, 100);
        REQUIRE(snap.size() <= 100);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    stop.store(true);
    producer.join();

    // Some samples must have flowed through.
    REQUIRE(buf.pushed_count(0) > 0);
}
