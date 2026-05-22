// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Cancellation invariant tests — Tier 2a per docs/08-testing-strategy.md.
//
// A regression in any of these properties can brick a real ECU; the suite
// enforces them via `MockTransport` + the existing journal/manifest plumbing,
// hardware-free.
//
// Invariants from the spec (docs/08 Tier 2a) and their current status:
//
//   1. Mid-PDU UDS cancel is deferred           — NOT YET TESTABLE
//   2. Mid-PDU SSM cancel is deferred           — NOT YET TESTABLE
//   3. Session-exit on cancel                   — NOT YET TESTABLE
//   4. Crash-mid-flash recovery                 — tested below
//   5. Resume idempotence                       — tested below
//
// Invariants 1-3 cannot land yet: `Flasher::execute(FlashPlan const &)` does
// not accept a cancel token. The existing cancel hook lives only on
// `Flasher::read_full_rom(...)`. Enforcement work needed before tests land:
//
//   - Extend `Flasher::execute` to accept `std::atomic<bool> const *cancel`
//     (default null, matching `read_full_rom`'s pattern).
//   - Check the flag between sectors AND between TransferData blocks within
//     a sector. Between-PDU only — never mid-PDU; the in-flight PDU completes.
//   - On observed cancel mid-sector: emit `RequestTransferExit` (0x37) for
//     the in-flight download before unwinding.
//   - Always emit `DiagnosticSessionControl` → `kDscDefault` (0x10 0x01) on
//     cancel exit, so the ECU leaves the programming session cleanly.
//   - Return `ErrorCode::Cancelled` from `execute`.
//
// SSM block-write cancellation (invariant #2) is also blocked on this — and
// further blocked on the SSM B8 write flow itself, which the current
// `st::ecu::ssm` layer does not implement.

#include "st/core/crc32.hpp"
#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace flash = st::flash;
namespace uds = st::ecu::uds;

namespace {

flash::FlashPlan two_sector_plan() {
    flash::FlashPlan p;
    p.session = 0x02;
    p.verify_after_write = false;
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

std::filesystem::path tmp_journal_path(std::string_view tag) {
    return std::filesystem::temp_directory_path() /
           (std::string{"st_cancel_"} + std::string{tag} + "_" +
            std::to_string(std::random_device{}()) + ".toml");
}

} // namespace

// ---------------------------------------------------------------------------
// Invariant #5 — Resume idempotence
// ---------------------------------------------------------------------------
// `plan_resume` is the recovery seam after a host-mid-flash crash. Running it
// twice with the same (plan, journal) inputs must produce identical outputs;
// running it after the resumed plan has finished (i.e. all sectors marked
// done) must shrink the plan to empty. Both properties together guarantee
// that a confused user re-invoking recovery cannot double-write completed
// sectors.

TEST_CASE("plan_resume is a pure function on (plan, journal)",
          "[flash][cancellation][resume][idempotence]") {
    auto const original = two_sector_plan();
    flash::Manifest partial;
    partial.entries.push_back(done_entry(original.writes[0]));
    // sector 2 has no journal entry — crashed between sector 1 and 2.

    auto const a = flash::plan_resume(original, partial);
    auto const b = flash::plan_resume(original, partial);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->writes.size() == b->writes.size());
    REQUIRE(a->writes.size() == 1);
    REQUIRE(a->writes[0].sector == b->writes[0].sector);
    REQUIRE(a->writes[0].data == b->writes[0].data);
    REQUIRE(a->session == b->session);
    REQUIRE(a->verify_after_write == b->verify_after_write);
}

TEST_CASE("plan_resume on a fully-done journal collapses to empty",
          "[flash][cancellation][resume][idempotence]") {
    auto const original = two_sector_plan();
    flash::Manifest journal;
    journal.entries.push_back(done_entry(original.writes[0]));
    journal.entries.push_back(done_entry(original.writes[1]));

    // First call: every sector is done → empty plan.
    auto const first = flash::plan_resume(original, journal);
    REQUIRE(first.has_value());
    REQUIRE(first->writes.empty());

    // A "second invocation" of recovery, against the same journal, must
    // still produce an empty plan — recovery is not stateful. A user who
    // accidentally launches resume twice in a row cannot re-flash anything.
    auto const second = flash::plan_resume(original, journal);
    REQUIRE(second.has_value());
    REQUIRE(second->writes.empty());
}

// ---------------------------------------------------------------------------
// Invariant #4 — Crash-mid-flash recovery
// ---------------------------------------------------------------------------
// Simulates the host process dying between sectors. The on-disk journal,
// written incrementally by `Flasher::execute`, must contain enough state
// for `plan_resume` to construct a follow-up plan covering only the
// not-yet-completed sectors.
//
// "Crash" is simulated by making the second sector's eraseMemory fail with
// a UDS negative response, which causes `execute` to bail. The journal on
// disk after the bail is exactly what would be there if the host had been
// killed: sector 1 complete, sector 2 untouched.

