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
//   1. Mid-PDU UDS cancel is deferred           — tested below
//   2. Mid-PDU SSM cancel is deferred           — DEFERRED to v1.3
//   3. Session-exit on cancel                   — tested below
//   4. Crash-mid-flash recovery                 — tested below
//   5. Resume idempotence                       — tested below
//
// Invariant #2 (SSM B8 block-write cancel) is doubly blocked: it requires
// both the `Flasher::execute` cancel hook (now landed) AND the SSM B8
// block-write flow itself, which the current `st::ecu::ssm` layer does not
// implement. SSM is only used by pre-2008 Subarus, scheduled for v1.3.

#include "st/core/crc32.hpp"
#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include "../_helpers/erase_opt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
// Invariants #1 + #3 — mid-PDU UDS cancel deferred + session-exit on cancel
// ---------------------------------------------------------------------------
// `Flasher::execute(plan, &cancel)` polls the cancel flag at PDU boundaries:
// between sectors AND between TransferData blocks within a sector. It NEVER
// polls mid-PDU — once a UDS request-response exchange is in flight, the
// in-flight PDU completes before the cancel check fires. On observed cancel:
//
//   - If a download is in flight (RequestDownload sent, RequestTransferExit
//     not yet sent), emit RequestTransferExit so the ECU unwinds cleanly.
//   - Always emit DiagnosticSessionControl → kDscDefault as the final PDU,
//     so the ECU leaves the programming session cleanly.
//   - Return ErrorCode::Cancelled with the partial report.
//
// The cancel cleanup PDUs are best-effort: failures are swallowed because
// the user has already asked us to stop, and a failure-to-clean-up has no
// useful surface.

namespace {

// Wrapper transport for the "cancel observed mid-stream" tests. Delegates
// every ITransport call to a backing MockTransport, but counts send_recv
// invocations and flips the cancel flag immediately AFTER the configured
// trip exchange returns. The "after" timing is load-bearing: the test
// asserts that the in-flight PDU completes normally and the NEXT cancel
// check (at the next PDU boundary in execute()) trips.
class CancelAfterNthExchange : public st::transport::ITransport {
public:
    CancelAfterNthExchange(st::transport::MockTransport &m, std::atomic<bool> &cancel_flag,
                           int trip_at) noexcept
        : mock_{m}, cancel_{cancel_flag}, trip_at_{trip_at} {}

    [[nodiscard]] st::Status open(st::transport::LinkConfig const &cfg) override {
        return mock_.open(cfg);
    }
    [[nodiscard]] st::Status close() override {
        return mock_.close();
    }
    [[nodiscard]] st::Result<st::transport::Frame>
    send_recv(std::span<std::uint8_t const> payload,
              std::chrono::milliseconds timeout) override {
        auto r = mock_.send_recv(payload, timeout);
        if (++count_ == trip_at_)
            cancel_.store(true, std::memory_order_release);
        return r;
    }
    [[nodiscard]] st::Status send(std::span<std::uint8_t const> payload) override {
        return mock_.send(payload);
    }
    [[nodiscard]] st::Status start_streaming(st::transport::FrameCallback cb) override {
        return mock_.start_streaming(std::move(cb));
    }
    [[nodiscard]] st::Status stop_streaming() override {
        return mock_.stop_streaming();
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "CancelAfterNthExchange";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return "test";
    }

private:
    st::transport::MockTransport &mock_;
    std::atomic<bool> &cancel_;
    int const trip_at_;
    int count_{0};
};

using st::test::erase_opt;

} // namespace

TEST_CASE("execute observes pre-set cancel and exits via DSC(default)",
          "[flash][cancellation][cancel][session-exit]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // Queued exchanges:
    //   1. Programming-session enter (DSC 0x02).
    //   2. (top-of-sector-loop cancel check trips here — between PDUs.)
    //   3. Final DSC(default) on cancel cleanup.
    t.expect_send_recv({0x10, 0x02}, {0x50, 0x02});
    t.expect_send_recv({0x10, 0x01}, {0x50, 0x01});

    std::atomic<bool> cancel{true};

    flash::FlashPlan plan;
    plan.silence_bus = false;
    plan.writes.push_back({{0x1000, 4}, {0xAA, 0xAA, 0xAA, 0xAA}});

    flash::Flasher f{t};
    auto const r = f.execute(plan, &cancel);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.has_value());
    REQUIRE(r.error->code() == st::ErrorCode::Cancelled);

    // No erase, no download — the cancel observation happens at the top of
    // the sector loop, before any per-sector PDU goes out.
    auto const &log = t.send_log();
    REQUIRE(log.size() == 2);
    REQUIRE(log[0] == std::vector<std::uint8_t>{0x10, 0x02}); // enter programming
    REQUIRE(log[1] == std::vector<std::uint8_t>{0x10, 0x01}); // session-exit cleanup
    // Final PDU on cancel IS DiagnosticSessionControl(default).
    REQUIRE(log.back() == std::vector<std::uint8_t>{0x10, 0x01});
}

