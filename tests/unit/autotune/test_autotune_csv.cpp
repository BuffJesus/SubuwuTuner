// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"
#include "st/core/error.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace at = st::autotune;
using Catch::Approx;

namespace {

constexpr std::string_view kMinimalHeader =
    "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c\n";

} // namespace

TEST_CASE("read_maf_samples_csv: empty input -> empty vector", "[autotune][csv]") {
    auto const r = at::read_maf_samples_csv("");
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("read_maf_samples_csv: header only -> empty vector", "[autotune][csv]") {
    auto const r = at::read_maf_samples_csv(kMinimalHeader);
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("read_maf_samples_csv: required columns populate fields", "[autotune][csv]") {
    std::string text(kMinimalHeader);
    text += "2.31,14.5,14.7,2500,45,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    auto const &s = (*r)[0];
    CHECK(s.maf_voltage == Approx(2.31));
    CHECK(s.actual_afr == Approx(14.5));
    CHECK(s.commanded_afr == Approx(14.7));
    CHECK(s.rpm == Approx(2500.0));
    CHECK(s.throttle_pct == Approx(45.0));
    CHECK(s.coolant_c == Approx(90.0));
    CHECK(s.iat_c == Approx(30.0));
    // Optional flags default false; first sample's rates are 0.
    CHECK(s.closed_loop == false);
    CHECK(s.knock_in_window == false);
    CHECK(s.limp_mode == false);
    CHECK(s.rpm_rate == Approx(0.0));
    CHECK(s.throttle_rate == Approx(0.0));
}

TEST_CASE("read_maf_samples_csv: [unit] header suffix is stripped", "[autotune][csv][header]") {
    // CsvSink in st::log writes columns like `coolant_c [C]` -- the unit
    // annotation comes from the channel's scaling. The autotune reader
    // strips ` [...]` so a log CSV drops straight in.
    std::string text = "maf_voltage [V],actual_afr [AFR],commanded_afr [AFR],"
                       "rpm [rpm],throttle_pct [%],coolant_c [C],iat_c [C]\n"
                       "2.31,14.5,14.7,2500,45,90,30\n";
    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].maf_voltage == Approx(2.31));
    CHECK((*r)[0].coolant_c == Approx(90.0));
}

TEST_CASE("read_maf_samples_csv: missing required column -> error", "[autotune][csv]") {
    // Drop iat_c.
    std::string text = "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c\n"
                       "2.31,14.5,14.7,2500,45,90\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
    CHECK(r.error().to_string().find("iat_c") != std::string::npos);
}

TEST_CASE("read_maf_samples_csv: column order is irrelevant", "[autotune][csv]") {
    // Shuffle columns.
    std::string text = "iat_c,rpm,maf_voltage,commanded_afr,coolant_c,actual_afr,throttle_pct\n"
                       "30,2500,2.31,14.7,90,14.5,45\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    auto const &s = (*r)[0];
    CHECK(s.maf_voltage == Approx(2.31));
    CHECK(s.actual_afr == Approx(14.5));
    CHECK(s.commanded_afr == Approx(14.7));
    CHECK(s.rpm == Approx(2500.0));
    CHECK(s.throttle_pct == Approx(45.0));
    CHECK(s.coolant_c == Approx(90.0));
    CHECK(s.iat_c == Approx(30.0));
}

TEST_CASE("read_maf_samples_csv: case-insensitive header names", "[autotune][csv]") {
    std::string text = "MAF_Voltage,Actual_AFR,Commanded_AFR,RPM,Throttle_PCT,Coolant_C,IAT_C\n"
                       "2.31,14.5,14.7,2500,45,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].maf_voltage == Approx(2.31));
}

TEST_CASE("read_maf_samples_csv: blank lines and # comments skipped", "[autotune][csv]") {
    std::string text = "# header comment\n"
                       "\n"
                       "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c\n"
                       "\n"
                       "# inline comment\n"
                       "2.31,14.5,14.7,2500,45,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].actual_afr == Approx(14.5));
}

