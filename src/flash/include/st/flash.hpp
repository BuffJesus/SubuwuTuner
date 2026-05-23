// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_FLASH_HPP
#define ST_FLASH_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/ecu/ssm.hpp"
#include "st/ecu/uds.hpp"
#include "st/policy.hpp"
#include "st/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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
    Sector sector{};
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

    // When non-empty, `Flasher::execute` rewrites a Manifest at this
    // path after every per-sector outcome update. This is the
    // resume-from-crash foundation per docs/05-improvements.md §4: if
    // the host process dies mid-flash, the journal on disk reflects the
    // last successfully-completed sector. Write failure on the journal
    // is best-effort — the flash itself proceeds regardless, so a full
    // disk does not turn into a brick. The path is environment-specific
    // and is deliberately NOT round-tripped through plan TOML; set it
    // programmatically or via a CLI flag.
    std::filesystem::path journal_path{};

    std::vector<SectorWrite> writes;
};

// Per-sector outcome surfaced through `FlashReport`. Every boolean
// reports the success of a discrete UDS exchange so a partial failure
// can be diagnosed without re-running the flash.
struct SectorOutcome {
    Sector sector{};
    bool erased{false};
    bool downloaded{false};
    bool transferred{false};
    bool exited{false};
    bool check_deps_passed{false};
    bool verified{true}; // always true when verify_after_write=false
};

// Result of `Flasher::execute`. The orchestrator returns this even on
// partial failures so the caller can see which sector died at which
// step.
struct FlashReport {
    bool entered_session{false};
    bool silenced_bus{false};
    bool restored_bus{false};
    std::vector<SectorOutcome> sectors;
    std::size_t bytes_transferred{0};

    [[nodiscard]] bool all_sectors_completed() const noexcept;
    [[nodiscard]] bool all_sectors_verified() const noexcept;
};

// What `Flasher::execute` returns. The report is ALWAYS populated, even
// on failure — which lets callers diagnose where in the flash sequence
// the error happened (entered_session false ↔ DSC failed; sectors[i]
// with erased=true, downloaded=false ↔ RequestDownload failed on
// sector i, etc.). When `error` is nullopt the flash succeeded; when
// set, its `code()` and `message()` describe what went wrong.
//
// Previously `execute` returned `Result<FlashReport>` and the failure
// path discarded the report entirely. The journal mechanism captured
// partial state on disk per-sector, but session-level failures (DSC,
// initial CommunicationControl) left no trace at all. ExecuteOutcome
// fixes that by guaranteeing in-memory visibility into partial state
// regardless of where in the sequence the failure occurred.
struct ExecuteOutcome {
    FlashReport report;
    std::optional<Error> error;

    [[nodiscard]] bool ok() const noexcept {
        return !error.has_value();
    }
};

class Flasher {
public:
    // Constructed once per transport; the Flasher exposes both a UDS
    // read path (`read_full_rom`) for ECUs that speak ISO 14229 (Subaru
    // VB and newer) and an SSM read path (`read_full_rom_ssm`) for
    // ECUs that speak Subaru Select Monitor — which is every CAN-era
    // VA WRX. The clients share the transport; only one is ever in
    // flight at a time (caller picks).
    //
    // `ssm_framing` picks the SSM wire format. Default is K-Line for
    // backward compatibility with pre-CAN transports + existing tests.
    // GUI / CLI callers that opened a `LinkKind::CanIso15765` transport
    // MUST pass `ecu::ssm::Framing::IsoTp` — emitting K-Line frames on
    // CAN causes the ECU to silently drop the request (diagnosed on a
    // real 2017 WRX, 2026-05-23).
    explicit Flasher(transport::ITransport &t,
                     ecu::ssm::Framing ssm_framing = ecu::ssm::Framing::KLine) noexcept
        : client_{t}, ssm_client_{t, ssm_framing} {}