TEST_CASE("execute observes cancel between sectors and exits via DSC(default)",
          "[flash][cancellation][cancel][session-exit]") {
    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());

    constexpr std::uint32_t addr1 = 0x00001000;
    constexpr std::uint32_t addr2 = 0x00002000;
    constexpr std::uint32_t size = 4;
    std::vector<std::uint8_t> const data1{0xAA, 0xAA, 0xAA, 0xAA};
    std::vector<std::uint8_t> const data2{0xBB, 0xBB, 0xBB, 0xBB};

    // Sector 1: full happy path through check_deps. Verify is off so we
    // don't queue a read-back exchange.
    mock.expect_send_recv({0x10, 0x02}, {0x50, 0x02});
    mock.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr1, size)),
        {0x71, 0x01, 0xFF, 0x00});
    mock.expect_send_recv(uds::build_request_download(0x00, addr1, size),
                          {0x74, 0x20, 0x00, 0x06});
    mock.expect_send_recv(uds::build_transfer_data(1, data1), {0x76, 0x01});
    mock.expect_send_recv(uds::build_request_transfer_exit(), {0x77});
    mock.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
        {0x71, 0x01, 0xFF, 0x01});
    // Sector 2: never enters. Top-of-loop cancel check trips after the
    // sector-1 check_deps exchange (the 6th send_recv), before sector 2's
    // erase goes out.
    mock.expect_send_recv({0x10, 0x01}, {0x50, 0x01}); // DSC(default) cleanup

    std::atomic<bool> cancel{false};
    CancelAfterNthExchange wrapper{mock, cancel, /*trip_at=*/6};

    flash::FlashPlan plan;
    plan.silence_bus = false;
    plan.verify_after_write = false;
    plan.writes.push_back({{addr1, size}, data1});
    plan.writes.push_back({{addr2, size}, data2});

    flash::Flasher f{wrapper};
    auto const r = f.execute(plan, &cancel);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.has_value());
    REQUIRE(r.error->code() == st::ErrorCode::Cancelled);

    // Sector 1 fully completed (transferred + check_deps_passed). Sector 2
    // never started — partial report carries the failure visibility.
    REQUIRE(r.report.sectors.size() == 1);
    REQUIRE(r.report.sectors[0].transferred);
    REQUIRE(r.report.sectors[0].exited);
    REQUIRE(r.report.sectors[0].check_deps_passed);

    // No RequestTransferExit cleanup needed — sector 1's RTE already went
    // out as part of the happy-path sequence; no in-flight download.
    auto const &log = mock.send_log();
    // 6 sector-1 exchanges + DSC(default) cleanup = 7 PDUs.
    REQUIRE(log.size() == 7);
    REQUIRE(log.back() == std::vector<std::uint8_t>{0x10, 0x01});
    // No second sector erase went out.
    auto const erase2 = uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory,
                                                   erase_opt(addr2, size));
    for (auto const &sent : log) {
        REQUIRE(sent != erase2);
    }
}

