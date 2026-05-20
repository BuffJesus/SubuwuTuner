// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_DBC_HPP
#define ST_DBC_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::dbc {

// DBC bit order — the long-standing Vector convention:
//   @1 = Intel  (little-endian; LSB-first byte/bit numbering)
//   @0 = Motorola (big-endian; MSB-first byte/bit numbering)
// We carry the byte_order through round-trips without doing the
// bit-extraction here; a separate decoder layer (CAN frame + DBC →
// engineering value) is a follow-up.
enum class ByteOrder : std::uint8_t {
    Intel = 1,
    Motorola = 0,
};

// Sign convention for the raw bit field.
enum class SignKind : std::uint8_t {
    Unsigned = 0,
    Signed = 1,
};

// One signal inside a message. Mirrors the DBC `SG_` row.
struct Signal {
    std::string name;
    std::size_t start_bit{0};
    std::size_t length_bits{0};
    ByteOrder byte_order{ByteOrder::Intel};
    SignKind sign{SignKind::Unsigned};
    double factor{1.0};
    double offset{0.0};
    double min_value{0.0};
    double max_value{0.0};
    std::string unit;
    std::vector<std::string> receivers; // typically just "Vector__XXX"
};

// One CAN message. Mirrors the DBC `BO_` row + the SG_ rows beneath it.
struct Message {
    std::uint32_t id{0};  // 11-bit or 29-bit CAN ID
    bool extended{false}; // true if 29-bit
    std::string name;
    std::size_t length{0}; // bytes (DLC)
    std::string sender;    // node id; "Vector__XXX" if anonymous
    std::vector<Signal> signals;
};

// A parsed DBC database. Holds the messages plus the few top-level
// fields the round-trip needs to preserve. Everything else in a real
// DBC (comments, attribute definitions, value tables, environment
// variables, ...) is skipped on read and not emitted on write — adding
// those is a separate, bounded slice.
struct Database {
    std::string version;            // VERSION ""
    std::vector<std::string> nodes; // BU_: list
    std::vector<Message> messages;  // BO_ ... entries

    // Lookups. Linear scan — DBC files have ~thousands of messages at
    // most, so the simplicity is worth the cost.
    [[nodiscard]] Message const *find_message(std::uint32_t id) const noexcept;
    [[nodiscard]] Signal const *find_signal(std::uint32_t msg_id,
                                            std::string_view signal_name) const noexcept;
};

// Parsing / emitting.
//
// The parser supports the canonical 24-bit DBC formed by VERSION, BU_,
// and BO_/SG_ rows. Other section markers (NS_, BS_, CM_, BA_, VAL_,
// EV_, SIG_GROUP_, …) are recognised by their leading tag and skipped
// — they do not produce errors. A malformed SG_ row produces a
// ParseError. Multi-line continuations and quoted-string escapes in
// VERSION strings are handled.

[[nodiscard]] Result<Database> parse_dbc(std::string_view text);

[[nodiscard]] Result<Database> read_dbc(std::filesystem::path const &path);

[[nodiscard]] std::string format_dbc(Database const &db);

[[nodiscard]] Status write_dbc(std::filesystem::path const &path, Database const &db);

// =====================================================================
// Signal decoding: raw frame bytes -> engineering value
// =====================================================================
//
// Given a signal definition and the data bytes of a CAN frame, extract
// the raw bit field according to the DBC bit-numbering convention,
// apply sign extension (if `sign == Signed`), and apply linear scaling
// (factor / offset).
//
// Vector's bit-numbering convention — the same one cantools and SavvyCAN
// use:
//   - Bit 0 is the LSB of byte 0.
//   - Bit 7 is the MSB of byte 0.
//   - Bit 8 is the LSB of byte 1, ... bit 63 is the MSB of byte 7.
// Signal `start_bit` is the index of the LSB of the field (for Intel /
// little-endian) or the MSB of the field's first byte (for Motorola /
// big-endian, with the next bits walking down to bit 0 of that byte,
// then wrapping to bit 7 of the next byte).
//
// Returns 0.0 if `data` is shorter than required, or if `length_bits`
// is 0 or > 64. No allocations, no exceptions.

[[nodiscard]] std::uint64_t extract_raw_bits(std::span<std::uint8_t const> data,
                                             Signal const &sig) noexcept;

[[nodiscard]] double decode_signal(std::span<std::uint8_t const> data, Signal const &sig) noexcept;

} // namespace st::dbc

#endif // ST_DBC_HPP
