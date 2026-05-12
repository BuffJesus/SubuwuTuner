// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