    // Read a contiguous span of ECU memory via ReadMemoryByAddress,
    // chunked into `max_chunk_size`-byte requests. Returns the
    // concatenated bytes in the requested order. Errors at any chunk
    // abort the read and surface through the Result.
    //
    // The optional `progress` callback fires AFTER each successful chunk
    // with the running (bytes_done, total) tuple. UI callers wire this
    // to an atomic counter + a progress bar; CLI callers can no-op it.
    // The callback runs on the caller's thread (i.e. the worker thread
    // for a GUI background read) — keep it cheap (single-atomic-store
    // is fine; no allocation).
    //
    // The optional `cancel` flag is polled BEFORE each chunk. If set,
    // the read aborts with ErrorCode::Cancelled and the partial buffer
    // is discarded — the caller does not get back the bytes read so far.
    // Atomic so multi-threaded readers can flip it from a UI thread.
    struct ReadProgress {
        std::uint32_t bytes_done;
        std::uint32_t total_bytes;
    };
    using ReadProgressFn = std::function<void(ReadProgress)>;

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    read_full_rom(std::uint32_t base_address, std::uint32_t total_length,
                  std::uint32_t max_chunk_size = 0x100,
                  std::chrono::milliseconds per_chunk_timeout = std::chrono::milliseconds{1000},
                  ReadProgressFn progress = nullptr, std::atomic<bool> const *cancel = nullptr);

    // SSM (Subaru Select Monitor) variant of read_full_rom — uses
    // SSM 0xA8 (ReadByAddress) instead of UDS 0x23. This is the path
    // for VA WRX (and every other CAN-era Subaru that hasn't moved to
    // UDS).
    //
    // Wire framing depends on the `ssm_framing` passed to the Flasher
    // constructor:
    //   * KLine — `80 10 F0 LEN A8 00 <addrs> CSUM` over ISO 9141 serial
    //     (Tactrix OpenPort, pre-2008 Subarus).
    //   * IsoTp — `A8 00 <addrs>` raw payload over CAN ID 0x7E0 with
    //     the OBDX adapter handling ISO-TP segmentation. The K-Line
    //     wrapper is stripped because CAN+ISO-TP supersedes it.
    //
    // `max_chunk_size` is silently clamped to the SSM single-frame
    // limit (80 bytes ≈ 84 addresses; we use 80 for headroom). Tooling
    // can pass 256 or 4096 — we'll do the right thing internally. SSM
    // addresses are 24 bits; `base_address + total_length` must stay
    // within `0x00FFFFFF`.
    //
    // Speed: ~10–30 KB/s on a healthy adapter (one SSM round-trip per
    // chunk; ISO-TP fragmentation handled by the adapter). A 1 MiB ROM
    // takes 5–15 minutes; surface progress + cancel exactly as the UDS
    // variant does.
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    read_full_rom_ssm(std::uint32_t base_address, std::uint32_t total_length,
                      std::uint32_t max_chunk_size = 0x40,
                      std::chrono::milliseconds per_chunk_timeout = std::chrono::milliseconds{1500},
                      ReadProgressFn progress = nullptr,
                      std::atomic<bool> const *cancel = nullptr);

    // Walk `current` and `target` in `sector_size`-aligned chunks; emit
    // a Sector for every chunk whose bytes differ. Both spans must be
    // the same length and a multiple of `sector_size` (the last sector
    // is short if not, and is included whole). Pure function; no
    // transport calls.
    [[nodiscard]] static std::vector<Sector> compute_delta(std::span<std::uint8_t const> current,
                                                           std::span<std::uint8_t const> target,
                                                           std::uint32_t sector_size = 0x1000,
                                                           std::uint32_t base_address = 0);

    // Execute a plan. Always returns an `ExecuteOutcome` whose `report`
    // reflects in-progress state up to wherever the sequence stopped;
    // `error` is set iff the flash did not complete successfully.
    //
    // The optional `cancel` flag is polled at PDU boundaries: between
    // sectors AND between TransferData blocks within a sector. It is
    // NEVER polled mid-PDU — once a UDS request is on the wire, the
    // in-flight exchange completes before the cancel check fires. This
    // is load-bearing: a UDS request torn mid-PDU can leave the ECU in
    // an inconsistent download/erase state. Cancel arrives between
    // PDUs, not within one (docs/08 Tier 2a).
    //
    // On observed cancel:
    //   * If mid-sector (RequestDownload sent but RequestTransferExit
    //     not yet sent), emit RequestTransferExit so the ECU unwinds
    //     its download state machine cleanly. Best-effort — a failure
    //     here is not surfaced.
    //   * Always emit DiagnosticSessionControl → kDscDefault so the
    //     ECU exits the programming session. Best-effort — a failure
    //     is not surfaced.
    //   * Return ExecuteOutcome with `error.code() == Cancelled` and
    //     a partial `report` describing what completed before cancel.
    //
    // Atomic so a UI thread can flip the flag while a worker thread
    // runs execute(). Mirror of read_full_rom's cancel parameter.
    [[nodiscard]] ExecuteOutcome execute(FlashPlan const &plan,
                                         std::atomic<bool> const *cancel = nullptr);

private:
    ecu::uds::UdsClient client_;
    ecu::ssm::SsmClient ssm_client_;
};

