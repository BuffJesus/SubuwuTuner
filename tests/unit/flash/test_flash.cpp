// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace flash = st::flash;
namespace uds = st::ecu::uds;

namespace {

// Convenience: queue a (request, response) pair from a uds::build_* call
// and a hand-written response.
void expect(st::transport::MockTransport &t, std::vector<std::uint8_t> req,
            std::vector<std::uint8_t> resp) {
    t.expect_send_recv(std::move(req), std::move(resp));
}

// The 9-byte eraseMemory option record the orchestrator emits: aLFI 0x44
// (4-byte size, 4-byte address), 32-bit address, 32-bit size, all big-
// endian. Mirrors the format documented in flash.cpp.
std::vector<std::uint8_t> erase_opt(std::uint32_t addr, std::uint32_t size) {
    return {
        0x44,
        static_cast<std::uint8_t>((addr >> 24) & 0xFFU),
        static_cast<std::uint8_t>((addr >> 16) & 0xFFU),
        static_cast<std::uint8_t>((addr >> 8) & 0xFFU),
        static_cast<std::uint8_t>(addr & 0xFFU),
        static_cast<std::uint8_t>((size >> 24) & 0xFFU),
        static_cast<std::uint8_t>((size >> 16) & 0xFFU),
        static_cast<std::uint8_t>((size >> 8) & 0xFFU),
        static_cast<std::uint8_t>(size & 0xFFU),
    };
}

} // namespace

// ---------------------------------------------------------------------
// read_full_rom
// ---------------------------------------------------------------------

TEST_CASE("Flasher::read_full_rom concatenates chunked ReadMemoryByAddress", "[flash][read]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // 16 bytes from 0x00001000, split into 2 chunks of 8.
    expect(t, uds::build_read_memory_by_address(0x00001000, 8),
           {0x63, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
    expect(t, uds::build_read_memory_by_address(0x00001008, 8),
           {0x63, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F});

    flash::Flasher f{t};
    auto const r = f.read_full_rom(0x00001000, 16, /*max_chunk=*/8);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 16);
    for (std::size_t i = 0; i < r->size(); ++i) {
        REQUIRE((*r)[i] == i);
    }
    REQUIRE(t.exhausted());
}

TEST_CASE("Flasher::read_full_rom with zero length returns empty without I/O", "[flash][read]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    flash::Flasher f{t};
    auto const r = f.read_full_rom(0x1000, 0);
    REQUIRE(r.has_value());
    REQUIRE(r->empty());
    REQUIRE(t.send_log().empty());
}

TEST_CASE("Flasher::read_full_rom rejects zero max_chunk_size", "[flash][read][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    flash::Flasher f{t};
    auto const r = f.read_full_rom(0x1000, 16, /*max_chunk=*/0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Flasher::read_full_rom fires progress callback per chunk", "[flash][read][progress]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Two-chunk read at 8 bytes each = 16 bytes total. Expect 3 progress
    // events: the t=0 start ping + one after each chunk.
    expect(t, uds::build_read_memory_by_address(0x1000, 8),
           {0x63, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
    expect(t, uds::build_read_memory_by_address(0x1008, 8),
           {0x63, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F});

    std::vector<std::uint32_t> bytes_done_at_each_event;
    std::uint32_t observed_total = 0;
    flash::Flasher f{t};
    auto const r = f.read_full_rom(0x1000, 16, /*max_chunk=*/8, std::chrono::milliseconds{1000},
                                   [&](flash::Flasher::ReadProgress p) {
                                       bytes_done_at_each_event.push_back(p.bytes_done);
                                       observed_total = p.total_bytes;
                                   });
    REQUIRE(r.has_value());
    REQUIRE(observed_total == 16);
    // Events: 0 (start), 8 (after chunk 1), 16 (after chunk 2).
    REQUIRE(bytes_done_at_each_event == std::vector<std::uint32_t>{0, 8, 16});
}

TEST_CASE("Flasher::read_full_rom aborts when cancel flag set", "[flash][read][cancel]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Cancel BEFORE the loop even hits the first chunk: nothing should
    // be sent. The flag is checked at the top of every iteration.
    std::atomic<bool> cancel{true};
    flash::Flasher f{t};
    auto const r = f.read_full_rom(0x1000, 16, /*max_chunk=*/8, std::chrono::milliseconds{1000},
                                   /*progress=*/nullptr, &cancel);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::Cancelled);
    REQUIRE(t.send_log().empty());
}

TEST_CASE("Flasher::read_full_rom cancel observed mid-read", "[flash][read][cancel]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // One chunk succeeds; cancel flips inside its progress callback;
    // the next chunk's cancel check trips before any more I/O.
    expect(t, uds::build_read_memory_by_address(0x1000, 8),
           {0x63, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});

    std::atomic<bool> cancel{false};
    flash::Flasher f{t};
    auto const r = f.read_full_rom(
        0x1000, 16, /*max_chunk=*/8, std::chrono::milliseconds{1000},
        [&](flash::Flasher::ReadProgress p) {
            if (p.bytes_done >= 8)
                cancel.store(true, std::memory_order_release);
        },
        &cancel);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::Cancelled);
    // The second chunk's request was never sent — exactly one chunk on
    // the wire.
    REQUIRE(t.send_log().size() == 1);
}

// ---------------------------------------------------------------------
// compute_delta
// ---------------------------------------------------------------------

TEST_CASE("Flasher::compute_delta finds the sectors that differ", "[flash][delta]") {
    std::vector<std::uint8_t> current(0x4000, 0xAA);
    std::vector<std::uint8_t> target = current;
    target[0x0500] = 0x55; // changes sector 0 (0x0000..0x0FFF)
    target[0x2800] = 0x55; // changes sector 2 (0x2000..0x2FFF)

    auto const sectors =
        flash::Flasher::compute_delta(current, target, /*sector_size=*/0x1000, /*base=*/0x10000);
    REQUIRE(sectors.size() == 2);
    REQUIRE(sectors[0] == flash::Sector{0x10000, 0x1000});
    REQUIRE(sectors[1] == flash::Sector{0x12000, 0x1000});
}

TEST_CASE("Flasher::compute_delta returns empty for identical buffers", "[flash][delta]") {
    std::vector<std::uint8_t> a(0x2000, 0x00);
    auto const sectors = flash::Flasher::compute_delta(a, a, 0x1000);
    REQUIRE(sectors.empty());
}

TEST_CASE("Flasher::compute_delta handles a final short sector", "[flash][delta]") {
    // 0x1800 bytes — the last sector (0x1000..0x17FF) is short (0x800).
    std::vector<std::uint8_t> current(0x1800, 0xFF);
    std::vector<std::uint8_t> target = current;
    target[0x1700] = 0x00; // change in the short sector
    auto const sectors = flash::Flasher::compute_delta(current, target, 0x1000);
    REQUIRE(sectors.size() == 1);
    REQUIRE(sectors[0].address == 0x1000);
    REQUIRE(sectors[0].length == 0x800);
}

// ---------------------------------------------------------------------
// execute — validation
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute rejects a write whose data.size() != length",
          "[flash][execute][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    flash::FlashPlan plan;
    plan.writes.push_back({{0x00001000, 4}, {0xDE, 0xAD, 0xBE}}); // 3 != 4
    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error->code() == st::ErrorCode::InvalidArgument);
    // No I/O attempted.
    REQUIRE(t.send_log().empty());
    // Even on validation failure, the report comes back populated.
    REQUIRE_FALSE(r.report.entered_session);
}