TEST_CASE("read_maf_samples_csv: CRLF tolerated", "[autotune][csv]") {
    std::string text(kMinimalHeader);
    // Replace LF with CRLF and add a couple of rows.
    text.pop_back();
    text += "\r\n2.31,14.5,14.7,2500,45,90,30\r\n2.50,14.8,14.7,2600,50,90,30\r\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    CHECK((*r)[1].rpm == Approx(2600.0));
}

TEST_CASE("read_maf_samples_csv: non-numeric value -> error w/ row + col", "[autotune][csv]") {
    std::string text(kMinimalHeader);
    text += "2.31,14.5,14.7,2500,45,90,30\n";
    text += "oops,14.5,14.7,2500,45,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    auto const msg = r.error().to_string();
    CHECK(msg.find("row 3") != std::string::npos);
    CHECK(msg.find("maf_voltage") != std::string::npos);
    CHECK(msg.find("oops") != std::string::npos);
}

TEST_CASE("read_maf_samples_csv: optional booleans", "[autotune][csv]") {
    std::string text = "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c,"
                       "closed_loop,limp_mode\n"
                       "2.31,14.5,14.7,2500,45,90,30,1,false\n"
                       "2.31,14.5,14.7,2500,45,90,30,true,YES\n"
                       "2.31,14.5,14.7,2500,45,90,30,no,off\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    CHECK((*r)[0].closed_loop == true);
    CHECK((*r)[0].limp_mode == false);
    CHECK((*r)[1].closed_loop == true);
    CHECK((*r)[1].limp_mode == true);
    CHECK((*r)[2].closed_loop == false);
    CHECK((*r)[2].limp_mode == false);
}

TEST_CASE("read_maf_samples_csv: non-boolean in bool column -> error", "[autotune][csv]") {
    std::string text = "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c,"
                       "closed_loop\n"
                       "2.31,14.5,14.7,2500,45,90,30,maybe\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    auto const msg = r.error().to_string();
    CHECK(msg.find("closed_loop") != std::string::npos);
    CHECK(msg.find("maybe") != std::string::npos);
}

TEST_CASE("read_maf_samples_csv: forward-diff rates use time_ms", "[autotune][csv]") {
    std::string text =
        "time_ms,maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c\n"
        "0,2.31,14.5,14.7,2500,45,90,30\n"
        "100,2.31,14.5,14.7,2600,50,90,30\n"
        "200,2.31,14.5,14.7,2400,45,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    // First row: rates start at 0.
    CHECK((*r)[0].rpm_rate == Approx(0.0));
    CHECK((*r)[0].throttle_rate == Approx(0.0));
    // Row 2: (2600 - 2500) / 0.1 s = 1000 RPM/s; (50 - 45) / 0.1 = 50 %/s.
    CHECK((*r)[1].rpm_rate == Approx(1000.0));
    CHECK((*r)[1].throttle_rate == Approx(50.0));
    // Row 3: (2400 - 2600) / 0.1 s = -2000 RPM/s; (45 - 50) / 0.1 = -50 %/s.
    CHECK((*r)[2].rpm_rate == Approx(-2000.0));
    CHECK((*r)[2].throttle_rate == Approx(-50.0));
}

TEST_CASE("read_maf_samples_csv: rates default to 50 ms period without time_ms",
          "[autotune][csv]") {
    std::string text(kMinimalHeader);
    text += "2.31,14.5,14.7,2500,45,90,30\n";
    text += "2.31,14.5,14.7,2550,46,90,30\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    // 50 ms default -> 0.05 s. (2550 - 2500) / 0.05 = 1000 RPM/s.
    CHECK((*r)[1].rpm_rate == Approx(1000.0));
    CHECK((*r)[1].throttle_rate == Approx(20.0));
}

TEST_CASE("read_maf_samples_csv: zero or negative dt clamps rates to 0", "[autotune][csv]") {
    std::string text =
        "time_ms,maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c\n"
        "100,2.31,14.5,14.7,2500,45,90,30\n"
        "100,2.31,14.5,14.7,2600,50,90,30\n" // duplicate timestamp
        "50,2.31,14.5,14.7,2700,55,90,30\n"; // backwards timestamp

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    CHECK((*r)[1].rpm_rate == Approx(0.0));
    CHECK((*r)[1].throttle_rate == Approx(0.0));
    CHECK((*r)[2].rpm_rate == Approx(0.0));
    CHECK((*r)[2].throttle_rate == Approx(0.0));
}

