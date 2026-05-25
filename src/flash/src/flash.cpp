// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/flash.hpp"

#include "st/core/crc32.hpp"
#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

namespace st::flash {

namespace {
// Hex-format a 32-bit address for inclusion in error messages.
// std::to_string is decimal, so "0x" + std::to_string(0x1234) used to
// produce the very-confusing "0x4660".
std::string hex_addr(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return std::string{buf};
}
} // namespace

// ---------------------------------------------------------------------
// Public-API helpers
// ---------------------------------------------------------------------

bool FlashReport::all_sectors_completed() const noexcept {
    if (sectors.empty())
        return false;
    for (auto const &s : sectors) {
        if (!s.erased || !s.downloaded || !s.transferred || !s.exited || !s.check_deps_passed) {
            return false;
        }
    }
    return true;
}

bool FlashReport::all_sectors_verified() const noexcept {
    for (auto const &s : sectors) {
        if (!s.verified)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// read_full_rom — chunked ReadMemoryByAddress loop
// ---------------------------------------------------------------------

Result<std::vector<std::uint8_t>>
Flasher::read_full_rom(std::uint32_t base_address, std::uint32_t total_length,
                       std::uint32_t max_chunk_size, std::chrono::milliseconds per_chunk_timeout,
                       Flasher::ReadProgressFn progress, std::atomic<bool> const *cancel,
                       bool enter_diagnostic_session, bool authenticate,
                       std::uint8_t security_level) {
    if (total_length == 0) {
        if (progress)
            progress({0, 0});
        return std::vector<std::uint8_t>{};
    }
    if (max_chunk_size == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "flash: read_full_rom max_chunk_size must be > 0");
    }

    // Optional extendedDiagnostic-session entry before the chunk loop.
    // Subaru Hitachi ECUs (FA20DIT and similar) silently drop RMBA
    // (SID 0x23) in the default session — no NRC, just silence.
    // Diagnosed 2026-05-23: stock VA WRX with COBB tune uninstalled
    // still produced silent RX timeouts on UDS RMBA until DSC entry
    // was added. Mirrors what EcuTek's ProECU calls "Enter Utility
    // Mode" in its ROM-dump workflow.
    //
    // Opt-in (default off) because Flasher::execute's verify-after-write
    // path already enters programmingSession at the top of the sequence;
    // a transition to extendedSession mid-flash would invalidate the
    // erase+download work. Standalone Read-ROM callers (CLI / GUI Tools →
    // Read ROM from Car) pass true so the ECU is in the right state
    // before the first RMBA.
    if (enter_diagnostic_session) {
        if (auto s = client_.diagnostic_session_control(ecu::uds::kDscExtendedDiagnostic,
                                                        per_chunk_timeout);
            !s.has_value()) {
            return failure(s.error().code(),
                           std::string{"flash: read_full_rom session entry failed: "} +
                               std::string{s.error().message()});
        }
    }

    // Optional UDS SecurityAccess preamble. Real-hardware Subaru ECUs
    // gate RMBA (and most other "interesting" services) behind seed/key
    // authentication — read attempts without it come back as
    // `7F 23 33` (securityAccessDenied), diagnosed 2026-05-23 on a 2017
    // VA WRX. Per ISO 14229, odd sub-functions = requestSeed, even =
    // sendKey-one-greater; `security_level` selects the odd one.
    //
    // `security_key_fn_` is a runtime-pluggable transform. Since
    // 2026-05-24 the default `st::ecu::subaru::ssmcan1_key_stub` is the
    // real Gen-A.2 L1 implementation (SH7058 ECUs ~2008-2017). Other eras
    // — Gen-A K-Line, Gen-B AES, or any user-supplied override — slot in
    // via `set_security_key_fn`. See `src/ecu/include/st/ecu/security_key.hpp`.
    if (authenticate) {
        if (!security_key_fn_) {
            return failure(ErrorCode::InvalidArgument,
                           "flash: read_full_rom: authenticate=true but "
                           "security_key_fn_ is empty — call "
                           "Flasher::set_security_key_fn first");
        }
        auto seed = client_.security_access_request_seed(security_level, per_chunk_timeout);
        if (!seed.has_value()) {
            return failure(seed.error().code(),
                           std::string{"flash: read_full_rom SecurityAccess requestSeed "
                                       "(sub 0x"} +
                               [security_level]() {
                                   char b[3];
                                   std::snprintf(b, sizeof b, "%02X",
                                                 static_cast<unsigned>(security_level));
                                   return std::string{b};
                               }() +
                               ") failed: " + std::string{seed.error().message()});
        }
        auto key = security_key_fn_(*seed);
        if (!key.has_value()) {
            return failure(key.error().code(),
                           std::string{"flash: read_full_rom SecurityAccess key derivation "
                                       "failed: "} +
                               std::string{key.error().message()});
        }
        auto const send_sub = static_cast<std::uint8_t>(security_level + 1U);
        if (auto s = client_.security_access_send_key(send_sub, *key, per_chunk_timeout);
            !s.has_value()) {
            return failure(s.error().code(),
                           std::string{"flash: read_full_rom SecurityAccess sendKey "
                                       "(sub 0x"} +
                               [send_sub]() {
                                   char b[3];
                                   std::snprintf(b, sizeof b, "%02X",
                                                 static_cast<unsigned>(send_sub));
                                   return std::string{b};
                               }() +
                               ") rejected by ECU: " + std::string{s.error().message()});
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(total_length);
    std::uint32_t cursor = base_address;
    std::uint32_t remaining = total_length;
    // Emit a t=0 progress event so the UI can show "starting…" immediately.
    if (progress)
        progress({0, total_length});
    while (remaining > 0) {
        // Cancel check between chunks — caller's atomic flag. We discard
        // the partial buffer rather than returning it (the partial bytes
        // wouldn't form a valid ROM and we don't want callers building
        // pipelines that try to salvage incomplete reads).
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return failure(ErrorCode::Cancelled,
                           "flash: read_full_rom cancelled at " + hex_addr(cursor));
        }
        std::uint32_t const this_chunk = remaining < max_chunk_size ? remaining : max_chunk_size;
        auto chunk = client_.read_memory_by_address(cursor, this_chunk, per_chunk_timeout);
        if (!chunk.has_value()) {
            return failure(chunk.error().code(), "flash: read_full_rom failed at " +
                                                     hex_addr(cursor) + ": " +
                                                     std::string{chunk.error().message()});
        }
        if (chunk->size() != this_chunk) {
            return failure(ErrorCode::UnexpectedEof, "flash: short read at " + hex_addr(cursor) +
                                                         " (expected " +
                                                         std::to_string(this_chunk) + ", got " +
                                                         std::to_string(chunk->size()) + ")");
        }
        out.insert(out.end(), chunk->begin(), chunk->end());
        cursor += this_chunk;
        remaining -= this_chunk;
        if (progress)
            progress({total_length - remaining, total_length});
    }
    return out;
}

// ---------------------------------------------------------------------
// read_full_rom_ssm — chunked SSM 0xA8 ReadByAddress loop
// ---------------------------------------------------------------------
//
// VA WRX (and every CAN-era Subaru still on SSM rather than UDS) responds
// to the K-Line SSM frame format encapsulated in ISO-TP on CAN ID 0x7E0.
// SSM 0xA8 carries one address per data byte; the request's LEN field is
// a single byte and the payload is `CMD(1) + PAD(1) + 3*N`, so the per-
// request ceiling is N=84 addresses. We cap at 80 for headroom and let
// the caller pass the same `max_chunk_size` they would for UDS — internal
// clamping does the right thing either way.

namespace {
constexpr std::uint32_t kSsmMaxChunkBytes = 80;
} // namespace

Result<std::vector<std::uint8_t>>
Flasher::read_full_rom_ssm(std::uint32_t base_address, std::uint32_t total_length,
                           std::uint32_t max_chunk_size,
                           std::chrono::milliseconds per_chunk_timeout,
                           Flasher::ReadProgressFn progress,
                           std::atomic<bool> const *cancel) {
    if (total_length == 0) {
        if (progress)
            progress({0, 0});
        return std::vector<std::uint8_t>{};
    }
    if (max_chunk_size == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "flash: read_full_rom_ssm max_chunk_size must be > 0");
    }
    if (base_address > ecu::ssm::kMaxAddress ||
        static_cast<std::uint64_t>(base_address) + total_length - 1U > ecu::ssm::kMaxAddress) {
        return failure(ErrorCode::InvalidArgument,
                       "flash: read_full_rom_ssm: address range exceeds SSM 24-bit limit "
                       "(base " +
                           hex_addr(base_address) + ", len " + std::to_string(total_length) + ")");
    }
    std::uint32_t const effective_chunk =
        max_chunk_size < kSsmMaxChunkBytes ? max_chunk_size : kSsmMaxChunkBytes;

    std::vector<std::uint8_t> out;
    out.reserve(total_length);
    std::uint32_t cursor = base_address;
    std::uint32_t remaining = total_length;
    if (progress)
        progress({0, total_length});
    while (remaining > 0) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return failure(ErrorCode::Cancelled,
                           "flash: read_full_rom_ssm cancelled at " + hex_addr(cursor));
        }
        std::uint32_t const this_chunk = remaining < effective_chunk ? remaining : effective_chunk;
        auto chunk = ssm_client_.read_block(cursor, this_chunk, per_chunk_timeout);
        if (!chunk.has_value()) {
            return failure(chunk.error().code(), "flash: read_full_rom_ssm failed at " +
                                                     hex_addr(cursor) + ": " +
                                                     std::string{chunk.error().message()});
        }
        if (chunk->size() != this_chunk) {
            return failure(ErrorCode::UnexpectedEof, "flash: short SSM read at " +
                                                         hex_addr(cursor) + " (expected " +
                                                         std::to_string(this_chunk) + ", got " +
                                                         std::to_string(chunk->size()) + ")");
        }
        out.insert(out.end(), chunk->begin(), chunk->end());
        cursor += this_chunk;
        remaining -= this_chunk;
        if (progress)
            progress({total_length - remaining, total_length});
    }
    return out;
}

