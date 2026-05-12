// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

namespace st::flash {

// ---------------------------------------------------------------------
// Public-API helpers
// ---------------------------------------------------------------------

bool FlashReport::all_sectors_completed() const noexcept {
    if (sectors.empty()) return false;
    for (auto const &s : sectors) {
        if (!s.erased || !s.downloaded || !s.transferred || !s.exited
            || !s.check_deps_passed) {
            return false;
        }
    }
    return true;
}

bool FlashReport::all_sectors_verified() const noexcept {
    for (auto const &s : sectors) {
        if (!s.verified) return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// read_full_rom — chunked ReadMemoryByAddress loop
// ---------------------------------------------------------------------

Result<std::vector<std::uint8_t>> Flasher::read_full_rom(
    std::uint32_t             base_address,
    std::uint32_t             total_length,
    std::uint32_t             max_chunk_size,
    std::chrono::milliseconds per_chunk_timeout) {
    if (total_length == 0) {
        return std::vector<std::uint8_t>{};
    }
    if (max_chunk_size == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "flash: read_full_rom max_chunk_size must be > 0");
    }
    std::vector<std::uint8_t> out;
    out.reserve(total_length);
    std::uint32_t cursor    = base_address;
    std::uint32_t remaining = total_length;
    while (remaining > 0) {
        std::uint32_t const this_chunk =
            remaining < max_chunk_size ? remaining : max_chunk_size;
        auto chunk = client_.read_memory_by_address(cursor, this_chunk,
                                                     per_chunk_timeout);
        if (!chunk.has_value()) {
            return failure(chunk.error().code(),
                           "flash: read_full_rom failed at 0x"
                           + std::to_string(cursor) + ": "
                           + std::string{chunk.error().message()});
        }
        if (chunk->size() != this_chunk) {
            return failure(ErrorCode::UnexpectedEof,
                           "flash: short read at 0x" + std::to_string(cursor)
                           + " (expected " + std::to_string(this_chunk)
                           + ", got " + std::to_string(chunk->size()) + ")");
        }
        out.insert(out.end(), chunk->begin(), chunk->end());
        cursor    += this_chunk;
        remaining -= this_chunk;
    }
    return out;
}

// ---------------------------------------------------------------------
// compute_delta — sector-aligned byte-diff
// ---------------------------------------------------------------------

std::vector<Sector> Flasher::compute_delta(
    std::span<std::uint8_t const> current,
    std::span<std::uint8_t const> target,
    std::uint32_t                 sector_size,
    std::uint32_t                 base_address) {
    std::vector<Sector> out;
    if (sector_size == 0) return out;
    auto const n = std::min(current.size(), target.size());
    for (std::size_t off = 0; off < n; off += sector_size) {
        std::size_t const end = std::min(off + sector_size, n);
        bool              differs = false;
        for (std::size_t i = off; i < end; ++i) {
            if (current[i] != target[i]) {
                differs = true;
                break;
            }
        }
        if (differs) {
            out.push_back(Sector{
                base_address + static_cast<std::uint32_t>(off),
                static_cast<std::uint32_t>(end - off),
            });
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// execute — full flash orchestration
// ---------------------------------------------------------------------

namespace {

// ISO 14229-1 Annex F eraseMemory routine option record:
//   [aLFI] [memoryAddress bytes] [memorySize bytes]
// where aLFI's high nibble = number of memorySize bytes, low nibble =
// number of memoryAddress bytes. SubuwuTuner v1 targets 32-bit address
// space and 32-bit sizes → aLFI = 0x44.
std::vector<std::uint8_t> build_erase_option_record(
    std::uint32_t addr, std::uint32_t size) {
    std::vector<std::uint8_t> opt;
    opt.reserve(9);
    opt.push_back(0x44);
    opt.push_back(static_cast<std::uint8_t>((addr >> 24) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((addr >> 16) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((addr >> 8) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>(addr & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    return opt;
}

// Pick a per-TransferData payload size that respects both the ECU's
// reported maxNumberOfBlockLength and the caller's hint. The reported
// value counts the SID byte and the block-sequence-counter byte, so
// the payload is `reported - 2`.
std::uint32_t choose_block_payload(std::uint32_t reported_max,
                                    std::uint32_t hint) {
    std::uint32_t const safe_reported =
        reported_max > 2 ? reported_max - 2 : 0;
    if (hint == 0)                         return safe_reported;
    if (safe_reported == 0)                return hint;
    return safe_reported < hint ? safe_reported : hint;
}

} // namespace

Result<FlashReport> Flasher::execute(FlashPlan const &plan) {
    FlashReport report{};
    report.sectors.reserve(plan.writes.size());

    // Validate the plan before touching the bus.
    for (auto const &w : plan.writes) {
        if (w.data.size() != w.sector.length) {
            return failure(ErrorCode::InvalidArgument,
                           "flash: SectorWrite.data.size() ("
                           + std::to_string(w.data.size())
                           + ") != sector.length ("
                           + std::to_string(w.sector.length) + ")");
        }
        if (w.sector.length == 0) {
            return failure(ErrorCode::InvalidArgument,
                           "flash: SectorWrite.sector.length must be > 0");
        }
    }

    // 1. Enter programming session.
    if (auto s = client_.diagnostic_session_control(plan.session);
        !s.has_value()) {
        return failure(s.error().code(),
                       "flash: diagnostic_session_control failed: "
                       + std::string{s.error().message()});
    }
    report.entered_session = true;

    // 2. Optionally silence non-diagnostic traffic.
    if (plan.silence_bus) {
        if (auto s = client_.communication_control(
                ecu::uds::kCcDisableRxAndTx,
                ecu::uds::kCtNormalAndNetworkManagement);
            !s.has_value()) {
            return failure(s.error().code(),
                           "flash: communication_control(off) failed: "
                           + std::string{s.error().message()});
        }
        report.silenced_bus = true;
    }

    // 3. Dry-run: skip every per-sector operation. Record one outcome
    // per planned sector with everything false so the caller can see
    // what would have been done.
    if (plan.dry_run) {
        for (auto const &w : plan.writes) {
            report.sectors.push_back(SectorOutcome{w.sector});
        }
    } else {
        // 3'. Full execution: erase + download + transfer + exit +
        // check_deps + optional verify, per sector.
        for (auto const &w : plan.writes) {
            SectorOutcome outcome{};
            outcome.sector = w.sector;

            // 3a. eraseMemory routine.
            auto erase = client_.routine_control(
                ecu::uds::kRcStart, ecu::uds::kRidEraseMemory,
                build_erase_option_record(w.sector.address, w.sector.length));
            if (!erase.has_value()) {
                report.sectors.push_back(outcome);
                return failure(erase.error().code(),
                               "flash: eraseMemory failed at 0x"
                               + std::to_string(w.sector.address) + ": "
                               + std::string{erase.error().message()});
            }
            outcome.erased = true;

            // 3b. RequestDownload.
            auto rdl = client_.request_download(
                plan.data_format, w.sector.address, w.sector.length);
            if (!rdl.has_value()) {
                report.sectors.push_back(outcome);
                return failure(rdl.error().code(),
                               "flash: request_download failed at 0x"
                               + std::to_string(w.sector.address) + ": "
                               + std::string{rdl.error().message()});
            }
            outcome.downloaded = true;
            std::uint32_t const block_payload =
                choose_block_payload(*rdl, plan.block_size_hint);
            if (block_payload == 0) {
                report.sectors.push_back(outcome);
                return failure(ErrorCode::EcuRejected,
                               "flash: ECU reported unusable "
                               "maxNumberOfBlockLength="
                               + std::to_string(*rdl));
            }

            // 3c. TransferData blocks.
            std::uint8_t  counter = 1;
            std::size_t   offset  = 0;
            while (offset < w.data.size()) {
                std::size_t const remaining = w.data.size() - offset;
                std::size_t const this_block =
                    remaining < block_payload ? remaining : block_payload;
                std::span<std::uint8_t const> chunk{w.data.data() + offset,
                                                     this_block};
                if (auto s = client_.transfer_data(counter, chunk);
                    !s.has_value()) {
                    report.bytes_transferred += offset;
                    report.sectors.push_back(outcome);
                    return failure(s.error().code(),
                                   "flash: transfer_data counter="
                                   + std::to_string(counter)
                                   + " failed: "
                                   + std::string{s.error().message()});
                }
                offset  += this_block;
                counter  = static_cast<std::uint8_t>(counter + 1U);
                // counter wraps from 0xFF to 0x00 per ISO 14229.
            }
            report.bytes_transferred += offset;
            outcome.transferred = true;

            // 3d. RequestTransferExit.
            if (auto s = client_.request_transfer_exit(); !s.has_value()) {
                report.sectors.push_back(outcome);
                return failure(s.error().code(),
                               "flash: request_transfer_exit failed at 0x"
                               + std::to_string(w.sector.address) + ": "
                               + std::string{s.error().message()});
            }
            outcome.exited = true;

            // 3e. checkProgrammingDependencies routine.
            auto check = client_.routine_control(
                ecu::uds::kRcStart,
                ecu::uds::kRidCheckProgrammingDependencies);
            if (!check.has_value()) {
                report.sectors.push_back(outcome);
                return failure(check.error().code(),
                               "flash: checkProgrammingDependencies "
                               "failed at 0x"
                               + std::to_string(w.sector.address) + ": "
                               + std::string{check.error().message()});
            }
            outcome.check_deps_passed = true;

            // 3f. Optional verify pass.
            if (plan.verify_after_write) {
                auto readback = read_full_rom(w.sector.address,
                                              w.sector.length,
                                              plan.verify_chunk_size);
                if (!readback.has_value()) {
                    outcome.verified = false;
                    report.sectors.push_back(outcome);
                    return failure(readback.error().code(),
                                   "flash: verify read-back failed: "
                                   + std::string{readback.error().message()});
                }
                outcome.verified = (readback->size() == w.data.size()
                                    && std::equal(readback->begin(),
                                                  readback->end(),
                                                  w.data.begin()));
            }

            report.sectors.push_back(outcome);
        }
    }

    // 4. Restore the bus. Best-effort: a failure here is reported but
    // not fatal because the flash itself has already completed.
    if (plan.silence_bus) {
        if (auto s = client_.communication_control(
                ecu::uds::kCcEnableRxAndTx,
                ecu::uds::kCtNormalAndNetworkManagement);
            s.has_value()) {
            report.restored_bus = true;
        }
    }

    return report;
}

// ---------------------------------------------------------------------
// FlashPlan TOML persistence
// ---------------------------------------------------------------------

namespace {

// Parse a hex string ("DE AD BE EF" or "DEADBEEF" or "0xDE 0xAD ...") into
// raw bytes. Whitespace and optional per-byte "0x" prefix are tolerated.
// Returns nullopt on any unparseable character.
Result<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view s) {
    std::vector<std::uint8_t> out;
    std::size_t               i = 0;
    while (i < s.size()) {
        // Skip whitespace.
        while (i < s.size()
               && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (i >= s.size()) break;
        // Optional "0x" / "0X" prefix per byte.
        if (i + 1 < s.size() && s[i] == '0'
            && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i += 2;
        }
        if (i + 1 >= s.size()) {
            return failure(ErrorCode::ParseError,
                           "flash plan: odd hex digit at position "
                           + std::to_string(i));
        }
        unsigned const   first  = static_cast<unsigned char>(s[i]);
        unsigned const   second = static_cast<unsigned char>(s[i + 1]);
        auto const       is_hex = [](unsigned c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                   || (c >= 'A' && c <= 'F');
        };
        if (!is_hex(first) || !is_hex(second)) {
            return failure(ErrorCode::ParseError,
                           "flash plan: bad hex byte near position "
                           + std::to_string(i));
        }
        unsigned    value = 0;
        char const  pair[2]{static_cast<char>(first),
                             static_cast<char>(second)};
        auto const  res =
            std::from_chars(pair, pair + 2, value, 16);
        if (res.ec != std::errc{} || res.ptr != pair + 2) {
            return failure(ErrorCode::ParseError,
                           "flash plan: bad hex byte near position "
                           + std::to_string(i));
        }
        out.push_back(static_cast<std::uint8_t>(value));
        i += 2;
    }
    return out;
}

// Emit bytes as "DE AD BE EF" — uppercase, space-separated, no prefix,
// chunked into 32 bytes per line for readability on long payloads.
std::string format_hex_bytes(std::span<std::uint8_t const> bytes) {
    if (bytes.empty()) return std::string{};
    constexpr char const *digits = "0123456789ABCDEF";
    constexpr std::size_t per_line = 32;
    std::string           out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            out.push_back((i % per_line == 0) ? '\n' : ' ');
        }
        out.push_back(digits[(bytes[i] >> 4) & 0xFU]);
        out.push_back(digits[bytes[i] & 0xFU]);
    }
    return out;
}

} // namespace

Result<FlashPlan> parse_plan(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError,
                       std::string{"flash plan: TOML parse: "}
                       + e.description().data());
    }

    auto const *plan_tbl = root["plan"].as_table();
    if (plan_tbl == nullptr) {
        return failure(ErrorCode::ParseError,
                       "flash plan: missing [plan] table");
    }

    int const schema = plan_tbl->get_as<int64_t>("schema_version")
                           ? static_cast<int>(plan_tbl->get_as<int64_t>("schema_version")->get())
                           : 0;
    if (schema != kPlanSchemaVersion) {
        return failure(ErrorCode::UnsupportedVersion,
                       "flash plan: schema_version "
                       + std::to_string(schema)
                       + " not supported (expected "
                       + std::to_string(kPlanSchemaVersion) + ")");
    }

    FlashPlan plan;

    auto opt_u8 = [&](char const *key, std::uint8_t fallback) {
        auto const *v = plan_tbl->get_as<int64_t>(key);
        if (v == nullptr) return fallback;
        auto const raw = v->get();
        if (raw < 0 || raw > 0xFF) return fallback;
        return static_cast<std::uint8_t>(raw);
    };
    auto opt_u32 = [&](char const *key, std::uint32_t fallback) {
        auto const *v = plan_tbl->get_as<int64_t>(key);
        if (v == nullptr) return fallback;
        auto const raw = v->get();
        if (raw < 0) return fallback;
        return static_cast<std::uint32_t>(raw);
    };
    auto opt_bool = [&](char const *key, bool fallback) {
        auto const *v = plan_tbl->get_as<bool>(key);
        return v == nullptr ? fallback : v->get();
    };

    plan.session            = opt_u8("session", plan.session);
    plan.data_format        = opt_u8("data_format", plan.data_format);
    plan.silence_bus        = opt_bool("silence_bus", plan.silence_bus);
    plan.verify_after_write = opt_bool("verify_after_write",
                                       plan.verify_after_write);
    plan.dry_run            = opt_bool("dry_run", plan.dry_run);
    plan.block_size_hint    = opt_u32("block_size_hint", plan.block_size_hint);
    plan.verify_chunk_size  = opt_u32("verify_chunk_size",
                                       plan.verify_chunk_size);

    auto const *writes = root["write"].as_array();
    if (writes == nullptr) {
        return failure(ErrorCode::ParseError,
                       "flash plan: at least one [[write]] entry is required");
    }
    plan.writes.reserve(writes->size());
    std::size_t idx = 0;
    for (auto const &node : *writes) {
        auto const *tbl = node.as_table();
        if (tbl == nullptr) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] entry " + std::to_string(idx)
                           + " is not a table");
        }
        SectorWrite sw;
        auto const *addr_node = tbl->get_as<int64_t>("address");
        if (addr_node == nullptr) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx)
                           + ": missing 'address'");
        }
        auto const addr_val = addr_node->get();
        if (addr_val < 0 || addr_val > 0xFFFFFFFFLL) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx)
                           + ": address out of 32-bit range");
        }
        sw.sector.address = static_cast<std::uint32_t>(addr_val);

        auto const *data_node = tbl->get_as<std::string>("data");
        if (data_node == nullptr) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx)
                           + ": missing 'data' (hex string)");
        }
        auto bytes = parse_hex_bytes(data_node->get());
        if (!bytes.has_value()) {
            return failure(bytes.error().code(),
                           "flash plan: [[write]] " + std::to_string(idx)
                           + ": " + std::string{bytes.error().message()});
        }
        if (bytes->empty()) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx)
                           + ": 'data' must contain at least one byte");
        }
        sw.sector.length = static_cast<std::uint32_t>(bytes->size());
        sw.data          = std::move(*bytes);

        plan.writes.push_back(std::move(sw));
        ++idx;
    }
    if (plan.writes.empty()) {
        return failure(ErrorCode::ParseError,
                       "flash plan: at least one [[write]] entry is required");
    }
    return plan;
}

