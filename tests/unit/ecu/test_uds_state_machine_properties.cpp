// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Property tests for the UDS state machine — the orchestrator-level
// sequencing of session control / SA / RequestDownload / TransferData /
// RequestTransferExit / CommunicationControl that lives in
// `st::flash::Flasher::execute`. Closes the last ⬜ on docs/04 §Pre-1.0
// ship blocker #11 ("UDS state-machine sequence properties — the
// orchestrator-level state, not the framing").
//
// Companion to test_uds_properties.cpp (framing only) and test_uds.cpp
// (example-based fixed sequences). The properties here vary the plan
// shape — sector count, dry_run, verify_after_write, silence_bus, the
// cancel point — and assert the emitted PDU sequence respects every
// state-machine invariant the orchestrator commits to.
//
// State-machine invariants pinned per `Flasher::execute` source +
// docs/08-testing-strategy.md Tier 2a:
//
//   1. DSC(programming) is ALWAYS the first PDU. No prior PDU.
//   2. CommunicationControl(off) follows DSC iff plan.silence_bus.
//   3. For every sector in plan order: eraseMemory → RequestDownload →
//      TransferData* → RequestTransferExit → checkProgrammingDependencies.
//   4. When plan.verify_after_write: ReadMemoryByAddress* follows
//      checkProgrammingDependencies for that sector.
//   5. Final PDU on success is CommunicationControl(on) iff silence_bus.
//   6. Cancel between sectors: NO RequestTransferExit (nothing in
//      flight) + CommunicationControl(on) + DSC(default).
//   7. Cancel mid-sector after RequestDownload: RequestTransferExit
//      MUST appear in the cancel-cleanup wash + CC(on) + DSC(default).
//   8. dry_run: NO eraseMemory / RequestDownload / TransferData /
//      RequestTransferExit / checkProgrammingDependencies / RMBA in
//      the trace. Only DSC + (optional) CC pair.

#include "../_helpers/erase_opt.hpp"
#include "../_helpers/property.hpp"

#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace flash = st::flash;
namespace uds = st::ecu::uds;
using st::test::erase_opt;

namespace {

void expect(st::transport::MockTransport &t, std::vector<std::uint8_t> req,
            std::vector<std::uint8_t> resp) {
    t.expect_send_recv(std::move(req), std::move(resp));
}

// Build the FlashPlan + queue the MockTransport with the expected
// successful sequence. Returns the actual sent payloads for invariant
// checks after execute() returns.
struct PlanShape {
    std::uint8_t sector_count;     // 1..3 (cap for property iterations)
    bool silence_bus;
    bool verify_after_write;
    bool dry_run;
};

PlanShape draw_plan_shape(st::test::PropertyRng &rng) {
    PlanShape s{};
    s.sector_count = static_cast<std::uint8_t>(rng.size_in(1, 3));
    s.silence_bus = rng.boolean();
    s.verify_after_write = rng.boolean();
    s.dry_run = rng.boolean();
    return s;
}

flash::FlashPlan make_plan(PlanShape const &s) {
    flash::FlashPlan plan;
    plan.silence_bus = s.silence_bus;
    plan.verify_after_write = s.verify_after_write;
    plan.dry_run = s.dry_run;
    plan.block_size_hint = 4; // single-block transfers — keeps the property test compact
    plan.verify_chunk_size = 0x100;
    for (std::uint8_t i = 0; i < s.sector_count; ++i) {
        flash::SectorWrite sw;
        sw.sector.address = 0x1000U + static_cast<std::uint32_t>(i) * 0x100U;
        sw.sector.length = 4;
        sw.data = {static_cast<std::uint8_t>(0xA0U + i),
                   static_cast<std::uint8_t>(0xB0U + i),
                   static_cast<std::uint8_t>(0xC0U + i),
                   static_cast<std::uint8_t>(0xD0U + i)};
        plan.writes.push_back(std::move(sw));
    }
    return plan;
}

void queue_happy_path(st::transport::MockTransport &t, flash::FlashPlan const &plan) {
    // 1. DSC programming
    expect(t, {0x10, 0x02}, {0x50, 0x02});
    // 2. CC off iff silence_bus
    if (plan.silence_bus) {
        expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
    }
    if (!plan.dry_run) {
        for (auto const &w : plan.writes) {
            // 3a. eraseMemory
            expect(t,
                   uds::build_routine_control(uds::kRcStart, uds::kRidEraseMemory,
                                              erase_opt(w.sector.address, w.sector.length)),
                   {0x71, 0x01, 0xFF, 0x00});
            // 3b. RequestDownload — ECU max-block-length = 6 (payload 4).
            expect(t,
                   uds::build_request_download(0x00, w.sector.address, w.sector.length),
                   {0x74, 0x20, 0x00, 0x06});
            // 3c. TransferData(counter=1) — single block by construction.
            expect(t, uds::build_transfer_data(1, w.data), {0x76, 0x01});
            // 3d. RequestTransferExit
            expect(t, uds::build_request_transfer_exit(), {0x77});
            // 3e. checkProgrammingDependencies
            expect(t, uds::build_routine_control(uds::kRcStart, uds::kRidCheckProgrammingDependencies),
                   {0x71, 0x01, 0xFF, 0x01});
            // 3f. Verify pass
            if (plan.verify_after_write) {
                std::vector<std::uint8_t> rmba_resp{0x63};
                rmba_resp.insert(rmba_resp.end(), w.data.begin(), w.data.end());
                expect(t,
                       uds::build_read_memory_by_address(w.sector.address, w.sector.length),
                       std::move(rmba_resp));
            }
        }
    }
    // 4. CC on iff silence_bus
    if (plan.silence_bus) {
        expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});
    }
}