// ---------------------------------------------------------------------
// compute_delta — sector-aligned byte-diff
// ---------------------------------------------------------------------

std::vector<Sector> Flasher::compute_delta(std::span<std::uint8_t const> current,
                                           std::span<std::uint8_t const> target,
                                           std::uint32_t sector_size, std::uint32_t base_address) {
    std::vector<Sector> out;
    if (sector_size == 0)
        return out;
    auto const n = std::min(current.size(), target.size());
    for (std::size_t off = 0; off < n; off += sector_size) {
        std::size_t const end = std::min(off + sector_size, n);
        bool differs = false;
        for (std::size_t i = off; i < end; ++i) {
            if (current[i] != target[i]) {
                differs = true;
                break;
            }
        }
        if (differs) {
            out.push_back(Sector{
                base_address + static_cast<std::uint32_t>(off),
                static_cast<std::uint32_t>(end - off),
            });
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// execute — full flash orchestration
// ---------------------------------------------------------------------

namespace {

// ISO 14229-1 Annex F eraseMemory routine option record:
//   [aLFI] [memoryAddress bytes] [memorySize bytes]
// where aLFI's high nibble = number of memorySize bytes, low nibble =
// number of memoryAddress bytes. SubuwuTuner v1 targets 32-bit address
// space and 32-bit sizes → aLFI = 0x44.
std::vector<std::uint8_t> build_erase_option_record(std::uint32_t addr, std::uint32_t size) {
    std::vector<std::uint8_t> opt;
    opt.reserve(9);
    opt.push_back(0x44);
    opt.push_back(static_cast<std::uint8_t>((addr >> 24) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((addr >> 16) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((addr >> 8) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>(addr & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFU));
    opt.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    return opt;
}

// Pick a per-TransferData payload size that respects both the ECU's
// reported maxNumberOfBlockLength and the caller's hint. The reported
// value counts the SID byte and the block-sequence-counter byte, so
// the payload is `reported - 2`.
std::uint32_t choose_block_payload(std::uint32_t reported_max, std::uint32_t hint) {
    std::uint32_t const safe_reported = reported_max > 2 ? reported_max - 2 : 0;
    if (hint == 0)
        return safe_reported;
    if (safe_reported == 0)
        return hint;
    return safe_reported < hint ? safe_reported : hint;
}

} // namespace

ExecuteOutcome Flasher::execute(FlashPlan const &plan, std::atomic<bool> const *cancel) {
    FlashReport report{};
    report.sectors.reserve(plan.writes.size());

    // Failure-path helper: package the in-progress report + an Error
    // into an ExecuteOutcome and return. Keeps the original failure
    // sites readable while guaranteeing the partial report is never
    // discarded.
    //
    // When the bus was silenced earlier in the sequence and we haven't
    // yet restored it, bail attempts a best-effort CC restore before
    // returning. Otherwise a mid-flash failure (eraseMemory, RequestDownload,
    // TransferData, RequestTransferExit, checkProgrammingDependencies,
    // verify) would leave non-diagnostic traffic muted until something
    // else (power cycle, extended-diag session) un-mutes it — a real
    // user-visible nuisance even though it isn't a brick risk. Mirrors
    // the cancel_cleanup behavior below; the failure case was the gap
    // flagged in this module's review.
    auto bail = [&](ErrorCode code, std::string message) {
        if (report.silenced_bus && !report.restored_bus) {
            if (auto s = client_.communication_control(ecu::uds::kCcEnableRxAndTx,
                                                       ecu::uds::kCtNormalAndNetworkManagement);
                s.has_value()) {
                report.restored_bus = true;
            }
        }
        ExecuteOutcome out;
        out.report = std::move(report);
        out.error = Error{code, std::move(message)};
        return out;
    };

    // Cancellation predicate. The acquire load pairs with whatever release
    // store the UI/worker thread does when flipping the flag (mirrors the
    // pattern in read_full_rom). Pure read — does not mutate the flag.
    auto is_cancelled = [&]() noexcept {
        return cancel != nullptr && cancel->load(std::memory_order_acquire);
    };

    // Best-effort cleanup emitted on observed cancel:
    //   1. If a download is in-flight (RequestDownload sent, but
    //      RequestTransferExit not yet sent), emit RequestTransferExit so
    //      the ECU's download state machine unwinds cleanly.
    //   2. If we silenced non-diagnostic traffic at step 2 of the main
    //      sequence, restore it. Skipping this would leave the bus muted
    //      until a power cycle or extended-diag session unsticks the
    //      CommunicationControl state — caller-visible nuisance even
    //      though it isn't a brick risk.
    //   3. Always emit DiagnosticSessionControl → kDscDefault so the ECU
    //      leaves the programming session.
    // All three swallow their errors: the user has already asked us to
    // stop, and forwarding a cleanup failure here obscures the cancel.
    // `report.restored_bus` is updated when CC restore succeeds so the
    // partial report accurately reflects bus state.
    auto cancel_cleanup = [&](bool mid_sector_download) {
        if (mid_sector_download) {
            (void)client_.request_transfer_exit();
        }
        if (report.silenced_bus && !report.restored_bus) {
            if (auto s = client_.communication_control(ecu::uds::kCcEnableRxAndTx,
                                                       ecu::uds::kCtNormalAndNetworkManagement);
                s.has_value()) {
                report.restored_bus = true;
            }
        }
        (void)client_.diagnostic_session_control(ecu::uds::kDscDefault);
    };

    // Validate the plan before touching the bus.
    for (auto const &w : plan.writes) {
        if (w.data.size() != w.sector.length) {
            return bail(ErrorCode::InvalidArgument,
                        "flash: SectorWrite.data.size() (" + std::to_string(w.data.size()) +
                            ") != sector.length (" + std::to_string(w.sector.length) + ")");
        }
        if (w.sector.length == 0) {
            return bail(ErrorCode::InvalidArgument, "flash: SectorWrite.sector.length must be > 0");
        }
    }

    // Brick-safe sector ordering: if the plan specifies an
    // `integrity_check_offset` and one of the sectors contains it, move
    // that sector to the END of the iteration order. The checksum stays
    // invalid throughout the body of the flash; only the final write
    // restores it. A power loss between the first erase and the last
    // write leaves the ECU detecting corruption at boot and refusing
    // to run — the safe failure mode. See FlashPlan doc for rationale.
    std::vector<SectorWrite> writes_ordered = plan.writes;
    if (plan.integrity_check_offset.has_value()) {
        auto const off = *plan.integrity_check_offset;
        auto const it = std::find_if(
            writes_ordered.begin(), writes_ordered.end(), [off](SectorWrite const &w) {
                return w.sector.address <= off && off < w.sector.address + w.sector.length;
            });
        if (it != writes_ordered.end() && it + 1 != writes_ordered.end()) {
            std::rotate(it, it + 1, writes_ordered.end());
        }
    }

    // 1. Enter programming session.
    if (auto s = client_.diagnostic_session_control(plan.session); !s.has_value()) {
        return bail(s.error().code(), "flash: diagnostic_session_control failed: " +
                                          std::string{s.error().message()});
    }
    report.entered_session = true;

    // After every per-sector outcome update, rewrite a Manifest snapshot
    // at plan.journal_path. Best-effort: a write failure here does NOT
    // abort the flash. The point of the journal is to give the recovery
    // flow somewhere to look after a host crash; a disk failure that
    // also disabled the journal is a real possibility, but the flash
    // itself proceeding is the safer default than aborting in the
    // middle of a sector write. plan_text is empty here — the caller
    // can rebuild a complete manifest with the source plan TOML after
    // execute returns and overwrite the journal one last time.
    auto const commit_outcome = [&](SectorOutcome const &so) {
        report.sectors.push_back(so);
        if (!plan.journal_path.empty()) {
            auto const snapshot = build_manifest(plan, std::string_view{}, report);
            (void)write_manifest(plan.journal_path, snapshot);
        }
    };

    // 2. Optionally silence non-diagnostic traffic.
    if (plan.silence_bus) {
        if (auto s = client_.communication_control(ecu::uds::kCcDisableRxAndTx,
                                                   ecu::uds::kCtNormalAndNetworkManagement);
            !s.has_value()) {
            return bail(s.error().code(), "flash: communication_control(off) failed: " +
                                              std::string{s.error().message()});
        }
        report.silenced_bus = true;
    }

    // 3. Dry-run: skip every per-sector operation. Record one outcome
    // per planned sector with everything false so the caller can see
    // what would have been done.
    if (plan.dry_run) {
        for (auto const &w : writes_ordered) {
            commit_outcome(SectorOutcome{w.sector});
        }
    } else {
        // 3'. Full execution: erase + download + transfer + exit +
        // check_deps + optional verify, per sector.
        for (auto const &w : writes_ordered) {
            // Cancel check at the sector boundary — between PDUs, never
            // mid-PDU. Any in-flight transfer was on a previous sector
            // that completed RequestTransferExit before we got here, so
            // there's no download to unwind. Just exit the session.
            if (is_cancelled()) {
                cancel_cleanup(/*mid_sector_download=*/false);
                return bail(ErrorCode::Cancelled,
                            "flash: cancelled between sectors at " + hex_addr(w.sector.address));
            }

            SectorOutcome outcome{};
            outcome.sector = w.sector;

            // 3a. eraseMemory routine.
            auto erase = client_.routine_control(
                ecu::uds::kRcStart, ecu::uds::kRidEraseMemory,
                build_erase_option_record(w.sector.address, w.sector.length));
            if (!erase.has_value()) {
                commit_outcome(outcome);
                return bail(erase.error().code(), "flash: eraseMemory failed at " +
                                                      hex_addr(w.sector.address) + ": " +
                                                      std::string{erase.error().message()});
            }
            outcome.erased = true;

            // 3b. RequestDownload.
            auto rdl =
                client_.request_download(plan.data_format, w.sector.address, w.sector.length);
            if (!rdl.has_value()) {
                commit_outcome(outcome);
                return bail(rdl.error().code(), "flash: request_download failed at " +
                                                    hex_addr(w.sector.address) + ": " +
                                                    std::string{rdl.error().message()});
            }
            outcome.downloaded = true;
            std::uint32_t const block_payload = choose_block_payload(*rdl, plan.block_size_hint);
            if (block_payload == 0) {
                commit_outcome(outcome);
                return bail(ErrorCode::EcuRejected, "flash: ECU reported unusable "
                                                    "maxNumberOfBlockLength=" +
                                                        std::to_string(*rdl));
            }

            // 3c. TransferData blocks. Two wire formats: standard
            // 0x36 with a 1-byte block-sequence counter (default), or
            // 0xB6 Subaru-bulk-transfer with an explicit 3-byte address
            // per call (selected by data_format == 0x04). For the 0xB6
            // path the orchestrator advances the address by chunk size
            // between calls; there is no counter.
            bool const use_b6 = (plan.data_format == kDataFormatSubaruCiphertext);
            std::uint8_t counter = 1;
            std::size_t offset = 0;
            while (offset < w.data.size()) {
                // Cancel check at the PDU boundary, BEFORE the next
                // transfer call. At this point the previous block's
                // request-response exchange has completed and the next
                // hasn't started — nothing in flight. Mid-PDU cancel is
                // not possible (transfer call is synchronous).
                if (is_cancelled()) {
                    report.bytes_transferred += offset;
                    commit_outcome(outcome);
                    cancel_cleanup(/*mid_sector_download=*/true);
                    return bail(ErrorCode::Cancelled,
                                "flash: cancelled mid-sector at " + hex_addr(w.sector.address) +
                                    (use_b6 ? " at chunk offset 0x" + hex_addr(
                                                  static_cast<std::uint32_t>(offset))
                                            : " after block counter=" +
                                                  std::to_string(counter - 1)));
                }
                std::size_t const remaining = w.data.size() - offset;
                std::size_t const this_block =
                    remaining < block_payload ? remaining : block_payload;
                std::span<std::uint8_t const> chunk{w.data.data() + offset, this_block};
                if (use_b6) {
                    std::uint32_t const chunk_addr =
                        w.sector.address + static_cast<std::uint32_t>(offset);
                    if (auto s = client_.subaru_bulk_transfer(chunk_addr, chunk); !s.has_value()) {
                        report.bytes_transferred += offset;
                        commit_outcome(outcome);
                        return bail(s.error().code(), "flash: subaru_bulk_transfer at " +
                                                          hex_addr(chunk_addr) + " failed: " +
                                                          std::string{s.error().message()});
                    }
                } else {
                    if (auto s = client_.transfer_data(counter, chunk); !s.has_value()) {
                        report.bytes_transferred += offset;
                        commit_outcome(outcome);
                        return bail(s.error().code(),
                                    "flash: transfer_data counter=" + std::to_string(counter) +
                                        " failed: " + std::string{s.error().message()});
                    }
                    counter = static_cast<std::uint8_t>(counter + 1U);
                    // counter wraps from 0xFF to 0x00 per ISO 14229.
                }
                offset += this_block;
            }
            report.bytes_transferred += offset;
            outcome.transferred = true;

            // 3d. RequestTransferExit.
            if (auto s = client_.request_transfer_exit(); !s.has_value()) {
                commit_outcome(outcome);
                return bail(s.error().code(), "flash: request_transfer_exit failed at " +
                                                  hex_addr(w.sector.address) + ": " +
                                                  std::string{s.error().message()});
            }
            outcome.exited = true;

            // 3e. checkProgrammingDependencies routine.
            auto check = client_.routine_control(ecu::uds::kRcStart,
                                                 ecu::uds::kRidCheckProgrammingDependencies);
            if (!check.has_value()) {
                commit_outcome(outcome);
                return bail(check.error().code(), "flash: checkProgrammingDependencies "
                                                  "failed at " +
                                                      hex_addr(w.sector.address) + ": " +
                                                      std::string{check.error().message()});
            }
            outcome.check_deps_passed = true;

            // 3f. Optional verify pass.
            if (plan.verify_after_write) {
                auto readback =
                    read_full_rom(w.sector.address, w.sector.length, plan.verify_chunk_size);
                if (!readback.has_value()) {
                    outcome.verified = false;
                    commit_outcome(outcome);
                    return bail(readback.error().code(),
                                "flash: verify read-back failed: " +
                                    std::string{readback.error().message()});
                }
                outcome.verified = (readback->size() == w.data.size() &&
                                    std::equal(readback->begin(), readback->end(), w.data.begin()));
            }

            commit_outcome(outcome);
        }
    }

    // 4. Restore the bus. Best-effort: a failure here is reported but
    // not fatal because the flash itself has already completed.
    if (plan.silence_bus) {
        if (auto s = client_.communication_control(ecu::uds::kCcEnableRxAndTx,
                                                   ecu::uds::kCtNormalAndNetworkManagement);
            s.has_value()) {
            report.restored_bus = true;
        }
    }

    ExecuteOutcome out;
    out.report = std::move(report);
    return out;
}

// ---------------------------------------------------------------------
// FlashPlan TOML persistence
// ---------------------------------------------------------------------

namespace {

// Parse a hex string ("DE AD BE EF" or "DEADBEEF" or "0xDE 0xAD ...") into
// raw bytes. Whitespace and optional per-byte "0x" prefix are tolerated.
// Returns nullopt on any unparseable character.
Result<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view s) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        // Skip whitespace.
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (i >= s.size())
            break;
        // Optional "0x" / "0X" prefix per byte.
        if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i += 2;
        }
        if (i + 1 >= s.size()) {
            return failure(ErrorCode::ParseError,
                           "flash plan: odd hex digit at position " + std::to_string(i));
        }
        unsigned const first = static_cast<unsigned char>(s[i]);
        unsigned const second = static_cast<unsigned char>(s[i + 1]);
        auto const is_hex = [](unsigned c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        if (!is_hex(first) || !is_hex(second)) {
            return failure(ErrorCode::ParseError,
                           "flash plan: bad hex byte near position " + std::to_string(i));
        }
        unsigned value = 0;
        char const pair[2]{static_cast<char>(first), static_cast<char>(second)};
        auto const res = std::from_chars(pair, pair + 2, value, 16);
        if (res.ec != std::errc{} || res.ptr != pair + 2) {
            return failure(ErrorCode::ParseError,
                           "flash plan: bad hex byte near position " + std::to_string(i));
        }
        out.push_back(static_cast<std::uint8_t>(value));
        i += 2;
    }
    return out;
}

// Emit bytes as "DE AD BE EF" — uppercase, space-separated, no prefix,
// chunked into 32 bytes per line for readability on long payloads.
std::string format_hex_bytes(std::span<std::uint8_t const> bytes) {
    if (bytes.empty())
        return std::string{};
    constexpr char const *digits = "0123456789ABCDEF";
    constexpr std::size_t per_line = 32;
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            out.push_back((i % per_line == 0) ? '\n' : ' ');
        }
        out.push_back(digits[(bytes[i] >> 4) & 0xFU]);
        out.push_back(digits[bytes[i] & 0xFU]);
    }
    return out;
}

} // namespace

// ---- evaluate_plan_policy -----------------------------------------------

namespace {

// Return the byte extent of `t` in the ROM, taking grid + axes into account.
// Returns 0 when the data type can't be sized (shouldn't happen on a
// well-formed pack but the validator could in principle let it through).
std::size_t table_byte_extent(Table const &t, Definition const &def) noexcept {
    auto const cell_bytes = byte_size(t.data_type);
    if (cell_bytes == 0)
        return 0;
    std::size_t cells = 1;
    if (t.dimensions >= 1 && t.axis_x.has_value()) {
        auto const *ax = def.find_axis(*t.axis_x);
        if (ax != nullptr && ax->length > 0) {
            cells *= ax->length;
        }
    }
    if (t.dimensions >= 2 && t.axis_y.has_value()) {
        auto const *ay = def.find_axis(*t.axis_y);
        if (ay != nullptr && ay->length > 0) {
            cells *= ay->length;
        }
    }
    if (t.dimensions >= 3 && t.axis_z.has_value()) {
        auto const *az = def.find_axis(*t.axis_z);
        if (az != nullptr && az->length > 0) {
            cells *= az->length;
        }
    }
    return cells * cell_bytes;
}

// True iff `plan` writes any byte in `[start, end)` that differs from
// `source`. Bytes outside any SectorWrite or past the source's end count
// as unchanged.
bool plan_touches_changed_byte_in(FlashPlan const &plan, std::span<std::uint8_t const> source,
                                  std::size_t start, std::size_t end) noexcept {
    if (start >= end)
        return false;
    for (auto const &w : plan.writes) {
        auto const sec_start = static_cast<std::size_t>(w.sector.address);
        auto const sec_end = sec_start + w.data.size();
        auto const lo = std::max(start, sec_start);
        auto const hi = std::min(end, sec_end);
        if (lo >= hi)
            continue;
        for (auto off = lo; off < hi; ++off) {
            std::uint8_t const target_byte = w.data[off - sec_start];
            std::uint8_t const source_byte = off < source.size() ? source[off] : 0x00;
            if (target_byte != source_byte)
                return true;
        }
    }
    return false;
}

} // namespace

PolicyDecision evaluate_plan_policy(FlashPlan const &plan, Definition const &def,
                                    std::span<std::uint8_t const> source_rom,
                                    policy::Profile profile) noexcept {
    PolicyDecision result;
    for (auto const &t : def.tables()) {
        auto const extent = table_byte_extent(t, def);
        if (extent == 0)
            continue;
        auto const start = t.address;
        auto const end = start + extent;
        if (!plan_touches_changed_byte_in(plan, source_rom, start, end)) {
            continue;
        }
        if (t.engine_safety_critical) {
            result.engine_safety_tables.push_back(t.id);
        }
        if (t.emissions_relevant) {
            result.emissions_tables.push_back(t.id);
        }
    }
    // No dedup pass needed — each table appears at most once in the loop.

    result.emissions_action = result.emissions_tables.empty()
                                  ? policy::Action::Silent
                                  : policy::emissions_action(profile).on_flash;
    result.overall_action = result.engine_safety_tables.empty()
                                ? result.emissions_action
                                : policy::engine_safety_on_flash(profile);
    return result;
}

namespace {

// Load a binary file into a byte vector. Used by parse_plan when a
// [[write]] uses `data_file` instead of inline `data`.
Result<std::vector<std::uint8_t>> read_binary_file(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound,
                       "flash plan: cannot open data_file: " + path.string());
    }
    in.seekg(0, std::ios::end);
    auto const size = in.tellg();
    if (size < 0) {
        return failure(ErrorCode::IoFailure, "flash plan: cannot size data_file: " + path.string());
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
        if (!in) {
            return failure(ErrorCode::IoFailure,
                           "flash plan: short read on data_file: " + path.string());
        }
    }
    return out;
}

} // namespace

