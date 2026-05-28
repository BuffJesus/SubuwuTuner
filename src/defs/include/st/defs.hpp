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

[[nodiscard]] Result<DataType> parse_data_type(std::string_view s);
[[nodiscard]] std::string_view to_string(DataType dt) noexcept;
[[nodiscard]] std::size_t byte_size(DataType dt) noexcept;

// Read `byte_size(dt)` bytes from `rom` at `offset`, interpret per `dt`, and
// return the value as a double. The double carries integer values exactly up
// to 2^53; for float32_* the conversion is lossless. Returns OutOfRange if
// the read would extend past the ROM.
[[nodiscard]] Result<double> read_typed(Rom const &rom, std::size_t offset, DataType dt);

// How to find the calibration ID inside a ROM and which CID strings this pack
// claims to match.
//
// Two location modes:
// - **Fixed-offset** (default): compare `cid_length` bytes at `cid_address`
//   against `cid_match`. EJ-era Subaru ROMs put the CID at a stable offset
//   (typically 0x2000) and this is the natural shape.
// - **Scan** (`cid_scan = true`): search the entire ROM for an occurrence
//   of `cid_match` (as raw bytes). FA-DIT WRX / VB ROMs embed the CID in a
//   descriptor block at a variable per-firmware offset; `0x2F7DD` for one
//   `LF75300E` plaintext, `0x38035` for an `LF9C000C` plaintext, etc. — same
//   surrounding shape but no fixed address. When `cid_scan` is true,
//   `cid_address` is ignored.
struct Identification {
    std::string name;
    std::size_t cid_address{};
    std::size_t cid_length{};
    std::string cid_match;
    std::string ecu_part;
    bool cid_scan{false};
};

// Pack-level metadata.
struct Pack {
    int schema_version{1};
    std::string id;
    std::string display_name;
    std::string platform;
    std::string transmission;
    std::vector<int> years;
    std::string endianness{"big"};
    std::size_t rom_size_bytes{};
    // Per-ECU-family checksum selector. Names mirror RomRaider's
    // ChecksumXxx class family (`subaru_std` ↔ ChecksumSTD,
    // `subaru_alt` ↔ ChecksumALT, `subaru_alt2` ↔ ChecksumALT2).
    // Empty string is permitted at parse time + treated as
    // "unspecified" — st::flash::checksum_kind_from() defaults
    // that to None (no-op repair). The actual repair algorithm
    // is consumed by st::flash::IChecksumRepair when Phase 4
    // post-write checksum repair lands; today this field is
    // recorded + surfaced through pack-info so a pack author can
    // declare intent ahead of the implementation.
    std::string checksum_type;
    std::vector<std::string> authors;
    std::vector<std::string> data_sources;
    std::string license;
    std::optional<std::string> extends;
    // Relative paths to sibling fragment TOMLs whose [[scaling]]/[[axis]]/
    // [[table]]/[[pid]]/[[switch]] arrays get merged into this pack at load
    // time. Fragments may themselves carry a [pack] header — it's ignored
    // (only records flow through). Paths are resolved against the pack
    // file's parent directory.
    std::vector<std::string> includes;
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

// Subaru-specific enrichment formula: value = numerator / (1 + raw * k).
// Used by primary open-loop fueling tables that store an enrichment offset
// as raw uint8 0..255 and decode to AFR via 14.7/(1 + raw*0.0078125).
// Source: Merp's canonical ecu_defs.xml expression="14.7/(1+x*.0078125)".
// Cannot be expressed as a LinearScaling because the relationship is
// reciprocal, not affine. Defgen recognizes the expression pattern and
// emits this named formula in pack.toml.
struct SubaruAfrEnrichment {
    double numerator{14.7};
    double k{0.0078125};
};

// Inverse-divide formula: value = numerator / raw.
// Used by Subaru injector flow scaling (the float-stored value is a
// reciprocal pulse-width-to-flow-rate constant; correct cc/min display
// requires `2707090 / raw`), and by gear-determination thresholds
// (`96560.6 / raw`). Cannot be expressed as a LinearScaling because
// the relationship is reciprocal. Defgen recognizes `N/x` expressions
// in the source XML and emits this formula.
struct InverseDivideScaling {
    double numerator{1.0};
};

using ScalingFormula =
    std::variant<LinearScaling, PiecewiseScaling, SubaruAfrEnrichment, InverseDivideScaling>;

struct Scaling {
    std::string id;
    ScalingFormula formula;
    std::string unit;
    double min{0.0};
    double max{0.0};
    int precision{0};
    DataType data_type{DataType::Uint8};
};

// Apply a scaling: raw -> engineering units.
[[nodiscard]] double apply_scaling(double raw, Scaling const &s) noexcept;

// Invert a scaling: engineering units -> raw. Returns InvalidArgument for a
// degenerate scaling (e.g. linear with factor=0, or piecewise with a single
// breakpoint), since the mapping has no unique inverse.
[[nodiscard]] Result<double> invert_scaling(double engineering, Scaling const &s);

// Write `value` (an integer or float, depending on `dt`) to `rom` at
// `offset`. The value is first clamped to the representable range of the
// target type, so callers don't have to. Returns OutOfRange if the write
// would extend past the ROM.
[[nodiscard]] Status write_typed(Rom &rom, std::size_t offset, DataType dt, double value);

// Index axis for a table — values live in the ROM at a fixed offset.
struct Axis {
    std::string id;
    std::string name;
    std::string unit;
    std::string type{"static"};
    std::size_t address{};
    std::size_t length{};
    DataType data_type{DataType::Uint16Be};
    std::string scaling;
};

// A calibration table.
struct Table {
    std::string id;
    std::string name;
    std::string category;
    int dimensions{2};
    std::size_t address{};
    DataType data_type{DataType::Uint16Be};
    std::string scaling;
    std::optional<std::string> axis_x;
    std::optional<std::string> axis_y;
    std::optional<std::string> axis_z;
    std::optional<std::string> notes;
    bool emissions_relevant{false};
    bool engine_safety_critical{false};