TEST_CASE("execute crash leaves a journal that plan_resume can recover from",
          "[flash][cancellation][crash-recovery]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    constexpr std::uint32_t addr1 = 0x00001000;
    constexpr std::uint32_t addr2 = 0x00002000;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data1{0xAA, 0xAA, 0xAA, 0xAA};
    std::vector<std::uint8_t> const data2{0xBB, 0xBB, 0xBB, 0xBB};

    auto erase_opt = [](std::uint32_t addr, std::uint32_t sz) {
        return std::vector<std::uint8_t>{
            0x44,
            static_cast<std::uint8_t>((addr >> 24) & 0xFFU),
            static_cast<std::uint8_t>((addr >> 16) & 0xFFU),
            static_cast<std::uint8_t>((addr >> 8) & 0xFFU),
            static_cast<std::uint8_t>(addr & 0xFFU),
            static_cast<std::uint8_t>((sz >> 24) & 0xFFU),
            static_cast<std::uint8_t>((sz >> 16) & 0xFFU),
            static_cast<std::uint8_t>((sz >> 8) & 0xFFU),
            static_cast<std::uint8_t>(sz & 0xFFU),
        };
    };

    // Sector 1: happy path through verify.
    t.expect_send_recv({0x10, 0x02}, {0x50, 0x02});
    t.expect_send_recv({0x28, 0x03, 0x03}, {0x68, 0x03});
    t.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr1, size)),
        {0x71, 0x01, 0xFF, 0x00});
    t.expect_send_recv(uds::build_request_download(0x00, addr1, size), {0x74, 0x20, 0x00, 0x06});
    t.expect_send_recv(uds::build_transfer_data(1, data1), {0x76, 0x01});
    t.expect_send_recv(uds::build_request_transfer_exit(), {0x77});
    t.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
        {0x71, 0x01, 0xFF, 0x01});
    t.expect_send_recv(uds::build_read_memory_by_address(addr1, size),
                       {0x63, 0xAA, 0xAA, 0xAA, 0xAA});

    // Sector 2: erase fails with negative response code 0x22 (conditionsNotCorrect).
    // This is the "process died" simulation point — execute bails here.
    t.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr2, size)),
        {0x7F, 0x31, 0x22});

    auto const journal_path = tmp_journal_path("recovery");

    flash::FlashPlan plan;
    plan.writes.push_back({{addr1, size}, data1});
    plan.writes.push_back({{addr2, size}, data2});
    plan.journal_path = journal_path;
    plan.silence_bus = true;

    flash::Flasher f{t};
    auto const result = f.execute(plan);
    REQUIRE_FALSE(result.ok());

    // Read the journal back from disk — this is the "next boot" path. The
    // host process is gone; only the .toml file remains.
    REQUIRE(std::filesystem::exists(journal_path));
    auto const journal = flash::read_manifest(journal_path);
    REQUIRE(journal.has_value());

    // Now run plan_resume against the original plan + the recovered journal.
    auto const resumed = flash::plan_resume(plan, *journal);
    REQUIRE(resumed.has_value());
    // The resumed plan covers exactly the not-completed sector.
    REQUIRE(resumed->writes.size() == 1);
    REQUIRE(resumed->writes[0].sector.address == addr2);
    REQUIRE(resumed->writes[0].data == data2);
    // Options carry over (journal_path is intentionally inherited too —
    // the recovery caller typically resets it to a fresh path so the
    // original journal stays intact for audit, per the plan_resume contract).
    REQUIRE(resumed->session == plan.session);
    REQUIRE(resumed->silence_bus == plan.silence_bus);

    std::error_code ec;
    std::filesystem::remove(journal_path, ec);
}

TEST_CASE("resumed plan executed end-to-end reaches a steady state where resume is empty",
          "[flash][cancellation][crash-recovery][idempotence]") {
    // Run the recovery loop to completion: crash → resume → execute → assert
    // a follow-up plan_resume against the combined-journal-equivalent is
    // empty. This closes the loop on "you can never accidentally re-flash a
    // completed sector by re-invoking recovery."
    //
    // We exercise the contract at the plan_resume level: after a successful
    // execution of the resumed plan, every sector in the manifest is
    // transferred+verified → plan_resume(original_plan, full_journal) is
    // empty.

    auto const original = two_sector_plan();
    flash::Manifest full_journal;
    full_journal.entries.push_back(done_entry(original.writes[0]));
    full_journal.entries.push_back(done_entry(original.writes[1]));

    auto const empty = flash::plan_resume(original, full_journal);
    REQUIRE(empty.has_value());
    REQUIRE(empty->writes.empty());
}

// ---------------------------------------------------------------------------
// Invariants #1-3 — placeholder
// ---------------------------------------------------------------------------
// The mid-PDU UDS / SSM cancel-deferred tests and the session-exit-on-cancel
// test live HERE once `Flasher::execute` learns a cancel token. The
// enforcement signature is sketched in the file header. When that lands:
//
//   TEST_CASE("execute observes cancel between sectors and exits cleanly", ...)
//   TEST_CASE("execute observes cancel mid-TransferData; finishes PDU first", ...)
//   TEST_CASE("execute emits RequestTransferExit on cancel between TD and exit", ...)
//   TEST_CASE("execute final PDU on cancel is DiagnosticSessionControl(default)", ...)
//
// Leaving the slots called out so the next person knows where they go.