Result<FlashPlan> parse_plan(std::string_view text, std::filesystem::path const &base_dir) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError,
                       std::string{"flash plan: TOML parse: "} + e.description().data());
    }

    auto const *plan_tbl = root["plan"].as_table();
    if (plan_tbl == nullptr) {
        return failure(ErrorCode::ParseError, "flash plan: missing [plan] table");
    }

    int const schema = plan_tbl->get_as<int64_t>("schema_version")
                           ? static_cast<int>(plan_tbl->get_as<int64_t>("schema_version")->get())
                           : 0;
    if (schema != kPlanSchemaVersion) {
        return failure(ErrorCode::UnsupportedVersion,
                       "flash plan: schema_version " + std::to_string(schema) +
                           " not supported (expected " + std::to_string(kPlanSchemaVersion) + ")");
    }

    FlashPlan plan;

    auto opt_u8 = [&](char const *key, std::uint8_t fallback) {
        auto const *v = plan_tbl->get_as<int64_t>(key);
        if (v == nullptr)
            return fallback;
        auto const raw = v->get();
        if (raw < 0 || raw > 0xFF)
            return fallback;
        return static_cast<std::uint8_t>(raw);
    };
    auto opt_u32 = [&](char const *key, std::uint32_t fallback) {
        auto const *v = plan_tbl->get_as<int64_t>(key);
        if (v == nullptr)
            return fallback;
        auto const raw = v->get();
        if (raw < 0)
            return fallback;
        return static_cast<std::uint32_t>(raw);
    };
    auto opt_bool = [&](char const *key, bool fallback) {
        auto const *v = plan_tbl->get_as<bool>(key);
        return v == nullptr ? fallback : v->get();
    };

    plan.session = opt_u8("session", plan.session);
    plan.data_format = opt_u8("data_format", plan.data_format);
    plan.silence_bus = opt_bool("silence_bus", plan.silence_bus);
    plan.verify_after_write = opt_bool("verify_after_write", plan.verify_after_write);
    plan.dry_run = opt_bool("dry_run", plan.dry_run);
    plan.block_size_hint = opt_u32("block_size_hint", plan.block_size_hint);
    plan.verify_chunk_size = opt_u32("verify_chunk_size", plan.verify_chunk_size);

    // integrity_check_offset is optional — present only when the plan
    // wants the brick-safe sector reordering (checksum sector last).
    // Read as int64 so we get a clean range check and clear errors on
    // out-of-32-bit-range values.
    if (auto const *node = plan_tbl->get_as<int64_t>("integrity_check_offset")) {
        auto const v = node->get();
        if (v < 0 || v > 0xFFFFFFFFLL) {
            return failure(ErrorCode::ParseError,
                           "flash plan: integrity_check_offset out of 32-bit range");
        }
        plan.integrity_check_offset = static_cast<std::uint32_t>(v);
    }

    auto const *writes = root["write"].as_array();
    if (writes == nullptr) {
        return failure(ErrorCode::ParseError,
                       "flash plan: at least one [[write]] entry is required");
    }
    plan.writes.reserve(writes->size());
    std::size_t idx = 0;
    for (auto const &node : *writes) {
        auto const *tbl = node.as_table();
        if (tbl == nullptr) {
            return failure(ErrorCode::ParseError, "flash plan: [[write]] entry " +
                                                      std::to_string(idx) + " is not a table");
        }
        SectorWrite sw;
        auto const *addr_node = tbl->get_as<int64_t>("address");
        if (addr_node == nullptr) {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx) + ": missing 'address'");
        }
        auto const addr_val = addr_node->get();
        if (addr_val < 0 || addr_val > 0xFFFFFFFFLL) {
            return failure(ErrorCode::ParseError, "flash plan: [[write]] " + std::to_string(idx) +
                                                      ": address out of 32-bit range");
        }
        sw.sector.address = static_cast<std::uint32_t>(addr_val);

        auto const *data_node = tbl->get_as<std::string>("data");
        auto const *data_file_node = tbl->get_as<std::string>("data_file");
        if (data_node != nullptr && data_file_node != nullptr) {
            return failure(ErrorCode::ParseError, "flash plan: [[write]] " + std::to_string(idx) +
                                                      ": both 'data' and 'data_file' present "
                                                      "— pick one");
        }
        std::vector<std::uint8_t> bytes_buf;
        if (data_node != nullptr) {
            auto parsed = parse_hex_bytes(data_node->get());
            if (!parsed.has_value()) {
                return failure(parsed.error().code(), "flash plan: [[write]] " +
                                                          std::to_string(idx) + ": " +
                                                          std::string{parsed.error().message()});
            }
            bytes_buf = std::move(*parsed);
        } else if (data_file_node != nullptr) {
            std::filesystem::path file_path{data_file_node->get()};
            if (file_path.is_relative()) {
                if (base_dir.empty()) {
                    return failure(ErrorCode::InvalidArgument,
                                   "flash plan: [[write]] " + std::to_string(idx) +
                                       ": relative data_file '" + file_path.string() +
                                       "' but no base_dir supplied to "
                                       "parse_plan");
                }
                file_path = base_dir / file_path;
            }
            auto loaded = read_binary_file(file_path);
            if (!loaded.has_value()) {
                return failure(loaded.error().code(), "flash plan: [[write]] " +
                                                          std::to_string(idx) + ": " +
                                                          std::string{loaded.error().message()});
            }
            bytes_buf = std::move(*loaded);
        } else {
            return failure(ErrorCode::ParseError,
                           "flash plan: [[write]] " + std::to_string(idx) +
                               ": missing 'data' (hex string) or 'data_file' "
                               "(path to raw binary)");
        }
        if (bytes_buf.empty()) {
            return failure(ErrorCode::ParseError, "flash plan: [[write]] " + std::to_string(idx) +
                                                      ": data must contain at least one byte");
        }
        sw.sector.length = static_cast<std::uint32_t>(bytes_buf.size());
        sw.data = std::move(bytes_buf);

        plan.writes.push_back(std::move(sw));
        ++idx;
    }
    if (plan.writes.empty()) {
        return failure(ErrorCode::ParseError,
                       "flash plan: at least one [[write]] entry is required");
    }
    return plan;
}