    // Canonical role string for cross-pack table identity (e.g.
    // "coldstart.open_loop_fuel_vs_ect"). Optional. Packs that don't
    // tag their tables are unaffected — the suggestion-to-edit
    // affordance in the §11 panels (cold-start / EBCS / knock /
    // adaptive-history) simply stays inactive on those tables. See
    // docs/05-improvements.md §11 + docs/11-definition-format.md for
    // the role vocabulary and the per-platform mapping.
    std::optional<std::string> role;
};

// A datalogger PID — addressed via SSM, scaled the same way as table values.
struct Pid {
    std::string id;
    std::string name;
    std::size_t ssm_address{};
    std::size_t length{};
    DataType data_type{DataType::Uint8};
    std::string scaling;
    std::string unit;
    bool default_log{false};
};

// A single-bit status flag in an SSM RAM byte. RomRaider numbers bits with
// `bit=0` as the LSB. `default_log` mirrors the Pid convention.
struct Switch {
    std::string id;
    std::string name;
    std::size_t ssm_address{};
    int bit{};
    bool default_log{false};
};

// A DTC enable bitmap region in the ROM. Modern ECUs gate each diagnostic
// trouble code behind a bit in such a bitmap; clearing the bit suppresses
// the code (the monitoring logic still runs but the MIL stays off and the
// code doesn't surface to a scanner). See docs/11-definition-format.md
// "dtcs.toml — diagnostic-code enable bitmaps".
struct DtcBitmap {
    std::string id;
    std::string name;
    std::size_t address{};
    std::size_t length_bytes{};
    std::string endianness{"big"};
};

// A single diagnostic trouble code. Identified by its standard OBD-II code
// string ("P0401", "P0420", etc.); `byte_offset`/`bit` locate the enable
// bit inside the named bitmap. `bit=0` is the LSB of the byte.
struct Dtc {
    std::string code;
    std::string name;
    std::string bitmap_id;
    std::size_t byte_offset{};
    int bit{};
    bool emissions_relevant{false};
};

// Read / set the enable bit for `dtc` inside `bitmap` in `rom`. The
// effective byte address is `bitmap.address + dtc.byte_offset`; OutOfRange
// if that's past `rom.size()`. `set_dtc_enabled` returns the byte's address
// and the value before/after the change so callers can show a diff, print
// an audit line, or record the write as a ByteEdit in edit::History.
struct DtcBitChange {
    std::size_t address{};
    std::uint8_t before{};
    std::uint8_t after{};
};

[[nodiscard]] Result<bool> is_dtc_enabled(Rom const &rom, DtcBitmap const &bitmap, Dtc const &dtc);
[[nodiscard]] Result<DtcBitChange> set_dtc_enabled(Rom &rom, DtcBitmap const &bitmap,
                                                   Dtc const &dtc, bool enabled);

// One named pin on a hook — typed (Float/Int/Bool) signal that the user's
// custom-feature graph either reads from the ECU or writes back. `name` is
// the canonical id used by codegen (snake_case, stable across pack
// revisions); `label` is what the editor shows the user (free-form, may
// contain spaces, falls back to `name` when empty). Unit is optional
// human-readable metadata (e.g. "rpm", "kPa"); the editor surfaces it in
// tooltips but doesn't enforce dimensional analysis yet — that's the
// future "type system beyond enum" work in docs/16.
struct HookSignal {
    std::string name;
    std::string label;
    std::string type; // "float" | "int" | "bool" (matches feature::PinType)
    std::string unit;
    // Firmware address backing this signal. For a hook's *input* pin
    // (data the ECU offers to user logic), this is where codegen reads
    // from. For an *output* pin (user-written override), this is
    // where the firmware reads the override after the splice returns;
    // codegen would write to a private RAM slot, and the patch
    // insertion layer would later wire that slot to this address.
    // Optional — the editor doesn't need it; codegen refuses on
    // missing values with a specific error.
    std::optional<std::size_t> address;
};

// A custom-features splice point declared by the definition pack. Per
// docs/16, the pack author owns the responsibility of identifying ECU
// addresses where a hook can safely splice; the editor only needs the
// signal-level interface (inputs flow OUT of the hook node into user
// logic, outputs flow IN from user logic to the hook). ecu_address +
// free_ram are codegen metadata — present in the pack for forward use
// but the editor doesn't read them.
struct Hook {
    std::string id;
    std::string display_name; // human label; falls back to id
    std::string description;
    std::vector<HookSignal> inputs;
    std::vector<HookSignal> outputs;
    std::optional<std::size_t> ecu_address;
    std::optional<std::size_t> free_ram_base;
    std::optional<std::size_t> free_ram_length;
};

// A pack-level declaration of one flash address range that the
// codegen address gate (docs/16 §Safety #6) will accept as a target
// for compiled `.stmod` patches. Multiple entries allowed; ranges
// may be non-contiguous. A pack with zero `[[writable_region]]`
// entries fails closed at gate time — no `.stmod` will compile
// against it. This is a security boundary, not a sanity check: a
// buggy or malicious graph that bypassed the gate would otherwise
// reach `st::flash` with an arbitrary target address.
//
// The `kind` field is informational for v1.0 — the gate checks
// containment regardless of kind. Per-kind policy (e.g. production-
// fleet profiles refusing patches into `code` regions) can land in
// a future bundle without breaking the schema.
//
// `bank` is reserved for RH850 dual-bank platforms (v1.3+), where
// each calibration region exists in two banks for live update; v1.0
// targets SH-2A single-bank and ignores the field.
struct WritableRegion {
    std::string name;
    std::string kind; // "calibration" | "code" | "data"; informational v1.0
    std::size_t address{0};
    std::size_t length{0};
    std::string bank;        // optional; "" if not set
    std::string description; // optional; "" if not set
};

// A reusable computation node declared by the pack — arithmetic,
// boolean logic, table lookup, etc. Distinct from Hook in that
// primitives don't splice into the firmware; they're pure functions
// that compose between hook inputs and hook outputs. Pin direction
// follows ordinary dataflow: a primitive's `inputs` are its graph
// Input pins (consumed from upstream), `outputs` are graph Output
// pins (driven to downstream). No ECU-address or RAM-region fields
// — those make no sense for a pure computation.
struct Primitive {
    std::string id;
    std::string display_name;
    std::string description;
    std::vector<HookSignal> inputs;
    std::vector<HookSignal> outputs;
};

// A complete definition pack: one Pack header, plus 0..N of each child kind.
class Definition {
public:
    [[nodiscard]] static Result<Definition> from_toml_string(std::string_view toml);