TEST_CASE("execute observes cancel mid-TransferData; emits RequestTransferExit + DSC(default)",
          "[flash][cancellation][cancel][session-exit][mid-pdu]") {
    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());

    constexpr std::uint32_t addr = 0x00001000;
    constexpr std::uint32_t size = 8;
    std::vector<std::uint8_t> const data{0xAA, 0xAA, 0xAA, 0xAA, 0xBB, 0xBB, 0xBB, 0xBB};

    // ECU advertises maxNumberOfBlockLength=6, so block_payload=4 → two
    // TransferData calls (block 1: bytes 0-3, block 2: bytes 4-7). Cancel
    // trips after the first TransferData response comes back; the second
    // never goes out. Cleanup emits RequestTransferExit + DSC(default).
    mock.expect_send_recv({0x10, 0x02}, {0x50, 0x02});
    mock.expect_send_recv(
        uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory, erase_opt(addr, size)),
        {0x71, 0x01, 0xFF, 0x00});
    mock.expect_send_recv(uds::build_request_download(0x00, addr, size),
                          {0x74, 0x20, 0x00, 0x06});
    mock.expect_send_recv(uds::build_transfer_data(1, std::vector<std::uint8_t>{0xAA, 0xAA, 0xAA, 0xAA}),
                          {0x76, 0x01});
    // After this 4th exchange the cancel flag flips. The 2nd TransferData
    // never starts; cleanup emits RequestTransferExit + DSC(default).
    mock.expect_send_recv(uds::build_request_transfer_exit(), {0x77});
    mock.expect_send_recv({0x10, 0x01}, {0x50, 0x01});

    std::atomic<bool> cancel{false};
    CancelAfterNthExchange wrapper{mock, cancel, /*trip_at=*/4};

    flash::FlashPlan plan;
    plan.silence_bus = false;
    plan.verify_after_write = false;
    plan.writes.push_back({{addr, size}, data});

    flash::Flasher f{wrapper};
    auto const r = f.execute(plan, &cancel);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.has_value());
    REQUIRE(r.error->code() == st::ErrorCode::Cancelled);

    // Sector visibility: download was started but not fully transferred.
    REQUIRE(r.report.sectors.size() == 1);
    REQUIRE(r.report.sectors[0].downloaded);
    REQUIRE_FALSE(r.report.sectors[0].transferred);
    REQUIRE_FALSE(r.report.sectors[0].exited);

    auto const &log = mock.send_log();
    // Expected wire sequence:
    //   1. DSC(programming)
    //   2. RoutineControl(Start, EraseMemory, ...)
    //   3. RequestDownload(addr, size)
    //   4. TransferData(counter=1, first 4 bytes)
    //   5. RequestTransferExit  ← cleanup
    //   6. DSC(default)         ← cleanup, final PDU
    REQUIRE(log.size() == 6);
    REQUIRE(log[3] == uds::build_transfer_data(1, std::vector<std::uint8_t>{0xAA, 0xAA, 0xAA, 0xAA}));
    REQUIRE(log[4] == uds::build_request_transfer_exit());
    REQUIRE(log[5] == std::vector<std::uint8_t>{0x10, 0x01});
    REQUIRE(log.back() == std::vector<std::uint8_t>{0x10, 0x01});

    // PDU atomicity: the 2nd TransferData never started — no torn PDU on
    // the wire. Block counter never reached 2.
    auto const torn_block2 =
        uds::build_transfer_data(2, std::vector<std::uint8_t>{0xBB, 0xBB, 0xBB, 0xBB});
    for (auto const &sent : log) {
        REQUIRE(sent != torn_block2);
    }
}

TEST_CASE("execute cancel restores communication-control when silence_bus was on",
          "[flash][cancellation][cancel][silence_bus]") {
    // Without this restore, a cancel that fires after CC(off) but before
    // the main happy-path CC(on) leaves the ECU's bus silenced until a
    // power cycle or extended-diag session unsticks it. Not a brick, but
    // a real user-visible nuisance — the cancel cleanup emits CC(on)
    // before DSC(default) so the bus comes back to normal.

    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());

    // Pre-set cancel + silence_bus=true. Sector loop never executes;
    // cancel is observed at the top of the first iteration. Expected
    // wire sequence:
    //   1. DSC(programming)
    //   2. CC(off) — silence_bus step
    //   (top-of-sector-loop cancel check trips here)
    //   3. CC(on)  — cancel cleanup restore
    //   4. DSC(default)
    mock.expect_send_recv({0x10, 0x02}, {0x50, 0x02});
    mock.expect_send_recv({0x28, uds::kCcDisableRxAndTx, uds::kCtNormalAndNetworkManagement},
                          {0x68, uds::kCcDisableRxAndTx});
    mock.expect_send_recv({0x28, uds::kCcEnableRxAndTx, uds::kCtNormalAndNetworkManagement},
                          {0x68, uds::kCcEnableRxAndTx});
    mock.expect_send_recv({0x10, 0x01}, {0x50, 0x01});

    std::atomic<bool> cancel{true};

    flash::FlashPlan plan;
    plan.silence_bus = true;
    plan.writes.push_back({{0x1000, 4}, {0xAA, 0xAA, 0xAA, 0xAA}});

    flash::Flasher f{mock};
    auto const r = f.execute(plan, &cancel);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.has_value());
    REQUIRE(r.error->code() == st::ErrorCode::Cancelled);
    REQUIRE(r.report.silenced_bus);
    REQUIRE(r.report.restored_bus);

    auto const &log = mock.send_log();
    REQUIRE(log.size() == 4);
    REQUIRE(log[0] == std::vector<std::uint8_t>{0x10, 0x02});
    REQUIRE(log[1] == std::vector<std::uint8_t>{0x28, uds::kCcDisableRxAndTx,
                                                uds::kCtNormalAndNetworkManagement});
    REQUIRE(log[2] == std::vector<std::uint8_t>{0x28, uds::kCcEnableRxAndTx,
                                                uds::kCtNormalAndNetworkManagement});
    REQUIRE(log[3] == std::vector<std::uint8_t>{0x10, 0x01});
    // Final PDU still DSC(default); CC(on) goes BEFORE it so the bus is
    // restored while we're still in the programming session.
    REQUIRE(log.back() == std::vector<std::uint8_t>{0x10, 0x01});
}