// ---------------------------------------------------------------------
// execute — dry run
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute dry_run exercises session + CC only", "[flash][execute][dry-run]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // 1. DSC programming
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    // 2. CC off (disableRxAndTx, normal+nm)
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    // 3. (NO per-sector exchanges in dry_run)
    // 4. CC on (enableRxAndTx, normal+nm)
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.dry_run = true;
    plan.writes.push_back({{0x00001234, 4}, {0xDE, 0xAD, 0xBE, 0xEF}});

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE(r.ok());
    REQUIRE(r.report.entered_session);
    REQUIRE(r.report.silenced_bus);
    REQUIRE(r.report.restored_bus);
    REQUIRE(r.report.bytes_transferred == 0);
    REQUIRE(r.report.sectors.size() == 1);
    // Every per-sector step is false because we skipped them.
    REQUIRE_FALSE(r.report.sectors[0].erased);
    REQUIRE_FALSE(r.report.sectors[0].transferred);
    REQUIRE(t.exhausted());
}

// ---------------------------------------------------------------------
// execute — full happy path
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute full single-sector flash with verify", "[flash][execute][happy-path]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    // 1. DSC programming
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    // 2. CC off
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});

    // 3a. eraseMemory routine
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr, size)),
           {0x71, 0x01, 0xFF, 0x00});

    // 3b. RequestDownload — ECU reports max_block_length = 6 (payload 4).
    //     aLFI for addr 0x1234 (2 bytes) + size 4 (1 byte) = 0x12.
    expect(t, uds::build_request_download(0x00, addr, size), {0x74, 0x20, 0x00, 0x06});

    // 3c. TransferData(counter=1, data) — fits in one block.
    expect(t, uds::build_transfer_data(1, data), {0x76, 0x01});

    // 3d. RequestTransferExit
    expect(t, uds::build_request_transfer_exit(), {0x77});

    // 3e. checkProgrammingDependencies
    expect(t, uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
           {0x71, 0x01, 0xFF, 0x01});

    // 3f. Verify pass: read back 4 bytes; the response carries the
    //     bytes we just (notionally) wrote.
    expect(t, uds::build_read_memory_by_address(addr, size), {0x63, 0xDE, 0xAD, 0xBE, 0xEF});

    // 4. CC on
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});
    plan.block_size_hint = 0; // use ECU's reported maximum
    plan.verify_chunk_size = 0x100;

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE(r.ok());
    REQUIRE(r.report.entered_session);
    REQUIRE(r.report.silenced_bus);
    REQUIRE(r.report.restored_bus);
    REQUIRE(r.report.sectors.size() == 1);
    auto const &so = r.report.sectors[0];
    REQUIRE(so.erased);
    REQUIRE(so.downloaded);
    REQUIRE(so.transferred);
    REQUIRE(so.exited);
    REQUIRE(so.check_deps_passed);
    REQUIRE(so.verified);
    REQUIRE(r.report.bytes_transferred == size);
    REQUIRE(r.report.all_sectors_completed());
    REQUIRE(r.report.all_sectors_verified());
    REQUIRE(t.exhausted());
}