Result<FlashPlan> read_plan(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound, "flash plan: cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_plan(ss.str(), path.parent_path());
}

std::string format_plan(FlashPlan const &plan) {
    std::ostringstream out;
    out << "# SubuwuTuner flash plan\n";
    out << "[plan]\n";
    out << "schema_version     = " << kPlanSchemaVersion << "\n";
    out << "session            = 0x" << std::hex << static_cast<unsigned>(plan.session) << std::dec
        << "\n";
    out << "data_format        = 0x" << std::hex << static_cast<unsigned>(plan.data_format)
        << std::dec << "\n";
    out << "silence_bus        = " << (plan.silence_bus ? "true" : "false") << "\n";
    out << "verify_after_write = " << (plan.verify_after_write ? "true" : "false") << "\n";
    out << "dry_run            = " << (plan.dry_run ? "true" : "false") << "\n";
    out << "block_size_hint    = " << plan.block_size_hint << "\n";
    out << "verify_chunk_size  = 0x" << std::hex << plan.verify_chunk_size << std::dec << "\n";
    // Emit integrity_check_offset only when set — keeps plans that don't
    // need brick-safe reordering free of a noise key.
    if (plan.integrity_check_offset.has_value()) {
        out << "integrity_check_offset = 0x" << std::hex << *plan.integrity_check_offset << std::dec
            << "\n";
    }

    for (auto const &w : plan.writes) {
        out << "\n[[write]]\n";
        out << "address = 0x" << std::hex << w.sector.address << std::dec << "\n";
        // Multi-line basic string (triple-quoted) so the line-wrapped hex
        // output round-trips through TOML. Basic single-line strings
        // can't contain raw newlines.
        out << "data    = \"\"\"\n" << format_hex_bytes(w.data) << "\n\"\"\"\n";
    }
    return out.str();
}

