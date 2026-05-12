// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/ssm.hpp"
#include "st/log.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace log_ns = st::log;

TEST_CASE("LogStream rounds capacity up to next power of two",
          "[log][construct]") {
    log_ns::LogStream a{4, 1};   REQUIRE(a.capacity() == 2);
    log_ns::LogStream b{4, 2};   REQUIRE(b.capacity() == 2);
    log_ns::LogStream c{4, 3};   REQUIRE(c.capacity() == 4);
    log_ns::LogStream d{4, 9};   REQUIRE(d.capacity() == 16);
    log_ns::LogStream e{4, 64};  REQUIRE(e.capacity() == 64);
    REQUIRE(a.pid_count() == 4);
}

TEST_CASE("LogStream starts empty with zero counters", "[log][construct]") {
    log_ns::LogStream s{3, 8};
    REQUIRE(s.approximate_size() == 0);
    REQUIRE(s.pushed_count()  == 0);
    REQUIRE(s.popped_count()  == 0);
    REQUIRE(s.dropped_count() == 0);
}

TEST_CASE("LogStream round-trips one sample", "[log][push_pop]") {
    log_ns::LogStream s{3, 4};
    std::vector<double> const in{1.5, 2.5, 3.5};
    REQUIRE(s.try_push(123, in));

    REQUIRE(s.approximate_size() == 1);
    REQUIRE(s.pushed_count() == 1);

    std::int64_t        ts  = 0;
    std::vector<double> out(3, 0.0);
    REQUIRE(s.try_pop(ts, out));
    REQUIRE(ts == 123);
    REQUIRE(out == in);

    REQUIRE(s.approximate_size() == 0);
    REQUIRE(s.popped_count() == 1);
}

TEST_CASE("LogStream try_pop on empty returns false", "[log][push_pop]") {
    log_ns::LogStream   s{2, 4};
    std::int64_t        ts = 0;
    std::vector<double> out(2, 0.0);
    REQUIRE_FALSE(s.try_pop(ts, out));
    REQUIRE(s.popped_count() == 0);
}

TEST_CASE("LogStream wraps the ring index correctly", "[log][push_pop]") {
    // Capacity 4 (power of two). Push, pop, push, pop … 12 times so the
    // ring index wraps three times.
    log_ns::LogStream   s{1, 4};
    std::int64_t        ts = 0;
    std::vector<double> out(1, 0.0);
    for (int i = 0; i < 12; ++i) {
        std::vector<double> in{static_cast<double>(i)};
        REQUIRE(s.try_push(i, in));
        REQUIRE(s.try_pop(ts, out));
        REQUIRE(ts == i);
        REQUIRE(out[0] == static_cast<double>(i));
    }
    REQUIRE(s.dropped_count() == 0);
}

TEST_CASE("LogStream drops when full and counts the drops",
          "[log][drop]") {
    log_ns::LogStream s{1, 4};  // capacity 4
    for (int i = 0; i < 4; ++i) {
        std::vector<double> in{static_cast<double>(i)};
        REQUIRE(s.try_push(i, in));
    }
    REQUIRE(s.approximate_size() == 4);

    // Buffer full — the next three pushes must all fail.
    for (int i = 0; i < 3; ++i) {
        std::vector<double> in{99.0};
        REQUIRE_FALSE(s.try_push(100 + i, in));
    }
    REQUIRE(s.dropped_count() == 3);
    REQUIRE(s.pushed_count()  == 4);

    // Drain one slot, push must then succeed again.
    std::int64_t        ts = 0;
    std::vector<double> out(1, 0.0);
    REQUIRE(s.try_pop(ts, out));
    REQUIRE(ts == 0);

    std::vector<double> in{42.0};
    REQUIRE(s.try_push(200, in));
    REQUIRE(s.dropped_count() == 3);  // unchanged
}

TEST_CASE("LogStream::reset clears counters and the ring",
          "[log][reset]") {
    log_ns::LogStream s{2, 4};
    for (int i = 0; i < 3; ++i) {
        std::vector<double> in{static_cast<double>(i),
                               static_cast<double>(i) * 2};
        REQUIRE(s.try_push(i, in));
    }
    s.reset();

    REQUIRE(s.approximate_size() == 0);
    REQUIRE(s.pushed_count()  == 0);
    REQUIRE(s.popped_count()  == 0);
    REQUIRE(s.dropped_count() == 0);

    std::int64_t        ts = 0;
    std::vector<double> out(2, 0.0);
    REQUIRE_FALSE(s.try_pop(ts, out));
}

// =====================================================================
// LogSession
// =====================================================================

