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
#include <random>
#include <string_view>
#include <system_error>
#include <vector>

namespace flash = st::flash;
namespace uds   = st::ecu::uds;

namespace {

// Convenience: queue a (request, response) pair from a uds::build_* call
// and a hand-written response.
void expect(st::transport::MockTransport &t,
            std::vector<std::uint8_t>     req,
            std::vector<std::uint8_t>     resp) {
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

TEST_CASE("Flasher::read_full_rom concatenates chunked ReadMemoryByAddress",
          "[flash][read]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // 16 bytes from 0x00001000, split into 2 chunks of 8.
    expect(t, uds::build_read_memory_by_address(0x00001000, 8),
              {0x63, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
    expect(t, uds::build_read_memory_by_address(0x00001008, 8),
              {0x63, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F});

    flash::Flasher    f{t};
    auto const        r = f.read_full_rom(0x00001000, 16, /*max_chunk=*/8);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 16);
    for (std::size_t i = 0; i < r->size(); ++i) {
        REQUIRE((*r)[i] == i);
    }
    REQUIRE(t.exhausted());
}

TEST_CASE("Flasher::read_full_rom with zero length returns empty without I/O",
          "[flash][read]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    flash::Flasher    f{t};
    auto const        r = f.read_full_rom(0x1000, 0);
    REQUIRE(r.has_value());
    REQUIRE(r->empty());
    REQUIRE(t.send_log().empty());
}

TEST_CASE("Flasher::read_full_rom rejects zero max_chunk_size",
          "[flash][read][error]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    flash::Flasher    f{t};
    auto const        r = f.read_full_rom(0x1000, 16, /*max_chunk=*/0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------
// compute_delta
// ---------------------------------------------------------------------

TEST_CASE("Flasher::compute_delta finds the sectors that differ",
          "[flash][delta]") {
    std::vector<std::uint8_t> current(0x4000, 0xAA);
    std::vector<std::uint8_t> target = current;
    target[0x0500] = 0x55;  // changes sector 0 (0x0000..0x0FFF)
    target[0x2800] = 0x55;  // changes sector 2 (0x2000..0x2FFF)

    auto const sectors = flash::Flasher::compute_delta(
        current, target, /*sector_size=*/0x1000, /*base=*/0x10000);
    REQUIRE(sectors.size() == 2);
    REQUIRE(sectors[0] == flash::Sector{0x10000, 0x1000});
    REQUIRE(sectors[1] == flash::Sector{0x12000, 0x1000});
}

TEST_CASE("Flasher::compute_delta returns empty for identical buffers",
          "[flash][delta]") {
    std::vector<std::uint8_t> a(0x2000, 0x00);
    auto const sectors = flash::Flasher::compute_delta(a, a, 0x1000);
    REQUIRE(sectors.empty());
}

TEST_CASE("Flasher::compute_delta handles a final short sector",
          "[flash][delta]") {
    // 0x1800 bytes — the last sector (0x1000..0x17FF) is short (0x800).
    std::vector<std::uint8_t> current(0x1800, 0xFF);
    std::vector<std::uint8_t> target = current;
    target[0x1700] = 0x00;  // change in the short sector
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
    plan.writes.push_back({{0x00001000, 4}, {0xDE, 0xAD, 0xBE}});  // 3 != 4
    flash::Flasher f{t};
    auto const     r = f.execute(plan);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    // No I/O attempted.
    REQUIRE(t.send_log().empty());
}

// ---------------------------------------------------------------------
// execute — dry run
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute dry_run exercises session + CC only",
          "[flash][execute][dry-run]") {
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
    auto const     r = f.execute(plan);
    REQUIRE(r.has_value());
    REQUIRE(r->entered_session);
    REQUIRE(r->silenced_bus);
    REQUIRE(r->restored_bus);
    REQUIRE(r->bytes_transferred == 0);
    REQUIRE(r->sectors.size() == 1);
    // Every per-sector step is false because we skipped them.
    REQUIRE_FALSE(r->sectors[0].erased);
    REQUIRE_FALSE(r->sectors[0].transferred);
    REQUIRE(t.exhausted());
}

// ---------------------------------------------------------------------
// execute — full happy path
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute full single-sector flash with verify",
          "[flash][execute][happy-path]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr    = 0x00001234;
    constexpr std::uint32_t size    = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    // 1. DSC programming
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    // 2. CC off
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});

    // 3a. eraseMemory routine
    expect(t, uds::build_routine_control(uds::kRcStart,
                                         uds::kRidEraseMemory,
                                         erase_opt(addr, size)),
              {0x71, 0x01, 0xFF, 0x00});

    // 3b. RequestDownload — ECU reports max_block_length = 6 (payload 4).
    //     aLFI for addr 0x1234 (2 bytes) + size 4 (1 byte) = 0x12.
    expect(t, uds::build_request_download(0x00, addr, size),
              {0x74, 0x20, 0x00, 0x06});

    // 3c. TransferData(counter=1, data) — fits in one block.
    expect(t, uds::build_transfer_data(1, data),
              {0x76, 0x01});

    // 3d. RequestTransferExit
    expect(t, uds::build_request_transfer_exit(), {0x77});

    // 3e. checkProgrammingDependencies
    expect(t, uds::build_routine_control(uds::kRcStart,
                                         uds::kRidCheckProgrammingDependencies),
              {0x71, 0x01, 0xFF, 0x01});

    // 3f. Verify pass: read back 4 bytes; the response carries the
    //     bytes we just (notionally) wrote.
    expect(t, uds::build_read_memory_by_address(addr, size),
              {0x63, 0xDE, 0xAD, 0xBE, 0xEF});

    // 4. CC on
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});
    plan.block_size_hint    = 0; // use ECU's reported maximum
    plan.verify_chunk_size  = 0x100;

    flash::Flasher f{t};
    auto const     r = f.execute(plan);
    REQUIRE(r.has_value());
    REQUIRE(r->entered_session);
    REQUIRE(r->silenced_bus);
    REQUIRE(r->restored_bus);
    REQUIRE(r->sectors.size() == 1);
    auto const &so = r->sectors[0];
    REQUIRE(so.erased);
    REQUIRE(so.downloaded);
    REQUIRE(so.transferred);
    REQUIRE(so.exited);
    REQUIRE(so.check_deps_passed);
    REQUIRE(so.verified);
    REQUIRE(r->bytes_transferred == size);
    REQUIRE(r->all_sectors_completed());
    REQUIRE(r->all_sectors_verified());
    REQUIRE(t.exhausted());
}