// ---------------------------------------------------------------------
// execute — NRC propagation
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute surfaces an NRC on RequestDownload", "[flash][execute][error][nrc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr, size)),
           {0x71, 0x01, 0xFF, 0x00});
    // RequestDownload denied (securityAccessDenied = 0x33).
    expect(t, uds::build_request_download(0x00, addr, size), {0x7F, 0x34, 0x33});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error->code() == st::ErrorCode::EcuRejected);
    // Partial-failure report visibility: erase succeeded for this sector
    // even though the download was denied. That's exactly the case the
    // new ExecuteOutcome shape was meant to expose.
    REQUIRE(r.report.sectors.size() == 1);
    REQUIRE(r.report.sectors[0].erased);
    REQUIRE_FALSE(r.report.sectors[0].downloaded);
}

// ---------------------------------------------------------------------
// Plan TOML I/O
// ---------------------------------------------------------------------

TEST_CASE("FlashPlan round-trips through format_plan + parse_plan", "[flash][plan][toml]") {
    flash::FlashPlan p;
    p.session = 0x03;
    p.data_format = 0x10;
    p.silence_bus = false;
    p.verify_after_write = false;
    p.dry_run = true;
    p.block_size_hint = 0x80;
    p.verify_chunk_size = 0x200;
    p.writes.push_back({{0x00001234, 4}, {0xDE, 0xAD, 0xBE, 0xEF}});
    p.writes.push_back({{0x00010000, 2}, {0xAA, 0x55}});

    auto const text = flash::format_plan(p);
    auto const r = flash::parse_plan(text);
    REQUIRE(r.has_value());

    REQUIRE(r->session == p.session);
    REQUIRE(r->data_format == p.data_format);
    REQUIRE(r->silence_bus == p.silence_bus);
    REQUIRE(r->verify_after_write == p.verify_after_write);
    REQUIRE(r->dry_run == p.dry_run);
    REQUIRE(r->block_size_hint == p.block_size_hint);
    REQUIRE(r->verify_chunk_size == p.verify_chunk_size);
    REQUIRE(r->writes.size() == 2);
    REQUIRE(r->writes[0].sector == p.writes[0].sector);
    REQUIRE(r->writes[0].data == p.writes[0].data);
    REQUIRE(r->writes[1].sector == p.writes[1].sector);
    REQUIRE(r->writes[1].data == p.writes[1].data);
}