namespace {

// Build the SSM A8 request the session is going to emit for a given list
// of channels. Tests use this to seed MockTransport expectations.
std::vector<std::uint8_t> a8_request_for(
    std::vector<log_ns::LogChannel> const &channels) {
    std::vector<std::uint32_t> addrs;
    for (auto const &ch : channels) {
        auto const bytes = st::byte_size(ch.data_type);
        for (std::size_t i = 0; i < bytes; ++i) {
            addrs.push_back(ch.address + static_cast<std::uint32_t>(i));
        }
    }
    auto r = st::ecu::ssm::build_a8_request(addrs);
    REQUIRE(r.has_value());
    return *r;
}

// Build an SSM A8 positive response carrying `data_bytes` as the payload.
// Frame: 80 F0 10 [LEN=1+N] E8 [data...] [csum]
std::vector<std::uint8_t> a8_response(std::vector<std::uint8_t> data_bytes) {
    std::vector<std::uint8_t> resp;
    resp.reserve(5 + data_bytes.size() + 1);
    resp.push_back(0x80);
    resp.push_back(0xF0);
    resp.push_back(0x10);
    resp.push_back(static_cast<std::uint8_t>(1U + data_bytes.size()));
    resp.push_back(0xE8);
    resp.insert(resp.end(), data_bytes.begin(), data_bytes.end());
    resp.push_back(st::ecu::ssm::ssm_checksum(resp));
    return resp;
}

// Block the caller until `pred()` becomes true or the deadline passes.
// Returns whether the predicate fired.
template <typename Pred>
bool wait_until(std::chrono::milliseconds budget, Pred pred) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

} // namespace

TEST_CASE("LogSession refuses to start with no channels",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    log_ns::LogSession session{t, {}, 16};
    auto const r = session.start();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE_FALSE(session.is_running());
}

TEST_CASE("LogSession reads a single-byte channel and pushes samples",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<log_ns::LogChannel> const channels{
        {"iat", 0x1000, st::DataType::Uint8, std::nullopt},
    };
    auto const req = a8_request_for(channels);

    // Three cycles: bytes [0x42] [0x55] [0x77].
    t.expect_send_recv(req, a8_response({0x42}));
    t.expect_send_recv(req, a8_response({0x55}));
    t.expect_send_recv(req, a8_response({0x77}));

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());
    REQUIRE(session.is_running());

    // Wait for the I/O thread to drain the queue. Once exhausted, the
    // next send_recv errors out; that's our signal that all queued
    // cycles ran.
    REQUIRE(wait_until(1000ms, [&] { return t.remaining() == 0; }));
    // Let one error cycle land so we can verify io_errors() increments
    // separately from cycles_completed().
    REQUIRE(wait_until(200ms,
                       [&] { return session.io_errors() > 0; }));

    session.stop();
    REQUIRE_FALSE(session.is_running());

    REQUIRE(session.cycles_completed() == 3);
    REQUIRE(session.io_errors() >= 1);

    std::int64_t        ts = 0;
    std::vector<double> out(1, 0.0);
    std::vector<double> seen;
    while (session.stream().try_pop(ts, out)) {
        seen.push_back(out[0]);
    }
    REQUIRE(seen == std::vector<double>{0x42, 0x55, 0x77});
}

TEST_CASE("LogSession decodes a multi-byte big-endian channel",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // One channel: RPM as uint16_be at 0x0200. The session must read
    // 0x0200 AND 0x0201 in a single A8 request and reassemble the value.
    std::vector<log_ns::LogChannel> const channels{
        {"rpm", 0x0200, st::DataType::Uint16Be, std::nullopt},
    };
    auto const req = a8_request_for(channels);

    // 0x1A 0x85 = 6789 rpm.
    t.expect_send_recv(req, a8_response({0x1A, 0x85}));

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());

    REQUIRE(wait_until(500ms, [&] { return session.cycles_completed() >= 1; }));
    session.stop();

    std::int64_t        ts = 0;
    std::vector<double> out(1, 0.0);
    REQUIRE(session.stream().try_pop(ts, out));
    REQUIRE(out[0] == 6789.0);
}

TEST_CASE("LogSession applies per-channel scaling on the I/O thread",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // PID at 0x300, uint8, scaling = raw * 0.5 - 40 (a typical Subaru
    // intake-air-temperature scaling). Raw 0x90 = 144 → 144*0.5 - 40 = 32.
    st::Scaling scaling;
    scaling.id        = "iat_c";
    scaling.formula   = st::LinearScaling{0.5, -40.0};
    scaling.unit      = "C";
    scaling.precision = 1;
    scaling.data_type = st::DataType::Uint8;

    std::vector<log_ns::LogChannel> const channels{
        {"iat", 0x0300, st::DataType::Uint8, scaling},
    };
    auto const req = a8_request_for(channels);
    t.expect_send_recv(req, a8_response({0x90}));

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());
    REQUIRE(wait_until(500ms, [&] { return session.cycles_completed() >= 1; }));
    session.stop();

    std::int64_t        ts = 0;
    std::vector<double> out(1, 0.0);
    REQUIRE(session.stream().try_pop(ts, out));
    REQUIRE(out[0] == 32.0);
}

