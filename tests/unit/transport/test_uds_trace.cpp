// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/uds_trace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace {

// Write `content` to a temp file and return its path. Caller is
// responsible for deletion; in practice the OS temp dir does that.
std::filesystem::path write_temp_trace(std::string_view content) {
    auto const path =
        std::filesystem::temp_directory_path() /
        ("test_uds_trace_" + std::to_string(static_cast<unsigned>(std::rand())) + ".uds");
    std::ofstream out{path};
    out << content;
    return path;
}

} // namespace

TEST_CASE("parse_uds_trace: canonical alternating exchange", "[transport][uds_trace]") {
    auto const path = write_temp_trace("> 22 F1 90\n"
                                       "< 62 F1 90 01 02 03\n"
                                       "> 22 F1 80\n"
                                       "< 62 F1 80 04 05 06\n");
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(err.empty());
    REQUIRE(pairs.size() == 2);
    REQUIRE(pairs[0].request == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});
    REQUIRE(pairs[0].response == std::vector<std::uint8_t>{0x62, 0xF1, 0x90, 0x01, 0x02, 0x03});
    REQUIRE(pairs[1].request == std::vector<std::uint8_t>{0x22, 0xF1, 0x80});
}

TEST_CASE("parse_uds_trace: tolerates comments + blank lines + 0x prefix",
          "[transport][uds_trace]") {
    auto const path = write_temp_trace("# leading comment\n"
                                       "\n"
                                       "  > 0x22 0xF1 0x90    # inline comment after request\n"
                                       "  < 0x62 F1 90\n"
                                       "\n"
                                       "# trailing comment, no more exchanges\n");
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(pairs.size() == 1);
    REQUIRE(pairs[0].request == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});
}

TEST_CASE("parse_uds_trace: missing file error", "[transport][uds_trace]") {
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE_FALSE(
        st::transport::parse_uds_trace("/nonexistent/path/should/not/exist.uds", pairs, err));
    REQUIRE(err.find("cannot open") != std::string::npos);
}

TEST_CASE("parse_uds_trace: empty trace file rejected", "[transport][uds_trace]") {
    auto const path = write_temp_trace("# just a comment, no pairs\n");
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE_FALSE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(err.find("contains no exchanges") != std::string::npos);
}

TEST_CASE("parse_uds_trace: unmatched request rejected", "[transport][uds_trace]") {
    auto const path = write_temp_trace("> 22 F1 90\n"
                                       "< 62 F1 90\n"
                                       "> 22 F1 80\n"); // no closing response
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE_FALSE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(err.find("unmatched request") != std::string::npos);
}

TEST_CASE("parse_uds_trace: two requests in a row rejected", "[transport][uds_trace]") {
    auto const path = write_temp_trace("> 22 F1 90\n"
                                       "> 22 F1 80\n");
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE_FALSE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(err.find("two requests in a row") != std::string::npos);
}

TEST_CASE("parse_uds_trace: bad hex byte rejected", "[transport][uds_trace]") {
    auto const path = write_temp_trace("> 22 ZZ 90\n"
                                       "< 62 F1 90\n");
    std::vector<st::transport::UdsTracePair> pairs;
    std::string err;
    REQUIRE_FALSE(st::transport::parse_uds_trace(path, pairs, err));
    REQUIRE(err.find("bad hex byte") != std::string::npos);
}