TEST_CASE("parse_plan accepts whitespace and 0x prefixes in data", "[flash][plan][toml]") {
    // Basic TOML strings can't span lines; we wrap the multi-line hex
    // payload in a triple-quoted literal so the parser receives the raw
    // (embedded-newline-allowed) text and parse_hex_bytes ignores the
    // whitespace inside.
    constexpr std::string_view text = "[plan]\n"
                                      "schema_version = 1\n"
                                      "[[write]]\n"
                                      "address = 0x100\n"
                                      "data    = \"\"\"  0xDE 0xAD\n0xBE 0xEF  \"\"\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector.length == 4);
    REQUIRE(r->writes[0].data == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_plan rejects missing [plan]", "[flash][plan][toml][error]") {
    constexpr std::string_view text = "[[write]]\naddress=0\ndata=\"00\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan rejects an unsupported schema_version", "[flash][plan][toml][error]") {
    constexpr std::string_view text =
        "[plan]\nschema_version = 999\n[[write]]\naddress=0\ndata=\"00\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("parse_plan rejects a plan with no writes", "[flash][plan][toml][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan rejects malformed hex in data", "[flash][plan][toml][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n"
                                      "[[write]]\naddress=0\ndata=\"XX YY\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("read_plan + write_plan round-trip via the filesystem", "[flash][plan][toml][file]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     ("st_flash_plan_" + std::to_string(std::random_device{}()) + ".toml");

    flash::FlashPlan p;
    p.session = 0x02;
    p.writes.push_back({{0xABCD, 3}, {0x01, 0x02, 0x03}});

    REQUIRE(flash::write_plan(tmp, p).has_value());
    auto const r = flash::read_plan(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector.address == 0xABCD);
    REQUIRE(r->writes[0].data == std::vector<std::uint8_t>{0x01, 0x02, 0x03});

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("read_plan errors clearly when the file does not exist",
          "[flash][plan][toml][file][error]") {
    auto const r = flash::read_plan("/no/such/path/should/not.exist.plan.toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// Plan TOML — data_file
// ---------------------------------------------------------------------

namespace {

// Build a temp directory and write a binary payload to it; returns the
// path. Caller removes after use.
std::filesystem::path write_temp_binary(std::vector<std::uint8_t> const &bytes, char const *name) {
    auto const p = std::filesystem::temp_directory_path() /
                   ("st_flash_df_" + std::to_string(std::random_device{}()) + "_" + name);
    std::ofstream out{p, std::ios::binary};
    out.write(reinterpret_cast<char const *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return p;
}

} // namespace

TEST_CASE("parse_plan loads bytes via data_file (absolute path)",
          "[flash][plan][toml][data_file]") {
    auto const bin = write_temp_binary({0xCA, 0xFE, 0xF0, 0x0D}, "abs.bin");
    std::string text = "[plan]\nschema_version = 1\n\n[[write]]\naddress = 0x4000\n"
                       "data_file = \"" +
                       bin.generic_string() + "\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector.length == 4);
    REQUIRE(r->writes[0].data == std::vector<std::uint8_t>{0xCA, 0xFE, 0xF0, 0x0D});
    std::error_code ec;
    std::filesystem::remove(bin, ec);
}

TEST_CASE("read_plan resolves a relative data_file against the plan dir",
          "[flash][plan][toml][data_file]") {
    // Make a temp dir, drop a binary AND a plan TOML referencing it
    // relatively, then read_plan and check the bytes loaded.
    auto const dir = std::filesystem::temp_directory_path() /
                     ("st_flash_dfdir_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    auto const bin_name = "payload.bin";
    auto const bin_path = dir / bin_name;
    std::ofstream out{bin_path, std::ios::binary};
    out.put(static_cast<char>(0x11));
    out.put(static_cast<char>(0x22));
    out.put(static_cast<char>(0x33));
    out.close();
    auto const plan_path = dir / "p.toml";
    std::ofstream pout{plan_path};
    pout << "[plan]\nschema_version = 1\n"
            "[[write]]\naddress = 0x100\n"
            "data_file = \"payload.bin\"\n";
    pout.close();
    auto const r = flash::read_plan(plan_path);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].data == std::vector<std::uint8_t>{0x11, 0x22, 0x33});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("parse_plan rejects relative data_file without base_dir",
          "[flash][plan][toml][data_file][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n"
                                      "[[write]]\naddress = 0\ndata_file = \"payload.bin\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_plan rejects both data and data_file present",
          "[flash][plan][toml][data_file][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n"
                                      "[[write]]\naddress = 0\ndata = \"AA\"\n"
                                      "data_file = \"x.bin\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan rejects neither data nor data_file present",
          "[flash][plan][toml][data_file][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n[[write]]\naddress = 0\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan surfaces a missing data_file with FileNotFound",
          "[flash][plan][toml][data_file][error]") {
    // Use an explicit base_dir so the relative path is resolved
    // (sidestepping the platform-specific "is /foo absolute?" question)
    // and we exercise the actual file-not-found path on disk.
    constexpr std::string_view text = "[plan]\nschema_version = 1\n"
                                      "[[write]]\naddress = 0\n"
                                      "data_file = \"definitely-not-on-disk.bin\"\n";
    auto const r = flash::parse_plan(text, std::filesystem::temp_directory_path());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------

namespace {

flash::FlashReport make_report_for(flash::SectorWrite const &w, bool transferred = true,
                                   bool verified = true) {
    flash::FlashReport r;
    r.entered_session = true;
    r.silenced_bus = true;
    r.restored_bus = true;
    flash::SectorOutcome so{};
    so.sector = w.sector;
    so.erased = true;
    so.downloaded = true;
    so.transferred = transferred;
    so.exited = true;
    so.check_deps_passed = true;
    so.verified = verified;
    r.sectors.push_back(so);
    r.bytes_transferred = transferred ? w.sector.length : 0;
    return r;
}

} // namespace

TEST_CASE("build_manifest fills per-sector and overall CRC32s", "[flash][manifest]") {
    flash::FlashPlan plan;
    plan.writes.push_back({{0x1000, 4}, {0xDE, 0xAD, 0xBE, 0xEF}});

    std::string_view const plan_text = "[plan]\nschema_version = 1\n";

    auto const report = make_report_for(plan.writes[0]);
    auto const m = flash::build_manifest(plan, plan_text, report);

    REQUIRE(m.schema_version == flash::kManifestSchemaVersion);
    REQUIRE_FALSE(m.created_at.empty());
    REQUIRE(m.entries.size() == 1);
    REQUIRE(m.entries[0].sector == plan.writes[0].sector);
    REQUIRE(m.entries[0].transferred);
    REQUIRE(m.entries[0].verified);
    // overall_crc32 over a single transferred entry equals that entry's CRC.
    REQUIRE(m.overall_crc32 == m.entries[0].data_crc32);
    REQUIRE(m.plan_crc32 != 0);
}

TEST_CASE("build_manifest excludes non-transferred entries from overall_crc32",
          "[flash][manifest]") {
    flash::FlashPlan plan;
    plan.writes.push_back({{0x1000, 4}, {0xAA, 0xAA, 0xAA, 0xAA}});
    plan.writes.push_back({{0x2000, 4}, {0xBB, 0xBB, 0xBB, 0xBB}});

    flash::FlashReport r;
    r.sectors.push_back(
        {plan.writes[0].sector, true, true, /*transferred=*/true, true, true, true});
    r.sectors.push_back(
        {plan.writes[1].sector, true, true, /*transferred=*/false, false, false, false});

    auto const m = flash::build_manifest(plan, "", r);
    REQUIRE(m.entries.size() == 2);
    REQUIRE(m.entries[0].transferred);
    REQUIRE_FALSE(m.entries[1].transferred);

    // Same plan, but the second sector is also not transferred — overall
    // CRC32 should match because only the first entry's bytes feed it.
    flash::FlashReport r2;
    r2.sectors.push_back(r.sectors[0]);
    r2.sectors.push_back({plan.writes[1].sector});
    auto const m2 = flash::build_manifest(plan, "", r2);
    REQUIRE(m.overall_crc32 == m2.overall_crc32);
}

TEST_CASE("Manifest round-trips through format_manifest + parse_manifest",
          "[flash][manifest][toml]") {
    flash::Manifest m;
    m.schema_version = flash::kManifestSchemaVersion;
    m.created_at = "2026-05-12T15:30:00Z";
    m.plan_crc32 = 0xDEADBEEF;
    m.overall_crc32 = 0xCAFEF00D;
    m.entries.push_back({{0x1000, 4}, 0x11223344, true, true});
    m.entries.push_back({{0x2000, 0x100}, 0x55667788, true, false});

    auto const text = flash::format_manifest(m);
    auto const r = flash::parse_manifest(text);
    REQUIRE(r.has_value());
    REQUIRE(r->schema_version == m.schema_version);
    REQUIRE(r->created_at == m.created_at);
    REQUIRE(r->plan_crc32 == m.plan_crc32);
    REQUIRE(r->overall_crc32 == m.overall_crc32);
    REQUIRE(r->entries.size() == 2);
    REQUIRE(r->entries[0].sector == m.entries[0].sector);
    REQUIRE(r->entries[0].data_crc32 == m.entries[0].data_crc32);
    REQUIRE(r->entries[0].transferred == m.entries[0].transferred);
    REQUIRE(r->entries[0].verified == m.entries[0].verified);
    REQUIRE(r->entries[1].sector == m.entries[1].sector);
    REQUIRE(r->entries[1].verified == m.entries[1].verified);
}

TEST_CASE("Manifest round-trips policy_profile + policy_reason", "[flash][manifest][policy]") {
    flash::Manifest m;
    m.created_at = "2026-05-12T15:30:00Z";
    m.plan_crc32 = 0xAA;
    m.overall_crc32 = 0xBB;
    m.policy_profile = "california-us";
    m.policy_reason = "Test cell run #42 — verified on dyno";
    m.entries.push_back({{0x1000, 4}, 0x11223344, true, true});

    auto const text = flash::format_manifest(m);
    auto const r = flash::parse_manifest(text);
    REQUIRE(r.has_value());
    REQUIRE(r->policy_profile == m.policy_profile);
    REQUIRE(r->policy_reason == m.policy_reason);
}

TEST_CASE("Manifest omits empty policy fields from the emitted TOML", "[flash][manifest][policy]") {
    flash::Manifest m;
    m.created_at = "2026-05-12T15:30:00Z";
    // policy_profile + policy_reason both empty: no gate was applied.
    auto const text = flash::format_manifest(m);
    REQUIRE(text.find("policy_profile") == std::string::npos);
    REQUIRE(text.find("policy_reason") == std::string::npos);
}

TEST_CASE("Manifest escapes quotes + backslashes in policy_reason", "[flash][manifest][policy]") {
    flash::Manifest m;
    m.policy_profile = "california-us";
    m.policy_reason = R"(Run "A/B" \ trial #3)";
    auto const text = flash::format_manifest(m);
    auto const r = flash::parse_manifest(text);
    REQUIRE(r.has_value());
    REQUIRE(r->policy_reason == m.policy_reason);
}

TEST_CASE("parse_manifest rejects an unsupported schema_version",
          "[flash][manifest][toml][error]") {
    constexpr std::string_view text = "schema_version = 999\n";
    auto const r = flash::parse_manifest(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("read_manifest + write_manifest round-trip via the filesystem",
          "[flash][manifest][toml][file]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     ("st_flash_manifest_" + std::to_string(std::random_device{}()) + ".toml");

    flash::Manifest m;
    m.created_at = "2026-05-12T00:00:00Z";
    m.plan_crc32 = 0x12345678;
    m.overall_crc32 = 0x87654321;
    m.entries.push_back({{0xABCD, 16}, 0xABCDEF01, true, true});

    REQUIRE(flash::write_manifest(tmp, m).has_value());
    auto const r = flash::read_manifest(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size() == 1);
    REQUIRE(r->entries[0].data_crc32 == 0xABCDEF01);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("read_manifest errors clearly when the file does not exist",
          "[flash][manifest][toml][file][error]") {
    auto const r = flash::read_manifest("/no/such/path/should/not.exist.manifest.toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// execute — incremental journal
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute writes a journal manifest on every sector",
          "[flash][execute][journal]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    // Full happy-path expected exchanges (mirrors the happy-path test).
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr, size)),
           {0x71, 0x01, 0xFF, 0x00});
    expect(t, uds::build_request_download(0x00, addr, size), {0x74, 0x20, 0x00, 0x06});
    expect(t, uds::build_transfer_data(1, data), {0x76, 0x01});
    expect(t, uds::build_request_transfer_exit(), {0x77});
    expect(t, uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
           {0x71, 0x01, 0xFF, 0x01});
    expect(t, uds::build_read_memory_by_address(addr, size), {0x63, 0xDE, 0xAD, 0xBE, 0xEF});
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    auto const journal = std::filesystem::temp_directory_path() /
                         ("st_flash_journal_" + std::to_string(std::random_device{}()) + ".toml");

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});
    plan.journal_path = journal;

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE(r.ok());

    // After execute returns, the journal file exists and reflects the
    // final state.
    REQUIRE(std::filesystem::exists(journal));
    auto const loaded = flash::read_manifest(journal);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 1);
    REQUIRE(loaded->entries[0].sector.address == addr);
    REQUIRE(loaded->entries[0].transferred);
    REQUIRE(loaded->entries[0].verified);

    std::error_code ec;
    std::filesystem::remove(journal, ec);
}

TEST_CASE("Flasher::execute journal captures partial mid-flash failure",
          "[flash][execute][journal]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr1 = 0x00001234;
    constexpr std::uint32_t addr2 = 0x00005678;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data1{0xAA, 0xAA, 0xAA, 0xAA};
    std::vector<std::uint8_t> const data2{0xBB, 0xBB, 0xBB, 0xBB};

    // Sector 1: happy path through verify.
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr1, size)),
           {0x71, 0x01, 0xFF, 0x00});
    expect(t, uds::build_request_download(0x00, addr1, size), {0x74, 0x20, 0x00, 0x06});
    expect(t, uds::build_transfer_data(1, data1), {0x76, 0x01});
    expect(t, uds::build_request_transfer_exit(), {0x77});
    expect(t, uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
           {0x71, 0x01, 0xFF, 0x01});
    expect(t, uds::build_read_memory_by_address(addr1, size), {0x63, 0xAA, 0xAA, 0xAA, 0xAA});

    // Sector 2: eraseMemory fails (conditions not correct = 0x22).
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr2, size)),
           {0x7F, 0x31, 0x22});

    auto const journal =
        std::filesystem::temp_directory_path() /
        ("st_flash_journal_fail_" + std::to_string(std::random_device{}()) + ".toml");

    flash::FlashPlan plan;
    plan.writes.push_back({{addr1, size}, data1});
    plan.writes.push_back({{addr2, size}, data2});
    plan.journal_path = journal;
    // No CC-restore exchange queued because execute returns before CC.

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE_FALSE(r.ok());
    // In-memory partial-state visibility matches the on-disk journal.
    REQUIRE(r.report.sectors.size() == 2);
    REQUIRE(r.report.sectors[0].transferred);
    REQUIRE_FALSE(r.report.sectors[1].transferred);

    // The journal on disk reflects: sector 1 fully transferred, sector
    // 2 with transferred=false (erase failed before any data moved).
    REQUIRE(std::filesystem::exists(journal));
    auto const loaded = flash::read_manifest(journal);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);
    REQUIRE(loaded->entries[0].sector.address == addr1);
    REQUIRE(loaded->entries[0].transferred);
    REQUIRE(loaded->entries[1].sector.address == addr2);
    REQUIRE_FALSE(loaded->entries[1].transferred);

    std::error_code ec;
    std::filesystem::remove(journal, ec);
}

