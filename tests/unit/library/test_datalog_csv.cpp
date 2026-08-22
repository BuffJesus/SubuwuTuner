// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Pins the CSV parser's behavior for already-decompressed COBB AP
// datalog files: row-major header + numeric columns, CRLF tolerated,
// empty cells -> NaN, NaN excluded from stats.

#include "st/library/datalog_csv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using st::library::datalog_csv::append_derived_channel;
using st::library::datalog_csv::DerivedOperation;
using st::library::datalog_csv::parse;
using st::library::datalog_csv::parse_cell;
using st::library::datalog_csv::range_stats;
using st::library::datalog_csv::split_line;

TEST_CASE("split_line: simple comma separators", "[library][datalog_csv]") {
    auto const cells = split_line("a,b,c");
    REQUIRE(cells.size() == 3);
    CHECK(cells[0] == "a");
    CHECK(cells[1] == "b");
    CHECK(cells[2] == "c");
}

TEST_CASE("split_line: empty cells", "[library][datalog_csv]") {
    auto const cells = split_line("a,,c");
    REQUIRE(cells.size() == 3);
    CHECK(cells[0] == "a");
    CHECK(cells[1].empty());
    CHECK(cells[2] == "c");
}

TEST_CASE("split_line: trims trailing CR", "[library][datalog_csv]") {
    auto const cells = split_line("a,b,c\r");
    REQUIRE(cells.size() == 3);
    CHECK(cells[2] == "c");
}

TEST_CASE("split_line: trailing comma -> trailing empty cell", "[library][datalog_csv]") {
    auto const cells = split_line("a,b,");
    REQUIRE(cells.size() == 3);
    CHECK(cells[2].empty());
}

TEST_CASE("parse_cell: numeric", "[library][datalog_csv]") {
    CHECK(parse_cell("1.5") == 1.5f);
    CHECK(parse_cell("-2.25") == -2.25f);
    CHECK(parse_cell("0") == 0.0f);
}

TEST_CASE("parse_cell: empty -> NaN", "[library][datalog_csv]") {
    CHECK(std::isnan(parse_cell("")));
}

TEST_CASE("parse_cell: non-numeric -> NaN", "[library][datalog_csv]") {
    CHECK(std::isnan(parse_cell("nope")));
    CHECK(std::isnan(parse_cell("xyz")));
    CHECK(std::isnan(parse_cell("12rpm")));
    CHECK(parse_cell(" 12.5\t") == 12.5f);
}

TEST_CASE("parse: detects time columns and UTF-8 BOM", "[library][datalog_csv]") {
    auto const dl = parse("\xEF\xBB\xBFTime (s),RPM\n0,1000\n1,2000\n");
    REQUIRE(dl.time_column.has_value());
    CHECK(*dl.time_column == 0);
    CHECK(dl.headers[0] == "Time (s)");
}

TEST_CASE("parse: ignores hash comments before headers and among samples",
          "[library][datalog_csv]") {
    auto const dl = parse("# exported synthetic log\n"
                          "  # channel notes\n"
                          "Time,RPM,Boost\n"
                          "0,1000,5\n"
                          "# pull begins\n"
                          "1,2000,10\n");
    REQUIRE(dl.headers.size() == 3);
    CHECK(dl.headers[0] == "Time");
    CHECK(dl.headers[1] == "RPM");
    CHECK(dl.row_count == 2);
    CHECK(dl.malformed_row_count == 0);
    CHECK(dl.invalid_cell_count == 0);
    CHECK(dl.stats[1].mean == 1500.0);
}

TEST_CASE("parse: records import fidelity diagnostics", "[library][datalog_csv]") {
    auto const dl = parse("Time,RPM,Boost\n0,1000,5\n1,broken\n2,3000,8,extra\n");
    CHECK(dl.malformed_row_count == 2);
    CHECK(dl.invalid_cell_count == 1);
    CHECK(dl.stats[1].sample_count == 2);
}

TEST_CASE("parse: empty input", "[library][datalog_csv]") {
    auto const dl = parse("");
    CHECK(dl.headers.empty());
    CHECK(dl.data.empty());
    CHECK(dl.row_count == 0);
}

TEST_CASE("parse: header-only input", "[library][datalog_csv]") {
    auto const dl = parse("Time,RPM,MAP\n");
    REQUIRE(dl.headers.size() == 3);
    CHECK(dl.headers[0] == "Time");
    CHECK(dl.headers[1] == "RPM");
    CHECK(dl.headers[2] == "MAP");
    CHECK(dl.row_count == 0);
    REQUIRE(dl.data.size() == 3);
    for (auto const &col : dl.data) {
        CHECK(col.empty());
    }
}

TEST_CASE("parse: well-formed CSV with stats", "[library][datalog_csv]") {
    std::string const text = "Time,RPM,Boost\n"
                             "0.0,1000,5.0\n"
                             "0.1,2000,10.0\n"
                             "0.2,3000,15.0\n";
    auto const dl = parse(text);
    REQUIRE(dl.headers.size() == 3);
    CHECK(dl.row_count == 3);
    REQUIRE(dl.data.size() == 3);
    REQUIRE(dl.data[1].size() == 3);
    CHECK(dl.data[1][0] == 1000.0f);
    CHECK(dl.data[1][1] == 2000.0f);
    CHECK(dl.data[1][2] == 3000.0f);
    // Boost stats: min 5, max 15, mean 10
    REQUIRE(dl.stats.size() == 3);
    CHECK(dl.stats[2].sample_count == 3);
    CHECK(dl.stats[2].min == 5.0);
    CHECK(dl.stats[2].max == 15.0);
    CHECK(dl.stats[2].mean == 10.0);
}

