// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_DEFS_HPP
#define ST_DEFS_HPP

#include "st/core/result.hpp"
#include "st/rom.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace st {

// How raw bytes in the ROM are interpreted. Subaru ECUs are big-endian by
// convention; LE variants exist so the same enum can describe other ECUs we
// haven't added platform support for yet.
enum class DataType {
    Uint8,
    Int8,
    Uint16Be,
    Uint16Le,
    Int16Be,
    Int16Le,
    Uint32Be,
    Uint32Le,
    Int32Be,
    Int32Le,
    Float32Be,
    Float32Le,
};

[[nodiscard]] Result<DataType>  parse_data_type(std::string_view s);
[[nodiscard]] std::string_view  to_string(DataType dt) noexcept;
[[nodiscard]] std::size_t       byte_size(DataType dt) noexcept;

// Read `byte_size(dt)` bytes from `rom` at `offset`, interpret per `dt`, and
// return the value as a double. The double carries integer values exactly up
// to 2^53; for float32_* the conversion is lossless. Returns OutOfRange if
// the read would extend past the ROM.
[[nodiscard]] Result<double> read_typed(Rom const &rom, std::size_t offset, DataType dt);

// How to find the calibration ID inside a ROM and which CID strings this pack
// claims to match.
struct Identification {
    std::string name;
    std::size_t cid_address{};
    std::size_t cid_length{};
    std::string cid_match;
    std::string ecu_part;
};

// Pack-level metadata.
struct Pack {
    int                       schema_version{1};
    std::string               id;
    std::string               display_name;
    std::string               platform;
    std::string               transmission;
    std::vector<int>          years;
    std::string               endianness{"big"};
    std::size_t               rom_size_bytes{};
    std::vector<std::string>  authors;
    std::vector<std::string>  data_sources;
    std::string               license;
    std::optional<std::string> extends;
};

// Linear: raw -> (raw * factor) + offset.
struct LinearScaling {
    double factor{1.0};
    double offset{0.0};
};

// Piecewise: linear interpolation between (breakpoints[i], values[i]) pairs.
// The two arrays must be the same length and breakpoints must be sorted.
struct PiecewiseScaling {
    std::vector<double> breakpoints;
    std::vector<double> values;
};

using ScalingFormula = std::variant<LinearScaling, PiecewiseScaling>;

struct Scaling {
    std::string    id;
    ScalingFormula formula;
    std::string    unit;
    double         min{0.0};
    double         max{0.0};
    int            precision{0};
    DataType       data_type{DataType::Uint8};
};

// Apply a scaling: raw -> engineering units.
[[nodiscard]] double apply_scaling(double raw, Scaling const &s) noexcept;

// Index axis for a table — values live in the ROM at a fixed offset.
struct Axis {
    std::string id;
    std::string name;
    std::string unit;
    std::string type{"static"};
    std::size_t address{};
    std::size_t length{};
    DataType    data_type{DataType::Uint16Be};
    std::string scaling;
};

// A calibration table.
struct Table {
    std::string                id;
    std::string                name;
    std::string                category;
    int                        dimensions{2};
    std::size_t                address{};
    DataType                   data_type{DataType::Uint16Be};
    std::string                scaling;
    std::optional<std::string> axis_x;
    std::optional<std::string> axis_y;
    std::optional<std::string> axis_z;
    std::optional<std::string> notes;
    bool                       emissions_relevant{false};
    bool                       engine_safety_critical{false};
};

// A datalogger PID — addressed via SSM, scaled the same way as table values.
struct Pid {
    std::string id;
    std::string name;
    std::size_t ssm_address{};
    std::size_t length{};
    DataType    data_type{DataType::Uint8};
    std::string scaling;
    std::string unit;
    bool        default_log{false};
};

// A complete definition pack: one Pack header, plus 0..N of each child kind.
class Definition {
  public:
    [[nodiscard]] static Result<Definition> from_toml_string(std::string_view toml);
    [[nodiscard]] static Result<Definition> from_file(std::filesystem::path const &path);

    // Cross-reference + bounds validation. Returns Ok or a ParseError naming
    // every violation found (one Error, multi-line message).
    [[nodiscard]] Status validate() const;

    [[nodiscard]] Pack const &                       pack() const noexcept { return pack_; }
    [[nodiscard]] std::vector<Identification> const &identifications() const noexcept {
        return ids_;
    }
    [[nodiscard]] std::vector<Axis> const &    axes() const noexcept { return axes_; }
    [[nodiscard]] std::vector<Scaling> const & scalings() const noexcept { return scalings_; }
    [[nodiscard]] std::vector<Table> const &   tables() const noexcept { return tables_; }
    [[nodiscard]] std::vector<Pid> const &     pids() const noexcept { return pids_; }

    [[nodiscard]] Axis const *    find_axis(std::string_view id) const noexcept;
    [[nodiscard]] Scaling const * find_scaling(std::string_view id) const noexcept;
    [[nodiscard]] Table const *   find_table(std::string_view id) const noexcept;
    [[nodiscard]] Pid const *     find_pid(std::string_view id) const noexcept;

    // If `rom` matches one of the [[identification]] entries, return the
    // entry's `name`. Otherwise nullopt.
    [[nodiscard]] std::optional<std::string> matches(Rom const &rom) const;

    // Read `axis.length` values from the ROM, applying `axis.scaling` if it
    // resolves to a known scaling. If the axis has no scaling, raw values
    // are returned as-is.
    [[nodiscard]] Result<std::vector<double>> read_axis_values(Rom const &rom,
                                                               Axis const &axis) const;

    // Materialized view of a calibration table: axis labels plus the value
    // grid. For a 1D table, `axis_y` is empty and `values` is a single row.
    // For a 2D table, `values[row][col]` is the value at (axis_y[row],
    // axis_x[col]). Subaru convention: row-major storage, X varying fastest.
    struct TableData {
        std::vector<double>              axis_x;
        std::vector<double>              axis_y;
        std::vector<std::vector<double>> values;
    };

    [[nodiscard]] Result<TableData> read_table_values(Rom const &  rom,
                                                      Table const &table) const;

  private:
    Pack                        pack_;
    std::vector<Identification> ids_;
    std::vector<Axis>           axes_;
    std::vector<Scaling>        scalings_;
    std::vector<Table>          tables_;
    std::vector<Pid>            pids_;

    friend class DefinitionBuilder;
};

} // namespace st

#endif // ST_DEFS_HPP