Result<FlashPlan> read_plan(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound,
                       "flash plan: cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_plan(ss.str());
}

std::string format_plan(FlashPlan const &plan) {
    std::ostringstream out;
    out << "# SubuwuTuner flash plan\n";
    out << "[plan]\n";
    out << "schema_version     = " << kPlanSchemaVersion << "\n";
    out << "session            = 0x"
        << std::hex << static_cast<unsigned>(plan.session) << std::dec << "\n";
    out << "data_format        = 0x"
        << std::hex << static_cast<unsigned>(plan.data_format) << std::dec
        << "\n";
    out << "silence_bus        = " << (plan.silence_bus ? "true" : "false")
        << "\n";
    out << "verify_after_write = "
        << (plan.verify_after_write ? "true" : "false") << "\n";
    out << "dry_run            = " << (plan.dry_run ? "true" : "false")
        << "\n";
    out << "block_size_hint    = " << plan.block_size_hint << "\n";
    out << "verify_chunk_size  = 0x"
        << std::hex << plan.verify_chunk_size << std::dec << "\n";

    for (auto const &w : plan.writes) {
        out << "\n[[write]]\n";
        out << "address = 0x"
            << std::hex << w.sector.address << std::dec << "\n";
        // Multi-line basic string (triple-quoted) so the line-wrapped hex
        // output round-trips through TOML. Basic single-line strings
        // can't contain raw newlines.
        out << "data    = \"\"\"\n" << format_hex_bytes(w.data) << "\n\"\"\"\n";
    }
    return out.str();
}

Status write_plan(std::filesystem::path const &path, FlashPlan const &plan) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "flash plan: cannot open for write: " + path.string());
    }
    auto const text = format_plan(plan);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "flash plan: write failed: " + path.string());
    }
    return ok();
}

} // namespace st::flash
