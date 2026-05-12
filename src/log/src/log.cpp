// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/ecu/ssm.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace st::log {

namespace {

constexpr std::size_t kMinCapacity = 2;

std::size_t round_capacity(std::size_t requested) noexcept {
    auto const at_least = std::max(requested, kMinCapacity);
    return std::bit_ceil(at_least);
}

// Decode `bytes.size() == byte_size(dt)` raw bytes into an engineering-
// agnostic double. Mirrors st::read_typed but operates on a span so the
// I/O hot path doesn't have to wrap each per-cycle response in a Rom.
double interpret_bytes(std::span<std::uint8_t const> bytes, DataType dt) noexcept {
    auto const b = bytes.data();
    switch (dt) {
        case DataType::Uint8:
            return static_cast<double>(b[0]);
        case DataType::Int8:
            return static_cast<double>(static_cast<std::int8_t>(b[0]));
        case DataType::Uint16Be: {
            auto const v = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(b[0]) << 8U) | b[1]);
            return static_cast<double>(v);
        }
        case DataType::Uint16Le: {
            auto const v = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(b[1]) << 8U) | b[0]);
            return static_cast<double>(v);
        }
        case DataType::Int16Be: {
            auto const v = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(b[0]) << 8U) | b[1]);
            return static_cast<double>(static_cast<std::int16_t>(v));
        }
        case DataType::Int16Le: {
            auto const v = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(b[1]) << 8U) | b[0]);
            return static_cast<double>(static_cast<std::int16_t>(v));
        }
        case DataType::Uint32Be: {
            auto const v = (static_cast<std::uint32_t>(b[0]) << 24U)
                         | (static_cast<std::uint32_t>(b[1]) << 16U)
                         | (static_cast<std::uint32_t>(b[2]) << 8U)
                         |  static_cast<std::uint32_t>(b[3]);
            return static_cast<double>(v);
        }
        case DataType::Uint32Le: {
            auto const v = (static_cast<std::uint32_t>(b[3]) << 24U)
                         | (static_cast<std::uint32_t>(b[2]) << 16U)
                         | (static_cast<std::uint32_t>(b[1]) << 8U)
                         |  static_cast<std::uint32_t>(b[0]);
            return static_cast<double>(v);
        }
        case DataType::Int32Be: {
            auto const v = (static_cast<std::uint32_t>(b[0]) << 24U)
                         | (static_cast<std::uint32_t>(b[1]) << 16U)
                         | (static_cast<std::uint32_t>(b[2]) << 8U)
                         |  static_cast<std::uint32_t>(b[3]);
            return static_cast<double>(static_cast<std::int32_t>(v));
        }
        case DataType::Int32Le: {
            auto const v = (static_cast<std::uint32_t>(b[3]) << 24U)
                         | (static_cast<std::uint32_t>(b[2]) << 16U)
                         | (static_cast<std::uint32_t>(b[1]) << 8U)
                         |  static_cast<std::uint32_t>(b[0]);
            return static_cast<double>(static_cast<std::int32_t>(v));
        }
        case DataType::Float32Be: {
            auto const bits = (static_cast<std::uint32_t>(b[0]) << 24U)
                            | (static_cast<std::uint32_t>(b[1]) << 16U)
                            | (static_cast<std::uint32_t>(b[2]) << 8U)
                            |  static_cast<std::uint32_t>(b[3]);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return static_cast<double>(f);
        }
        case DataType::Float32Le: {
            auto const bits = (static_cast<std::uint32_t>(b[3]) << 24U)
                            | (static_cast<std::uint32_t>(b[2]) << 16U)
                            | (static_cast<std::uint32_t>(b[1]) << 8U)
                            |  static_cast<std::uint32_t>(b[0]);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return static_cast<double>(f);
        }
    }
    return 0.0;
}

} // namespace

// =====================================================================
// LogStream
// =====================================================================

LogStream::LogStream(std::size_t pid_count, std::size_t requested_capacity)
    : pid_count_{pid_count},
      capacity_{round_capacity(requested_capacity)},
      mask_{capacity_ - 1},
      timestamps_(capacity_, 0),
      values_(capacity_ * pid_count, 0.0) {
    assert(pid_count_ > 0 && "LogStream requires at least one PID");
}

bool LogStream::try_push(std::int64_t              timestamp_ns,
                         std::span<double const>   values) noexcept {
    assert(values.size() == pid_count_
           && "values.size() must equal pid_count()");

    auto const head = head_.load(std::memory_order_relaxed);
    auto const tail = tail_.load(std::memory_order_acquire);

    if (head - tail >= capacity_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto const slot = head & mask_;
    timestamps_[slot] = timestamp_ns;
    auto const base   = slot * pid_count_;
    for (std::size_t i = 0; i < pid_count_; ++i) {
        values_[base + i] = values[i];
    }
    head_.store(head + 1, std::memory_order_release);
    return true;
}

bool LogStream::try_pop(std::int64_t      &out_timestamp_ns,
                        std::span<double>  out_values) noexcept {
    assert(out_values.size() == pid_count_
           && "out_values.size() must equal pid_count()");

    auto const tail = tail_.load(std::memory_order_relaxed);
    auto const head = head_.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }

    auto const slot = tail & mask_;
    out_timestamp_ns = timestamps_[slot];
    auto const base  = slot * pid_count_;
    for (std::size_t i = 0; i < pid_count_; ++i) {
        out_values[i] = values_[base + i];
    }
    tail_.store(tail + 1, std::memory_order_release);
    return true;
}

void LogStream::reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
}

// =====================================================================
// LogSession
// =====================================================================

namespace {

std::size_t total_bytes(std::vector<LogChannel> const &channels) noexcept {
    std::size_t n = 0;
    for (auto const &ch : channels) {
        n += byte_size(ch.data_type);
    }
    return n;
}

} // namespace