Status write_plan(std::filesystem::path const &path, FlashPlan const &plan) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure, "flash plan: cannot open for write: " + path.string());
    }
    auto const text = format_plan(plan);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "flash plan: write failed: " + path.string());
    }
    return ok();
}

// ---------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------

namespace {

std::string current_iso8601_utc() {
    auto const now = std::chrono::system_clock::now();
    std::time_t const t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

std::uint32_t crc32_of(std::string_view text) noexcept {
    return st::crc32({reinterpret_cast<std::uint8_t const *>(text.data()), text.size()});
}

} // namespace

Manifest build_manifest(FlashPlan const &plan, std::string_view plan_text,
                        FlashReport const &report) {
    Manifest m;
    m.schema_version = kManifestSchemaVersion;
    m.created_at = current_iso8601_utc();
    m.plan_crc32 = crc32_of(plan_text);

    // Build per-sector entries by matching writes to outcomes positionally.
    // Outcomes that don't have a matching write (or vice versa) shouldn't
    // happen with the current orchestrator, but we tolerate the size mismatch
    // and emit only what aligns.
    auto const n =
        plan.writes.size() < report.sectors.size() ? plan.writes.size() : report.sectors.size();
    m.entries.reserve(n);

    // Overall CRC is computed over the concatenation of every transferred
    // sector's bytes, in plan order. If an entry was not transferred (dry
    // run or a mid-flash failure), it contributes nothing to the overall
    // hash — only its per-sector CRC is recorded.
    Crc32 overall;
    for (std::size_t i = 0; i < n; ++i) {
        auto const &w = plan.writes[i];
        auto const &so = report.sectors[i];
        ManifestEntry e;
        e.sector = w.sector;
        e.data_crc32 = st::crc32(w.data);
        e.transferred = so.transferred;
        e.verified = so.verified;
        if (e.transferred) {
            overall.update(w.data);
        }
        m.entries.push_back(e);
    }
    m.overall_crc32 = overall.value();
    return m;
}

Result<Manifest> parse_manifest(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError,
                       std::string{"flash manifest: TOML parse: "} + e.description().data());
    }

    auto const *sv = root.get_as<int64_t>("schema_version");
    int const schema = (sv != nullptr) ? static_cast<int>(sv->get()) : 0;
    if (schema != kManifestSchemaVersion) {
        return failure(ErrorCode::UnsupportedVersion,
                       "flash manifest: schema_version " + std::to_string(schema) +
                           " not supported (expected " + std::to_string(kManifestSchemaVersion) +
                           ")");
    }

    Manifest m;
    m.schema_version = schema;

    if (auto const *t = root.get_as<std::string>("created_at"); t != nullptr) {
        m.created_at = t->get();
    }
    if (auto const *t = root.get_as<int64_t>("plan_crc32"); t != nullptr) {
        m.plan_crc32 = static_cast<std::uint32_t>(t->get());
    }
    if (auto const *t = root.get_as<int64_t>("overall_crc32"); t != nullptr) {
        m.overall_crc32 = static_cast<std::uint32_t>(t->get());
    }
    if (auto const *t = root.get_as<std::string>("policy_profile"); t != nullptr) {
        m.policy_profile = t->get();
    }
    if (auto const *t = root.get_as<std::string>("policy_reason"); t != nullptr) {
        m.policy_reason = t->get();
    }

    auto const *entries = root["entry"].as_array();
    if (entries != nullptr) {
        for (auto const &node : *entries) {
            auto const *tbl = node.as_table();
            if (tbl == nullptr)
                continue;
            ManifestEntry e;
            if (auto const *a = tbl->get_as<int64_t>("address"); a != nullptr) {
                e.sector.address = static_cast<std::uint32_t>(a->get());
            }
            if (auto const *l = tbl->get_as<int64_t>("length"); l != nullptr) {
                e.sector.length = static_cast<std::uint32_t>(l->get());
            }
            if (auto const *c = tbl->get_as<int64_t>("data_crc32"); c != nullptr) {
                e.data_crc32 = static_cast<std::uint32_t>(c->get());
            }
            if (auto const *b = tbl->get_as<bool>("transferred"); b != nullptr) {
                e.transferred = b->get();
            }
            if (auto const *b = tbl->get_as<bool>("verified"); b != nullptr) {
                e.verified = b->get();
            }
            m.entries.push_back(e);
        }
    }
    return m;
}