TEST_CASE("Flasher::execute with no journal_path makes no journal file",
          "[flash][execute][journal]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.dry_run = true;
    plan.writes.push_back({{0x100, 4}, {0, 0, 0, 0}});

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE(r.ok());
    // journal_path is empty by default; nothing written. Nothing to assert
    // about the filesystem here, but the test confirms execute() works
    // through dry-run without trying to open an empty path.
    REQUIRE(t.exhausted());
}

// ---------------------------------------------------------------------
// plan_resume
// ---------------------------------------------------------------------

#include "st/core/crc32.hpp"

namespace {

flash::FlashPlan two_sector_plan() {
    flash::FlashPlan p;
    p.session = 0x03;             // distinguish from defaults
    p.verify_after_write = false; // pin a non-default flag
    p.writes.push_back({{0x1000, 4}, {0xAA, 0xAA, 0xAA, 0xAA}});
    p.writes.push_back({{0x2000, 4}, {0xBB, 0xBB, 0xBB, 0xBB}});
    return p;
}

flash::ManifestEntry done_entry(flash::SectorWrite const &w) {
    flash::ManifestEntry e;
    e.sector = w.sector;
    e.data_crc32 = st::crc32(w.data);
    e.transferred = true;
    e.verified = true;
    return e;
}

flash::ManifestEntry partial_entry(flash::SectorWrite const &w) {
    flash::ManifestEntry e;
    e.sector = w.sector;
    e.data_crc32 = st::crc32(w.data);
    e.transferred = false;
    e.verified = false;
    return e;
}

} // namespace

