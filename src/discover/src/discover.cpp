// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/discover.hpp"

#include "st/can.hpp"
#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace st::discover {

namespace {

constexpr std::uint64_t key_of(can::BusId bus, std::uint32_t can_id) noexcept {
    return (static_cast<std::uint64_t>(bus) << 32) | can_id;
}

// Most-common value in `vs`; ties broken by first-seen. Caller
// guarantees `vs` is non-empty.
std::uint8_t mode_of(std::vector<std::uint8_t> const &vs) noexcept {
    std::array<std::size_t, 256> counts{};
    std::array<std::size_t, 256> first_seen{};
    constexpr std::size_t kSentinel = static_cast<std::size_t>(-1);
    first_seen.fill(kSentinel);

    for (std::size_t i = 0; i < vs.size(); ++i) {
        auto const v = vs[i];
        if (first_seen[v] == kSentinel)
            first_seen[v] = i;
        counts[v]++;
    }

    std::size_t best = 0;
    std::size_t best_count = 0;
    std::size_t best_first = kSentinel;
    for (std::size_t v = 0; v < 256; ++v) {
        if (counts[v] == 0)
            continue;
        if (counts[v] > best_count || (counts[v] == best_count && first_seen[v] < best_first)) {
            best = v;
            best_count = counts[v];
            best_first = first_seen[v];
        }
    }
    return static_cast<std::uint8_t>(best);
}

// Collect the distinct values in `vs`, preserving first-seen order.
std::vector<std::uint8_t> distinct_values(std::vector<std::uint8_t> const &vs) noexcept {
    std::array<bool, 256> seen{};
    std::vector<std::uint8_t> out;
    out.reserve(8);
    for (auto const v : vs) {
        if (!seen[v]) {
            seen[v] = true;
            out.push_back(v);
        }
    }
    return out;
}

// Decide stable / cyclic / noisy for one byte column.
ByteClass classify_byte(std::vector<std::uint8_t> const &vs,
                        BaselineModel::Options const &opts) noexcept {
    if (vs.size() < 2) {
        return ByteClass::Stable;
    }
    auto const distincts = distinct_values(vs);
    if (distincts.size() <= opts.stable_max_distinct) {
        return ByteClass::Stable;
    }
    if (vs.size() < opts.min_samples_for_noisy) {
        // Not enough evidence to call this noisy — keep it stable with
        // whatever values we saw. Change detection will then flag any
        // future value not in that set.
        return ByteClass::Stable;
    }

    // Cyclic detection: count deltas, look for a dominant modal delta.
    // Deltas use signed int8 arithmetic (range -255..+255), which keeps
    // mod-256 counter wraps as a distinct delta from the +1 step. Most
    // counters in the wild have one dominant non-zero step delta.
    std::unordered_map<int, std::size_t> delta_counts;
    for (std::size_t i = 1; i < vs.size(); ++i) {
        int const d = static_cast<int>(vs[i]) - static_cast<int>(vs[i - 1]);
        if (d == 0)
            continue; // duplicate frame, ignore
        ++delta_counts[d];
    }
    if (delta_counts.empty()) {
        // All-equal? Already caught above. Guard anyway.
        return ByteClass::Stable;
    }
    int modal_delta = 0;
    std::size_t modal_count = 0;
    for (auto const &[d, c] : delta_counts) {
        if (c > modal_count) {
            modal_count = c;
            modal_delta = d;
        }
    }
    auto const considered = vs.size() - 1;
    if (considered > 0 && (static_cast<double>(modal_count) / static_cast<double>(considered)) >=
                              opts.cyclic_modal_delta_fraction) {
        // Modal non-zero delta dominates → cyclic. (The non-modal
        // deltas are typically the wrap-around step.)
        (void)modal_delta;
        return ByteClass::Cyclic;
    }

    return ByteClass::Noisy;
}

} // namespace

// =====================================================================
// BaselineModel
// =====================================================================

BaselineModel::BaselineModel() : opts_{} {}
BaselineModel::BaselineModel(Options opts) : opts_{opts} {}

void BaselineModel::observe(can::Frame const &f) {
    if (finalized_)
        return;
    auto const k = key_of(f.bus, f.id);
    auto it = builders_.find(k);
    if (it == builders_.end()) {
        Builder b;
        b.bus = f.bus;
        b.extended = f.extended;
        b.dlc = f.dlc;
        it = builders_.emplace(k, std::move(b)).first;
    }
    auto &b = it->second;
    // If we see different DLCs for the same id, keep the largest so we
    // observe every byte position the id might use.
    b.dlc = std::max(b.dlc, f.dlc);
    b.frames++;
    for (std::uint8_t i = 0; i < f.dlc; ++i) {
        b.samples[i].push_back(f.data[i]);
    }
}