    // Load from a single TOML file (legacy "everything in one file") or from
    // a directory laid out per docs/11-definition-format.md (pack.toml plus
    // any number of additional *.toml files in subdirectories whose
    // top-level arrays merge into one pack). Auto-dispatches on the path.
    [[nodiscard]] static Result<Definition> from_file(std::filesystem::path const &path);

    // Explicit directory loader for callers that want to require a directory
    // layout. `path` must point at a directory containing at least pack.toml.
    [[nodiscard]] static Result<Definition> from_directory(std::filesystem::path const &path);

    // Cross-reference + bounds validation. Returns Ok or a ParseError naming
    // every violation found (one Error, multi-line message).
    [[nodiscard]] Status validate() const;

    [[nodiscard]] Pack const &pack() const noexcept {
        return pack_;
    }
    [[nodiscard]] std::vector<Identification> const &identifications() const noexcept {
        return ids_;
    }
    [[nodiscard]] std::vector<Axis> const &axes() const noexcept {
        return axes_;
    }
    [[nodiscard]] std::vector<Scaling> const &scalings() const noexcept {
        return scalings_;
    }
    [[nodiscard]] std::vector<Table> const &tables() const noexcept {
        return tables_;
    }
    [[nodiscard]] std::vector<Pid> const &pids() const noexcept {
        return pids_;
    }
    [[nodiscard]] std::vector<Switch> const &switches() const noexcept {
        return switches_;
    }
    [[nodiscard]] std::vector<DtcBitmap> const &dtc_bitmaps() const noexcept {
        return dtc_bitmaps_;
    }
    [[nodiscard]] std::vector<Dtc> const &dtcs() const noexcept {
        return dtcs_;
    }
    [[nodiscard]] std::vector<Hook> const &hooks() const noexcept {
        return hooks_;
    }
    [[nodiscard]] std::vector<Primitive> const &primitives() const noexcept {
        return primitives_;
    }
    [[nodiscard]] std::vector<WritableRegion> const &writable_regions() const noexcept {
        return writable_regions_;
    }