TEST_CASE("plan_resume with all-done journal returns an empty plan", "[flash][resume]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    journal.entries.push_back(done_entry(original.writes[0]));
    journal.entries.push_back(done_entry(original.writes[1]));

    auto const r = flash::plan_resume(original, journal);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.empty());
    // Options carry over.
    REQUIRE(r->session == original.session);
    REQUIRE(r->verify_after_write == original.verify_after_write);
}

TEST_CASE("plan_resume includes sectors that didn't fully verify", "[flash][resume]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    journal.entries.push_back(done_entry(original.writes[0]));
    journal.entries.push_back(partial_entry(original.writes[1]));

    auto const r = flash::plan_resume(original, journal);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector == original.writes[1].sector);
    REQUIRE(r->writes[0].data == original.writes[1].data);
}

TEST_CASE("plan_resume with empty journal returns the original plan", "[flash][resume]") {
    auto const original = two_sector_plan();
    flash::Manifest journal; // no entries

    auto const r = flash::plan_resume(original, journal);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == original.writes.size());
    REQUIRE(r->writes[0].sector == original.writes[0].sector);
    REQUIRE(r->writes[1].sector == original.writes[1].sector);
}

TEST_CASE("plan_resume covers sectors past the end of the journal", "[flash][resume]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    // Only the first sector has a journal entry — the host crashed
    // before sector 2 was even started.
    journal.entries.push_back(done_entry(original.writes[0]));

    auto const r = flash::plan_resume(original, journal);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector == original.writes[1].sector);
}