// ---------------------------------------------------------------------
// execute — NRC propagation
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute surfaces an NRC on RequestDownload",
          "[flash][execute][error][nrc]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t, uds::build_routine_control(uds::kRcStart,
                                         uds::kRidEraseMemory,
                                         erase_opt(addr, size)),
              {0x71, 0x01, 0xFF, 0x00});
    // RequestDownload denied (securityAccessDenied = 0x33).
    expect(t, uds::build_request_download(0x00, addr, size),
              {0x7F, 0x34, 0x33});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});

    flash::Flasher f{t};
    auto const     r = f.execute(plan);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

// ---------------------------------------------------------------------
// Plan TOML I/O
// ---------------------------------------------------------------------

TEST_CASE("FlashPlan round-trips through format_plan + parse_plan",
          "[flash][plan][toml]") {
    flash::FlashPlan p;
    p.session            = 0x03;
    p.data_format        = 0x10;
    p.silence_bus        = false;
    p.verify_after_write = false;
    p.dry_run            = true;
    p.block_size_hint    = 0x80;
    p.verify_chunk_size  = 0x200;
    p.writes.push_back({{0x00001234, 4}, {0xDE, 0xAD, 0xBE, 0xEF}});
    p.writes.push_back({{0x00010000, 2}, {0xAA, 0x55}});

    auto const text  = flash::format_plan(p);
    auto const r     = flash::parse_plan(text);
    REQUIRE(r.has_value());

    REQUIRE(r->session            == p.session);
    REQUIRE(r->data_format        == p.data_format);
    REQUIRE(r->silence_bus        == p.silence_bus);
    REQUIRE(r->verify_after_write == p.verify_after_write);
    REQUIRE(r->dry_run            == p.dry_run);
    REQUIRE(r->block_size_hint    == p.block_size_hint);
    REQUIRE(r->verify_chunk_size  == p.verify_chunk_size);
    REQUIRE(r->writes.size() == 2);
    REQUIRE(r->writes[0].sector == p.writes[0].sector);
    REQUIRE(r->writes[0].data   == p.writes[0].data);
    REQUIRE(r->writes[1].sector == p.writes[1].sector);
    REQUIRE(r->writes[1].data   == p.writes[1].data);
}

