// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LOG_HPP
#define ST_LOG_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/ecu/ssm.hpp"
#include "st/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace st::log {

class LiveBuffer; // forward decl — full definition in st/log/live_buffer.hpp

// LogStream is the single-producer / single-consumer lock-free ring at the
// heart of Phase 3 datalogging. The transport's I/O thread is the producer
// (it pushes one Sample per ECU response); the UI thread, the CSV sink, or
// any other consumer is the single consumer pulling samples out.
//
// Each "sample" is a fixed-size record: a monotonic timestamp (in
// nanoseconds, relative to whatever clock the producer uses — we don't
// pin a clock here, just a 64-bit integer) followed by `pid_count`
// doubles in engineering units, one per logged PID. The slot layout is
// allocated once at construction so the hot path never allocates.
//
// Capacity is rounded up to the next power of two so the index math is a
// cheap mask. The producer drops (and counts) when the consumer falls
// behind — we never block the I/O thread.
//
// Threading rules — the SPSC pattern:
//   - `try_push` may be called from exactly ONE producer thread.
//   - `try_pop`  may be called from exactly ONE consumer thread.
//   - Stats / size accessors are safe to call from any thread but are
//     racy by construction (they are intended for UI display).
//   - `reset` is NOT concurrent-safe; the caller is responsible for
//     ensuring neither side is active when it is invoked.
//
// Pid count and capacity are immutable after construction.
class LogStream {
public:
    LogStream(std::size_t pid_count, std::size_t requested_capacity);

    LogStream(LogStream const &) = delete;
    LogStream &operator=(LogStream const &) = delete;
    LogStream(LogStream &&) = delete;
    LogStream &operator=(LogStream &&) = delete;