TEST_CASE("LogSession reads multiple heterogeneous channels per cycle",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Three channels with different widths, batched into one A8 request:
    //   ch0 rpm    uint16_be @ 0x0200 -> bytes [0x0200, 0x0201]
    //   ch1 iat    uint8     @ 0x0210 -> byte  [0x0210]
    //   ch2 maf    uint16_be @ 0x0220 -> bytes [0x0220, 0x0221]
    std::vector<log_ns::LogChannel> const channels{
        {"rpm", 0x0200, st::DataType::Uint16Be, std::nullopt},
        {"iat", 0x0210, st::DataType::Uint8,    std::nullopt},
        {"maf", 0x0220, st::DataType::Uint16Be, std::nullopt},
    };
    auto const req = a8_request_for(channels);
    // 5 data bytes: 0x1A 0x85, 0x32, 0x07 0xD0  ->  6789, 50, 2000.
    t.expect_send_recv(req,
                       a8_response({0x1A, 0x85, 0x32, 0x07, 0xD0}));

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());
    REQUIRE(wait_until(500ms, [&] { return session.cycles_completed() >= 1; }));
    session.stop();

    std::int64_t        ts = 0;
    std::vector<double> out(3, 0.0);
    REQUIRE(session.stream().try_pop(ts, out));
    REQUIRE(out[0] == 6789.0);
    REQUIRE(out[1] == 50.0);
    REQUIRE(out[2] == 2000.0);
}

TEST_CASE("LogSession::stop is idempotent and safe on a never-started session",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    std::vector<log_ns::LogChannel> const channels{
        {"x", 0x0001, st::DataType::Uint8, std::nullopt},
    };
    log_ns::LogSession session{t, channels, 16};
    // Stop before start — must not deadlock.
    session.stop();
    // Stop after a no-op start failure — must also not deadlock.
    log_ns::LogSession nochans{t, {}, 16};
    (void) nochans.start();
    nochans.stop();
    nochans.stop();
    REQUIRE_FALSE(nochans.is_running());
}

TEST_CASE("LogSession refuses to start twice",
          "[log][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    std::vector<log_ns::LogChannel> const channels{
        {"x", 0x0001, st::DataType::Uint8, std::nullopt},
    };
    // Enqueue one response so the thread has something to do without
    // immediately erroring; this keeps the I/O thread alive for the
    // second start() call to land on a running session.
    auto const req = a8_request_for(channels);
    t.expect_send_recv(req, a8_response({0x10}));

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());

    auto const r2 = session.start();
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().code() == st::ErrorCode::InvalidArgument);

    session.stop();
}

// =====================================================================
// CsvSink
// =====================================================================

namespace {

st::Scaling make_scaling(std::string id, std::string unit, int precision) {
    st::Scaling s;
    s.id        = std::move(id);
    s.formula   = st::LinearScaling{1.0, 0.0};
    s.unit      = std::move(unit);
    s.precision = precision;
    s.data_type = st::DataType::Uint8;
    return s;
}

} // namespace

TEST_CASE("CsvSink emits a header derived from channel ids + units",
          "[log][csv]") {
    std::vector<log_ns::LogChannel> channels{
        {"rpm", 0x0200, st::DataType::Uint16Be, make_scaling("rpm_x1", "rpm", 0)},
        {"iat", 0x0210, st::DataType::Uint8,    make_scaling("iat_c",  "C",   1)},
        // Channel with no scaling: header label falls back to bare id.
        {"raw", 0x0220, st::DataType::Uint8,    std::nullopt},
    };

    std::stringstream out;
    log_ns::CsvSink sink{out, channels};
    sink.write_header();

    REQUIRE(out.str() == "timestamp_ms,rpm [rpm],iat [C],raw\n");
}