// =====================================================================
// Plan-vs-policy evaluation
// =====================================================================
//
// Before executing a plan against a real ECU, the host has to decide:
//   1. Does the plan touch any engine-safety-critical table?
//      If yes -> Block (every profile blocks engine-safety per docs/06).
//   2. Does the plan touch any emissions-relevant table?
//      If yes -> profile-dependent action (silent / confirm / etc.).
//
// `evaluate_plan_policy` does both: walks every `SectorWrite` in `plan`,
// diffs each byte against `source_rom`, maps the changed-byte ranges to
// tables via `def`, and accumulates the union of flagged table ids.
// Pure function — no transport calls, no I/O.

struct PolicyDecision {
    // Distinct table ids whose `engine_safety_critical = true` AND whose
    // byte extent overlaps a sector in the plan with at least one
    // changed byte.
    std::vector<std::string> engine_safety_tables;

    // Same for `emissions_relevant = true`.
    std::vector<std::string> emissions_tables;

    // The Action `policy::emissions_action(profile).on_flash` resolves
    // to, given this plan's contents. `Silent` when no emissions tables
    // are touched, regardless of profile.
    policy::Action emissions_action{policy::Action::Silent};

    // Combined verdict: `Block` if any engine-safety table is touched,
    // else the emissions action. This is the value a caller should
    // compare against to decide whether to proceed / require a
    // confirmation / refuse.
    policy::Action overall_action{policy::Action::Silent};
};

[[nodiscard]] PolicyDecision evaluate_plan_policy(FlashPlan const &plan, Definition const &def,
                                                  std::span<std::uint8_t const> source_rom,
                                                  policy::Profile profile) noexcept;

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
// Each `[[write]]` carries `data` (inline hex string) OR `data_file`
// (path to a raw binary), never both. `data_file` is the right choice
// for realistic flash payloads (64 KB sectors become 192 KB of hex
// otherwise). Relative `data_file` paths resolve against `base_dir`
// — `read_plan` passes the plan file's parent directory automatically;
// callers of `parse_plan` who pass plan text from a string must supply
// `base_dir` explicitly if their plans contain relative `data_file`
// references.
//
// Round-trip: a plan loaded via `data_file` formats back as inline
// `data`. The in-memory `FlashPlan` is authoritative; TOML is one
// serialization. Users who want `data_file` persisted across a
// load/save cycle hand-edit the result.
//
// Round-trip stable for plans without `data_file`:
// `parse_plan(format_plan(p))` yields a plan whose fields all match
// `p`. The schema_version field exists so future schema changes can
// be detected and rejected cleanly.

inline constexpr int kPlanSchemaVersion = 1;

[[nodiscard]] Result<FlashPlan> parse_plan(std::string_view text,
                                           std::filesystem::path const &base_dir = {});

[[nodiscard]] Result<FlashPlan> read_plan(std::filesystem::path const &path);

[[nodiscard]] std::string format_plan(FlashPlan const &plan);

[[nodiscard]] Status write_plan(std::filesystem::path const &path, FlashPlan const &plan);