    [[nodiscard]] Axis const *find_axis(std::string_view id) const noexcept;
    [[nodiscard]] Scaling const *find_scaling(std::string_view id) const noexcept;
    [[nodiscard]] Table const *find_table(std::string_view id) const noexcept;
    // Find a table by its canonical `role` string (e.g.
    // "coldstart.open_loop_fuel_vs_ect"). Returns the first matching
    // table in pack-declared order, or nullptr if no table carries
    // that role. Tables without a role are skipped. Used by the §11
    // panels to resolve a suggested edit to the right table on the
    // user's loaded pack without name-pattern guessing.
    [[nodiscard]] Table const *find_table_by_role(std::string_view role) const noexcept;
    [[nodiscard]] Pid const *find_pid(std::string_view id) const noexcept;
    [[nodiscard]] Switch const *find_switch(std::string_view id) const noexcept;
    [[nodiscard]] DtcBitmap const *find_dtc_bitmap(std::string_view id) const noexcept;
    [[nodiscard]] Dtc const *find_dtc(std::string_view code) const noexcept;
    [[nodiscard]] Hook const *find_hook(std::string_view id) const noexcept;
    [[nodiscard]] Primitive const *find_primitive(std::string_view id) const noexcept;

    // If `rom` matches one of the [[identification]] entries, return the
    // entry's `name`. Otherwise nullopt.
    [[nodiscard]] std::optional<std::string> matches(Rom const &rom) const;