Result<Manifest> read_manifest(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound, "flash manifest: cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_manifest(ss.str());
}

std::string format_manifest(Manifest const &m) {
    auto const escape = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    std::ostringstream out;
    out << "# SubuwuTuner flash manifest\n";
    out << "schema_version = " << kManifestSchemaVersion << "\n";
    out << "created_at     = \"" << m.created_at << "\"\n";
    out << "plan_crc32     = 0x" << std::hex << m.plan_crc32 << std::dec << "\n";
    out << "overall_crc32  = 0x" << std::hex << m.overall_crc32 << std::dec << "\n";
    if (!m.policy_profile.empty()) {
        out << "policy_profile = \"" << escape(m.policy_profile) << "\"\n";
    }
    if (!m.policy_reason.empty()) {
        out << "policy_reason  = \"" << escape(m.policy_reason) << "\"\n";
    }
    for (auto const &e : m.entries) {
        out << "\n[[entry]]\n";
        out << "address     = 0x" << std::hex << e.sector.address << std::dec << "\n";
        out << "length      = " << e.sector.length << "\n";
        out << "data_crc32  = 0x" << std::hex << e.data_crc32 << std::dec << "\n";
        out << "transferred = " << (e.transferred ? "true" : "false") << "\n";
        out << "verified    = " << (e.verified ? "true" : "false") << "\n";
    }
    return out.str();
}