TEST_CASE("parse_plan accepts whitespace and 0x prefixes in data",
          "[flash][plan][toml]") {
    // Basic TOML strings can't span lines; we wrap the multi-line hex
    // payload in a triple-quoted literal so the parser receives the raw
    // (embedded-newline-allowed) text and parse_hex_bytes ignores the
    // whitespace inside.
    constexpr std::string_view text =
        "[plan]\n"
        "schema_version = 1\n"
        "[[write]]\n"
        "address = 0x100\n"
        "data    = \"\"\"  0xDE 0xAD\n0xBE 0xEF  \"\"\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE(r.has_value());
    REQUIRE(r->writes.size() == 1);
    REQUIRE(r->writes[0].sector.length == 4);
    REQUIRE(r->writes[0].data
            == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("parse_plan rejects missing [plan]", "[flash][plan][toml][error]") {
    constexpr std::string_view text = "[[write]]\naddress=0\ndata=\"00\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan rejects an unsupported schema_version",
          "[flash][plan][toml][error]") {
    constexpr std::string_view text =
        "[plan]\nschema_version = 999\n[[write]]\naddress=0\ndata=\"00\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("parse_plan rejects a plan with no writes",
          "[flash][plan][toml][error]") {
    constexpr std::string_view text = "[plan]\nschema_version = 1\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_plan rejects malformed hex in data",
          "[flash][plan][toml][error]") {
    constexpr std::string_view text =
        "[plan]\nschema_version = 1\n"
        "[[write]]\naddress=0\ndata=\"XX YY\"\n";
    auto const r = flash::parse_plan(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("read_plan + write_plan round-trip via the filesystem",
          "[flash][plan][toml][file]") {
    auto const tmp = std::filesystem::temp_directory_path()
                     / ("st_flash_plan_"
                        + std::to_string(std::random_device{}())
                        + ".toml");

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
// Manifest
// ---------------------------------------------------------------------

namespace {

flash::FlashReport make_report_for(flash::SectorWrite const &w,
                                    bool transferred = true,
                                    bool verified    = true) {
    flash::FlashReport r;
    r.entered_session   = true;
    r.silenced_bus      = true;
    r.restored_bus      = true;
    flash::SectorOutcome so{};
    so.sector            = w.sector;
    so.erased            = true;
    so.downloaded        = true;
    so.transferred       = transferred;
    so.exited            = true;
    so.check_deps_passed = true;
    so.verified          = verified;
    r.sectors.push_back(so);
    r.bytes_transferred  = transferred ? w.sector.length : 0;
    return r;
}

} // namespace

TEST_CASE("build_manifest fills per-sector and overall CRC32s",
          "[flash][manifest]") {
    flash::FlashPlan plan;
    plan.writes.push_back({{0x1000, 4}, {0xDE, 0xAD, 0xBE, 0xEF}});

    std::string_view const plan_text = "[plan]\nschema_version = 1\n";

    auto const report = make_report_for(plan.writes[0]);
    auto const m      = flash::build_manifest(plan, plan_text, report);

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
    r.sectors.push_back({plan.writes[0].sector,
                          true, true, /*transferred=*/true, true, true, true});
    r.sectors.push_back({plan.writes[1].sector,
                          true, true, /*transferred=*/false, false, false, false});

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
    m.created_at     = "2026-05-12T15:30:00Z";
    m.plan_crc32     = 0xDEADBEEF;
    m.overall_crc32  = 0xCAFEF00D;
    m.entries.push_back({{0x1000, 4},   0x11223344, true, true});
    m.entries.push_back({{0x2000, 0x100}, 0x55667788, true, false});

    auto const text = flash::format_manifest(m);
    auto const r    = flash::parse_manifest(text);
    REQUIRE(r.has_value());
    REQUIRE(r->schema_version == m.schema_version);
    REQUIRE(r->created_at     == m.created_at);
    REQUIRE(r->plan_crc32     == m.plan_crc32);
    REQUIRE(r->overall_crc32  == m.overall_crc32);
    REQUIRE(r->entries.size() == 2);
    REQUIRE(r->entries[0].sector      == m.entries[0].sector);
    REQUIRE(r->entries[0].data_crc32  == m.entries[0].data_crc32);
    REQUIRE(r->entries[0].transferred == m.entries[0].transferred);
    REQUIRE(r->entries[0].verified    == m.entries[0].verified);
    REQUIRE(r->entries[1].sector      == m.entries[1].sector);
    REQUIRE(r->entries[1].verified    == m.entries[1].verified);
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
    auto const tmp = std::filesystem::temp_directory_path()
                     / ("st_flash_manifest_"
                        + std::to_string(std::random_device{}())
                        + ".toml");

    flash::Manifest m;
    m.created_at    = "2026-05-12T00:00:00Z";
    m.plan_crc32    = 0x12345678;
    m.overall_crc32 = 0x87654321;
    m.entries.push_back({{0xABCD, 16}, 0xABCDEF01, true, true});

    REQUIRE(flash::write_manifest(tmp, m).has_value());
    auto const r = flash::read_manifest(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size()        == 1);
    REQUIRE(r->entries[0].data_crc32 == 0xABCDEF01);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("read_manifest errors clearly when the file does not exist",
          "[flash][manifest][toml][file][error]") {
    auto const r = flash::read_manifest(
        "/no/such/path/should/not.exist.manifest.toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// execute — verify mismatch
// ---------------------------------------------------------------------

TEST_CASE("Flasher::execute reports verify_after_write mismatch",
          "[flash][execute][verify]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001234;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data{0xDE, 0xAD, 0xBE, 0xEF};

    expect(t, {0x10, 0x02}, {0x50, 0x02});
    expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    expect(t, uds::build_routine_control(uds::kRcStart,
                                         uds::kRidEraseMemory,
                                         erase_opt(addr, size)),
              {0x71, 0x01, 0xFF, 0x00});
    expect(t, uds::build_request_download(0x00, addr, size),
              {0x74, 0x20, 0x00, 0x06});
    expect(t, uds::build_transfer_data(1, data), {0x76, 0x01});
    expect(t, uds::build_request_transfer_exit(), {0x77});
    expect(t, uds::build_routine_control(uds::kRcStart,
                                         uds::kRidCheckProgrammingDependencies),
              {0x71, 0x01, 0xFF, 0x01});
    // Verify read-back returns DIFFERENT bytes than what was written.
    expect(t, uds::build_read_memory_by_address(addr, size),
              {0x63, 0x00, 0x00, 0x00, 0x00});
    expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});

    flash::FlashPlan plan;
    plan.writes.push_back({{addr, size}, data});

    flash::Flasher f{t};
    auto const     r = f.execute(plan);
    REQUIRE(r.has_value());
    REQUIRE(r->sectors.size() == 1);
    REQUIRE(r->sectors[0].transferred);
    REQUIRE(r->sectors[0].check_deps_passed);
    REQUIRE_FALSE(r->sectors[0].verified);
    REQUIRE_FALSE(r->all_sectors_verified());
    REQUIRE(t.exhausted());
}