    // Same as `matches`, but also reports the byte offset where the CID
    // was found and whether scan mode was used. Useful for diagnostic
    // display (`rom-info --def …` shows the discovered offset for
    // scan-mode matches). When `scanned == false`, `offset == cid_address`
    // from the matching identification.
    struct MatchInfo {
        std::string name;
        std::size_t offset{};
        bool scanned{false};
    };
    [[nodiscard]] std::optional<MatchInfo> match_info(Rom const &rom) const;

    // Read `axis.length` values from the ROM, applying `axis.scaling` if it
    // resolves to a known scaling. If the axis has no scaling, raw values
    // are returned as-is.
    [[nodiscard]] Result<std::vector<double>> read_axis_values(Rom const &rom,
                                                               Axis const &axis) const;

    // Materialized view of a calibration table: axis labels plus the value
    // grid. Storage shape depends on the table's dimensions:
    //
    //   1D: `axis_y` is empty; `values` is a single row of size axis_x.size().
    //   2D: `values[row][col]` = value at (axis_y[row], axis_x[col]).
    //   3D: `axis_z` carries the slice labels; `slices[z][y][x]` carries the
    //        grid. For 1D and 2D, `axis_z` and `slices` are empty.
    //
    // Subaru convention: row-major outer-to-inner Z/Y/X. The bytes on disk
    // are slice 0's whole y*x grid, then slice 1's, etc.
    struct TableData {
        std::vector<double> axis_x;
        std::vector<double> axis_y;
        std::vector<double> axis_z;
        std::vector<std::vector<double>> values;
        std::vector<std::vector<std::vector<double>>> slices;
    };

    [[nodiscard]] Result<TableData> read_table_values(Rom const &rom, Table const &table) const;

    // Write `td.values` back to `rom` at `table.address`, inverting the
    // table's scaling and using `table.data_type` for the per-cell byte
    // layout. Axis bytes are NOT written — they are read-only metadata in
    // most cases, and writing them would require re-resolving every table
    // that shares the axis. The TableData's grid shape must match the
    // axis lengths declared in the definition; otherwise InvalidArgument.
    [[nodiscard]] Status write_table_values(Rom &rom, Table const &table,
                                            TableData const &td) const;

    // Summary of a per-table diff between two ROMs of the same definition.
    struct TableDiff {
        std::size_t total_cells{0};
        std::size_t cells_changed{0};
        double max_abs_delta{0.0};
        double mean_abs_delta{0.0};

        [[nodiscard]] bool changed() const noexcept {
            return cells_changed > 0;
        }
    };

    // Compare a single table's scaled values between two ROMs. Both ROMs must
    // contain enough bytes for the table; the call fails OutOfRange otherwise.
    [[nodiscard]] Result<TableDiff> diff_table(Rom const &a, Rom const &b,
                                               Table const &table) const;

private:
    Pack pack_;
    std::vector<Identification> ids_;
    std::vector<Axis> axes_;
    std::vector<Scaling> scalings_;
    std::vector<Table> tables_;
    std::vector<Pid> pids_;
    std::vector<Switch> switches_;
    std::vector<DtcBitmap> dtc_bitmaps_;
    std::vector<Dtc> dtcs_;
    std::vector<Hook> hooks_;
    std::vector<Primitive> primitives_;
    std::vector<WritableRegion> writable_regions_;

    friend class DefinitionBuilder;
};

} // namespace st

#endif // ST_DEFS_HPP