// Pretty-print one wire byte sequence for CAPTURE on failure. CAPTUREd
// strings show up alongside the seed in Catch2's failure dump so a
// failing case is reproducible just from the test output.
std::string render_send_log(std::vector<std::vector<std::uint8_t>> const &log) {
    std::string out;
    char buf[16];
    for (std::size_t i = 0; i < log.size(); ++i) {
        out.append("  [").append(std::to_string(i)).append("] ");
        for (auto b : log[i]) {
            std::snprintf(buf, sizeof buf, "%02X ", b);
            out.append(buf);
        }
        out.append("\n");
    }
    return out;
}

} // namespace

TEST_CASE("property: Flasher::execute happy-path PDU sequence respects every invariant",
          "[ecu][uds][state-machine][property]") {
    st::test::check_property(0xC0FFEEU, 80, [&](st::test::PropertyRng &rng) {
        auto const shape = draw_plan_shape(rng);
        st::transport::MockTransport t;
        REQUIRE(t.open({}).has_value());

        auto const plan = make_plan(shape);
        queue_happy_path(t, plan);

        flash::Flasher f{t};
        auto const r = f.execute(plan);
        CAPTURE(render_send_log(t.send_log()));
        REQUIRE(r.ok());
        REQUIRE(r.report.entered_session);
        REQUIRE(r.report.silenced_bus == shape.silence_bus);
        REQUIRE(r.report.restored_bus == shape.silence_bus);

        auto const &sent = t.send_log();
        REQUIRE_FALSE(sent.empty());

        // Invariant 1: DSC programming is the first PDU.
        REQUIRE(sent[0].size() >= 2);
        REQUIRE(sent[0][0] == uds::kSidDiagnosticSessionControl);
        REQUIRE(sent[0][1] == uds::kDscProgramming);

        // Invariant 2: CC(off) follows DSC iff silence_bus.
        if (shape.silence_bus) {
            REQUIRE(sent.size() >= 2);
            REQUIRE(sent[1][0] == uds::kSidCommunicationControl);
            REQUIRE(sent[1][1] == uds::kCcDisableRxAndTx);
        }

        // Invariant 5: final PDU is CC(on) iff silence_bus.
        if (shape.silence_bus) {
            auto const &last = sent.back();
            REQUIRE(last.size() >= 2);
            REQUIRE(last[0] == uds::kSidCommunicationControl);
            REQUIRE(last[1] == uds::kCcEnableRxAndTx);
        }

        // Invariant 8 partial: dry_run emits NO sector-level PDUs.
        if (shape.dry_run) {
            for (auto const &pdu : sent) {
                REQUIRE_FALSE(pdu.empty());
                std::uint8_t const sid = pdu[0];
                REQUIRE_FALSE(sid == uds::kSidRoutineControl);
                REQUIRE_FALSE(sid == uds::kSidRequestDownload);
                REQUIRE_FALSE(sid == uds::kSidTransferData);
                REQUIRE_FALSE(sid == uds::kSidRequestTransferExit);
                REQUIRE_FALSE(sid == uds::kSidReadMemoryByAddress);
            }
        } else {
            // Invariant 3: per-sector ordering — eraseMemory ↦ RD ↦ TD ↦
            // RTE ↦ checkDeps in that order. Indexed by counting
            // occurrences.
            std::size_t erase_count = 0;
            std::size_t rdl_count = 0;
            std::size_t td_count = 0;
            std::size_t rte_count = 0;
            for (auto const &pdu : sent) {
                if (pdu.empty())
                    continue;
                if (pdu[0] == uds::kSidRoutineControl && pdu.size() >= 4) {
                    auto const rid = static_cast<std::uint16_t>((pdu[2] << 8U) | pdu[3]);
                    if (rid == uds::kRidEraseMemory)
                        ++erase_count;
                } else if (pdu[0] == uds::kSidRequestDownload) {
                    ++rdl_count;
                } else if (pdu[0] == uds::kSidTransferData) {
                    ++td_count;
                } else if (pdu[0] == uds::kSidRequestTransferExit) {
                    ++rte_count;
                }
            }
            REQUIRE(erase_count == shape.sector_count);
            REQUIRE(rdl_count == shape.sector_count);
            REQUIRE(td_count == shape.sector_count);
            REQUIRE(rte_count == shape.sector_count);
        }
    });
}