TEST_CASE("plan_resume rejects journal/plan CRC mismatch", "[flash][resume][error]") {
    auto original = two_sector_plan();
    flash::Manifest journal;
    journal.entries.push_back(done_entry(original.writes[0]));
    journal.entries.push_back(done_entry(original.writes[1]));

    // Mutate the original plan AFTER building the journal: simulates a
    // user editing the plan between flash attempts.
    original.writes[0].data[0] = 0x55;

    auto const r = flash::plan_resume(original, journal);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::BadChecksum);
}

TEST_CASE("plan_resume rejects sector address mismatch at the same index",
          "[flash][resume][error]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    // Manufacture a journal entry whose address doesn't match the plan.
    flash::ManifestEntry bogus = done_entry(original.writes[0]);
    bogus.sector.address = 0xDEAD;
    journal.entries.push_back(bogus);

    auto const r = flash::plan_resume(original, journal);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("plan_resume rejects an oversize journal", "[flash][resume][error]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    journal.entries.push_back(done_entry(original.writes[0]));
    journal.entries.push_back(done_entry(original.writes[1]));
    // Extra entry — journal claims more sectors than the plan has.
    flash::ManifestEntry extra;
    extra.sector = {0x9999, 4};
    journal.entries.push_back(extra);

    auto const r = flash::plan_resume(original, journal);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

// ---------------------------------------------------------------------
// execute — verify mismatch
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute reports verify_after_write mismatch", "[flash][execute][verify]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t,
           uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr, size)),
           {0x71, 0x01, 0xFF, 0x00});
    expect(t, uds::build_request_download(0x00, addr, size), {0x74, 0x20, 0x00, 0x06});
    expect(t, uds::build_transfer_data(1, data), {0x76, 0x01});
    expect(t, uds::build_request_transfer_exit(), {0x77});
    expect(t, uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
           {0x71, 0x01, 0xFF, 0x01});
    // Verify read-back returns DIFFERENT bytes than what was written.
    expect(t, uds::build_read_memory_by_address(addr, size), {0x63, 0x00, 0x00, 0x00, 0x00});
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});

    flash::Flasher f{t};
    auto const r = f.execute(plan);
    REQUIRE(r.ok());
    REQUIRE(r.report.sectors.size() == 1);
    REQUIRE(r.report.sectors[0].transferred);
    REQUIRE(r.report.sectors[0].check_deps_passed);
    REQUIRE_FALSE(r.report.sectors[0].verified);
    REQUIRE_FALSE(r.report.all_sectors_verified());
    REQUIRE(t.exhausted());
}

// ---- evaluate_plan_policy ------------------------------------------------

namespace {

// Build a minimal Definition with two tables: one engine-safety-critical,
// one emissions-relevant. Both 1-byte scalars at distinct addresses so we
// can compose plans that touch either, both, or neither.
st::Definition make_two_table_def() {
    auto def = st::Definition::from_toml_string(R"toml(
[pack]
id             = "policy-test"
endianness     = "big"
rom_size_bytes = 256

[[scaling]]
id        = "raw"
formula   = "linear"
factor    = 1.0
data_type = "uint8"

[[table]]
id                     = "rev_limit"
dimensions             = 0
address                = 0x10
data_type              = "uint8"
scaling                = "raw"
engine_safety_critical = true

[[table]]
id                     = "cl_target"
dimensions             = 0
address                = 0x20
data_type              = "uint8"
scaling                = "raw"
emissions_relevant     = true

[[table]]
id                     = "boost_target"
dimensions             = 0
address                = 0x30
data_type              = "uint8"
scaling                = "raw"
)toml");
    REQUIRE(def.has_value());
    return std::move(*def);
}

// A 256-byte source ROM filled with a known background pattern.
std::vector<std::uint8_t> make_source_rom() {
    std::vector<std::uint8_t> rom(256, 0x00);
    rom[0x10] = 0x50; // rev_limit baseline
    rom[0x20] = 0x80; // cl_target  baseline
    rom[0x30] = 0xA0; // boost_target baseline
    return rom;
}

// Build a one-sector plan that overwrites `[start, start + bytes.size())`.
flash::FlashPlan plan_for(std::uint32_t start, std::vector<std::uint8_t> bytes) {
    flash::FlashPlan p;
    p.writes.push_back({{start, static_cast<std::uint32_t>(bytes.size())}, std::move(bytes)});
    return p;
}

} // namespace