TEST_CASE("parse: extracts display names and units", "[library][datalog_csv]") {
    auto const dl = parse("Time (s),Engine Speed [rpm],DAM\n0,1000,1\n");
    REQUIRE(dl.metadata.size() == 3);
    CHECK(dl.metadata[0].display_name == "Time");
    CHECK(dl.metadata[0].unit == "s");
    CHECK(dl.metadata[1].display_name == "Engine Speed");
    CHECK(dl.metadata[1].unit == "rpm");
    CHECK(dl.metadata[2].display_name == "DAM");
    CHECK(dl.metadata[2].unit.empty());
}

TEST_CASE("append_derived_channel: subtract ratio and percent error", "[library][datalog_csv]") {
    auto dl = parse("Actual,Target\n11,10\n9,10\n5,0\n");
    auto const delta = append_derived_channel(dl, "Error", "psi", 0, 1, DerivedOperation::Subtract);
    REQUIRE(delta.has_value());
    CHECK(dl.data[*delta][0] == 1.0f);
    CHECK(dl.data[*delta][1] == -1.0f);
    auto const ratio = append_derived_channel(dl, "Ratio", "", 0, 1, DerivedOperation::Ratio);
    REQUIRE(ratio.has_value());
    CHECK(dl.data[*ratio][0] == 1.1f);
    CHECK(std::isnan(dl.data[*ratio][2]));
    auto const pct = append_derived_channel(dl, "Error", "%", 0, 1, DerivedOperation::PercentError);
    REQUIRE(pct.has_value());
    CHECK(dl.data[*pct][0] == 10.0f);
    CHECK(dl.metadata[*pct].unit == "%");
    CHECK(dl.stats[*pct].sample_count == 2);
}

TEST_CASE("parse: CRLF line endings tolerated", "[library][datalog_csv]") {
    std::string const text = "Time,RPM\r\n"
                             "0.0,1000\r\n"
                             "0.1,2000\r\n";
    auto const dl = parse(text);
    REQUIRE(dl.headers.size() == 2);
    CHECK(dl.headers[0] == "Time");
    CHECK(dl.headers[1] == "RPM");
    CHECK(dl.row_count == 2);
    CHECK(dl.data[1][0] == 1000.0f);
    CHECK(dl.data[1][1] == 2000.0f);
}

TEST_CASE("parse: NaN cells skipped in stats", "[library][datalog_csv]") {
    std::string const text = "X\n"
                             "1\n"
                             "\n" // empty -> NaN
                             "3\n"
                             "bad\n" // non-numeric -> NaN
                             "5\n";
    auto const dl = parse(text);
    REQUIRE(dl.data.size() == 1);
    REQUIRE(dl.data[0].size() == 5);
    CHECK(dl.stats[0].sample_count == 3);
    CHECK(dl.stats[0].min == 1.0);
    CHECK(dl.stats[0].max == 5.0);
    CHECK(dl.stats[0].mean == 3.0);
}

TEST_CASE("parse: short rows pad to column count with NaN", "[library][datalog_csv]") {
    std::string const text = "A,B,C\n"
                             "1,2,3\n"
                             "4,5\n" // missing C
                             "6\n";  // missing B,C
    auto const dl = parse(text);
    REQUIRE(dl.data.size() == 3);
    CHECK(dl.row_count == 3);
    // Column C should have 1 valid sample (value 3) and 2 NaN.
    CHECK(dl.stats[2].sample_count == 1);
    CHECK(dl.stats[2].min == 3.0);
}

TEST_CASE("parse: trailing newline doesn't add a phantom row", "[library][datalog_csv]") {
    auto const dl = parse("A\n1\n2\n\n\n");
    CHECK(dl.row_count == 2);
}

TEST_CASE("range stats clamp and exclude missing samples", "[library][datalog_csv]") {
    auto const dl = parse("RPM,Boost\n1000,5\n2000,bad\n3000,15\n4000,20\n");
    auto const stats = range_stats(dl, 1, 3);
    REQUIRE(stats.size() == 2);
    CHECK(stats[0].min == 2000.0);
    CHECK(stats[0].max == 3000.0);
    CHECK(stats[0].mean == 2500.0);
    CHECK(stats[1].sample_count == 1);
    CHECK(stats[1].mean == 15.0);

    auto const reversed = range_stats(dl, 99, 2);
    CHECK(reversed[0].sample_count == 2);
}

TEST_CASE("CSV export writes selected range, gaps, and quoted headers",
          "[library][datalog_csv]") {
    auto const dl = parse("Time,Boost,Tar\"get\n0,5,6\n1,bad,7\n2,9,10\n");
    CHECK(st::library::datalog_csv::export_csv(dl, {2, 1, 99}, 1, 3) ==
          "\"Tar\"\"get\",Boost\n7,\n10,9\n");
}