TEST_CASE("property: cancel-before-execute emits only the cleanup PDUs, no work",
          "[ecu][uds][state-machine][property][cancel]") {
    // Cancel flag pre-set → execute should bail at the first cancel
    // check, which is between sectors (before any erase). Per the
    // cancel_cleanup logic: no in-flight RTE, CC restore iff bus was
    // silenced, DSC default always.
    st::test::check_property(0xCA11CE1ULL, 60, [&](st::test::PropertyRng &rng) {
        auto const shape = draw_plan_shape(rng);
        if (shape.dry_run) {
            return; // dry_run never executes sector loop; cancel point doesn't apply
        }
        st::transport::MockTransport t;
        REQUIRE(t.open({}).has_value());

        auto const plan = make_plan(shape);

        // Queue: DSC enter, optional CC off, then the cleanup wash.
        expect(t, {0x10, 0x02}, {0x50, 0x02});
        if (shape.silence_bus) {
            expect(t, {0x28, 0x03, 0x03}, {0x68, 0x03});
            // Cancel cleanup: CC on first (between-sectors cancel has
            // no in-flight RTE).
            expect(t, {0x28, 0x00, 0x03}, {0x68, 0x00});
        }
        // Cleanup: DSC default.
        expect(t, {0x10, 0x01}, {0x50, 0x01});

        std::atomic<bool> cancel{true};
        flash::Flasher f{t};
        auto const r = f.execute(plan, &cancel);
        CAPTURE(render_send_log(t.send_log()));
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error->code() == st::ErrorCode::Cancelled);

        // Invariant 6: NO eraseMemory PDU was emitted.
        for (auto const &pdu : t.send_log()) {
            if (pdu.size() >= 4 && pdu[0] == uds::kSidRoutineControl) {
                auto const rid = static_cast<std::uint16_t>((pdu[2] << 8U) | pdu[3]);
                REQUIRE(rid != uds::kRidEraseMemory);
            }
        }

        // Last PDU is always DSC(default) on cancel cleanup.
        auto const &last = t.send_log().back();
        REQUIRE(last.size() >= 2);
        REQUIRE(last[0] == uds::kSidDiagnosticSessionControl);
        REQUIRE(last[1] == uds::kDscDefault);
    });
}

// NOTE: cancel-mid-sector (the case where the cancel flag flips AFTER
// RequestDownload but BEFORE all TransferData blocks have been sent) is
// pinned by the example-based suite in test_cancellation_invariants.cpp,
// which uses a custom transport wrapper to flip cancel during a specific
// send_recv. Property-testing that case adds little — the seed-driven
// shape variation is irrelevant once the cancel point is fixed mid-PDU.