Status write_manifest(std::filesystem::path const &path, Manifest const &m) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "flash manifest: cannot open for write: " + path.string());
    }
    auto const text = format_manifest(m);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "flash manifest: write failed: " + path.string());
    }
    return ok();
}

// ---------------------------------------------------------------------
// plan_resume
// ---------------------------------------------------------------------

Result<FlashPlan> plan_resume(FlashPlan const &original, Manifest const &journal) {
    if (journal.entries.size() > original.writes.size()) {
        return failure(ErrorCode::ParseError,
                       "flash resume: journal has " + std::to_string(journal.entries.size()) +
                           " entries but plan has only " + std::to_string(original.writes.size()) +
                           " writes; mismatched plan/journal pair");
    }

    FlashPlan resumed = original;
    resumed.writes.clear();
    resumed.writes.reserve(original.writes.size());

    for (std::size_t i = 0; i < original.writes.size(); ++i) {
        auto const &w = original.writes[i];
        bool done = false;
        if (i < journal.entries.size()) {
            auto const &e = journal.entries[i];
            if (e.sector != w.sector) {
                return failure(ErrorCode::ParseError,
                               "flash resume: journal entry " + std::to_string(i) +
                                   " sector mismatch (plan address 0x" +
                                   std::to_string(w.sector.address) + ", journal address 0x" +
                                   std::to_string(e.sector.address) + ")");
            }
            if (e.transferred && e.verified) {
                auto const expected_crc = st::crc32(w.data);
                if (e.data_crc32 != expected_crc) {
                    return failure(ErrorCode::BadChecksum, "flash resume: journal entry " +
                                                               std::to_string(i) +
                                                               " claims success but data CRC32 "
                                                               "mismatches; plan was modified "
                                                               "between flashes");
                }
                done = true;
            }
        }
        if (!done) {
            resumed.writes.push_back(w);
        }
    }
    return resumed;
}

} // namespace st::flash