void BaselineModel::finalize(std::chrono::nanoseconds duration) {
    if (finalized_)
        return;
    finalized_ = true;

    auto const dur_ns = static_cast<double>(duration.count());
    entries_.reserve(builders_.size());
    for (auto &[key, b] : builders_) {
        IdBaseline ib;
        ib.bus = b.bus;
        ib.can_id = static_cast<std::uint32_t>(key & 0xFFFF'FFFFU);
        ib.extended = b.extended;
        ib.dlc = b.dlc;
        ib.samples = b.frames;
        ib.freq_hz = dur_ns > 0.0 ? static_cast<double>(b.frames) * 1e9 / dur_ns : 0.0;
        for (std::uint8_t i = 0; i < b.dlc && i < 8; ++i) {
            auto const cls = classify_byte(b.samples[i], opts_);
            ib.classes[i] = cls;
            if (!b.samples[i].empty()) {
                ib.mode_value[i] = mode_of(b.samples[i]);
                if (cls == ByteClass::Stable) {
                    ib.stable_values[i] = distinct_values(b.samples[i]);
                }
            }
        }
        entries_.push_back(std::move(ib));
    }
    // Deterministic order — easier on tests and humans.
    std::sort(entries_.begin(), entries_.end(), [](IdBaseline const &a, IdBaseline const &c) {
        if (a.bus != c.bus) {
            return static_cast<std::uint8_t>(a.bus) < static_cast<std::uint8_t>(c.bus);
        }
        return a.can_id < c.can_id;
    });
    builders_.clear();
}

IdBaseline const *BaselineModel::find(can::BusId bus, std::uint32_t can_id) const noexcept {
    for (auto const &e : entries_) {
        if (e.bus == bus && e.can_id == can_id)
            return &e;
    }
    return nullptr;
}

// =====================================================================
// ChangeDetector
// =====================================================================

ChangeDetector::ChangeDetector(BaselineModel const &baseline) : baseline_{&baseline}, opts_{} {}

ChangeDetector::ChangeDetector(BaselineModel const &baseline, Options opts)
    : baseline_{&baseline}, opts_{opts} {}

namespace {

bool value_in_stable_set(std::vector<std::uint8_t> const &allowed, std::uint8_t v) noexcept {
    for (auto const a : allowed) {
        if (a == v)
            return true;
    }
    return false;
}

} // namespace

std::optional<DiscoveryEvent> ChangeDetector::observe(can::Frame const &f) {
    auto const *baseline = baseline_->find(f.bus, f.id);

    if (baseline == nullptr) {
        // Unknown id. Emit one NewId event the first time we see it
        // during watch; subsequent appearances are silent.
        auto const k = key_of(f.bus, f.id);
        if (reported_new_ids_.contains(k)) {
            return std::nullopt;
        }
        if (f.timestamp_ns < last_event_ns_ + opts_.debounce_ns) {
            return std::nullopt;
        }
        reported_new_ids_.insert(k);
        DiscoveryEvent e;
        e.kind = DiscoveryEvent::Kind::NewId;
        e.timestamp_ns = f.timestamp_ns;
        e.bus = f.bus;
        e.can_id = f.id;
        e.extended = f.extended;
        for (std::uint8_t i = 0; i < f.dlc && i < 8; ++i) {
            e.after[i] = f.data[i];
        }
        last_event_ns_ = f.timestamp_ns;
        ++events_count_;
        return e;
    }

    // Walk bytes and collect deviations.
    std::vector<std::uint8_t> changed;
    std::array<std::uint8_t, 8> before{};
    std::array<std::uint8_t, 8> after{};
    auto const dlc = std::min<std::uint8_t>(f.dlc, baseline->dlc);
    for (std::uint8_t i = 0; i < dlc; ++i) {
        auto const cls = baseline->classes[i];
        auto const value = f.data[i];
        before[i] = baseline->mode_value[i];
        after[i] = value;
        if (cls == ByteClass::Stable) {
            if (!value_in_stable_set(baseline->stable_values[i], value)) {
                changed.push_back(i);
            }
        } else if (cls == ByteClass::Noisy && opts_.flag_noisy_deviations) {
            // Mode-deviation heuristic — for now, any value != mode counts.
            if (value != baseline->mode_value[i]) {
                changed.push_back(i);
            }
        }
        // Cyclic bytes are always ignored, per docs/14.
    }
    if (changed.empty())
        return std::nullopt;
    if (f.timestamp_ns < last_event_ns_ + opts_.debounce_ns) {
        return std::nullopt;
    }

    DiscoveryEvent e;
    e.kind = DiscoveryEvent::Kind::Change;
    e.timestamp_ns = f.timestamp_ns;
    e.bus = f.bus;
    e.can_id = f.id;
    e.extended = f.extended;
    e.changed_byte_indices = std::move(changed);
    e.before = before;
    e.after = after;
    last_event_ns_ = f.timestamp_ns;
    ++events_count_;
    return e;
}

// =====================================================================
// Convenience entry points
// =====================================================================

BaselineModel build_baseline(std::span<can::Frame const> frames, BaselineModel::Options opts) {
    BaselineModel m{opts};
    for (auto const &f : frames)
        m.observe(f);

    std::int64_t first_ts = 0;
    std::int64_t last_ts = 0;
    if (!frames.empty()) {
        first_ts = frames.front().timestamp_ns;
        last_ts = frames.back().timestamp_ns;
    }
    auto const dur = std::chrono::nanoseconds{last_ts - first_ts};
    m.finalize(dur);
    return m;
}

std::vector<DiscoveryEvent> detect_changes(BaselineModel const &baseline,
                                           std::span<can::Frame const> watch_frames,
                                           ChangeDetector::Options opts) {
    ChangeDetector det{baseline, opts};
    std::vector<DiscoveryEvent> events;
    events.reserve(watch_frames.size() / 4 + 1);
    for (auto const &f : watch_frames) {
        if (auto e = det.observe(f); e.has_value()) {
            events.push_back(std::move(*e));
        }
    }
    return events;
}

// =====================================================================
// .cdb serialization
// =====================================================================

namespace {

constexpr std::string_view kKindNewId = "new_id";
constexpr std::string_view kKindChange = "change";

toml::array bytes_to_toml(std::span<std::uint8_t const> bytes) {
    toml::array arr;
    for (auto const b : bytes) {
        arr.push_back(static_cast<std::int64_t>(b));
    }
    return arr;
}

// Read an array of integers in [0, 255] into a vector<uint8_t>. Returns
// false (without modifying out) when any element is out of range or not
// an integer.
bool toml_to_byte_vector(toml::array const *arr, std::vector<std::uint8_t> &out) {
    if (arr == nullptr)
        return false;
    std::vector<std::uint8_t> tmp;
    tmp.reserve(arr->size());
    for (auto const &el : *arr) {
        auto const opt = el.value<std::int64_t>();
        if (!opt.has_value() || *opt < 0 || *opt > 255)
            return false;
        tmp.push_back(static_cast<std::uint8_t>(*opt));
    }
    out = std::move(tmp);
    return true;
}

void emit_baseline_entry(toml::array &dst, IdBaseline const &ib) {
    toml::table t;
    t.insert_or_assign("can_id", static_cast<std::int64_t>(ib.can_id));
    t.insert_or_assign("extended", ib.extended);
    t.insert_or_assign("bus", static_cast<std::int64_t>(static_cast<std::uint8_t>(ib.bus)));
    t.insert_or_assign("dlc", static_cast<std::int64_t>(ib.dlc));
    t.insert_or_assign("samples", static_cast<std::int64_t>(ib.samples));
    t.insert_or_assign("freq_hz", ib.freq_hz);

    // Per-byte classification — three index lists.
    toml::array stable_bytes, cyclic_bytes, noisy_bytes;
    for (std::size_t i = 0; i < ib.dlc; ++i) {
        switch (ib.classes[i]) {
        case ByteClass::Stable:
            stable_bytes.push_back(static_cast<std::int64_t>(i));
            break;
        case ByteClass::Cyclic:
            cyclic_bytes.push_back(static_cast<std::int64_t>(i));
            break;
        case ByteClass::Noisy:
            noisy_bytes.push_back(static_cast<std::int64_t>(i));
            break;
        }
    }
    t.insert_or_assign("stable_bytes", std::move(stable_bytes));
    t.insert_or_assign("cyclic_bytes", std::move(cyclic_bytes));
    t.insert_or_assign("noisy_bytes", std::move(noisy_bytes));

    // Per-stable-byte: the allowed-value set. Emitted as an array of
    // arrays, parallel to `stable_bytes`. Cyclic/Noisy positions have
    // no entry here.
    toml::array stable_values;
    for (std::size_t i = 0; i < ib.dlc; ++i) {
        if (ib.classes[i] != ByteClass::Stable)
            continue;
        toml::array vs;
        for (auto const v : ib.stable_values[i]) {
            vs.push_back(static_cast<std::int64_t>(v));
        }
        stable_values.push_back(std::move(vs));
    }
    t.insert_or_assign("stable_values", std::move(stable_values));

    // Mode value per byte position (length = dlc).
    toml::array mode_values;
    for (std::size_t i = 0; i < ib.dlc; ++i) {
        mode_values.push_back(static_cast<std::int64_t>(ib.mode_value[i]));
    }
    t.insert_or_assign("mode_values", std::move(mode_values));

    dst.push_back(std::move(t));
}

void emit_event(toml::array &dst, DiscoveryEvent const &e) {
    toml::table t;
    t.insert_or_assign(
        "kind", std::string{e.kind == DiscoveryEvent::Kind::NewId ? kKindNewId : kKindChange});
    t.insert_or_assign("timestamp_ns", e.timestamp_ns);
    t.insert_or_assign("can_id", static_cast<std::int64_t>(e.can_id));
    t.insert_or_assign("extended", e.extended);
    t.insert_or_assign("bus", static_cast<std::int64_t>(static_cast<std::uint8_t>(e.bus)));
    if (!e.description.empty()) {
        t.insert_or_assign("description", e.description);
    }

    if (e.kind == DiscoveryEvent::Kind::NewId) {
        // For NewId we don't know DLC after the fact; emit all 8 bytes
        // of `after` (the first frame's data). The reader treats this
        // as the full payload.
        t.insert_or_assign("after", bytes_to_toml(e.after));
    } else {
        // Change: bytes / before / after as parallel arrays of length N.
        toml::array bytes_arr, before_arr, after_arr;
        for (auto const i : e.changed_byte_indices) {
            bytes_arr.push_back(static_cast<std::int64_t>(i));
            before_arr.push_back(static_cast<std::int64_t>(e.before[i]));
            after_arr.push_back(static_cast<std::int64_t>(e.after[i]));
        }
        t.insert_or_assign("bytes", std::move(bytes_arr));
        t.insert_or_assign("before", std::move(before_arr));
        t.insert_or_assign("after", std::move(after_arr));
    }
    dst.push_back(std::move(t));
}

Result<IdBaseline> parse_baseline_entry(toml::table const &t) {
    IdBaseline ib;
    if (auto const v = t["can_id"].value<std::int64_t>(); v.has_value()) {
        ib.can_id = static_cast<std::uint32_t>(*v);
    } else {
        return failure(ErrorCode::ParseError, "cdb: baseline.id missing can_id");
    }
    ib.extended = t["extended"].value_or(false);
    auto const bus_idx = t["bus"].value_or<std::int64_t>(0);
    ib.bus = static_cast<can::BusId>(bus_idx);
    ib.dlc = static_cast<std::uint8_t>(t["dlc"].value_or<std::int64_t>(0));
    ib.samples = static_cast<std::size_t>(t["samples"].value_or<std::int64_t>(0));
    ib.freq_hz = t["freq_hz"].value_or<double>(0.0);

    // Within dlc, default to Noisy so an unlisted byte index is at least
    // classified (a well-formed file lists every position exactly once).
    // Past dlc the bytes are meaningless; leave them at the value-init
    // ByteClass::Stable produced by std::array<ByteClass, 8>{} so the
    // on-disk shape matches a freshly built BaselineModel and round-trips.
    for (std::size_t i = 0; i < ib.dlc && i < 8; ++i) {
        ib.classes[i] = ByteClass::Noisy;
    }

    auto const tag_bytes = [&](char const *key, ByteClass cls) -> Status {
        auto const *arr = t[key].as_array();
        if (arr == nullptr)
            return ok();
        for (auto const &el : *arr) {
            auto const opt = el.value<std::int64_t>();
            if (!opt.has_value() || *opt < 0 || *opt >= 8) {
                return failure(ErrorCode::ParseError, std::string{"cdb: bad byte index in "} + key);
            }
            ib.classes[static_cast<std::size_t>(*opt)] = cls;
        }
        return ok();
    };
    if (auto r = tag_bytes("stable_bytes", ByteClass::Stable); !r.has_value())
        return failure(r.error());
    if (auto r = tag_bytes("cyclic_bytes", ByteClass::Cyclic); !r.has_value())
        return failure(r.error());
    if (auto r = tag_bytes("noisy_bytes", ByteClass::Noisy); !r.has_value())
        return failure(r.error());

    // stable_values: array of inner arrays, parallel to stable_bytes.
    if (auto const *sv_outer = t["stable_values"].as_array(); sv_outer != nullptr) {
        std::size_t pos = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            if (ib.classes[i] != ByteClass::Stable)
                continue;
            if (pos >= sv_outer->size())
                break;
            std::vector<std::uint8_t> vs;
            if (!toml_to_byte_vector((*sv_outer)[pos].as_array(), vs)) {
                return failure(ErrorCode::ParseError,
                               "cdb: stable_values: bad byte at position " + std::to_string(pos));
            }
            ib.stable_values[i] = std::move(vs);
            ++pos;
        }
    }

    if (auto const *mv = t["mode_values"].as_array(); mv != nullptr) {
        std::size_t i = 0;
        for (auto const &el : *mv) {
            if (i >= 8)
                break;
            auto const opt = el.value<std::int64_t>();
            if (!opt.has_value() || *opt < 0 || *opt > 255) {
                std::string msg{"cdb: bad mode_values entry at index "};
                msg += std::to_string(i);
                if (opt.has_value()) {
                    msg += " value=";
                    msg += std::to_string(*opt);
                    msg += " (must be 0..255)";
                } else {
                    msg += " (not an integer)";
                }
                return failure(ErrorCode::ParseError, std::move(msg));
            }
            ib.mode_value[i++] = static_cast<std::uint8_t>(*opt);
        }
    }
    return ib;
}

Result<DiscoveryEvent> parse_event_entry(toml::table const &t) {
    DiscoveryEvent e;
    auto const kind_opt = t["kind"].value<std::string>();
    if (!kind_opt.has_value()) {
        return failure(ErrorCode::ParseError, "cdb: event missing kind");
    }
    if (*kind_opt == kKindNewId) {
        e.kind = DiscoveryEvent::Kind::NewId;
    } else if (*kind_opt == kKindChange) {
        e.kind = DiscoveryEvent::Kind::Change;
    } else {
        return failure(ErrorCode::ParseError, "cdb: unknown event kind '" + *kind_opt + "'");
    }

    e.timestamp_ns = t["timestamp_ns"].value_or<std::int64_t>(0);
    if (auto const v = t["can_id"].value<std::int64_t>(); v.has_value()) {
        e.can_id = static_cast<std::uint32_t>(*v);
    }
    e.extended = t["extended"].value_or(false);
    e.bus = static_cast<can::BusId>(t["bus"].value_or<std::int64_t>(0));
    if (auto const d = t["description"].value<std::string>(); d.has_value()) {
        e.description = *d;
    }

    auto const id_tag = [&]() {
        return " (can_id=0x" + [&]() {
            char buf[12];
            std::snprintf(buf, sizeof(buf), "%X", e.can_id);
            return std::string{buf};
        }() + ")";
    };

    if (e.kind == DiscoveryEvent::Kind::NewId) {
        if (auto const *after = t["after"].as_array(); after != nullptr) {
            std::size_t i = 0;
            for (auto const &el : *after) {
                if (i >= 8)
                    break;
                auto const opt = el.value<std::int64_t>();
                if (!opt.has_value() || *opt < 0 || *opt > 255) {
                    std::string msg{"cdb: bad after byte at index "};
                    msg += std::to_string(i);
                    msg += " in new_id event";
                    msg += id_tag();
                    if (opt.has_value()) {
                        msg += " value=" + std::to_string(*opt) + " (must be 0..255)";
                    } else {
                        msg += " (not an integer)";
                    }
                    return failure(ErrorCode::ParseError, std::move(msg));
                }
                e.after[i++] = static_cast<std::uint8_t>(*opt);
            }
        }
    } else {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
        bool const ok_bytes = toml_to_byte_vector(t["bytes"].as_array(), bytes);
        bool const ok_before = toml_to_byte_vector(t["before"].as_array(), before);
        bool const ok_after = toml_to_byte_vector(t["after"].as_array(), after);
        if (!ok_bytes || !ok_before || !ok_after) {
            std::string msg{"cdb: change event"};
            msg += id_tag();
            msg += " has malformed";
            bool first = true;
            auto add = [&](char const *name) {
                msg += first ? " " : "/";
                msg += name;
                first = false;
            };
            if (!ok_bytes)
                add("bytes");
            if (!ok_before)
                add("before");
            if (!ok_after)
                add("after");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        if (bytes.size() != before.size() || bytes.size() != after.size()) {
            std::string msg{"cdb: change event"};
            msg += id_tag();
            msg += " bytes/before/after length mismatch (bytes=";
            msg += std::to_string(bytes.size());
            msg += " before=";
            msg += std::to_string(before.size());
            msg += " after=";
            msg += std::to_string(after.size());
            msg += ")";
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        for (std::size_t k = 0; k < bytes.size(); ++k) {
            auto const idx = bytes[k];
            if (idx >= 8) {
                std::string msg{"cdb: change event"};
                msg += id_tag();
                msg += " byte index ";
                msg += std::to_string(static_cast<int>(idx));
                msg += " out of range (must be 0..7)";
                return failure(ErrorCode::ParseError, std::move(msg));
            }
            e.changed_byte_indices.push_back(idx);
            e.before[idx] = before[k];
            e.after[idx] = after[k];
        }
    }
    return e;
}

} // namespace

Result<std::string> write_cdb_string(Bundle const &b) {
    toml::table root;
    root.insert_or_assign("schema_version", static_cast<std::int64_t>(b.schema_version));
    if (!b.captured_at.empty()) {
        root.insert_or_assign("captured_at", b.captured_at);
    }
    if (!b.bus_label.empty()) {
        root.insert_or_assign("bus", b.bus_label);
    }
    root.insert_or_assign("baseline_ns", b.baseline_ns);

    toml::array baseline_array;
    for (auto const &ib : b.baseline_entries) {
        emit_baseline_entry(baseline_array, ib);
    }
    toml::table baseline_holder;
    baseline_holder.insert_or_assign("id", std::move(baseline_array));
    root.insert_or_assign("baseline", std::move(baseline_holder));

    toml::array events_array;
    for (auto const &e : b.events) {
        emit_event(events_array, e);
    }
    root.insert_or_assign("event", std::move(events_array));

    std::ostringstream ss;
    ss << root;
    return ss.str();
}

Result<Bundle> parse_cdb_string(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError,
                       std::string{"cdb: TOML parse error: "} + e.description().data());
    }

    Bundle out;
    out.schema_version = static_cast<int>(root["schema_version"].value_or<std::int64_t>(1));
    out.captured_at = root["captured_at"].value_or<std::string>("");
    out.bus_label = root["bus"].value_or<std::string>("");
    out.baseline_ns = root["baseline_ns"].value_or<std::int64_t>(0);

    if (auto const *baseline = root["baseline"]["id"].as_array(); baseline != nullptr) {
        out.baseline_entries.reserve(baseline->size());
        for (auto const &el : *baseline) {
            auto const *tbl = el.as_table();
            if (tbl == nullptr) {
                return failure(ErrorCode::ParseError, "cdb: baseline.id element is not a table");
            }
            auto ib = parse_baseline_entry(*tbl);
            if (!ib.has_value())
                return failure(ib.error());
            out.baseline_entries.push_back(std::move(*ib));
        }
    }

    if (auto const *events = root["event"].as_array(); events != nullptr) {
        out.events.reserve(events->size());
        for (auto const &el : *events) {
            auto const *tbl = el.as_table();
            if (tbl == nullptr) {
                return failure(ErrorCode::ParseError, "cdb: event element is not a table");
            }
            auto e = parse_event_entry(*tbl);
            if (!e.has_value())
                return failure(e.error());
            out.events.push_back(std::move(*e));
        }
    }

    return out;
}

Result<Bundle> read_cdb(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound, "cdb: cannot open: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_cdb_string(std::move(ss).str());
}

Status write_cdb(std::filesystem::path const &path, Bundle const &b) {
    auto text = write_cdb_string(b);
    if (!text.has_value())
        return failure(text.error());
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure, "cdb: cannot open for write: " + path.string());
    }
    out.write(text->data(), static_cast<std::streamsize>(text->size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "cdb: write failed: " + path.string());
    }
    return ok();
}

} // namespace st::discover
