// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_FLASH_HPP
#define ST_FLASH_HPP

#include "st/core/result.hpp"
#include "st/ecu/uds.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// =====================================================================
// st::flash — read/erase/program/verify orchestration for UDS-capable
//             Subaru ECUs (VB and newer). Built on top of UdsClient.
// =====================================================================
//
// This module sits above `st::ecu::uds` and below the CLI / GUI. It owns
// the *sequence* of UDS calls that make up a flash operation; it does
// not know about K-Line, CAN-TP, definition packs, or projects.
//
// Safety posture (docs/05-improvements.md §4, docs/08-testing-strategy.md
// Tier 4): every operation that touches the flash channel publishes
// enough state through `FlashReport` that a host-side crash mid-flash
// can be reasoned about post-mortem. Brick-protection (signed sectors,
// recovery-shim verification) is a separate slice that layers on top.

namespace st::flash {

// A contiguous flash region. How it maps to physical sectors on the ECU
// is platform-specific and lives in a definition pack — `Sector` itself
// is a plain {address, length} pair.
struct Sector {
    std::uint32_t address{0};
    std::uint32_t length{0};

    [[nodiscard]] bool operator==(Sector const &) const noexcept = default;
};

// One contiguous write within a plan: which range, and the replacement
// bytes. `data.size()` must equal `sector.length`.
struct SectorWrite {
    Sector                    sector{};
    std::vector<std::uint8_t> data;
};

// A flash plan: enter programming session, erase + reprogram each
// listed `SectorWrite`, optionally verify, exit session. Built by hand
// or via `Flasher::compute_delta`.
struct FlashPlan {
    // Diagnostic session id to enter before flashing. ISO 14229 says
    // programming is sub-function 0x02; some Subaru ECUs require
    // extendedDiagnostic (0x03) first — we leave that orchestration to
    // a caller that knows the platform.
    std::uint8_t session{ecu::uds::kDscProgramming};

    // RequestDownload's dataFormatIdentifier. High nibble = compression
    // method, low nibble = encryption method. 0x00 is the most common
    // case for stock Subaru flashing tools (no compression, no
    // encryption).
    std::uint8_t data_format{0x00};

    // When true, the plan probes the ECU's session-level acceptance
    // only: enter programming session, gate the bus, ungate the bus,
    // exit. NO per-sector operation (erase, RequestDownload,
    // TransferData, RequestTransferExit, checkProgrammingDependencies)
    // is attempted. This is the only flash-safe form of partial
    // execution — exercising erase without the subsequent write would
    // leave a sector empty, which on most Subaru ECUs means a brick.
    // Use dry_run to validate connectivity, security access, and
    // session-level rejection codes before risking a write.
    bool dry_run{false};

    // When true, each completed sector is re-read with
    // ReadMemoryByAddress and compared against `data`. A mismatch
    // surfaces in the report's `verify_passed` flag.
    bool verify_after_write{true};

    // Whether to silence non-diagnostic CAN traffic during the flash
    // window via CommunicationControl. Defaults on — keeps unrelated
    // ECUs from re-asserting addresses while a flash is in flight.
    bool silence_bus{true};

    // Per-TransferData payload size, in bytes. The ECU reports its own
    // `maxNumberOfBlockLength` via RequestDownload's positive response;
    // the orchestrator caps the actual payload at
    // `min(reported_max - 2, block_size_hint)` where the -2 accounts for
    // the SID + block-sequence-counter overhead. Set to 0 to use the
    // ECU's reported maximum unmodified.
    std::uint32_t block_size_hint{0};

    // Per-chunk size for the verify-pass ReadMemoryByAddress loop.
    std::uint32_t verify_chunk_size{0x100};

    std::vector<SectorWrite> writes;
};

// Per-sector outcome surfaced through `FlashReport`. Every boolean
// reports the success of a discrete UDS exchange so a partial failure
// can be diagnosed without re-running the flash.
struct SectorOutcome {
    Sector sector{};
    bool   erased{false};
    bool   downloaded{false};
    bool   transferred{false};
    bool   exited{false};
    bool   check_deps_passed{false};
    bool   verified{true};  // always true when verify_after_write=false
};

// Result of `Flasher::execute`. The orchestrator returns this even on
// partial failures so the caller can see which sector died at which
// step.
struct FlashReport {
    bool                        entered_session{false};
    bool                        silenced_bus{false};
    bool                        restored_bus{false};
    std::vector<SectorOutcome>  sectors;
    std::size_t                 bytes_transferred{0};

    [[nodiscard]] bool all_sectors_completed() const noexcept;
    [[nodiscard]] bool all_sectors_verified() const noexcept;
};

class Flasher {
  public:
    explicit Flasher(transport::ITransport &t) noexcept : client_{t} {}

    // Read a contiguous span of ECU memory via ReadMemoryByAddress,
    // chunked into `max_chunk_size`-byte requests. Returns the
    // concatenated bytes in the requested order. Errors at any chunk
    // abort the read and surface through the Result.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_full_rom(
        std::uint32_t             base_address,
        std::uint32_t             total_length,
        std::uint32_t             max_chunk_size = 0x100,
        std::chrono::milliseconds per_chunk_timeout =
            std::chrono::milliseconds{1000});

    // Walk `current` and `target` in `sector_size`-aligned chunks; emit
    // a Sector for every chunk whose bytes differ. Both spans must be
    // the same length and a multiple of `sector_size` (the last sector
    // is short if not, and is included whole). Pure function; no
    // transport calls.
    [[nodiscard]] static std::vector<Sector> compute_delta(
        std::span<std::uint8_t const> current,
        std::span<std::uint8_t const> target,
        std::uint32_t                 sector_size = 0x1000,
        std::uint32_t                 base_address = 0);

    // Execute a plan. Always returns the in-progress `FlashReport` even
    // when the wrapped error is non-empty, so the caller can inspect
    // partial-failure state.
    [[nodiscard]] Result<FlashReport> execute(FlashPlan const &plan);

  private:
    ecu::uds::UdsClient client_;
};

// =====================================================================
// FlashPlan TOML persistence
// =====================================================================
//
// A plan TOML carries one `[plan]` table with the session/options flags
// plus one `[[write]]` entry per sector. Sector data is stored as a hex
// string of whitespace-separated byte pairs (optionally with a leading
// "0x" prefix per byte). `address` and the per-byte values are TOML
// integers, so `0x` literals are accepted natively by the parser.
//
// Example:
//
//   [plan]
//   schema_version     = 1
//   session            = 0x02
//   data_format        = 0x00
//   silence_bus        = true
//   verify_after_write = true
//   block_size_hint    = 0
//   verify_chunk_size  = 0x100
//   dry_run            = false
//
//   [[write]]
//   address = 0x00001234
//   data    = "DE AD BE EF"
//
// Round-trip stable: `parse_plan(format_plan(p))` yields a plan whose
// fields all match `p`. The schema_version field exists so future
// schema changes can be detected and rejected cleanly.

inline constexpr int kPlanSchemaVersion = 1;

[[nodiscard]] Result<FlashPlan> parse_plan(std::string_view text);

[[nodiscard]] Result<FlashPlan> read_plan(std::filesystem::path const &path);

[[nodiscard]] std::string format_plan(FlashPlan const &plan);

[[nodiscard]] Status write_plan(std::filesystem::path const &path,
                                FlashPlan const             &plan);

} // namespace st::flash

#endif // ST_FLASH_HPP