// =====================================================================
// Manifest — tamper-evident record of an executed flash
// =====================================================================
//
// Per docs/05-improvements.md §4: every flash operation publishes a
// manifest the user can keep. The manifest stores per-sector hashes of
// the bytes that were (or would have been) transferred, an overall hash
// of the concatenation, a hash of the source plan TOML, and an ISO-8601
// timestamp. Useful for audit ("on this date, this plan was flashed
// with these resulting sector hashes") and as the data foundation for
// future resume-from-crash support.
//
// Current hash: 32-bit CRC (st::crc32, IEEE 802.3). This is detection
// of accidental corruption, NOT cryptographic tamper-evidence. The
// upgrade path is BLAKE3 once the bench rig lands and we have a real
// signing flow — see docs/05 §4 for the full threat model.
//
// `build_manifest` is the canonical producer: given the executed plan,
// the source plan TOML (for plan_crc32), and the resulting FlashReport
// (for transferred / verified flags), it returns a complete Manifest.
// Build it AFTER Flasher::execute returns — this slice does not yet
// support incremental persistence during a flash.

struct ManifestEntry {
    Sector sector{};
    std::uint32_t data_crc32{0};
    bool transferred{false};
    bool verified{false};
};

struct Manifest {
    int schema_version{1};
    // Free-form ISO-8601 UTC timestamp ("2026-05-12T15:30:00Z"). Opaque
    // to the parser; round-tripped as a string.
    std::string created_at;
    // CRC32 of the source plan TOML text, end-to-end. Lets a later
    // verifier confirm the manifest was produced from a specific plan
    // without storing the whole plan.
    std::uint32_t plan_crc32{0};
    // CRC32 of the concatenation of every entry's transferred bytes, in
    // plan order. Independent of the per-entry hashes; both are stored
    // so a tampered single-sector value is detectable two ways.
    std::uint32_t overall_crc32{0};
    // Audit-trail fields populated by the CLI when a policy-gated flash
    // proceeds under a profile that demands `Confirm` / `ConfirmWithReason`
    // (see `docs/06-legal-ethics.md`). `policy_profile` is the active
    // jurisdiction profile at flash time; `policy_reason` is the user-
    // supplied justification for emissions-flagged edits. Both empty when
    // no policy gate was applied (e.g. `flash-apply` without `--profile`
    // on motorsport-only).
    std::string policy_profile;
    std::string policy_reason;
    std::vector<ManifestEntry> entries;
};

inline constexpr int kManifestSchemaVersion = 1;

// Construct a manifest after a successful (or partially-successful)
// flash. `plan_text` should be the exact source TOML the plan was
// loaded from — passing format_plan(plan) is fine for plans built in
// memory but loses any user comments / whitespace.
[[nodiscard]] Manifest build_manifest(FlashPlan const &plan, std::string_view plan_text,
                                      FlashReport const &report);

[[nodiscard]] Result<Manifest> parse_manifest(std::string_view text);
[[nodiscard]] Result<Manifest> read_manifest(std::filesystem::path const &path);
[[nodiscard]] std::string format_manifest(Manifest const &m);
[[nodiscard]] Status write_manifest(std::filesystem::path const &path, Manifest const &m);

// =====================================================================
// Resume-from-journal
// =====================================================================
//
// `plan_resume` closes the docs/05-improvements.md §4 resume-from-crash
// loop: given an original `FlashPlan` and a `Manifest` produced by a
// prior partial execution (the journal on disk), return a new plan
// containing only the sectors that did NOT complete successfully. The
// recovery flow's caller then executes that resumed plan as usual.
//
// A sector is considered "done" iff its journal entry has both
// `transferred=true` AND `verified=true` AND the entry's `data_crc32`
// matches the CRC32 of the original plan's bytes for that write.
//
// Errors:
//   * `BadChecksum` — the journal claims a sector was transferred and
//     verified, but the original plan's bytes hash to a different
//     CRC32 than the journal records. The plan was modified between
//     the first execution and the resume attempt — refusing here is
//     the safe default (re-flashing with the new bytes risks
//     overwriting a sector with mismatched data without re-verifying).
//   * `ParseError` — the journal has more entries than the plan has
//     writes, or at any matching index the sector (address, length)
//     differs between plan and journal. Indicates the journal was
//     produced from a different plan than the one passed in.
//
// The resumed plan inherits all options from the original (session,
// data_format, dry_run, silence_bus, verify_after_write,
// block_size_hint, verify_chunk_size, journal_path). Callers typically
// set a fresh `journal_path` before executing the resumed plan so the
// original journal stays intact for audit.
[[nodiscard]] Result<FlashPlan> plan_resume(FlashPlan const &original, Manifest const &journal);

} // namespace st::flash

#endif // ST_FLASH_HPP