    [[nodiscard]] std::size_t pid_count() const noexcept {
        return pid_count_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool try_push(std::int64_t timestamp_ns, std::span<double const> values) noexcept;

    [[nodiscard]] bool try_pop(std::int64_t &out_timestamp_ns,
                               std::span<double> out_values) noexcept;

    [[nodiscard]] std::uint64_t pushed_count() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t popped_count() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t approximate_size() const noexcept {
        auto const h = head_.load(std::memory_order_relaxed);
        auto const t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    void reset() noexcept;

private:
    std::size_t pid_count_;
    std::size_t capacity_;
    std::size_t mask_;

    std::vector<std::int64_t> timestamps_;
    std::vector<double> values_;

    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

// A channel = one ECU value to read each cycle. `address` is the 24-bit
// SSM address; `data_type` controls how many bytes are read and how
// they're decoded; `scaling` (if present) converts raw to engineering
// units before push. `id` is a display-only label.
struct LogChannel {
    std::string id;
    std::uint32_t address{0};
    DataType data_type{DataType::Uint8};
    std::optional<Scaling> scaling;
};

// LogSession orchestrates the I/O loop on top of LogStream. Owns a
// transport, owns a worker thread, fills the stream at whatever cadence
// the transport supports.
//
// Lifecycle:
//   ctor → channels parsed and addresses expanded; thread not yet running
//   start() → spawns the I/O thread
//   stream().try_pop(...) → consumer-side; safe from any single thread
//   stop() → cooperative shutdown, joins the I/O thread; idempotent
//
// Threading rules:
//   - start / stop / is_running / cycle stats: any thread, but stop is
//     not safe to call from inside the I/O thread (it would deadlock).
//   - The transport handed to the session belongs to the session while
//     it's running. Don't share it with any other thread for that
//     duration — ITransport's send_recv is not thread-safe.
class LogSession {
public:
    // `ssm_framing` selects the SSM wire format the io-loop emits — pick
    // `IsoTp` on CAN-era Subarus (2008+ — every VA/VB WRX) and `KLine`
    // for ISO 9141 serial via a Tactrix OpenPort or for trace-replay
    // fixtures captured in the K-Line wire format. Same trap as Read
    // ROM: a default-KLine LogSession against a CAN-bus ECU produces
    // bytes the ECU drops silently, and the io-loop just accumulates
    // io_errors without explanation.
    LogSession(transport::ITransport &transport, std::vector<LogChannel> channels,
               std::size_t ring_capacity = 1024,
               ecu::ssm::Framing ssm_framing = ecu::ssm::Framing::KLine);

    // Attach a LiveBuffer the I/O thread should fan out per-channel
    // samples into, in addition to the LogStream the existing
    // consumer-pull pattern uses. Must be called BEFORE start() —
    // once the I/O thread is running, the attach list is read
    // without synchronization. The buffer must outlive the
    // LogSession (the gauge-cluster panel owns the buffer and keeps
    // it alive across session lifecycle in practice).
    //
    // Multiple LiveBuffers can be attached (every fan-out target
    // receives every sample). Empty/null pointers are skipped.
    // Channel count of each attached buffer must equal this session's
    // channel count; otherwise attach() returns InvalidArgument and
    // the buffer is not registered.
    [[nodiscard]] Status attach_live_buffer(LiveBuffer *buffer);

    LogSession(LogSession const &) = delete;
    LogSession &operator=(LogSession const &) = delete;
    LogSession(LogSession &&) = delete;
    LogSession &operator=(LogSession &&) = delete;
    ~LogSession();

    // Spawn the I/O thread. Per-cycle send_recv timeout applies to each
    // request the thread issues. Returns InvalidArgument if there are no
    // channels, or if the session is already running.
    [[nodiscard]] Status
    start(std::chrono::milliseconds per_cycle_timeout = std::chrono::milliseconds{500});

    // Signal the I/O thread to stop and join it. Safe to call multiple
    // times; safe to call before start; only unsafe to call from inside
    // the I/O thread (which would deadlock).
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] LogStream const &stream() const noexcept {
        return stream_;
    }
    [[nodiscard]] LogStream &stream() noexcept {
        return stream_;
    }

    // Successful cycles since the most recent start().
    [[nodiscard]] std::uint64_t cycles_completed() const noexcept {
        return cycles_completed_.load(std::memory_order_relaxed);
    }
    // Failed cycles (transport / parse errors) since the most recent start().
    [[nodiscard]] std::uint64_t io_errors() const noexcept {
        return io_errors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<LogChannel> const &channels() const noexcept {
        return channels_;
    }

private:
    void io_loop(std::chrono::milliseconds timeout) noexcept;

    transport::ITransport *transport_;
    std::vector<LogChannel> channels_;
    ecu::ssm::Framing ssm_framing_;

    // Expanded read plan: every byte we need to read in one shot, plus
    // an index into that flat list for each channel's first byte.
    std::vector<std::uint32_t> read_addresses_;
    std::vector<std::size_t> channel_byte_offsets_;

    LogStream stream_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread io_thread_;

    std::atomic<std::uint64_t> cycles_completed_{0};
    std::atomic<std::uint64_t> io_errors_{0};

    std::chrono::steady_clock::time_point start_time_{};

    // Attached fan-out targets. Read by the I/O thread without
    // synchronization; the attach API enforces "before start()" so
    // there's no concurrent access.
    std::vector<LiveBuffer *> attached_buffers_;
};

// Streams datalog samples to an ostream in standard CSV: a header row of
// channel ids (with units in brackets where the channel carries a unit)
// and one data row per sample with timestamp in milliseconds (three
// decimal places). Per-channel formatting precision follows the channel's
// scaling.precision when present; otherwise it falls back to a compact
// `%g` representation.
//
// The sink does not own the stream; the caller controls flush + close.
// Thread-safety: not safe for concurrent calls; intended to be driven by
// the single consumer of a LogStream.
class CsvSink {
public:
    CsvSink(std::ostream &out, std::vector<LogChannel> channels);

    // Write the header row. Call once per session, before any rows.
    void write_header();

    // Write a single data row. `values.size()` must equal channels().size().
    void write_row(std::int64_t timestamp_ns, std::span<double const> values);

    [[nodiscard]] std::vector<LogChannel> const &channels() const noexcept {
        return channels_;
    }

private:
    std::ostream &out_;
    std::vector<LogChannel> channels_;
};

} // namespace st::log

#endif // ST_LOG_HPP