LogSession::LogSession(transport::ITransport &transport,
                       std::vector<LogChannel> channels,
                       std::size_t            ring_capacity)
    : transport_{&transport},
      channels_{std::move(channels)},
      stream_{channels_.empty() ? std::size_t{1} : channels_.size(), ring_capacity} {
    // Build the flat read plan: each channel contributes byte_size(dt)
    // consecutive addresses; we remember each channel's first-byte
    // offset into that flat list so the response can be re-grouped.
    read_addresses_.reserve(total_bytes(channels_));
    channel_byte_offsets_.reserve(channels_.size());

    for (auto const &ch : channels_) {
        channel_byte_offsets_.push_back(read_addresses_.size());
        auto const bytes = byte_size(ch.data_type);
        for (std::size_t i = 0; i < bytes; ++i) {
            read_addresses_.push_back(ch.address + static_cast<std::uint32_t>(i));
        }
    }
}

LogSession::~LogSession() {
    stop();
}

Status LogSession::start(std::chrono::milliseconds per_cycle_timeout) {
    if (channels_.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "LogSession has no channels");
    }
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        // Already running — restore the flag (compare-and-swap style)
        // for an honest no-op.
        return failure(ErrorCode::InvalidArgument,
                       "LogSession is already running");
    }

    stop_requested_.store(false, std::memory_order_release);
    cycles_completed_.store(0, std::memory_order_relaxed);
    io_errors_.store(0, std::memory_order_relaxed);
    stream_.reset();
    start_time_ = std::chrono::steady_clock::now();

    io_thread_ = std::thread([this, per_cycle_timeout] {
        io_loop(per_cycle_timeout);
    });
    return ok();
}

void LogSession::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void LogSession::io_loop(std::chrono::milliseconds timeout) noexcept {
    ecu::ssm::SsmClient client{*transport_};
    std::vector<double> sample(channels_.size(), 0.0);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        auto resp = client.read(read_addresses_, timeout);
        if (!resp.has_value()) {
            io_errors_.fetch_add(1, std::memory_order_relaxed);
            // Back off briefly so a stuck transport doesn't burn CPU.
            // Short enough that stop() still feels responsive (<= 20 ms).
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }

        auto const &bytes = *resp;
        bool        ok_cycle = true;
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            auto const  offset       = channel_byte_offsets_[i];
            auto const  bytes_per_ch = byte_size(channels_[i].data_type);
            if (offset + bytes_per_ch > bytes.size()) {
                ok_cycle = false;
                break;
            }
            std::span<std::uint8_t const> slice{bytes.data() + offset,
                                                 bytes_per_ch};
            double const raw = interpret_bytes(slice, channels_[i].data_type);
            sample[i] = channels_[i].scaling.has_value()
                            ? apply_scaling(raw, *channels_[i].scaling)
                            : raw;
        }
        if (!ok_cycle) {
            io_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        auto const ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
        (void) stream_.try_push(ts_ns, sample);
        cycles_completed_.fetch_add(1, std::memory_order_relaxed);
    }
}

// =====================================================================
// CsvSink
// =====================================================================

namespace {

// Header label for a channel: "id" or "id [unit]" when scaling carries
// a non-empty unit. Keeps the spreadsheet column readable without
// requiring a separate units row.
std::string header_label_for(LogChannel const &ch) {
    if (ch.scaling.has_value() && !ch.scaling->unit.empty()) {
        return ch.id + " [" + ch.scaling->unit + "]";
    }
    return ch.id;
}

// Format one value into a 32-byte buffer using the channel's precision,
// falling back to compact %g when no scaling is set. Returns a pointer
// to the buffer (which is owned by the caller).
char const *format_value(double v, LogChannel const &ch, char *buf,
                         std::size_t buf_size) {
    if (ch.scaling.has_value()) {
        int prec = ch.scaling->precision;
        if (prec < 0) prec = 0;
        if (prec > 9) prec = 9;
        std::snprintf(buf, buf_size, "%.*f", prec, v);
    } else {
        std::snprintf(buf, buf_size, "%g", v);
    }
    return buf;
}

} // namespace

CsvSink::CsvSink(std::ostream &out, std::vector<LogChannel> channels)
    : out_{out}, channels_{std::move(channels)} {}

void CsvSink::write_header() {
    out_ << "timestamp_ms";
    for (auto const &ch : channels_) {
        out_ << ',' << header_label_for(ch);
    }
    out_ << '\n';
}

void CsvSink::write_row(std::int64_t           timestamp_ns,
                        std::span<double const> values) {
    assert(values.size() == channels_.size()
           && "CsvSink::write_row size mismatch");

    // Timestamp: nanoseconds → milliseconds with 3 decimal places. Avoids
    // floating-point drift for cycle-counter values produced by
    // std::chrono::nanoseconds difference arithmetic.
    auto const whole_ms = timestamp_ns / 1'000'000;
    auto const frac_us  = std::abs(timestamp_ns % 1'000'000) / 1'000;
    char ts_buf[32];
    if (timestamp_ns < 0 && whole_ms == 0) {
        std::snprintf(ts_buf, sizeof(ts_buf), "-0.%03lld",
                      static_cast<long long>(frac_us));
    } else {
        std::snprintf(ts_buf, sizeof(ts_buf), "%lld.%03lld",
                      static_cast<long long>(whole_ms),
                      static_cast<long long>(frac_us));
    }
    out_ << ts_buf;

    char val_buf[32];
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        out_ << ','
             << format_value(values[i], channels_[i], val_buf, sizeof(val_buf));
    }
    out_ << '\n';
}

} // namespace st::log
