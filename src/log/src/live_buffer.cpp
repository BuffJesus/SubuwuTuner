// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/live_buffer.hpp"

#include <algorithm>

namespace st::log {

namespace {

[[nodiscard]] std::size_t round_up_pow2(std::size_t n) noexcept {
    if (n <= 2)
        return 2;
    --n;
    n |= n >> 1U;
    n |= n >> 2U;
    n |= n >> 4U;
    n |= n >> 8U;
    n |= n >> 16U;
    if constexpr (sizeof(std::size_t) > 4) {
        n |= n >> 32U;
    }
    return n + 1;
}

} // namespace

LiveBuffer::LiveBuffer(std::size_t channel_count, std::size_t per_channel_capacity) {
    capacity_ = round_up_pow2(per_channel_capacity);
    mask_ = capacity_ - 1;
    channels_.reserve(channel_count);
    for (std::size_t i = 0; i < channel_count; ++i) {
        auto ch = std::make_unique<Channel>();
        ch->ring.assign(capacity_, Sample{});
        channels_.push_back(std::move(ch));
    }
}

void LiveBuffer::push(std::size_t channel_idx, std::int64_t timestamp_ns,
                      double value) noexcept {
    if (channel_idx >= channels_.size())
        return;
    auto &ch = *channels_[channel_idx];
    // Slot for the next sample = current write_count & mask. Write
    // the payload, then publish the new write_count with release so
    // the consumer sees both the slot contents and the count update
    // atomically.
    auto const wc = ch.write_count.load(std::memory_order_relaxed);
    ch.ring[wc & mask_] = Sample{timestamp_ns, value};
    ch.write_count.store(wc + 1, std::memory_order_release);
}

std::vector<LiveBuffer::Sample> LiveBuffer::snapshot(std::size_t channel_idx,
                                                     std::size_t count) const {
    std::vector<Sample> out;
    if (channel_idx >= channels_.size() || count == 0) {
        return out;
    }
    auto &ch = *channels_[channel_idx];
    auto const wc = ch.write_count.load(std::memory_order_acquire);
    if (wc == 0) {
        return out;
    }
    // Cap count by what's actually been pushed AND by ring capacity.
    auto const available = static_cast<std::size_t>(std::min<std::uint64_t>(
        wc, static_cast<std::uint64_t>(capacity_)));
    auto const take = std::min(count, available);
    out.reserve(take);
    // The most-recent sample is at index (wc-1) & mask. Walk back
    // `take` slots to find the start, then emit in chronological
    // (oldest-first) order.
    auto const start_count = wc - take;
    for (std::size_t i = 0; i < take; ++i) {
        auto const slot = (start_count + i) & mask_;
        out.push_back(ch.ring[slot]);
    }
    return out;
}

std::optional<LiveBuffer::Sample> LiveBuffer::latest(std::size_t channel_idx) const noexcept {
    if (channel_idx >= channels_.size()) {
        return std::nullopt;
    }
    auto &ch = *channels_[channel_idx];
    auto const wc = ch.write_count.load(std::memory_order_acquire);
    if (wc == 0) {
        return std::nullopt;
    }
    return ch.ring[(wc - 1) & mask_];
}

std::uint64_t LiveBuffer::pushed_count(std::size_t channel_idx) const noexcept {
    if (channel_idx >= channels_.size()) {
        return 0;
    }
    return channels_[channel_idx]->write_count.load(std::memory_order_acquire);
}

} // namespace st::log