TEST_CASE("read_maf_samples_csv: knock_in_window covers prior 250 ms", "[autotune][csv]") {
    // Sample period 50 ms. Knock fires on sample at t=100 ms (row 3).
    // Window is the inclusive 250 ms prior, so samples at t∈[100, 350]
    // should have knock_in_window=true; t=0/50/400/450 should be false.
    std::string text =
        "time_ms,maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c,knock\n"
        "0,2.31,14.5,14.7,2500,45,90,30,0\n"
        "50,2.31,14.5,14.7,2500,45,90,30,0\n"
        "100,2.31,14.5,14.7,2500,45,90,30,1\n"
        "150,2.31,14.5,14.7,2500,45,90,30,0\n"
        "200,2.31,14.5,14.7,2500,45,90,30,0\n"
        "250,2.31,14.5,14.7,2500,45,90,30,0\n"
        "300,2.31,14.5,14.7,2500,45,90,30,0\n"
        "350,2.31,14.5,14.7,2500,45,90,30,0\n"
        "400,2.31,14.5,14.7,2500,45,90,30,0\n"
        "450,2.31,14.5,14.7,2500,45,90,30,0\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 10);
    CHECK((*r)[0].knock_in_window == false); // t=0
    CHECK((*r)[1].knock_in_window == false); // t=50
    CHECK((*r)[2].knock_in_window == true);  // t=100  (knock fires)
    CHECK((*r)[3].knock_in_window == true);  // t=150
    CHECK((*r)[4].knock_in_window == true);  // t=200
    CHECK((*r)[5].knock_in_window == true);  // t=250
    CHECK((*r)[6].knock_in_window == true);  // t=300
    CHECK((*r)[7].knock_in_window == true);  // t=350  (boundary inclusive)
    CHECK((*r)[8].knock_in_window == false); // t=400  (>250 ms past event)
    CHECK((*r)[9].knock_in_window == false); // t=450
}

TEST_CASE("read_maf_samples_csv: unknown columns are tolerated", "[autotune][csv]") {
    std::string text = "maf_voltage,actual_afr,commanded_afr,rpm,throttle_pct,coolant_c,iat_c,"
                       "engine_oil_temp,brake_pressure\n"
                       "2.31,14.5,14.7,2500,45,90,30,95,0\n";

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].maf_voltage == Approx(2.31));
}

TEST_CASE("read_maf_samples_csv: missing trailing field in a row -> error", "[autotune][csv]") {
    std::string text(kMinimalHeader);
    text += "2.31,14.5,14.7,2500,45,90\n"; // missing iat_c value

    auto const r = at::read_maf_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("read_maf_samples_csv: integrates with tune_maf end-to-end", "[autotune][csv]") {
    // Three voltage cells worth of samples; all warm/stable/no knock.
    // Build a CSV programmatically so the test stays compact.
    std::string text(kMinimalHeader);
    auto add_row = [&](double v, double actual, double cmd) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%.2f,%.3f,%.3f,2500,30,90,30\n", v, actual, cmd);
        text += buf;
    };
    for (int i = 0; i < 100; ++i) {
        // Slightly lean at v=1.0 (commanded 14.7, actual 15.0)
        add_row(1.0, 15.0, 14.7);
    }
    for (int i = 0; i < 100; ++i) {
        // Stoich at v=2.0
        add_row(2.0, 14.7, 14.7);
    }

    auto const samples = at::read_maf_samples_csv(text);
    REQUIRE(samples.has_value());
    REQUIRE(samples->size() == 200);

    std::vector<double> axis{1.0, 2.0};
    std::vector<double> current{1.0, 1.0};
    auto const result = at::tune_maf(axis, current, *samples);
    REQUIRE(result.has_value());
    REQUIRE(result->cells.size() == 2);
    // Lean cell -> proposed > current (we report mean error > 1).
    CHECK(result->cells[0].mean_error > 1.0);
    CHECK(result->cells[0].proposed_value > result->cells[0].current_value);
    // Stoich cell -> unchanged within noise.
    CHECK(result->cells[1].mean_error == Approx(1.0));
    CHECK(result->cells[1].proposed_value == Approx(1.0));
}