TEST_CASE("CsvSink formats values with per-channel precision",
          "[log][csv]") {
    std::vector<log_ns::LogChannel> channels{
        {"rpm", 0x0200, st::DataType::Uint16Be, make_scaling("rpm_x1", "rpm", 0)},
        {"iat", 0x0210, st::DataType::Uint8,    make_scaling("iat_c",  "C",   1)},
        {"raw", 0x0220, st::DataType::Uint8,    std::nullopt},
    };

    std::stringstream out;
    log_ns::CsvSink sink{out, channels};

    // 1_234_567_000 ns = 1234.567 ms.
    // 50.26 (not 50.25) avoids the implementation-defined "half" case;
    // %.1f on an exact midpoint differs across C libraries.
    std::vector<double> const values{6789.0, 50.26, 42.0};
    sink.write_row(1'234'567'000LL, values);

    REQUIRE(out.str() == "1234.567,6789,50.3,42\n");
}

TEST_CASE("CsvSink reports timestamps to millisecond precision",
          "[log][csv]") {
    std::vector<log_ns::LogChannel> channels{
        {"x", 0x0001, st::DataType::Uint8, std::nullopt},
    };
    std::stringstream out;
    log_ns::CsvSink sink{out, channels};

    sink.write_row(0,             std::vector<double>{1.0});  // 0
    sink.write_row(500'000,       std::vector<double>{2.0});  // 0.500 ms
    sink.write_row(1'500'000,     std::vector<double>{3.0});  // 1.500 ms
    sink.write_row(1'234'567'000, std::vector<double>{4.0});  // 1234.567 ms

    REQUIRE(out.str()
            == "0.000,1\n0.500,2\n1.500,3\n1234.567,4\n");
}

TEST_CASE("CsvSink end-to-end against LogSession",
          "[log][csv][session]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::vector<log_ns::LogChannel> channels{
        {"rpm", 0x0200, st::DataType::Uint16Be, make_scaling("rpm_x1", "rpm", 0)},
        {"iat", 0x0210, st::DataType::Uint8,    make_scaling("iat_c",  "C",   0)},
    };
    auto const req = a8_request_for(channels);
    t.expect_send_recv(req, a8_response({0x1A, 0x85, 0x32}));  // 6789, 50
    t.expect_send_recv(req, a8_response({0x1A, 0x86, 0x33}));  // 6790, 51

    log_ns::LogSession session{t, channels, 16};
    REQUIRE(session.start().has_value());
    REQUIRE(wait_until(500ms, [&] { return session.cycles_completed() >= 2; }));
    session.stop();

    std::stringstream  out;
    log_ns::CsvSink    sink{out, channels};
    sink.write_header();

    std::int64_t        ts = 0;
    std::vector<double> values(channels.size(), 0.0);
    while (session.stream().try_pop(ts, values)) {
        sink.write_row(ts, values);
    }

    auto const text = out.str();
    REQUIRE(text.starts_with("timestamp_ms,rpm [rpm],iat [C]\n"));
    // The remaining body has two rows; values are deterministic, timestamps
    // depend on wall-clock and are skipped in this assertion.
    REQUIRE(text.find(",6789,50\n") != std::string::npos);
    REQUIRE(text.find(",6790,51\n") != std::string::npos);
}

TEST_CASE("LogStream SPSC stress: producer + consumer agree on totals",
          "[log][concurrent][stress]") {
    // Producer pushes `N` distinctly-numbered samples as fast as it can;
    // consumer pops on its own thread, slightly slower, so the ring fills
    // and we exercise the drop path. We then verify that the counts add
    // up and that consumed samples arrive in strictly-monotonic order.
    constexpr int N        = 50'000;
    constexpr int kPidCount = 2;
    log_ns::LogStream s{kPidCount, 128};

    std::atomic<bool>             producer_done{false};
    std::vector<std::int64_t>     consumed_ts;
    consumed_ts.reserve(static_cast<std::size_t>(N));

    std::thread producer{[&] {
        for (int i = 0; i < N; ++i) {
            std::vector<double> v{static_cast<double>(i),
                                  -static_cast<double>(i)};
            (void) s.try_push(i, v);
        }
        producer_done.store(true, std::memory_order_release);
    }};

    std::thread consumer{[&] {
        std::int64_t        ts = 0;
        std::vector<double> out(kPidCount, 0.0);
        while (true) {
            if (s.try_pop(ts, out)) {
                // Sanity: the second slot must mirror the first.
                REQUIRE(out[1] == -out[0]);
                consumed_ts.push_back(ts);
                continue;
            }
            // Empty for now — if the producer is done and we've drained,
            // we're finished.
            if (producer_done.load(std::memory_order_acquire)
                && s.approximate_size() == 0) {
                break;
            }
            std::this_thread::yield();
        }
    }};

    producer.join();
    consumer.join();

    // Every sample is either consumed or dropped; none vanish, none
    // duplicate.
    REQUIRE(consumed_ts.size() + s.dropped_count() == static_cast<std::uint64_t>(N));
    REQUIRE(s.pushed_count() + s.dropped_count() == static_cast<std::uint64_t>(N));

    // Consumed timestamps must be strictly increasing — the SPSC ring
    // preserves FIFO order even though some samples were dropped.
    for (std::size_t i = 1; i < consumed_ts.size(); ++i) {
        REQUIRE(consumed_ts[i] > consumed_ts[i - 1]);
    }
}
