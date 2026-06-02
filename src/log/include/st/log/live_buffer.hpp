// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// LiveBuffer — a per-channel SPSC ring of recent samples for the
// live datalogger gauge cluster (docs/32-live-datalogger.md). The
// I/O thread (LogSession::io_loop) calls `push()`; the UI thread
// calls `snapshot()` / `latest()` to feed the gauge mini-line and
// headline value at 60 fps.
//
// Snapshot semantics: the buffer holds the most-recent N samples
// per channel; reads peek without consuming. The UI thread re-reads
// the same samples every render frame; new samples replace the
// oldest one (per-channel capacity is fixed at construction).
//
// SPSC discipline (enforced by the calling pattern):
//   - One producer thread per channel (the LogSession owns the
//     LiveBuffer; LogSession's I/O loop is the only producer).
//   - One consumer thread per channel (the panel that owns the
//     gauge cluster).
//
// Memory ordering: release on write_count after writing the slot;
// acquire on write_count before reading slots. No mutex, no
// shared_mutex. Capacity is rounded up to a power of two for cheap
// modulo via mask.
//
// Race window: at the wraparound boundary, the consumer can in
// principle read a partial write of the {timestamp, value} pair.
// For an 8192-slot ring at 100 Hz, the producer needs ~80 seconds
// to wrap; the consumer's snapshot takes microseconds. The window
// is negligible for gauge purposes (one momentarily-jittered
// sample at 100 Hz is invisible). If a future use case needs
// strict tear-free reads (e.g. for replay accuracy), the slot
// layout can grow a sequence number per entry.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace st::log {

class LiveBuffer {
public:
    struct Sample {
        std::int64_t timestamp_ns{0};
        double value{0.0};
    };

    // Construct a buffer over `channel_count` channels, each holding
    // up to `per_channel_capacity` samples (rounded UP to the next
    // power of two; the minimum is 2). `channel_count = 0` is a
    // degenerate "no channels" buffer that refuses every push and
    // returns empty snapshots.
    LiveBuffer(std::size_t channel_count, std::size_t per_channel_capacity);

    LiveBuffer(LiveBuffer const &) = delete;
    LiveBuffer &operator=(LiveBuffer const &) = delete;
    LiveBuffer(LiveBuffer &&) = delete;
    LiveBuffer &operator=(LiveBuffer &&) = delete;

    // Producer side. Append a sample to `channel_idx`'s ring. Wraps
    // when capacity is exceeded — the oldest sample is overwritten.
    // No-op if `channel_idx` is out of range. Thread-safe vs
    // concurrent `snapshot`/`latest` calls.
    void push(std::size_t channel_idx, std::int64_t timestamp_ns, double value) noexcept;

    // Consumer side. Returns up to `count` most-recent samples for
    // `channel_idx` in chronological order (oldest first). If the
    // buffer has fewer than `count` samples, returns what's there.
    // Returns empty for an out-of-range channel.
    [[nodiscard]] std::vector<Sample> snapshot(std::size_t channel_idx,
                                               std::size_t count) const;

    // Most-recent single sample for `channel_idx`, or nullopt if
    // none has been pushed yet (or the channel is out of range).
    [[nodiscard]] std::optional<Sample> latest(std::size_t channel_idx) const noexcept;

    [[nodiscard]] std::size_t channel_count() const noexcept {
        return channels_.size();
    }

    // Per-channel capacity (rounded-up power of two). Same for every
    // channel — set at construction.
    [[nodiscard]] std::size_t per_channel_capacity() const noexcept {
        return capacity_;
    }

    // Total samples pushed to a channel since construction (does NOT
    // decrease on wrap; use modulo per_channel_capacity for the
    // current slot offset). Returns 0 for an out-of-range channel.
    [[nodiscard]] std::uint64_t pushed_count(std::size_t channel_idx) const noexcept;

private:
    struct Channel {
        std::atomic<std::uint64_t> write_count{0};
        std::vector<Sample> ring;
    };

    // unique_ptr so the atomic-containing Channel is non-movable
    // without forcing LiveBuffer to be non-movable for unrelated
    // reasons (vector<Channel> would need Channel to be movable).
    std::vector<std::unique_ptr<Channel>> channels_;
    std::size_t capacity_{0};
    std::size_t mask_{0};
};

} // namespace st::log