// ---------------------------------------------------------------------
// read_knock_samples_csv
// ---------------------------------------------------------------------

namespace {

constexpr std::string_view kKnockHeader = "rpm,load,feedback_knock,coolant_c,iat_c\n";

} // namespace

TEST_CASE("read_knock_samples_csv: empty input -> empty vector", "[autotune][csv][knock]") {
    auto const r = at::read_knock_samples_csv("");
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("read_knock_samples_csv: required columns populate fields", "[autotune][csv][knock]") {
    std::string text(kKnockHeader);
    text += "3000,2.5,-1.75,90,30\n";

    auto const r = at::read_knock_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    auto const &s = (*r)[0];
    CHECK(s.rpm == Approx(3000.0));
    CHECK(s.load == Approx(2.5));
    CHECK(s.feedback_knock == Approx(-1.75));
    CHECK(s.coolant_c == Approx(90.0));
    CHECK(s.iat_c == Approx(30.0));
    CHECK(s.limp_mode == false);
}

TEST_CASE("read_knock_samples_csv: missing required column -> error", "[autotune][csv][knock]") {
    // Drop feedback_knock.
    std::string text = "rpm,load,coolant_c,iat_c\n3000,2.5,90,30\n";

    auto const r = at::read_knock_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
    CHECK(r.error().to_string().find("feedback_knock") != std::string::npos);
}

TEST_CASE("read_knock_samples_csv: optional limp_mode", "[autotune][csv][knock]") {
    std::string text = "rpm,load,feedback_knock,coolant_c,iat_c,limp_mode\n"
                       "3000,2.5,-1.75,90,30,1\n"
                       "3000,2.5,-1.75,90,30,false\n"
                       "3000,2.5,-1.75,90,30,\n"; // empty cell -> false

    auto const r = at::read_knock_samples_csv(text);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    CHECK((*r)[0].limp_mode == true);
    CHECK((*r)[1].limp_mode == false);
    CHECK((*r)[2].limp_mode == false);
}

TEST_CASE("read_knock_samples_csv: non-numeric feedback_knock -> error", "[autotune][csv][knock]") {
    std::string text(kKnockHeader);
    text += "3000,2.5,nope,90,30\n";

    auto const r = at::read_knock_samples_csv(text);
    REQUIRE_FALSE(r.has_value());
    auto const msg = r.error().to_string();
    CHECK(msg.find("feedback_knock") != std::string::npos);
    CHECK(msg.find("nope") != std::string::npos);
}

TEST_CASE("read_knock_samples_csv: integrates with tune_knock_pull "
          "end-to-end",
          "[autotune][csv][knock]") {
    // Two cells. Cell (load=2, rpm=4000) sees sustained -2.0° feedback
    // knock; cell (load=2, rpm=3000) stays at 0. With trigger_degrees
    // default 1.5° and pull_step 0.75°, only the knock-heavy cell pulls.
    std::string text(kKnockHeader);
    auto add_row = [&](double rpm, double fk) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f,2.0,%.2f,90,30\n", rpm, fk);
        text += buf;
    };
    for (int i = 0; i < 50; ++i) {
        add_row(3000, 0.0);
    }
    for (int i = 0; i < 50; ++i) {
        add_row(4000, -2.0);
    }

    auto const samples = at::read_knock_samples_csv(text);
    REQUIRE(samples.has_value());
    REQUIRE(samples->size() == 100);

    std::vector<double> rpm_axis{3000.0, 4000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current_timing{20.0, 20.0}; // 1×2 (load × rpm)
    auto const result = at::tune_knock_pull(rpm_axis, load_axis, current_timing, *samples);
    REQUIRE(result.has_value());
    REQUIRE(result->cells.size() == 2);
    // Cell 0 (rpm=3000): no knock -> unchanged.
    CHECK_FALSE(result->cells[0].pulled);
    CHECK(result->cells[0].proposed_value == Approx(20.0));
    // Cell 1 (rpm=4000): sustained -2.0 < -1.5 trigger -> pulled by 0.75.
    CHECK(result->cells[1].pulled);
    CHECK(result->cells[1].proposed_value == Approx(19.25));
}