TEST_CASE("evaluate_plan_policy: no flagged tables -> Silent regardless of profile",
          "[flash][policy]") {
    auto const def = make_two_table_def();
    auto const src = make_source_rom();
    // Plan changes byte 0x30 only — boost_target, neither flag.
    auto const plan = plan_for(0x30, {0xB0});
    for (auto p : {st::policy::Profile::MotorsportOnly, st::policy::Profile::AlbertaCa,
                   st::policy::Profile::CaliforniaUs}) {
        auto const d = flash::evaluate_plan_policy(plan, def, src, p);
        REQUIRE(d.engine_safety_tables.empty());
        REQUIRE(d.emissions_tables.empty());
        REQUIRE(d.overall_action == st::policy::Action::Silent);
    }
}

TEST_CASE("evaluate_plan_policy: emissions-flagged table changes -> profile action",
          "[flash][policy]") {
    auto const def = make_two_table_def();
    auto const src = make_source_rom();
    auto const plan = plan_for(0x20, {0x90}); // cl_target: 0x80 -> 0x90

    auto const motorsport =
        flash::evaluate_plan_policy(plan, def, src, st::policy::Profile::MotorsportOnly);
    REQUIRE(motorsport.emissions_tables == std::vector<std::string>{"cl_target"});
    REQUIRE(motorsport.engine_safety_tables.empty());
    REQUIRE(motorsport.overall_action == st::policy::Action::Silent);

    auto const calif =
        flash::evaluate_plan_policy(plan, def, src, st::policy::Profile::CaliforniaUs);
    REQUIRE(calif.emissions_tables == std::vector<std::string>{"cl_target"});
    REQUIRE(calif.overall_action == st::policy::Action::ConfirmWithReason);
}

TEST_CASE("evaluate_plan_policy: engine-safety table -> Block every profile", "[flash][policy]") {
    auto const def = make_two_table_def();
    auto const src = make_source_rom();
    // Plan changes BOTH the safety-critical rev_limit AND the emissions
    // cl_target. Safety wins -> Block, regardless of profile.
    flash::FlashPlan plan;
    plan.writes.push_back({{0x10, 1}, {0x55}}); // rev_limit changed
    plan.writes.push_back({{0x20, 1}, {0x90}}); // cl_target changed

    for (auto p : {st::policy::Profile::MotorsportOnly, st::policy::Profile::AlbertaCa,
                   st::policy::Profile::EuRoadworthy, st::policy::Profile::CaliforniaUs}) {
        auto const d = flash::evaluate_plan_policy(plan, def, src, p);
        REQUIRE(d.engine_safety_tables == std::vector<std::string>{"rev_limit"});
        REQUIRE(d.emissions_tables == std::vector<std::string>{"cl_target"});
        REQUIRE(d.overall_action == st::policy::Action::Block);
    }
}

TEST_CASE("evaluate_plan_policy: writing identical bytes is not a change", "[flash][policy]") {
    // A plan that writes the same bytes already in source should NOT trip
    // the linter — there's no actual edit happening.
    auto const def = make_two_table_def();
    auto const src = make_source_rom();
    auto const plan = plan_for(0x20, {0x80}); // cl_target's existing value
    auto const d = flash::evaluate_plan_policy(plan, def, src, st::policy::Profile::CaliforniaUs);
    REQUIRE(d.emissions_tables.empty());
    REQUIRE(d.engine_safety_tables.empty());
    REQUIRE(d.overall_action == st::policy::Action::Silent);
}

TEST_CASE("evaluate_plan_policy: sector that overlaps multiple tables flags each one",
          "[flash][policy]") {
    auto const def = make_two_table_def();
    auto const src = make_source_rom();
    // One sector spans 0x10..0x30 inclusive and changes a byte in each
    // of the three tables. Both flagged tables should appear.
    std::vector<std::uint8_t> bytes(0x21, 0x00);
    bytes[0x10 - 0x10] = 0x55; // rev_limit
    bytes[0x20 - 0x10] = 0x90; // cl_target
    bytes[0x30 - 0x10] = 0xB0; // boost_target (unflagged)
    flash::FlashPlan plan;
    plan.writes.push_back({{0x10, static_cast<std::uint32_t>(bytes.size())}, std::move(bytes)});

    auto const d = flash::evaluate_plan_policy(plan, def, src, st::policy::Profile::EuRoadworthy);
    REQUIRE(d.engine_safety_tables == std::vector<std::string>{"rev_limit"});
    REQUIRE(d.emissions_tables == std::vector<std::string>{"cl_target"});
    REQUIRE(d.overall_action == st::policy::Action::Block);
}
