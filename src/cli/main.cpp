// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"
#include "st/can.hpp"
#include "st/config.hpp"
#include "st/core/crc32.hpp"
#include "st/core/json_util.hpp"
#include "st/core/version.hpp"
#include "st/dbc.hpp"
#include "st/audit.hpp"
#include "st/defs.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/diff.hpp"
#include "st/profile.hpp"
#include "st/discover.hpp"
#include "st/ecu/bulk_reflash_cipher.hpp"
#include "st/ecu/extended_did_datalog.hpp"
#include "st/ecu/ssm.hpp"
#include "st/ecu/subaru_security.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/feature_codegen.hpp"
#include "st/feature_ir.hpp"
#include "st/flash.hpp"
#include "st/flash/checksum.hpp"
#include "st/log.hpp"
#include "st/ai/backend.hpp"
#include "st/ai/drift.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/policy.hpp"
#include "st/policy/flash_preflight.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"
#include "st/devices/ets/architectural_classifier.hpp"
#include "st/devices/ets/client.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/devices/ets/ota_cipher.hpp"
#include "st/devices/ets/ptm_cipher.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/ptm_xml_builder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/library/tune_diff.hpp"

#include <toml++/toml.hpp>
#include "st/transport/ets_channel.hpp"
#include "st/transport/ets_packet.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/j2534_discovery.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include <algorithm>
#include <regex>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Defined far below alongside the other autotune CLI helpers (which all
// sit at file scope, outside the anonymous namespace). Forward-declared
// here so callers inside the anonymous namespace below can find it.
std::optional<double> parse_fraction_or_percent(std::string_view raw);

namespace {

using st::json_escape;

constexpr std::string_view kUsage =
    "subuwutuner-cli — headless ECU calibration tool\n"
    "\n"
    "USAGE:\n"
    "    subuwutuner-cli [OPTIONS] [COMMAND] [ARGS...]\n"
    "\n"
    "OPTIONS:\n"
    "    -h, --help              Print this help and exit\n"
    "    -V, --version           Print version and exit\n"
    "    --enable-bulk-reflash-cipher\n"
    "                            Arm an optional advanced write path for this\n"
    "                            invocation. Off by default. See\n"
    "                            docs/26-bulk-reflash-cipher.md.\n"
    "\n"
    "COMMANDS:\n"
    "    pack-list [<dir>] [--quiet]\n"
    "                            Walk a directory for definition packs (recursive\n"
    "                            scan for nested pack.toml files; defaults to the\n"
    "                            definitions_root from `config show` when <dir>\n"
    "                            is omitted) and print one line per pack with id,\n"
    "                            display name, platform, and content counts.\n"
    "                            Companion to rom-identify for browsing a pack\n"
    "                            collection without needing a ROM in hand.\n"
    "                            --quiet suppresses load-failure noise for\n"
    "                            unparseable packs.\n"
    "    rom-identify <FILE.bin> --pack-dir <dir> [--quiet] [--json]\n"
    "                            Walk a directory of definition packs (recursive\n"
    "                            scan for nested pack.toml files) and report which\n"
    "                            ones match the ROM's CID. For \"I just got this\n"
    "                            ROM, which pack do I use?\" Prints one line per\n"
    "                            matching pack with the matched identification\n"
    "                            name; --quiet suppresses the load-failure noise\n"
    "                            for packs that don't parse cleanly. --json emits\n"
    "                            a one-line subuwutuner.rom-identify.v1 object\n"
    "                            with the full matches[] + failed[] arrays for\n"
    "                            CI scripts. Exit 0 on ≥1 match, 1 on zero\n"
    "                            matches, 2 on argument errors.\n"
    "    checksum-repair <FILE.bin> --def <pack> --output <FILE.bin>\n"
    "                            Run the pack's declared checksum_type repair\n"
    "                            algorithm against the ROM and write the result\n"
    "                            to --output. Read-only on the input. Exit 0 on\n"
    "                            success (even when the repair is a no-op for\n"
    "                            'none' kinds — output bytes still get written\n"
    "                            for a clean copy semantic); 3 when the declared\n"
    "                            kind's algorithm isn't yet implemented.\n"
    "    checksum-verify <FILE.bin> --def <pack>\n"
    "                            Run the pack's declared checksum_type repair\n"
    "                            algorithm against a copy of the ROM and report\n"
    "                            whether the existing checksum bytes are already\n"
    "                            valid. Exit 0 = valid (or pack declares 'none');\n"
    "                            1 = invalid (would be repaired on flash); 3 =\n"
    "                            algorithm not yet implemented for the declared\n"
    "                            kind. Read-only — never modifies the ROM file.\n"
    "    rom-info [--def <pack.toml>] [--json] <FILE>\n"
    "                            Print size, CRC32, embedded ASCII strings of a ROM.\n"
    "                            With --def, also identify the ROM against the pack\n"
    "                            and summarize its tables. --json emits a one-line\n"
    "                            subuwutuner.rom-info.v1 object instead (skips the\n"
    "                            ASCII listing — text mode is for exploratory\n"
    "                            browsing, JSON for CI scripts).\n"
    "    dump-axis --def <pack.toml> --axis <id> [--csv|--json] <FILE>\n"
    "                            Read the named axis from the ROM via the pack and\n"
    "                            print its scaled values, one per line.\n"
    "    dump-table --def <pack.toml> --table <id> [--csv|--json] <FILE>\n"
    "                            Read the named table from the ROM via the pack and\n"
    "                            print it as a labeled grid (or CSV with --csv).\n"
    "    rom-diff --def <pack.toml> [--json] [--verbose] <A.bin> <B.bin>\n"
    "                            Compare two ROMs of the same definition table-by-\n"
    "                            table. Reports which tables changed and by how\n"
    "                            much (max delta, mean absolute delta). --json\n"
    "                            emits a subuwutuner.rom-diff.v1 summary envelope.\n"
    "    diff --pack <pack.toml> [--format text|csv|json] [--output FILE]\n"
    "         [--include-identical] [--include-unchanged] [--save SESSION.stcompare]\n"
    "         <A.bin> <B.bin>\n"
    "                            Structured ROM compare via st::diff. Per-cell\n"
    "                            change list (not just aggregate stats), plus\n"
    "                            text / CSV / JSON output formats for scripting.\n"
    "                            --save also writes a .stcompare session file\n"
    "                            (analyst Issue #5): inputs + options preserved\n"
    "                            for hash-verified replay via `diff-load`.\n"
    "                            Exit code: 0 identical, 1 on any change, 2 on\n"
    "                            CLI usage error. JSON schema:\n"
    "                            subuwutuner.diff.v1.\n"
    "    diff-load --pack <pack.toml> [--format text|csv|json] [--output FILE]\n"
    "              <SESSION.stcompare>\n"
    "                            Open a saved .stcompare session, verify both\n"
    "                            ROMs still match their recorded CRC32, recompute\n"
    "                            the diff with the session's saved options, and\n"
    "                            render. Text-mode also prints any saved\n"
    "                            annotations. Exit codes match `diff`.\n"
    "    analyze-tune-iterations --roms <A.bin> <B.bin> [...] --out-dir <DIR>\n"
    "                            [--labels L1 L2 ...] [--pack <pack.toml>]\n"
    "                            [--gap-bytes 8] [--undeclared-threshold 256]\n"
    "                            [--sample-bytes 32] [--out json|md|both]\n"
    "                            N-ROM byte-coalesced diff with per-pair pack-xref.\n"
    "                            Adjacent pairs + cumulative first/last when >=3.\n"
    "                            Classifies each diff run against the nearest\n"
    "                            declared table; flags runs >threshold past any\n"
    "                            declared table as RE candidates (catches the\n"
    "                            kind of 4x-wrong declaration that surfaced the\n"
    "                            wastegate_duty bug 2026-06-09). Writes\n"
    "                            summary.json + report.md into --out-dir.\n"
    "                            JSON schema: subuwutuner.analyze-tune-iterations.v1.\n"
    "    table-edit --def <pack.toml> --table <id> [--rows A:B] [--cols A:B]\n"
    "               OP [VALUE] <FILE> --output <OUT>\n"
    "                            Edit a table in <FILE> and write the result to\n"
    "                            <OUT>. OP is one of: set, add, multiply, percent,\n"
    "                            smooth, interpolate. VALUE is required for the\n"
    "                            first four ops and ignored for smooth/interpolate.\n"
    "    project-new --source <rom> --def <pack> [--name <name>] <dir>\n"
    "                            Create a new .stune project directory containing\n"
    "                            a copy of the source ROM, an editable working\n"
    "                            ROM, and a reference to the definition pack.\n"
    "    project-info [--json] [--audit-summary] <dir>\n"
    "                            Print metadata + current working-ROM CRC32 for\n"
    "                            a .stune project. --json emits a one-line\n"
    "                            subuwutuner.project-info.v1 object for CI scripts.\n"
    "    project-edit --table <id> [--rows A:B] [--cols A:B] OP [VALUE] <dir>\n"
    "                            Apply an edit to a project's working ROM and\n"
    "                            update project.toml. Same OPs as table-edit.\n"
    "    project-edit-csv <dir> --table <id> --from <FILE.csv> [--dry-run]\n"
    "                            Bulk cell-edit import. CSV: optional header,\n"
    "                            then one edit per row as `row,col,value`.\n"
    "                            Applied as a single project edit through\n"
    "                            edit::History. --dry-run previews the edits\n"
    "                            (before -> after) without writing.\n"
    "    project-export-csv <dir> --table <id> [--diff-only] [--output <FILE>]\n"
    "                            Export the table's working-ROM cells as\n"
    "                            `row,col,value` rows (the format project-edit-\n"
    "                            csv consumes). --diff-only restricts to cells\n"
    "                            that differ from source — share-able tune diff.\n"
    "    project-set-profile <dir> <profile>\n"
    "                            Set the project's jurisdiction profile (see\n"
    "                            docs/06-legal-ethics.md). Valid: motorsport-only,\n"
    "                            alberta-ca, eu-roadworthy, california-us.\n"
    "    project-add-rom <dir> --id <slug> --path <rom-file>\n"
    "                          [--display-name \"Name\"] [--notes \"...\"]\n"
    "                            Add an additional ROM to the project. The file is\n"
    "                            copied into the project dir as <slug>.bin and a\n"
    "                            [[rom]] entry is written to project.toml. The GUI\n"
    "                            Compare panel can then pick this ROM by slug\n"
    "                            without a file dialog (analyst Issue #10).\n"
    "    project-list-roms <dir> [--json]\n"
    "                            List every ROM declared by the project — source,\n"
    "                            working, and each [[rom]] additional entry. Surfaces\n"
    "                            any project.toml parse warnings (missing files,\n"
    "                            empty ids).\n"
    "    project-clone <src-dir> <dst-dir> [--name \"New name\"]\n"
    "                            Duplicate a project. Copies source.bin, working.bin,\n"
    "                            edits.toml, definitions/ (when in-tree), and any\n"
    "                            [[rom]] additional files. Skips audit.log so the\n"
    "                            clone starts with a fresh timeline. Optional --name\n"
    "                            rewrites the display_name in the cloned project.toml.\n"
    "    project-validate <dir> [--json]\n"
    "                            Health check: project.toml loads, both ROMs read,\n"
    "                            pack has tables, source CRC matches recorded value,\n"
    "                            [[rom]] entries resolve, audit.log integrity is OK.\n"
    "                            Exits 1 on the first failure — gateable from CI.\n"
    "    project-set-active-rom <dir> <id>\n"
    "                            Designate which ROM read-side verbs target by default.\n"
    "                            id: 'working' (default), 'source', or an additional\n"
    "                            ROM id from `project-list-roms`. Pass '' to reset.\n"
    "    stats <dir> --table <id> [--source|--working] [--json]\n"
    "                            Print min/max/mean/stddev/p10/p50/p90/cells/edited\n"
    "                            for one table. Same shape as the GUI Stats panel.\n"
    "                            Defaults to --working; --source reads the unmodified\n"
    "                            ROM if you want pre-edit baseline.\n"
    "    project-history <dir> [--table <id>] [--limit N]\n"
    "                            List the project's edit history with cursor\n"
    "                            position and per-edit emissions/safety flags.\n"
    "                            --table filters to one table; --limit caps to\n"
    "                            the most-recent N rows after filtering.\n"
    "    project-diff <A.stune> <B.stune> [--table <id>] [--profile P] [--verbose]\n"
    "                            Compare two projects' working ROMs table-by-\n"
    "                            table. Both must reference the same pack.\n"
    "                            --table filters the comparison to one table.\n"
    "                            --profile runs the policy gate on the full\n"
    "                            A->B byte changes (independent of --table).\n"
    "    project-autotune-maf <dir> --table <id> --log <CSV>\n"
    "                  [--gain N] [--max-delta P] [--min-samples-per-cell N]\n"
    "                  [--require-open-loop] [--no-smooth] [--strict-lint] [--apply]\n"
    "                            Run the docs/12 MAF auto-tune kernel against\n"
    "                            the project's MAF scaling table and a CSV\n"
    "                            datalog. Prints per-cell proposals; with\n"
    "                            --apply, commits them as a single project edit.\n"
    "    project-autotune-knock-pull <dir> --table <id> --log <CSV>\n"
    "                  [--trigger-degrees D] [--pull-step-degrees D]\n"
    "                  [--min-samples-per-cell N] [--strict-lint] [--apply]\n"
    "                  [--enable-add-back [--add-back-step-degrees D]\n"
    "                                     [--add-back-min-clean-samples N]\n"
    "                                     [--add-back-clean-threshold-degrees D]]\n"
    "                            Run the docs/12 knock-pull algorithm against\n"
    "                            the project's 2D timing table (load × RPM) and\n"
    "                            a CSV datalog. Prints a 2D delta-ledger; with\n"
    "                            --apply, commits as a single project edit.\n"
    "    project-flash <dir> [--trace <FILE.uds>]\n"
    "                  [--journal <FILE.toml>] [--manifest <FILE.toml>]\n"
    "                  [--confirm] [--reason \"…\"] [--dry-run]\n"
    "                  [--sector-size <N>] [--base-address <addr>]\n"
    "                            Build a FlashPlan from the project's source vs\n"
    "                            working delta, gate it through st::policy using\n"
    "                            the project's stored profile, then run the\n"
    "                            orchestrator against a MockTransport-replayed UDS\n"
    "                            trace. Without --trace, runs everything up\n"
    "                            through the policy gate and exits — a preview of\n"
    "                            what the flash would do without touching the\n"
    "                            transport. Refuses on engine-safety violations\n"
    "                            and on emissions edits without the confirmation/\n"
    "                            reason the active profile demands.\n"
    "    pack-info [--json] <DEF>\n"
    "                            Print metadata + counts for a definition pack.\n"
    "                            --json emits a one-line subuwutuner.pack-info.v1\n"
    "                            object for CI scripts.\n"
    "    pack-lint [--json] <DEF>\n"
    "                            Load + Definition::validate() a pack and report\n"
    "                            the result. Exit 0 on clean, 1 on validation\n"
    "                            failure, 2 on bad CLI usage. --json for CI.\n"
    "    table-list <DEF> [--category C] [--emissions] [--safety-critical]\n"
    "                            List tables in a pack with optional filters.\n"
    "    primitive-list <DEF> [--type int|float|bool]\n"
    "                            List custom-feature primitives in a pack with\n"
    "                            their type signature + description. Useful for\n"
    "                            .stmod authors browsing what's available.\n"
    "    workflow-list <DEF> [--id <id>] [--eligible] [--json]\n"
    "                            List pack-declared [[workflow]] entries with\n"
    "                            their required_tables presence check. Exit 1\n"
    "                            when any workflow is ineligible (CI gate).\n"
    "                            --json emits a subuwutuner.workflow-list.v1\n"
    "                            one-liner for scripted consumption.\n"
    "    hook-list <DEF> [--sensor] [--action]\n"
    "                            List custom-feature hooks in a pack with their\n"
    "                            ECU splice address + signal signature. --sensor\n"
    "                            filters to read-only hooks (data flowing OUT of\n"
    "                            the ECU); --action filters to write-only hooks\n"
    "                            (values flowing INTO the ECU).\n"
    "    pack-dtcs <DEF> [--bitmap <id>] [--emissions] [--show-state <ROM>]\n"
    "                            List DTC codes in a pack, with their bitmap\n"
    "                            location and emissions-relevant flag.\n"
    "                            --show-state reads enable bits from <ROM>\n"
    "                            and adds an On column (Y/n) per code.\n"
    "    project-disable-dtc --code P0401[,...] <dir>\n"
    "    project-enable-dtc  --code P0401[,...] <dir>\n"
    "                            Clear or set the enable bit for one or more\n"
    "                            DTC codes in the project's working ROM. DTC\n"
    "                            edits bypass edit::History (no project-undo).\n"
    "    policy [--profile P]    Print the jurisdiction-profile lint matrix\n"
    "                            (emissions on-save/on-flash, engine-safety) for\n"
    "                            the named profile (or all profiles if omitted).\n"
    "    project-undo <dir>      Walk back one edit in the project's history.\n"
    "    project-redo <dir>      Walk forward one edit in the project's history.\n"
    "    log --def <pack> --pid <id[,id...]> --trace <file> [--output <csv>]\n"
    "        [--canonical-columns]\n"
    "                            Replay an SSM-response trace file through a mock\n"
    "                            transport and write a CSV datalog. Trace format:\n"
    "                            one response frame per line as whitespace-separated\n"
    "                            hex bytes; '#' starts a comment. Without --output,\n"
    "                            the CSV is written to stdout. --canonical-columns\n"
    "                            renames standard SSM PIDs to autotune-friendly\n"
    "                            column names (p2→coolant_c, p7→throttle_pct,\n"
    "                            p8→rpm, p11→iat_c, p18→maf_voltage) so the output\n"
    "                            drops into `autotune *` / `project-autotune-*`\n"
    "                            without a rename pass.\n"
    "    ebcs-analyze --log <CSV> --timestamp-col <name>\n"
    "                 --target-boost-col <name> --actual-boost-col <name>\n"
    "                 --throttle-col <name>\n"
    "                 [--wgdc-col <name>] [--rpm-col <name>]\n"
    "                 [--throttle-step-pct N] [--target-step N]\n"
    "                 [--max-event-duration N] [--overshoot-warn-pct N]\n"
    "                 [--timestamp-unit seconds|millis|micros|rows] [--verbose]\n"
    "                            Detect tip-in events in a boost log, characterize each\n"
    "                            step response (rise time / overshoot / settling time /\n"
    "                            steady-state error), and produce heuristic Kp/Ki/Kd\n"
    "                            gain-adjustment suggestions. Output is advisory — verify\n"
    "                            on a dyno before driving aggressively. See\n"
    "                            docs/05-improvements.md §11.\n"
    "    coldstart-analyze --log <CSV> --timestamp-col <name> --ect-col <name> --rpm-col <name>\n"
    "                      [--iat-col <name>] [--observed-lambda-col <name>]\n"
    "                      [--commanded-lambda-col <name>] [--timing-col <name>]\n"
    "                      [--closed-loop-col <name>]\n"
    "                      [--cold-threshold-c N] [--ect-bin-width-c N]\n"
    "                      [--min-samples-per-bin N]\n"
    "                      [--timestamp-unit seconds|millis|micros|rows]\n"
    "                      [--target ect:lambda,ect:lambda,...]\n"
    "                      [--json] [--csv]\n"
    "                            Phase-classify + ECT-bin a cold-start datalog (engine cold-\n"
    "                            soak → warmup). Reports per-phase sample/time counts, per-\n"
    "                            ECT-bin observed lambda + deviation from a user-supplied\n"
    "                            target curve. --target points are linearly interpolated\n"
    "                            (sorted ascending by ECT). See docs/05-improvements.md §11.\n"
    "    ai-drift --log <CSV> --timestamp-col <name>\n"
    "             [--ltft-col <name>] [--dam-col <name>] [--idle-adapt-col <name>]\n"
    "             [--json]\n"
    "                            Tier-1 rule-based drift classifier (docs/20). Loads\n"
    "                            an adaptive-history CSV, runs classify(), prints a\n"
    "                            diagnosis (cause + confidence + evidence + alternatives\n"
    "                            + recommended checks). Pure-domain, no LLM.\n"
    "    ai-narrate --provider anthropic|openai (--key <K> | --key-env <NAME>)\n"
    "               (--prompt <text> | --prompt-file <P> | --prompt-stdin)\n"
    "               [--model <M>] [--system <text>] [--system-file <P>] [--json]\n"
    "                            Tier-2 LLM narration via st::ai::Backend (docs/20).\n"
    "                            Shells out to curl; needs network + a valid API key.\n"
    "                            Pipe-friendly: `ai-drift … | ai-narrate --prompt-stdin`.\n"
    "                            Prefer --key-env on shared boxes — cmdline args are\n"
    "                            visible to other local users via tasklist/ps.\n"
    "                            --json emits subuwutuner.ai-narrate.v1 envelope.\n"
    "    adaptive-history --log <CSV> --timestamp-col <name>\n"
    "                     [--ltft-col <name>] [--dam-col <name>] [--idle-adapt-col <name>]\n"
    "                     [--bucket-seconds N] [--timestamp-unit seconds|millis|micros|rows]\n"
    "                     [--min-samples-per-bucket N] [--verbose]\n"
    "                            Chart adaptive-learning drift (LTFT / DAM / idle-adapt)\n"
    "                            across days/weeks from a CSV datalog. Buckets samples by\n"
    "                            time (default 1-day buckets) and reports per-signal mean /\n"
    "                            min / max + a linear drift/day slope. --verbose adds a\n"
    "                            per-bucket breakdown. At least one of --ltft-col /\n"
    "                            --dam-col / --idle-adapt-col is required. See\n"
    "                            docs/05-improvements.md §11 for the design.\n"
    "    knock-snapshot --log <CSV> --flkc-cols <a,b,c,d>\n"
    "                   [--fbkc-cols <a,b,c,d>] [--rpm-col <name>] [--load-col <name>]\n"
    "                   [--cylinders N] [--window-seconds N] [--sample-rate-hz N]\n"
    "                   [--min-rpm N] [--min-load N] [--no-gate] [--json] [--csv]\n"
    "                            Compute a per-cylinder knock dashboard from a CSV\n"
    "                            datalog. --flkc-cols is required (per-cyl fine knock\n"
    "                            learn column names, order = cyl 1..N). --fbkc-cols\n"
    "                            optionally adds feedback knock correction per cyl;\n"
    "                            negative samples are counted as knock events. The\n"
    "                            load gate (--min-rpm, --min-load) filters out idle/\n"
    "                            decel samples; --no-gate disables it. Cylinder count\n"
    "                            defaults to len(--flkc-cols); --cylinders overrides.\n"
    "                            See docs/05-improvements.md §11 for the design.\n"
    "    can-replay <FILE.asc>   Load a Vector .asc CAN trace and print per-id\n"
    "                            statistics (count, rate, dlc, first byte mode).\n"
    "    can-diff <A.asc> <B.asc>\n"
    "                            Compare two .asc captures. Reports ids present only\n"
    "                            in A, only in B, and per-id frame-count deltas.\n"
    "    can-discover --from <FILE.asc> [--baseline <secs>] [--bus <0..3>]\n"
    "                 [--output <session.cdb>]\n"
    "                            Run offline CAN reverse-engineering discovery over\n"
    "                            a captured trace. Frames before `baseline` seconds\n"
    "                            (default 10) build the BaselineModel; later frames\n"
    "                            drive ChangeDetector. Writes a .cdb bundle to stdout\n"
    "                            unless --output is given. Events are unlabeled —\n"
    "                            edit the .cdb to add descriptions before exporting.\n"
    "    can-export-dbc <session.cdb> [--output <draft.dbc>]\n"
    "                            Emit a draft DBC from a discovery bundle. Each id\n"
    "                            becomes a BO_; each labeled Change event becomes a\n"
    "                            SG_ at identity scaling. Refine the DBC by hand.\n"
    "    can-decode --dbc <FILE.dbc> <FILE.asc> [--output <csv>]\n"
    "                            Decode a .asc capture against a DBC. Writes long-\n"
    "                            format CSV (timestamp_ns,bus,can_id,signal,value,\n"
    "                            unit) — one row per (frame, signal) pair. Frames\n"
    "                            whose id is not in the DBC are skipped.\n"
    "    flash-plan-info <FILE.toml> [--def <pack> --source <rom> [--profile P]]\n"
    "                            Load a flash plan TOML and print its summary —\n"
    "                            session, options, and the address/length of each\n"
    "                            sector write. With --def + --source + optional\n"
    "                            --profile, also runs the plan through st::policy\n"
    "                            and surfaces the engine-safety / emissions gates\n"
    "                            it would trigger at flash time. Hardware-free;\n"
    "                            touches no transport.\n"
    "    flash-manifest-info <FILE.toml>\n"
    "                            Pretty-print a flash manifest: schema, timestamps,\n"
    "                            CRCs, policy fields, per-sector status.\n"
    "    flash-delta <SOURCE.bin> <TARGET.bin> [--sector-size <N>]\n"
    "                [--base-address <addr>] [--output <plan.toml>]\n"
    "                            Diff two ROMs of equal size; for every sector-aligned\n"
    "                            region that differs, emit a flash plan whose [[write]]\n"
    "                            entries cover those sectors with TARGET's bytes. The\n"
    "                            plan is hand-editable before execution.\n"
    "    flash-resume <ORIGINAL.plan.toml> <JOURNAL.manifest.toml>\n"
    "                [--output <resumed.plan.toml>]\n"
    "                            Given the plan from a partial-flash attempt and the\n"
    "                            manifest journal it left behind, emit a plan covering\n"
    "                            only the sectors that didn't complete. Refuses if the\n"
    "                            plan was modified between attempts (data CRC32 of any\n"
    "                            done sector differs).\n"
    "    flash-apply --plan <FILE.toml> --trace <FILE.uds>\n"
    "                [--journal <FILE.toml>] [--manifest <FILE.toml>]\n"
    "                [--profile <P> --def <pack.toml> --source <rom.bin>]\n"
    "                [--confirm] [--reason \"…\"]\n"
    "                            Apply a flash plan against a MockTransport-\n"
    "                            replayed UDS trace. With --profile + --def +\n"
    "                            --source, gates the flash through st::policy:\n"
    "                            refuses on engine-safety violations regardless\n"
    "                            of profile, and on emissions edits the profile\n"
    "                            demands a confirmation/reason for without one.\n"
    "                            Trace is text with one '> req hex' / '< resp hex'\n"
    "                            pair per exchange; '#' starts a comment. Hardware-\n"
    "                            free smoke for the Flasher orchestrator; prints the\n"
    "                            FlashReport summary. With --manifest, writes a\n"
    "                            Manifest of the run; with --journal, sets\n"
    "                            FlashPlan.journal_path for incremental writes.\n"
    "    obd-info --transport <kind> --pid <cal-id|cvn|vin>\n"
    "             [--device <path>] [--dll <path>] [--verbose]\n"
    "                            Send OBD-II Mode 0x09 (vehicle information).\n"
    "                            cal-id (0x04) returns the 16-byte calibration ID,\n"
    "                            cvn (0x06) the 4-byte calibration verification\n"
    "                            number, vin (0x02) the 17-byte VIN. Lives in the\n"
    "                            default session, no SecurityAccess needed.\n"
    "                            Useful as a quick tune-state identity check\n"
    "                            before/after applying a tune via any\n"
    "                            aftermarket flasher.\n"
    "    rdbi --transport <kind> --did <hex>\n"
    "         [--device <path>] [--dll <path>] [--output FILE.bin]\n"
    "         [--no-dsc] [--verbose]\n"
    "                            Read one UDS DataByIdentifier (SID 0x22) and\n"
    "                            print hex + ASCII. Useful for ground-truth\n"
    "                            validation against live hardware without needing\n"
    "                            SecurityAccess — well-known emissions DIDs are\n"
    "                            always readable in the default session. The VIN\n"
    "                            DID (0xF190) forces multi-frame ISO-TP RX which\n"
    "                            validates the OBDX transport's First-Frame strip\n"
    "                            path. Defaults enter extendedDiagnosticSession;\n"
    "                            --no-dsc to skip.\n"
    "    sniff --transport <kind> --output <FILE.log> [--device <path>]\n"
    "          [--dll <path>] [--filter <id>[,<id>...]] [--duration <sec>]\n"
    "                            Passive CAN bus monitor. Opens the adapter in\n"
    "                            LISTEN-ONLY mode (adapter never transmits — safe to\n"
    "                            share the OBD-II port with another active tester\n"
    "                            via a Y-cable) and records every CAN frame to\n"
    "                            --output. Stops on Ctrl+C or after --duration\n"
    "                            seconds. --filter narrows the recorded IDs (the\n"
    "                            adapter still hears everything; the filter only\n"
    "                            controls what gets written to the file). Output is\n"
    "                            one frame per line: '<ms> 0xCANID <hex bytes>'.\n"
    "                            Use case: SecurityAccess seed/key capture (run\n"
    "                            during an aftermarket-flasher connect cycle) for\n"
    "                            downstream feeding into tools/extract_subaru_sa.py.\n"
    "    ssm-a8-poll --transport <kind> --output <FILE.log>\n"
    "                --addr <hex>[,<hex>...] [--addr <hex> ...]\n"
    "                [--device <path>] [--dll <path>] [--duration <sec>]\n"
    "                [--interval-ms <ms>] [--timeout-ms <ms>] [--verbose]\n"
    "                            OEM Subaru SSM Command 0xA8 (Read-Multi-Address)\n"
    "                            over ISO-15765 (tester 0x7E0, ECU 0x7E8, 500 kbps).\n"
    "                            Reads one byte per --addr from ECU RAM, repeatedly\n"
    "                            at --interval-ms (default 333 ms, ~3 Hz). For\n"
    "                            multi-byte fields (e.g. RPM u16 at SSM 0x00000E),\n"
    "                            pass each byte as a separate --addr; the downstream\n"
    "                            correlator assembles them. Stops on Ctrl+C or after\n"
    "                            --duration seconds. Output is a header-commented\n"
    "                            log: per-poll lines '<elapsed_ms>\\t<HH HH ...>'\n"
    "                            with one hex byte per address in request order.\n"
    "                            Use case: cross-correlate captured aftermarket-\n"
    "                            datalogger F3xx response bytes against direct OEM\n"
    "                            RAM reads to recover the byte-to-RAM-address map\n"
    "                            without ROM disassembly. May be SA-gated on\n"
    "                            aftermarket-tuned ECUs; if denied, retry on a\n"
    "                            factory / uninstalled ECU or unlock SSM first.\n"
    "    rom-pull --addr <hex> --size <hex> --output <FILE.bin>\n"
    "             (--trace <FILE.uds> | --transport <kind> [--dll <path>]\n"
    "                                   [--device <path>])\n"
    "             [--max-chunk <hex>]\n"
    "                            Read N bytes of ECU memory via Flasher::read_full_rom\n"
    "                            into a raw binary file. Pick a source:\n"
    "                              --trace <FILE.uds>: MockTransport-replayed trace\n"
    "                                  ('> req' / '< resp' pairs). The everyday\n"
    "                                  testing path; runs hardware-free.\n"
    "                              --transport <kind>: real adapter via the factory.\n"
    "                                  <kind> ∈ j2534|obdx|native. j2534 needs\n"
    "                                  --dll <vendor-DLL-path>; obdx + native need\n"
    "                                  --device <USB-CDC-endpoint> (e.g. COM5\n"
    "                                  for OBDX Pro VX on Windows). obdx + native\n"
    "                                  open() live against real hardware today;\n"
    "                                  j2534 is still a NotImplemented stub until\n"
    "                                  the platform DLL dynload layer lands.\n"
    "                            Default --max-chunk=0x1000 (upper bound; the\n"
    "                            upfront probe may settle lower).\n"
    "    flash-trace --plan <FILE.toml> --output <FILE.uds>\n"
    "                            Emit a guaranteed-success UDS trace for the given plan,\n"
    "                            walking the same sequence Flasher::execute would and\n"
    "                            pairing every request with a canned positive response.\n"
    "                            Useful as a regression fixture or as input to\n"
    "                            'flash-apply --trace ...' for hardware-free validation.\n"
    "    autotune maf --log <CSV> (--axis <v,v,…> | --axis-file <path>)\n"
    "                 (--current <gs,gs,…> | --current-file <path>)\n"
    "                 [--gain 0.5] [--max-delta 8%] [--min-samples-per-cell 50]\n"
    "                 [--require-open-loop] [--no-smooth] [--strict-lint]\n"
    "                            Propose a MAF-scaling correction per docs/12. Reads a\n"
    "                            column-headered CSV log (required cols: maf_voltage,\n"
    "                            actual_afr, commanded_afr, rpm, throttle_pct, coolant_c,\n"
    "                            iat_c; optional: time_ms, closed_loop, knock, limp_mode),\n"
    "                            applies the configured data-quality gates, buckets each\n"
    "                            sample to the nearest axis cell, runs the docs/12\n"
    "                            engine-safety lint (non-monotonic + step discontinuity),\n"
    "                            and prints the per-cell diff. --strict-lint exits 3 on\n"
    "                            any violation. Hardware-free; no flashing.\n"
    "    autotune knock-pull --log <CSV>\n"
    "                        (--rpm-axis <r,r,…>  | --rpm-axis-file <path>)\n"
    "                        (--load-axis <l,l,…> | --load-axis-file <path>)\n"
    "                        (--current-timing <d,d,…> | --current-timing-file <path>)\n"
    "                        [--trigger-degrees 1.5] [--pull-step-degrees 0.75]\n"
    "                        [--min-samples-per-cell 30]\n"
    "                        [--max-neighbor-step-degrees 3.0] [--strict-lint]\n"
    "                        [--enable-add-back] [--add-back-step-degrees 0.5]\n"
    "                        [--add-back-min-clean-samples 50]\n"
    "                        [--add-back-clean-threshold-degrees 0.05]\n"
    "                            Propose ignition-timing pull on cells with sustained\n"
    "                            knock per docs/12 §\"Knock-based ignition pull\". CSV\n"
    "                            cols: rpm, load, feedback_knock, coolant_c, iat_c;\n"
    "                            optional: limp_mode. --current-timing is flat row-major\n"
    "                            (rows × cols = load × rpm). Monotonic-subtract: cells\n"
    "                            never gain timing in the pull pass. --enable-add-back\n"
    "                            opts into the docs/12 follow-up pass that adds a small\n"
    "                            amount of timing BACK in cells whose mean feedback\n"
    "                            knock was clean over enough samples; default off per\n"
    "                            spec. Runs the docs/12 neighbor-smoothness lint on the\n"
    "                            final proposed surface; --strict-lint exits 3 on any\n"
    "                            violation. Hardware-free.\n"
    "    lint-graph <FILE.stmod> [--strict]\n"
    "                            Load a .stmod feature-graph file, run structural\n"
    "                            validate() + lint() (see docs/16). Prints any\n"
    "                            structural error first (exit 1), then a bulleted\n"
    "                            list of completeness warnings (exit 0 by default,\n"
    "                            3 with --strict).\n"
    "    dump-ir <FILE.stmod>    Lower a .stmod feature-graph to st::feature::ir\n"
    "                            and print one instruction per line (see docs/16).\n"
    "                            Intended as a developer / regression-fixture aid;\n"
    "                            codegen will consume the same Module.\n"
    "    lint-ir <FILE.stmod> [--budget N] [--strict]\n"
    "                            Lower a .stmod, then run st::feature::ir::lint —\n"
    "                            catches RT-budget overruns and duplicate hook-\n"
    "                            override stores (see docs/16 §Safety). --budget\n"
    "                            sets the per-module cycle budget (0 disables;\n"
    "                            default 200). Exit 0 normally, 3 with --strict\n"
    "                            on any finding.\n"
    "    profile list|show|import|export [--dir <dir>] [ARGS...]\n"
    "                            VehicleProfile registry — one .stprofile per\n"
    "                            ECU/car the user tunes. Subcommands: list (all\n"
    "                            profiles in --dir or default), show <id-or-\n"
    "                            path>, import <FILE.stprofile>, export <id>\n"
    "                            <FILE>. Default dir: ~/.config/subuwutuner/\n"
    "                            profiles (or %APPDATA%/SubuwuTuner/profiles on\n"
    "                            Windows). See docs/33 + analyst Issue #7.\n"
    "    audit show <project-dir> [--json]\n"
    "    changelog show [--all]\n"
    "                            Print CHANGELOG.md's [Unreleased] section (or the\n"
    "                            whole file with --all). Mirrors the welcome panel's\n"
    "                            \"What's new\" parser so CLI + GUI render the same\n"
    "                            content. Useful for release-prep scripts.\n"
    "    audit append <project-dir> --kind <name> --description \"text\"\n"
    "                              [--source <text>] [--field key=value ...]\n"
    "                            Append one entry. kind is a wire-format name\n"
    "                            (e.g. project.saved, flash.started, custom).\n"
    "                            Unknown kinds get serialized as 'custom' with\n"
    "                            the requested name preserved as a field. Useful\n"
    "                            for cron / CI / external tooling — handles the\n"
    "                            CRC32 + timestamp like the in-process callers.\n"
    "    audit verify <project-dir> [--json]\n"
    "                            Walk audit.log and report any entries whose\n"
    "                            stored CRC32 doesn't match the recomputed value.\n"
    "                            Exits 1 when any bad entry is found so CI can\n"
    "                            gate on log integrity.\n"
    "    audit stats <project-dir> [--json]\n"
    "                            Per-kind counts + first/last timestamp + bad-CRC\n"
    "                            count. Read-only summary for activity surveys.\n"
    "                            Read the per-project audit.log (cross-session\n"
    "                            ECU-touch log) and print one entry per line.\n"
    "                            Tampered entries (CRC32 mismatch) get a\n"
    "                            [TAMPERED] prefix. --json emits the full\n"
    "                            subuwutuner.audit.v1 array for CI scoring.\n"
    "                            See docs/05 §4 / analyst Issue #8.\n"
    "    list-validators [--json]\n"
    "                            List the static inventory of flash pre-flight\n"
    "                            validators (`st::policy::available_validators`)\n"
    "                            — what each checks, when it no-ops, and its\n"
    "                            default thresholds. --json emits the\n"
    "                            subuwutuner.list-validators.v1 envelope.\n"
    "    did-datalog-preset [--json|--toml] [--firmware narrow|wide]\n"
    "                            Print the aftermarket-datalogger SSM-extended\n"
    "                            DID polling protocol shape (DID set 0xF300-0xF304,\n"
    "                            ~25 Hz polling) and per-byte signal layout\n"
    "                            including RAM address + scaling expression\n"
    "                            + tester-internal monitor_id.\n"
    "                            --firmware picks the DID layout version\n"
    "                            (defaults to 'wide', 44 signals; 'narrow'\n"
    "                            covers 31 signals).\n"
    "                            --json emits subuwutuner.did-datalog-preset.v1.\n"
    "                            --toml emits a SubuwuTuner-ready pack fragment\n"
    "                            for the Verified-only subset (drop-in for a\n"
    "                            definitions tree).\n"
    "                            Sourced clean-room from sniff + CSV captures\n"
    "                            + the LF79103P live-signals catalog.\n"
    "    transport-list [--json] [--explain]\n"
    "                            List J2534 v04.04 vendor DLLs registered on\n"
    "                            this host (HKLM\\Software\\PassThruSupport.04.04\n"
    "                            + the Wow6432Node mirror). Read-only — never\n"
    "                            modifies the registry, never loads any DLL.\n"
    "                            The DLL path printed for each entry is what\n"
    "                            you'd pass to `--transport j2534 --dll <path>`\n"
    "                            once the platform dynload layer lands.\n"
    "                            --explain prints per-transport decision text\n"
    "                            (obdx / j2534 / native / mock — when to pick\n"
    "                            each, plus the CLI flag shape).\n"
    "    doctor [--pack-dir <dir>] [--rom <FILE.bin>] [--json]\n"
    "                            Triage the install: tool/build identity, J2534\n"
    "                            adapter registry, definition-pack health, and\n"
    "                            (with --pack-dir + --rom) CID identification.\n"
    "                            Prints traffic-light status per section + a\n"
    "                            'what to do next' hint block. Exit 0 on healthy\n"
    "                            or advisory-only, 1 on any failure, 2 on bad\n"
    "                            CLI usage. The 'start here' command when an\n"
    "                            install isn't behaving — composes pack-list /\n"
    "                            rom-identify / transport-list, no live link.\n"
    "                            --json emits one-line `subuwutuner.doctor.v1`\n"
    "                            JSON to stdout instead of human-readable text;\n"
    "                            same checks, same exit codes — for CI scripts.\n"
    "    feature-compile <FILE.stmod> --def <pack.toml> [--arch sh2a|rh850]\n"
    "                    [--format hex|toml|raw|stmod] [--output <FILE>]\n"
    "                    [--validate-only]\n"
    "                            Lower a .stmod, pick a codegen backend by the\n"
    "                            pack's platform (or --arch override), and emit\n"
    "                            the compiled PatchObject. Default --format=hex\n"
    "                            renders a per-hook header + line-wrapped hex of\n"
    "                            the code bytes; --format=toml emits the structured\n"
    "                            [patch] table only; --format=stmod emits a full\n"
    "                            bundled .stmod file (source graph + compiled patch\n"
    "                            in one TOML document, ready to redistribute);\n"
    "                            --format=raw writes the code bytes verbatim and\n"
    "                            requires a single-hook patch + --output. Without\n"
    "                            --output the text formats print to stdout.\n"
    "                            --validate-only runs parse + lower + compile and\n"
    "                            exits 0 (success) or non-zero (failure) without\n"
    "                            producing output — for CI / pre-commit hooks that\n"
    "                            want to confirm a .stmod still builds against a\n"
    "                            pack without leaving artifacts behind.\n";

void print_version() {
    std::printf("%.*s %.*s\n", static_cast<int>(st::Version::name().size()),
                st::Version::name().data(), static_cast<int>(st::Version::string().size()),
                st::Version::string().data());
}

void print_usage() {
    std::fputs(kUsage.data(), stdout);
}

// Upgrade a `<dir>/pack.toml` path to its parent directory whenever
// the parent contains other *.toml files — the multi-file pack shape
// from docs/11. `Definition::from_file(pack.toml)` only loads the
// manifest and misses the companion fragments, so any CLI command
// pointed at `--def path/to/pack.toml` would otherwise silently see
// "0 tables / 0 hooks" on a perfectly valid split pack. A standalone
// pack.toml with no companions falls through unchanged, preserving
// the legacy single-file pack flow.
std::filesystem::path resolve_def_path(std::filesystem::path const &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return path;
    if (path.filename() != "pack.toml")
        return path;
    auto const parent = path.parent_path();
    std::error_code walk_ec;
    for (auto const &entry : std::filesystem::directory_iterator{parent, walk_ec}) {
        if (walk_ec)
            break;
        std::error_code is_file_ec;
        if (!entry.is_regular_file(is_file_ec) || is_file_ec)
            continue;
        auto const &p = entry.path();
        if (p.extension() == ".toml" && p.filename() != "pack.toml") {
            return parent;
        }
    }
    return path;
}

// Walk `pack_dir` recursively for .toml files that look like pack
// roots. Returns one path per pack, deterministically ordered.
//
// Two supported layouts (docs/11):
//   - Multi-file:  <dir>/pack.toml + companion fragments
//     (loaded via from_file -> resolve_includes)
//   - Single-file: a self-contained `<id>.toml` directly under
//     <dir> with no companion fragments
//
// To distinguish the two: any .toml found alongside a `pack.toml`
// in the same directory is treated as a fragment of that pack and
// skipped; standalone .toml files (no pack.toml sibling) are kept
// as single-file pack candidates. Definition::from_file then
// validates the candidate by attempting a load — fragments that
// somehow slipped through (no [pack] section) fail naturally and
// the caller skips them.
//
// Returns the list of candidate paths; the caller invokes from_file
// on each to materialize Definitions and tally load failures.
std::vector<std::filesystem::path> discover_pack_paths(std::filesystem::path const &pack_dir,
                                                       char const *subcmd) {
    std::vector<std::filesystem::path> paths;

    // First pass: identify directories that contain a pack.toml. Any
    // other .toml in such a directory is a fragment.
    std::vector<std::filesystem::path> multi_pack_dirs;
    try {
        std::error_code walk_ec;
        std::filesystem::recursive_directory_iterator it{
            pack_dir, std::filesystem::directory_options::skip_permission_denied, walk_ec};
        if (walk_ec) {
            std::fprintf(stderr, "%s: cannot walk %s: %s\n", subcmd, pack_dir.string().c_str(),
                         walk_ec.message().c_str());
            return paths;
        }
        for (auto const &e : it) {
            std::error_code is_file_ec;
            if (!e.is_regular_file(is_file_ec) || is_file_ec)
                continue;
            if (e.path().filename() == "pack.toml") {
                multi_pack_dirs.push_back(e.path().parent_path());
            }
        }
    } catch (std::filesystem::filesystem_error const &fs_err) {
        std::fprintf(stderr, "%s: filesystem error walking %s: %s\n", subcmd,
                     pack_dir.string().c_str(), fs_err.what());
        return paths;
    }

    auto const is_under_multi_pack_dir = [&](std::filesystem::path const &file) {
        // True if `file` is at or under any directory containing a
        // pack.toml. Walk ancestors up to (but not including) the
        // top-level pack_dir.
        std::error_code root_ec;
        auto const root = std::filesystem::weakly_canonical(pack_dir, root_ec);
        for (auto p = file.parent_path();; p = p.parent_path()) {
            if (std::find(multi_pack_dirs.begin(), multi_pack_dirs.end(), p) !=
                multi_pack_dirs.end()) {
                return true;
            }
            std::error_code cmp_ec;
            auto const canon = std::filesystem::weakly_canonical(p, cmp_ec);
            if (canon == root)
                return false;
            if (p == p.parent_path()) // filesystem root
                return false;
        }
    };

    // Second pass: collect pack.toml roots + any standalone .toml whose
    // parent isn't a multi-file pack dir (those are fragments).
    try {
        std::error_code walk_ec;
        std::filesystem::recursive_directory_iterator it{
            pack_dir, std::filesystem::directory_options::skip_permission_denied, walk_ec};
        if (walk_ec) {
            return paths;
        }
        for (auto const &e : it) {
            std::error_code is_file_ec;
            if (!e.is_regular_file(is_file_ec) || is_file_ec)
                continue;
            auto const &p = e.path();
            if (p.extension() != ".toml")
                continue;
            if (p.filename() == "pack.toml") {
                paths.push_back(p);
                continue;
            }
            if (is_under_multi_pack_dir(p))
                continue; // companion fragment of a multi-file pack
            paths.push_back(p);
        }
    } catch (std::filesystem::filesystem_error const &) {
        return paths;
    }

    std::sort(paths.begin(), paths.end());
    return paths;
}

// Print a definition-load failure with a Path-B context hint when the
// supplied path doesn't exist on disk. SubuwuTuner ships infrastructure
// only — calibration packs are user-supplied — so a missing path is a
// "where do I get a pack" moment, not just a generic I/O error.
int print_def_load_error(char const *subcmd, std::filesystem::path const &path,
                         st::Error const &err) {
    std::fprintf(stderr, "%s: %s\n", subcmd, err.to_string().c_str());
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        std::fputs("\n  No definition pack found at the path above.\n"
                   "  SubuwuTuner does not ship with bundled calibration packs;\n"
                   "  generate one with tools/defgen/defgen.py from a public\n"
                   "  community XML, or use fixtures/demo-pack/ as an example.\n",
                   stderr);
    }
    return 1;
}

bool arg_matches(char const *arg, std::string_view short_form, std::string_view long_form) {
    std::string_view const sv{arg};
    return sv == short_form || sv == long_form;
}

// Print "did you mean" suggestions to stderr when the user provided
// an id that didn't match any table/axis in the pack. For a pack
// with 300+ entries, listing all of them buries the signal; a small
// list of substring-matching ids gives the user a fast on-ramp to
// the right name.
//
// `entry_kind` is the human label ("table" / "axis") used in the
// "Did you mean" preamble. `ids` is the full list to filter from.
// Case-insensitive substring match; results stable-sorted by length
// ascending then by id alphabetic (so the shortest most-likely
// matches lead).
void print_id_suggestions(char const *entry_kind, std::string_view needle,
                          std::vector<std::string> const &ids) {
    auto const lower = [](std::string s) {
        for (char &c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return s;
    };
    std::string const needle_lower(needle);
    std::string const folded_needle = lower(needle_lower);

    std::vector<std::string> matches;
    for (auto const &id : ids) {
        if (lower(id).find(folded_needle) != std::string::npos)
            matches.push_back(id);
    }
    if (matches.empty()) {
        std::fprintf(stderr,
                     "  (no %s id contains '%.*s'; "
                     "use `subuwutuner-cli table-list <pack>` to see all)\n",
                     entry_kind, static_cast<int>(needle.size()), needle.data());
        return;
    }
    std::sort(matches.begin(), matches.end(), [](auto const &a, auto const &b) {
        if (a.size() != b.size())
            return a.size() < b.size();
        return a < b;
    });
    constexpr std::size_t kMaxSuggestions = 10;
    auto const n = std::min(matches.size(), kMaxSuggestions);
    std::fprintf(stderr, "  Did you mean one of these %ss?\n", entry_kind);
    for (std::size_t i = 0; i < n; ++i) {
        std::fprintf(stderr, "    %s\n", matches[i].c_str());
    }
    if (matches.size() > kMaxSuggestions) {
        std::fprintf(stderr, "    ... and %zu more matching '%.*s'\n",
                     matches.size() - kMaxSuggestions, static_cast<int>(needle.size()),
                     needle.data());
    }
}

// Forward-declared so commands defined ahead of parse_range's body can use it.
bool parse_range(std::string_view s, std::size_t &lo, std::size_t &hi);

// Subaru-convention CID location. Per the community per-ECU XML
// definitions for every VA / VB / older-EJ family ECU we've
// catalogued, the 8-byte ASCII "internal ID" (CID) sits at byte
// offset 0x00002000. Per-pack `internalidaddress` fields exist
// for outliers, but 0x2000 is the right zero-cost default for
// "what ROM did I just download?" before a pack is even chosen.
inline constexpr std::size_t kSubaruCidOffset = 0x00002000;
inline constexpr std::size_t kSubaruCidLength = 8;

// Print the bytes at the canonical Subaru CID offset if they're
// all printable ASCII (trailing spaces are real — leave them).
// Falls back to a "no signature" note for non-Subaru / mutated
// dumps so users don't see a misleading garbage string.
void print_subaru_cid(st::Rom const &rom) {
    if (rom.size() < kSubaruCidOffset + kSubaruCidLength) {
        std::printf("Subaru CID:     (ROM too short to hold a CID at "
                    "0x%08zX)\n",
                    kSubaruCidOffset);
        return;
    }
    auto const data = rom.data();
    bool all_printable = true;
    bool in_trailing_nul = false;
    char cid[kSubaruCidLength + 1] = {};
    for (std::size_t i = 0; i < kSubaruCidLength; ++i) {
        auto const b = data[kSubaruCidOffset + i];
        // Printable ASCII range 0x20..0x7E. Trailing 0x00 padding
        // is accepted IFF every subsequent byte is also 0x00 —
        // [41 53 38 30 00 00 00 00] is fine ("AS80"); a single
        // NUL followed by non-NUL bytes is garbage and rejected
        // so we don't print a misleading prefix.
        if (in_trailing_nul) {
            if (b != 0) {
                all_printable = false;
                break;
            }
            continue;
        }
        if (b == 0 && i > 0) {
            in_trailing_nul = true;
            cid[i] = 0;
            continue;
        }
        if (b < 0x20 || b > 0x7E) {
            all_printable = false;
            break;
        }
        cid[i] = static_cast<char>(b);
    }
    if (all_printable) {
        std::printf("Subaru CID:     '%s'  (8 bytes @ 0x%08zX)\n", cid, kSubaruCidOffset);
    } else {
        std::printf("Subaru CID:     (no printable signature at "
                    "0x%08zX — not a Subaru-format ROM at this "
                    "offset, or a corrupted dump)\n",
                    kSubaruCidOffset);
    }
}

// "Wordy" filter for the rom-info ASCII display. A bare
// is_printable_ascii run lets through every 5-byte sequence of
// punctuation / random bytes that happen to lie in 0x20..0x7E —
// for a 1MB Subaru ROM that's typically 4000+ "strings" of pure
// noise (`;A+d"cC`, `b@@(c`, etc.) drowning the dozens of real
// labels (CIDs, calibration IDs, build dates, diagnostic text).
//
// Real labels are punctuation-light — almost entirely alphanumeric
// plus space / underscore / hyphen / dot. Random ASCII-range
// machine code is punctuation-heavy.
//
// Rule:
//   1. >= 6 chars total
//   2. >= 4 letters (a-zA-Z) — not just alphanumerics; digit-only
//      strings like `1234567` are usually not labels either
//   3. <= 2 chars outside the "label charset" [a-zA-Z0-9_.- ]
//
// Empirically takes the typical 1MB-ROM 4000+ raw run down to a
// few dozen genuine labels — the signal-to-noise jump is large.
bool ascii_looks_wordy(std::string const &s) {
    if (s.size() < 6)
        return false;
    std::size_t letters = 0;
    std::size_t non_label = 0;
    for (char c : s) {
        bool const is_letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool const is_digit = (c >= '0' && c <= '9');
        bool const is_label_punct = (c == '_' || c == '.' || c == '-' || c == ' ');
        if (is_letter)
            ++letters;
        if (!is_letter && !is_digit && !is_label_punct)
            ++non_label;
    }
    return letters >= 4 && non_label <= 2;
}

void print_rom_summary(std::filesystem::path const &path, st::Rom const &rom) {
    std::printf("File:           %s\n", path.string().c_str());
    std::printf("Size:           %zu bytes (%.2f KiB)\n", rom.size(),
                static_cast<double>(rom.size()) / 1024.0);
    std::printf("CRC32:          0x%08X\n", rom.crc32());
    print_subaru_cid(rom);

    auto const all_strings = rom.scan_ascii(/*min_length=*/5);
    std::vector<st::Rom::AsciiString> wordy;
    wordy.reserve(all_strings.size() / 16);
    for (auto const &s : all_strings) {
        if (ascii_looks_wordy(s.text))
            wordy.push_back(s);
    }
    std::printf("Embedded ASCII: %zu strings (>=5 chars; %zu wordy)\n", all_strings.size(),
                wordy.size());

    // Sort by length descending — real labels (firmware version
    // strings, copyright notices, calibration IDs) tend to be longer
    // than the false-positive runs that still survive the wordy
    // filter. Ties broken by offset so output is deterministic.
    std::vector<st::Rom::AsciiString> display = wordy;
    std::sort(display.begin(), display.end(), [](auto const &a, auto const &b) {
        if (a.text.size() != b.text.size())
            return a.text.size() > b.text.size();
        return a.offset < b.offset;
    });

    constexpr std::size_t kMaxToPrint = 32;
    auto const limit = display.size() < kMaxToPrint ? display.size() : kMaxToPrint;
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &s = display[i];
        std::printf("  0x%08zX  (%2zu chars)  %s\n", s.offset, s.text.size(), s.text.c_str());
    }
    if (display.size() > kMaxToPrint) {
        std::printf("  ... %zu wordy strings not shown (sorted by length desc)\n",
                    display.size() - kMaxToPrint);
    }
}

void print_def_summary(st::Definition const &def, st::Rom const &rom) {
    auto const &pack = def.pack();
    std::printf("\nDefinition pack: %s\n", pack.id.c_str());
    if (!pack.display_name.empty()) {
        std::printf("  Display name:  %s\n", pack.display_name.c_str());
    }
    if (!pack.platform.empty()) {
        std::printf("  Platform:      %s\n", pack.platform.c_str());
    }
    if (pack.rom_size_bytes != 0) {
        std::printf("  Expected ROM:  %zu bytes\n", pack.rom_size_bytes);
        if (rom.size() != pack.rom_size_bytes) {
            std::printf("  ! ROM size mismatch (got %zu)\n", rom.size());
        }
    }
    if (!pack.checksum_type.empty()) {
        std::printf("  Checksum type: %s\n", pack.checksum_type.c_str());
    }

    auto const info = def.match_info(rom);
    if (info.has_value()) {
        if (info->scanned) {
            std::printf("  Match:         %s @ 0x%08zX (scanned)\n", info->name.c_str(),
                        info->offset);
        } else {
            std::printf("  Match:         %s @ 0x%08zX\n", info->name.c_str(), info->offset);
        }
    } else {
        // Distinguish "no scan-mode CID anywhere" from "fixed-offset
        // bytes don't match" — both surface as no-match but the
        // diagnostic phrasing differs.
        bool any_scan = false;
        for (auto const &id : def.identifications()) {
            if (id.cid_scan) {
                any_scan = true;
                break;
            }
        }
        if (any_scan) {
            std::printf("  Match:         (none — CID not found by scan; "
                        "ROM may be a different calibration or partial decrypt)\n");
        } else {
            std::printf("  Match:         (none — CID at declared offset does not match)\n");
        }
    }

    std::printf("\nTables defined: %zu\n", def.tables().size());

    // Category histogram — top 8 by count. Useful for sizing up an
    // unfamiliar pack at a glance ("does this have boost tables?
    // knock tables?") without having to scan a 300-line table list.
    // Empty-category entries are bucketed under "(uncategorized)".
    {
        std::map<std::string, std::size_t> cat_counts;
        for (auto const &t : def.tables()) {
            cat_counts[t.category.empty() ? "(uncategorized)" : t.category]++;
        }
        if (!cat_counts.empty()) {
            std::vector<std::pair<std::string, std::size_t>> sorted(cat_counts.begin(),
                                                                    cat_counts.end());
            std::sort(sorted.begin(), sorted.end(), [](auto const &a, auto const &b) {
                if (a.second != b.second)
                    return a.second > b.second;
                return a.first < b.first;
            });
            constexpr std::size_t kMaxCats = 8;
            auto const cat_limit = std::min(sorted.size(), kMaxCats);
            std::printf("  Categories (top %zu of %zu):\n", cat_limit, sorted.size());
            for (std::size_t i = 0; i < cat_limit; ++i) {
                std::printf("    %4zu  %s\n", sorted[i].second, sorted[i].first.c_str());
            }
            if (sorted.size() > kMaxCats) {
                std::size_t rest = 0;
                for (std::size_t i = kMaxCats; i < sorted.size(); ++i)
                    rest += sorted[i].second;
                std::printf("    %4zu  (+%zu more categories)\n", rest, sorted.size() - kMaxCats);
            }
        }
    }

    constexpr std::size_t kMaxTables = 16;
    auto const limit = def.tables().size() < kMaxTables ? def.tables().size() : kMaxTables;
    std::printf("  First %zu tables:\n", limit);
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &t = def.tables()[i];
        std::printf("    %dD  0x%08zX  %-32s  %s\n", t.dimensions, t.address, t.id.c_str(),
                    t.name.c_str());
    }
    if (def.tables().size() > kMaxTables) {
        std::printf("    ... %zu more not shown\n", def.tables().size() - kMaxTables);
    }
    std::printf("PIDs defined:   %zu\n", def.pids().size());
}

int cmd_dump_axis(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> axis_id;
    std::optional<std::filesystem::path> rom_path;
    bool csv = false;
    bool json_out = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("dump-axis: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--axis") {
            if (i + 1 >= argc) {
                std::fputs("dump-axis: --axis requires an id\n", stderr);
                return 2;
            }
            axis_id = std::string{argv[++i]};
        } else if (a == "--csv") {
            csv = true;
        } else if (a == "--json") {
            json_out = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "dump-axis: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "dump-axis: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (csv && json_out) {
        std::fputs("dump-axis: --json and --csv are mutually exclusive\n", stderr);
        return 2;
    }

    if (!def_path.has_value() || !axis_id.has_value() || !rom_path.has_value()) {
        std::fputs("dump-axis: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!axis_id.has_value())
            std::fputs(" --axis", stderr);
        if (!rom_path.has_value())
            std::fputs(" <FILE>", stderr);
        std::fputs(
            "\nUsage: subuwutuner-cli dump-axis --def <pack.toml> --axis <id> "
            "[--csv|--json] <FILE>\n",
            stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("dump-axis", *def_path, def.error());
    }
    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "dump-axis: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const axis = def->find_axis(*axis_id);
    if (axis == nullptr) {
        std::fprintf(stderr, "dump-axis: axis '%s' not found in pack\n", axis_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(def->axes().size());
        for (auto const &a : def->axes())
            ids.push_back(a.id);
        print_id_suggestions("axis", *axis_id, ids);
        return 1;
    }

    auto const values = def->read_axis_values(*rom, *axis);
    if (!values.has_value()) {
        std::fprintf(stderr, "dump-axis: %s\n", values.error().to_string().c_str());
        return 1;
    }

    auto const *scaling = def->find_scaling(axis->scaling);
    auto const unit = (scaling != nullptr ? scaling->unit : axis->unit);
    auto const precision = scaling != nullptr ? scaling->precision : 0;

    if (json_out) {
        // subuwutuner.dump-axis.v1 — axis header + values array.
        // Numbers emit at scaling precision to match the engineering
        // values consumers see in the GUI / dump-table grid.
        std::string out{"{\"schema\":\"subuwutuner.dump-axis.v1\",\"axis\":{\"id\":"};
        json_escape(out, axis->id);
        out.append(",\"unit\":");
        json_escape(out, unit);
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      ",\"precision\":%d,\"length\":%zu",
                      precision, values->size());
        out.append(buf);
        // Monotonicity check mirrors the text-mode summary so JSON
        // consumers can gate on the same "is this axis usable for
        // lookup?" predicate the human-readable summary surfaces.
        if (values->size() >= 2) {
            bool strictly_increasing = true;
            bool strictly_decreasing = true;
            for (std::size_t i = 1; i < values->size(); ++i) {
                if ((*values)[i] <= (*values)[i - 1])
                    strictly_increasing = false;
                if ((*values)[i] >= (*values)[i - 1])
                    strictly_decreasing = false;
            }
            char const *mono = strictly_increasing   ? "increasing"
                               : strictly_decreasing ? "decreasing"
                                                     : "not-monotonic";
            out.append(",\"monotonic\":\"");
            out.append(mono);
            out.append("\"");
        }
        out.append("},\"values\":[");
        for (std::size_t i = 0; i < values->size(); ++i) {
            if (i > 0) out.push_back(',');
            std::snprintf(buf, sizeof buf, "%.*f", precision, (*values)[i]);
            out.append(buf);
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    if (csv) {
        for (std::size_t i = 0; i < values->size(); ++i) {
            if (i > 0)
                std::printf(",");
            std::printf("%.*f", precision, (*values)[i]);
        }
        std::printf("\n");
    } else {
        std::printf("# %s  (%zu values%s%s)\n", axis->id.c_str(), values->size(),
                    unit.empty() ? "" : ", unit=", unit.c_str());

        // Summary: range + monotonicity. Axes MUST be strictly
        // monotonic for table-lookup interpolation to work; a
        // non-monotonic axis is either a load-error, a bad relocation
        // (post-cousin_seed before patch-pack), or a corrupted ROM.
        // Surfacing the check up-front avoids "the dump looks fine
        // but the table can't actually be used" surprises.
        if (values->size() >= 2) {
            double const min_v = *std::min_element(values->begin(), values->end());
            double const max_v = *std::max_element(values->begin(), values->end());
            bool strictly_increasing = true;
            bool strictly_decreasing = true;
            for (std::size_t i = 1; i < values->size(); ++i) {
                if ((*values)[i] <= (*values)[i - 1])
                    strictly_increasing = false;
                if ((*values)[i] >= (*values)[i - 1])
                    strictly_decreasing = false;
            }
            char const *mono = strictly_increasing   ? "strictly increasing"
                               : strictly_decreasing ? "strictly decreasing"
                                                     : "NOT monotonic — axis unusable for lookup";
            double const span = max_v - min_v;
            double const avg_step = span / static_cast<double>(values->size() - 1);
            std::printf("# range %.*f .. %.*f, step ~%.*f, %s\n", precision, min_v, precision,
                        max_v, precision, avg_step, mono);
        }

        for (auto const v : *values) {
            std::printf("%.*f\n", precision, v);
        }
    }
    return 0;
}

int cmd_dump_table(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> table_id;
    std::optional<std::filesystem::path> rom_path;
    bool csv = false;
    bool json_out = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("dump-table: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--table") {
            if (i + 1 >= argc) {
                std::fputs("dump-table: --table requires an id\n", stderr);
                return 2;
            }
            table_id = std::string{argv[++i]};
        } else if (a == "--csv") {
            csv = true;
        } else if (a == "--json") {
            json_out = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "dump-table: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "dump-table: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (csv && json_out) {
        std::fputs("dump-table: --json and --csv are mutually exclusive\n", stderr);
        return 2;
    }

    if (!def_path.has_value() || !table_id.has_value() || !rom_path.has_value()) {
        std::fputs("dump-table: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!rom_path.has_value())
            std::fputs(" <FILE>", stderr);
        std::fputs("\nUsage: subuwutuner-cli dump-table --def <pack.toml> --table <id> "
                   "[--csv|--json] <FILE>\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("dump-table", *def_path, def.error());
    }
    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "dump-table: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const *table = def->find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "dump-table: table '%s' not found in pack\n", table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(def->tables().size());
        for (auto const &t : def->tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }

    auto const td = def->read_table_values(*rom, *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "dump-table: %s\n", td.error().to_string().c_str());
        return 1;
    }

    auto const *scal = def->find_scaling(table->scaling);
    auto const precision = scal != nullptr ? scal->precision : 0;
    auto const unit = scal != nullptr ? scal->unit : std::string{};

    auto const &xs = td->axis_x;
    auto const &ys = td->axis_y;
    auto const &zs = td->axis_z;

    auto const print_slice_csv = [&](std::vector<std::vector<double>> const &grid) {
        // Scalars print as the single value on its own line — no axis
        // header row, no leading comma.
        if (xs.empty() && ys.empty() && grid.size() == 1 && grid[0].size() == 1) {
            std::printf("%.*f\n", precision, grid[0][0]);
            return;
        }
        for (auto const x : xs)
            std::printf(",%.*f", precision, x);
        std::printf("\n");
        for (std::size_t r = 0; r < grid.size(); ++r) {
            if (!ys.empty())
                std::printf("%.*f", precision, ys[r]);
            for (auto const v : grid[r])
                std::printf(",%.*f", precision, v);
            std::printf("\n");
        }
    };

    constexpr int kColWidth = 10;
    auto const print_slice_pretty = [&](std::vector<std::vector<double>> const &grid) {
        if (xs.empty() && ys.empty() && grid.size() == 1 && grid[0].size() == 1) {
            std::printf("%.*f\n", precision, grid[0][0]);
            return;
        }
        std::printf("%*s", kColWidth, "");
        for (auto const x : xs)
            std::printf(" %*.*f", kColWidth - 1, precision, x);
        std::printf("\n");
        for (std::size_t r = 0; r < grid.size(); ++r) {
            if (!ys.empty())
                std::printf("%*.*f", kColWidth, precision, ys[r]);
            else
                std::printf("%*s", kColWidth, "");
            for (auto const v : grid[r])
                std::printf(" %*.*f", kColWidth - 1, precision, v);
            std::printf("\n");
        }
    };

    if (json_out) {
        // subuwutuner.dump-table.v1 — header + axes + values
        // grid (2D) or slices array (3D). Numbers emit at the
        // table's scaling precision so JSON consumers see what
        // the engineering-unit grid actually shows.
        auto append_num = [precision](std::string &out, double v) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", precision, v);
            out.append(buf);
        };
        auto append_array = [&](std::string &out, std::vector<double> const &xs_in) {
            out.push_back('[');
            for (std::size_t i = 0; i < xs_in.size(); ++i) {
                if (i > 0) out.push_back(',');
                append_num(out, xs_in[i]);
            }
            out.push_back(']');
        };
        auto append_grid = [&](std::string &out, std::vector<std::vector<double>> const &grid) {
            out.push_back('[');
            for (std::size_t r = 0; r < grid.size(); ++r) {
                if (r > 0) out.push_back(',');
                append_array(out, grid[r]);
            }
            out.push_back(']');
        };

        std::string out{"{\"schema\":\"subuwutuner.dump-table.v1\",\"table\":{\"id\":"};
        json_escape(out, table->id);
        out.append(",\"name\":");
        json_escape(out, table->name);
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      ",\"dimensions\":%d,\"address\":%zu,",
                      table->dimensions, table->address);
        out.append(buf);
        out.append("\"unit\":");
        json_escape(out, unit);
        std::snprintf(buf, sizeof buf, ",\"precision\":%d", precision);
        out.append(buf);
        if (table->engine_safety_critical) {
            out.append(",\"engine_safety_critical\":true");
        }
        if (table->emissions_relevant) {
            out.append(",\"emissions_relevant\":true");
        }
        out.append("},\"axis_x\":");
        append_array(out, xs);
        out.append(",\"axis_y\":");
        append_array(out, ys);
        if (table->dimensions == 3) {
            out.append(",\"axis_z\":");
            append_array(out, zs);
            out.append(",\"slices\":[");
            for (std::size_t z = 0; z < td->slices.size(); ++z) {
                if (z > 0) out.push_back(',');
                append_grid(out, td->slices[z]);
            }
            out.push_back(']');
        } else {
            out.append(",\"values\":");
            append_grid(out, td->values);
        }
        out.push_back('}');
        out.push_back('\n');
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    if (csv) {
        if (table->dimensions == 3) {
            for (std::size_t z = 0; z < td->slices.size(); ++z) {
                std::printf("# z=%.*f\n", precision, zs.empty() ? 0.0 : zs[z]);
                print_slice_csv(td->slices[z]);
            }
        } else {
            print_slice_csv(td->values);
        }
        return 0;
    }

    std::printf("# %s  (%dD", table->id.c_str(), table->dimensions);
    if (!table->name.empty())
        std::printf(", %s", table->name.c_str());
    if (!unit.empty())
        std::printf(", unit=%s", unit.c_str());
    std::printf(")\n");

    // Summary line — at-a-glance sanity check on the table contents
    // (cell count, value range, mean, zero count). Useful when
    // localize.py reports a table as HIGH/MED but the values turn out
    // to be at the wrong address — the summary stats immediately
    // signal "this looks like noise" vs "this looks like a real cal".
    auto const compute_summary = [&]() {
        std::size_t n_rows = 0;
        std::size_t n_cols = 0;
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        double sum_v = 0.0;
        std::size_t count = 0;
        std::size_t zeros = 0;
        auto const visit = [&](std::vector<std::vector<double>> const &grid) {
            for (auto const &row : grid) {
                if (n_cols == 0)
                    n_cols = row.size();
                for (auto const v : row) {
                    min_v = std::min(min_v, v);
                    max_v = std::max(max_v, v);
                    sum_v += v;
                    ++count;
                    if (v == 0.0)
                        ++zeros;
                }
            }
            n_rows += grid.size();
        };
        if (table->dimensions == 3) {
            for (auto const &slice : td->slices)
                visit(slice);
        } else {
            visit(td->values);
        }
        return std::make_tuple(n_rows, n_cols, min_v, max_v, sum_v, count, zeros);
    };
    auto const [n_rows, n_cols, min_v, max_v, sum_v, count, zeros] = compute_summary();
    // Elide the summary for scalars — the value-on-a-line output is
    // already self-describing.
    if (count > 1) {
        double const mean_v = sum_v / static_cast<double>(count);
        std::printf("# %zu cols x %zu rows = %zu cells.  "
                    "min=%.*f  max=%.*f  mean=%.*f  zeros=%zu (%.1f%%)\n",
                    n_cols, n_rows, count, precision, min_v, precision, max_v, precision, mean_v,
                    zeros, 100.0 * static_cast<double>(zeros) / static_cast<double>(count));
    }

    if (table->dimensions == 3) {
        for (std::size_t z = 0; z < td->slices.size(); ++z) {
            std::printf("\n--- z = %.*f ---\n", precision, zs.empty() ? 0.0 : zs[z]);
            print_slice_pretty(td->slices[z]);
        }
    } else {
        print_slice_pretty(td->values);
    }
    return 0;
}

int cmd_project_step(int argc, char *argv[], bool forward) {
    char const *cmd_name = forward ? "project-redo" : "project-undo";
    if (argc < 1) {
        std::fprintf(stderr, "%s: missing project directory\n", cmd_name);
        std::fprintf(stderr, "Usage: subuwutuner-cli %s <dir>\n", cmd_name);
        return 2;
    }
    std::filesystem::path const dir{argv[0]};

    auto proj = st::Project::open(dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "%s: %s\n", cmd_name, proj.error().to_string().c_str());
        return 1;
    }

    st::edit::Edit const *edit_record = forward ? proj->history().redo() : proj->history().undo();
    if (edit_record == nullptr) {
        std::fprintf(stderr, "%s: no edit to %s\n", cmd_name, forward ? "redo" : "undo");
        return 1;
    }

    if (auto const *te = edit_record->as_table(); te != nullptr) {
        auto const *table = proj->definition().find_table(te->table_id);
        if (table == nullptr) {
            std::fprintf(stderr,
                         "%s: edit references table '%s' which is not in the current pack\n",
                         cmd_name, te->table_id.c_str());
            return 1;
        }

        auto td = proj->definition().read_table_values(proj->working_rom(), *table);
        if (!td.has_value()) {
            std::fprintf(stderr, "%s: %s\n", cmd_name, td.error().to_string().c_str());
            return 1;
        }

        // Undo restores `before`; redo restores `after`.
        auto const &snap = forward ? te->after : te->before;
        if (auto s = st::edit::restore(*td, snap); !s.has_value()) {
            std::fprintf(stderr, "%s: %s\n", cmd_name, s.error().to_string().c_str());
            return 1;
        }

        auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
        if (!wb.has_value()) {
            std::fprintf(stderr, "%s: writeback: %s\n", cmd_name, wb.error().to_string().c_str());
            return 1;
        }

        std::printf("%s applied edit: %s on %s\n", forward ? "Redo" : "Undo",
                    edit_record->description.c_str(), te->table_id.c_str());
    } else if (auto const *be = edit_record->as_byte(); be != nullptr) {
        // ByteEdit: walk the per-byte changes and write the appropriate side
        // directly to the working ROM. No definition indirection — works
        // even if the originating DTC entry was removed from the pack
        // between sessions.
        auto &rom = proj->working_rom();
        for (auto const &c : be->changes) {
            auto const v = forward ? c.after : c.before;
            if (auto s = rom.write_u8(c.address, v); !s.has_value()) {
                std::fprintf(stderr, "%s: byte writeback @0x%zX: %s\n", cmd_name, c.address,
                             s.error().to_string().c_str());
                return 1;
            }
        }
        std::printf("%s applied edit: %s (%zu byte%s)\n", forward ? "Redo" : "Undo",
                    edit_record->description.c_str(), be->changes.size(),
                    be->changes.size() == 1 ? "" : "s");
    }

    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "%s: save: %s\n", cmd_name, s.error().to_string().c_str());
        return 1;
    }

    std::printf("Working CRC32: 0x%08X\n", proj->working_rom().crc32());
    std::printf("History cursor: %zu / %zu\n", proj->history().cursor(), proj->history().size());
    return 0;
}

// `pack-info --json`. Schema subuwutuner.pack-info.v1. Same fields the
// text mode surfaces, structured for CI consumption. Validation result
// included as `validation: {ok: bool, message: string?}`.
int cmd_pack_info_json(std::filesystem::path const &path) {
    auto const def = st::Definition::from_file(resolve_def_path(path));
    if (!def.has_value()) {
        std::string out{"{\"schema\":\"subuwutuner.pack-info.v1\",\"path\":"};
        json_escape(out, path.string());
        out.append(",\"error\":");
        json_escape(out, def.error().to_string());
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
        return 1;
    }
    auto const &pack = def->pack();
    std::string out;
    out.reserve(2048);
    out.append("{\"schema\":\"subuwutuner.pack-info.v1\",\"path\":");
    json_escape(out, path.string());
    out.append(",\"schema_version\":");
    out.append(std::to_string(pack.schema_version));
    out.append(",\"id\":");
    json_escape(out, pack.id);
    out.append(",\"display_name\":");
    json_escape(out, pack.display_name);
    out.append(",\"platform\":");
    json_escape(out, pack.platform);
    out.append(",\"transmission\":");
    json_escape(out, pack.transmission);
    out.append(",\"years\":[");
    for (std::size_t i = 0; i < pack.years.size(); ++i) {
        if (i != 0)
            out.append(",");
        out.append(std::to_string(pack.years[i]));
    }
    out.append("],\"endianness\":");
    json_escape(out, pack.endianness);
    out.append(",\"expected_rom_size\":");
    out.append(std::to_string(pack.rom_size_bytes));
    out.append(",\"checksum_type\":");
    json_escape(out, pack.checksum_type);
    out.append(",\"license\":");
    json_escape(out, pack.license);
    if (pack.extends.has_value()) {
        out.append(",\"extends\":");
        json_escape(out, *pack.extends);
    } else {
        out.append(",\"extends\":null");
    }
    out.append(",\"includes\":[");
    for (std::size_t i = 0; i < pack.includes.size(); ++i) {
        if (i != 0)
            out.append(",");
        json_escape(out, pack.includes[i]);
    }
    out.append("]");

    // Identifications.
    out.append(",\"identifications\":[");
    for (std::size_t i = 0; i < def->identifications().size(); ++i) {
        auto const &id = def->identifications()[i];
        if (i != 0)
            out.append(",");
        out.append("{\"name\":");
        json_escape(out, id.name);
        out.append(",\"cid_match\":");
        json_escape(out, id.cid_match);
        if (id.cid_scan) {
            out.append(",\"cid_scan\":true,\"cid_address\":null");
        } else {
            out.append(",\"cid_scan\":false,\"cid_address\":");
            out.append(std::to_string(id.cid_address));
        }
        out.append("}");
    }
    out.append("]");

    // Counts.
    std::size_t emissions_tables = 0;
    std::size_t safety_tables = 0;
    for (auto const &t : def->tables()) {
        if (t.emissions_relevant)
            ++emissions_tables;
        if (t.engine_safety_critical)
            ++safety_tables;
    }
    std::size_t emissions_dtcs = 0;
    for (auto const &d : def->dtcs()) {
        if (d.emissions_relevant)
            ++emissions_dtcs;
    }
    out.append(",\"counts\":{");
    out.append("\"axes\":");
    out.append(std::to_string(def->axes().size()));
    out.append(",\"scalings\":");
    out.append(std::to_string(def->scalings().size()));
    out.append(",\"tables\":");
    out.append(std::to_string(def->tables().size()));
    out.append(",\"tables_emissions\":");
    out.append(std::to_string(emissions_tables));
    out.append(",\"tables_engine_safety\":");
    out.append(std::to_string(safety_tables));
    out.append(",\"pids\":");
    out.append(std::to_string(def->pids().size()));
    out.append(",\"switches\":");
    out.append(std::to_string(def->switches().size()));
    out.append(",\"dtc_bitmaps\":");
    out.append(std::to_string(def->dtc_bitmaps().size()));
    out.append(",\"dtcs\":");
    out.append(std::to_string(def->dtcs().size()));
    out.append(",\"dtcs_emissions\":");
    out.append(std::to_string(emissions_dtcs));
    out.append(",\"hooks\":");
    out.append(std::to_string(def->hooks().size()));
    out.append(",\"primitives\":");
    out.append(std::to_string(def->primitives().size()));
    out.append(",\"workflows\":");
    out.append(std::to_string(def->workflows().size()));
    out.append("}");

    // Workflows — pack-declared [[workflow]] entries with their
    // required_tables list + eligibility (supports_workflow). CI
    // scripts that gate on workflow availability consume this without
    // needing to re-parse the TOML.
    out.append(",\"workflows\":[");
    for (std::size_t i = 0; i < def->workflows().size(); ++i) {
        auto const &w = def->workflows()[i];
        if (i != 0)
            out.append(",");
        out.append("{\"id\":");
        json_escape(out, w.id);
        out.append(",\"display_name\":");
        json_escape(out, w.display_name);
        out.append(",\"modal\":");
        json_escape(out, w.modal);
        out.append(",\"required_tables\":[");
        for (std::size_t j = 0; j < w.required_tables.size(); ++j) {
            if (j != 0)
                out.append(",");
            json_escape(out, w.required_tables[j]);
        }
        out.append("],\"eligible\":");
        out.append(def->supports_workflow(w.id) ? "true" : "false");
        out.append("}");
    }
    out.append("]");

    // Validation.
    auto const validity = def->validate();
    out.append(",\"validation\":{\"ok\":");
    if (validity.has_value()) {
        out.append("true,\"message\":null}");
    } else {
        out.append("false,\"message\":");
        json_escape(out, std::string{validity.error().message()});
        out.append("}");
    }
    out.append("}\n");
    std::fputs(out.c_str(), stdout);
    return validity.has_value() ? 0 : 1;
}

int cmd_pack_info(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("pack-info: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli pack-info [--json] <DEF>\n", stderr);
        return 2;
    }
    bool json_mode = false;
    std::filesystem::path path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "pack-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (path.empty()) {
            path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "pack-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (path.empty()) {
        std::fputs("pack-info: missing path\n", stderr);
        return 2;
    }
    if (json_mode) {
        return cmd_pack_info_json(path);
    }
    auto const def = st::Definition::from_file(resolve_def_path(path));
    if (!def.has_value()) {
        return print_def_load_error("pack-info", path, def.error());
    }
    auto const &pack = def->pack();
    std::printf("Path:           %s\n", path.string().c_str());
    std::printf("Schema version: %d\n", pack.schema_version);
    std::printf("Id:             %s\n", pack.id.c_str());
    if (!pack.display_name.empty()) {
        std::printf("Display name:   %s\n", pack.display_name.c_str());
    }
    if (!pack.platform.empty()) {
        std::printf("Platform:       %s\n", pack.platform.c_str());
    }
    if (!pack.transmission.empty()) {
        std::printf("Transmission:   %s\n", pack.transmission.c_str());
    }
    if (!pack.years.empty()) {
        std::printf("Years:         ");
        for (int y : pack.years)
            std::printf(" %d", y);
        std::printf("\n");
    }
    std::printf("Endianness:     %s\n", pack.endianness.c_str());
    if (pack.rom_size_bytes != 0) {
        std::printf("Expected ROM:   %zu bytes (%.2f KiB)\n", pack.rom_size_bytes,
                    static_cast<double>(pack.rom_size_bytes) / 1024.0);
    }
    if (!pack.checksum_type.empty()) {
        // Surface the pack's declared checksum kind. Recognized
        // values (per the docs/11 enum) are subaru_std / subaru_alt
        // / subaru_alt2 / none; anything else passes through as-is
        // for forward-compat with packs from a future schema
        // revision.
        std::printf("Checksum type:  %s\n", pack.checksum_type.c_str());
    }
    if (!pack.license.empty()) {
        std::printf("License:        %s\n", pack.license.c_str());
    }
    if (pack.extends.has_value()) {
        std::printf("Extends:        %s\n", pack.extends->c_str());
    }
    if (!pack.includes.empty()) {
        std::printf("Includes:      ");
        for (auto const &inc : pack.includes)
            std::printf(" %s", inc.c_str());
        std::printf("\n");
    }
    std::printf("\n");
    std::printf("Identifications: %zu\n", def->identifications().size());
    for (auto const &id : def->identifications()) {
        if (id.cid_scan) {
            // Scan-mode identifications don't carry a meaningful
            // cid_address — the loader searches the whole ROM for
            // cid_match. Show "scan" instead of the misleading 0x0.
            std::printf("  - %s  (CID '%s', scan)\n", id.name.c_str(), id.cid_match.c_str());
        } else {
            std::printf("  - %s  (CID '%s' @ 0x%08zX)\n", id.name.c_str(), id.cid_match.c_str(),
                        id.cid_address);
        }
    }
    std::printf("Axes:            %zu\n", def->axes().size());
    std::printf("Scalings:        %zu\n", def->scalings().size());
    std::printf("Tables:          %zu\n", def->tables().size());
    {
        std::size_t emissions = 0;
        std::size_t safety = 0;
        for (auto const &t : def->tables()) {
            if (t.emissions_relevant)
                ++emissions;
            if (t.engine_safety_critical)
                ++safety;
        }
        if (emissions > 0) {
            std::printf("  emissions:     %zu\n", emissions);
        }
        if (safety > 0) {
            std::printf("  safety:        %zu\n", safety);
        }
    }
    std::printf("PIDs:            %zu\n", def->pids().size());
    std::printf("Switches:        %zu\n", def->switches().size());
    std::printf("DTC bitmaps:     %zu\n", def->dtc_bitmaps().size());
    std::printf("DTCs:            %zu\n", def->dtcs().size());
    {
        std::size_t emissions = 0;
        for (auto const &d : def->dtcs()) {
            if (d.emissions_relevant)
                ++emissions;
        }
        if (emissions > 0) {
            std::printf("  emissions:     %zu\n", emissions);
        }
    }
    std::printf("Hooks:           %zu\n", def->hooks().size());
    std::printf("Primitives:      %zu\n", def->primitives().size());
    if (!def->workflows().empty()) {
        std::printf("Workflows:       %zu\n", def->workflows().size());
        for (auto const &w : def->workflows()) {
            bool const supported = def->supports_workflow(w.id);
            std::printf("  - %s%s%s\n",
                        w.id.c_str(),
                        w.display_name.empty() ? "" : " ",
                        w.display_name.empty() ? "" : w.display_name.c_str());
            if (!supported) {
                std::printf("      (missing one or more required_tables — not eligible)\n");
            }
        }
    }

    // Quick validate so users see issues without running rom-info.
    auto const validity = def->validate();
    if (!validity.has_value()) {
        std::printf("\nValidation: ! %s\n", validity.error().message().data());
        return 1;
    }
    std::printf("\nValidation: OK\n");
    return 0;
}

// `pack-lint` — load a pack and report the result of Definition::validate()
// without printing the rest of the pack-info dump. Pack authors get a
// fast "is my pack well-formed?" answer that's also script-friendly.
//
// Exit codes:
//   0  pack loaded AND validate() returned ok()
//   1  pack failed to load OR validate() returned an error
//   2  bad CLI usage (missing path / unknown option)
//
// --json emits a one-line subuwutuner.pack-lint.v1 record so CI can
// gate without parsing prose.
int cmd_pack_lint(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("pack-lint: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli pack-lint [--json] <DEF>\n", stderr);
        return 2;
    }
    bool json_mode = false;
    std::filesystem::path path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "pack-lint: unknown option: %s\n", argv[i]);
            return 2;
        } else if (path.empty()) {
            path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "pack-lint: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (path.empty()) {
        std::fputs("pack-lint: missing path\n", stderr);
        return 2;
    }

    auto const resolved = resolve_def_path(path);
    auto const def = st::Definition::from_file(resolved);
    if (!def.has_value()) {
        if (json_mode) {
            std::string out{"{\"schema\":\"subuwutuner.pack-lint.v1\",\"path\":"};
            json_escape(out, path.string());
            out.append(",\"ok\":false,\"stage\":\"load\",\"error\":");
            json_escape(out, def.error().to_string());
            out.append("}\n");
            std::fputs(out.c_str(), stdout);
            return 1;
        }
        return print_def_load_error("pack-lint", path, def.error());
    }
    auto const v = def->validate();
    if (json_mode) {
        std::string out{"{\"schema\":\"subuwutuner.pack-lint.v1\",\"path\":"};
        json_escape(out, path.string());
        out.append(",\"id\":");
        json_escape(out, def->pack().id);
        out.append(",\"tables\":").append(std::to_string(def->tables().size()));
        out.append(",\"ok\":").append(v.has_value() ? "true" : "false");
        if (!v.has_value()) {
            out.append(",\"error\":");
            json_escape(out, v.error().to_string());
        }
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
        return v.has_value() ? 0 : 1;
    }
    std::printf("Pack: %s\n", def->pack().id.c_str());
    std::printf("Path: %s\n", path.string().c_str());
    std::printf("Tables: %zu  Scalings: %zu  PIDs: %zu  Hooks: %zu  Primitives: %zu\n",
                def->tables().size(), def->scalings().size(), def->pids().size(),
                def->hooks().size(), def->primitives().size());
    if (v.has_value()) {
        std::printf("\nValidation: OK\n");
        return 0;
    }
    std::printf("\nValidation: FAIL\n");
    std::printf("%s\n", v.error().to_string().c_str());
    return 1;
}

namespace {
char const *action_name(st::policy::Action a) noexcept {
    using A = st::policy::Action;
    switch (a) {
    case A::Silent:
        return "silent";
    case A::Badge:
        return "badge";
    case A::Warn:
        return "warn";
    case A::Confirm:
        return "confirm";
    case A::ConfirmWithReason:
        return "confirm+reason";
    case A::Block:
        return "block";
    }
    return "?";
}

void print_policy_row(st::policy::Profile p) {
    auto const emissions = st::policy::emissions_action(p);
    auto const safety = st::policy::engine_safety_on_flash(p);
    std::printf("%-18s  %-16s  %-16s  %-16s\n", std::string{st::policy::profile_name(p)}.c_str(),
                action_name(emissions.on_save), action_name(emissions.on_flash),
                action_name(safety));
}
} // namespace

int cmd_policy(int argc, char *argv[]) {
    std::optional<std::string> profile_name;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--profile") {
            if (i + 1 >= argc) {
                std::fputs("policy: --profile requires a value\n", stderr);
                return 2;
            }
            profile_name = std::string{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "policy: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "policy: unexpected positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    std::optional<st::policy::Profile> only;
    if (profile_name.has_value()) {
        only = st::policy::parse_profile(*profile_name);
        if (!only.has_value()) {
            std::fprintf(stderr,
                         "policy: unknown profile '%s'. Known: motorsport-only, "
                         "alberta-ca, eu-roadworthy, california-us\n",
                         profile_name->c_str());
            return 1;
        }
    }

    std::printf("%-18s  %-16s  %-16s  %-16s\n", "profile", "emissions-save", "emissions-flash",
                "safety-flash");
    std::printf("%-18s  %-16s  %-16s  %-16s\n", "------", "--------------", "---------------",
                "------------");

    if (only.has_value()) {
        print_policy_row(*only);
    } else {
        for (auto p : {st::policy::Profile::MotorsportOnly, st::policy::Profile::AlbertaCa,
                       st::policy::Profile::EuRoadworthy, st::policy::Profile::CaliforniaUs}) {
            print_policy_row(p);
        }
    }
    return 0;
}

int cmd_table_list(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> category_filter;
    bool emissions_only = false;
    bool safety_critical_only = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "table-list: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--category") {
            if (auto const *v = require("--category"); v)
                category_filter = std::string{v};
            else
                return 2;
        } else if (a == "--emissions") {
            emissions_only = true;
        } else if (a == "--safety-critical") {
            safety_critical_only = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "table-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "table-list: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("table-list: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli table-list <DEF> [--category C] "
                   "[--emissions] [--safety-critical]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("table-list", *def_path, def.error());
    }

    // Case-insensitive substring matcher for --category. Subaru's
    // category strings are dot-delimited and verbose ("boost control
    // - target", "ignition timing - knock control"); requiring an
    // exact match here means `--category boost` returns 0 rows even
    // though 25+ tables sit under "boost control - *". Substring
    // match is the natural mental model — `--category boost` ->
    // every boost-* category, `--category "boost control - target"`
    // -> just that subcategory.
    auto const cat_lower = [](std::string s) {
        for (char &c : s) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    std::string const category_needle =
        category_filter.has_value() ? cat_lower(*category_filter) : std::string{};

    std::size_t matched = 0;
    std::printf("%-3s %-3s %-10s %-32s %-18s %s\n", "D", "F", "address", "id", "category", "name");
    for (auto const &t : def->tables()) {
        if (!category_needle.empty() &&
            cat_lower(t.category).find(category_needle) == std::string::npos)
            continue;
        if (emissions_only && !t.emissions_relevant)
            continue;
        if (safety_critical_only && !t.engine_safety_critical)
            continue;
        char flags[3];
        flags[0] = t.emissions_relevant ? 'E' : '-';
        flags[1] = t.engine_safety_critical ? 'S' : '-';
        flags[2] = '\0';
        std::printf("%dD  %-3s 0x%08zX %-32s %-18s %s\n", t.dimensions, flags, t.address,
                    t.id.c_str(), t.category.c_str(), t.name.c_str());
        ++matched;
    }
    std::printf("\n%zu tables shown (of %zu in pack). "
                "Flags: E=emissions, S=engine-safety.\n",
                matched, def->tables().size());
    return 0;
}

// Render a signature pair from the graph designer's perspective as
// "consumed -> produced". Each side renders as a comma-separated
// list of type names, or `()` when empty. The arrow is always
// present so the direction is unambiguous (no chance of reading a
// bare `float` as "takes a float" when the hook actually emits one).
std::string format_signature(std::vector<st::HookSignal> const &consumed,
                             std::vector<st::HookSignal> const &produced) {
    auto join = [](std::vector<st::HookSignal> const &xs) {
        if (xs.empty())
            return std::string{"()"};
        std::string s;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i > 0)
                s += ',';
            s += xs[i].type.empty() ? "?" : xs[i].type;
        }
        return s;
    };
    return join(consumed) + " -> " + join(produced);
}

int cmd_primitive_list(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> type_filter;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--type") {
            if (i + 1 >= argc) {
                std::fputs("primitive-list: --type requires a value\n", stderr);
                return 2;
            }
            type_filter = std::string{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "primitive-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "primitive-list: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("primitive-list: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli primitive-list <DEF> "
                   "[--type int|float|bool]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("primitive-list", *def_path, def.error());
    }

    // Lowercase the filter once; primitive types in defs are lowercase
    // by convention ("int", "float", "bool") but tolerate user-typed
    // "INT" / "Float" without surprises.
    auto const ascii_lower = [](std::string s) {
        for (char &c : s) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    std::string const type_needle =
        type_filter.has_value() ? ascii_lower(*type_filter) : std::string{};

    std::size_t matched = 0;
    std::printf("%-24s %-32s %s\n", "id", "signature", "description");
    for (auto const &p : def->primitives()) {
        // --type filter: match when any output pin's type equals the
        // requested value. Most primitives have a single output so the
        // any-match semantics rarely surprises; for multi-output
        // primitives this lists the primitive if any output is the
        // requested type.
        if (!type_needle.empty()) {
            bool ok = false;
            for (auto const &o : p.outputs) {
                if (ascii_lower(o.type) == type_needle) {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                continue;
        }
        auto const sig = format_signature(p.inputs, p.outputs);
        // Truncate description for column fit; the user can re-read
        // the pack's primitives.toml for full text.
        std::string desc = p.description;
        if (desc.size() > 64)
            desc = desc.substr(0, 61) + "...";
        std::printf("%-24s %-32s %s\n", p.id.c_str(), sig.c_str(), desc.c_str());
        ++matched;
    }
    std::printf("\n%zu primitives shown (of %zu in pack).\n", matched, def->primitives().size());
    return 0;
}

int cmd_workflow_list(int argc, char *argv[]) {
    // Enumerate [[workflow]] entries declared by the pack. Useful for:
    //   - pack catalog browsing ("which packs support FA24 swap?")
    //   - end-to-end tests that assert a pack's declared workflows still
    //     resolve all required_tables after a regen
    //   - debugging the welcome card / Tools menu gating: if a workflow
    //     shows as "not eligible" here, the GUI's pack_supports_*()
    //     will also return false.
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> id_filter;
    bool only_eligible = false;
    bool emit_json = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--id") {
            if (i + 1 >= argc) {
                std::fputs("workflow-list: --id requires a value\n", stderr);
                return 2;
            }
            id_filter = std::string{argv[++i]};
        } else if (a == "--eligible") {
            only_eligible = true;
        } else if (a == "--json") {
            emit_json = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "workflow-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "workflow-list: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("workflow-list: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli workflow-list <DEF> "
                   "[--id <id>] [--eligible] [--json]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        if (emit_json) {
            std::string out{"{\"schema\":\"subuwutuner.workflow-list.v1\",\"path\":"};
            json_escape(out, def_path->string());
            out.append(",\"error\":");
            json_escape(out, def.error().to_string());
            out.append("}\n");
            std::fputs(out.c_str(), stdout);
            return 1;
        }
        return print_def_load_error("workflow-list", *def_path, def.error());
    }

    // JSON emitter — mirrors the pack-info --json shape so CI scripts
    // can consume both with the same JSON tooling. Schema name
    // subuwutuner.workflow-list.v1.
    if (emit_json) {
        std::string out{"{\"schema\":\"subuwutuner.workflow-list.v1\",\"path\":"};
        json_escape(out, def_path->string());
        out.append(",\"pack_id\":");
        json_escape(out, def->pack().id);
        out.append(",\"workflows\":[");
        std::size_t shown = 0;
        bool any_ineligible_unfiltered = false;
        for (auto const &w : def->workflows()) {
            if (id_filter.has_value() && w.id != *id_filter) {
                continue;
            }
            bool const eligible = def->supports_workflow(w.id);
            if (!id_filter.has_value() && !eligible) {
                any_ineligible_unfiltered = true;
            }
            if (only_eligible && !eligible) {
                continue;
            }
            if (shown != 0) {
                out.append(",");
            }
            out.append("{\"id\":");
            json_escape(out, w.id);
            out.append(",\"display_name\":");
            json_escape(out, w.display_name);
            out.append(",\"modal\":");
            json_escape(out, w.modal);
            out.append(",\"required_tables\":[");
            for (std::size_t j = 0; j < w.required_tables.size(); ++j) {
                if (j != 0) {
                    out.append(",");
                }
                json_escape(out, w.required_tables[j]);
            }
            out.append("],\"required_tables_present\":[");
            for (std::size_t j = 0; j < w.required_tables.size(); ++j) {
                if (j != 0) {
                    out.append(",");
                }
                out.append(def->find_table(w.required_tables[j]) != nullptr ? "true"
                                                                            : "false");
            }
            out.append("],\"eligible\":");
            out.append(eligible ? "true" : "false");
            out.append("}");
            ++shown;
        }
        out.append("],\"shown\":");
        out.append(std::to_string(shown));
        out.append(",\"declared\":");
        out.append(std::to_string(def->workflows().size()));
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
        return any_ineligible_unfiltered ? 1 : 0;
    }

    if (def->workflows().empty()) {
        std::printf("Pack declares no workflows.\n");
        return 0;
    }

    std::size_t shown = 0;
    for (auto const &w : def->workflows()) {
        if (id_filter.has_value() && w.id != *id_filter) {
            continue;
        }
        bool const eligible = def->supports_workflow(w.id);
        if (only_eligible && !eligible) {
            continue;
        }
        std::printf("%-18s %s%s\n",
                    w.id.c_str(),
                    w.display_name.empty() ? "" : w.display_name.c_str(),
                    eligible ? "" : "  (not eligible)");
        if (!w.modal.empty()) {
            std::printf("    modal:           %s\n", w.modal.c_str());
        }
        std::printf("    required_tables: %zu\n", w.required_tables.size());
        // Per-table presence: surface which IDs are missing so a regen
        // failure is obvious without grepping the pack TOML manually.
        for (auto const &table_id : w.required_tables) {
            bool const present = def->find_table(table_id) != nullptr;
            std::printf("      %s  %s\n", present ? "OK  " : "MISS",
                        table_id.c_str());
        }
        ++shown;
    }
    std::printf("\n%zu workflow%s shown (of %zu declared).\n",
                shown, shown == 1 ? "" : "s", def->workflows().size());
    // Exit code: 0 on clean, 1 if any shown workflow is "not eligible"
    // when no --id filter was set — useful as a CI gate that a pack's
    // declared workflows actually resolve.
    if (!id_filter.has_value()) {
        for (auto const &w : def->workflows()) {
            if (!def->supports_workflow(w.id)) {
                return 1;
            }
        }
    }
    return 0;
}

int cmd_hook_list(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    bool only_sensor = false;
    bool only_action = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--sensor") {
            only_sensor = true;
        } else if (a == "--action") {
            only_action = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "hook-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "hook-list: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("hook-list: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli hook-list <DEF> "
                   "[--sensor] [--action]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("hook-list", *def_path, def.error());
    }

    std::size_t matched = 0;
    std::printf("%-24s %-10s %-32s %s\n", "id", "addr", "signature", "description");
    for (auto const &h : def->hooks()) {
        // Sensor hooks expose data to user logic (inputs from the
        // ECU); action hooks consume user-written values (outputs
        // back into the ECU). A pure-sensor hook has no outputs;
        // a pure-action hook has no inputs. Mixed hooks (override
        // splices like after_fuel_calc) have both.
        bool const sensor = !h.inputs.empty() && h.outputs.empty();
        bool const action = h.inputs.empty() && !h.outputs.empty();
        if (only_sensor && !sensor)
            continue;
        if (only_action && !action)
            continue;

        char addr_buf[16];
        if (h.ecu_address.has_value()) {
            std::snprintf(addr_buf, sizeof addr_buf, "0x%06zX", *h.ecu_address);
        } else {
            std::snprintf(addr_buf, sizeof addr_buf, "%s", "-");
        }
        // Per docs/16, a Hook's `inputs` are signals flowing FROM the
        // ECU into user logic (graph designer renders as Output pins
        // on the hook node); `outputs` are values flowing FROM user
        // logic back into the ECU (rendered as Input pins). Swap the
        // pair so the printed signature reads in graph-designer
        // direction ("what the hook consumes -> what it produces").
        auto const sig = format_signature(h.outputs, h.inputs);
        std::string desc = h.description;
        if (desc.size() > 48)
            desc = desc.substr(0, 45) + "...";
        std::printf("%-24s %-10s %-32s %s\n", h.id.c_str(), addr_buf, sig.c_str(), desc.c_str());
        ++matched;
    }
    std::printf("\n%zu hooks shown (of %zu in pack).\n", matched, def->hooks().size());
    return 0;
}

int cmd_pack_dtcs(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::string> bitmap_filter;
    bool emissions_only = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "pack-dtcs: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--bitmap") {
            if (auto const *v = require("--bitmap"); v)
                bitmap_filter = std::string{v};
            else
                return 2;
        } else if (a == "--emissions") {
            emissions_only = true;
        } else if (a == "--show-state") {
            if (auto const *v = require("--show-state"); v)
                rom_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "pack-dtcs: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "pack-dtcs: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("pack-dtcs: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli pack-dtcs <DEF> "
                   "[--bitmap <id>] [--emissions] [--show-state <ROM>]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("pack-dtcs", *def_path, def.error());
    }

    // Optional ROM read for the "On" column. Loaded once; we just need
    // byte access to compute per-DTC enable state.
    std::optional<st::Rom> rom;
    if (rom_path.has_value()) {
        auto r = st::Rom::from_file(*rom_path);
        if (!r.has_value()) {
            std::fprintf(stderr, "pack-dtcs: --show-state ROM: %s\n",
                         r.error().to_string().c_str());
            return 1;
        }
        rom = std::move(*r);
    }

    if (def->dtc_bitmaps().empty() && def->dtcs().empty()) {
        std::printf("(no DTC bitmaps or codes declared in this pack)\n");
        return 0;
    }

    if (!def->dtc_bitmaps().empty()) {
        std::printf("Bitmaps:\n");
        for (auto const &b : def->dtc_bitmaps()) {
            std::printf("  %-24s @ 0x%08zX  %zu bytes\n", b.id.c_str(), b.address, b.length_bytes);
        }
        std::printf("\n");
    }

    std::size_t matched = 0;
    std::size_t disabled_cnt = 0;
    if (rom.has_value()) {
        std::printf("%-6s %-2s %-1s %-32s %s\n", "code", "On", "F", "location", "name");
    } else {
        std::printf("%-6s %-1s %-32s %s\n", "code", "F", "location", "name");
    }
    for (auto const &d : def->dtcs()) {
        if (emissions_only && !d.emissions_relevant)
            continue;
        if (bitmap_filter.has_value() && d.bitmap_id != *bitmap_filter)
            continue;
        char location[64];
        std::snprintf(location, sizeof(location), "%s:%zu.%d", d.bitmap_id.c_str(), d.byte_offset,
                      d.bit);
        if (rom.has_value()) {
            auto const *bm = def->find_dtc_bitmap(d.bitmap_id);
            char on_glyph = '?';
            if (bm != nullptr) {
                auto const en = st::is_dtc_enabled(*rom, *bm, d);
                if (en.has_value()) {
                    on_glyph = *en ? 'Y' : 'n';
                    if (!*en)
                        ++disabled_cnt;
                }
            }
            std::printf("%-6s %c  %c %-32s %s\n", d.code.c_str(), on_glyph,
                        d.emissions_relevant ? 'E' : '-', location, d.name.c_str());
        } else {
            std::printf("%-6s %c %-32s %s\n", d.code.c_str(), d.emissions_relevant ? 'E' : '-',
                        location, d.name.c_str());
        }
        ++matched;
    }
    if (rom.has_value()) {
        std::printf("\n%zu DTC(s) shown (of %zu in pack); %zu disabled. "
                    "Flags: On=Y/n, F=E (emissions-relevant).\n",
                    matched, def->dtcs().size(), disabled_cnt);
    } else {
        std::printf("\n%zu DTC(s) shown (of %zu in pack). "
                    "Flag: E=emissions-relevant.\n",
                    matched, def->dtcs().size());
    }
    return 0;
}

// `project-disable-dtc` / `project-enable-dtc` — flip the enable bit for one
// or more DTC codes inside the project's working ROM. The byte writes are
// captured as a single ByteEdit committed through edit::History, so a
// follow-up `project-undo` restores the original bit pattern. The flash-time
// policy gate still applies on emissions-flagged DTCs when the user runs
// project-flash.
int cmd_project_dtc_toggle(int argc, char *argv[], bool enable) {
    char const *cmd_name = enable ? "project-enable-dtc" : "project-disable-dtc";
    std::optional<std::string> codes_arg;
    std::optional<std::filesystem::path> proj_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s requires a value\n", cmd_name, name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--code") {
            if (auto const *v = require("--code"); v)
                codes_arg = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "%s: unknown option: %s\n", cmd_name, argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "%s: extra argument: %s\n", cmd_name, argv[i]);
            return 2;
        }
    }

    if (!codes_arg.has_value() || !proj_path.has_value()) {
        std::fprintf(stderr, "%s: missing required arguments:", cmd_name);
        if (!codes_arg.has_value())
            std::fputs(" --code", stderr);
        if (!proj_path.has_value())
            std::fputs(" <dir>", stderr);
        std::fprintf(stderr, "\nUsage: subuwutuner-cli %s --code P0401[,P0420,...] <dir>\n",
                     cmd_name);
        return 2;
    }

    // Split the comma-separated code list.
    std::vector<std::string> codes;
    {
        std::string_view rest = *codes_arg;
        while (!rest.empty()) {
            auto const comma = rest.find(',');
            auto const token = rest.substr(0, comma);
            if (!token.empty())
                codes.emplace_back(token);
            if (comma == std::string_view::npos)
                break;
            rest = rest.substr(comma + 1);
        }
    }
    if (codes.empty()) {
        std::fprintf(stderr, "%s: --code list is empty\n", cmd_name);
        return 2;
    }

    auto proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "%s: %s\n", cmd_name, proj.error().to_string().c_str());
        return 1;
    }

    auto const &def = proj->definition();
    auto &rom = proj->working_rom();

    // We collect ByteEdit::Change entries during the toggle loop and
    // commit them as one undoable Edit. Two DTCs sharing the same byte
    // (common — DTC enable bits are packed) coalesce: a second toggle on
    // the same address updates the prior entry's `after` rather than
    // appending a new entry, so the recorded delta matches the ROM's
    // final state and an undo restores the original byte.
    std::vector<st::edit::ByteEdit::Change> changes;
    std::size_t emissions_count = 0;
    std::vector<std::string> touched_codes;
    for (auto const &code : codes) {
        auto const *dtc = def.find_dtc(code);
        if (dtc == nullptr) {
            std::fprintf(stderr, "%s: code '%s' not found in pack\n", cmd_name, code.c_str());
            return 1;
        }
        auto const *bm = def.find_dtc_bitmap(dtc->bitmap_id);
        if (bm == nullptr) {
            std::fprintf(stderr, "%s: code '%s' references unknown bitmap '%s'\n", cmd_name,
                         code.c_str(), dtc->bitmap_id.c_str());
            return 1;
        }
        auto result = st::set_dtc_enabled(rom, *bm, *dtc, enable);
        if (!result.has_value()) {
            std::fprintf(stderr, "%s: %s\n", cmd_name, result.error().to_string().c_str());
            return 1;
        }
        bool const byte_changed = result->before != result->after;
        if (byte_changed) {
            auto it = std::find_if(
                changes.begin(), changes.end(),
                [&](st::edit::ByteEdit::Change const &c) { return c.address == result->address; });
            if (it == changes.end()) {
                changes.push_back({result->address, result->before, result->after});
            } else {
                it->after = result->after;
            }
            touched_codes.push_back(dtc->code);
        }
        if (dtc->emissions_relevant)
            ++emissions_count;
        std::printf("  %s %-6s %s  (byte 0x%02X -> 0x%02X)%s\n", enable ? "ENABLE " : "DISABLE",
                    dtc->code.c_str(), byte_changed ? "changed" : "no-op  ", result->before,
                    result->after, dtc->emissions_relevant ? "  [emissions]" : "");
    }

    if (!changes.empty()) {
        std::string desc;
        desc.reserve(64);
        desc.append(enable ? "enable DTC " : "disable DTC ");
        for (std::size_t i = 0; i < touched_codes.size(); ++i) {
            if (i > 0)
                desc.append(", ");
            desc.append(touched_codes[i]);
        }
        proj->history().record(st::edit::Edit::bytes(std::move(changes), std::move(desc)));
        if (auto s = proj->save_working_rom(); !s.has_value()) {
            std::fprintf(stderr, "%s: save: %s\n", cmd_name, s.error().to_string().c_str());
            return 1;
        }
        std::printf("\nWorking ROM saved. New CRC32: 0x%08X\n", rom.crc32());
    } else {
        std::printf("\nNo bits changed; working ROM not rewritten.\n");
    }
    if (emissions_count > 0) {
        std::printf("Note: %zu of the affected codes are emissions-flagged. "
                    "The flash-time policy gate will surface this when you "
                    "run project-flash under a non-motorsport profile.\n",
                    emissions_count);
    }
    return 0;
}

int cmd_project_edit(int argc, char *argv[]) {
    std::optional<std::string> table_id;
    std::optional<std::string> rows_arg;
    std::optional<std::string> cols_arg;
    std::optional<std::string> op;
    std::optional<double> value;
    std::optional<std::filesystem::path> proj_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-edit: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--rows") {
            if (auto const *v = require("--rows"); v)
                rows_arg = std::string{v};
            else
                return 2;
        } else if (a == "--cols") {
            if (auto const *v = require("--cols"); v)
                cols_arg = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-edit: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!op.has_value()) {
            op = std::string{a};
        } else if (!value.has_value() && op != "smooth" && op != "interpolate") {
            // libc++ (Apple Clang) does not implement std::from_chars for
            // floating-point types through LLVM 18, so route through
            // std::strtod for portability.
            std::string const buf{a};
            char *end = nullptr;
            errno = 0;
            double const d = std::strtod(buf.c_str(), &end);
            if (errno == 0 && end == buf.c_str() + buf.size() && !buf.empty()) {
                value = d;
            } else if (!proj_path.has_value()) {
                proj_path = std::filesystem::path{a};
            } else {
                std::fprintf(stderr, "project-edit: extra argument: %s\n", argv[i]);
                return 2;
            }
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-edit: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!table_id.has_value() || !op.has_value() || !proj_path.has_value()) {
        std::fputs("project-edit: missing required arguments:", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!op.has_value())
            std::fputs(" OP", stderr);
        if (!proj_path.has_value())
            std::fputs(" <dir>", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-edit --table <id> [--rows A:B] "
                   "[--cols A:B] OP [VALUE] <dir>\n",
                   stderr);
        return 2;
    }

    bool const op_needs_value =
        *op == "set" || *op == "add" || *op == "multiply" || *op == "percent";
    if (op_needs_value && !value.has_value()) {
        std::fprintf(stderr, "project-edit: op '%s' requires a numeric value\n", op->c_str());
        return 2;
    }

    auto proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", proj.error().to_string().c_str());
        return 1;
    }

    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-edit: table '%s' not found in pack\n", table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(proj->definition().tables().size());
        for (auto const &t : proj->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }

    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", td.error().to_string().c_str());
        return 1;
    }

    st::edit::Rect rect = st::edit::whole_table(*td);
    if (rows_arg.has_value() && !parse_range(*rows_arg, rect.r_start, rect.r_end)) {
        std::fprintf(stderr, "project-edit: bad --rows: %s\n", rows_arg->c_str());
        return 2;
    }
    if (cols_arg.has_value() && !parse_range(*cols_arg, rect.c_start, rect.c_end)) {
        std::fprintf(stderr, "project-edit: bad --cols: %s\n", cols_arg->c_str());
        return 2;
    }

    // Capture before-snapshot (for the history record).
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", before.error().to_string().c_str());
        return 1;
    }

    st::Status status = st::ok();
    if (*op == "set")
        status = st::edit::set_cells(*td, rect, *value);
    else if (*op == "add")
        status = st::edit::add_cells(*td, rect, *value);
    else if (*op == "multiply")
        status = st::edit::multiply_cells(*td, rect, *value);
    else if (*op == "percent")
        status = st::edit::percent_scale_cells(*td, rect, *value);
    else if (*op == "smooth")
        status = st::edit::smooth_cells(*td, rect, 1);
    else if (*op == "interpolate")
        status = st::edit::interpolate_cells(*td, rect);
    else {
        std::fprintf(stderr, "project-edit: unknown op '%s'\n", op->c_str());
        return 2;
    }
    if (!status.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", status.error().to_string().c_str());
        return 1;
    }

    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", after.error().to_string().c_str());
        return 1;
    }

    auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "project-edit: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }

    // Build a short human-readable description for the history record.
    std::string desc{*op};
    if (op_needs_value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %g", *value);
        desc.append(buf);
    }
    proj->history().record(
        st::edit::Edit::table(table->id, std::move(*before), std::move(*after), std::move(desc)));

    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "project-edit: save: %s\n", s.error().to_string().c_str());
        return 1;
    }

    std::printf("Table:      %s\n", table->id.c_str());
    std::printf("Op:         %s", op->c_str());
    if (op_needs_value)
        std::printf(" %g", *value);
    std::printf("\n");
    std::printf("Selection:  rows %zu..%zu, cols %zu..%zu (%zu cells)\n", rect.r_start, rect.r_end,
                rect.c_start, rect.c_end, rect.rows() * rect.cols());
    std::printf("Saved to:   %s\n", proj_path->string().c_str());
    std::printf("New CRC32:  0x%08X\n", proj->working_rom().crc32());
    return 0;
}

int cmd_project_edit_csv(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_path;
    std::optional<std::string> table_id;
    std::optional<std::filesystem::path> csv_path;
    bool dry_run = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-edit-csv: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--from") {
            if (auto const *v = require("--from"); v)
                csv_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--dry-run") {
            dry_run = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-edit-csv: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-edit-csv: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_path.has_value() || !table_id.has_value() || !csv_path.has_value()) {
        std::fputs("project-edit-csv: missing required arguments:", stderr);
        if (!proj_path.has_value())
            std::fputs(" <dir>", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!csv_path.has_value())
            std::fputs(" --from", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-edit-csv <dir> "
                   "--table <id> --from <FILE.csv> [--dry-run]\n"
                   "  CSV format: optional header row, then one edit per row\n"
                   "  in the shape `row,col,value` (engineering units; the\n"
                   "  pack's scaling does the byte conversion). Applied as a\n"
                   "  single project edit through edit::History.\n"
                   "  --dry-run validates and previews the edits (table+bounds\n"
                   "  match, cells in range, identity headers OK) and prints a\n"
                   "  before->after preview without touching the project.\n",
                   stderr);
        return 2;
    }

    auto proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-edit-csv: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-edit-csv: table '%s' not found in pack\n", table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(proj->definition().tables().size());
        for (auto const &t : proj->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }
    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "project-edit-csv: %s\n", td.error().to_string().c_str());
        return 1;
    }
    std::size_t const rows = td->values.size();
    std::size_t const cols = rows > 0 ? td->values[0].size() : 0;

    // Slurp the CSV and hand it to st::parse_edit_csv. The parser is the
    // single source of truth for identity-header validation, bounds
    // checking, and comment/blank-line handling; the GUI's Import path
    // goes through the same code.
    std::ifstream in{*csv_path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "project-edit-csv: cannot open %s\n", csv_path->string().c_str());
        return 1;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string const text = std::move(buf).str();

    st::EditCsvParseOptions opts;
    opts.expected_pack_id = proj->definition().pack().id;
    opts.expected_table_id = table->id;
    opts.table_rows = rows;
    opts.table_cols = cols;
    auto parsed = st::parse_edit_csv(text, opts);
    if (!parsed.has_value()) {
        std::fprintf(stderr, "project-edit-csv: %s\n", parsed.error().to_string().c_str());
        return 1;
    }
    for (auto const &w : parsed->warnings) {
        std::fprintf(stderr, "project-edit-csv: WARNING: %s\n", w.message.c_str());
    }
    auto const &edits = parsed->cells;
    if (edits.empty()) {
        std::fputs("project-edit-csv: no edit rows parsed; nothing to do.\n", stderr);
        return 0;
    }

    // Bounding rect over all touched cells. Snapshot once, mutate, snapshot.
    std::size_t r_min = edits[0].row, r_max = edits[0].row;
    std::size_t c_min = edits[0].col, c_max = edits[0].col;
    for (auto const &e : edits) {
        r_min = std::min(r_min, e.row);
        r_max = std::max(r_max, e.row);
        c_min = std::min(c_min, e.col);
        c_max = std::max(c_max, e.col);
    }
    if (dry_run) {
        auto const *scaling = proj->definition().find_scaling(table->scaling);
        int const prec = scaling != nullptr ? scaling->precision : 6;
        std::printf("Table:      %s\n", table->id.c_str());
        std::printf("Cells:      %zu  (dry-run; no edits applied)\n", edits.size());
        std::printf("Bounding:   rows %zu..%zu, cols %zu..%zu\n", r_min, r_max, c_min, c_max);
        constexpr std::size_t kPreviewLimit = 10;
        std::size_t const shown = std::min(kPreviewLimit, edits.size());
        std::printf("Preview:    %zu of %zu edits (row,col: before -> after)\n", shown,
                    edits.size());
        for (std::size_t i = 0; i < shown; ++i) {
            auto const &e = edits[i];
            std::printf("  (%zu,%zu): %.*f -> %.*f\n", e.row, e.col, prec, td->values[e.row][e.col],
                        prec, e.value);
        }
        if (edits.size() > shown) {
            std::printf("  ... %zu more not shown\n", edits.size() - shown);
        }
        return 0;
    }
    st::edit::Rect const rect{r_min, r_max, c_min, c_max};
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        std::fprintf(stderr, "project-edit-csv: %s\n", before.error().to_string().c_str());
        return 1;
    }
    for (auto const &e : edits)
        td->values[e.row][e.col] = e.value;
    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        std::fprintf(stderr, "project-edit-csv: %s\n", after.error().to_string().c_str());
        return 1;
    }
    if (auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
        !wb.has_value()) {
        std::fprintf(stderr, "project-edit-csv: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "csv import (%zu cell%s)", edits.size(),
                  edits.size() == 1 ? "" : "s");
    proj->history().record(st::edit::Edit::table(table->id, std::move(*before), std::move(*after),
                                                 std::string{descbuf}));
    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "project-edit-csv: save: %s\n", s.error().to_string().c_str());
        return 1;
    }
    std::printf("Table:      %s\n", table->id.c_str());
    std::printf("Cells:      %zu\n", edits.size());
    std::printf("Bounding:   rows %zu..%zu, cols %zu..%zu\n", r_min, r_max, c_min, c_max);
    std::printf("New CRC32:  0x%08X\n", proj->working_rom().crc32());
    return 0;
}

int cmd_project_export_csv(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_path;
    std::optional<std::string> table_id;
    std::optional<std::filesystem::path> output_path;
    bool diff_only = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-export-csv: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--diff-only") {
            diff_only = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-export-csv: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-export-csv: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_path.has_value() || !table_id.has_value()) {
        std::fputs("project-export-csv: missing required arguments:", stderr);
        if (!proj_path.has_value())
            std::fputs(" <dir>", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-export-csv <dir> "
                   "--table <id> [--diff-only] [--output <FILE.csv>]\n"
                   "  Emits the table's working-ROM cells in the "
                   "`row,col,value` format that project-edit-csv consumes.\n"
                   "  --diff-only restricts the output to cells whose working\n"
                   "  value differs from the source. Without --output, the CSV\n"
                   "  goes to stdout.\n",
                   stderr);
        return 2;
    }
    auto proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-export-csv: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-export-csv: table '%s' not found in pack\n",
                     table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(proj->definition().tables().size());
        for (auto const &t : proj->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }
    auto const working_td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!working_td.has_value()) {
        std::fprintf(stderr, "project-export-csv: %s\n", working_td.error().to_string().c_str());
        return 1;
    }
    std::optional<st::Definition::TableData> source_td;
    if (diff_only) {
        auto s = proj->definition().read_table_values(proj->source_rom(), *table);
        if (!s.has_value()) {
            std::fprintf(stderr, "project-export-csv: source read: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        source_td = std::move(*s);
    }

    auto const *scaling = proj->definition().find_scaling(table->scaling);
    auto const prec = scaling != nullptr ? scaling->precision : 6;

    // Open output (default stdout).
    std::ofstream file;
    std::ostream *out = &std::cout;
    if (output_path.has_value()) {
        file.open(*output_path, std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "project-export-csv: cannot open %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out = &file;
    }
    // Identity header — lets project-edit-csv verify on import that the
    // CSV is being applied against a matching pack + table. Both lines
    // are # comments so the file still round-trips as plain CSV through
    // generic tools.
    *out << "# pack_id = \"" << proj->definition().pack().id << "\"\n";
    *out << "# table   = \"" << table->id << "\"\n";
    *out << "row,col,value\n";
    std::size_t emitted = 0;
    char buf[64];
    for (std::size_t r = 0; r < working_td->values.size(); ++r) {
        for (std::size_t c = 0; c < working_td->values[r].size(); ++c) {
            double const v = working_td->values[r][c];
            if (diff_only) {
                if (r >= source_td->values.size() || c >= source_td->values[r].size())
                    continue;
                if (v == source_td->values[r][c])
                    continue;
            }
            std::snprintf(buf, sizeof buf, "%zu,%zu,%.*f\n", r, c, prec, v);
            *out << buf;
            ++emitted;
        }
    }
    if (output_path.has_value()) {
        std::fprintf(stderr, "project-export-csv: wrote %zu cell%s to %s\n", emitted,
                     emitted == 1 ? "" : "s", output_path->string().c_str());
    }
    return 0;
}

int cmd_project_new(int argc, char *argv[]) {
    std::optional<std::filesystem::path> source_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> proj_path;
    std::string display_name;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-new: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--source") {
            if (auto const *v = require("--source"); v)
                source_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--def") {
            if (auto const *v = require("--def"); v)
                def_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--name") {
            if (auto const *v = require("--name"); v)
                display_name = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-new: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-new: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!source_path.has_value() || !def_path.has_value() || !proj_path.has_value()) {
        std::fputs("project-new: missing required arguments:", stderr);
        if (!source_path.has_value())
            std::fputs(" --source", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!proj_path.has_value())
            std::fputs(" <dir>", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-new --source <rom> --def <pack> "
                   "[--name <name>] <dir>\n",
                   stderr);
        return 2;
    }
    if (display_name.empty()) {
        display_name = proj_path->filename().string();
    }

    auto p = st::Project::create(*proj_path, *source_path, *def_path, display_name);
    if (!p.has_value()) {
        return print_def_load_error("project-new", *def_path, p.error());
    }

    std::printf("Created project: %s\n", proj_path->string().c_str());
    std::printf("  Name:       %s\n", p->display_name().c_str());
    std::printf("  Source:     %s  (CRC32=0x%08X, %zu bytes)\n", source_path->string().c_str(),
                p->source_rom().crc32(), p->source_rom().size());
    std::printf("  Definition: %s  (pack id: %s)\n", def_path->string().c_str(),
                p->definition().pack().id.c_str());
    auto const cid = p->definition().matches(p->source_rom());
    std::printf("  CID match:  %s\n", cid.has_value() ? cid->c_str() : "(no match)");
    return 0;
}

// `project-info --json`. Schema subuwutuner.project-info.v1. Captures
// the structured project state for CI scripts (have edits landed?
// does the working ROM differ from source? did source.bin change?).
// Skips the prose-heavy CID-suggestion heuristic from text mode —
// that's for interactive diagnosis, not scripting.
int cmd_project_info_json(std::filesystem::path const &dir) {
    auto p = st::Project::open(dir);
    if (!p.has_value()) {
        std::string out{"{\"schema\":\"subuwutuner.project-info.v1\",\"path\":"};
        json_escape(out, dir.string());
        out.append(",\"error\":");
        json_escape(out, p.error().to_string());
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
        return 1;
    }

    std::string out;
    out.reserve(1024);
    out.append("{\"schema\":\"subuwutuner.project-info.v1\",\"path\":");
    json_escape(out, dir.string());
    out.append(",\"name\":");
    json_escape(out, p->display_name());
    out.append(",\"notes\":");
    json_escape(out, p->notes());

    out.append(",\"source_rom\":{\"size\":");
    out.append(std::to_string(p->source_rom().size()));
    out.append(",\"crc32\":");
    out.append(std::to_string(p->source_rom().crc32()));
    out.append(",\"crc32_at_create\":");
    out.append(std::to_string(p->source_crc32_at_create()));
    out.append(",\"changed_since_create\":");
    out.append(p->source_rom().crc32() != p->source_crc32_at_create() ? "true" : "false");
    out.append("}");

    out.append(",\"working_rom\":{\"size\":");
    out.append(std::to_string(p->working_rom().size()));
    out.append(",\"crc32\":");
    out.append(std::to_string(p->working_rom().crc32()));
    out.append(",\"differs_from_source\":");
    out.append(p->source_rom().crc32() != p->working_rom().crc32() ? "true" : "false");
    out.append("}");

    out.append(",\"definition\":{\"pack_id\":");
    json_escape(out, p->definition().pack().id);
    out.append(",\"table_count\":");
    out.append(std::to_string(p->definition().tables().size()));
    out.append("}");

    auto const cid = p->definition().matches(p->source_rom());
    if (cid.has_value()) {
        out.append(",\"cid_match\":");
        json_escape(out, *cid);
    } else {
        out.append(",\"cid_match\":null");
    }
    out.append(",\"profile\":");
    json_escape(out, std::string{st::policy::profile_name(p->policy_profile())});

    // active_rom: empty string in the field maps to "working" so JSON
    // consumers don't need to special-case the default.
    out.append(",\"active_rom\":");
    json_escape(out, p->active_rom_id().empty() ? std::string{"working"} : p->active_rom_id());

    auto const &records = p->history().records();
    auto const cursor = p->history().cursor();
    out.append(",\"history\":{\"edit_count\":");
    out.append(std::to_string(records.size()));
    out.append(",\"cursor\":");
    out.append(std::to_string(cursor));
    out.append(",\"redo_steps_available\":");
    out.append(std::to_string(records.size() > cursor ? records.size() - cursor : 0U));
    out.append(",\"can_undo\":");
    out.append(cursor > 0 ? "true" : "false");
    out.append(",\"can_redo\":");
    out.append(cursor < records.size() ? "true" : "false");
    out.append("}}\n");
    std::fputs(out.c_str(), stdout);
    return 0;
}

int cmd_project_info(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("project-info: missing project directory\n", stderr);
        std::fputs("Usage: subuwutuner-cli project-info [--json] [--audit-summary] <dir>\n",
                   stderr);
        return 2;
    }
    bool json_mode = false;
    bool audit_summary = false;
    std::filesystem::path dir;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a == "--audit-summary") {
            audit_summary = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (dir.empty()) {
            dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-info: extra argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (dir.empty()) {
        std::fputs("project-info: missing project directory\n", stderr);
        return 2;
    }
    if (json_mode) {
        return cmd_project_info_json(dir);
    }

    auto p = st::Project::open(dir);
    if (!p.has_value()) {
        std::fprintf(stderr, "project-info: %s\n", p.error().to_string().c_str());
        return 1;
    }

    std::printf("Project:    %s\n", dir.string().c_str());
    std::printf("Name:       %s\n", p->display_name().c_str());
    if (!p->notes().empty()) {
        std::printf("Notes:      %s\n", p->notes().c_str());
    }
    std::printf("Source ROM: %zu bytes, CRC32=0x%08X (recorded: 0x%08X)\n", p->source_rom().size(),
                p->source_rom().crc32(), p->source_crc32_at_create());
    if (p->source_rom().crc32() != p->source_crc32_at_create()) {
        std::printf("  ! source.bin has changed since project creation\n");
    }
    std::printf("Working ROM: %zu bytes, CRC32=0x%08X\n", p->working_rom().size(),
                p->working_rom().crc32());
    if (p->source_rom().crc32() == p->working_rom().crc32()) {
        std::printf("  (working matches source — no edits yet)\n");
    } else {
        std::printf("  (working differs from source — edits applied)\n");
    }
    std::printf("Definition: pack id %s (%zu table%s)\n", p->definition().pack().id.c_str(),
                p->definition().tables().size(), p->definition().tables().size() == 1 ? "" : "s");
    auto const cid = p->definition().matches(p->source_rom());
    if (cid.has_value()) {
        std::printf("CID match:  %s\n", cid->c_str());
    } else {
        // "(no match)" is too quiet — the user needs to know whether
        // the pack is wrong, the ROM is wrong, or both. Surface what
        // the pack *expected* + scan the ROM for any CID-shaped string
        // so we can suggest the right pack to load.
        std::string expected_msg{"(no match"};
        if (!p->definition().identifications().empty()) {
            expected_msg += " — pack expects '";
            expected_msg += p->definition().identifications()[0].cid_match;
            expected_msg += "'";
        }
        // Scan ROM bytes for any 8-char CID-shape (LF/LV/LH/AF/AE/AS
        // letter pairs followed by 6 alnum chars). One hit is the
        // probable real CID; multiple hits → just report the count
        // since picking one is guesswork.
        auto const bytes = p->source_rom().data();
        auto const is_cid_start = [](std::uint8_t a, std::uint8_t b) {
            return (a == 'L' && (b == 'F' || b == 'V' || b == 'H')) ||
                   (a == 'A' && (b == 'F' || b == 'E' || b == 'S')) ||
                   (a == 'E' && (b == 'Z' || b == 'P')) ||
                   (a == 'Z' && b == '1');
        };
        auto const is_cid_body = [](std::uint8_t c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        };
        std::vector<std::string> found;
        for (std::size_t i = 0; i + 8 <= bytes.size(); ++i) {
            if (!is_cid_start(bytes[i], bytes[i + 1]))
                continue;
            bool ok = true;
            for (std::size_t j = 2; j < 8; ++j) {
                if (!is_cid_body(bytes[i + j])) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                std::string s(reinterpret_cast<char const *>(&bytes[i]), 8);
                if (std::find(found.begin(), found.end(), s) == found.end()) {
                    found.push_back(s);
                    if (found.size() >= 3)
                        break;
                }
            }
        }
        if (!found.empty()) {
            expected_msg += "; ROM appears to contain ";
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (i > 0)
                    expected_msg += " / ";
                expected_msg += "'" + found[i] + "'";
            }
            expected_msg += " — wrong pack?";
        }
        expected_msg += ")";
        std::printf("CID match:  %s\n", expected_msg.c_str());
    }
    std::printf("Profile:    %s\n",
                std::string{st::policy::profile_name(p->policy_profile())}.c_str());

    {
        auto const &active = p->active_rom_id();
        std::printf("Active ROM: %s%s\n",
                    active.empty() ? "working" : active.c_str(),
                    active.empty() ? " (default)" : "");
    }

    // Edit-history summary. Collapse per-table edits to a flag set so
    // the user sees at-a-glance which tables have been touched + whether
    // any of them carry E/S flags that the policy gate will pick up.
    auto const &records = p->history().records();
    auto const cursor = p->history().cursor();
    if (records.empty()) {
        std::printf("Edits:      0\n");
    } else {
        std::printf("Edits:      %zu (cursor at %zu", records.size(), cursor);
        if (cursor < records.size()) {
            std::printf(", %zu redo step%s available", records.size() - cursor,
                        records.size() - cursor == 1 ? "" : "s");
        }
        std::printf(")\n");

        struct TableTouch {
            std::string id;
            std::size_t count{};
            bool emissions{};
            bool safety{};
        };
        std::vector<TableTouch> touched;
        std::size_t byte_edit_count = 0;
        for (auto const &e : records) {
            auto const *te = e.as_table();
            if (te == nullptr) {
                ++byte_edit_count;
                continue;
            }
            auto it = std::find_if(touched.begin(), touched.end(),
                                   [&](TableTouch const &t) { return t.id == te->table_id; });
            if (it == touched.end()) {
                auto const *table = p->definition().find_table(te->table_id);
                touched.push_back({te->table_id, 1, table != nullptr && table->emissions_relevant,
                                   table != nullptr && table->engine_safety_critical});
            } else {
                ++it->count;
            }
        }
        std::printf("Tables touched: %zu\n", touched.size());
        for (auto const &t : touched) {
            std::string flags;
            if (t.emissions)
                flags += "E";
            if (t.safety)
                flags += "S";
            if (flags.empty())
                flags = "-";
            std::printf("  %-30s %zu edit%s  [%s]\n", t.id.c_str(), t.count,
                        t.count == 1 ? "" : "s", flags.c_str());
        }
        if (byte_edit_count > 0) {
            std::printf("Byte edits:     %zu (e.g. DTC enable-bit toggles)\n", byte_edit_count);
        }
    }
    if (audit_summary) {
        std::printf("\nAudit summary:\n");
        auto const log_path = dir / "audit.log";
        if (!std::filesystem::exists(log_path)) {
            std::printf("  (no audit.log — nothing recorded yet)\n");
            return 0;
        }
        auto entries = st::audit::read_all(log_path);
        if (!entries.has_value()) {
            std::fprintf(stderr, "project-info: audit read: %s\n",
                         entries.error().to_string().c_str());
            return 1;
        }
        if (entries->empty()) {
            std::printf("  (audit.log present but empty)\n");
            return 0;
        }
        // Bucket by kind name, also track first / last timestamp.
        struct Bucket {
            std::string kind;
            std::size_t count{0};
            std::int64_t first_ns{0};
            std::int64_t last_ns{0};
        };
        std::vector<Bucket> buckets;
        std::size_t bad = 0;
        for (auto const &e : *entries) {
            if (!e.checksum_valid)
                ++bad;
            auto const kn = std::string{st::audit::kind_name(e.kind)};
            auto it = std::find_if(buckets.begin(), buckets.end(),
                                   [&](Bucket const &b) { return b.kind == kn; });
            if (it == buckets.end()) {
                buckets.push_back({kn, 1, e.timestamp_ns, e.timestamp_ns});
            } else {
                ++it->count;
                if (e.timestamp_ns < it->first_ns)
                    it->first_ns = e.timestamp_ns;
                if (e.timestamp_ns > it->last_ns)
                    it->last_ns = e.timestamp_ns;
            }
        }
        std::sort(buckets.begin(), buckets.end(),
                  [](Bucket const &a, Bucket const &b) {
                      if (a.count != b.count)
                          return a.count > b.count; // desc by count
                      return a.kind < b.kind;
                  });
        auto const iso = [](std::int64_t ns) -> std::string {
            std::time_t const t = ns / 1'000'000'000;
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
            return std::string{buf};
        };
        std::printf("  Entries:        %zu\n", entries->size());
        if (bad > 0) {
            std::printf("  Bad checksum:   %zu  (integrity FAILED — see `audit verify`)\n",
                        bad);
        }
        std::printf("  Kinds:\n");
        for (auto const &b : buckets) {
            if (b.count > 1 && b.first_ns != b.last_ns) {
                std::printf("    %-32s %4zu  %s … %s\n", b.kind.c_str(), b.count,
                            iso(b.first_ns).c_str(), iso(b.last_ns).c_str());
            } else {
                std::printf("    %-32s %4zu  %s\n", b.kind.c_str(), b.count,
                            iso(b.last_ns).c_str());
            }
        }
    }
    return 0;
}

// Add an [[rom]] entry to an existing project. Copies the source ROM
// file into the project dir (named `<id>.bin`) so the project stays
// self-contained, then appends the AdditionalRom + persists via
// save_metadata. Analyst Issue #10 read-slice ergonomics — without
// this the user has to hand-edit project.toml + drop the file in
// themselves.
int cmd_project_add_rom(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_dir;
    std::optional<std::string> rom_id;
    std::optional<std::filesystem::path> rom_src;
    std::optional<std::string> display_name;
    std::optional<std::string> notes;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-add-rom: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--id") {
            if (auto const *v = require("--id"); v)
                rom_id = std::string{v};
            else
                return 2;
        } else if (a == "--path") {
            if (auto const *v = require("--path"); v)
                rom_src = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--display-name") {
            if (auto const *v = require("--display-name"); v)
                display_name = std::string{v};
            else
                return 2;
        } else if (a == "--notes") {
            if (auto const *v = require("--notes"); v)
                notes = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-add-rom: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_dir.has_value()) {
            proj_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-add-rom: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_dir.has_value() || !rom_id.has_value() || !rom_src.has_value()) {
        std::fputs("project-add-rom: missing required arguments\n"
                   "Usage: subuwutuner-cli project-add-rom <dir> --id <slug> --path <rom-file>\n"
                   "                                       [--display-name \"Pretty name\"]\n"
                   "                                       [--notes \"Free-form notes\"]\n"
                   "  The ROM is copied into the project dir as <slug>.bin so the project\n"
                   "  stays self-contained.\n",
                   stderr);
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::exists(*rom_src, ec) || ec) {
        std::fprintf(stderr, "project-add-rom: source ROM does not exist: %s\n",
                     rom_src->string().c_str());
        return 1;
    }
    auto proj = st::Project::open(*proj_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-add-rom: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    // Reject duplicate id early — open() will have surfaced any
    // existing [[rom]] entries via additional_roms() already.
    for (auto const &r : proj->additional_roms()) {
        if (r.id == *rom_id) {
            std::fprintf(stderr, "project-add-rom: id '%s' already exists in this project\n",
                         rom_id->c_str());
            return 1;
        }
    }
    // Copy the source ROM into <project>/<id>.bin so the project is
    // portable. If a file with that name already exists, refuse — the
    // user probably forgot --id and we'd overwrite something useful.
    std::string const dest_filename = *rom_id + ".bin";
    auto const dest_path = *proj_dir / dest_filename;
    if (std::filesystem::exists(dest_path, ec)) {
        std::fprintf(stderr, "project-add-rom: destination file already exists: %s\n",
                     dest_path.string().c_str());
        return 1;
    }
    std::filesystem::copy_file(*rom_src, dest_path,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
        std::fprintf(stderr, "project-add-rom: copy failed: %s\n", ec.message().c_str());
        return 1;
    }
    auto rom_loaded = st::Rom::from_file(dest_path);
    if (!rom_loaded.has_value()) {
        std::fprintf(stderr, "project-add-rom: re-read after copy failed: %s\n",
                     rom_loaded.error().to_string().c_str());
        return 1;
    }
    st::Project::AdditionalRom entry;
    entry.id = *rom_id;
    entry.display_name = display_name.value_or(*rom_id);
    entry.path_rel = dest_filename;
    entry.notes = notes.value_or("");
    entry.rom = std::move(*rom_loaded);
    if (auto s = proj->add_additional_rom(std::move(entry)); !s.has_value()) {
        std::fprintf(stderr, "project-add-rom: %s\n", s.error().to_string().c_str());
        return 1;
    }
    if (auto s = proj->save_metadata(); !s.has_value()) {
        std::fprintf(stderr, "project-add-rom: save: %s\n", s.error().to_string().c_str());
        return 1;
    }
    std::printf("Added ROM '%s' (path: %s) to project %s\n", rom_id->c_str(),
                dest_filename.c_str(), proj_dir->string().c_str());
    return 0;
}

// List every ROM declared by a project — source, working, and each
// [[rom]] additional entry — in either text or JSON shape. Companion
// to project-add-rom: confirms what's in the project without firing
// up the GUI.
// Print CHANGELOG.md's [Unreleased] section to stdout. Mirrors the
// welcome panel's parser (welcome.cpp::parse_unreleased) so what the
// CLI prints matches what the GUI shows. Useful for release-prep
// scripts: `subuwutuner-cli changelog show > release-notes.md`.
int cmd_changelog_show(int argc, char *argv[]) {
    bool unreleased_only = true;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--all" || a == "--full") {
            unreleased_only = false;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "changelog show: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "changelog show: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    // Resolve CHANGELOG.md alongside the CLI binary. Same candidate
    // ladder the GUI uses (dev tree, sibling install, flat install)
    // but rooted via argv0 — readResolveDocsDir-style logic inlined
    // here since the CLI doesn't link the UI helpers.
    auto resolve = []() -> std::filesystem::path {
        namespace fs = std::filesystem;
        std::error_code ec;
        // exe_path() is available on the CLI through… actually
        // there isn't a portable wrapper, so just look in cwd then
        // bail out. The GUI uses argv0 because the binary location
        // is known at launch; for the CLI users typically invoke
        // from the repo or install dir.
        std::array<fs::path, 3> candidates{
            fs::path{"CHANGELOG.md"},
            fs::path{".."} / "CHANGELOG.md",
            fs::path{"../.."} / "CHANGELOG.md",
        };
        for (auto const &c : candidates) {
            if (fs::exists(c, ec) && !ec) {
                return c;
            }
        }
        return {};
    };
    auto const path = resolve();
    if (path.empty()) {
        std::fputs("changelog show: CHANGELOG.md not found in current dir or parents.\n"
                   "  Run from the repo root or an install dir that ships CHANGELOG.md.\n",
                   stderr);
        return 1;
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "changelog show: cannot open %s\n", path.string().c_str());
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto const body = std::move(ss).str();
    if (!unreleased_only) {
        std::fputs(body.c_str(), stdout);
        return 0;
    }
    // [Unreleased] section only — walk the body, capturing the lines
    // between "## [Unreleased]" and the next "## " heading.
    bool in_section = false;
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t line_end = body.find('\n', i);
        if (line_end == std::string::npos) {
            line_end = body.size();
        }
        std::string_view line{body.data() + i, line_end - i};
        std::size_t const next = line_end + 1;
        if (line.starts_with("## ")) {
            if (line.find("[Unreleased]") != std::string_view::npos) {
                in_section = true;
                std::fputs(std::string{line}.c_str(), stdout);
                std::fputc('\n', stdout);
                i = next;
                continue;
            } else if (in_section) {
                break; // hit the next section heading — stop.
            }
        }
        if (in_section) {
            std::fputs(std::string{line}.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        i = next;
    }
    if (!in_section) {
        std::fputs("changelog show: no [Unreleased] section found in CHANGELOG.md\n", stderr);
        return 1;
    }
    return 0;
}

// One-shot health check for a project — pass/fail per check, exit
// 1 on the first failure. Useful for CI (validate every project in
// the repo before merge), portable handoffs ("does this .stune work
// on the receiver's machine?"), and post-flash sanity ("did the
// roundtrip break anything?").
// Print min/max/mean/stddev/percentiles for a single table — same
// shape as the GUI Stats panel. Useful for scripted summaries
// ("how big is the working-vs-source delta on this map?") without
// firing up the GUI.
// Duplicate a project to a new dir. Copies source.bin, working.bin,
// project.toml, edits.toml, definitions/ (when in-tree), and any
// referenced [[rom]] additional files. Skips audit.log — a clone is
// a fresh timeline. Rewrites the cloned project.toml's display_name
// when --name is provided.
// Print a shell-completion script. bash and zsh covered; output is
// the full subcommand list so `subuwutuner-cli <tab>` works after
// sourcing. Intentionally minimal — no per-subcommand flag completion
// since the help/--help surface is already discoverable.
// Case-insensitive substring search across table id + name + category.
// One match per line in text mode; JSON envelope in JSON mode.
int cmd_table_grep(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_dir;
    std::optional<std::string> pattern;
    bool json_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "table-grep: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_dir.has_value()) {
            proj_dir = std::filesystem::path{argv[i]};
        } else if (!pattern.has_value()) {
            pattern = std::string{argv[i]};
        } else {
            std::fprintf(stderr, "table-grep: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_dir.has_value() || !pattern.has_value()) {
        std::fputs("table-grep: missing arguments\n"
                   "Usage: subuwutuner-cli table-grep <project-dir> <pattern> [--json]\n",
                   stderr);
        return 2;
    }
    auto proj = st::Project::open(*proj_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "table-grep: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    std::string needle = *pattern;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto const has_substring = [&needle](std::string_view hay) {
        std::string lower{hay};
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return lower.find(needle) != std::string::npos;
    };
    std::vector<st::Table const *> matches;
    for (auto const &t : proj->definition().tables()) {
        if (has_substring(t.id) || has_substring(t.name) || has_substring(t.category)) {
            matches.push_back(&t);
        }
    }
    if (json_mode) {
        std::string out;
        out.append("{\"schema\":\"subuwutuner.table-grep.v1\",\"pattern\":");
        json_escape(out, *pattern);
        out.append(",\"matches\":[");
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i > 0)
                out.append(",");
            out.append("{\"id\":");
            json_escape(out, matches[i]->id);
            out.append(",\"name\":");
            json_escape(out, matches[i]->name);
            out.append(",\"category\":");
            json_escape(out, matches[i]->category);
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        for (auto const *t : matches) {
            std::printf("%-40s  %-30s  [%s]\n", t->id.c_str(), t->name.c_str(),
                        t->category.empty() ? "-" : t->category.c_str());
        }
        if (matches.empty()) {
            std::fprintf(stderr, "(no matches for '%s')\n", pattern->c_str());
        }
    }
    return matches.empty() ? 1 : 0;
}

int cmd_completion(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("completion: missing shell name (bash|zsh)\n", stderr);
        return 2;
    }
    std::string_view const shell{argv[0]};
    // Keep this list manually synchronized with the dispatcher above.
    // Worth the duplication — completion has to be a fast string
    // scan, not a full subcommand-table walk.
    static char const *const kSubcommands[] = {
        "rom-info", "rom-diff", "rom-pull", "rom-identify",
        "dump-table", "dump-axis", "table-edit", "table-list",
        "project-new", "project-info", "project-edit", "project-edit-csv",
        "project-export-csv", "project-set-profile", "project-add-rom",
        "project-list-roms", "project-validate", "project-clone",
        "project-set-active-rom",
        "project-history", "project-flash", "project-diff",
        "project-autotune-maf", "project-autotune-knock-pull",
        "pack-info", "pack-lint", "primitive-list", "workflow-list",
        "hook-list", "pack-dtcs",
        "stats", "diff", "diff-load", "analyze-tune-iterations",
        "audit", "profile", "config",
        "changelog", "log", "ssm-a8-poll", "doctor", "transport-list",
        "uds-test", "feature-graph", "ai-drift", "ai-narrate",
        "list-validators", "did-datalog-preset",
        nullptr,
    };
    // Flag completion vocabulary. Reused across bash + zsh handlers.
    // Common flags that appear across many subcommands — `--json`,
    // `--profile`, `--dry-run`, `--yes`, `--pack-dir`, etc. We bias
    // toward flags that benefit from completion (paths, named modes)
    // and skip ones that take freeform strings.
    static char const *const kCommonFlags[] = {
        "--json", "--csv", "--help", "--version",
        "--dry-run", "--yes", "--verbose", "--quiet",
        "--profile", "--pack-dir", "--def", "--def-dir",
        "--rom", "--source-rom", "--working-rom",
        "--output", "--format", "--table", "--code",
        "--device", "--transport", "--sa-variant",
        "--authenticate", "--listen-only", "--filter",
        "--kind", "--desc", "--since", "--until",
        "--id", "--display-name", "--year", "--make",
        "--model", "--transmission", "--dir",
        "--all", "--audit-summary", "--validate-only", "--arch",
        nullptr,
    };

    if (shell == "bash") {
        std::printf("# subuwutuner-cli bash completion\n");
        std::printf("# Install: subuwutuner-cli completion bash > "
                    "~/.local/share/bash-completion/completions/subuwutuner-cli\n");
        std::printf("# Or:      source <(subuwutuner-cli completion bash)\n\n");
        std::printf("_subuwutuner_cli() {\n");
        std::printf("  local cur opts flags\n");
        std::printf("  COMPREPLY=()\n");
        std::printf("  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n");
        std::printf("  if [ ${COMP_CWORD} -eq 1 ]; then\n");
        std::printf("    opts=\"");
        for (auto const *s : kSubcommands) {
            if (s == nullptr)
                break;
            std::printf("%s ", s);
        }
        std::printf("\"\n");
        std::printf("    COMPREPLY=($(compgen -W \"${opts}\" -- ${cur}))\n");
        std::printf("    return 0\n");
        std::printf("  fi\n");
        // After the subcommand, flag completion kicks in when the
        // current word starts with `-` — same pattern git-completion
        // uses. Path completion still works when the user is typing a
        // path arg.
        std::printf("  if [[ \"${cur}\" == -* ]]; then\n");
        std::printf("    flags=\"");
        for (auto const *s : kCommonFlags) {
            if (s == nullptr)
                break;
            std::printf("%s ", s);
        }
        std::printf("\"\n");
        std::printf("    COMPREPLY=($(compgen -W \"${flags}\" -- ${cur}))\n");
        std::printf("    return 0\n");
        std::printf("  fi\n");
        std::printf("  COMPREPLY=($(compgen -f -- ${cur}))\n");
        std::printf("}\n");
        std::printf("complete -F _subuwutuner_cli subuwutuner-cli\n");
        return 0;
    }
    if (shell == "zsh") {
        std::printf("# subuwutuner-cli zsh completion\n");
        std::printf("# Install: subuwutuner-cli completion zsh > "
                    "${fpath[1]}/_subuwutuner-cli\n");
        std::printf("# Or:      source <(subuwutuner-cli completion zsh)\n\n");
        std::printf("#compdef subuwutuner-cli\n\n");
        std::printf("_subuwutuner_cli() {\n");
        std::printf("  local -a subcommands flags\n");
        std::printf("  subcommands=(");
        for (auto const *s : kSubcommands) {
            if (s == nullptr)
                break;
            std::printf("'%s' ", s);
        }
        std::printf(")\n");
        std::printf("  flags=(");
        for (auto const *s : kCommonFlags) {
            if (s == nullptr)
                break;
            std::printf("'%s' ", s);
        }
        std::printf(")\n");
        std::printf("  if (( CURRENT == 2 )); then\n");
        std::printf("    _describe 'subcommand' subcommands\n");
        std::printf("  elif [[ \"${words[CURRENT]}\" == -* ]]; then\n");
        std::printf("    _describe 'flag' flags\n");
        std::printf("  else\n");
        std::printf("    _files\n");
        std::printf("  fi\n");
        std::printf("}\n");
        std::printf("_subuwutuner_cli\n");
        return 0;
    }
    std::fprintf(stderr, "completion: unknown shell '%.*s' (try bash or zsh)\n",
                 static_cast<int>(shell.size()), shell.data());
    return 2;
}

int cmd_project_clone(int argc, char *argv[]) {
    std::optional<std::filesystem::path> src_dir;
    std::optional<std::filesystem::path> dst_dir;
    std::optional<std::string> new_name;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need_val = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-clone: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--name") {
            if (auto const *v = need_val("--name"); v)
                new_name = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-clone: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!src_dir.has_value()) {
            src_dir = std::filesystem::path{argv[i]};
        } else if (!dst_dir.has_value()) {
            dst_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-clone: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!src_dir.has_value() || !dst_dir.has_value()) {
        std::fputs("project-clone: missing arguments\n"
                   "Usage: subuwutuner-cli project-clone <src-dir> <dst-dir> "
                   "[--name \"New display name\"]\n",
                   stderr);
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*src_dir, ec) || ec) {
        std::fprintf(stderr, "project-clone: src is not a directory: %s\n",
                     src_dir->string().c_str());
        return 1;
    }
    if (std::filesystem::exists(*dst_dir, ec)) {
        std::fprintf(stderr, "project-clone: dst already exists: %s\n",
                     dst_dir->string().c_str());
        return 1;
    }
    // Open source to validate + get the AdditionalRom path list before
    // copying. If the source project is broken (CRC mismatch, etc), we
    // surface that early rather than producing a broken clone.
    auto src_proj = st::Project::open(*src_dir);
    if (!src_proj.has_value()) {
        std::fprintf(stderr, "project-clone: source open: %s\n",
                     src_proj.error().to_string().c_str());
        return 1;
    }
    std::filesystem::create_directories(*dst_dir, ec);
    if (ec) {
        std::fprintf(stderr, "project-clone: mkdir: %s\n", ec.message().c_str());
        return 1;
    }
    // File list to copy. We do this deterministically (not via
    // copy_options::recursive) so audit.log is excluded and we have
    // explicit control over each entry.
    std::vector<std::string> to_copy{"project.toml", "source.bin", "working.bin"};
    if (std::filesystem::exists(*src_dir / "edits.toml")) {
        to_copy.emplace_back("edits.toml");
    }
    for (auto const &r : src_proj->additional_roms()) {
        to_copy.push_back(r.path_rel.string());
    }
    // definitions/ sub-dir if the project references an in-tree pack.
    if (std::filesystem::is_directory(*src_dir / "definitions")) {
        // Whole-dir copy preserving structure.
        std::filesystem::copy(*src_dir / "definitions", *dst_dir / "definitions",
                              std::filesystem::copy_options::recursive, ec);
        if (ec) {
            std::fprintf(stderr, "project-clone: copy definitions/: %s\n",
                         ec.message().c_str());
            return 1;
        }
    }
    for (auto const &rel : to_copy) {
        std::filesystem::copy_file(*src_dir / rel, *dst_dir / rel,
                                   std::filesystem::copy_options::none, ec);
        if (ec) {
            std::fprintf(stderr, "project-clone: copy %s: %s\n", rel.c_str(),
                         ec.message().c_str());
            return 1;
        }
    }
    // Rewrite the project.toml's definition path to absolute so a
    // sibling-relative pack reference doesn't break in the new dir.
    // Best-effort: read the file, replace the line, write it back. Done
    // before reopen-for-rename so the clone can actually load.
    {
        std::ifstream in{*dst_dir / "project.toml", std::ios::binary};
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string text = std::move(ss).str();
        in.close();
        // Resolve the source pack path against the source dir.
        auto const src_pack_path = std::filesystem::weakly_canonical(
            *src_dir / std::filesystem::path{[&]() {
                // Find `path = "..."` under [project.definition].
                auto const sec = text.find("[project.definition]");
                if (sec == std::string::npos)
                    return std::string{};
                auto const eq = text.find("path", sec);
                if (eq == std::string::npos)
                    return std::string{};
                auto const q1 = text.find('"', eq);
                if (q1 == std::string::npos)
                    return std::string{};
                auto const q2 = text.find('"', q1 + 1);
                if (q2 == std::string::npos)
                    return std::string{};
                return text.substr(q1 + 1, q2 - q1 - 1);
            }()},
            ec);
        if (!ec && !src_pack_path.empty()) {
            // Replace the path string with the absolute resolved path.
            auto const sec = text.find("[project.definition]");
            if (sec != std::string::npos) {
                auto const eq = text.find("path", sec);
                if (eq != std::string::npos) {
                    auto const q1 = text.find('"', eq);
                    auto const q2 = text.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        // generic_string() so the embedded backslashes
                        // get normalized — TOML accepts forward slashes
                        // on Windows and the original style was forward.
                        text.replace(q1 + 1, q2 - q1 - 1,
                                     src_pack_path.generic_string());
                        std::ofstream out{*dst_dir / "project.toml", std::ios::binary};
                        out << text;
                    }
                }
            }
        }
    }
    // Rewrite display_name in the clone via Project::open + save_metadata.
    // Cheaper than hand-rolling a TOML edit + keeps formatting consistent
    // with what the in-process callers produce.
    if (new_name.has_value()) {
        auto cloned = st::Project::open(*dst_dir);
        if (!cloned.has_value()) {
            std::fprintf(stderr, "project-clone: reopen for rename: %s\n",
                         cloned.error().to_string().c_str());
            return 1;
        }
        cloned->set_display_name(*new_name);
        if (auto s = cloned->save_metadata(); !s.has_value()) {
            std::fprintf(stderr, "project-clone: save renamed: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
    }
    std::printf("Cloned %s → %s%s%s\n", src_dir->string().c_str(),
                dst_dir->string().c_str(),
                new_name.has_value() ? "  (renamed: " : "",
                new_name.has_value() ? new_name->c_str() : "");
    if (new_name.has_value()) {
        std::printf(")\n");
    }
    return 0;
}

int cmd_stats(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_dir;
    std::optional<std::string> table_id;
    bool json_mode = false;
    // rom_arg: explicit per-invocation override.
    //   "source" / "working" / additional id, or unset → project default.
    // --source / --working are kept as ergonomic shortcuts but route
    // through the same find_rom_by_id() path as --rom for consistency.
    std::optional<std::string> rom_arg;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need_val = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "stats: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = need_val("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--source") {
            rom_arg = std::string{"source"};
        } else if (a == "--working") {
            rom_arg = std::string{"working"};
        } else if (a == "--rom") {
            if (auto const *v = need_val("--rom"); v)
                rom_arg = std::string{v};
            else
                return 2;
        } else if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "stats: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_dir.has_value()) {
            proj_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "stats: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_dir.has_value() || !table_id.has_value()) {
        std::fputs("stats: missing required arguments\n"
                   "Usage: subuwutuner-cli stats <project-dir> --table <id> "
                   "[--rom <id> | --source | --working] [--json]\n",
                   stderr);
        return 2;
    }
    auto proj = st::Project::open(*proj_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "stats: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const *tbl = proj->definition().find_table(*table_id);
    if (tbl == nullptr) {
        std::fprintf(stderr, "stats: table '%s' not found in pack\n", table_id->c_str());
        return 1;
    }
    // Resolve which ROM to read. Precedence: explicit flag this
    // invocation → project's persisted active_rom_id → working slot.
    std::string const rom_id = rom_arg.has_value() ? *rom_arg : proj->active_rom_id();
    auto const *rom_ptr = proj->find_rom_by_id(rom_id);
    if (rom_ptr == nullptr) {
        std::fprintf(stderr, "stats: no ROM with id '%s' in this project\n", rom_id.c_str());
        return 1;
    }
    auto const &rom = *rom_ptr;
    bool const use_source = (rom_id == "source");
    std::string const rom_label = rom_id.empty() ? "working" : rom_id;
    auto td = proj->definition().read_table_values(rom, *tbl);
    if (!td.has_value()) {
        std::fprintf(stderr, "stats: %s\n", td.error().to_string().c_str());
        return 1;
    }
    std::vector<double> cells;
    for (auto const &row : td->values) {
        for (auto v : row) {
            cells.push_back(v);
        }
    }
    if (cells.empty()) {
        std::fprintf(stderr, "stats: table '%s' has zero cells\n", table_id->c_str());
        return 1;
    }
    double const mn = *std::min_element(cells.begin(), cells.end());
    double const mx = *std::max_element(cells.begin(), cells.end());
    double const sum = std::accumulate(cells.begin(), cells.end(), 0.0);
    double const mean = sum / static_cast<double>(cells.size());
    double sq = 0.0;
    for (auto v : cells) {
        double const d = v - mean;
        sq += d * d;
    }
    double const stddev = cells.size() > 1
                              ? std::sqrt(sq / static_cast<double>(cells.size() - 1))
                              : 0.0;
    auto percentile = [&cells](double p) -> double {
        std::vector<double> tmp = cells;
        std::size_t const idx = static_cast<std::size_t>(std::clamp(
            p * static_cast<double>(tmp.size()), 0.0,
            static_cast<double>(tmp.size() - 1)));
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(idx),
                         tmp.end());
        return tmp[idx];
    };
    double const p10 = percentile(0.10);
    double const p50 = percentile(0.50);
    double const p90 = percentile(0.90);
    // edited count = active-ROM cells differing from source. Only
    // meaningful when the active slot is not source itself. Recompute
    // the source table to compare (cheap for any GUI-sized table).
    // For an additional ROM the count represents how it differs from
    // source — useful as a "how heavily-tuned is this calibration?"
    // signal even though the additional ROM has no edit history of
    // its own yet.
    std::size_t edited = 0;
    bool const compute_edited = !use_source;
    if (compute_edited) {
        auto src_td = proj->definition().read_table_values(proj->source_rom(), *tbl);
        if (src_td.has_value() && src_td->values.size() == td->values.size()) {
            for (std::size_t r = 0; r < td->values.size(); ++r) {
                if (src_td->values[r].size() != td->values[r].size())
                    continue;
                for (std::size_t c = 0; c < td->values[r].size(); ++c) {
                    if (src_td->values[r][c] != td->values[r][c])
                        ++edited;
                }
            }
        }
    }
    auto const *scal = proj->definition().find_scaling(tbl->scaling);
    std::string const unit = scal != nullptr ? scal->unit : std::string{};
    if (json_mode) {
        std::string out;
        out.append("{\"schema\":\"subuwutuner.stats.v1\",\"table\":");
        json_escape(out, tbl->id);
        out.append(",\"rom\":");
        json_escape(out, rom_label);
        out.append(",\"unit\":");
        json_escape(out, unit);
        char num[64];
        auto emit = [&](char const *key, double v) {
            out.append(",\"");
            out.append(key);
            out.append("\":");
            std::snprintf(num, sizeof num, "%g", v);
            out.append(num);
        };
        emit("min", mn);
        emit("max", mx);
        emit("mean", mean);
        emit("stddev", stddev);
        emit("p10", p10);
        emit("p50", p50);
        emit("p90", p90);
        out.append(",\"cells\":");
        out.append(std::to_string(cells.size()));
        if (compute_edited) {
            out.append(",\"edited\":");
            out.append(std::to_string(edited));
        }
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        std::printf("Table:   %s\n", tbl->id.c_str());
        std::printf("ROM:     %s\n", rom_label.c_str());
        if (!unit.empty())
            std::printf("Unit:    %s\n", unit.c_str());
        std::printf("min:     %g\n", mn);
        std::printf("max:     %g\n", mx);
        std::printf("mean:    %g\n", mean);
        std::printf("stddev:  %g\n", stddev);
        std::printf("p10:     %g\n", p10);
        std::printf("p50:     %g\n", p50);
        std::printf("p90:     %g\n", p90);
        std::printf("cells:   %zu\n", cells.size());
        if (compute_edited) {
            std::printf("edited:  %zu (vs source)\n", edited);
        }
    }
    return 0;
}

int cmd_project_validate(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_dir;
    bool json_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-validate: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_dir.has_value()) {
            proj_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-validate: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_dir.has_value()) {
        std::fputs("project-validate: missing <dir>\n"
                   "Usage: subuwutuner-cli project-validate <dir> [--json]\n",
                   stderr);
        return 2;
    }
    struct Check {
        std::string name;
        bool ok{true};
        std::string detail;
    };
    std::vector<Check> checks;
    auto check = [&](std::string name, bool ok, std::string detail = "") {
        checks.push_back({std::move(name), ok, std::move(detail)});
    };
    // 1. project.toml exists.
    auto const manifest = *proj_dir / "project.toml";
    bool const manifest_ok = std::filesystem::exists(manifest);
    check("project.toml exists", manifest_ok, manifest.string());
    if (!manifest_ok) {
        // Skip remaining checks — nothing else makes sense without a manifest.
        for (auto const &c : checks) {
            std::printf("%s  %s%s%s\n", c.ok ? "[OK]   " : "[FAIL] ", c.name.c_str(),
                        c.detail.empty() ? "" : "  — ",
                        c.detail.c_str());
        }
        return 1;
    }
    // 2. Project::open succeeds (covers source.bin / working.bin / pack load).
    auto proj_r = st::Project::open(*proj_dir);
    bool const project_ok = proj_r.has_value();
    check("Project::open succeeds", project_ok,
          project_ok ? "" : proj_r.error().to_string());
    if (!project_ok) {
        for (auto const &c : checks) {
            std::printf("%s  %s%s%s\n", c.ok ? "[OK]   " : "[FAIL] ", c.name.c_str(),
                        c.detail.empty() ? "" : "  — ",
                        c.detail.c_str());
        }
        return 1;
    }
    auto const &proj = *proj_r;
    // 3. source ROM CRC32 matches the recorded value (tampering /
    //    accidental overwrite detection).
    bool const src_crc_ok =
        proj.source_rom().crc32() == proj.source_crc32_at_create();
    check("source.bin CRC32 matches recorded", src_crc_ok,
          src_crc_ok ? "" :
              "current 0x" + std::to_string(proj.source_rom().crc32()) +
              " vs recorded 0x" + std::to_string(proj.source_crc32_at_create()));
    // 4. Definition pack loaded with tables.
    bool const pack_ok = !proj.definition().tables().empty();
    check("definition pack has tables", pack_ok,
          pack_ok ? "" : "pack reports zero tables — pack path / parse problem?");
    // 5. Additional ROMs all resolved.
    auto const &warns = proj.additional_rom_warnings();
    bool const extras_ok = warns.empty();
    check("[[rom]] entries resolve", extras_ok,
          extras_ok ? "" : std::to_string(warns.size()) +
                                " warning(s) — first: " + warns.front());
    // 6. edits.toml — optional, but if present it must load (we already
    //    proved that via Project::open above, since open() restores history
    //    or fails).
    auto const edits_path = *proj_dir / "edits.toml";
    if (std::filesystem::exists(edits_path)) {
        check("edits.toml loads", true,
              std::to_string(proj.history().records().size()) + " records");
    }
    // 7. Audit log integrity — same checksum walk audit verify does.
    auto const log_path = *proj_dir / "audit.log";
    if (std::filesystem::exists(log_path)) {
        auto entries = st::audit::read_all(log_path);
        if (!entries.has_value()) {
            check("audit.log readable", false, entries.error().to_string());
        } else {
            std::size_t bad = 0;
            for (auto const &e : *entries) {
                if (!e.checksum_valid)
                    ++bad;
            }
            check("audit.log CRC32 integrity", bad == 0,
                  bad == 0 ? std::to_string(entries->size()) + " entries OK"
                           : std::to_string(bad) + " bad entr" +
                                 (bad == 1 ? "y" : "ies") +
                                 " of " + std::to_string(entries->size()));
        }
    }
    // Output.
    std::size_t failed = 0;
    for (auto const &c : checks) {
        if (!c.ok)
            ++failed;
    }
    if (json_mode) {
        std::string out;
        out.append("{\"schema\":\"subuwutuner.project-validate.v1\",\"project_dir\":");
        json_escape(out, proj_dir->string());
        out.append(",\"failed\":");
        out.append(std::to_string(failed));
        out.append(",\"checks\":[");
        for (std::size_t i = 0; i < checks.size(); ++i) {
            if (i > 0)
                out.append(",");
            out.append("{\"name\":");
            json_escape(out, checks[i].name);
            out.append(",\"ok\":");
            out.append(checks[i].ok ? "true" : "false");
            out.append(",\"detail\":");
            json_escape(out, checks[i].detail);
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        std::printf("Project: %s\n", proj_dir->string().c_str());
        for (auto const &c : checks) {
            std::printf("  %s  %s%s%s\n",
                        c.ok ? "[OK]  " : "[FAIL]",
                        c.name.c_str(),
                        c.detail.empty() ? "" : "  — ",
                        c.detail.c_str());
        }
        std::printf("\n%zu of %zu checks failed.\n", failed, checks.size());
    }
    return failed == 0 ? 0 : 1;
}

int cmd_project_list_roms(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_dir;
    bool json_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-list-roms: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_dir.has_value()) {
            proj_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-list-roms: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_dir.has_value()) {
        std::fputs("project-list-roms: missing <dir>\n"
                   "Usage: subuwutuner-cli project-list-roms <dir> [--json]\n",
                   stderr);
        return 2;
    }
    auto proj = st::Project::open(*proj_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-list-roms: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const &source = proj->source_rom();
    auto const &working = proj->working_rom();
    auto const &extras = proj->additional_roms();
    if (json_mode) {
        std::string out;
        out.reserve(512);
        out.append("{\"schema\":\"subuwutuner.project-list-roms.v1\",\"project_dir\":");
        json_escape(out, proj_dir->string());
        out.append(",\"source\":{\"path\":\"source.bin\",\"size\":");
        out.append(std::to_string(source.size()));
        out.append(",\"crc32\":");
        out.append(std::to_string(source.crc32()));
        out.append("},\"working\":{\"path\":\"working.bin\",\"size\":");
        out.append(std::to_string(working.size()));
        out.append(",\"crc32\":");
        out.append(std::to_string(working.crc32()));
        out.append("},\"additional\":[");
        for (std::size_t i = 0; i < extras.size(); ++i) {
            if (i > 0)
                out.append(",");
            auto const &r = extras[i];
            out.append("{\"id\":");
            json_escape(out, r.id);
            out.append(",\"display_name\":");
            json_escape(out, r.display_name);
            out.append(",\"path\":");
            json_escape(out, r.path_rel.generic_string());
            out.append(",\"size\":");
            out.append(std::to_string(r.rom.size()));
            out.append(",\"crc32\":");
            out.append(std::to_string(r.rom.crc32()));
            out.append(",\"notes\":");
            json_escape(out, r.notes);
            out.append("}");
        }
        out.append("],\"warnings\":[");
        auto const &warnings = proj->additional_rom_warnings();
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            if (i > 0)
                out.append(",");
            json_escape(out, warnings[i]);
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        std::printf("Project: %s\n", proj_dir->string().c_str());
        std::printf("  source   path=source.bin   size=%zu (0x%zX)  crc32=0x%08X\n",
                    source.size(), source.size(), source.crc32());
        std::printf("  working  path=working.bin  size=%zu (0x%zX)  crc32=0x%08X\n",
                    working.size(), working.size(), working.crc32());
        if (extras.empty()) {
            std::printf("  (no [[rom]] additional entries — use `project-add-rom` to "
                        "register one)\n");
        }
        for (auto const &r : extras) {
            std::printf("  %-8s path=%s  size=%zu  crc32=0x%08X\n", r.id.c_str(),
                        r.path_rel.string().c_str(), r.rom.size(), r.rom.crc32());
            if (!r.display_name.empty() && r.display_name != r.id) {
                std::printf("           name=%s\n", r.display_name.c_str());
            }
            if (!r.notes.empty()) {
                std::printf("           notes=%s\n", r.notes.c_str());
            }
        }
        auto const &warnings = proj->additional_rom_warnings();
        if (!warnings.empty()) {
            std::printf("\nWarnings (%zu):\n", warnings.size());
            for (auto const &w : warnings) {
                std::printf("  %s\n", w.c_str());
            }
        }
    }
    return 0;
}

int cmd_project_set_profile(int argc, char *argv[]) {
    if (argc < 2) {
        std::fputs("project-set-profile: missing arguments\n"
                   "Usage: subuwutuner-cli project-set-profile <dir> <profile>\n"
                   "  profile: motorsport-only | alberta-ca | eu-roadworthy "
                   "| california-us\n",
                   stderr);
        return 2;
    }
    std::filesystem::path const dir{argv[0]};
    std::string_view const profile_arg{argv[1]};

    auto const parsed = st::policy::parse_profile(profile_arg);
    if (!parsed.has_value()) {
        std::fprintf(stderr,
                     "project-set-profile: unknown profile '%.*s' "
                     "(valid: motorsport-only, alberta-ca, eu-roadworthy, "
                     "california-us)\n",
                     static_cast<int>(profile_arg.size()), profile_arg.data());
        return 2;
    }

    auto p = st::Project::open(dir);
    if (!p.has_value()) {
        std::fprintf(stderr, "project-set-profile: %s\n", p.error().to_string().c_str());
        return 1;
    }
    p->set_policy_profile(*parsed);
    if (auto s = p->save_metadata(); !s.has_value()) {
        std::fprintf(stderr, "project-set-profile: %s\n", s.error().to_string().c_str());
        return 1;
    }
    std::printf("Profile set to: %s\n", std::string{st::policy::profile_name(*parsed)}.c_str());
    return 0;
}

// project-set-active-rom <dir> <id>
//   Issue #10 multi-ROM foundation: pick which ROM read-side verbs
//   target by default. "" / "working" → working slot (default);
//   "source" → the immutable source ROM; any other id must match an
//   entry from `project-list-roms`.
int cmd_project_set_active_rom(int argc, char *argv[]) {
    if (argc < 2) {
        std::fputs("project-set-active-rom: missing arguments\n"
                   "Usage: subuwutuner-cli project-set-active-rom <dir> <id>\n"
                   "  id: 'working' (default), 'source', or an additional ROM id\n"
                   "      (see `project-list-roms`). Pass '' to reset to the\n"
                   "      working slot.\n",
                   stderr);
        return 2;
    }
    std::filesystem::path const dir{argv[0]};
    std::string_view const id_arg{argv[1]};

    auto p = st::Project::open(dir);
    if (!p.has_value()) {
        std::fprintf(stderr, "project-set-active-rom: %s\n", p.error().to_string().c_str());
        return 1;
    }
    if (auto s = p->set_active_rom_id(id_arg); !s.has_value()) {
        std::fprintf(stderr, "project-set-active-rom: %s\n", s.error().to_string().c_str());
        if (!p->additional_roms().empty()) {
            std::fputs("Available additional ROM ids:\n", stderr);
            for (auto const &r : p->additional_roms()) {
                std::fprintf(stderr, "  %s\n", r.id.c_str());
            }
        }
        return 2;
    }
    if (auto s = p->save_metadata(); !s.has_value()) {
        std::fprintf(stderr, "project-set-active-rom: %s\n", s.error().to_string().c_str());
        return 1;
    }
    auto const &active = p->active_rom_id();
    std::printf("Active ROM set to: %s\n", active.empty() ? "working" : active.c_str());
    return 0;
}

int cmd_project_history(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_path;
    std::optional<std::string> table_filter;
    std::optional<std::size_t> limit;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-history: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require("--table"); v)
                table_filter = std::string{v};
            else
                return 2;
        } else if (a == "--limit") {
            if (auto const *v = require("--limit"); v) {
                std::size_t n = 0;
                auto const res = std::from_chars(v, v + std::strlen(v), n);
                if (res.ec != std::errc{} || *res.ptr != '\0' || n == 0) {
                    std::fprintf(stderr,
                                 "project-history: --limit expects a positive integer, "
                                 "got '%s'\n",
                                 v);
                    return 2;
                }
                limit = n;
            } else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-history: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-history: extra positional: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_path.has_value()) {
        std::fputs("project-history: missing project directory\n"
                   "Usage: subuwutuner-cli project-history <dir> "
                   "[--table <id>] [--limit N]\n"
                   "  --table <id>  Filter to edits on a single table.\n"
                   "  --limit N     Show only the most recent N rows after\n"
                   "                filtering (counts the full set, prints N).\n",
                   stderr);
        return 2;
    }

    auto const proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-history: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const &history = proj->history();
    auto const &records = history.records();
    auto const cursor = history.cursor();

    std::printf("Project:  %s\n", proj_path->string().c_str());
    std::printf("Profile:  %s\n",
                std::string{st::policy::profile_name(proj->policy_profile())}.c_str());
    std::printf("History:  %zu edit(s), cursor at %zu", records.size(), cursor);
    if (cursor < records.size()) {
        std::printf("  (%zu redo step(s) available)", records.size() - cursor);
    }
    std::printf("\n");

    // Materialize the filtered index set (preserves natural order +
    // original indices so the marker semantics still make sense). The
    // --table filter only matches TableEdit records; byte edits are
    // filtered out entirely when --table is set since they don't carry
    // a table id.
    std::vector<std::size_t> filtered;
    filtered.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (table_filter.has_value()) {
            auto const *te = records[i].as_table();
            if (te == nullptr || te->table_id != *table_filter) {
                continue;
            }
        }
        filtered.push_back(i);
    }

    if (table_filter.has_value()) {
        std::printf("Filter:   --table %s (%zu match%s)\n", table_filter->c_str(), filtered.size(),
                    filtered.size() == 1 ? "" : "es");
    }
    std::printf("\n");

    if (filtered.empty()) {
        std::printf("(no edits%s)\n", table_filter.has_value() ? " matching --table" : "");
        return 0;
    }

    std::size_t start_at = 0;
    if (limit.has_value() && *limit < filtered.size()) {
        start_at = filtered.size() - *limit;
        std::printf("(showing most recent %zu of %zu)\n\n", *limit, filtered.size());
    }

    std::printf("%-4s %-30s %-22s %-25s %s\n", "#", "table_id", "rect", "description", "flags");
    std::printf("%-4s %-30s %-22s %-25s %s\n", "----", "------------------------------",
                "----------------------", "-------------------------", "-----");
    for (std::size_t k = start_at; k < filtered.size(); ++k) {
        std::size_t const i = filtered[k];
        auto const &e = records[i];

        // Marker: '>' for the entry the cursor sits AT (next to undo),
        // '.' for entries already undone past, ' ' for active entries.
        char marker = ' ';
        if (i + 1 == cursor)
            marker = '>'; // most-recent committed
        else if (i >= cursor)
            marker = '.'; // redo-pending

        char rect_buf[32]{};
        std::string id_str;
        std::string flags;
        if (auto const *te = e.as_table(); te != nullptr) {
            auto const rec = te->before.rect;
            std::snprintf(rect_buf, sizeof(rect_buf), "[%zu:%zu, %zu:%zu]", rec.r_start, rec.r_end,
                          rec.c_start, rec.c_end);
            id_str = te->table_id;

            // Pull emissions/safety flags from the table being edited so
            // the history doubles as an audit log of which edits would
            // trip the policy gate at flash time.
            auto const *table = proj->definition().find_table(te->table_id);
            if (table != nullptr) {
                if (table->emissions_relevant)
                    flags += "E";
                if (table->engine_safety_critical)
                    flags += "S";
            }
        } else if (auto const *be = e.as_byte(); be != nullptr) {
            std::snprintf(rect_buf, sizeof(rect_buf), "%zu byte%s", be->changes.size(),
                          be->changes.size() == 1 ? "" : "s");
            id_str = "(byte-edit)";
        }
        if (flags.empty())
            flags = "-";

        char idx_buf[8]{};
        std::snprintf(idx_buf, sizeof(idx_buf), "%c%zu", marker, i);

        std::printf("%-4s %-30s %-22s %-25s %s\n", idx_buf, id_str.c_str(), rect_buf,
                    e.description.empty() ? "(no description)" : e.description.c_str(),
                    flags.c_str());
    }
    std::printf("\nFlags: E=emissions-relevant, S=engine-safety-critical, "
                "-=neither.\n");
    return 0;
}

int cmd_project_autotune_maf(int argc, char *argv[]) {
    std::optional<std::filesystem::path> project_dir;
    std::optional<std::string> table_id;
    std::optional<std::filesystem::path> log_path;
    std::optional<double> gain;
    std::optional<double> max_delta_pct;
    std::optional<std::size_t> min_samples;
    bool require_open_loop = false;
    bool skip_smooth = false;
    bool strict_lint = false;
    bool apply = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-autotune-maf: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require_arg("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--log") {
            if (auto const *v = require_arg("--log"); v)
                log_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--gain") {
            auto const *v = require_arg("--gain");
            if (!v)
                return 2;
            auto const parsed = parse_fraction_or_percent(v);
            if (!parsed.has_value()) {
                std::fprintf(stderr, "project-autotune-maf: --gain must be a number (got '%s')\n",
                             v);
                return 2;
            }
            gain = *parsed;
        } else if (a == "--max-delta" || a == "--max-delta-pct") {
            auto const *v = require_arg("--max-delta");
            if (!v)
                return 2;
            auto const parsed = parse_fraction_or_percent(v);
            if (!parsed.has_value()) {
                std::fprintf(stderr,
                             "project-autotune-maf: --max-delta must be a number (got '%s')\n", v);
                return 2;
            }
            max_delta_pct = *parsed;
        } else if (a == "--min-samples-per-cell") {
            auto const *v = require_arg("--min-samples-per-cell");
            if (!v)
                return 2;
            std::size_t val = 0;
            auto const res = std::from_chars(v, v + std::strlen(v), val);
            if (res.ec != std::errc{}) {
                std::fprintf(stderr,
                             "project-autotune-maf: --min-samples-per-cell must be a "
                             "non-negative integer (got '%s')\n",
                             v);
                return 2;
            }
            min_samples = val;
        } else if (a == "--require-open-loop") {
            require_open_loop = true;
        } else if (a == "--no-smooth") {
            skip_smooth = true;
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else if (a == "--apply") {
            apply = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-autotune-maf: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!project_dir.has_value()) {
            project_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-autotune-maf: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!project_dir.has_value() || !table_id.has_value() || !log_path.has_value()) {
        std::fputs("project-autotune-maf: missing required arguments:", stderr);
        if (!project_dir.has_value())
            std::fputs(" <dir>", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-autotune-maf <dir>\n"
                   "       --table <id> --log <CSV>\n"
                   "       [--gain <num>] [--max-delta <pct>]\n"
                   "       [--min-samples-per-cell <N>] [--require-open-loop]\n"
                   "       [--no-smooth] [--strict-lint] [--apply]\n"
                   "  Reads the project's MAF scaling table (must be 1D),\n"
                   "  runs the docs/12 auto-tune kernel against the supplied\n"
                   "  CSV datalog, prints per-cell proposals, and optionally\n"
                   "  commits them as a project edit when --apply is given.\n",
                   stderr);
        return 2;
    }

    auto proj = st::Project::open(*project_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-autotune-maf: table '%s' not found in pack\n",
                     table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(proj->definition().tables().size());
        for (auto const &t : proj->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }
    if (table->dimensions != 1) {
        std::fprintf(stderr,
                     "project-autotune-maf: table '%s' has dimensions=%d; MAF scaling "
                     "must be 1D\n",
                     table->id.c_str(), table->dimensions);
        return 1;
    }
    if (!table->axis_x.has_value() || table->axis_x->empty()) {
        std::fprintf(stderr, "project-autotune-maf: table '%s' has no axis_x\n", table->id.c_str());
        return 1;
    }
    auto const *axis = proj->definition().find_axis(*table->axis_x);
    if (axis == nullptr) {
        std::fprintf(stderr, "project-autotune-maf: axis '%s' (referenced by '%s') not found\n",
                     table->axis_x->c_str(), table->id.c_str());
        return 1;
    }

    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", td.error().to_string().c_str());
        return 1;
    }
    if (td->values.empty() || td->values[0].empty()) {
        std::fprintf(stderr, "project-autotune-maf: table '%s' is empty\n", table->id.c_str());
        return 1;
    }

    // Axis values come from `td->axis_x` (already scaled). Current cell
    // values are `td->values[0][i]` for the 1D row.
    auto const &axis_values = td->axis_x;
    std::vector<double> const current(td->values[0].begin(), td->values[0].end());
    if (axis_values.size() != current.size()) {
        std::fprintf(stderr,
                     "project-autotune-maf: axis length %zu doesn't match current "
                     "row length %zu\n",
                     axis_values.size(), current.size());
        return 1;
    }

    // Read log file.
    std::ifstream log_in{*log_path, std::ios::binary};
    if (!log_in) {
        std::fprintf(stderr, "project-autotune-maf: cannot open log: %s\n",
                     log_path->string().c_str());
        return 1;
    }
    std::ostringstream log_ss;
    log_ss << log_in.rdbuf();
    auto samples = st::autotune::read_maf_samples_csv(log_ss.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::MafTuneOptions opts;
    if (gain.has_value())
        opts.gain = *gain;
    if (max_delta_pct.has_value())
        opts.max_delta_pct = *max_delta_pct;
    if (min_samples.has_value())
        opts.min_samples_per_cell = *min_samples;
    opts.require_open_loop = require_open_loop;

    auto result = st::autotune::tune_maf(axis_values, current, *samples, opts);
    if (!result.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", result.error().to_string().c_str());
        return 1;
    }
    if (!skip_smooth) {
        *result = st::autotune::smooth_proposals(*result, opts.max_delta_pct);
    }
    auto const lints = st::autotune::lint_maf_proposal(axis_values, current, *result);

    // Summary.
    std::printf("Table:               %s\n", table->id.c_str());
    std::printf("Profile:             %s\n",
                std::string{st::policy::profile_name(proj->policy_profile())}.c_str());
    std::printf("Samples (raw):       %zu\n", result->total_samples);
    std::printf("Samples after gates: %zu\n", result->samples_after_gates);
    std::printf("\n%-4s %-9s %-9s %-9s %-8s %-7s\n", "#", "axis", "current", "proposed", "samples",
                "conf");
    std::size_t modified = 0;
    std::size_t underpowered = 0;
    for (auto const &c : result->cells) {
        char marker = ' ';
        if (c.confidence == 0.0) {
            ++underpowered;
            marker = '.';
        } else if (c.proposed_value != c.current_value) {
            ++modified;
            marker = '>';
        }
        std::printf("%c%-3zu %-9.4f %-9.4f %-9.4f %-8zu %.2f\n", marker, c.cell_index,
                    axis_values[c.cell_index], c.current_value, c.proposed_value, c.samples_used,
                    c.confidence);
    }
    std::printf("\nmodified: %zu / %zu cells; underpowered (samples < %zu): %zu\n", modified,
                result->cells.size(), opts.min_samples_per_cell, underpowered);
    if (!lints.empty()) {
        std::printf("\nLint findings (%zu):\n", lints.size());
        for (auto const &v : lints) {
            std::printf("  - cells %zu..%zu: %s (%s)\n", v.cell_index, v.cell_index + 1,
                        v.message.c_str(), st::autotune::lint_kind_name(v.kind));
        }
        if (strict_lint) {
            std::fprintf(stderr,
                         "project-autotune-maf: --strict-lint set; "
                         "refusing to apply with %zu lint violation(s)\n",
                         lints.size());
            return 3;
        }
    }

    if (!apply) {
        std::printf("\n(Dry-run — pass --apply to commit the proposals as a "
                    "project edit.)\n");
        return 0;
    }

    // ---- Apply ----------------------------------------------------------
    // Snapshot the row before mutation, set each cell to its proposed
    // value, snapshot after, write back to the working ROM, record an
    // edit in History, save.

    st::edit::Rect const rect{0, 0, 0, result->cells.size() - 1};
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", before.error().to_string().c_str());
        return 1;
    }
    for (auto const &c : result->cells) {
        td->values[0][c.cell_index] = c.proposed_value;
    }
    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: %s\n", after.error().to_string().c_str());
        return 1;
    }
    if (auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
        !wb.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: writeback: %s\n",
                     wb.error().to_string().c_str());
        return 1;
    }
    {
        char descbuf[64];
        std::snprintf(descbuf, sizeof descbuf, "autotune maf (%zu cell%s)", modified,
                      modified == 1 ? "" : "s");
        proj->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                     std::move(*after), std::string{descbuf}));
    }
    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "project-autotune-maf: save: %s\n", s.error().to_string().c_str());
        return 1;
    }
    std::printf("\nApplied. New CRC32: 0x%08X\n", proj->working_rom().crc32());
    return 0;
}

int cmd_project_autotune_knock_pull(int argc, char *argv[]) {
    std::optional<std::filesystem::path> project_dir;
    std::optional<std::string> table_id;
    std::optional<std::filesystem::path> log_path;
    std::optional<double> trigger_degrees;
    std::optional<double> pull_step_degrees;
    std::optional<std::size_t> min_samples;
    bool strict_lint = false;
    bool enable_add_back = false;
    std::optional<double> add_step_degrees;
    std::optional<std::size_t> add_back_min_clean;
    std::optional<double> clean_threshold;
    bool apply = false;
    // Which of the pack's two axes is the RPM axis. Default 'y' matches
    // the common Subaru convention (axis_x = load, axis_y = engine speed).
    char rpm_axis_kind = 'y';

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-autotune-knock-pull: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        auto const parse_double = [&](char const *v, double &out, char const *label) -> bool {
            char *end = nullptr;
            double const d = std::strtod(v, &end);
            if (end == v || *end != '\0') {
                std::fprintf(stderr, "project-autotune-knock-pull: %s must be numeric (got '%s')\n",
                             label, v);
                return false;
            }
            out = d;
            return true;
        };
        if (a == "--table") {
            if (auto const *v = require_arg("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--log") {
            if (auto const *v = require_arg("--log"); v)
                log_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--trigger-degrees") {
            auto const *v = require_arg("--trigger-degrees");
            if (!v)
                return 2;
            double d = 0.0;
            if (!parse_double(v, d, "--trigger-degrees"))
                return 2;
            trigger_degrees = d;
        } else if (a == "--pull-step-degrees") {
            auto const *v = require_arg("--pull-step-degrees");
            if (!v)
                return 2;
            double d = 0.0;
            if (!parse_double(v, d, "--pull-step-degrees"))
                return 2;
            pull_step_degrees = d;
        } else if (a == "--min-samples-per-cell") {
            auto const *v = require_arg("--min-samples-per-cell");
            if (!v)
                return 2;
            std::size_t val = 0;
            auto const res = std::from_chars(v, v + std::strlen(v), val);
            if (res.ec != std::errc{}) {
                std::fprintf(stderr,
                             "project-autotune-knock-pull: --min-samples-per-cell must be a "
                             "non-negative integer (got '%s')\n",
                             v);
                return 2;
            }
            min_samples = val;
        } else if (a == "--enable-add-back") {
            enable_add_back = true;
        } else if (a == "--add-back-step-degrees") {
            auto const *v = require_arg("--add-back-step-degrees");
            if (!v)
                return 2;
            double d = 0.0;
            if (!parse_double(v, d, "--add-back-step-degrees"))
                return 2;
            add_step_degrees = d;
        } else if (a == "--add-back-min-clean-samples") {
            auto const *v = require_arg("--add-back-min-clean-samples");
            if (!v)
                return 2;
            std::size_t val = 0;
            auto const res = std::from_chars(v, v + std::strlen(v), val);
            if (res.ec != std::errc{}) {
                std::fprintf(stderr,
                             "project-autotune-knock-pull: --add-back-min-clean-samples must be a "
                             "non-negative integer (got '%s')\n",
                             v);
                return 2;
            }
            add_back_min_clean = val;
        } else if (a == "--add-back-clean-threshold-degrees") {
            auto const *v = require_arg("--add-back-clean-threshold-degrees");
            if (!v)
                return 2;
            double d = 0.0;
            if (!parse_double(v, d, "--add-back-clean-threshold-degrees"))
                return 2;
            clean_threshold = d;
        } else if (a == "--rpm-axis") {
            auto const *v = require_arg("--rpm-axis");
            if (!v)
                return 2;
            std::string_view const sv{v};
            if (sv == "x" || sv == "X")
                rpm_axis_kind = 'x';
            else if (sv == "y" || sv == "Y")
                rpm_axis_kind = 'y';
            else {
                std::fprintf(stderr,
                             "project-autotune-knock-pull: --rpm-axis must be 'x' or 'y' "
                             "(got '%s')\n",
                             v);
                return 2;
            }
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else if (a == "--apply") {
            apply = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-autotune-knock-pull: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!project_dir.has_value()) {
            project_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-autotune-knock-pull: extra positional argument: %s\n",
                         argv[i]);
            return 2;
        }
    }
    if (!project_dir.has_value() || !table_id.has_value() || !log_path.has_value()) {
        std::fputs("project-autotune-knock-pull: missing required arguments:", stderr);
        if (!project_dir.has_value())
            std::fputs(" <dir>", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-autotune-knock-pull <dir>\n"
                   "       --table <id> --log <CSV>\n"
                   "       [--rpm-axis x|y]\n"
                   "       [--trigger-degrees D] [--pull-step-degrees D]\n"
                   "       [--min-samples-per-cell N] [--strict-lint] [--apply]\n"
                   "       [--enable-add-back [--add-back-step-degrees D]\n"
                   "                          [--add-back-min-clean-samples N]\n"
                   "                          [--add-back-clean-threshold-degrees D]]\n"
                   "  Runs the docs/12 knock-pull algorithm against the project's\n"
                   "  2D timing table and a CSV datalog. --rpm-axis tells the tool\n"
                   "  which of the pack's axes is RPM (default 'y' matches the\n"
                   "  Subaru convention axis_x=load, axis_y=engine_speed).\n",
                   stderr);
        return 2;
    }

    auto proj = st::Project::open(*project_dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n", proj.error().to_string().c_str());
        return 1;
    }
    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-autotune-knock-pull: table '%s' not found in pack\n",
                     table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(proj->definition().tables().size());
        for (auto const &t : proj->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }
    if (table->dimensions != 2) {
        std::fprintf(stderr,
                     "project-autotune-knock-pull: table '%s' has dimensions=%d; "
                     "knock-pull needs a 2D timing table (load × RPM)\n",
                     table->id.c_str(), table->dimensions);
        return 1;
    }
    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n", td.error().to_string().c_str());
        return 1;
    }

    // Map the pack's (axis_x, axis_y) onto the kernel's (rpm_axis,
    // load_axis). td.values is indexed [r=axis_y_idx][c=axis_x_idx].
    // The kernel wants `current_timing` flattened as
    // `cell_index = load_row * rpm_count + rpm_col`, i.e. load on the
    // outer axis and rpm on the inner axis.
    bool const rpm_is_y = (rpm_axis_kind == 'y');
    auto const &rpm_axis = rpm_is_y ? td->axis_y : td->axis_x;
    auto const &load_axis = rpm_is_y ? td->axis_x : td->axis_y;
    std::vector<double> current_timing;
    current_timing.reserve(load_axis.size() * rpm_axis.size());
    for (std::size_t li = 0; li < load_axis.size(); ++li) {
        for (std::size_t ri = 0; ri < rpm_axis.size(); ++ri) {
            std::size_t td_r = rpm_is_y ? ri : li;
            std::size_t td_c = rpm_is_y ? li : ri;
            current_timing.push_back(td->values[td_r][td_c]);
        }
    }

    std::ifstream log_in{*log_path, std::ios::binary};
    if (!log_in) {
        std::fprintf(stderr, "project-autotune-knock-pull: cannot open log: %s\n",
                     log_path->string().c_str());
        return 1;
    }
    std::ostringstream log_ss;
    log_ss << log_in.rdbuf();
    auto samples = st::autotune::read_knock_samples_csv(log_ss.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n",
                     samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::KnockPullOptions opts;
    if (trigger_degrees.has_value())
        opts.trigger_degrees = *trigger_degrees;
    if (pull_step_degrees.has_value())
        opts.pull_step_degrees = *pull_step_degrees;
    if (min_samples.has_value())
        opts.min_samples_per_cell = *min_samples;

    auto result =
        st::autotune::tune_knock_pull(rpm_axis, load_axis, current_timing, *samples, opts);
    if (!result.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n",
                     result.error().to_string().c_str());
        return 1;
    }
    if (enable_add_back) {
        st::autotune::KnockAddBackOptions abo;
        abo.enabled = true;
        if (add_step_degrees.has_value())
            abo.add_step_degrees = *add_step_degrees;
        if (add_back_min_clean.has_value())
            abo.min_clean_samples_per_cell = *add_back_min_clean;
        if (clean_threshold.has_value())
            abo.clean_threshold_degrees = *clean_threshold;
        *result = st::autotune::apply_knock_add_back(*result, abo);
    }
    auto const lints = st::autotune::lint_knock_proposal(rpm_axis, load_axis, *result);

    std::printf("Table:               %s\n", table->id.c_str());
    std::printf("Profile:             %s\n",
                std::string{st::policy::profile_name(proj->policy_profile())}.c_str());
    std::printf("Grid:                %zu rows × %zu cols (load × RPM)\n", result->rows,
                result->cols);
    std::printf("Samples (raw):       %zu\n", result->total_samples);
    std::printf("Samples after gates: %zu\n", result->samples_after_gates);

    std::size_t pulled = 0;
    std::size_t added = 0;
    for (auto const &c : result->cells) {
        if (c.pulled)
            ++pulled;
        else if (c.proposed_value > c.current_value)
            ++added;
    }
    std::printf("Cells pulled:        %zu\n", pulled);
    if (enable_add_back) {
        std::printf("Cells added-back:    %zu\n", added);
    }

    // 2D ledger: rows are load breakpoints, columns are RPM breakpoints.
    // For each cell where the value changed, print the delta.
    std::printf("\n%-9s", "load\\rpm");
    for (auto rpm : rpm_axis)
        std::printf(" %8.0f", rpm);
    std::printf("\n");
    for (std::size_t r = 0; r < result->rows; ++r) {
        std::printf("%-9.2f", load_axis[r]);
        for (std::size_t c = 0; c < result->cols; ++c) {
            auto const &cell = result->cells[r * result->cols + c];
            double const delta = cell.proposed_value - cell.current_value;
            if (std::abs(delta) < 0.0001) {
                std::printf("       . ");
            } else {
                std::printf(" %+8.2f", delta);
            }
        }
        std::printf("\n");
    }

    if (!lints.empty()) {
        std::printf("\nLint findings (%zu):\n", lints.size());
        for (auto const &v : lints) {
            std::printf("  - cell %zu: %s (%s)\n", v.cell_index, v.message.c_str(),
                        st::autotune::lint_kind_name(v.kind));
        }
        if (strict_lint) {
            std::fprintf(stderr,
                         "project-autotune-knock-pull: --strict-lint set; refusing to "
                         "apply with %zu lint violation(s)\n",
                         lints.size());
            return 3;
        }
    }

    if (!apply) {
        std::printf("\n(Dry-run — pass --apply to commit the proposals as a "
                    "project edit.)\n");
        return 0;
    }

    // --apply: rewrite td.values from the proposal grid, snapshot+writeback+history.
    // The proposal grid is (load × rpm) in kernel-row-major; td.values
    // is the pack's native (axis_y × axis_x) layout — un-transpose
    // accordingly. Edit rect covers the whole table.
    std::size_t const grid_rows = td->values.size();
    std::size_t const grid_cols = grid_rows > 0 ? td->values[0].size() : 0;
    st::edit::Rect const rect{0, grid_rows > 0 ? grid_rows - 1 : 0, 0,
                              grid_cols > 0 ? grid_cols - 1 : 0};
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n",
                     before.error().to_string().c_str());
        return 1;
    }
    for (std::size_t li = 0; li < result->rows; ++li) {
        for (std::size_t ri = 0; ri < result->cols; ++ri) {
            std::size_t td_r = rpm_is_y ? ri : li;
            std::size_t td_c = rpm_is_y ? li : ri;
            td->values[td_r][td_c] = result->cells[li * result->cols + ri].proposed_value;
        }
    }
    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: %s\n",
                     after.error().to_string().c_str());
        return 1;
    }
    if (auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
        !wb.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: writeback: %s\n",
                     wb.error().to_string().c_str());
        return 1;
    }
    {
        char descbuf[80];
        std::snprintf(descbuf, sizeof descbuf, "autotune knock-pull (%zu pulled%s%s)", pulled,
                      added > 0 ? ", " : "",
                      added > 0 ? (std::to_string(added) + " added-back").c_str() : "");
        proj->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                     std::move(*after), std::string{descbuf}));
    }
    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "project-autotune-knock-pull: save: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    std::printf("\nApplied. New CRC32: 0x%08X\n", proj->working_rom().crc32());
    return 0;
}

// Parse "A:B" or "A" into [lo, hi]. Returns false on malformed input.
bool parse_range(std::string_view s, std::size_t &lo, std::size_t &hi) {
    auto const colon = s.find(':');
    auto parse = [](std::string_view sv, std::size_t &out) {
        std::size_t value = 0;
        auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size()) {
            return false;
        }
        out = value;
        return true;
    };
    if (colon == std::string_view::npos) {
        std::size_t v = 0;
        if (!parse(s, v))
            return false;
        lo = hi = v;
        return true;
    }
    return parse(s.substr(0, colon), lo) && parse(s.substr(colon + 1), hi);
}

int cmd_table_edit(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> table_id;
    std::optional<std::string> rows_arg;
    std::optional<std::string> cols_arg;
    std::optional<std::string> op;
    std::optional<double> value;
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "table-edit: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--def") {
            if (auto const *v = require_arg("--def"); v)
                def_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--table") {
            if (auto const *v = require_arg("--table"); v)
                table_id = std::string{v};
            else
                return 2;
        } else if (a == "--rows") {
            if (auto const *v = require_arg("--rows"); v)
                rows_arg = std::string{v};
            else
                return 2;
        } else if (a == "--cols") {
            if (auto const *v = require_arg("--cols"); v)
                cols_arg = std::string{v};
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "table-edit: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!op.has_value()) {
            op = std::string{a};
        } else if (!value.has_value() && op != "smooth" && op != "interpolate") {
            // Try parsing as a double; if it doesn't parse, treat as the ROM path.
            // libc++ on Apple Clang lacks from_chars(double) through LLVM 18,
            // so this routes through std::strtod for portability.
            std::string const buf{a};
            char *end = nullptr;
            errno = 0;
            double const d = std::strtod(buf.c_str(), &end);
            if (errno == 0 && end == buf.c_str() + buf.size() && !buf.empty()) {
                value = d;
            } else if (!rom_path.has_value()) {
                rom_path = std::filesystem::path{a};
            } else {
                std::fprintf(stderr, "table-edit: extra argument: %s\n", argv[i]);
                return 2;
            }
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "table-edit: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !table_id.has_value() || !op.has_value() ||
        !rom_path.has_value() || !output_path.has_value()) {
        std::fputs("table-edit: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!table_id.has_value())
            std::fputs(" --table", stderr);
        if (!op.has_value())
            std::fputs(" OP", stderr);
        if (!rom_path.has_value())
            std::fputs(" <FILE>", stderr);
        if (!output_path.has_value())
            std::fputs(" --output", stderr);
        std::fputs("\nUsage: subuwutuner-cli table-edit --def <pack.toml> --table <id>\n"
                   "       [--rows A:B] [--cols A:B] OP [VALUE] <FILE> --output <OUT>\n",
                   stderr);
        return 2;
    }

    bool const op_needs_value =
        *op == "set" || *op == "add" || *op == "multiply" || *op == "percent";
    if (op_needs_value && !value.has_value()) {
        std::fprintf(stderr, "table-edit: op '%s' requires a numeric value\n", op->c_str());
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("table-edit", *def_path, def.error());
    }
    auto rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const *table = def->find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "table-edit: table '%s' not found in pack\n", table_id->c_str());
        std::vector<std::string> ids;
        ids.reserve(def->tables().size());
        for (auto const &t : def->tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_id, ids);
        return 1;
    }

    auto td = def->read_table_values(*rom, *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", td.error().to_string().c_str());
        return 1;
    }

    st::edit::Rect rect = st::edit::whole_table(*td);
    if (rows_arg.has_value()) {
        if (!parse_range(*rows_arg, rect.r_start, rect.r_end)) {
            std::fprintf(stderr, "table-edit: bad --rows: %s\n", rows_arg->c_str());
            return 2;
        }
    }
    if (cols_arg.has_value()) {
        if (!parse_range(*cols_arg, rect.c_start, rect.c_end)) {
            std::fprintf(stderr, "table-edit: bad --cols: %s\n", cols_arg->c_str());
            return 2;
        }
    }

    st::Status edit_status = st::ok();
    if (*op == "set") {
        edit_status = st::edit::set_cells(*td, rect, *value);
    } else if (*op == "add") {
        edit_status = st::edit::add_cells(*td, rect, *value);
    } else if (*op == "multiply") {
        edit_status = st::edit::multiply_cells(*td, rect, *value);
    } else if (*op == "percent") {
        edit_status = st::edit::percent_scale_cells(*td, rect, *value);
    } else if (*op == "smooth") {
        edit_status = st::edit::smooth_cells(*td, rect, /*iterations=*/1);
    } else if (*op == "interpolate") {
        edit_status = st::edit::interpolate_cells(*td, rect);
    } else {
        std::fprintf(stderr, "table-edit: unknown op '%s'\n", op->c_str());
        return 2;
    }
    if (!edit_status.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", edit_status.error().to_string().c_str());
        return 1;
    }

    auto wb = def->write_table_values(*rom, *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "table-edit: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "table-edit: cannot open output: %s\n", output_path->string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(rom->data().data()),
              static_cast<std::streamsize>(rom->size()));
    if (!out) {
        std::fprintf(stderr, "table-edit: write failed\n");
        return 1;
    }

    std::printf("Table:     %s\n", table->id.c_str());
    std::printf("Op:        %s", op->c_str());
    if (op_needs_value)
        std::printf(" %g", *value);
    std::printf("\n");
    std::printf("Selection: rows %zu..%zu, cols %zu..%zu (%zu cells)\n", rect.r_start, rect.r_end,
                rect.c_start, rect.c_end, rect.rows() * rect.cols());
    std::printf("Output:    %s\n", output_path->string().c_str());
    return 0;
}

int cmd_rom_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_a;
    std::optional<std::filesystem::path> rom_b;
    bool verbose = false;
    bool json_out = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("rom-diff: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a == "--json") {
            json_out = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_a.has_value()) {
            rom_a = std::filesystem::path{argv[i]};
        } else if (!rom_b.has_value()) {
            rom_b = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "rom-diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !rom_a.has_value() || !rom_b.has_value()) {
        std::fputs("rom-diff: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!rom_a.has_value())
            std::fputs(" <A.bin>", stderr);
        if (!rom_b.has_value())
            std::fputs(" <B.bin>", stderr);
        std::fputs("\nUsage: subuwutuner-cli rom-diff --def <pack.toml> [--json] "
                   "[--verbose] <A.bin> <B.bin>\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("rom-diff", *def_path, def.error());
    }
    auto const a = st::Rom::from_file(*rom_a);
    if (!a.has_value()) {
        std::fprintf(stderr, "rom-diff: a: %s\n", a.error().to_string().c_str());
        return 1;
    }
    auto const b = st::Rom::from_file(*rom_b);
    if (!b.has_value()) {
        std::fprintf(stderr, "rom-diff: b: %s\n", b.error().to_string().c_str());
        return 1;
    }

    auto const id_a = def->matches(*a);
    auto const id_b = def->matches(*b);

    std::size_t changed_count = 0;
    std::size_t skipped = 0;
    struct Row {
        std::string id;
        std::size_t total{};
        std::size_t changed{};
        double max{};
        double mean{};
        std::string unit;
    };
    std::vector<Row> rows;
    rows.reserve(def->tables().size());

    for (auto const &table : def->tables()) {
        auto const d = def->diff_table(*a, *b, table);
        if (!d.has_value()) {
            ++skipped;
            if (verbose) {
                std::fprintf(stderr, "  skip %s: %s\n", table.id.c_str(),
                             d.error().to_string().c_str());
            }
            continue;
        }
        if (!d->changed()) {
            continue;
        }
        ++changed_count;
        auto const *scal = def->find_scaling(table.scaling);
        rows.push_back({table.id, d->total_cells, d->cells_changed, d->max_abs_delta,
                        d->mean_abs_delta, scal != nullptr ? scal->unit : std::string{}});
    }

    // Sort by max |Δ| descending — the biggest changes lead the
    // output, which is what someone diffing stock vs tuned actually
    // wants to see. Ties (e.g. 0-delta tables that happen to differ
    // in a single byte by 1) broken by table id for determinism.
    std::sort(rows.begin(), rows.end(), [](Row const &lhs, Row const &rhs) {
        if (lhs.max != rhs.max)
            return lhs.max > rhs.max;
        return lhs.id < rhs.id;
    });

    if (json_out) {
        // subuwutuner.rom-diff.v1 — summary envelope. For a full
        // per-cell DiffSet use the `diff` verb (st::diff backend) —
        // rom-diff stays at the per-table-summary level it had before
        // this JSON path was added, so dyno scripts that just want
        // "which tables moved and by how much" keep a stable shape.
        std::string out{"{\"schema\":\"subuwutuner.rom-diff.v1\",\"rom_a\":{"};
        out.append("\"path\":");
        json_escape(out, rom_a->string());
        char buf[160];
        std::snprintf(buf, sizeof buf, ",\"crc32\":%u,\"size\":%zu,\"match\":",
                      a->crc32(), a->size());
        out.append(buf);
        if (id_a.has_value()) {
            json_escape(out, *id_a);
        } else {
            out.append("null");
        }
        out.append("},\"rom_b\":{\"path\":");
        json_escape(out, rom_b->string());
        std::snprintf(buf, sizeof buf, ",\"crc32\":%u,\"size\":%zu,\"match\":",
                      b->crc32(), b->size());
        out.append(buf);
        if (id_b.has_value()) {
            json_escape(out, *id_b);
        } else {
            out.append("null");
        }
        out.append("},\"pack\":{\"id\":");
        json_escape(out, def->pack().id);
        std::snprintf(buf, sizeof buf, "},\"tables_compared\":%zu,"
                                       "\"tables_changed\":%zu,\"tables_skipped\":%zu,"
                                       "\"changes\":[",
                      def->tables().size(), changed_count, skipped);
        out.append(buf);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto const &r = rows[i];
            if (i > 0) out.push_back(',');
            out.append("{\"table\":");
            json_escape(out, r.id);
            std::snprintf(buf, sizeof buf,
                          ",\"cells_changed\":%zu,\"cells_total\":%zu,"
                          "\"max_abs_delta\":%.6f,\"mean_abs_delta\":%.6f,\"unit\":",
                          r.changed, r.total, r.max, r.mean);
            out.append(buf);
            json_escape(out, r.unit);
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    std::printf("ROM A: %s  (CRC32=0x%08X, %zu bytes)\n", rom_a->string().c_str(), a->crc32(),
                a->size());
    std::printf("ROM B: %s  (CRC32=0x%08X, %zu bytes)\n", rom_b->string().c_str(), b->crc32(),
                b->size());
    std::printf("Pack:  %s\n", def->pack().id.c_str());

    std::printf("Match A: %s\n", id_a.has_value() ? id_a->c_str() : "(no match)");
    std::printf("Match B: %s\n", id_b.has_value() ? id_b->c_str() : "(no match)");

    std::printf("\nTables compared: %zu  changed: %zu  skipped: %zu\n", def->tables().size(),
                changed_count, skipped);

    if (rows.empty()) {
        std::printf("\nNo tables differ.\n");
        return 0;
    }

    std::printf("\n%-40s %10s %12s %12s\n", "table", "cells", "max |Δ|", "mean |Δ|");
    for (auto const &r : rows) {
        char cell_buf[32];
        std::snprintf(cell_buf, sizeof(cell_buf), "%zu/%zu", r.changed, r.total);
        std::printf("%-40s %10s %12.3f %12.3f", r.id.c_str(), cell_buf, r.max, r.mean);
        if (!r.unit.empty()) {
            std::printf(" %s", r.unit.c_str());
        }
        std::printf("\n");
    }
    return 0;
}

// ---------------------------------------------------------------------
// analyze-tune-iterations — N-ROM byte-coalesced diff with pack xref
// ---------------------------------------------------------------------
//
// Given N ROM dumps (chronological) and an optional toml pack,
// produces a per-pair coalesced byte-diff report (report.md +
// summary.json) classifying each run against the nearest declared
// table. Flags `UNDECLARED` regions (runs >threshold past any
// declared table) as RE candidates — this is what surfaced the
// 4x-wrong wastegate_duty_initial declaration in lf79103p.toml
// 2026-06-09.
//
// PoC absorbed from findings/scripts/analyze_tune_iterations.py
// (analyst handoff 2026-06-09). The Python PoC validated end-to-end
// on Fehr's WRK1/WRK2/WRK3 corpus. Sample output at
// findings/tune-evolution/2026-06-09-wrk1-to-wrk3-analysis/.

namespace {

struct AtiRun {
    std::size_t start;          // inclusive
    std::size_t end;            // exclusive
    // Cached / classified against the nearest declared table; -1 sentinel
    // when no table sits at-or-before the run start.
    long long nearest_addr{-1};
    std::string nearest_name;
    std::string nearest_category;
    long long nearest_distance{-1};
    bool is_undeclared{false};
    std::string sample_a_hex;
    std::string sample_b_hex;
};

// Coalesce contiguous-differing byte ranges, joining runs separated by
// <= gap_bytes of matching bytes. Mirrors the Python PoC's
// coalesce_diff_runs exactly so the C++ output matches the analyst's
// sample report for the same inputs.
std::vector<AtiRun> ati_coalesce(std::span<std::uint8_t const> a,
                                 std::span<std::uint8_t const> b,
                                 std::size_t gap_bytes) {
    std::vector<AtiRun> out;
    bool in_run = false;
    std::size_t run_start = 0;
    std::size_t last_diff = 0;
    auto const n = a.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            if (!in_run) {
                in_run = true;
                run_start = i;
            } else if (i - last_diff > gap_bytes) {
                AtiRun ar;
        ar.start = run_start;
        ar.end = last_diff + 1;
        out.push_back(std::move(ar));
                run_start = i;
            }
            last_diff = i;
        }
    }
    if (in_run) {
        AtiRun ar;
        ar.start = run_start;
        ar.end = last_diff + 1;
        out.push_back(std::move(ar));
    }
    return out;
}

// Lower-case hex encode of a byte span. Used for the sample bytes
// embedded in JSON output so consumers can spot-check what each pair
// actually changed without re-reading the ROMs.
std::string ati_hex_encode(std::span<std::uint8_t const> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(kDigits[(b >> 4) & 0xF]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

// Classify each run against the nearest declared table. Tables are
// pre-sorted by address; binary search would scale but the inner loop
// is at most ~3,500 runs * ~300 tables == 1M ops, sub-ms even linear.
void ati_classify_runs(std::vector<AtiRun> &runs,
                       std::vector<st::Table> const &tables_sorted,
                       std::size_t undeclared_threshold) {
    for (auto &r : runs) {
        st::Table const *nearest = nullptr;
        st::Table const *next_table = nullptr;
        for (auto const &t : tables_sorted) {
            if (t.address <= r.start) {
                nearest = &t;
            } else {
                next_table = &t;
                break;
            }
        }
        if (nearest == nullptr) {
            continue;
        }
        r.nearest_addr = static_cast<long long>(nearest->address);
        r.nearest_name = nearest->name;
        r.nearest_category = nearest->category;
        r.nearest_distance = static_cast<long long>(r.start) -
                             static_cast<long long>(nearest->address);
        bool const spans_into_next = next_table != nullptr && next_table->address < r.end;
        r.is_undeclared = static_cast<std::size_t>(r.nearest_distance) > undeclared_threshold &&
                          !spans_into_next;
    }
}

// Render the summary JSON. Schema: subuwutuner.analyze-tune-iterations.v1.
// Mirrors the PoC's summary.json shape so any downstream tooling the
// analyst wrote against the PoC also works against the CLI.
struct AtiPair {
    std::string label_a;
    std::string label_b;
    std::vector<AtiRun> runs;
};

void ati_emit_json(std::string &out, std::vector<std::string> const &labels,
                   std::vector<std::filesystem::path> const &rom_paths,
                   std::size_t rom_size, std::optional<std::filesystem::path> const &pack_path,
                   std::size_t gap_bytes, std::size_t undeclared_threshold,
                   std::vector<AtiPair> const &pairs) {
    out.append("{\"schema\":\"subuwutuner.analyze-tune-iterations.v1\"");
    out.append(",\"rom_labels\":[");
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) out.append(",");
        json_escape(out, labels[i]);
    }
    out.append("],\"rom_paths\":[");
    for (std::size_t i = 0; i < rom_paths.size(); ++i) {
        if (i != 0) out.append(",");
        json_escape(out, rom_paths[i].string());
    }
    out.append("],\"rom_size\":");
    out.append(std::to_string(rom_size));
    out.append(",\"pack_path\":");
    if (pack_path.has_value()) {
        json_escape(out, pack_path->string());
    } else {
        out.append("null");
    }
    out.append(",\"gap_bytes\":");
    out.append(std::to_string(gap_bytes));
    out.append(",\"undeclared_threshold\":");
    out.append(std::to_string(undeclared_threshold));
    out.append(",\"pairs\":{");
    bool first_pair = true;
    for (auto const &p : pairs) {
        if (!first_pair) out.append(",");
        first_pair = false;
        std::string const pair_label = p.label_a + " -> " + p.label_b;
        json_escape(out, pair_label);
        out.append(":{\"runs\":[");
        std::size_t total_bytes = 0;
        std::size_t undeclared_runs = 0;
        for (std::size_t i = 0; i < p.runs.size(); ++i) {
            auto const &r = p.runs[i];
            if (i != 0) out.append(",");
            out.append("{\"start\":");
            out.append(std::to_string(r.start));
            out.append(",\"end\":");
            out.append(std::to_string(r.end));
            out.append(",\"span\":");
            out.append(std::to_string(r.end - r.start));
            out.append(",\"iter_a\":");
            json_escape(out, p.label_a);
            out.append(",\"iter_b\":");
            json_escape(out, p.label_b);
            out.append(",\"bytes_a_hex\":");
            json_escape(out, r.sample_a_hex);
            out.append(",\"bytes_b_hex\":");
            json_escape(out, r.sample_b_hex);
            out.append(",\"nearest_table_addr\":");
            if (r.nearest_addr < 0) {
                out.append("null");
            } else {
                out.append(std::to_string(r.nearest_addr));
            }
            out.append(",\"nearest_table_name\":");
            if (r.nearest_addr < 0) {
                out.append("null");
            } else {
                json_escape(out, r.nearest_name);
            }
            out.append(",\"nearest_table_category\":");
            if (r.nearest_addr < 0) {
                out.append("null");
            } else {
                json_escape(out, r.nearest_category);
            }
            out.append(",\"nearest_table_distance\":");
            if (r.nearest_distance < 0) {
                out.append("null");
            } else {
                out.append(std::to_string(r.nearest_distance));
            }
            out.append(",\"is_undeclared\":");
            out.append(r.is_undeclared ? "true" : "false");
            out.append("}");
            total_bytes += (r.end - r.start);
            if (r.is_undeclared) ++undeclared_runs;
        }
        out.append("],\"summary\":{\"total_runs\":");
        out.append(std::to_string(p.runs.size()));
        out.append(",\"total_bytes\":");
        out.append(std::to_string(total_bytes));
        out.append(",\"undeclared_runs\":");
        out.append(std::to_string(undeclared_runs));
        out.append("}}");
    }
    out.append("}}\n");
}

// Render the human-readable markdown. Same layout as the PoC's
// report.md so analyst handoffs that reference specific section
// headings (e.g. "Per-category rollup", "RE candidates") still match.
void ati_emit_markdown(std::string &out, std::vector<std::string> const &labels,
                       std::optional<std::filesystem::path> const &pack_path,
                       std::size_t gap_bytes, std::size_t undeclared_threshold,
                       std::vector<AtiPair> const &pairs) {
    out.append("# Tune-iteration diff analysis\n\n");
    out.append("- ROMs analysed: ");
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) out.append(", ");
        out.append(labels[i]);
    }
    out.append("\n- Pack (for table xref): ");
    if (pack_path.has_value()) {
        out.append("`");
        out.append(pack_path->string());
        out.append("`");
    } else {
        out.append("(none)");
    }
    out.append("\n- Coalesce gap: ");
    out.append(std::to_string(gap_bytes));
    out.append(" B\n- Undeclared threshold: ");
    out.append(std::to_string(undeclared_threshold));
    out.append(" B\n\n");

    for (auto const &p : pairs) {
        out.append("## ");
        out.append(p.label_a);
        out.append(" \xE2\x86\x92 ");
        out.append(p.label_b);
        out.append("\n\n");
        std::size_t total = 0;
        for (auto const &r : p.runs) total += (r.end - r.start);
        out.append("- ");
        out.append(std::to_string(p.runs.size()));
        out.append(" coalesced diff runs, ");
        out.append(std::to_string(total));
        out.append(" bytes total\n\n");

        // Per-category rollup
        struct Roll {
            std::size_t runs{0};
            std::size_t bytes{0};
            std::set<std::string> tables;
        };
        std::map<std::string, Roll> cat_roll;
        for (auto const &r : p.runs) {
            std::string const cat = r.nearest_addr < 0
                                        ? std::string{"(unclassified)"}
                                        : r.nearest_category;
            auto &s = cat_roll[cat];
            ++s.runs;
            s.bytes += (r.end - r.start);
            if (!r.nearest_name.empty()) {
                s.tables.insert(r.nearest_name);
            }
        }
        out.append("### Per-category rollup\n\n");
        out.append("| Category | Runs | Bytes | Tables touched |\n");
        out.append("|---|---|---|---|\n");
        // Sort by descending bytes
        std::vector<std::pair<std::string, Roll>> sorted_roll(cat_roll.begin(), cat_roll.end());
        std::sort(sorted_roll.begin(), sorted_roll.end(),
                  [](auto const &a, auto const &b) { return a.second.bytes > b.second.bytes; });
        for (auto const &[cat, s] : sorted_roll) {
            out.append("| ");
            out.append(cat);
            out.append(" | ");
            out.append(std::to_string(s.runs));
            out.append(" | ");
            out.append(std::to_string(s.bytes));
            out.append(" | ");
            out.append(std::to_string(s.tables.size()));
            out.append(" |\n");
        }
        out.append("\n### Run-by-run classification\n\n");
        out.append("| canon range | span | nearest declared table | category | undecl |\n");
        out.append("|---|---|---|---|---|\n");
        for (auto const &r : p.runs) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "0x%05zx..0x%05zx",
                          r.start, r.end == 0 ? r.end : r.end - 1);
            out.append("| ");
            out.append(buf);
            out.append(" | ");
            out.append(std::to_string(r.end - r.start));
            out.append(" | ");
            if (r.nearest_addr < 0) {
                out.append("(none)");
            } else {
                out.append(r.nearest_name);
                char addr_buf[64];
                std::snprintf(addr_buf, sizeof addr_buf, " @ 0x%05llx(+%lldB)",
                              r.nearest_addr, r.nearest_distance);
                out.append(addr_buf);
            }
            out.append(" | ");
            out.append(r.nearest_addr < 0 ? std::string{"?"} : r.nearest_category);
            out.append(" | ");
            out.append(r.is_undeclared ? "yes" : "");
            out.append(" |\n");
        }
        out.append("\n");

        // RE-candidate summary: group undeclared runs by anchor.
        std::vector<AtiRun const *> undecl;
        for (auto const &r : p.runs) {
            if (r.is_undeclared) undecl.push_back(&r);
        }
        if (!undecl.empty()) {
            out.append("### RE candidates \xE2\x80\x94 undeclared edited regions (");
            out.append(std::to_string(undecl.size()));
            out.append(")\n\n");
            std::map<std::pair<long long, std::string>, std::vector<AtiRun const *>> by_anchor;
            for (auto const *r : undecl) {
                by_anchor[{r->nearest_addr, r->nearest_name}].push_back(r);
            }
            for (auto const &[anchor, rl] : by_anchor) {
                if (rl.empty()) continue;
                auto const min_off = rl.front()->nearest_distance;
                auto const max_off = rl.back()->nearest_distance +
                                     static_cast<long long>(rl.back()->end - rl.back()->start);
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "- `%s` @ 0x%05llx: %zu run(s), spanning "
                              "+%lld..+%lld B past the declared table address \xE2\x80\x94 "
                              "declared boundary likely too small.\n",
                              anchor.second.c_str(), anchor.first, rl.size(), min_off, max_off);
                out.append(buf);
            }
            out.append("\n");
        }
    }
}

} // namespace

int cmd_analyze_tune_iterations(int argc, char *argv[]) {
    std::vector<std::filesystem::path> rom_paths;
    std::vector<std::string> labels;
    std::optional<std::filesystem::path> pack_path;
    std::optional<std::filesystem::path> out_dir;
    std::size_t gap_bytes = 8;
    std::size_t undeclared_threshold = 256;
    std::size_t sample_bytes = 32;
    std::string out_mode{"both"};  // json | md | both

    // Simple two-state parser: --roms / --labels each consume the
    // following non-flag tokens until the next --flag.
    enum class State { None, Roms, Labels } collecting = State::None;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            collecting = State::None;
        }
        if (collecting == State::Roms) {
            rom_paths.emplace_back(a);
            continue;
        }
        if (collecting == State::Labels) {
            labels.emplace_back(a);
            continue;
        }
        if (a == "--roms") {
            collecting = State::Roms;
        } else if (a == "--labels") {
            collecting = State::Labels;
        } else if (a == "--pack") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --pack requires a path\n", stderr);
                return 2;
            }
            pack_path = std::filesystem::path{argv[++i]};
        } else if (a == "--out-dir") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --out-dir requires a path\n", stderr);
                return 2;
            }
            out_dir = std::filesystem::path{argv[++i]};
        } else if (a == "--gap-bytes") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --gap-bytes requires a value\n", stderr);
                return 2;
            }
            gap_bytes = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--undeclared-threshold") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --undeclared-threshold requires a value\n",
                           stderr);
                return 2;
            }
            undeclared_threshold = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--sample-bytes") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --sample-bytes requires a value\n", stderr);
                return 2;
            }
            sample_bytes = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--out") {
            if (i + 1 >= argc) {
                std::fputs("analyze-tune-iterations: --out requires json|md|both\n", stderr);
                return 2;
            }
            out_mode = argv[++i];
            if (out_mode != "json" && out_mode != "md" && out_mode != "both") {
                std::fprintf(stderr,
                             "analyze-tune-iterations: --out must be json, md, or both (got %s)\n",
                             out_mode.c_str());
                return 2;
            }
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "analyze-tune-iterations: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (rom_paths.size() < 2) {
        std::fputs("analyze-tune-iterations: at least 2 --roms required\n", stderr);
        std::fputs("Usage: subuwutuner-cli analyze-tune-iterations --roms <A.bin> <B.bin> [...]\n"
                   "       [--labels L1 L2 ...] [--pack <pack.toml>] --out-dir <dir>\n"
                   "       [--gap-bytes 8] [--undeclared-threshold 256] [--sample-bytes 32]\n"
                   "       [--out json|md|both]\n",
                   stderr);
        return 2;
    }
    if (!out_dir.has_value()) {
        std::fputs("analyze-tune-iterations: --out-dir is required\n", stderr);
        return 2;
    }
    if (!labels.empty() && labels.size() != rom_paths.size()) {
        std::fprintf(stderr,
                     "analyze-tune-iterations: --labels count (%zu) must match --roms count (%zu)\n",
                     labels.size(), rom_paths.size());
        return 2;
    }
    if (labels.empty()) {
        for (auto const &p : rom_paths) labels.push_back(p.stem().string());
    }

    // Load ROMs, verify size match.
    std::vector<std::vector<std::uint8_t>> roms;
    roms.reserve(rom_paths.size());
    for (auto const &p : rom_paths) {
        auto r = st::Rom::from_file(p);
        if (!r.has_value()) {
            std::fprintf(stderr, "analyze-tune-iterations: %s: %s\n",
                         p.string().c_str(), r.error().to_string().c_str());
            return 1;
        }
        auto const span = r->data();
        roms.emplace_back(span.begin(), span.end());
    }
    std::size_t const rom_size = roms[0].size();
    for (std::size_t i = 1; i < roms.size(); ++i) {
        if (roms[i].size() != rom_size) {
            std::fprintf(stderr,
                         "analyze-tune-iterations: ROM size mismatch: %s is %zu B, %s is %zu B. "
                         "Align ROMs to identical size before analysing.\n",
                         rom_paths[0].string().c_str(), rom_size,
                         rom_paths[i].string().c_str(), roms[i].size());
            return 1;
        }
    }

    // Load pack tables if provided. Sort by address for the classify loop.
    std::vector<st::Table> tables_sorted;
    if (pack_path.has_value()) {
        auto const def = st::Definition::from_file(resolve_def_path(*pack_path));
        if (!def.has_value()) {
            return print_def_load_error("analyze-tune-iterations", *pack_path, def.error());
        }
        tables_sorted = def->tables();
        std::sort(tables_sorted.begin(), tables_sorted.end(),
                  [](st::Table const &a, st::Table const &b) { return a.address < b.address; });
    }

    std::fprintf(stderr,
                 "analyze-tune-iterations: loaded %zu ROMs (0x%zx B each); %zu declared tables\n",
                 roms.size(), rom_size, tables_sorted.size());

    // Adjacent pairs + cumulative first/last if >2 ROMs.
    std::vector<std::pair<std::size_t, std::size_t>> pair_idx;
    for (std::size_t i = 0; i + 1 < roms.size(); ++i) pair_idx.emplace_back(i, i + 1);
    if (roms.size() > 2) pair_idx.emplace_back(0, roms.size() - 1);

    std::vector<AtiPair> pairs;
    pairs.reserve(pair_idx.size());
    for (auto const &[i, j] : pair_idx) {
        AtiPair p;
        p.label_a = labels[i];
        p.label_b = labels[j];
        p.runs = ati_coalesce(roms[i], roms[j], gap_bytes);
        // Embed sample bytes (capped) for spot-checking.
        for (auto &r : p.runs) {
            std::size_t const sample_len =
                std::min(sample_bytes, r.end - r.start);
            r.sample_a_hex = ati_hex_encode(std::span{roms[i].data() + r.start, sample_len});
            r.sample_b_hex = ati_hex_encode(std::span{roms[j].data() + r.start, sample_len});
        }
        ati_classify_runs(p.runs, tables_sorted, undeclared_threshold);
        std::size_t total = 0;
        for (auto const &r : p.runs) total += (r.end - r.start);
        std::fprintf(stderr, "  %s -> %s: %zu runs, %zu B total\n",
                     p.label_a.c_str(), p.label_b.c_str(), p.runs.size(), total);
        pairs.push_back(std::move(p));
    }

    // Create output dir + write files.
    std::error_code ec;
    std::filesystem::create_directories(*out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "analyze-tune-iterations: couldn't create %s: %s\n",
                     out_dir->string().c_str(), ec.message().c_str());
        return 1;
    }

    if (out_mode == "json" || out_mode == "both") {
        std::string json_buf;
        ati_emit_json(json_buf, labels, rom_paths, rom_size, pack_path,
                      gap_bytes, undeclared_threshold, pairs);
        auto const json_path = *out_dir / "summary.json";
        std::ofstream js{json_path, std::ios::binary};
        if (!js) {
            std::fprintf(stderr, "analyze-tune-iterations: open %s failed\n",
                         json_path.string().c_str());
            return 1;
        }
        js.write(json_buf.data(), static_cast<std::streamsize>(json_buf.size()));
        std::fprintf(stderr, "wrote %s\n", json_path.string().c_str());
    }
    if (out_mode == "md" || out_mode == "both") {
        std::string md_buf;
        ati_emit_markdown(md_buf, labels, pack_path, gap_bytes, undeclared_threshold, pairs);
        auto const md_path = *out_dir / "report.md";
        std::ofstream ms{md_path, std::ios::binary};
        if (!ms) {
            std::fprintf(stderr, "analyze-tune-iterations: open %s failed\n",
                         md_path.string().c_str());
            return 1;
        }
        ms.write(md_buf.data(), static_cast<std::streamsize>(md_buf.size()));
        std::fprintf(stderr, "wrote %s\n", md_path.string().c_str());
    }

    return 0;
}

// `diff` — structured ROM compare via st::diff. Sister to the
// existing rom-diff (which is text-summary-only). Supports
// --format text|csv|json and exits non-zero when ROMs differ so a
// CI script can gate on the result without parsing prose. Per the
// analyst Issue #4 / I-03+I-04 + docs/33 triage, this is the
// CLI-side of the Compare workflow; the GUI Compare panel is the
// next slice.
int cmd_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_a;
    std::optional<std::filesystem::path> rom_b;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::filesystem::path> save_session_path;
    std::string format = "text";
    bool include_identical = false;
    bool include_unchanged_csv = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--pack" || a == "--def") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "diff: %.*s requires a path\n",
                             static_cast<int>(a.size()), a.data());
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--format") {
            if (i + 1 >= argc) {
                std::fputs("diff: --format requires text|csv|json\n", stderr);
                return 2;
            }
            format = std::string{argv[++i]};
            if (format != "text" && format != "csv" && format != "json") {
                std::fprintf(stderr, "diff: unknown --format '%s' "
                                     "(expected text|csv|json)\n",
                             format.c_str());
                return 2;
            }
        } else if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("diff: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a == "--save") {
            if (i + 1 >= argc) {
                std::fputs("diff: --save requires a .stcompare path\n", stderr);
                return 2;
            }
            save_session_path = std::filesystem::path{argv[++i]};
        } else if (a == "--include-identical") {
            include_identical = true;
        } else if (a == "--include-unchanged") {
            include_unchanged_csv = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_a.has_value()) {
            rom_a = std::filesystem::path{argv[i]};
        } else if (!rom_b.has_value()) {
            rom_b = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !rom_a.has_value() || !rom_b.has_value()) {
        std::fputs("diff: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --pack", stderr);
        if (!rom_a.has_value())
            std::fputs(" <A.bin>", stderr);
        if (!rom_b.has_value())
            std::fputs(" <B.bin>", stderr);
        std::fputs("\nUsage: subuwutuner-cli diff --pack <pack.toml> "
                   "[--format text|csv|json] [--output FILE] "
                   "[--include-identical] [--include-unchanged] "
                   "<A.bin> <B.bin>\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("diff", *def_path, def.error());
    }
    auto const a = st::Rom::from_file(*rom_a);
    if (!a.has_value()) {
        std::fprintf(stderr, "diff: a: %s\n", a.error().to_string().c_str());
        return 1;
    }
    auto const b = st::Rom::from_file(*rom_b);
    if (!b.has_value()) {
        std::fprintf(stderr, "diff: b: %s\n", b.error().to_string().c_str());
        return 1;
    }

    st::diff::Options opts;
    opts.include_identical = include_identical;
    auto result = st::diff::compare(*a, *b, *def, opts);
    if (!result.has_value()) {
        std::fprintf(stderr, "diff: %s\n", result.error().to_string().c_str());
        return 1;
    }

    std::string rendered;
    if (format == "text") {
        rendered = st::diff::render_text(*result);
    } else if (format == "csv") {
        rendered = st::diff::render_csv(*result, include_unchanged_csv);
    } else { // json — already validated above
        rendered = st::diff::render_json(*result);
        rendered.push_back('\n');
    }

    if (output_path.has_value()) {
        std::ofstream out{*output_path, std::ios::binary};
        if (!out) {
            std::fprintf(stderr, "diff: cannot open --output: %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
    } else {
        std::fputs(rendered.c_str(), stdout);
    }

    // Optional --save: write a .stcompare session file alongside.
    // Records the inputs + options (the DiffSet is regenerated on
    // load) plus the always-empty annotations array. The GUI Compare
    // panel grows annotations over time; the CLI just round-trips
    // them.
    if (save_session_path.has_value()) {
        st::diff::CompareSession session;
        session.rom_a_path = std::filesystem::absolute(*rom_a).string();
        session.rom_a_crc32 = a->crc32();
        session.rom_a_size = a->size();
        session.rom_b_path = std::filesystem::absolute(*rom_b).string();
        session.rom_b_crc32 = b->crc32();
        session.rom_b_size = b->size();
        session.pack_id = def->pack().id;
        session.options.cell_epsilon = static_cast<double>(opts.cell_epsilon);
        session.options.include_identical = opts.include_identical;
        // ISO-8601 UTC. snprintf with time_t avoids the std::format
        // chrono path which isn't universally available on MinGW.
        std::time_t const now = std::time(nullptr);
        std::tm tm_utc{};
#if defined(_WIN32)
        gmtime_s(&tm_utc, &now);
#else
        gmtime_r(&now, &tm_utc);
#endif
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        session.created_at = ts;
        if (auto s = st::diff::save_compare_session(session, *save_session_path);
            !s.has_value()) {
            std::fprintf(stderr, "diff: --save failed: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
    }

    // Exit 0 if identical, 1 if any table changed (caller can gate
    // CI / scripts on the exit code without parsing the output).
    return result->identical() ? 0 : 1;
}

// `diff-load` — open a saved .stcompare session, verify the input
// ROMs still match their recorded CRC32, recompute the DiffSet, and
// render in the requested format. Pack is resolved via --pack
// (override the session's recorded pack_id if you've moved the pack
// since saving).
int cmd_diff_load(int argc, char *argv[]) {
    std::optional<std::filesystem::path> session_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> output_path;
    std::string format = "text";

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--pack" || a == "--def") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "diff-load: %.*s requires a path\n",
                             static_cast<int>(a.size()), a.data());
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--format") {
            if (i + 1 >= argc) {
                std::fputs("diff-load: --format requires text|csv|json\n", stderr);
                return 2;
            }
            format = std::string{argv[++i]};
            if (format != "text" && format != "csv" && format != "json") {
                std::fprintf(stderr, "diff-load: unknown --format '%s'\n", format.c_str());
                return 2;
            }
        } else if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("diff-load: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "diff-load: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!session_path.has_value()) {
            session_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "diff-load: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!session_path.has_value() || !def_path.has_value()) {
        std::fputs("diff-load: missing required arguments:", stderr);
        if (!session_path.has_value())
            std::fputs(" <SESSION.stcompare>", stderr);
        if (!def_path.has_value())
            std::fputs(" --pack", stderr);
        std::fputs("\nUsage: subuwutuner-cli diff-load --pack <pack.toml> "
                   "[--format text|csv|json] [--output FILE] "
                   "<SESSION.stcompare>\n",
                   stderr);
        return 2;
    }

    auto session = st::diff::load_compare_session(*session_path);
    if (!session.has_value()) {
        std::fprintf(stderr, "diff-load: %s\n", session.error().to_string().c_str());
        return 1;
    }
    auto const base_dir = std::filesystem::absolute(*session_path).parent_path();
    if (auto v = st::diff::verify_compare_inputs(*session, base_dir); !v.has_value()) {
        std::fprintf(stderr, "diff-load: %s\n", v.error().to_string().c_str());
        return 1;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("diff-load", *def_path, def.error());
    }
    if (def->pack().id != session->pack_id) {
        std::fprintf(stderr, "diff-load: warning: pack id mismatch — session was saved "
                             "against '%s', --pack provides '%s'. Recomputing anyway.\n",
                     session->pack_id.c_str(), def->pack().id.c_str());
    }

    auto const resolve_input = [&](std::string const &p) -> std::filesystem::path {
        std::filesystem::path pp{p};
        if (pp.is_absolute())
            return pp;
        return base_dir / pp;
    };
    auto rom_a = st::Rom::from_file(resolve_input(session->rom_a_path));
    auto rom_b = st::Rom::from_file(resolve_input(session->rom_b_path));
    if (!rom_a.has_value() || !rom_b.has_value()) {
        std::fputs("diff-load: ROM read failed (verify passed but file vanished mid-load?)\n",
                   stderr);
        return 1;
    }
    auto result = st::diff::compare(*rom_a, *rom_b, *def, session->options);
    if (!result.has_value()) {
        std::fprintf(stderr, "diff-load: %s\n", result.error().to_string().c_str());
        return 1;
    }

    std::string rendered;
    if (format == "text") {
        rendered = st::diff::render_text(*result);
        if (!session->annotations.empty()) {
            rendered.append("\nAnnotations:\n");
            for (auto const &an : session->annotations) {
                rendered.append("  ");
                rendered.append(an.table_id);
                rendered.append("[");
                rendered.append(std::to_string(an.row));
                rendered.append(",");
                rendered.append(std::to_string(an.col));
                rendered.append("]: ");
                rendered.append(an.text);
                rendered.append("\n");
            }
        }
    } else if (format == "csv") {
        rendered = st::diff::render_csv(*result);
    } else {
        rendered = st::diff::render_json(*result);
        rendered.push_back('\n');
    }

    if (output_path.has_value()) {
        std::ofstream out{*output_path, std::ios::binary};
        if (!out) {
            std::fprintf(stderr, "diff-load: cannot open --output: %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
    } else {
        std::fputs(rendered.c_str(), stdout);
    }
    return result->identical() ? 0 : 1;
}

int cmd_project_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> proj_a;
    std::optional<std::filesystem::path> proj_b;
    std::optional<std::string> profile_arg;
    std::optional<std::string> table_filter;
    bool verbose = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-diff: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--profile") {
            if (auto const *v = require_arg("--profile"); v)
                profile_arg = std::string{v};
            else
                return 2;
        } else if (a == "--table") {
            if (auto const *v = require_arg("--table"); v)
                table_filter = std::string{v};
            else
                return 2;
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_a.has_value()) {
            proj_a = std::filesystem::path{argv[i]};
        } else if (!proj_b.has_value()) {
            proj_b = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!proj_a.has_value() || !proj_b.has_value()) {
        std::fputs("project-diff: missing required arguments:", stderr);
        if (!proj_a.has_value())
            std::fputs(" <A.stune>", stderr);
        if (!proj_b.has_value())
            std::fputs(" <B.stune>", stderr);
        std::fputs("\nUsage: subuwutuner-cli project-diff <A.stune> <B.stune> "
                   "[--table <id>] [--profile P] [--verbose]\n"
                   "  Compares two projects' working ROMs table-by-table.\n"
                   "  Both must reference the same pack id. With --profile,\n"
                   "  also runs the policy gate over the A->B byte changes.\n"
                   "  --table restricts the comparison + policy gate to a\n"
                   "  single table.\n",
                   stderr);
        return 2;
    }
    auto a = st::Project::open(*proj_a);
    if (!a.has_value()) {
        std::fprintf(stderr, "project-diff: A: %s\n", a.error().to_string().c_str());
        return 1;
    }
    auto b = st::Project::open(*proj_b);
    if (!b.has_value()) {
        std::fprintf(stderr, "project-diff: B: %s\n", b.error().to_string().c_str());
        return 1;
    }
    if (a->definition().pack().id != b->definition().pack().id) {
        std::fprintf(stderr, "project-diff: A and B reference different packs ('%s' vs '%s')\n",
                     a->definition().pack().id.c_str(), b->definition().pack().id.c_str());
        return 1;
    }
    if (a->working_rom().size() != b->working_rom().size()) {
        std::fprintf(stderr, "project-diff: ROM sizes differ (%zu vs %zu)\n",
                     a->working_rom().size(), b->working_rom().size());
        return 1;
    }

    if (table_filter.has_value() && a->definition().find_table(*table_filter) == nullptr) {
        std::fprintf(stderr, "project-diff: table '%s' not found in pack '%s'\n",
                     table_filter->c_str(), a->definition().pack().id.c_str());
        std::vector<std::string> ids;
        ids.reserve(a->definition().tables().size());
        for (auto const &t : a->definition().tables())
            ids.push_back(t.id);
        print_id_suggestions("table", *table_filter, ids);
        return 1;
    }

    std::printf("A:    %s  (working CRC32=0x%08X)\n", proj_a->string().c_str(),
                a->working_rom().crc32());
    std::printf("B:    %s  (working CRC32=0x%08X)\n", proj_b->string().c_str(),
                b->working_rom().crc32());
    std::printf("Pack: %s\n", a->definition().pack().id.c_str());
    if (table_filter.has_value()) {
        std::printf("Filter: --table %s\n", table_filter->c_str());
    }

    struct Row {
        std::string id;
        std::size_t total{};
        std::size_t changed{};
        double max{};
        double mean{};
        std::string unit;
        bool emissions{};
        bool safety{};
    };
    std::vector<Row> rows;
    std::size_t changed_count = 0;
    std::size_t skipped = 0;
    std::size_t considered = 0;
    for (auto const &table : a->definition().tables()) {
        if (table_filter.has_value() && table.id != *table_filter)
            continue;
        ++considered;
        auto const d = a->definition().diff_table(a->working_rom(), b->working_rom(), table);
        if (!d.has_value()) {
            ++skipped;
            if (verbose) {
                std::fprintf(stderr, "  skip %s: %s\n", table.id.c_str(),
                             d.error().to_string().c_str());
            }
            continue;
        }
        if (!d->changed())
            continue;
        ++changed_count;
        auto const *scal = a->definition().find_scaling(table.scaling);
        rows.push_back({table.id, d->total_cells, d->cells_changed, d->max_abs_delta,
                        d->mean_abs_delta, scal != nullptr ? scal->unit : std::string{},
                        table.emissions_relevant, table.engine_safety_critical});
    }

    std::printf("\nTables compared: %zu  changed: %zu  skipped: %zu\n", considered, changed_count,
                skipped);
    if (rows.empty()) {
        std::printf("\nNo tables differ.\n");
    } else {
        std::printf("\n%-40s %10s %12s %12s %s\n", "table", "cells", "max |Δ|", "mean |Δ|",
                    "flags");
        for (auto const &r : rows) {
            char cell_buf[32];
            std::snprintf(cell_buf, sizeof(cell_buf), "%zu/%zu", r.changed, r.total);
            std::string flags;
            if (r.emissions)
                flags += "E";
            if (r.safety)
                flags += "S";
            if (flags.empty())
                flags = "-";
            std::printf("%-40s %10s %12.3f %12.3f %s", r.id.c_str(), cell_buf, r.max, r.mean,
                        flags.c_str());
            if (!r.unit.empty())
                std::printf(" %s", r.unit.c_str());
            std::printf("\n");
        }
    }

    // Optional policy gate: build a synthetic FlashPlan from A->B byte
    // differences and ask evaluate_plan_policy what would happen.
    if (profile_arg.has_value()) {
        auto const profile = st::policy::parse_profile(*profile_arg);
        if (!profile.has_value()) {
            std::fprintf(stderr,
                         "project-diff: unknown profile '%s'. Known: motorsport-only, "
                         "alberta-ca, eu-roadworthy, california-us\n",
                         profile_arg->c_str());
            return 2;
        }
        auto const sectors =
            st::flash::Flasher::compute_delta(a->working_rom().data(), b->working_rom().data(),
                                              /*sector_size=*/0x1000, /*base_address=*/0);
        st::flash::FlashPlan plan;
        plan.writes.reserve(sectors.size());
        for (auto const &s : sectors) {
            st::flash::SectorWrite sw;
            sw.sector = s;
            std::size_t const off = static_cast<std::size_t>(s.address);
            sw.data.assign(b->working_rom().data().begin() + static_cast<std::ptrdiff_t>(off),
                           b->working_rom().data().begin() +
                               static_cast<std::ptrdiff_t>(off + s.length));
            plan.writes.push_back(std::move(sw));
        }
        auto const d = st::flash::evaluate_plan_policy(plan, a->definition(),
                                                       a->working_rom().data(), *profile);
        std::printf("\nPolicy preview (profile=%s, A->B):\n", profile_arg->c_str());
        if (table_filter.has_value()) {
            std::printf("  (note: --table filters the diff display only; the\n"
                        "   policy gate still covers ALL bytes that would flash)\n");
        }
        std::printf("  engine_safety_tables = %zu", d.engine_safety_tables.size());
        for (auto const &id : d.engine_safety_tables)
            std::printf(" %s", id.c_str());
        std::printf("\n  emissions_tables     = %zu", d.emissions_tables.size());
        for (auto const &id : d.emissions_tables)
            std::printf(" %s", id.c_str());
        char const *action = "?";
        switch (d.overall_action) {
        case st::policy::Action::Silent:
            action = "silent";
            break;
        case st::policy::Action::Badge:
            action = "badge";
            break;
        case st::policy::Action::Warn:
            action = "warn";
            break;
        case st::policy::Action::Confirm:
            action = "confirm";
            break;
        case st::policy::Action::ConfirmWithReason:
            action = "confirm+reason";
            break;
        case st::policy::Action::Block:
            action = "block";
            break;
        }
        std::printf("\n  overall_action       = %s\n", action);
    }
    return 0;
}

// Returns true iff at least one immediate child of `dir` is a regular
// file with a .zip extension. Used by pack-list to decide whether to
// route through PackRegistry (overlay loader) or fall back to the
// recursive-walk path-based discovery.
[[nodiscard]] bool directory_contains_zip(std::filesystem::path const &dir) noexcept {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
    for (auto const &e : std::filesystem::directory_iterator{dir, ec}) {
        if (ec) {
            break;
        }
        std::error_code is_file_ec;
        if (!e.is_regular_file(is_file_ec) || is_file_ec) {
            continue;
        }
        if (e.path().extension() == ".zip") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string_view pack_source_label(st::defs::PackEntry::Source s) noexcept {
    switch (s) {
    case st::defs::PackEntry::Source::LooseTopLevel:
        return "loose-top";
    case st::defs::PackEntry::Source::LoosePlatform:
        return "loose";
    case st::defs::PackEntry::Source::Zipped:
        return "zipped";
    }
    return "?";
}

int cmd_pack_list(int argc, char *argv[]) {
    std::optional<std::filesystem::path> pack_dir;
    bool quiet = false;
    bool force_registry = false;
    bool force_walk = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--quiet" || a == "-q") {
            quiet = true;
        } else if (a == "--registry") {
            force_registry = true;
        } else if (a == "--walk") {
            force_walk = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "pack-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!pack_dir.has_value()) {
            pack_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "pack-list: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    // Default the directory to st::config::definitions_root() (per
    // docs/25 §7). Override precedence still applies — passing
    // `<dir>` positionally is the CLI-flag layer and wins.
    if (!pack_dir.has_value()) {
        pack_dir = st::config::definitions_root();
        if (!quiet) {
            std::fprintf(stderr,
                         "pack-list: no directory given, using "
                         "definitions_root from config: %s\n",
                         pack_dir->string().c_str());
        }
    }
    if (force_registry && force_walk) {
        std::fputs("pack-list: --registry and --walk are mutually exclusive\n",
                   stderr);
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*pack_dir, ec) || ec) {
        std::fprintf(stderr, "pack-list: not a directory: %s\n", pack_dir->string().c_str());
        return 2;
    }

    // Decide which discovery path to use:
    //   - `--registry` / `--walk` are explicit overrides
    //   - auto: PackRegistry when any .zip is present at the top level
    //           (the ship-time install layout); recursive walk otherwise
    //           (multi-file pack layouts like fixtures/demo-pack/).
    bool const use_registry =
        force_registry || (!force_walk && directory_contains_zip(*pack_dir));

    if (use_registry) {
        auto reg_r = st::defs::PackRegistry::from_directory(*pack_dir);
        if (!reg_r.has_value()) {
            std::fprintf(stderr, "pack-list: %s\n",
                         reg_r.error().to_string().c_str());
            return 1;
        }
        auto const &reg = *reg_r;
        for (auto const &w : reg.warnings()) {
            if (!quiet) {
                std::fprintf(stderr, "  (warning) %s\n", w.c_str());
            }
        }
        auto packs = reg.list_packs();
        if (packs.empty()) {
            std::fprintf(stderr, "pack-list: no packs in %s\n",
                         pack_dir->string().c_str());
            return 1;
        }
        std::sort(packs.begin(), packs.end(),
                  [](auto const &a, auto const &b) { return a.id < b.id; });
        std::size_t loaded = 0;
        std::size_t failed = 0;
        for (auto const &entry : packs) {
            auto def = reg.load_pack(entry.id);
            if (!def.has_value()) {
                ++failed;
                if (!quiet) {
                    std::fprintf(stderr, "  (skip) %s [%s] — load failed: %s\n",
                                 entry.id.c_str(),
                                 std::string{pack_source_label(entry.source)}.c_str(),
                                 def.error().to_string().c_str());
                }
                continue;
            }
            ++loaded;
            auto const &p = def->pack();
            std::printf(
                "%-32s  %-40s  %-18s  tables=%-3zu pids=%-3zu hooks=%-3zu  [%s]\n",
                p.id.c_str(),
                p.display_name.empty() ? "—" : p.display_name.c_str(),
                p.platform.empty() ? "—" : p.platform.c_str(),
                def->tables().size(), def->pids().size(), def->hooks().size(),
                std::string{pack_source_label(entry.source)}.c_str());
        }
        std::printf("\n%zu pack%s found, %zu loaded\n", packs.size(),
                    packs.size() == 1 ? "" : "s", loaded);
        if (failed != 0) {
            std::printf("(%zu failed to load%s)\n", failed,
                        quiet ? " — rerun without --quiet to see errors" : "");
        }
        return loaded == 0 ? 1 : 0;
    }

    // Walk pack_dir for both single-file packs (`<id>.toml`) and
    // multi-file packs (`<dir>/pack.toml` + companion fragments).
    // See discover_pack_paths() for the layout rules.
    auto const pack_paths = discover_pack_paths(*pack_dir, "pack-list");
    if (pack_paths.empty()) {
        std::fprintf(stderr, "pack-list: no .toml pack files found under %s\n",
                     pack_dir->string().c_str());
        return 1;
    }

    std::size_t loaded = 0;
    std::size_t failed = 0;
    for (auto const &path : pack_paths) {
        auto const def = st::Definition::from_file(path);
        if (!def.has_value()) {
            ++failed;
            if (!quiet) {
                std::fprintf(stderr, "  (skip) %s — load failed: %s\n", path.string().c_str(),
                             def.error().to_string().c_str());
            }
            continue;
        }
        ++loaded;
        auto const &p = def->pack();
        // One-line summary per pack. Columns: id, display name (or
        // "—" when empty), platform, table count, PID count, hook
        // count. Stable order so a `pack-list dir | sort` is
        // already sorted by the source path.
        std::printf("%-32s  %-40s  %-18s  tables=%-3zu pids=%-3zu hooks=%-3zu  %s\n", p.id.c_str(),
                    p.display_name.empty() ? "—" : p.display_name.c_str(),
                    p.platform.empty() ? "—" : p.platform.c_str(), def->tables().size(),
                    def->pids().size(), def->hooks().size(), path.string().c_str());
    }

    std::printf("\n%zu pack%s found, %zu loaded\n", pack_paths.size(),
                pack_paths.size() == 1 ? "" : "s", loaded);
    if (failed != 0) {
        std::printf("(%zu failed to load%s)\n", failed,
                    quiet ? " — rerun without --quiet to see errors" : "");
    }
    return loaded == 0 ? 1 : 0;
}

int cmd_rom_identify(int argc, char *argv[]) {
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::filesystem::path> pack_dir;
    bool quiet = false;
    bool json_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--pack-dir") {
            if (i + 1 >= argc) {
                std::fputs("rom-identify: --pack-dir requires a path\n", stderr);
                return 2;
            }
            pack_dir = std::filesystem::path{argv[++i]};
        } else if (a == "--quiet" || a == "-q") {
            quiet = true;
        } else if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-identify: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "rom-identify: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path.has_value() || !pack_dir.has_value()) {
        std::fputs("rom-identify: missing ROM path or --pack-dir\n"
                   "Usage: subuwutuner-cli rom-identify <FILE.bin> "
                   "--pack-dir <dir> [--quiet]\n",
                   stderr);
        return 2;
    }

    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "rom-identify: %s\n", rom.error().to_string().c_str());
        return 1;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*pack_dir, ec) || ec) {
        std::fprintf(stderr, "rom-identify: --pack-dir is not a directory: %s\n",
                     pack_dir->string().c_str());
        return 2;
    }

    // Walk pack_dir for both single-file packs and multi-file pack.toml
    // roots — see discover_pack_paths() for the layout rules.
    auto const pack_paths = discover_pack_paths(*pack_dir, "rom-identify");
    if (pack_paths.empty()) {
        std::fprintf(stderr, "rom-identify: no .toml pack files found under %s\n",
                     pack_dir->string().c_str());
        return 1;
    }

    // Structured collector — populated for both text and JSON paths,
    // since the JSON renderer happens after the scan loop.
    struct MatchEntry {
        std::string pack_path;
        std::string pack_id;
        std::string display_name;
        std::string cid;
    };
    struct FailedEntry {
        std::string pack_path;
        std::string error;
    };
    std::vector<MatchEntry> matches;
    std::vector<FailedEntry> failed_list;
    std::size_t loaded = 0;

    for (auto const &path : pack_paths) {
        auto const def = st::Definition::from_file(path);
        if (!def.has_value()) {
            failed_list.push_back({path.string(), def.error().to_string()});
            if (!quiet && !json_mode) {
                std::fprintf(stderr, "  (skip) %s — load failed: %s\n", path.string().c_str(),
                             def.error().to_string().c_str());
            }
            continue;
        }
        ++loaded;
        if (auto const match = def->matches(*rom); match.has_value()) {
            matches.push_back({path.string(), def->pack().id, def->pack().display_name, *match});
            if (!json_mode) {
                std::printf("MATCH  %s\n", path.string().c_str());
                std::printf("       (identification: %s)\n", match->c_str());
                if (!def->pack().display_name.empty()) {
                    std::printf("       (display name:   %s)\n",
                                def->pack().display_name.c_str());
                }
            }
        }
    }

    if (json_mode) {
        std::string out;
        out.reserve(1024);
        out.append("{\"schema\":\"subuwutuner.rom-identify.v1\",\"rom\":{\"path\":");
        json_escape(out, rom_path->string());
        out.append(",\"size\":");
        out.append(std::to_string(rom->size()));
        out.append(",\"crc32\":");
        out.append(std::to_string(rom->crc32()));
        out.append("},\"pack_dir\":");
        json_escape(out, pack_dir->string());
        out.append(",\"counts\":{\"scanned\":");
        out.append(std::to_string(pack_paths.size()));
        out.append(",\"loaded\":");
        out.append(std::to_string(loaded));
        out.append(",\"matched\":");
        out.append(std::to_string(matches.size()));
        out.append(",\"failed\":");
        out.append(std::to_string(failed_list.size()));
        out.append("},\"matches\":[");
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i != 0)
                out.append(",");
            auto const &m = matches[i];
            out.append("{\"pack_path\":");
            json_escape(out, m.pack_path);
            out.append(",\"pack_id\":");
            json_escape(out, m.pack_id);
            out.append(",\"display_name\":");
            json_escape(out, m.display_name);
            out.append(",\"cid\":");
            json_escape(out, m.cid);
            out.append("}");
        }
        out.append("],\"failed\":[");
        for (std::size_t i = 0; i < failed_list.size(); ++i) {
            if (i != 0)
                out.append(",");
            auto const &f = failed_list[i];
            out.append("{\"pack_path\":");
            json_escape(out, f.pack_path);
            out.append(",\"error\":");
            json_escape(out, f.error);
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        std::printf("\n%zu pack%s scanned, %zu loaded, %zu match%s\n", pack_paths.size(),
                    pack_paths.size() == 1 ? "" : "s", loaded, matches.size(),
                    matches.size() == 1 ? "" : "es");
        if (!failed_list.empty()) {
            std::printf("(%zu pack%s failed to load%s)\n", failed_list.size(),
                        failed_list.size() == 1 ? "" : "s",
                        quiet ? " — rerun without --quiet to see errors" : "");
        }
    }

    return matches.empty() ? 1 : 0;
}

int cmd_checksum_repair(int argc, char *argv[]) {
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("checksum-repair: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("checksum-repair: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "checksum-repair: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "checksum-repair: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path.has_value() || !def_path.has_value() || !output_path.has_value()) {
        std::fputs("checksum-repair: missing ROM path, --def, or --output\n"
                   "Usage: subuwutuner-cli checksum-repair <FILE.bin> "
                   "--def <pack> --output <FILE.bin>\n",
                   stderr);
        return 2;
    }

    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "checksum-repair: %s\n", rom.error().to_string().c_str());
        return 1;
    }
    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("checksum-repair", *def_path, def.error());
    }

    // Apply repair on a working copy. apply_checksum_repair is the
    // shared seam (st::flash::apply_checksum_repair) that also
    // backs the future Flasher prepare-flash path.
    std::vector<std::uint8_t> bytes{rom->data().begin(), rom->data().end()};
    auto const status = st::flash::apply_checksum_repair(bytes, *def);

    if (!status.has_value()) {
        if (status.error().code() == st::ErrorCode::NotImplemented) {
            auto const msg = status.error().message();
            std::fprintf(stderr, "checksum-repair: not implemented — %.*s\n",
                         static_cast<int>(msg.size()), msg.data());
            return 3;
        }
        std::fprintf(stderr, "checksum-repair: %s\n", status.error().to_string().c_str());
        return 1;
    }

    std::ofstream ofs{*output_path, std::ios::binary | std::ios::trunc};
    if (!ofs) {
        std::fprintf(stderr, "checksum-repair: cannot open output: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    ofs.write(reinterpret_cast<char const *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs) {
        std::fprintf(stderr, "checksum-repair: write to '%s' failed\n",
                     output_path->string().c_str());
        return 1;
    }

    auto const field = def->pack().checksum_type;
    auto const kind = st::flash::checksum_kind_from_pack(field);
    auto const *kind_name = st::flash::checksum_kind_name(kind);
    std::printf("checksum-repair: wrote %zu bytes to %s (kind: %s)\n", bytes.size(),
                output_path->string().c_str(), kind_name);
    if (kind == st::flash::ChecksumKind::PerInstallBlockCrc32) {
        std::puts(
            "note: slot 24 (block 0x1E0000-0x200000) left untouched - the algorithm\n"
            "      is self-referential and unrecovered. Independent disassembly\n"
            "      confirms the ECU does not consult the slot table at runtime, so\n"
            "      the value's only consumer is the original aftermarket installer\n"
            "      during a subsequent reflash. Direct-CAN flash via SubuwuTuner\n"
            "      ignores it.");
    }
    return 0;
}

int cmd_checksum_verify(int argc, char *argv[]) {
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::filesystem::path> def_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("checksum-verify: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "checksum-verify: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "checksum-verify: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path.has_value() || !def_path.has_value()) {
        std::fputs("checksum-verify: missing ROM path or --def\n"
                   "Usage: subuwutuner-cli checksum-verify <FILE.bin> "
                   "--def <pack>\n",
                   stderr);
        return 2;
    }

    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "checksum-verify: %s\n", rom.error().to_string().c_str());
        return 1;
    }
    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("checksum-verify", *def_path, def.error());
    }

    auto const field = def->pack().checksum_type;
    auto const kind = st::flash::checksum_kind_from_pack(field);
    auto const *kind_name = st::flash::checksum_kind_name(kind);

    std::printf("ROM:              %s\n", rom_path->string().c_str());
    std::printf("Pack:             %s\n", def->pack().id.c_str());
    std::printf("checksum_type:    %s%s\n", field.empty() ? "(unset)" : field.c_str(),
                field.empty() ? "  → defaulting to 'none'" : "");
    std::printf("Resolved kind:    %s\n", kind_name);

    auto repair = st::flash::make_checksum_repair(kind);
    // Copy bytes; never mutate the on-disk image. ROM data() is
    // read-only here, so build a vector + run repair against it.
    std::vector<std::uint8_t> copy{rom->data().begin(), rom->data().end()};
    auto const status = repair->repair(copy);

    if (!status.has_value()) {
        if (status.error().code() == st::ErrorCode::NotImplemented) {
            std::printf("\nResult: NOT IMPLEMENTED\n");
            auto const msg = status.error().message();
            std::printf("  %.*s\n", static_cast<int>(msg.size()), msg.data());
            return 3;
        }
        std::fprintf(stderr, "\nchecksum-verify: %s\n", status.error().to_string().c_str());
        return 1;
    }

    // Compare bytes. Equal → existing checksum was already correct
    // (or the pack declared "none" so no work was needed); unequal
    // → repair would have rewritten some bytes.
    auto const slot24_caveat = [&] {
        if (kind == st::flash::ChecksumKind::PerInstallBlockCrc32) {
            std::puts(
                "note: slot 24 (block 0x1E0000-0x200000) is not verified by\n"
                "      this algorithm; a mismatch there will not show up.\n"
                "      Per independent disassembly the ECU does not consult\n"
                "      the slot table at runtime - the table is installer-side\n"
                "      metadata only.");
        }
    };
    if (copy == std::vector<std::uint8_t>{rom->data().begin(), rom->data().end()}) {
        std::printf("\nResult: VALID (no bytes would change)\n");
        slot24_caveat();
        return 0;
    }

    // Count + summarize the diff.
    std::size_t diff_bytes = 0;
    std::size_t first_diff = SIZE_MAX;
    for (std::size_t i = 0; i < rom->size(); ++i) {
        if (rom->data()[i] != copy[i]) {
            ++diff_bytes;
            if (first_diff == SIZE_MAX)
                first_diff = i;
        }
    }
    std::printf("\nResult: INVALID (%zu byte%s would be rewritten", diff_bytes,
                diff_bytes == 1 ? "" : "s");
    if (first_diff != SIZE_MAX) {
        std::printf(", first at 0x%08zX", first_diff);
    }
    std::printf(")\n");
    slot24_caveat();
    return 1;
}

// Extract the printable Subaru CID from a ROM if present, or empty.
// Mirrors print_subaru_cid's validation logic without printing.
[[nodiscard]] std::string extract_subaru_cid(st::Rom const &rom) {
    if (rom.size() < kSubaruCidOffset + kSubaruCidLength) {
        return {};
    }
    auto const data = rom.data();
    bool in_trailing_nul = false;
    std::string cid;
    cid.reserve(kSubaruCidLength);
    for (std::size_t i = 0; i < kSubaruCidLength; ++i) {
        auto const b = data[kSubaruCidOffset + i];
        if (in_trailing_nul) {
            if (b != 0)
                return {};
            continue;
        }
        if (b == 0 && i > 0) {
            in_trailing_nul = true;
            continue;
        }
        if (b < 0x20 || b > 0x7E)
            return {};
        cid.push_back(static_cast<char>(b));
    }
    return cid;
}

// `rom-info --json` emit. Schema subuwutuner.rom-info.v1. Mirrors the
// text-mode fields a CI script would gate on (size, crc32, CID,
// pack-match) but skips the ASCII-string listing — too noisy for
// machine consumption, the text mode covers exploratory browsing.
int cmd_rom_info_json(std::filesystem::path const &rom_path,
                      std::optional<std::filesystem::path> const &def_path) {
    auto const rom = st::Rom::from_file(rom_path);
    if (!rom.has_value()) {
        // Emit a minimal error-shape JSON so callers can still parse.
        std::string out{"{\"schema\":\"subuwutuner.rom-info.v1\","
                        "\"file\":"};
        json_escape(out, rom_path.string());
        out.append(",\"error\":");
        json_escape(out, rom.error().to_string());
        out.append("}\n");
        std::fputs(out.c_str(), stdout);
        return 1;
    }

    std::string out;
    out.reserve(1024);
    out.append("{\"schema\":\"subuwutuner.rom-info.v1\",\"file\":");
    json_escape(out, rom_path.string());
    out.append(",\"size\":");
    out.append(std::to_string(rom->size()));
    out.append(",\"crc32\":");
    out.append(std::to_string(rom->crc32()));
    {
        auto const cid = extract_subaru_cid(*rom);
        if (!cid.empty()) {
            out.append(",\"subaru_cid\":");
            json_escape(out, cid);
        } else {
            out.append(",\"subaru_cid\":null");
        }
    }

    if (def_path.has_value()) {
        auto const def = st::Definition::from_file(resolve_def_path(*def_path));
        if (!def.has_value()) {
            out.append(",\"pack\":{\"path\":");
            json_escape(out, def_path->string());
            out.append(",\"error\":");
            json_escape(out, def.error().to_string());
            out.append("}}\n");
            std::fputs(out.c_str(), stdout);
            return 1;
        }
        auto const &pack = def->pack();
        out.append(",\"pack\":{\"path\":");
        json_escape(out, def_path->string());
        out.append(",\"id\":");
        json_escape(out, pack.id);
        out.append(",\"display_name\":");
        json_escape(out, pack.display_name);
        out.append(",\"platform\":");
        json_escape(out, pack.platform);
        out.append(",\"expected_rom_size\":");
        out.append(std::to_string(pack.rom_size_bytes));
        out.append(",\"size_mismatch\":");
        out.append((pack.rom_size_bytes != 0 && rom->size() != pack.rom_size_bytes) ? "true"
                                                                                    : "false");
        auto const info = def->match_info(*rom);
        if (info.has_value()) {
            out.append(",\"match\":{\"name\":");
            json_escape(out, info->name);
            out.append(",\"offset\":");
            out.append(std::to_string(info->offset));
            out.append(",\"scanned\":");
            out.append(info->scanned ? "true" : "false");
            out.append("}");
        } else {
            out.append(",\"match\":null");
        }
        out.append("}");
    } else {
        out.append(",\"pack\":null");
    }

    out.append("}\n");
    std::fputs(out.c_str(), stdout);
    return 0;
}

int cmd_rom_info(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_path;
    bool json_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("rom-info: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--json") {
            json_mode = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "rom-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!rom_path.has_value()) {
        std::fputs("rom-info: missing ROM path\n", stderr);
        std::fputs("Usage: subuwutuner-cli rom-info [--def <pack.toml>] [--json] <FILE>\n",
                   stderr);
        return 2;
    }

    if (json_mode) {
        return cmd_rom_info_json(*rom_path, def_path);
    }

    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "rom-info: %s\n", rom.error().to_string().c_str());
        return 1;
    }
    print_rom_summary(*rom_path, *rom);

    if (def_path.has_value()) {
        auto const def = st::Definition::from_file(resolve_def_path(*def_path));
        if (!def.has_value()) {
            return print_def_load_error("rom-info", *def_path, def.error());
        }
        auto const validity = def->validate();
        if (!validity.has_value()) {
            std::fprintf(stderr, "rom-info: definition has issues:\n%s\n",
                         validity.error().message().data());
            // Continue — show what we can. The user explicitly asked for this pack.
        }
        print_def_summary(*def, *rom);
    }

    return 0;
}

} // namespace

// Parse a trace file of SSM response frames. Format: one frame per line,
// hex bytes separated by whitespace; '#' starts a comment that runs to
// end-of-line; blank lines are skipped. Returns the parsed frames in file
// order, or an error string in `err` and false on parse failure.
bool load_trace_file(std::filesystem::path const &path,
                     std::vector<std::vector<std::uint8_t>> &out_frames, std::string &err) {
    std::ifstream in{path};
    if (!in) {
        err = "log: cannot open trace file: " + path.string();
        return false;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (auto const hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream iss{line};
        std::vector<std::uint8_t> frame;
        std::string tok;
        while (iss >> tok) {
            unsigned value = 0;
            auto const first = tok.data();
            auto const last = tok.data() + tok.size();
            auto const res = std::from_chars(first, last, value, 16);
            if (res.ec != std::errc{} || res.ptr != last || value > 0xFFU) {
                err = "log: bad hex byte '" + tok + "' on line " + std::to_string(line_no);
                return false;
            }
            frame.push_back(static_cast<std::uint8_t>(value));
        }
        if (!frame.empty()) {
            out_frames.push_back(std::move(frame));
        }
    }
    if (out_frames.empty()) {
        err = "log: trace file is empty: " + path.string();
        return false;
    }
    return true;
}

// Split a comma-separated list into trimmed, non-empty tokens.
std::vector<std::string> split_csv_list(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto const end = s.find(',', start);
        auto const piece =
            s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        // Trim whitespace.
        std::size_t a = 0;
        while (a < piece.size() && std::isspace(static_cast<unsigned char>(piece[a])))
            ++a;
        std::size_t b = piece.size();
        while (b > a && std::isspace(static_cast<unsigned char>(piece[b - 1])))
            --b;
        if (b > a) {
            out.emplace_back(piece.substr(a, b - a));
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return out;
}

// Forward declaration — full definition lives later in this file alongside
// the autotune-family commands.
std::optional<double> parse_decimal(std::string_view raw);

// Look up a column name in a CSV header; returns the index, or
// std::string_view::npos if not found. Comparison is case-insensitive
// because community log column names are inconsistently cased.
std::size_t find_csv_column(std::vector<std::string> const &header, std::string_view name) {
    auto const to_lower = [](char c) -> char {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::string want;
    want.reserve(name.size());
    for (char c : name)
        want.push_back(to_lower(c));
    for (std::size_t i = 0; i < header.size(); ++i) {
        std::string have;
        have.reserve(header[i].size());
        for (char c : header[i])
            have.push_back(to_lower(c));
        if (have == want)
            return i;
    }
    return std::string_view::npos;
}

// Read just the header row (first non-blank, non-# line) so the CLI
// can resolve column names to indices before handing off to the snapshot
// CSV reader (which addresses columns by index).
bool read_csv_header(std::filesystem::path const &path, std::vector<std::string> &out,
                     std::string &err) {
    std::ifstream f{path};
    if (!f) {
        err = "knock-snapshot: cannot open " + path.string();
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // Trim leading whitespace
        std::size_t a = 0;
        while (a < line.size() && std::isspace(static_cast<unsigned char>(line[a])))
            ++a;
        if (a >= line.size() || line[a] == '#')
            continue;
        // Split on commas
        std::size_t start = a;
        for (std::size_t i = a; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',') {
                // Trim each field
                std::size_t f0 = start;
                std::size_t f1 = i;
                while (f0 < f1 && std::isspace(static_cast<unsigned char>(line[f0])))
                    ++f0;
                while (f1 > f0 && std::isspace(static_cast<unsigned char>(line[f1 - 1])))
                    --f1;
                out.emplace_back(line.substr(f0, f1 - f0));
                start = i + 1;
            }
        }
        return true;
    }
    err = "knock-snapshot: " + path.string() + " is empty or has no header";
    return false;
}

int cmd_knock_snapshot(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> rpm_col;
    std::optional<std::string> load_col;
    std::optional<std::string> flkc_cols_arg;
    std::optional<std::string> fbkc_cols_arg;
    std::optional<std::size_t> cylinders_override;
    std::optional<double> window_seconds;
    std::optional<double> sample_rate_hz;
    std::optional<double> min_rpm_arg;
    std::optional<double> min_load_arg;
    bool no_gate = false;
    bool json_mode = false;
    bool csv_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "knock-snapshot: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--json") {
            json_mode = true;
            continue;
        }
        if (a == "--csv") {
            csv_mode = true;
            continue;
        }
        if (a == "--log") {
            auto *v = need("--log");
            if (!v)
                return 2;
            log_path = std::filesystem::path{v};
        } else if (a == "--rpm-col") {
            auto *v = need("--rpm-col");
            if (!v)
                return 2;
            rpm_col = v;
        } else if (a == "--load-col") {
            auto *v = need("--load-col");
            if (!v)
                return 2;
            load_col = v;
        } else if (a == "--flkc-cols") {
            auto *v = need("--flkc-cols");
            if (!v)
                return 2;
            flkc_cols_arg = v;
        } else if (a == "--fbkc-cols") {
            auto *v = need("--fbkc-cols");
            if (!v)
                return 2;
            fbkc_cols_arg = v;
        } else if (a == "--cylinders") {
            auto *v = need("--cylinders");
            if (!v)
                return 2;
            char *end = nullptr;
            long long const n = std::strtoll(v, &end, 10);
            if (end == v || *end != '\0' || n < 1 || n > 6) {
                std::fprintf(stderr, "knock-snapshot: --cylinders must be 1..6 (got '%s')\n", v);
                return 2;
            }
            cylinders_override = static_cast<std::size_t>(n);
        } else if (a == "--window-seconds") {
            auto *v = need("--window-seconds");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "knock-snapshot: --window-seconds must be a number (got '%s')\n", v);
                return 2;
            }
            window_seconds = *p;
        } else if (a == "--sample-rate-hz") {
            auto *v = need("--sample-rate-hz");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "knock-snapshot: --sample-rate-hz must be a number (got '%s')\n", v);
                return 2;
            }
            sample_rate_hz = *p;
        } else if (a == "--min-rpm") {
            auto *v = need("--min-rpm");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr, "knock-snapshot: --min-rpm must be a number (got '%s')\n", v);
                return 2;
            }
            min_rpm_arg = *p;
        } else if (a == "--min-load") {
            auto *v = need("--min-load");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr, "knock-snapshot: --min-load must be a number (got '%s')\n", v);
                return 2;
            }
            min_load_arg = *p;
        } else if (a == "--no-gate") {
            no_gate = true;
        } else {
            std::fprintf(stderr, "knock-snapshot: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (json_mode && csv_mode) {
        std::fputs("knock-snapshot: --json and --csv are mutually exclusive\n", stderr);
        return 2;
    }

    if (!log_path.has_value() || !flkc_cols_arg.has_value()) {
        std::fputs("knock-snapshot: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!flkc_cols_arg.has_value())
            std::fputs(" --flkc-cols", stderr);
        std::fputs(
            "\nUsage: subuwutuner-cli knock-snapshot --log <CSV> --flkc-cols <a,b,c,d>\n"
            "                                      [--fbkc-cols <a,b,c,d>] [--rpm-col <name>]\n"
            "                                      [--load-col <name>] [--cylinders N]\n"
            "                                      [--window-seconds N] [--sample-rate-hz N]\n"
            "                                      [--min-rpm N] [--min-load N] [--no-gate]\n",
            stderr);
        return 2;
    }

    std::vector<std::string> header;
    {
        std::string err;
        if (!read_csv_header(*log_path, header, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    auto const flkc_names = split_csv_list(*flkc_cols_arg);
    if (flkc_names.empty()) {
        std::fputs("knock-snapshot: --flkc-cols cannot be empty\n", stderr);
        return 2;
    }
    if (flkc_names.size() > st::log::knock::kMaxCylinders) {
        std::fprintf(stderr, "knock-snapshot: too many cylinders (%zu); max %u\n",
                     flkc_names.size(), static_cast<unsigned>(st::log::knock::kMaxCylinders));
        return 2;
    }
    std::vector<std::string> fbkc_names;
    if (fbkc_cols_arg.has_value()) {
        fbkc_names = split_csv_list(*fbkc_cols_arg);
        if (!fbkc_names.empty() && fbkc_names.size() != flkc_names.size()) {
            std::fprintf(stderr,
                         "knock-snapshot: --fbkc-cols count (%zu) must match "
                         "--flkc-cols (%zu)\n",
                         fbkc_names.size(), flkc_names.size());
            return 2;
        }
    }

    st::log::knock::PidMapping mapping;
    mapping.cylinder_count = cylinders_override.has_value()
                                 ? static_cast<std::uint8_t>(*cylinders_override)
                                 : static_cast<std::uint8_t>(flkc_names.size());

    auto const resolve = [&](std::string_view name, char const *flag) -> std::size_t {
        std::size_t const idx = find_csv_column(header, name);
        if (idx == std::string_view::npos) {
            std::fprintf(stderr, "knock-snapshot: %s column '%.*s' not in CSV header\n", flag,
                         static_cast<int>(name.size()), name.data());
        }
        return idx;
    };

    bool bad = false;
    if (rpm_col.has_value()) {
        std::size_t const idx = resolve(*rpm_col, "--rpm-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.rpm_idx = idx;
    }
    if (load_col.has_value()) {
        std::size_t const idx = resolve(*load_col, "--load-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.load_idx = idx;
    }
    for (std::size_t c = 0; c < flkc_names.size(); ++c) {
        std::size_t const idx = resolve(flkc_names[c], "--flkc-cols");
        if (idx == std::string_view::npos) {
            bad = true;
            continue;
        }
        mapping.fine_knock_learn[c] = idx;
    }
    for (std::size_t c = 0; c < fbkc_names.size(); ++c) {
        std::size_t const idx = resolve(fbkc_names[c], "--fbkc-cols");
        if (idx == std::string_view::npos) {
            bad = true;
            continue;
        }
        mapping.feedback_knock[c] = idx;
    }
    if (bad)
        return 1;

    st::log::knock::WindowConfig cfg;
    if (window_seconds.has_value())
        cfg.window_seconds = *window_seconds;
    if (sample_rate_hz.has_value())
        cfg.sample_rate_hz = *sample_rate_hz;
    if (min_rpm_arg.has_value())
        cfg.min_rpm = *min_rpm_arg;
    if (min_load_arg.has_value())
        cfg.min_load = *min_load_arg;
    cfg.require_load_gate = !no_gate;

    auto const r = st::log::knock::snapshot_from_csv(log_path->string(), mapping, cfg);
    if (!r.has_value()) {
        std::fprintf(stderr, "knock-snapshot: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &s = *r;

    if (json_mode) {
        // subuwutuner.knock-snapshot.v1 — per-cylinder counters + gate
        // status. Per-cylinder array uses 0-based indexing in JSON
        // (consistent with array natural order); the text dump shows
        // 1-based for human reading.
        // 256-byte buffer covers the longest record (verified by hand
        // against the per-cyl format string — 6 doubles + 5 keys + 2
        // bools peaks around 220 chars).
        std::string out{"{\"schema\":\"subuwutuner.knock-snapshot.v1\",\"log\":"};
        json_escape(out, log_path->string());
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      ",\"window_seconds\":%.3f,\"sample_rate_hz\":%.3f,"
                      "\"samples_considered\":%llu,\"samples_gated_out\":%llu,"
                      "\"gate\":%s,\"cylinder_count\":%u,\"per_cyl\":[",
                      cfg.window_seconds, cfg.sample_rate_hz,
                      static_cast<unsigned long long>(s.samples_considered),
                      static_cast<unsigned long long>(s.samples_gated_out),
                      cfg.require_load_gate ? "true" : "false",
                      static_cast<unsigned>(s.cylinder_count));
        out.append(buf);
        for (std::uint8_t c = 0; c < s.cylinder_count; ++c) {
            auto const &p = s.per_cyl[c];
            bool const has_flkc = mapping.fine_knock_learn[c] != st::log::knock::kNoPid;
            bool const has_fbkc = mapping.feedback_knock[c] != st::log::knock::kNoPid;
            if (c > 0) out.append(",");
            std::snprintf(buf, sizeof buf,
                          "{\"cyl\":%u,\"has_flkc\":%s,\"has_fbkc\":%s,"
                          "\"flkc_current\":%.6f,\"flkc_mean_window\":%.6f,"
                          "\"flkc_min_window\":%.6f,\"fbkc_current\":%.6f,"
                          "\"events_window\":%u,\"delta_from_cyl_mean\":%.6f}",
                          static_cast<unsigned>(c + 1),
                          has_flkc ? "true" : "false", has_fbkc ? "true" : "false",
                          has_flkc ? p.current_flkc : 0.0,
                          has_flkc ? p.mean_flkc_window : 0.0,
                          has_flkc ? p.min_flkc_window : 0.0,
                          has_fbkc ? p.current_fbkc : 0.0,
                          static_cast<unsigned>(p.event_count_window),
                          has_flkc ? p.delta_from_cyl_mean : 0.0);
            out.append(buf);
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    if (csv_mode) {
        // CSV shape matches the JSON envelope: comment-prefixed
        // metadata header (pandas read_csv(comment='#') / Excel
        // Power-Query/text-import both ignore #-prefixed lines), then
        // a per-cylinder data table. Per-cyl values are zero when the
        // mapping has no PID for that signal — same convention as the
        // human-readable text dump.
        std::printf("# schema=subuwutuner.knock-snapshot.v1\n");
        std::printf("# log=%s\n", log_path->string().c_str());
        std::printf("# window_seconds=%.3f\n", cfg.window_seconds);
        std::printf("# sample_rate_hz=%.3f\n", cfg.sample_rate_hz);
        std::printf("# samples_considered=%llu\n",
                    static_cast<unsigned long long>(s.samples_considered));
        std::printf("# samples_gated_out=%llu\n",
                    static_cast<unsigned long long>(s.samples_gated_out));
        std::printf("# gate=%s\n", cfg.require_load_gate ? "true" : "false");
        std::printf("# cylinder_count=%u\n", static_cast<unsigned>(s.cylinder_count));
        std::printf(
            "cyl,has_flkc,has_fbkc,flkc_current,flkc_mean_window,flkc_min_window,"
            "fbkc_current,events_window,delta_from_cyl_mean\n");
        for (std::uint8_t c = 0; c < s.cylinder_count; ++c) {
            auto const &p = s.per_cyl[c];
            bool const has_flkc = mapping.fine_knock_learn[c] != st::log::knock::kNoPid;
            bool const has_fbkc = mapping.feedback_knock[c] != st::log::knock::kNoPid;
            std::printf("%u,%s,%s,%.6f,%.6f,%.6f,%.6f,%u,%.6f\n",
                        static_cast<unsigned>(c + 1),
                        has_flkc ? "true" : "false", has_fbkc ? "true" : "false",
                        has_flkc ? p.current_flkc : 0.0,
                        has_flkc ? p.mean_flkc_window : 0.0,
                        has_flkc ? p.min_flkc_window : 0.0,
                        has_fbkc ? p.current_fbkc : 0.0,
                        static_cast<unsigned>(p.event_count_window),
                        has_flkc ? p.delta_from_cyl_mean : 0.0);
        }
        return 0;
    }

    std::printf("Per-Cylinder Knock Dashboard\n");
    std::printf("============================\n");
    std::printf("Log:                %s\n", log_path->string().c_str());
    std::printf("Window:             %.1fs @ %.1f Hz\n", cfg.window_seconds, cfg.sample_rate_hz);
    std::printf("Samples considered: %llu\n",
                static_cast<unsigned long long>(s.samples_considered));
    std::printf("Samples gated out:  %llu  (gate: %s)\n",
                static_cast<unsigned long long>(s.samples_gated_out),
                cfg.require_load_gate ? "on" : "off");
    std::printf("\n");
    std::printf("Cyl | FLKC current | FLKC mean | FLKC min | FBKC current | Events | dMean\n");
    std::printf("----+--------------+-----------+----------+--------------+--------+-------\n");
    for (std::uint8_t c = 0; c < s.cylinder_count; ++c) {
        auto const &p = s.per_cyl[c];
        bool const has_flkc = mapping.fine_knock_learn[c] != st::log::knock::kNoPid;
        bool const has_fbkc = mapping.feedback_knock[c] != st::log::knock::kNoPid;
        std::printf(" %2u | %+12.3f | %+9.3f | %+8.3f | %+12.3f | %6u | %+5.2f%s\n",
                    static_cast<unsigned>(c + 1), has_flkc ? p.current_flkc : 0.0,
                    has_flkc ? p.mean_flkc_window : 0.0, has_flkc ? p.min_flkc_window : 0.0,
                    has_fbkc ? p.current_fbkc : 0.0, static_cast<unsigned>(p.event_count_window),
                    has_flkc ? p.delta_from_cyl_mean : 0.0,
                    (!has_flkc && !has_fbkc) ? "  (no per-cyl signal)" : "");
    }

    return 0;
}

int cmd_adaptive_history(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> ts_col;
    std::optional<std::string> ltft_col;
    std::optional<std::string> dam_col;
    std::optional<std::string> iac_col;
    std::optional<double> bucket_seconds;
    std::optional<std::string> ts_unit_arg;
    std::optional<std::size_t> min_samples_per_bucket;
    bool verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "adaptive-history: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--log") {
            auto *v = need("--log");
            if (!v)
                return 2;
            log_path = std::filesystem::path{v};
        } else if (a == "--timestamp-col") {
            auto *v = need("--timestamp-col");
            if (!v)
                return 2;
            ts_col = v;
        } else if (a == "--ltft-col") {
            auto *v = need("--ltft-col");
            if (!v)
                return 2;
            ltft_col = v;
        } else if (a == "--dam-col") {
            auto *v = need("--dam-col");
            if (!v)
                return 2;
            dam_col = v;
        } else if (a == "--idle-adapt-col") {
            auto *v = need("--idle-adapt-col");
            if (!v)
                return 2;
            iac_col = v;
        } else if (a == "--bucket-seconds") {
            auto *v = need("--bucket-seconds");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "adaptive-history: --bucket-seconds must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            bucket_seconds = *p;
        } else if (a == "--timestamp-unit") {
            auto *v = need("--timestamp-unit");
            if (!v)
                return 2;
            ts_unit_arg = v;
        } else if (a == "--min-samples-per-bucket") {
            auto *v = need("--min-samples-per-bucket");
            if (!v)
                return 2;
            char *end = nullptr;
            long long const n = std::strtoll(v, &end, 10);
            if (end == v || *end != '\0' || n < 0) {
                std::fprintf(stderr,
                             "adaptive-history: "
                             "--min-samples-per-bucket must be a "
                             "non-negative integer (got '%s')\n",
                             v);
                return 2;
            }
            min_samples_per_bucket = static_cast<std::size_t>(n);
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else {
            std::fprintf(stderr, "adaptive-history: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (!log_path.has_value() || !ts_col.has_value()) {
        std::fputs("adaptive-history: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!ts_col.has_value())
            std::fputs(" --timestamp-col", stderr);
        std::fputs(
            "\nUsage: subuwutuner-cli adaptive-history --log <CSV> --timestamp-col <name>\n"
            "                                        [--ltft-col <name>] [--dam-col <name>]\n"
            "                                        [--idle-adapt-col <name>] [--bucket-seconds "
            "N]\n"
            "                                        [--timestamp-unit "
            "seconds|millis|micros|rows]\n"
            "                                        [--min-samples-per-bucket N] [--verbose]\n",
            stderr);
        return 2;
    }
    if (!ltft_col.has_value() && !dam_col.has_value() && !iac_col.has_value()) {
        std::fputs("adaptive-history: at least one of --ltft-col / "
                   "--dam-col / --idle-adapt-col is required\n",
                   stderr);
        return 2;
    }

    // Read the CSV header to resolve column names → indices.
    std::vector<std::string> header;
    {
        std::string err;
        if (!read_csv_header(*log_path, header, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    auto const resolve = [&](std::string_view name, char const *flag) -> std::size_t {
        std::size_t const idx = find_csv_column(header, name);
        if (idx == std::string_view::npos) {
            std::fprintf(stderr, "adaptive-history: %s column '%.*s' not in CSV header\n", flag,
                         static_cast<int>(name.size()), name.data());
        }
        return idx;
    };

    st::log::adaptive::ColumnMapping mapping;
    bool bad = false;
    {
        std::size_t const idx = resolve(*ts_col, "--timestamp-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.timestamp_idx = idx;
    }
    if (ltft_col.has_value()) {
        std::size_t const idx = resolve(*ltft_col, "--ltft-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Ltft)] = idx;
    }
    if (dam_col.has_value()) {
        std::size_t const idx = resolve(*dam_col, "--dam-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Dam)] = idx;
    }
    if (iac_col.has_value()) {
        std::size_t const idx = resolve(*iac_col, "--idle-adapt-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::IdleAdapt)] =
                idx;
    }
    if (bad)
        return 1;

    st::log::adaptive::BucketConfig cfg;
    if (bucket_seconds.has_value())
        cfg.bucket_seconds = *bucket_seconds;
    if (min_samples_per_bucket.has_value())
        cfg.min_samples_per_bucket = static_cast<std::uint32_t>(*min_samples_per_bucket);
    if (ts_unit_arg.has_value()) {
        std::string u = *ts_unit_arg;
        for (auto &c : u)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (u == "seconds" || u == "s")
            cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixSeconds;
        else if (u == "millis" || u == "ms")
            cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMillis;
        else if (u == "micros" || u == "us")
            cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMicros;
        else if (u == "rows" || u == "row")
            cfg.timestamp_unit = st::log::adaptive::TimestampUnit::RowIndex;
        else {
            std::fprintf(stderr,
                         "adaptive-history: --timestamp-unit must be "
                         "one of seconds|millis|micros|rows (got '%s')\n",
                         ts_unit_arg->c_str());
            return 2;
        }
    }

    auto const r = st::log::adaptive::snapshot_from_csv(log_path->string(), mapping, cfg);
    if (!r.has_value()) {
        std::fprintf(stderr, "adaptive-history: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &s = *r;

    char const *signal_names[3] = {"LTFT", "DAM", "IdleAdapt"};
    std::printf("Adaptive Learning History\n");
    std::printf("=========================\n");
    std::printf("Log:             %s\n", log_path->string().c_str());
    std::printf("Bucket width:    %.1f s\n", cfg.bucket_seconds);
    std::printf("Total samples:   %llu\n", static_cast<unsigned long long>(s.total_samples));
    if (s.time_span_seconds > 0) {
        std::printf("Time span:       %.1f s (%.2f days)\n", s.time_span_seconds,
                    s.time_span_seconds / 86400.0);
        std::printf("Range:           %.0f .. %.0f\n", s.earliest_timestamp, s.latest_timestamp);
    }
    std::printf("\n");
    std::printf("Signal     | buckets | mean      | min       | max       | drift/day\n");
    std::printf("-----------+---------+-----------+-----------+-----------+----------\n");
    for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
        auto const &ser = s.series[k];
        if (!ser.has_data) {
            std::printf(" %-9s | %7s | %9s | %9s | %9s | %9s\n", signal_names[k], "-", "-", "-",
                        "-", "-");
            continue;
        }
        std::printf(" %-9s | %7zu | %+9.4f | %+9.4f | %+9.4f | %+9.4f\n", signal_names[k],
                    ser.points.size(), ser.overall_mean, ser.overall_min, ser.overall_max,
                    ser.drift_per_second * 86400.0);
    }

    if (verbose) {
        std::printf("\nPer-bucket detail:\n");
        for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
            auto const &ser = s.series[k];
            if (!ser.has_data)
                continue;
            std::printf("\n[%s]\n", signal_names[k]);
            std::printf("  bucket_center   |   mean   |  min   |  max   | n\n");
            std::printf("  ----------------+----------+--------+--------+----\n");
            for (auto const &p : ser.points) {
                std::printf("  %15.1f | %+8.4f | %+6.4f | %+6.4f | %u\n", p.bucket_center, p.mean,
                            p.min, p.max, p.count);
            }
        }
    }

    return 0;
}

// `ai-drift` — load an adaptive-history CSV (same shape as
// adaptive-history) and run the Tier-1 rule classifier from
// docs/20. Prints a diagnosis (cause, confidence, evidence,
// alternatives, recommended checks). --json emits a single
// subuwutuner.ai-drift.v1 record for machine consumers.
//
// The classifier is pure-domain; this verb is just the file-I/O
// + presentation wrapper. Pinned by tests/unit/ai/test_drift.cpp at
// the library level — this CLI is also smoke-tested in
// tests/unit/cli/test_cli_integration.cpp.
int cmd_ai_drift(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> ts_col;
    std::optional<std::string> ltft_col;
    std::optional<std::string> dam_col;
    std::optional<std::string> iac_col;
    std::optional<double> bucket_seconds;
    std::optional<std::string> ts_unit_arg;
    bool json_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ai-drift: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--log") {
            auto *v = need("--log");
            if (!v) return 2;
            log_path = std::filesystem::path{v};
        } else if (a == "--timestamp-col") {
            auto *v = need("--timestamp-col");
            if (!v) return 2;
            ts_col = v;
        } else if (a == "--ltft-col") {
            auto *v = need("--ltft-col");
            if (!v) return 2;
            ltft_col = v;
        } else if (a == "--dam-col") {
            auto *v = need("--dam-col");
            if (!v) return 2;
            dam_col = v;
        } else if (a == "--idle-adapt-col") {
            auto *v = need("--idle-adapt-col");
            if (!v) return 2;
            iac_col = v;
        } else if (a == "--bucket-seconds") {
            auto *v = need("--bucket-seconds");
            if (!v) return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr, "ai-drift: --bucket-seconds must be a number (got '%s')\n", v);
                return 2;
            }
            bucket_seconds = *p;
        } else if (a == "--timestamp-unit") {
            auto *v = need("--timestamp-unit");
            if (!v) return 2;
            ts_unit_arg = v;
        } else if (a == "--json") {
            json_mode = true;
        } else {
            std::fprintf(stderr, "ai-drift: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (!log_path.has_value() || !ts_col.has_value()) {
        std::fputs("ai-drift: missing required arguments:", stderr);
        if (!log_path.has_value()) std::fputs(" --log", stderr);
        if (!ts_col.has_value()) std::fputs(" --timestamp-col", stderr);
        std::fputs(
            "\nUsage: subuwutuner-cli ai-drift --log <CSV> --timestamp-col <name>\n"
            "                                 [--ltft-col <name>] [--dam-col <name>]\n"
            "                                 [--idle-adapt-col <name>] [--bucket-seconds N]\n"
            "                                 [--timestamp-unit seconds|millis|micros|rows]\n"
            "                                 [--json]\n",
            stderr);
        return 2;
    }
    if (!ltft_col.has_value() && !dam_col.has_value() && !iac_col.has_value()) {
        std::fputs("ai-drift: at least one of --ltft-col / --dam-col / "
                   "--idle-adapt-col is required\n", stderr);
        return 2;
    }

    // Resolve header → column indices. Reuses the adaptive-history
    // CSV reader pattern; deliberately kept parallel rather than
    // factored out so the two verbs evolve independently if their
    // input requirements diverge.
    std::vector<std::string> header;
    {
        std::string err;
        if (!read_csv_header(*log_path, header, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    auto const resolve = [&](std::string_view name, char const *flag) -> std::size_t {
        std::size_t const idx = find_csv_column(header, name);
        if (idx == std::string_view::npos) {
            std::fprintf(stderr, "ai-drift: %s column '%.*s' not in CSV header\n", flag,
                         static_cast<int>(name.size()), name.data());
        }
        return idx;
    };

    st::log::adaptive::ColumnMapping mapping;
    bool bad = false;
    {
        std::size_t const idx = resolve(*ts_col, "--timestamp-col");
        if (idx == std::string_view::npos) bad = true;
        else mapping.timestamp_idx = idx;
    }
    if (ltft_col.has_value()) {
        std::size_t const idx = resolve(*ltft_col, "--ltft-col");
        if (idx == std::string_view::npos) bad = true;
        else mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Ltft)] = idx;
    }
    if (dam_col.has_value()) {
        std::size_t const idx = resolve(*dam_col, "--dam-col");
        if (idx == std::string_view::npos) bad = true;
        else mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Dam)] = idx;
    }
    if (iac_col.has_value()) {
        std::size_t const idx = resolve(*iac_col, "--idle-adapt-col");
        if (idx == std::string_view::npos) bad = true;
        else mapping.signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::IdleAdapt)] = idx;
    }
    if (bad) return 1;

    st::log::adaptive::BucketConfig cfg;
    if (bucket_seconds.has_value()) cfg.bucket_seconds = *bucket_seconds;
    if (ts_unit_arg.has_value()) {
        std::string u = *ts_unit_arg;
        for (auto &c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (u == "seconds") cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixSeconds;
        else if (u == "millis") cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMillis;
        else if (u == "micros") cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMicros;
        else if (u == "rows") cfg.timestamp_unit = st::log::adaptive::TimestampUnit::RowIndex;
        else {
            std::fprintf(stderr, "ai-drift: unknown --timestamp-unit '%s' "
                                 "(expected seconds / millis / micros / rows)\n",
                         ts_unit_arg->c_str());
            return 2;
        }
    }

    auto snapshot = st::log::adaptive::snapshot_from_csv(log_path->string(), mapping, cfg);
    if (!snapshot.has_value()) {
        std::fprintf(stderr, "ai-drift: %s\n", snapshot.error().to_string().c_str());
        return 1;
    }

    auto const diag = st::ai::drift::classify(*snapshot);

    if (json_mode) {
        std::string out{"{\"schema\":\"subuwutuner.ai-drift.v1\",\"cause\":"};
        json_escape(out, diag.cause);
        out.append(",\"confidence\":");
        json_escape(out, st::ai::drift::confidence_name(diag.confidence));
        out.append(",\"description\":");
        json_escape(out, diag.description);
        out.append(",\"evidence\":[");
        for (std::size_t k = 0; k < diag.evidence.size(); ++k) {
            if (k > 0) out.append(",");
            json_escape(out, diag.evidence[k]);
        }
        out.append("],\"alternatives\":[");
        for (std::size_t k = 0; k < diag.alternatives.size(); ++k) {
            if (k > 0) out.append(",");
            json_escape(out, diag.alternatives[k]);
        }
        out.append("],\"recommended_checks\":[");
        for (std::size_t k = 0; k < diag.recommended_checks.size(); ++k) {
            if (k > 0) out.append(",");
            json_escape(out, diag.recommended_checks[k]);
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    std::printf("Drift diagnosis\n");
    std::printf("  Cause:      %s\n", diag.cause.c_str());
    std::printf("  Confidence: %.*s\n",
                static_cast<int>(st::ai::drift::confidence_name(diag.confidence).size()),
                st::ai::drift::confidence_name(diag.confidence).data());
    std::printf("  %s\n", diag.description.c_str());
    if (!diag.evidence.empty()) {
        std::printf("\nEvidence:\n");
        for (auto const &e : diag.evidence) {
            std::printf("  - %s\n", e.c_str());
        }
    }
    if (!diag.alternatives.empty()) {
        std::printf("\nAlternatives:\n");
        for (auto const &a : diag.alternatives) {
            std::printf("  - %s\n", a.c_str());
        }
    }
    if (!diag.recommended_checks.empty()) {
        std::printf("\nRecommended checks:\n");
        for (auto const &c : diag.recommended_checks) {
            std::printf("  - %s\n", c.c_str());
        }
    }
    return 0;
}

// ai-narrate — Tier 2 LLM narration CLI surface (docs/20). Wraps
// st::ai::Backend so the user can smoke-test the live HTTPS path
// without going through the GUI confirm modal, and so the engine
// is scriptable for batch log processing. Auth is via either
// --key <K> (visible on cmdline — convenient for one-shot use)
// or --key-env <NAME> (read from env var — preferred for shared
// boxes / CI / shell history hygiene). Prompt comes from --prompt,
// --prompt-file, or stdin (--prompt - / piped input).
int cmd_ai_narrate(int argc, char *argv[]) {
    std::string provider_arg;
    std::string api_key;
    std::string key_env;
    std::string model;
    std::string system_prompt;
    std::string system_file;
    std::string prompt;
    std::string prompt_file;
    bool prompt_stdin = false;
    bool json_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ai-narrate: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--provider") {
            auto *v = need("--provider");
            if (!v) return 2;
            provider_arg = v;
        } else if (a == "--key") {
            auto *v = need("--key");
            if (!v) return 2;
            api_key = v;
        } else if (a == "--key-env") {
            auto *v = need("--key-env");
            if (!v) return 2;
            key_env = v;
        } else if (a == "--model") {
            auto *v = need("--model");
            if (!v) return 2;
            model = v;
        } else if (a == "--system") {
            auto *v = need("--system");
            if (!v) return 2;
            system_prompt = v;
        } else if (a == "--system-file") {
            auto *v = need("--system-file");
            if (!v) return 2;
            system_file = v;
        } else if (a == "--prompt") {
            auto *v = need("--prompt");
            if (!v) return 2;
            if (std::string_view{v} == "-") {
                prompt_stdin = true;
            } else {
                prompt = v;
            }
        } else if (a == "--prompt-file") {
            auto *v = need("--prompt-file");
            if (!v) return 2;
            prompt_file = v;
        } else if (a == "--prompt-stdin") {
            prompt_stdin = true;
        } else if (a == "--json") {
            json_mode = true;
        } else {
            std::fprintf(stderr, "ai-narrate: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (provider_arg.empty()) {
        std::fputs("ai-narrate: --provider is required (anthropic|openai)\n",
                   stderr);
        return 2;
    }
    if (api_key.empty() && !key_env.empty()) {
        if (auto const *v = std::getenv(key_env.c_str()); v != nullptr) {
            api_key = v;
        } else {
            std::fprintf(stderr, "ai-narrate: env var %s is not set\n",
                         key_env.c_str());
            return 2;
        }
    }
    if (api_key.empty()) {
        std::fputs("ai-narrate: --key <K> or --key-env <NAME> is required\n",
                   stderr);
        return 2;
    }

    // Resolve the prompt. Priority: --prompt-file > --prompt-stdin >
    // --prompt > empty (error). Stdin path slurps until EOF so a pipe
    // from another CLI verb (`subuwutuner-cli ai-drift ... | ai-narrate ...`)
    // is the natural composition.
    if (!prompt_file.empty()) {
        std::ifstream in{prompt_file, std::ios::binary};
        if (!in) {
            std::fprintf(stderr, "ai-narrate: cannot open %s\n",
                         prompt_file.c_str());
            return 2;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        prompt = ss.str();
    } else if (prompt_stdin) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        prompt = ss.str();
    }
    if (prompt.empty()) {
        std::fputs("ai-narrate: empty prompt — supply --prompt, "
                   "--prompt-file, or --prompt-stdin\n",
                   stderr);
        return 2;
    }
    if (!system_file.empty()) {
        std::ifstream in{system_file, std::ios::binary};
        if (!in) {
            std::fprintf(stderr, "ai-narrate: cannot open %s\n",
                         system_file.c_str());
            return 2;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        system_prompt = ss.str();
    }

    std::unique_ptr<st::ai::Backend> backend;
    if (provider_arg == "anthropic") {
        backend = st::ai::make_anthropic_backend(std::move(api_key),
                                                  std::move(model));
    } else if (provider_arg == "openai") {
        backend = st::ai::make_openai_backend(std::move(api_key),
                                               std::move(model));
    } else {
        std::fprintf(stderr,
                     "ai-narrate: --provider must be 'anthropic' or "
                     "'openai' (got '%s')\n",
                     provider_arg.c_str());
        return 2;
    }

    auto const info = backend->info();
    auto const r = backend->complete(system_prompt, prompt);
    if (!r.has_value()) {
        if (json_mode) {
            std::string out;
            out.append("{\"schema\":\"subuwutuner.ai-narrate.v1\",\"ok\":false,"
                       "\"error\":");
            st::json_escape(out, r.error().message());
            out.append(",\"provider\":");
            st::json_escape(out, info.name);
            out.append(",\"model\":");
            st::json_escape(out, info.model);
            out.append("}\n");
            std::fputs(out.c_str(), stdout);
        } else {
            std::fprintf(stderr, "ai-narrate: %.*s\n",
                         static_cast<int>(r.error().message().size()),
                         r.error().message().data());
        }
        return 1;
    }
    if (json_mode) {
        std::string out;
        out.append("{\"schema\":\"subuwutuner.ai-narrate.v1\",\"ok\":true,"
                   "\"response\":");
        st::json_escape(out, *r);
        out.append(",\"provider\":");
        st::json_escape(out, info.name);
        out.append(",\"model\":");
        st::json_escape(out, info.model);
        out.append(",\"locality\":\"");
        out.append(info.locality == st::ai::BackendLocality::Cloud ? "cloud"
                                                                    : "local");
        out.append("\"}\n");
        std::fputs(out.c_str(), stdout);
    } else {
        std::fputs(r->c_str(), stdout);
        if (r->empty() || r->back() != '\n')
            std::fputc('\n', stdout);
    }
    return 0;
}

int cmd_coldstart_analyze(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> ts_col;
    std::optional<std::string> ect_col;
    std::optional<std::string> iat_col;
    std::optional<std::string> rpm_col;
    std::optional<std::string> obs_col;
    std::optional<std::string> cmd_col;
    std::optional<std::string> timing_col;
    std::optional<std::string> cl_col;
    std::optional<double> cold_threshold_c;
    std::optional<double> ect_bin_width_c;
    std::optional<std::size_t> min_samples_per_bin;
    std::optional<std::string> ts_unit_arg;
    // Methodology target curve as comma-separated "ect:lambda" pairs.
    // Example: --target "0:0.82,20:0.88,40:0.93,55:1.00"
    std::optional<std::string> target_curve_arg;
    bool json_mode = false;
    bool csv_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "coldstart-analyze: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--log") {
            auto *v = need("--log");
            if (!v)
                return 2;
            log_path = std::filesystem::path{v};
        } else if (a == "--timestamp-col") {
            auto *v = need("--timestamp-col");
            if (!v)
                return 2;
            ts_col = v;
        } else if (a == "--ect-col") {
            auto *v = need("--ect-col");
            if (!v)
                return 2;
            ect_col = v;
        } else if (a == "--iat-col") {
            auto *v = need("--iat-col");
            if (!v)
                return 2;
            iat_col = v;
        } else if (a == "--rpm-col") {
            auto *v = need("--rpm-col");
            if (!v)
                return 2;
            rpm_col = v;
        } else if (a == "--observed-lambda-col") {
            auto *v = need("--observed-lambda-col");
            if (!v)
                return 2;
            obs_col = v;
        } else if (a == "--commanded-lambda-col") {
            auto *v = need("--commanded-lambda-col");
            if (!v)
                return 2;
            cmd_col = v;
        } else if (a == "--timing-col") {
            auto *v = need("--timing-col");
            if (!v)
                return 2;
            timing_col = v;
        } else if (a == "--closed-loop-col") {
            auto *v = need("--closed-loop-col");
            if (!v)
                return 2;
            cl_col = v;
        } else if (a == "--cold-threshold-c") {
            auto *v = need("--cold-threshold-c");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "coldstart-analyze: --cold-threshold-c must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            cold_threshold_c = *p;
        } else if (a == "--ect-bin-width-c") {
            auto *v = need("--ect-bin-width-c");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "coldstart-analyze: --ect-bin-width-c must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            ect_bin_width_c = *p;
        } else if (a == "--min-samples-per-bin") {
            auto *v = need("--min-samples-per-bin");
            if (!v)
                return 2;
            char *end = nullptr;
            long long const n = std::strtoll(v, &end, 10);
            if (end == v || *end != '\0' || n < 0) {
                std::fprintf(stderr,
                             "coldstart-analyze: "
                             "--min-samples-per-bin must be a "
                             "non-negative integer (got '%s')\n",
                             v);
                return 2;
            }
            min_samples_per_bin = static_cast<std::size_t>(n);
        } else if (a == "--timestamp-unit") {
            auto *v = need("--timestamp-unit");
            if (!v)
                return 2;
            ts_unit_arg = v;
        } else if (a == "--target") {
            auto *v = need("--target");
            if (!v)
                return 2;
            target_curve_arg = v;
        } else if (a == "--json") {
            json_mode = true;
        } else if (a == "--csv") {
            csv_mode = true;
        } else {
            std::fprintf(stderr, "coldstart-analyze: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (json_mode && csv_mode) {
        std::fputs("coldstart-analyze: --json and --csv are mutually exclusive\n",
                   stderr);
        return 2;
    }

    if (!log_path.has_value() || !ts_col.has_value() || !ect_col.has_value() ||
        !rpm_col.has_value()) {
        std::fputs("coldstart-analyze: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!ts_col.has_value())
            std::fputs(" --timestamp-col", stderr);
        if (!ect_col.has_value())
            std::fputs(" --ect-col", stderr);
        if (!rpm_col.has_value())
            std::fputs(" --rpm-col", stderr);
        std::fputs("\nUsage: subuwutuner-cli coldstart-analyze --log <CSV>\n"
                   "         --timestamp-col <name> --ect-col <name> --rpm-col <name>\n"
                   "         [--iat-col <name>] [--observed-lambda-col <name>]\n"
                   "         [--commanded-lambda-col <name>] [--timing-col <name>]\n"
                   "         [--closed-loop-col <name>]\n"
                   "         [--cold-threshold-c N] [--ect-bin-width-c N]\n"
                   "         [--min-samples-per-bin N]\n"
                   "         [--timestamp-unit seconds|millis|micros|rows]\n"
                   "         [--target ect:lambda,ect:lambda,...]\n",
                   stderr);
        return 2;
    }

    std::vector<std::string> header;
    {
        std::string err;
        if (!read_csv_header(*log_path, header, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    auto const resolve = [&](std::string_view name, char const *flag) -> std::size_t {
        std::size_t const idx = find_csv_column(header, name);
        if (idx == std::string_view::npos) {
            std::fprintf(stderr, "coldstart-analyze: %s column '%.*s' not in CSV header\n", flag,
                         static_cast<int>(name.size()), name.data());
        }
        return idx;
    };

    st::log::coldstart::PidMapping mapping;
    bool bad = false;
    {
        std::size_t const idx = resolve(*ts_col, "--timestamp-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.timestamp_idx = idx;
    }
    {
        std::size_t const idx = resolve(*ect_col, "--ect-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.ect_idx = idx;
    }
    {
        std::size_t const idx = resolve(*rpm_col, "--rpm-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.rpm_idx = idx;
    }
    auto const opt_resolve = [&](std::optional<std::string> const &c, std::size_t &slot,
                                 char const *flag) {
        if (!c.has_value())
            return;
        std::size_t const idx = resolve(*c, flag);
        if (idx == std::string_view::npos)
            bad = true;
        else
            slot = idx;
    };
    opt_resolve(iat_col, mapping.iat_idx, "--iat-col");
    opt_resolve(obs_col, mapping.observed_lambda_idx, "--observed-lambda-col");
    opt_resolve(cmd_col, mapping.commanded_lambda_idx, "--commanded-lambda-col");
    opt_resolve(timing_col, mapping.timing_deg_idx, "--timing-col");
    opt_resolve(cl_col, mapping.closed_loop_idx, "--closed-loop-col");
    if (bad)
        return 1;

    st::log::coldstart::WindowConfig cfg;
    if (cold_threshold_c.has_value())
        cfg.cold_threshold_c = *cold_threshold_c;
    if (ect_bin_width_c.has_value())
        cfg.ect_bin_width_c = *ect_bin_width_c;
    if (min_samples_per_bin.has_value())
        cfg.min_samples_per_bin = static_cast<std::uint32_t>(*min_samples_per_bin);
    if (ts_unit_arg.has_value()) {
        std::string u = *ts_unit_arg;
        for (auto &c : u)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (u == "seconds" || u == "s")
            cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixSeconds;
        else if (u == "millis" || u == "ms")
            cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMillis;
        else if (u == "micros" || u == "us")
            cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMicros;
        else if (u == "rows" || u == "row")
            cfg.timestamp_unit = st::log::coldstart::TimestampUnit::RowIndex;
        else {
            std::fprintf(stderr,
                         "coldstart-analyze: --timestamp-unit must be "
                         "one of seconds|millis|micros|rows (got '%s')\n",
                         ts_unit_arg->c_str());
            return 2;
        }
    }

    st::log::coldstart::Methodology methodology;
    if (target_curve_arg.has_value()) {
        for (auto const &pair_s : split_csv_list(*target_curve_arg)) {
            auto const colon = pair_s.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr,
                             "coldstart-analyze: --target entry '%s' "
                             "must be 'ect:lambda'\n",
                             pair_s.c_str());
                return 2;
            }
            auto const ect_p = parse_decimal(std::string_view{pair_s}.substr(0, colon));
            auto const lam_p = parse_decimal(std::string_view{pair_s}.substr(colon + 1));
            if (!ect_p.has_value() || !lam_p.has_value()) {
                std::fprintf(stderr,
                             "coldstart-analyze: --target entry '%s' "
                             "has non-numeric component\n",
                             pair_s.c_str());
                return 2;
            }
            methodology.target_lambda_vs_ect.points.emplace_back(*ect_p, *lam_p);
        }
        std::sort(methodology.target_lambda_vs_ect.points.begin(),
                  methodology.target_lambda_vs_ect.points.end(),
                  [](auto const &a, auto const &b) { return a.first < b.first; });
    }

    auto const r =
        st::log::coldstart::snapshot_from_csv(log_path->string(), mapping, cfg, methodology);
    if (!r.has_value()) {
        std::fprintf(stderr, "coldstart-analyze: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &s = *r;

    char const *phase_names[6] = {"PreCrank", "Cranking", "InitialFiring",
                                  "HighIdle", "Warmup",   "ClosedLoop"};

    if (json_mode) {
        // subuwutuner.coldstart-analyze.v1 — phase counts + ECT-bin
        // observed-lambda series. Same envelope shape as the other
        // *-analyze --json verbs so consumers can match on the
        // "schema" key. 256-byte buffer covers the longest snprintf
        // (verified by counting the longest format string + max
        // field widths against the format-spec output).
        std::string out{"{\"schema\":\"subuwutuner.coldstart-analyze.v1\",\"log\":"};
        json_escape(out, log_path->string());
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      ",\"coldest_ect_c\":%.3f,\"warmest_ect_c\":%.3f,"
                      "\"samples_considered\":%llu,\"samples_gated_out\":%llu,"
                      "\"mean_lambda_deviation\":%.6f,\"target_curve_set\":%s,",
                      s.coldest_ect_c, s.warmest_ect_c,
                      static_cast<unsigned long long>(s.samples_considered),
                      static_cast<unsigned long long>(s.samples_gated_out),
                      s.mean_lambda_deviation,
                      methodology.target_lambda_vs_ect.points.empty() ? "false" : "true");
        out.append(buf);
        out.append("\"phases\":[");
        for (std::size_t p = 0; p < st::log::coldstart::kPhaseCount; ++p) {
            if (p > 0) out.append(",");
            std::snprintf(buf, sizeof buf,
                          "{\"name\":\"%s\",\"samples\":%u,\"seconds\":%.3f}",
                          phase_names[p], static_cast<unsigned>(s.phase_counts[p]),
                          s.phase_seconds[p]);
            out.append(buf);
        }
        out.append("],\"ect_bins\":[");
        for (std::size_t i = 0; i < s.ect_bins.size(); ++i) {
            auto const &b = s.ect_bins[i];
            if (i > 0) out.append(",");
            std::snprintf(buf, sizeof buf,
                          "{\"ect_center_c\":%.3f,\"count\":%u,"
                          "\"observed_lambda_mean\":%.6f,"
                          "\"observed_lambda_min\":%.6f,"
                          "\"observed_lambda_max\":%.6f,"
                          "\"deviation_from_target\":%.6f}",
                          b.ect_center_c, static_cast<unsigned>(b.count),
                          b.observed_lambda_mean, b.observed_lambda_min,
                          b.observed_lambda_max, b.deviation_from_target);
            out.append(buf);
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }

    if (csv_mode) {
        // CSV shape mirrors knock-snapshot --csv: #-prefixed metadata
        // header (Pandas / Excel comment-aware) followed by the
        // per-bin observed-lambda series. Phase counts/seconds get
        // duplicated as comment lines because they're scalar metadata,
        // not a per-row series. ECT span and means stay top-of-file so
        // a `head` of the output is enough to read the run summary.
        std::printf("# schema=subuwutuner.coldstart-analyze.v1\n");
        std::printf("# log=%s\n", log_path->string().c_str());
        std::printf("# coldest_ect_c=%.3f\n", s.coldest_ect_c);
        std::printf("# warmest_ect_c=%.3f\n", s.warmest_ect_c);
        std::printf("# samples_considered=%llu\n",
                    static_cast<unsigned long long>(s.samples_considered));
        std::printf("# samples_gated_out=%llu\n",
                    static_cast<unsigned long long>(s.samples_gated_out));
        std::printf("# mean_lambda_deviation=%.6f\n", s.mean_lambda_deviation);
        std::printf("# target_curve_set=%s\n",
                    methodology.target_lambda_vs_ect.points.empty() ? "false" : "true");
        // Flat `phase_<name>_samples=` / `phase_<name>_seconds=` keys —
        // configparser / dotenv / shell-source readers all treat `[…]`
        // as a section header, which would mis-parse the bracketed
        // form. Underscores keep the round-trip clean.
        for (std::size_t p = 0; p < st::log::coldstart::kPhaseCount; ++p) {
            std::printf("# phase_%s_samples=%u\n", phase_names[p],
                        static_cast<unsigned>(s.phase_counts[p]));
            std::printf("# phase_%s_seconds=%.3f\n", phase_names[p],
                        s.phase_seconds[p]);
        }
        std::printf("ect_center_c,count,observed_lambda_mean,observed_lambda_min,"
                    "observed_lambda_max,deviation_from_target\n");
        for (auto const &b : s.ect_bins) {
            std::printf("%.3f,%u,%.6f,%.6f,%.6f,%.6f\n", b.ect_center_c,
                        static_cast<unsigned>(b.count), b.observed_lambda_mean,
                        b.observed_lambda_min, b.observed_lambda_max,
                        b.deviation_from_target);
        }
        return 0;
    }

    std::printf("Cold-Start Analysis\n");
    std::printf("===================\n");
    std::printf("Log:                   %s\n", log_path->string().c_str());
    std::printf("ECT span:              %+.1f .. %+.1f °C\n", s.coldest_ect_c, s.warmest_ect_c);
    std::printf("Samples considered:    %llu\n",
                static_cast<unsigned long long>(s.samples_considered));
    std::printf("Samples gated out:     %llu\n",
                static_cast<unsigned long long>(s.samples_gated_out));
    std::printf("Mean lambda deviation: %.4f%s\n", s.mean_lambda_deviation,
                methodology.target_lambda_vs_ect.points.empty() ? "  (no target curve set)" : "");

    std::printf("\n");
    std::printf("Phase           | samples | seconds\n");
    std::printf("----------------+---------+--------\n");
    for (std::size_t p = 0; p < st::log::coldstart::kPhaseCount; ++p) {
        std::printf(" %-14s | %7u | %7.1f\n", phase_names[p],
                    static_cast<unsigned>(s.phase_counts[p]), s.phase_seconds[p]);
    }

    if (!s.ect_bins.empty()) {
        std::printf("\n");
        std::printf("ECT bin (°C) | n  | obs λ mean | obs λ min | obs λ max | dev from target\n");
        std::printf("-------------+----+------------+-----------+-----------+---------------\n");
        for (auto const &b : s.ect_bins) {
            std::printf(" %+11.1f | %2u | %+10.4f | %+9.4f | %+9.4f | %+13.4f\n", b.ect_center_c,
                        static_cast<unsigned>(b.count), b.observed_lambda_mean,
                        b.observed_lambda_min, b.observed_lambda_max, b.deviation_from_target);
        }
    } else {
        std::printf("\n(No ECT bins — log has no cold-regime samples with "
                    "observed-lambda data above min_samples_per_bin.)\n");
    }

    return 0;
}

int cmd_ebcs_analyze(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> ts_col;
    std::optional<std::string> target_col;
    std::optional<std::string> actual_col;
    std::optional<std::string> wgdc_col;
    std::optional<std::string> throttle_col;
    std::optional<std::string> rpm_col;
    std::optional<double> throttle_step_pct;
    std::optional<double> target_step;
    std::optional<double> max_event_duration_s;
    std::optional<double> overshoot_warn_pct;
    std::optional<std::string> ts_unit_arg;
    bool verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const need = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ebcs-analyze: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--log") {
            auto *v = need("--log");
            if (!v)
                return 2;
            log_path = std::filesystem::path{v};
        } else if (a == "--timestamp-col") {
            auto *v = need("--timestamp-col");
            if (!v)
                return 2;
            ts_col = v;
        } else if (a == "--target-boost-col") {
            auto *v = need("--target-boost-col");
            if (!v)
                return 2;
            target_col = v;
        } else if (a == "--actual-boost-col") {
            auto *v = need("--actual-boost-col");
            if (!v)
                return 2;
            actual_col = v;
        } else if (a == "--wgdc-col") {
            auto *v = need("--wgdc-col");
            if (!v)
                return 2;
            wgdc_col = v;
        } else if (a == "--throttle-col") {
            auto *v = need("--throttle-col");
            if (!v)
                return 2;
            throttle_col = v;
        } else if (a == "--rpm-col") {
            auto *v = need("--rpm-col");
            if (!v)
                return 2;
            rpm_col = v;
        } else if (a == "--throttle-step-pct") {
            auto *v = need("--throttle-step-pct");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "ebcs-analyze: --throttle-step-pct must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            throttle_step_pct = *p;
        } else if (a == "--target-step") {
            auto *v = need("--target-step");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "ebcs-analyze: --target-step must be a "
                             "number (got '%s')\n",
                             v);
                return 2;
            }
            target_step = *p;
        } else if (a == "--max-event-duration") {
            auto *v = need("--max-event-duration");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "ebcs-analyze: --max-event-duration must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            max_event_duration_s = *p;
        } else if (a == "--overshoot-warn-pct") {
            auto *v = need("--overshoot-warn-pct");
            if (!v)
                return 2;
            auto const p = parse_decimal(v);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "ebcs-analyze: --overshoot-warn-pct must "
                             "be a number (got '%s')\n",
                             v);
                return 2;
            }
            overshoot_warn_pct = *p;
        } else if (a == "--timestamp-unit") {
            auto *v = need("--timestamp-unit");
            if (!v)
                return 2;
            ts_unit_arg = v;
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else {
            std::fprintf(stderr, "ebcs-analyze: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (!log_path.has_value() || !ts_col.has_value() || !target_col.has_value() ||
        !actual_col.has_value() || !throttle_col.has_value()) {
        std::fputs("ebcs-analyze: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!ts_col.has_value())
            std::fputs(" --timestamp-col", stderr);
        if (!target_col.has_value())
            std::fputs(" --target-boost-col", stderr);
        if (!actual_col.has_value())
            std::fputs(" --actual-boost-col", stderr);
        if (!throttle_col.has_value())
            std::fputs(" --throttle-col", stderr);
        std::fputs("\nUsage: subuwutuner-cli ebcs-analyze --log <CSV>\n"
                   "         --timestamp-col <name> --target-boost-col <name>\n"
                   "         --actual-boost-col <name> --throttle-col <name>\n"
                   "         [--wgdc-col <name>] [--rpm-col <name>]\n"
                   "         [--throttle-step-pct N] [--target-step N]\n"
                   "         [--max-event-duration N] [--overshoot-warn-pct N]\n"
                   "         [--timestamp-unit seconds|millis|micros|rows] [--verbose]\n",
                   stderr);
        return 2;
    }

    std::vector<std::string> header;
    {
        std::string err;
        if (!read_csv_header(*log_path, header, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    auto const resolve = [&](std::string_view name, char const *flag) -> std::size_t {
        std::size_t const idx = find_csv_column(header, name);
        if (idx == std::string_view::npos) {
            std::fprintf(stderr, "ebcs-analyze: %s column '%.*s' not in CSV header\n", flag,
                         static_cast<int>(name.size()), name.data());
        }
        return idx;
    };

    st::log::ebcs::PidMapping mapping;
    bool bad = false;
    {
        std::size_t const idx = resolve(*ts_col, "--timestamp-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.timestamp_idx = idx;
    }
    {
        std::size_t const idx = resolve(*target_col, "--target-boost-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.target_boost_idx = idx;
    }
    {
        std::size_t const idx = resolve(*actual_col, "--actual-boost-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.actual_boost_idx = idx;
    }
    {
        std::size_t const idx = resolve(*throttle_col, "--throttle-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.throttle_idx = idx;
    }
    if (wgdc_col.has_value()) {
        std::size_t const idx = resolve(*wgdc_col, "--wgdc-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.wgdc_idx = idx;
    }
    if (rpm_col.has_value()) {
        std::size_t const idx = resolve(*rpm_col, "--rpm-col");
        if (idx == std::string_view::npos)
            bad = true;
        else
            mapping.rpm_idx = idx;
    }
    if (bad)
        return 1;

    st::log::ebcs::DetectorConfig cfg;
    if (throttle_step_pct.has_value())
        cfg.throttle_step_threshold_pct = *throttle_step_pct;
    if (target_step.has_value())
        cfg.target_boost_step_threshold = *target_step;
    if (max_event_duration_s.has_value())
        cfg.max_event_duration_s = *max_event_duration_s;
    if (overshoot_warn_pct.has_value())
        cfg.overshoot_warn_pct = *overshoot_warn_pct;
    if (ts_unit_arg.has_value()) {
        std::string u = *ts_unit_arg;
        for (auto &c : u)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (u == "seconds" || u == "s")
            cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixSeconds;
        else if (u == "millis" || u == "ms")
            cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMillis;
        else if (u == "micros" || u == "us")
            cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMicros;
        else if (u == "rows" || u == "row")
            cfg.timestamp_unit = st::log::ebcs::TimestampUnit::RowIndex;
        else {
            std::fprintf(stderr,
                         "ebcs-analyze: --timestamp-unit must be one "
                         "of seconds|millis|micros|rows (got '%s')\n",
                         ts_unit_arg->c_str());
            return 2;
        }
    }

    auto const r = st::log::ebcs::snapshot_from_csv(log_path->string(), mapping, cfg);
    if (!r.has_value()) {
        std::fprintf(stderr, "ebcs-analyze: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &s = *r;

    char const *quality_names[5] = {"Good", "Choppy", "Slow", "Spiked", "Aborted"};

    std::printf("EBCS PID Assistant\n");
    std::printf("==================\n");
    std::printf("Log:                     %s\n", log_path->string().c_str());
    std::printf("Samples considered:      %llu\n",
                static_cast<unsigned long long>(s.samples_considered));
    std::printf("Events detected:         %u  (good: %u)\n",
                static_cast<unsigned>(s.total_event_count),
                static_cast<unsigned>(s.good_event_count));
    if (s.good_event_count > 0) {
        std::printf("Median rise time:        %.3f s\n", s.median_rise_time_s);
        std::printf("Median overshoot:        %.2f %%\n", s.median_overshoot_pct);
        std::printf("Median settling time:    %.3f s\n", s.median_settling_time_s);
        std::printf("Mean steady-state error: %+.3f\n", s.mean_steady_state_error);
    }

    std::printf("\nSuggestions:\n");
    for (auto const &line : s.gain_suggestions) {
        std::printf("  - %s\n", line.c_str());
    }

    if (verbose && !s.events.empty()) {
        std::printf("\nPer-event detail:\n");
        std::printf(" #  | t_start | t_end  | tgt  | peak | ss_boost | rise  | os%%  | settle | "
                    "sse    | quality\n");
        std::printf("----+---------+--------+------+------+----------+-------+-------+--------+----"
                    "----+--------\n");
        std::size_t n = 0;
        for (auto const &ev : s.events) {
            std::printf(" %2zu | %7.2f | %6.2f | %+4.1f | %+4.1f | %+8.2f | %5.2f | %5.1f | %6.2f "
                        "| %+6.2f | %s\n",
                        ++n, ev.start_timestamp, ev.end_timestamp, ev.target_boost, ev.peak_boost,
                        ev.steady_state_boost, ev.rise_time_s, ev.overshoot_pct, ev.settling_time_s,
                        ev.steady_state_error, quality_names[static_cast<std::size_t>(ev.quality)]);
        }
    }

    return 0;
}

int cmd_log(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string> pid_list_arg;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;
    bool canonical_columns = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "log: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--def") {
            if (auto const *v = require_arg("--def"); v)
                def_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--pid") {
            if (auto const *v = require_arg("--pid"); v)
                pid_list_arg = std::string{v};
            else
                return 2;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v)
                trace_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--canonical-columns") {
            canonical_columns = true;
        } else {
            std::fprintf(stderr, "log: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    // SSM PID id → autotune-canonical column name. Stable mapping
    // sourced from community PID inventories. Anything not in this
    // table keeps its original PID id as the column header.
    auto const canonical_for = [](std::string_view pid_id) -> std::string_view {
        if (pid_id == "p2")
            return "coolant_c";
        if (pid_id == "p7")
            return "throttle_pct";
        if (pid_id == "p8")
            return "rpm";
        if (pid_id == "p11")
            return "iat_c";
        if (pid_id == "p18")
            return "maf_voltage";
        return {};
    };

    if (!def_path.has_value() || !pid_list_arg.has_value() || !trace_path.has_value()) {
        std::fputs("log: missing required arguments:", stderr);
        if (!def_path.has_value())
            std::fputs(" --def", stderr);
        if (!pid_list_arg.has_value())
            std::fputs(" --pid", stderr);
        if (!trace_path.has_value())
            std::fputs(" --trace", stderr);
        std::fputs("\nUsage: subuwutuner-cli log --def <pack> --pid <id[,id...]>\n"
                   "       --trace <file> [--output <csv>]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("log", *def_path, def.error());
    }

    // Resolve each named PID to a LogChannel.
    auto const pid_ids = split_csv_list(*pid_list_arg);
    if (pid_ids.empty()) {
        std::fputs("log: --pid list is empty\n", stderr);
        return 2;
    }
    std::vector<st::log::LogChannel> channels;
    channels.reserve(pid_ids.size());
    for (auto const &id : pid_ids) {
        auto const *pid = def->find_pid(id);
        if (pid == nullptr) {
            std::fprintf(stderr, "log: pid '%s' not found in pack\n", id.c_str());
            return 1;
        }
        auto const *scaling = def->find_scaling(pid->scaling);
        std::string channel_name = pid->id;
        if (canonical_columns) {
            auto const canon = canonical_for(pid->id);
            if (!canon.empty())
                channel_name = std::string{canon};
        }
        channels.push_back(st::log::LogChannel{
            std::move(channel_name),
            static_cast<std::uint32_t>(pid->ssm_address),
            pid->data_type,
            scaling != nullptr ? std::optional<st::Scaling>{*scaling} : std::nullopt,
        });
    }

    // Load the trace and seed a MockTransport with one response per cycle.
    std::vector<std::vector<std::uint8_t>> frames;
    std::string err;
    if (!load_trace_file(*trace_path, frames, err)) {
        std::fputs(err.c_str(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    // Build the exact SSM A8 request the session will emit, since
    // MockTransport matches request bytes exactly.
    std::vector<std::uint32_t> read_addrs;
    for (auto const &ch : channels) {
        auto const bytes = st::byte_size(ch.data_type);
        for (std::size_t i = 0; i < bytes; ++i) {
            read_addrs.push_back(ch.address + static_cast<std::uint32_t>(i));
        }
    }
    auto const req = st::ecu::ssm::build_a8_request(read_addrs);
    if (!req.has_value()) {
        std::fprintf(stderr, "log: cannot build SSM request: %s\n",
                     req.error().to_string().c_str());
        return 1;
    }

    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "log: mock transport open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    for (auto const &resp : frames) {
        mock.expect_send_recv(*req, resp);
    }

    // Resolve where the CSV goes.
    std::ofstream file_out;
    std::ostream *out_stream = &std::cout;
    if (output_path.has_value()) {
        file_out.open(*output_path);
        if (!file_out) {
            std::fprintf(stderr, "log: cannot open output: %s\n", output_path->string().c_str());
            return 1;
        }
        out_stream = &file_out;
    }

    st::log::CsvSink sink{*out_stream, channels};
    sink.write_header();

    // Drive the session and drain. Once cycles_completed reaches the
    // trace size, every queued response has been consumed.
    st::log::LogSession session{mock, channels, /*ring_capacity=*/1024};
    if (auto s = session.start(); !s.has_value()) {
        std::fprintf(stderr, "log: %s\n", s.error().to_string().c_str());
        return 1;
    }

    std::int64_t ts = 0;
    std::vector<double> values(channels.size(), 0.0);
    std::uint64_t const target = frames.size();
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (session.cycles_completed() < target) {
        while (session.stream().try_pop(ts, values)) {
            sink.write_row(ts, values);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    // Final drain after the producer is done.
    while (session.stream().try_pop(ts, values)) {
        sink.write_row(ts, values);
    }
    session.stop();
    out_stream->flush();

    // %llu doesn't match std::uint64_t on Linux (where it's unsigned long, not
    // unsigned long long). PRIu64 from <cinttypes> expands to the right width
    // per platform — Clang's -Wformat enforces the match.
    std::fprintf(
        stderr, "log: cycles=%" PRIu64 "  drops=%" PRIu64 "  io_errors=%" PRIu64 "  channels=%zu\n",
        session.cycles_completed(), session.stream().dropped_count(), session.io_errors(),
        channels.size());
    return 0;
}

// Parse a fraction-or-percent string: "8%" → 0.08, "0.08" → 0.08. The
// trailing '%' is optional; whitespace around the value is tolerated.
// Returns nullopt on malformed input or non-finite results.
std::optional<double> parse_fraction_or_percent(std::string_view raw) {
    std::string s{raw};
    auto trim = [](std::string &t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) {
            t.erase(t.begin());
        }
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
            t.pop_back();
        }
    };
    trim(s);
    if (s.empty()) {
        return std::nullopt;
    }
    bool const percent = (s.back() == '%');
    if (percent) {
        s.pop_back();
        trim(s);
    }
    char *end = nullptr;
    double const v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
        return std::nullopt;
    }
    return percent ? v / 100.0 : v;
}

// Print the canonical "Lint:" section used by every `autotune *`
// subcommand. Returns 3 when `strict` is true AND any violation is
// present so the caller can use it as its exit code; returns 0
// otherwise. Centralising this keeps the cosmetic formatting and the
// strict-lint exit code in one place as more autotune commands land.
int print_lint_section(std::span<st::autotune::LintViolation const> violations, bool strict) {
    std::printf("\nLint:\n");
    if (violations.empty()) {
        std::printf("  No violations.\n");
        return 0;
    }
    std::printf("  %zu violation%s:\n", violations.size(), violations.size() == 1 ? "" : "s");
    for (auto const &v : violations) {
        std::printf("  - [%s] %s\n", st::autotune::lint_kind_name(v.kind), v.message.c_str());
    }
    return strict ? 3 : 0;
}

// Parse a plain decimal: strict strtod with no '%' acceptance. Used
// for arguments that name a physical unit (e.g. degrees) where a
// trailing '%' would be a user error to silently divide-by-100.
std::optional<double> parse_decimal(std::string_view raw) {
    std::string s{raw};
    auto trim = [](std::string &t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) {
            t.erase(t.begin());
        }
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
            t.pop_back();
        }
    };
    trim(s);
    if (s.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    double const v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

// Read a flat list of decimals from a text file. Tokens may be
// separated by commas, whitespace (including newlines), or any mix —
// the reader walks character-by-character and parses any contiguous
// numeric token. Lines starting with '#' are skipped (after leading
// whitespace). Empty file → empty vector. Used for `--axis-file`,
// `--current-file`, etc. so real-world tables (a 32-breakpoint MAF
// axis isn't fun to inline on the command line) can live in a file.
std::optional<std::vector<double>> read_decimal_list_file(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string const text = buf.str();
    std::vector<double> out;

    std::size_t i = 0;
    while (i < text.size()) {
        // Skip whitespace and commas.
        while (i < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        if (text[i] == '#') {
            // Comment to end of line.
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        // Parse a numeric token: read up to the next separator.
        std::size_t const start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
               text[i] != ',' && text[i] != '#') {
            ++i;
        }
        std::string const token{text.data() + start, i - start};
        char *end = nullptr;
        double const v = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
            return std::nullopt;
        }
        out.push_back(v);
    }
    return out;
}

// Parse a comma-separated list of doubles into a flat vector. Returns
// nullopt if any field fails to parse; an empty input gives an empty
// vector (the caller decides if that's an error).
std::optional<std::vector<double>> parse_double_list(std::string_view s) {
    std::vector<double> out;
    auto const fields = split_csv_list(s);
    out.reserve(fields.size());
    for (auto const &f : fields) {
        char *end = nullptr;
        double const v = std::strtod(f.c_str(), &end);
        if (end == f.c_str() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
            return std::nullopt;
        }
        out.push_back(v);
    }
    return out;
}

int cmd_autotune_maf(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> axis_arg;
    std::optional<std::filesystem::path> axis_file;
    std::optional<std::string> current_arg;
    std::optional<std::filesystem::path> current_file;
    std::optional<double> gain;
    std::optional<double> max_delta_pct;
    std::optional<std::size_t> min_samples;
    bool require_open_loop = false;
    bool skip_smooth = false;
    bool strict_lint = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "autotune maf: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--log") {
            if (auto const *v = require_arg("--log"); v) {
                log_path = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--axis") {
            if (auto const *v = require_arg("--axis"); v) {
                axis_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--axis-file") {
            if (auto const *v = require_arg("--axis-file"); v) {
                axis_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--current") {
            if (auto const *v = require_arg("--current"); v) {
                current_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--current-file") {
            if (auto const *v = require_arg("--current-file"); v) {
                current_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--gain") {
            if (auto const *v = require_arg("--gain"); v) {
                auto const parsed = parse_fraction_or_percent(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune maf: --gain must be a number "
                                 "(got '%s')\n",
                                 v);
                    return 2;
                }
                gain = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--max-delta" || a == "--max-delta-pct") {
            if (auto const *v = require_arg("--max-delta"); v) {
                auto const parsed = parse_fraction_or_percent(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune maf: --max-delta must be a "
                                 "number (got '%s')\n",
                                 v);
                    return 2;
                }
                max_delta_pct = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--min-samples-per-cell" || a == "--min-samples") {
            if (auto const *v = require_arg("--min-samples-per-cell"); v) {
                char *end = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune maf: --min-samples-per-cell "
                                 "must be a non-negative integer (got '%s')\n",
                                 v);
                    return 2;
                }
                min_samples = static_cast<std::size_t>(n);
            } else {
                return 2;
            }
        } else if (a == "--require-open-loop") {
            require_open_loop = true;
        } else if (a == "--no-smooth") {
            skip_smooth = true;
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else {
            std::fprintf(stderr, "autotune maf: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    bool const axis_given = axis_arg.has_value() || axis_file.has_value();
    bool const current_given = current_arg.has_value() || current_file.has_value();
    if (!log_path.has_value() || !axis_given || !current_given) {
        std::fputs("autotune maf: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!axis_given)
            std::fputs(" (--axis | --axis-file)", stderr);
        if (!current_given)
            std::fputs(" (--current | --current-file)", stderr);
        std::fputs("\nUsage: subuwutuner-cli autotune maf "
                   "--log <csv> (--axis <v,v,…> | --axis-file <path>)\n"
                   "       (--current <gs,gs,…> | --current-file <path>)\n"
                   "       [--gain 0.5] [--max-delta 8%] "
                   "[--min-samples-per-cell 50]\n"
                   "       [--require-open-loop] [--no-smooth] "
                   "[--strict-lint]\n",
                   stderr);
        return 2;
    }
    if (axis_arg.has_value() && axis_file.has_value()) {
        std::fputs("autotune maf: --axis and --axis-file are mutually exclusive\n", stderr);
        return 2;
    }
    if (current_arg.has_value() && current_file.has_value()) {
        std::fputs("autotune maf: --current and --current-file are mutually "
                   "exclusive\n",
                   stderr);
        return 2;
    }

    std::optional<std::vector<double>> axis;
    if (axis_file.has_value()) {
        axis = read_decimal_list_file(*axis_file);
        if (!axis.has_value()) {
            std::fprintf(stderr,
                         "autotune maf: cannot read --axis-file '%s' "
                         "(missing, unreadable, or contains a non-numeric "
                         "token)\n",
                         axis_file->string().c_str());
            return 1;
        }
    } else {
        axis = parse_double_list(*axis_arg);
    }
    if (!axis.has_value() || axis->empty()) {
        std::fputs("autotune maf: --axis / --axis-file must yield a non-empty "
                   "list of numbers\n",
                   stderr);
        return 2;
    }
    std::optional<std::vector<double>> current;
    if (current_file.has_value()) {
        current = read_decimal_list_file(*current_file);
        if (!current.has_value()) {
            std::fprintf(stderr,
                         "autotune maf: cannot read --current-file '%s' "
                         "(missing, unreadable, or contains a non-numeric "
                         "token)\n",
                         current_file->string().c_str());
            return 1;
        }
    } else {
        current = parse_double_list(*current_arg);
    }
    if (!current.has_value() || current->empty()) {
        std::fputs("autotune maf: --current / --current-file must yield a "
                   "non-empty list of numbers\n",
                   stderr);
        return 2;
    }
    if (axis->size() != current->size()) {
        std::fprintf(stderr,
                     "autotune maf: axis (%zu) and current (%zu) must have "
                     "the same length\n",
                     axis->size(), current->size());
        return 2;
    }

    std::ifstream in{*log_path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "autotune maf: cannot open --log '%s'\n", log_path->string().c_str());
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto const samples = st::autotune::read_maf_samples_csv(buf.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "autotune maf: %s\n", samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::MafTuneOptions opts;
    if (gain.has_value()) {
        opts.gain = *gain;
    }
    if (max_delta_pct.has_value()) {
        opts.max_delta_pct = *max_delta_pct;
    }
    if (min_samples.has_value()) {
        opts.min_samples_per_cell = *min_samples;
    }
    opts.require_open_loop = require_open_loop;

    auto const result = st::autotune::tune_maf(*axis, *current, *samples, opts);
    if (!result.has_value()) {
        std::fprintf(stderr, "autotune maf: %s\n", result.error().to_string().c_str());
        return 1;
    }

    auto const &final_result =
        skip_smooth ? *result : st::autotune::smooth_proposals(*result, opts.max_delta_pct);

    // Summary header — mirrors docs/12 §"Output".
    std::printf("Loaded %zu samples from %s\n", final_result.total_samples,
                log_path->string().c_str());
    double const retained_pct = final_result.total_samples == 0
                                    ? 0.0
                                    : 100.0 *
                                          static_cast<double>(final_result.samples_after_gates) /
                                          static_cast<double>(final_result.total_samples);
    std::printf("After quality gates: %zu samples (%.1f%% retained)\n\n",
                final_result.samples_after_gates, retained_pct);

    // Aggregate stats. After smoothing, a cell can have a non-zero
    // post-smooth delta from neighbor pull even when its own
    // samples_used < min_samples_per_cell — so `modified` counts every
    // cell that moved (regardless of why), and `underpowered` reports
    // separately how many cells lacked direct data of their own.
    constexpr double kModifiedEpsilon = 1e-9;
    std::size_t modified = 0;
    std::size_t underpowered = 0;
    double sum_delta_pct = 0.0;
    double max_delta_signed = 0.0;
    double min_delta_signed = 0.0;
    std::size_t max_cell_idx = 0;
    std::size_t min_cell_idx = 0;
    for (auto const &c : final_result.cells) {
        if (c.samples_used < opts.min_samples_per_cell) {
            ++underpowered;
        }
        double const delta_pct =
            c.current_value == 0.0 ? 0.0 : (c.proposed_value / c.current_value) - 1.0;
        if (std::abs(delta_pct) > kModifiedEpsilon) {
            ++modified;
            sum_delta_pct += delta_pct;
        }
        if (delta_pct > max_delta_signed) {
            max_delta_signed = delta_pct;
            max_cell_idx = c.cell_index;
        }
        if (delta_pct < min_delta_signed) {
            min_delta_signed = delta_pct;
            min_cell_idx = c.cell_index;
        }
    }

    std::printf("MAF scaling proposal:\n");
    std::printf("  Cells modified:        %zu / %zu\n", modified, final_result.cells.size());
    std::printf("  Underpowered cells:    %zu (<%zu direct samples; any "
                "delta is from neighbor smoothing)\n",
                underpowered, opts.min_samples_per_cell);
    if (modified > 0) {
        std::printf("  Mean delta:            %+.2f%%\n",
                    100.0 * sum_delta_pct / static_cast<double>(modified));
    }
    if (max_delta_signed > 0.0) {
        auto const &c = final_result.cells[max_cell_idx];
        std::printf("  Max delta:             %+.2f%% at v=%.2f (n=%zu)\n",
                    100.0 * max_delta_signed, (*axis)[max_cell_idx], c.samples_used);
    }
    if (min_delta_signed < 0.0) {
        auto const &c = final_result.cells[min_cell_idx];
        std::printf("  Min delta:             %+.2f%% at v=%.2f (n=%zu)\n",
                    100.0 * min_delta_signed, (*axis)[min_cell_idx], c.samples_used);
    }
    std::printf("\n");

    // Per-cell ledger — small N (the MAF axis has a couple of dozen
    // breakpoints) so we can afford one row per cell.
    std::printf("  v (V) |  current  | proposed  |   delta   |   n   | confidence\n");
    std::printf("  ------+-----------+-----------+-----------+-------+-----------\n");
    for (std::size_t i = 0; i < final_result.cells.size(); ++i) {
        auto const &c = final_result.cells[i];
        double const delta_pct =
            c.current_value == 0.0 ? 0.0 : (c.proposed_value / c.current_value) - 1.0;
        std::printf("  %5.2f | %9.4f | %9.4f | %+8.2f%% | %5zu | %6.2f\n", (*axis)[i],
                    c.current_value, c.proposed_value, 100.0 * delta_pct, c.samples_used,
                    c.confidence);
    }

    // Engine-safety lint per docs/12 §"Engine-safety linting". Always
    // runs — the proposal isn't auto-applied, so the user reviews the
    // findings and decides. With --strict-lint, exit non-zero when any
    // violation is present so this command can gate a downstream
    // --apply (when that lands in the project-integration slice).
    auto const violations = st::autotune::lint_maf_proposal(*axis, *current, final_result);
    return print_lint_section(violations, strict_lint);
}

int cmd_autotune_knock_pull(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string> rpm_axis_arg;
    std::optional<std::filesystem::path> rpm_axis_file;
    std::optional<std::string> load_axis_arg;
    std::optional<std::filesystem::path> load_axis_file;
    std::optional<std::string> current_arg;
    std::optional<std::filesystem::path> current_file;
    std::optional<double> trigger_deg;
    std::optional<double> pull_step_deg;
    std::optional<std::size_t> min_samples;
    std::optional<double> max_neighbor_step;
    bool strict_lint = false;
    bool enable_add_back = false;
    std::optional<double> add_back_step;
    std::optional<std::size_t> add_back_min_clean;
    std::optional<double> add_back_threshold;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "autotune knock-pull: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--log") {
            if (auto const *v = require_arg("--log"); v) {
                log_path = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--rpm-axis") {
            if (auto const *v = require_arg("--rpm-axis"); v) {
                rpm_axis_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--rpm-axis-file") {
            if (auto const *v = require_arg("--rpm-axis-file"); v) {
                rpm_axis_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--load-axis") {
            if (auto const *v = require_arg("--load-axis"); v) {
                load_axis_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--load-axis-file") {
            if (auto const *v = require_arg("--load-axis-file"); v) {
                load_axis_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--current-timing" || a == "--current") {
            if (auto const *v = require_arg("--current-timing"); v) {
                current_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--current-timing-file" || a == "--current-file") {
            if (auto const *v = require_arg("--current-timing-file"); v) {
                current_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--trigger-degrees" || a == "--trigger") {
            if (auto const *v = require_arg("--trigger-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --trigger-degrees must "
                                 "be a decimal number in degrees (got '%s')\n",
                                 v);
                    return 2;
                }
                trigger_deg = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--pull-step-degrees" || a == "--pull-step") {
            if (auto const *v = require_arg("--pull-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --pull-step-degrees "
                                 "must be a decimal number in degrees "
                                 "(got '%s')\n",
                                 v);
                    return 2;
                }
                pull_step_deg = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--min-samples-per-cell" || a == "--min-samples") {
            if (auto const *v = require_arg("--min-samples-per-cell"); v) {
                char *end = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --min-samples-per-cell "
                                 "must be a non-negative integer (got '%s')\n",
                                 v);
                    return 2;
                }
                min_samples = static_cast<std::size_t>(n);
            } else {
                return 2;
            }
        } else if (a == "--max-neighbor-step-degrees" || a == "--max-neighbor-step") {
            if (auto const *v = require_arg("--max-neighbor-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--max-neighbor-step-degrees must be a "
                                 "non-negative decimal (got '%s')\n",
                                 v);
                    return 2;
                }
                max_neighbor_step = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else if (a == "--enable-add-back") {
            enable_add_back = true;
        } else if (a == "--add-back-step-degrees" || a == "--add-back-step") {
            if (auto const *v = require_arg("--add-back-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-step-degrees must be a "
                                 "non-negative decimal (got '%s')\n",
                                 v);
                    return 2;
                }
                add_back_step = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--add-back-min-clean-samples" || a == "--add-back-min-samples") {
            if (auto const *v = require_arg("--add-back-min-clean-samples"); v) {
                char *end = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-min-clean-samples must be a "
                                 "non-negative integer (got '%s')\n",
                                 v);
                    return 2;
                }
                add_back_min_clean = static_cast<std::size_t>(n);
            } else {
                return 2;
            }
        } else if (a == "--add-back-clean-threshold-degrees" || a == "--add-back-threshold") {
            if (auto const *v = require_arg("--add-back-clean-threshold-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-clean-threshold-degrees must "
                                 "be a non-negative decimal (got '%s')\n",
                                 v);
                    return 2;
                }
                add_back_threshold = *parsed;
            } else {
                return 2;
            }
        } else {
            std::fprintf(stderr, "autotune knock-pull: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    bool const rpm_given = rpm_axis_arg.has_value() || rpm_axis_file.has_value();
    bool const load_given = load_axis_arg.has_value() || load_axis_file.has_value();
    bool const current_given = current_arg.has_value() || current_file.has_value();
    if (!log_path.has_value() || !rpm_given || !load_given || !current_given) {
        std::fputs("autotune knock-pull: missing required arguments:", stderr);
        if (!log_path.has_value())
            std::fputs(" --log", stderr);
        if (!rpm_given)
            std::fputs(" (--rpm-axis | --rpm-axis-file)", stderr);
        if (!load_given)
            std::fputs(" (--load-axis | --load-axis-file)", stderr);
        if (!current_given)
            std::fputs(" (--current-timing | --current-timing-file)", stderr);
        std::fputs("\nUsage: subuwutuner-cli autotune knock-pull "
                   "--log <csv>\n"
                   "       (--rpm-axis <r,r,…> | --rpm-axis-file <path>)\n"
                   "       (--load-axis <l,l,…> | --load-axis-file <path>)\n"
                   "       (--current-timing <d,d,…> | --current-timing-file <path>)"
                   "   (flat row-major, rows × cols = load × rpm)\n"
                   "       [--trigger-degrees 1.5] [--pull-step-degrees 0.75]\n"
                   "       [--min-samples-per-cell 30] "
                   "[--max-neighbor-step-degrees 3.0]\n"
                   "       [--strict-lint]\n",
                   stderr);
        return 2;
    }
    auto inline_and_file_exclusive = [](char const *flag_inline, char const *flag_file,
                                        bool has_inline, bool has_file) {
        if (has_inline && has_file) {
            std::fprintf(stderr, "autotune knock-pull: %s and %s are mutually exclusive\n",
                         flag_inline, flag_file);
            return true;
        }
        return false;
    };
    if (inline_and_file_exclusive("--rpm-axis", "--rpm-axis-file", rpm_axis_arg.has_value(),
                                  rpm_axis_file.has_value())) {
        return 2;
    }
    if (inline_and_file_exclusive("--load-axis", "--load-axis-file", load_axis_arg.has_value(),
                                  load_axis_file.has_value())) {
        return 2;
    }
    if (inline_and_file_exclusive("--current-timing", "--current-timing-file",
                                  current_arg.has_value(), current_file.has_value())) {
        return 2;
    }

    // Returns 0 on success and fills `out`; returns 1 if the file
    // path was given but couldn't be read (already printed by us — rc=1
    // mirrors MAF's "I/O failed" exit code); returns 2 if the list was
    // empty or the inline parser rejected the input (rc=2 mirrors MAF's
    // "argument was malformed" exit code).
    auto load_list = [](char const *flag_file, char const *list_label,
                        std::optional<std::string> const &inline_arg,
                        std::optional<std::filesystem::path> const &file_arg,
                        std::vector<double> &out) -> int {
        std::optional<std::vector<double>> v;
        if (file_arg.has_value()) {
            v = read_decimal_list_file(*file_arg);
            if (!v.has_value()) {
                std::fprintf(stderr,
                             "autotune knock-pull: cannot read %s '%s' "
                             "(missing, unreadable, or contains a non-numeric "
                             "token)\n",
                             flag_file, file_arg->string().c_str());
                return 1;
            }
        } else {
            v = parse_double_list(*inline_arg);
        }
        if (!v.has_value() || v->empty()) {
            std::fprintf(stderr,
                         "autotune knock-pull: %s must yield a non-empty "
                         "list of numbers\n",
                         list_label);
            return 2;
        }
        out = std::move(*v);
        return 0;
    };

    std::vector<double> rpm_axis;
    if (int const rc =
            load_list("--rpm-axis-file", "rpm-axis", rpm_axis_arg, rpm_axis_file, rpm_axis);
        rc != 0) {
        return rc;
    }
    std::vector<double> load_axis;
    if (int const rc =
            load_list("--load-axis-file", "load-axis", load_axis_arg, load_axis_file, load_axis);
        rc != 0) {
        return rc;
    }
    std::vector<double> current;
    if (int const rc = load_list("--current-timing-file", "current-timing", current_arg,
                                 current_file, current);
        rc != 0) {
        return rc;
    }
    if (current.size() != rpm_axis.size() * load_axis.size()) {
        std::fprintf(stderr,
                     "autotune knock-pull: --current-timing length (%zu) must "
                     "equal --rpm-axis (%zu) × --load-axis (%zu) = %zu\n",
                     current.size(), rpm_axis.size(), load_axis.size(),
                     rpm_axis.size() * load_axis.size());
        return 2;
    }

    std::ifstream in{*log_path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "autotune knock-pull: cannot open --log '%s'\n",
                     log_path->string().c_str());
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto const samples = st::autotune::read_knock_samples_csv(buf.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "autotune knock-pull: %s\n", samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::KnockPullOptions opts;
    if (trigger_deg.has_value()) {
        opts.trigger_degrees = *trigger_deg;
    }
    if (pull_step_deg.has_value()) {
        opts.pull_step_degrees = *pull_step_deg;
    }
    if (min_samples.has_value()) {
        opts.min_samples_per_cell = *min_samples;
    }

    auto const pull_result =
        st::autotune::tune_knock_pull(rpm_axis, load_axis, current, *samples, opts);
    if (!pull_result.has_value()) {
        std::fprintf(stderr, "autotune knock-pull: %s\n", pull_result.error().to_string().c_str());
        return 1;
    }

    // Opt-in add-back pass per docs/12. The pull pass is always run;
    // the add-back pass only runs when --enable-add-back is set,
    // matching the docs default-off posture for the dangerous direction
    // (adding timing). `apply_knock_add_back` is a no-op when
    // `opts.enabled` is false, so always calling it keeps the gate in
    // one place (the kernel) rather than splitting it across CLI + lib.
    st::autotune::KnockAddBackOptions add_opts;
    add_opts.enabled = enable_add_back;
    if (add_back_step.has_value()) {
        add_opts.add_step_degrees = *add_back_step;
    }
    if (add_back_min_clean.has_value()) {
        add_opts.min_clean_samples_per_cell = *add_back_min_clean;
    }
    if (add_back_threshold.has_value()) {
        add_opts.clean_threshold_degrees = *add_back_threshold;
    }
    auto const result = st::autotune::apply_knock_add_back(*pull_result, add_opts);

    std::printf("Loaded %zu samples from %s\n", result.total_samples, log_path->string().c_str());
    double const retained_pct = result.total_samples == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(result.samples_after_gates) /
                                          static_cast<double>(result.total_samples);
    std::printf("After quality gates: %zu samples (%.1f%% retained)\n\n",
                result.samples_after_gates, retained_pct);

    // Count pulled vs added-back cells. A cell is "added back" when it
    // wasn't pulled but its proposed_value differs from current_value
    // — that's the post-pass marker apply_knock_add_back leaves.
    std::size_t pulled = 0;
    std::size_t added_back = 0;
    double max_pull_deg = 0.0;
    std::size_t max_pull_cell = 0;
    double max_add_deg = 0.0;
    std::size_t max_add_cell = 0;
    for (auto const &c : result.cells) {
        if (c.pulled) {
            ++pulled;
            double const drop = c.current_value - c.proposed_value;
            if (drop > max_pull_deg) {
                max_pull_deg = drop;
                max_pull_cell = c.cell_index;
            }
        } else if (c.proposed_value > c.current_value) {
            ++added_back;
            double const gain = c.proposed_value - c.current_value;
            if (gain > max_add_deg) {
                max_add_deg = gain;
                max_add_cell = c.cell_index;
            }
        }
    }
    std::printf("Knock pull proposal:\n");
    std::printf("  Cells pulled:          %zu / %zu\n", pulled, result.cells.size());
    std::printf("  Pull step:             %.2f°\n", opts.pull_step_degrees);
    std::printf("  Trigger threshold:     mean feedback knock < -%.2f° "
                "(over ≥ %zu samples)\n",
                opts.trigger_degrees, opts.min_samples_per_cell);
    if (pulled > 0) {
        std::size_t const row = max_pull_cell / rpm_axis.size();
        std::size_t const col = max_pull_cell % rpm_axis.size();
        std::printf("  Worst cell:            load=%.2f rpm=%.0f "
                    "pulled %.2f°\n",
                    load_axis[row], rpm_axis[col], max_pull_deg);
    }
    if (enable_add_back) {
        std::printf("  Cells added back:      %zu (clean cells, +%.2f°)\n", added_back,
                    add_opts.add_step_degrees);
        if (added_back > 0) {
            std::size_t const row = max_add_cell / rpm_axis.size();
            std::size_t const col = max_add_cell % rpm_axis.size();
            std::printf("  Largest add-back:      load=%.2f rpm=%.0f "
                        "+%.2f°\n",
                        load_axis[row], rpm_axis[col], max_add_deg);
        }
    }
    std::printf("\n");

    // 2D ledger: one row per load × one cell-pair per RPM. Compact when
    // the table is small (the docs/12 timing maps are typically 8×8 to
    // 16×16, which fits on a terminal line). Added-back cells use the
    // same signed-degree format as pulled cells — a leading '+' makes
    // it obvious which direction the cell moved.
    std::printf("  load \\ rpm");
    for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
        std::printf(" | %7.0f", rpm_axis[c]);
    }
    std::printf("\n  -----------");
    for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
        std::printf("-+--------");
    }
    std::printf("\n");
    for (std::size_t r = 0; r < load_axis.size(); ++r) {
        std::printf("  %9.2f ", load_axis[r]);
        for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
            auto const &cell = result.cells[r * rpm_axis.size() + c];
            double const delta = cell.proposed_value - cell.current_value;
            if (cell.pulled || delta != 0.0) {
                std::printf("|  %+5.2f ", delta);
            } else {
                std::printf("|    .   ");
            }
        }
        std::printf("\n");
    }

    // Engine-safety lint per docs/12 §"Engine-safety linting" for the
    // 2D timing surface. Always runs; --strict-lint exits 3 on any
    // violation. Same exit-code convention as `autotune maf`.
    st::autotune::KnockLintOptions lint_opts;
    if (max_neighbor_step.has_value()) {
        lint_opts.max_neighbor_step_degrees = *max_neighbor_step;
    }
    auto const violations =
        st::autotune::lint_knock_proposal(rpm_axis, load_axis, result, lint_opts);
    return print_lint_section(violations, strict_lint);
}

// Helper for `can-*`: emit one column-aligned line for a (bus, id) summary.
void print_id_row(st::can::BusId bus, std::uint32_t can_id, bool extended, std::size_t count,
                  double rate_hz, double avg_dlc) {
    std::printf("  bus=%u  id=0x%0*X%s  frames=%6zu  rate=%8.2f Hz  dlc~%.1f\n",
                static_cast<unsigned>(bus), extended ? 8 : 3, can_id, extended ? "" : "  ", count,
                rate_hz, avg_dlc);
}

int cmd_can_replay(int argc, char *argv[]) {
    std::optional<std::filesystem::path> trace_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "can-replay: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!trace_path.has_value()) {
            trace_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-replay: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_path.has_value()) {
        std::fputs("can-replay: missing required argument\n"
                   "Usage: subuwutuner-cli can-replay <FILE.asc>\n",
                   stderr);
        return 2;
    }

    auto const frames = st::can::read_asc(*trace_path);
    if (!frames.has_value()) {
        std::fprintf(stderr, "can-replay: %s\n", frames.error().to_string().c_str());
        return 1;
    }
    if (frames->empty()) {
        std::printf("File: %s\nNo frames.\n", trace_path->string().c_str());
        return 0;
    }

    // Per-id aggregates. Key = (bus << 32) | id; preserve insertion order
    // for stable output via a parallel vector of keys.
    struct Agg {
        st::can::BusId bus{st::can::BusId::Hs};
        std::uint32_t id{0};
        bool extended{false};
        std::size_t count{0};
        std::uint64_t dlc_sum{0};
        std::array<std::size_t, 256> byte0_hist{};
        std::int64_t first_ts_ns{0};
        std::int64_t last_ts_ns{0};
    };
    std::unordered_map<std::uint64_t, Agg> aggs;
    std::vector<std::uint64_t> order;

    std::int64_t global_first = frames->front().timestamp_ns;
    std::int64_t global_last = frames->front().timestamp_ns;
    for (auto const &f : *frames) {
        std::uint64_t const key = (static_cast<std::uint64_t>(f.bus) << 32) | f.id;
        auto [it, inserted] = aggs.try_emplace(key);
        if (inserted) {
            it->second.bus = f.bus;
            it->second.id = f.id;
            it->second.extended = f.extended;
            it->second.first_ts_ns = f.timestamp_ns;
            order.push_back(key);
        }
        auto &agg = it->second;
        ++agg.count;
        agg.dlc_sum += f.dlc;
        agg.last_ts_ns = f.timestamp_ns;
        if (f.dlc > 0) {
            ++agg.byte0_hist[f.data[0]];
        }
        if (f.timestamp_ns < global_first)
            global_first = f.timestamp_ns;
        if (f.timestamp_ns > global_last)
            global_last = f.timestamp_ns;
    }

    double const duration_s = static_cast<double>(global_last - global_first) / 1e9;

    std::printf("File: %s\n", trace_path->string().c_str());
    std::printf("Frames: %zu   unique ids: %zu   duration: %.3f s\n", frames->size(), aggs.size(),
                duration_s);

    // Sort keys by descending frame count for readability.
    std::sort(order.begin(), order.end(),
              [&](std::uint64_t a, std::uint64_t b) { return aggs[a].count > aggs[b].count; });

    std::printf("\nPer-id summary (sorted by count):\n");
    for (auto const key : order) {
        auto const &a = aggs[key];
        double const rate = duration_s > 0.0 ? static_cast<double>(a.count) / duration_s : 0.0;
        double const avg_dlc =
            a.count > 0 ? static_cast<double>(a.dlc_sum) / static_cast<double>(a.count) : 0.0;
        print_id_row(a.bus, a.id, a.extended, a.count, rate, avg_dlc);

        // Modal first-byte value, if any payload was seen.
        std::size_t mode_count = 0;
        unsigned mode_byte = 0;
        for (std::size_t v = 0; v < 256; ++v) {
            if (a.byte0_hist[v] > mode_count) {
                mode_count = a.byte0_hist[v];
                mode_byte = static_cast<unsigned>(v);
            }
        }
        if (mode_count > 0) {
            double const share = static_cast<double>(mode_count) / static_cast<double>(a.count);
            std::printf("        byte0 mode=0x%02X (%.0f%% of frames)\n", mode_byte, share * 100.0);
        }
    }
    return 0;
}

int cmd_can_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> a_path;
    std::optional<std::filesystem::path> b_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "can-diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!a_path.has_value()) {
            a_path = std::filesystem::path{argv[i]};
        } else if (!b_path.has_value()) {
            b_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!a_path.has_value() || !b_path.has_value()) {
        std::fputs("can-diff: missing required arguments:", stderr);
        if (!a_path.has_value())
            std::fputs(" <A.asc>", stderr);
        if (!b_path.has_value())
            std::fputs(" <B.asc>", stderr);
        std::fputs("\nUsage: subuwutuner-cli can-diff <A.asc> <B.asc>\n", stderr);
        return 2;
    }

    auto const fa = st::can::read_asc(*a_path);
    if (!fa.has_value()) {
        std::fprintf(stderr, "can-diff: %s: %s\n", a_path->string().c_str(),
                     fa.error().to_string().c_str());
        return 1;
    }
    auto const fb = st::can::read_asc(*b_path);
    if (!fb.has_value()) {
        std::fprintf(stderr, "can-diff: %s: %s\n", b_path->string().c_str(),
                     fb.error().to_string().c_str());
        return 1;
    }

    struct Entry {
        st::can::BusId bus{st::can::BusId::Hs};
        std::uint32_t id{0};
        bool extended{false};
        std::size_t count{0};
    };
    auto tally = [](std::vector<st::can::Frame> const &frames) {
        std::unordered_map<std::uint64_t, Entry> m;
        for (auto const &f : frames) {
            std::uint64_t const key = (static_cast<std::uint64_t>(f.bus) << 32) | f.id;
            auto [it, inserted] = m.try_emplace(key);
            if (inserted) {
                it->second.bus = f.bus;
                it->second.id = f.id;
                it->second.extended = f.extended;
            }
            ++it->second.count;
        }
        return m;
    };

    auto const ma = tally(*fa);
    auto const mb = tally(*fb);

    std::printf("A: %s   (%zu frames, %zu ids)\n", a_path->string().c_str(), fa->size(), ma.size());
    std::printf("B: %s   (%zu frames, %zu ids)\n", b_path->string().c_str(), fb->size(), mb.size());

    auto sorted_keys = [](auto const &m) {
        std::vector<std::uint64_t> keys;
        keys.reserve(m.size());
        for (auto const &kv : m)
            keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    };

    std::vector<std::uint64_t> only_a;
    std::vector<std::uint64_t> only_b;
    std::vector<std::uint64_t> shared;
    for (auto const key : sorted_keys(ma)) {
        if (mb.find(key) == mb.end())
            only_a.push_back(key);
        else
            shared.push_back(key);
    }
    for (auto const key : sorted_keys(mb)) {
        if (ma.find(key) == ma.end())
            only_b.push_back(key);
    }

    std::printf("\nIds only in A (%zu):\n", only_a.size());
    for (auto const key : only_a) {
        auto const &e = ma.at(key);
        std::printf("  bus=%u  id=0x%0*X  count=%zu\n", static_cast<unsigned>(e.bus),
                    e.extended ? 8 : 3, e.id, e.count);
    }
    std::printf("\nIds only in B (%zu):\n", only_b.size());
    for (auto const key : only_b) {
        auto const &e = mb.at(key);
        std::printf("  bus=%u  id=0x%0*X  count=%zu\n", static_cast<unsigned>(e.bus),
                    e.extended ? 8 : 3, e.id, e.count);
    }

    std::size_t shared_changed = 0;
    for (auto const key : shared) {
        if (ma.at(key).count != mb.at(key).count)
            ++shared_changed;
    }
    std::printf("\nShared ids with count delta (%zu of %zu shared):\n", shared_changed,
                shared.size());
    for (auto const key : shared) {
        auto const &ea = ma.at(key);
        auto const &eb = mb.at(key);
        if (ea.count == eb.count)
            continue;
        long long const delta = static_cast<long long>(eb.count) - static_cast<long long>(ea.count);
        std::printf("  bus=%u  id=0x%0*X  A=%zu  B=%zu  delta=%+lld\n",
                    static_cast<unsigned>(ea.bus), ea.extended ? 8 : 3, ea.id, ea.count, eb.count,
                    delta);
    }
    return 0;
}

int cmd_can_discover(int argc, char *argv[]) {
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;
    double baseline_secs = 10.0;
    std::optional<unsigned> bus_filter;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "can-discover: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--from") {
            if (auto const *v = require_arg("--from"); v)
                trace_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--baseline") {
            auto const *v = require_arg("--baseline");
            if (v == nullptr)
                return 2;
            std::string_view sv{v};
            // Accept "10", "10s" — strip trailing 's' for ergonomics.
            if (!sv.empty() && (sv.back() == 's' || sv.back() == 'S')) {
                sv.remove_suffix(1);
            }
            double secs = 0.0;
            char *end = nullptr;
            std::string tmp{sv};
            secs = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || secs <= 0.0) {
                std::fprintf(stderr,
                             "can-discover: --baseline must be a positive number of seconds\n");
                return 2;
            }
            baseline_secs = secs;
        } else if (a == "--bus") {
            auto const *v = require_arg("--bus");
            if (v == nullptr)
                return 2;
            unsigned value = 0;
            auto const res = std::from_chars(v, v + std::strlen(v), value);
            if (res.ec != std::errc{} || value > 3) {
                std::fprintf(stderr, "can-discover: --bus must be 0..3\n");
                return 2;
            }
            bus_filter = value;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-discover: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "can-discover: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_path.has_value()) {
        std::fputs("can-discover: missing required argument\n"
                   "Usage: subuwutuner-cli can-discover --from <FILE.asc> [--baseline <secs>]\n"
                   "       [--bus <0..3>] [--output <session.cdb>]\n",
                   stderr);
        return 2;
    }

    auto const all_frames = st::can::read_asc(*trace_path);
    if (!all_frames.has_value()) {
        std::fprintf(stderr, "can-discover: %s\n", all_frames.error().to_string().c_str());
        return 1;
    }
    if (all_frames->empty()) {
        std::fputs("can-discover: trace contains no frames\n", stderr);
        return 1;
    }

    // Optional bus filter: keep only frames on the requested bus, in order.
    std::vector<st::can::Frame> frames;
    frames.reserve(all_frames->size());
    if (bus_filter.has_value()) {
        auto const want = static_cast<st::can::BusId>(*bus_filter);
        for (auto const &f : *all_frames) {
            if (f.bus == want)
                frames.push_back(f);
        }
        if (frames.empty()) {
            std::fprintf(stderr, "can-discover: no frames on bus %u\n", *bus_filter);
            return 1;
        }
    } else {
        frames = *all_frames;
    }

    std::int64_t const t0 = frames.front().timestamp_ns;
    std::int64_t const baseline_ns = static_cast<std::int64_t>(baseline_secs * 1e9);
    std::int64_t const split_ns = t0 + baseline_ns;

    // Partition into baseline (timestamp < split) and watch (>= split).
    std::vector<st::can::Frame> baseline_frames;
    std::vector<st::can::Frame> watch_frames;
    baseline_frames.reserve(frames.size());
    for (auto const &f : frames) {
        if (f.timestamp_ns < split_ns)
            baseline_frames.push_back(f);
        else
            watch_frames.push_back(f);
    }
    if (baseline_frames.empty()) {
        std::fputs("can-discover: baseline window is empty (trace shorter than --baseline?)\n",
                   stderr);
        return 1;
    }

    st::discover::BaselineModel model = st::discover::build_baseline(baseline_frames);

    auto const events = st::discover::detect_changes(model, watch_frames);

    st::discover::Bundle bundle;
    bundle.schema_version = 1;
    bundle.captured_at = "";
    bundle.bus_label =
        bus_filter.has_value() ? std::string{"bus"} + std::to_string(*bus_filter) : std::string{};
    bundle.baseline_ns = baseline_ns;
    bundle.baseline_entries = model.entries();
    bundle.events = events;

    if (output_path.has_value()) {
        if (auto s = st::discover::write_cdb(*output_path, bundle); !s.has_value()) {
            std::fprintf(stderr, "can-discover: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "can-discover: baseline ids=%zu  events=%zu  wrote %s\n",
                     bundle.baseline_entries.size(), bundle.events.size(),
                     output_path->string().c_str());
    } else {
        auto const text = st::discover::write_cdb_string(bundle);
        if (!text.has_value()) {
            std::fprintf(stderr, "can-discover: %s\n", text.error().to_string().c_str());
            return 1;
        }
        std::fputs(text->c_str(), stdout);
        std::fprintf(stderr, "can-discover: baseline ids=%zu  events=%zu\n",
                     bundle.baseline_entries.size(), bundle.events.size());
    }
    return 0;
}

// Convert free-form description text into a DBC-safe identifier:
// lowercase, [a-z0-9_] only, leading digit prefixed with underscore,
// empty input mapped to "signal".
std::string slugify(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (auto const c : text) {
        unsigned char const u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_') {
            out.push_back(static_cast<char>(u));
        } else if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(u - 'A' + 'a'));
        } else {
            if (!out.empty() && out.back() != '_')
                out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "signal";
    if (out.front() >= '0' && out.front() <= '9')
        out.insert(out.begin(), '_');
    return out;
}

int cmd_can_export_dbc(int argc, char *argv[]) {
    std::optional<std::filesystem::path> cdb_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("can-export-dbc: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-export-dbc: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!cdb_path.has_value()) {
            cdb_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-export-dbc: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!cdb_path.has_value()) {
        std::fputs("can-export-dbc: missing required argument\n"
                   "Usage: subuwutuner-cli can-export-dbc <session.cdb> [--output <draft.dbc>]\n",
                   stderr);
        return 2;
    }

    auto const bundle = st::discover::read_cdb(*cdb_path);
    if (!bundle.has_value()) {
        std::fprintf(stderr, "can-export-dbc: %s\n", bundle.error().to_string().c_str());
        return 1;
    }

    st::dbc::Database db;
    db.version = "";
    db.nodes.push_back("Vector__XXX");

    // Build one Message per (bus,id) appearing in the baseline. Index by
    // 64-bit key so signals can be attached to the right Message later.
    std::unordered_map<std::uint64_t, std::size_t> idx_by_key;
    for (auto const &b : bundle->baseline_entries) {
        std::uint64_t const key = (static_cast<std::uint64_t>(b.bus) << 32) | b.can_id;
        st::dbc::Message m;
        m.id = b.can_id;
        m.extended = b.extended;
        char buf[32];
        std::snprintf(buf, sizeof buf, "MSG_%X", b.can_id);
        m.name = buf;
        m.length = b.dlc;
        m.sender = "Vector__XXX";
        idx_by_key[key] = db.messages.size();
        db.messages.push_back(std::move(m));
    }

    // Each labeled Change event becomes a draft SG_ on its message.
    // Conventions per docs/14: identity scaling, Motorola/big-endian,
    // unsigned. Bit position = first changed byte * 8; length covers
    // the contiguous run min..max byte index.
    std::size_t signals_added = 0;
    std::size_t orphan_signals = 0; // event on an id missing from baseline
    std::size_t skipped_unlabeled = 0;
    std::size_t skipped_new_id = 0;
    std::unordered_map<std::size_t, std::unordered_map<std::string, int>> name_dedup;

    for (auto const &ev : bundle->events) {
        if (ev.kind == st::discover::DiscoveryEvent::Kind::NewId) {
            ++skipped_new_id;
            continue;
        }
        if (ev.description.empty() || ev.changed_byte_indices.empty()) {
            ++skipped_unlabeled;
            continue;
        }
        std::uint64_t const key = (static_cast<std::uint64_t>(ev.bus) << 32) | ev.can_id;
        auto const it = idx_by_key.find(key);
        if (it == idx_by_key.end()) {
            ++orphan_signals;
            continue;
        }
        auto &msg = db.messages[it->second];

        std::uint8_t lo = ev.changed_byte_indices.front();
        std::uint8_t hi = lo;
        for (auto const b : ev.changed_byte_indices) {
            if (b < lo)
                lo = b;
            if (b > hi)
                hi = b;
        }

        st::dbc::Signal sig;
        std::string base = slugify(ev.description);
        // Disambiguate signals that slugify to the same identifier
        // within the same message.
        auto &seen = name_dedup[it->second];
        int &n = seen[base];
        sig.name = (n == 0) ? base : base + "_" + std::to_string(n + 1);
        ++n;
        sig.start_bit = static_cast<std::size_t>(lo) * 8U;
        sig.length_bits = static_cast<std::size_t>(hi - lo + 1) * 8U;
        sig.byte_order = st::dbc::ByteOrder::Motorola;
        sig.sign = st::dbc::SignKind::Unsigned;
        sig.factor = 1.0;
        sig.offset = 0.0;
        sig.min_value = 0.0;
        sig.max_value = sig.length_bits >= 64
                            ? static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                            : static_cast<double>((1ULL << sig.length_bits) - 1);
        sig.unit = "";
        sig.receivers.push_back("Vector__XXX");
        msg.signals.push_back(std::move(sig));
        ++signals_added;
    }

    auto const text = st::dbc::format_dbc(db);
    if (output_path.has_value()) {
        if (auto s = st::dbc::write_dbc(*output_path, db); !s.has_value()) {
            std::fprintf(stderr, "can-export-dbc: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "can-export-dbc: messages=%zu  signals=%zu  wrote %s\n",
                     db.messages.size(), signals_added, output_path->string().c_str());
    } else {
        std::fputs(text.c_str(), stdout);
        std::fprintf(stderr, "can-export-dbc: messages=%zu  signals=%zu\n", db.messages.size(),
                     signals_added);
    }
    if (skipped_unlabeled > 0 || orphan_signals > 0 || skipped_new_id > 0) {
        std::fprintf(stderr, "can-export-dbc: skipped unlabeled=%zu  new-id=%zu  orphan=%zu\n",
                     skipped_unlabeled, skipped_new_id, orphan_signals);
    }
    return 0;
}

// Escape a single CSV cell per RFC 4180: wrap in quotes iff it contains
// comma, quote, CR, or LF; double any embedded quotes.
std::string csv_cell(std::string_view s) {
    bool needs_quotes = false;
    for (auto const c : s) {
        if (c == ',' || c == '"' || c == '\r' || c == '\n') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes)
        return std::string{s};
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (auto const c : s) {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int cmd_can_decode(int argc, char *argv[]) {
    std::optional<std::filesystem::path> dbc_path;
    std::optional<std::filesystem::path> asc_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "can-decode: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--dbc") {
            if (auto const *v = require_arg("--dbc"); v)
                dbc_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-decode: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!asc_path.has_value()) {
            asc_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-decode: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!dbc_path.has_value() || !asc_path.has_value()) {
        std::fputs("can-decode: missing required arguments:", stderr);
        if (!dbc_path.has_value())
            std::fputs(" --dbc", stderr);
        if (!asc_path.has_value())
            std::fputs(" <FILE.asc>", stderr);
        std::fputs(
            "\nUsage: subuwutuner-cli can-decode --dbc <FILE.dbc> <FILE.asc> [--output <csv>]\n",
            stderr);
        return 2;
    }

    auto const db = st::dbc::read_dbc(*dbc_path);
    if (!db.has_value()) {
        std::fprintf(stderr, "can-decode: %s\n", db.error().to_string().c_str());
        return 1;
    }
    auto const frames = st::can::read_asc(*asc_path);
    if (!frames.has_value()) {
        std::fprintf(stderr, "can-decode: %s\n", frames.error().to_string().c_str());
        return 1;
    }

    std::ofstream file_out;
    std::ostream *out_stream = &std::cout;
    if (output_path.has_value()) {
        file_out.open(*output_path);
        if (!file_out) {
            std::fprintf(stderr, "can-decode: cannot open output: %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out_stream = &file_out;
    }

    (*out_stream) << "timestamp_ns,bus,can_id,signal,value,unit\n";

    std::size_t decoded_rows = 0;
    std::size_t missing_id = 0;
    for (auto const &f : *frames) {
        auto const *msg = db->find_message(f.id);
        if (msg == nullptr || msg->extended != f.extended) {
            ++missing_id;
            continue;
        }
        for (auto const &sig : msg->signals) {
            double const value = st::dbc::decode_signal(f.payload(), sig);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", value);
            (*out_stream) << f.timestamp_ns << ',' << static_cast<unsigned>(f.bus) << ',' << f.id
                          << ',' << csv_cell(sig.name) << ',' << buf << ',' << csv_cell(sig.unit)
                          << '\n';
            ++decoded_rows;
        }
    }
    out_stream->flush();

    std::fprintf(stderr, "can-decode: frames=%zu  rows=%zu  unknown-id-frames=%zu\n",
                 frames->size(), decoded_rows, missing_id);
    return 0;
}

int cmd_flash_plan_info(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> source_path;
    std::optional<std::string> profile_arg;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-plan-info: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--def") {
            if (auto const *v = require_arg("--def"); v)
                def_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--source") {
            if (auto const *v = require_arg("--source"); v)
                source_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--profile") {
            if (auto const *v = require_arg("--profile"); v)
                profile_arg = std::string{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-plan-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!plan_path.has_value()) {
            plan_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-plan-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value()) {
        std::fputs("flash-plan-info: missing required argument\n"
                   "Usage: subuwutuner-cli flash-plan-info <FILE.toml>\n"
                   "       [--def <pack.toml> --source <rom.bin> "
                   "[--profile <P>]]\n"
                   "  With --def + --source, runs the same policy evaluation\n"
                   "  as flash-apply --profile (default profile is\n"
                   "  motorsport-only) and prints the lint summary without\n"
                   "  contacting any transport.\n",
                   stderr);
        return 2;
    }
    auto const r = st::flash::read_plan(*plan_path);
    if (!r.has_value()) {
        std::fprintf(stderr, "flash-plan-info: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &p = *r;
    std::printf("Plan: %s\n", plan_path->string().c_str());
    std::printf("  session            = 0x%02X\n", static_cast<unsigned>(p.session));
    std::printf("  data_format        = 0x%02X\n", static_cast<unsigned>(p.data_format));
    std::printf("  silence_bus        = %s\n", p.silence_bus ? "true" : "false");
    std::printf("  verify_after_write = %s\n", p.verify_after_write ? "true" : "false");
    std::printf("  dry_run            = %s\n", p.dry_run ? "true" : "false");
    std::printf("  block_size_hint    = %u\n", static_cast<unsigned>(p.block_size_hint));
    std::printf("  verify_chunk_size  = 0x%X\n", static_cast<unsigned>(p.verify_chunk_size));
    std::size_t total_bytes = 0;
    std::printf("\nSector writes (%zu):\n", p.writes.size());
    for (std::size_t i = 0; i < p.writes.size(); ++i) {
        auto const &w = p.writes[i];
        std::printf("  [%zu] 0x%08X .. 0x%08X  (%u bytes)\n", i, w.sector.address,
                    w.sector.address + w.sector.length, static_cast<unsigned>(w.sector.length));
        total_bytes += w.sector.length;
    }
    std::printf("  total              = %zu bytes\n", total_bytes);

    // Optional policy preview. --def + --source are required together
    // (need both the schema and the byte-baseline to map changes back to
    // tables). --profile is optional and defaults to motorsport-only.
    if (def_path.has_value() || source_path.has_value() || profile_arg.has_value()) {
        if (!def_path.has_value() || !source_path.has_value()) {
            std::fputs("\nflash-plan-info: policy preview requires both "
                       "--def and --source\n",
                       stderr);
            return 2;
        }
        auto profile = st::policy::Profile::MotorsportOnly;
        if (profile_arg.has_value()) {
            auto const parsed = st::policy::parse_profile(*profile_arg);
            if (!parsed.has_value()) {
                std::fprintf(stderr,
                             "flash-plan-info: unknown profile '%s' (valid: "
                             "motorsport-only, alberta-ca, eu-roadworthy, "
                             "california-us)\n",
                             profile_arg->c_str());
                return 2;
            }
            profile = *parsed;
        }
        auto const def = st::Definition::from_file(resolve_def_path(*def_path));
        if (!def.has_value()) {
            return print_def_load_error("flash-plan-info", *def_path, def.error());
        }
        auto const src = st::Rom::from_file(*source_path);
        if (!src.has_value()) {
            std::fprintf(stderr, "flash-plan-info: %s\n", src.error().to_string().c_str());
            return 1;
        }
        auto const d = st::flash::evaluate_plan_policy(p, *def, src->data(), profile);
        std::printf("\nPolicy preview (profile=%s):\n",
                    std::string{st::policy::profile_name(profile)}.c_str());
        std::printf("  engine_safety_tables = %zu", d.engine_safety_tables.size());
        for (auto const &id : d.engine_safety_tables)
            std::printf(" %s", id.c_str());
        std::printf("\n  emissions_tables     = %zu", d.emissions_tables.size());
        for (auto const &id : d.emissions_tables)
            std::printf(" %s", id.c_str());
        char const *action = "?";
        switch (d.overall_action) {
        case st::policy::Action::Silent:
            action = "silent";
            break;
        case st::policy::Action::Badge:
            action = "badge";
            break;
        case st::policy::Action::Warn:
            action = "warn";
            break;
        case st::policy::Action::Confirm:
            action = "confirm";
            break;
        case st::policy::Action::ConfirmWithReason:
            action = "confirm+reason";
            break;
        case st::policy::Action::Block:
            action = "block";
            break;
        }
        std::printf("\n  overall_action       = %s\n", action);
    }
    return 0;
}

int cmd_flash_manifest_info(int argc, char *argv[]) {
    std::optional<std::filesystem::path> manifest_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-manifest-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!manifest_path.has_value()) {
            manifest_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-manifest-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!manifest_path.has_value()) {
        std::fputs("flash-manifest-info: missing required argument\n"
                   "Usage: subuwutuner-cli flash-manifest-info <FILE.toml>\n",
                   stderr);
        return 2;
    }
    auto const r = st::flash::read_manifest(*manifest_path);
    if (!r.has_value()) {
        std::fprintf(stderr, "flash-manifest-info: %s\n", r.error().to_string().c_str());
        return 1;
    }
    auto const &m = *r;
    std::printf("Manifest:           %s\n", manifest_path->string().c_str());
    std::printf("  schema_version    = %d\n", m.schema_version);
    if (!m.created_at.empty()) {
        std::printf("  created_at        = %s\n", m.created_at.c_str());
    }
    std::printf("  plan_crc32        = 0x%08X\n", m.plan_crc32);
    std::printf("  overall_crc32     = 0x%08X\n", m.overall_crc32);
    if (!m.policy_profile.empty()) {
        std::printf("  policy_profile    = %s\n", m.policy_profile.c_str());
    }
    if (!m.policy_reason.empty()) {
        std::printf("  policy_reason     = %s\n", m.policy_reason.c_str());
    }
    std::size_t transferred = 0;
    std::size_t verified = 0;
    std::size_t bytes = 0;
    for (auto const &e : m.entries) {
        if (e.transferred)
            ++transferred;
        if (e.verified)
            ++verified;
        bytes += e.sector.length;
    }
    std::printf("\nEntries: %zu  (transferred %zu, verified %zu, %zu bytes)\n", m.entries.size(),
                transferred, verified, bytes);
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
        auto const &e = m.entries[i];
        std::printf("  [%zu] 0x%08X..0x%08X  crc=0x%08X  transferred=%s  verified=%s\n", i,
                    e.sector.address, e.sector.address + e.sector.length, e.data_crc32,
                    e.transferred ? "true" : "false", e.verified ? "true" : "false");
    }
    return 0;
}

int cmd_flash_delta(int argc, char *argv[]) {
    std::optional<std::filesystem::path> source_path;
    std::optional<std::filesystem::path> target_path;
    std::optional<std::filesystem::path> output_path;
    std::uint32_t sector_size = 0x1000;
    std::uint32_t base_address = 0;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-delta: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--sector-size") {
            auto const *v = require_arg("--sector-size");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), val, base);
            if (res.ec != std::errc{} || val == 0) {
                std::fprintf(stderr, "flash-delta: --sector-size must be a positive integer\n");
                return 2;
            }
            sector_size = val;
        } else if (a == "--base-address") {
            auto const *v = require_arg("--base-address");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), val, base);
            if (res.ec != std::errc{}) {
                std::fprintf(stderr, "flash-delta: --base-address must be an integer\n");
                return 2;
            }
            base_address = val;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) {
                output_path = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-delta: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!source_path.has_value()) {
            source_path = std::filesystem::path{argv[i]};
        } else if (!target_path.has_value()) {
            target_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-delta: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!source_path.has_value() || !target_path.has_value()) {
        std::fputs("flash-delta: missing required arguments:", stderr);
        if (!source_path.has_value())
            std::fputs(" <SOURCE.bin>", stderr);
        if (!target_path.has_value())
            std::fputs(" <TARGET.bin>", stderr);
        std::fputs("\nUsage: subuwutuner-cli flash-delta <SOURCE.bin> <TARGET.bin>\n"
                   "       [--sector-size <N>] [--base-address <addr>] [--output <plan.toml>]\n",
                   stderr);
        return 2;
    }
    auto const src = st::Rom::from_file(*source_path);
    if (!src.has_value()) {
        std::fprintf(stderr, "flash-delta: %s\n", src.error().to_string().c_str());
        return 1;
    }
    auto const tgt = st::Rom::from_file(*target_path);
    if (!tgt.has_value()) {
        std::fprintf(stderr, "flash-delta: %s\n", tgt.error().to_string().c_str());
        return 1;
    }
    if (src->size() != tgt->size()) {
        std::fprintf(stderr, "flash-delta: source size (%zu) != target size (%zu)\n", src->size(),
                     tgt->size());
        return 1;
    }
    auto const sectors =
        st::flash::Flasher::compute_delta(src->data(), tgt->data(), sector_size, base_address);

    st::flash::FlashPlan plan;
    plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off = static_cast<std::size_t>(s.address - base_address);
        sw.data.assign(tgt->data().begin() + static_cast<std::ptrdiff_t>(off),
                       tgt->data().begin() + static_cast<std::ptrdiff_t>(off + s.length));
        plan.writes.push_back(std::move(sw));
    }

    if (output_path.has_value()) {
        if (plan.writes.empty()) {
            std::fputs("flash-delta: source and target are identical; "
                       "no plan written\n",
                       stderr);
            return 0;
        }
        if (auto s = st::flash::write_plan(*output_path, plan); !s.has_value()) {
            std::fprintf(stderr, "flash-delta: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(
            stderr, "flash-delta: %zu sector(s), %zu bytes, wrote %s\n", plan.writes.size(),
            std::accumulate(plan.writes.begin(), plan.writes.end(), std::size_t{0},
                            [](std::size_t acc, auto const &w) { return acc + w.sector.length; }),
            output_path->string().c_str());
    } else {
        std::fputs(st::flash::format_plan(plan).c_str(), stdout);
        std::fprintf(
            stderr, "flash-delta: %zu sector(s), %zu bytes\n", plan.writes.size(),
            std::accumulate(plan.writes.begin(), plan.writes.end(), std::size_t{0},
                            [](std::size_t acc, auto const &w) { return acc + w.sector.length; }));
    }
    return 0;
}

int cmd_flash_resume(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> journal_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("flash-resume: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-resume: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!plan_path.has_value()) {
            plan_path = std::filesystem::path{argv[i]};
        } else if (!journal_path.has_value()) {
            journal_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-resume: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !journal_path.has_value()) {
        std::fputs("flash-resume: missing required arguments:", stderr);
        if (!plan_path.has_value())
            std::fputs(" <ORIGINAL.plan.toml>", stderr);
        if (!journal_path.has_value())
            std::fputs(" <JOURNAL.manifest.toml>", stderr);
        std::fputs("\nUsage: subuwutuner-cli flash-resume <ORIGINAL.plan.toml> "
                   "<JOURNAL.manifest.toml> [--output <resumed.plan.toml>]\n",
                   stderr);
        return 2;
    }

    auto const original = st::flash::read_plan(*plan_path);
    if (!original.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n", original.error().to_string().c_str());
        return 1;
    }
    auto const journal = st::flash::read_manifest(*journal_path);
    if (!journal.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n", journal.error().to_string().c_str());
        return 1;
    }

    auto resumed = st::flash::plan_resume(*original, *journal);
    if (!resumed.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n", resumed.error().to_string().c_str());
        return 1;
    }

    if (resumed->writes.empty()) {
        std::fputs("flash-resume: every sector in the original plan is already "
                   "transferred and verified; no resume needed\n",
                   stderr);
        return 0;
    }

    // Clear journal_path so the resumed plan doesn't accidentally
    // overwrite the original journal on the next execute(). The user can
    // set a fresh path explicitly via the resumed plan TOML if they want
    // continued journaling.
    resumed->journal_path.clear();

    std::size_t const bytes =
        std::accumulate(resumed->writes.begin(), resumed->writes.end(), std::size_t{0},
                        [](std::size_t acc, auto const &w) { return acc + w.sector.length; });

    if (output_path.has_value()) {
        if (auto s = st::flash::write_plan(*output_path, *resumed); !s.has_value()) {
            std::fprintf(stderr, "flash-resume: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "flash-resume: %zu sector(s), %zu bytes, wrote %s\n",
                     resumed->writes.size(), bytes, output_path->string().c_str());
    } else {
        std::fputs(st::flash::format_plan(*resumed).c_str(), stdout);
        std::fprintf(stderr, "flash-resume: %zu sector(s), %zu bytes\n", resumed->writes.size(),
                     bytes);
    }
    return 0;
}

// CLI-local alias for the library types so existing call sites
// (cmd_rom_pull, cmd_flash_apply, cmd_project_flash) stay
// unchanged. The implementation now lives in
// src/transport/src/uds_trace.cpp so the GUI can call it too —
// see Tools → Read ROM from Car's Trace-replay mode.
using st::transport::parse_uds_trace;
using st::transport::UdsTracePair;

namespace {
// Print the FlashReport from a successful or failed ExecuteOutcome. Shared
// between flash-apply and project-flash so the summary format stays
// identical across both entry points.
void print_flash_report(char const *cmd, st::flash::ExecuteOutcome const &outcome) {
    auto const &report = outcome.report;
    std::printf("%s: %s\n", cmd, outcome.ok() ? "SUCCESS" : "FAILED");
    std::printf("  entered_session    = %s\n", report.entered_session ? "true" : "false");
    std::printf("  silenced_bus       = %s\n", report.silenced_bus ? "true" : "false");
    std::printf("  restored_bus       = %s\n", report.restored_bus ? "true" : "false");
    std::printf("  bytes_transferred  = %zu\n", report.bytes_transferred);
    std::printf("  sectors            = %zu\n", report.sectors.size());
    for (std::size_t i = 0; i < report.sectors.size(); ++i) {
        auto const &so = report.sectors[i];
        std::printf("    [%zu] 0x%08X .. 0x%08X  erased=%d downloaded=%d "
                    "transferred=%d exited=%d check_deps=%d verified=%d\n",
                    i, so.sector.address, so.sector.address + so.sector.length,
                    static_cast<int>(so.erased), static_cast<int>(so.downloaded),
                    static_cast<int>(so.transferred), static_cast<int>(so.exited),
                    static_cast<int>(so.check_deps_passed), static_cast<int>(so.verified));
    }
    if (!outcome.ok()) {
        std::fprintf(stderr, "%s: %s\n", cmd, outcome.error->to_string().c_str());
    }
}

// Evaluate `plan` against `def` + `source_rom` under `profile`, print what
// the linter found, and return 0 if the host should proceed or a non-zero
// exit code if the plan is REFUSED by the active policy. `confirm` and
// `reason` are user-supplied: the linter checks they match what the
// profile demands (Confirm → --confirm; ConfirmWithReason → both).
int run_policy_gate(char const *cmd, st::flash::FlashPlan const &plan, st::Definition const &def,
                    std::span<std::uint8_t const> source_rom, st::policy::Profile profile,
                    bool confirm, std::optional<std::string> const &reason) {
    auto const d = st::flash::evaluate_plan_policy(plan, def, source_rom, profile);

    if (!d.engine_safety_tables.empty()) {
        std::fprintf(stderr, "%s: REFUSED: plan changes engine-safety-critical tables: ", cmd);
        for (auto const &id : d.engine_safety_tables) {
            std::fprintf(stderr, "%s ", id.c_str());
        }
        std::fprintf(stderr, "\nEngine-safety violations block in every profile (see "
                             "docs/06-legal-ethics.md).\n");
        return 3;
    }
    using A = st::policy::Action;
    auto const profile_str = std::string{st::policy::profile_name(profile)};
    if (d.overall_action == A::Block) {
        std::fprintf(stderr, "%s: REFUSED by policy under profile '%s'.\n", cmd,
                     profile_str.c_str());
        return 3;
    }
    if (d.overall_action == A::Confirm && !confirm) {
        std::fprintf(stderr,
                     "%s: profile '%s' requires --confirm to flash a plan that "
                     "changes emissions-relevant tables: ",
                     cmd, profile_str.c_str());
        for (auto const &id : d.emissions_tables) {
            std::fprintf(stderr, "%s ", id.c_str());
        }
        std::fputc('\n', stderr);
        return 3;
    }
    if (d.overall_action == A::ConfirmWithReason &&
        (!confirm || !reason.has_value() || reason->empty())) {
        std::fprintf(stderr,
                     "%s: profile '%s' requires --confirm AND a non-empty --reason "
                     "to flash a plan that changes emissions-relevant tables: ",
                     cmd, profile_str.c_str());
        for (auto const &id : d.emissions_tables) {
            std::fprintf(stderr, "%s ", id.c_str());
        }
        std::fputc('\n', stderr);
        return 3;
    }
    if (!d.emissions_tables.empty()) {
        std::fprintf(
            stderr,
            "%s: policy(%s) flagged emissions-relevant edits in %zu "
            "table(s); proceeding (%s)%s%s.\n",
            cmd, profile_str.c_str(), d.emissions_tables.size(),
            d.overall_action == A::ConfirmWithReason
                ? "confirmed + reason"
                : (d.overall_action == A::Confirm ? "confirmed" : "no confirmation required"),
            reason.has_value() ? ", reason=" : "", reason.has_value() ? reason->c_str() : "");
    }
    return 0;
}
} // namespace

int cmd_project_flash(int argc, char *argv[]) {
    std::optional<std::filesystem::path> project_dir;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> journal_path;
    bool confirm = false;
    bool dry_run = false;
    std::optional<std::string> reason;
    std::uint32_t sector_size = 0x1000;
    std::uint32_t base_address = 0;

    auto const parse_uint = [](char const *raw, std::uint32_t &out) -> bool {
        std::string_view sv{raw};
        int base = 10;
        if (sv.starts_with("0x") || sv.starts_with("0X")) {
            sv.remove_prefix(2);
            base = 16;
        }
        auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), out, base);
        return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
    };

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-flash: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v)
                trace_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--manifest") {
            if (auto const *v = require_arg("--manifest"); v)
                manifest_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--journal") {
            if (auto const *v = require_arg("--journal"); v)
                journal_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--reason") {
            if (auto const *v = require_arg("--reason"); v)
                reason = std::string{v};
            else
                return 2;
        } else if (a == "--confirm") {
            confirm = true;
        } else if (a == "--dry-run") {
            dry_run = true;
        } else if (a == "--sector-size") {
            auto const *v = require_arg("--sector-size");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint(v, val) || val == 0) {
                std::fprintf(stderr, "project-flash: --sector-size must be a positive integer\n");
                return 2;
            }
            sector_size = val;
        } else if (a == "--base-address") {
            auto const *v = require_arg("--base-address");
            if (v == nullptr)
                return 2;
            if (!parse_uint(v, base_address)) {
                std::fprintf(stderr, "project-flash: --base-address must be an integer\n");
                return 2;
            }
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-flash: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!project_dir.has_value()) {
            project_dir = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "project-flash: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!project_dir.has_value()) {
        std::fputs("project-flash: missing project directory\n"
                   "Usage: subuwutuner-cli project-flash <dir> [--trace <FILE.uds>]\n"
                   "       [--journal <FILE.toml>] [--manifest <FILE.toml>]\n"
                   "       [--confirm] [--reason \"…\"] [--dry-run]\n"
                   "       [--sector-size <N>] [--base-address <addr>]\n"
                   "  Without --trace, runs everything up through the policy\n"
                   "  gate and exits — a preview of what the flash would do.\n",
                   stderr);
        return 2;
    }
    bool const preview_only = !trace_path.has_value();

    auto project = st::Project::open(*project_dir);
    if (!project.has_value()) {
        std::fprintf(stderr, "project-flash: %s\n", project.error().to_string().c_str());
        return 1;
    }

    // Build the plan from the project's source/working delta. Empty delta
    // -> nothing to flash; treat as no-op success.
    if (project->source_rom().size() != project->working_rom().size()) {
        std::fprintf(stderr,
                     "project-flash: source/working size mismatch (%zu vs %zu); "
                     "abort\n",
                     project->source_rom().size(), project->working_rom().size());
        return 1;
    }
    auto const sectors = st::flash::Flasher::compute_delta(
        project->source_rom().data(), project->working_rom().data(), sector_size, base_address);
    if (sectors.empty()) {
        std::printf("project-flash: working ROM matches source — nothing "
                    "to flash.\n");
        return 0;
    }

    st::flash::FlashPlan plan;
    plan.dry_run = dry_run;
    plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off = static_cast<std::size_t>(s.address - base_address);
        sw.data.assign(project->working_rom().data().begin() + static_cast<std::ptrdiff_t>(off),
                       project->working_rom().data().begin() +
                           static_cast<std::ptrdiff_t>(off + s.length));
        plan.writes.push_back(std::move(sw));
    }
    if (journal_path.has_value()) {
        plan.journal_path = *journal_path;
    }

    // Policy gate uses the profile baked into the project.
    if (auto rc = run_policy_gate("project-flash", plan, project->definition(),
                                  project->source_rom().data(), project->policy_profile(), confirm,
                                  reason);
        rc != 0) {
        return rc;
    }

    if (preview_only) {
        std::printf(
            "project-flash: preview only (no --trace supplied); "
            "policy gate cleared %zu sector(s), %zu bytes. No "
            "transport contacted.\n",
            plan.writes.size(),
            std::accumulate(plan.writes.begin(), plan.writes.end(), std::size_t{0},
                            [](std::size_t a, auto const &w) { return a + w.data.size(); }));
        return 0;
    }

    // Replay trace through MockTransport, exactly like flash-apply.
    std::vector<UdsTracePair> pairs;
    std::string err;
    if (!parse_uds_trace(*trace_path, pairs, err)) {
        std::fputs(err.c_str(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "project-flash: mock open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    for (auto &p : pairs) {
        mock.expect_send_recv(std::move(p.request), std::move(p.response));
    }

    st::flash::Flasher flasher{mock};
    auto const outcome = flasher.execute(plan);
    print_flash_report("project-flash", outcome);
    if (!mock.exhausted()) {
        std::fprintf(stderr, "project-flash: warning: %zu trace entries unused\n",
                     mock.remaining());
    }

    // Optional manifest. plan_text is the rendered plan TOML so plan_crc32
    // is meaningful even though the plan was built in-memory.
    if (manifest_path.has_value()) {
        auto const plan_text = st::flash::format_plan(plan);
        auto manifest = st::flash::build_manifest(plan, plan_text, outcome.report);
        // Persist the audit trail: which profile gated this flash + the
        // operator-supplied justification (only set when --reason was given,
        // typically under california-us / EU ConfirmWithReason).
        manifest.policy_profile = std::string{st::policy::profile_name(project->policy_profile())};
        if (reason.has_value()) {
            manifest.policy_reason = *reason;
        }
        if (auto s = st::flash::write_manifest(*manifest_path, manifest); !s.has_value()) {
            std::fprintf(stderr, "project-flash: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "project-flash: wrote manifest %s\n", manifest_path->string().c_str());
    }

    return outcome.ok() ? 0 : 1;
}

int cmd_flash_apply(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> journal_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> source_path;
    std::optional<std::string> profile_arg;
    bool confirm = false;
    std::optional<std::string> reason;

    // Plan overrides. When set, these win over the TOML values after the
    // plan is loaded — convenient for ad-hoc runs without authoring a new
    // .plan.toml. The same fields can be set directly in plan TOML if
    // they belong to the plan itself.
    std::optional<std::uint8_t> data_format_override;
    std::optional<std::uint32_t> integrity_check_offset_override;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-apply: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--plan") {
            if (auto const *v = require_arg("--plan"); v)
                plan_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v)
                trace_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--manifest") {
            if (auto const *v = require_arg("--manifest"); v)
                manifest_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--journal") {
            if (auto const *v = require_arg("--journal"); v)
                journal_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--def") {
            if (auto const *v = require_arg("--def"); v)
                def_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--source") {
            if (auto const *v = require_arg("--source"); v)
                source_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--profile") {
            if (auto const *v = require_arg("--profile"); v)
                profile_arg = std::string{v};
            else
                return 2;
        } else if (a == "--confirm") {
            confirm = true;
        } else if (a == "--reason") {
            if (auto const *v = require_arg("--reason"); v)
                reason = std::string{v};
            else
                return 2;
        } else if (a == "--data-format") {
            // RequestDownload dataFormatIdentifier. 0x00 = no
            // compression/encryption (default). 0x04 = Subaru ciphertext,
            // which switches the per-block transfer service from 0x36
            // TransferData to 0xB6 Subaru bulk transfer.
            //
            // Hex when prefixed with 0x; otherwise base 10. NOT base 0
            // (auto-detect) — that would treat a leading-zero literal
            // like `010` as octal and parse it as 8, surprising users
            // who meant decimal ten.
            auto const *v = require_arg("--data-format");
            if (v == nullptr)
                return 2;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            char *end = nullptr;
            auto const val = std::strtoull(sv.data(), &end, base);
            if (end == sv.data() || *end != '\0' || val > 0xFFULL) {
                std::fprintf(stderr, "flash-apply: --data-format must be a "
                                     "0-255 hex (0x..) or decimal value "
                                     "(0x04 = Subaru ciphertext / bulk transfer)\n");
                return 2;
            }
            data_format_override = static_cast<std::uint8_t>(val);
        } else if (a == "--integrity-check-offset") {
            // Brick-safe sector reordering. The sector containing this
            // flash offset (e.g. 0x1FFFFE for the SH-2A WRX calibration
            // checksum) is written LAST regardless of plan order. Power
            // loss during the body leaves the checksum invalid and the
            // ECU detecting corruption at boot — the safe failure mode.
            //
            // Same hex-or-decimal parsing as --data-format; no octal
            // auto-detect.
            auto const *v = require_arg("--integrity-check-offset");
            if (v == nullptr)
                return 2;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            char *end = nullptr;
            auto const val = std::strtoull(sv.data(), &end, base);
            if (end == sv.data() || *end != '\0' || val > 0xFFFFFFFFULL) {
                std::fprintf(stderr, "flash-apply: --integrity-check-offset "
                                     "must be a hex (0x..) or decimal integer "
                                     "in 32-bit range\n");
                return 2;
            }
            integrity_check_offset_override = static_cast<std::uint32_t>(val);
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-apply: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "flash-apply: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !trace_path.has_value()) {
        std::fputs("flash-apply: missing required arguments:", stderr);
        if (!plan_path.has_value())
            std::fputs(" --plan", stderr);
        if (!trace_path.has_value())
            std::fputs(" --trace", stderr);
        std::fputs("\nUsage: subuwutuner-cli flash-apply --plan <FILE.toml> "
                   "--trace <FILE.uds>\n"
                   "       [--journal <FILE.toml>] [--manifest <FILE.toml>]\n"
                   "       [--data-format <hex>] [--integrity-check-offset <hex>]\n"
                   "       [--profile <P> --def <pack.toml> --source <rom.bin>]\n"
                   "       [--confirm] [--reason \"…\"]\n"
                   "\n"
                   "  --data-format            Override plan.data_format. 0x04 selects\n"
                   "                           Subaru bulk-transfer (0xB6) over standard\n"
                   "                           0x36 TransferData.\n"
                   "  --integrity-check-offset Override plan.integrity_check_offset.\n"
                   "                           Sector containing this offset is written\n"
                   "                           LAST (brick-safe ordering). Typical:\n"
                   "                           0x1FFFFE for SH-2A WRX calibration checksum.\n",
                   stderr);
        return 2;
    }

    auto plan = st::flash::read_plan(*plan_path);
    if (!plan.has_value()) {
        std::fprintf(stderr, "flash-apply: %s\n", plan.error().to_string().c_str());
        return 1;
    }
    if (journal_path.has_value()) {
        plan->journal_path = *journal_path;
    }
    if (data_format_override.has_value()) {
        plan->data_format = *data_format_override;
    }
    if (integrity_check_offset_override.has_value()) {
        plan->integrity_check_offset = *integrity_check_offset_override;
    }

    // Optional policy gate. When --profile is provided we diff the plan
    // against --source under --def via the shared run_policy_gate helper —
    // identical behaviour to project-flash, which loads the profile from
    // the project instead of from a flag.
    if (profile_arg.has_value()) {
        if (!def_path.has_value() || !source_path.has_value()) {
            std::fputs("flash-apply: --profile requires --def AND --source\n", stderr);
            return 2;
        }
        auto const profile = st::policy::parse_profile(*profile_arg);
        if (!profile.has_value()) {
            std::fprintf(stderr,
                         "flash-apply: unknown profile '%s'. Known: motorsport-only, "
                         "alberta-ca, eu-roadworthy, california-us\n",
                         profile_arg->c_str());
            return 2;
        }
        auto const def = st::Definition::from_file(resolve_def_path(*def_path));
        if (!def.has_value()) {
            return print_def_load_error("flash-apply", *def_path, def.error());
        }
        auto const src = st::Rom::from_file(*source_path);
        if (!src.has_value()) {
            std::fprintf(stderr, "flash-apply: %s\n", src.error().to_string().c_str());
            return 1;
        }
        if (auto rc =
                run_policy_gate("flash-apply", *plan, *def, src->data(), *profile, confirm, reason);
            rc != 0) {
            return rc;
        }
    }

    std::vector<UdsTracePair> pairs;
    std::string err;
    if (!parse_uds_trace(*trace_path, pairs, err)) {
        std::fputs(err.c_str(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "flash-apply: mock open failed: %s\n", s.error().to_string().c_str());
        return 1;
    }
    for (auto &p : pairs) {
        mock.expect_send_recv(std::move(p.request), std::move(p.response));
    }

    st::flash::Flasher flasher{mock};
    auto const outcome = flasher.execute(*plan);
    print_flash_report("flash-apply", outcome);
    auto const &report = outcome.report;
    if (!mock.exhausted()) {
        std::fprintf(stderr, "flash-apply: warning: %zu trace entries unused\n", mock.remaining());
    }

    // Build a manifest of the run if requested. plan_text is the source
    // TOML text so plan_crc32 is meaningful.
    if (manifest_path.has_value()) {
        std::ifstream pin{*plan_path, std::ios::binary};
        std::ostringstream pss;
        pss << pin.rdbuf();
        auto manifest = st::flash::build_manifest(*plan, pss.str(), report);
        // Persist the audit trail when a profile gated this flash. We only
        // populate `policy_profile` if --profile was supplied (so motorsport-
        // only ungated runs leave the manifest field blank, which means
        // "no policy applied" rather than "policy=motorsport-only").
        if (profile_arg.has_value()) {
            manifest.policy_profile = *profile_arg;
            if (reason.has_value()) {
                manifest.policy_reason = *reason;
            }
        }
        if (auto s = st::flash::write_manifest(*manifest_path, manifest); !s.has_value()) {
            std::fprintf(stderr, "flash-apply: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "flash-apply: wrote manifest %s\n", manifest_path->string().c_str());
    }

    return outcome.ok() ? 0 : 1;
}

namespace {

// Parse a TOML-style hex-or-decimal integer ("0x100" -> 256, "256" -> 256).
// Returns true on success.
bool parse_uint32_arg(std::string_view sv, std::uint32_t &out) {
    int base = 10;
    if (sv.starts_with("0x") || sv.starts_with("0X")) {
        sv.remove_prefix(2);
        base = 16;
    }
    auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), out, base);
    return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
}

} // namespace

// Format a byte span as space-separated uppercase hex (no prefix).
std::string hex_bytes_line(std::span<std::uint8_t const> bytes) {
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0)
            out.push_back(' ');
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(bytes[i]));
        out.append(buf, 2);
    }
    return out;
}

// flash-trace: walk the exact UDS sequence Flasher::execute would emit
// for `plan` and write `> req` / `< resp` pairs with canned positive
// responses to `out`. The reported maxNumberOfBlockLength in the
// RequestDownload response is fixed at 0x1000 so the TransferData
// chunking matches whatever choose_block_payload() picks given the
// plan's block_size_hint — same formula the orchestrator uses, so the
// generated trace lines up exactly with what the orchestrator sends.
void emit_happy_path_trace(st::flash::FlashPlan const &plan, std::ostream &out) {
    constexpr std::uint32_t kReportedMaxBlockLength = 0x1000;
    constexpr std::uint32_t kReportedPayloadCap = kReportedMaxBlockLength - 2U;

    auto const write_pair = [&](std::vector<std::uint8_t> const &req,
                                std::vector<std::uint8_t> const &resp, char const *label) {
        if (label != nullptr)
            out << "# " << label << "\n";
        out << "> " << hex_bytes_line(req) << "\n";
        out << "< " << hex_bytes_line(resp) << "\n";
    };

    out << "# Generated by 'subuwutuner-cli flash-trace' — guaranteed-success "
           "trace for the named plan.\n";

    // 1. DSC programming.
    write_pair(st::ecu::uds::build_dsc_request(plan.session),
               {static_cast<std::uint8_t>(0x50), static_cast<std::uint8_t>(plan.session)},
               "DSC programming");

    // 2. CC off.
    if (plan.silence_bus) {
        write_pair({0x28, 0x03, 0x03}, {0x68, 0x03},
                   "CommunicationControl: disable RX+TX, normal+nm");
    }

    // 3. Per sector. dry_run skips this whole block (matches
    //    Flasher::execute), so emit nothing for sectors when dry_run.
    if (!plan.dry_run) {
        for (auto const &w : plan.writes) {
            // 3a. eraseMemory.
            std::vector<std::uint8_t> opt;
            opt.reserve(9);
            opt.push_back(0x44);
            for (int shift : {24, 16, 8, 0}) {
                opt.push_back(static_cast<std::uint8_t>((w.sector.address >> shift) & 0xFFU));
            }
            for (int shift : {24, 16, 8, 0}) {
                opt.push_back(static_cast<std::uint8_t>((w.sector.length >> shift) & 0xFFU));
            }
            write_pair(st::ecu::uds::build_routine_control(st::ecu::uds::kRcStart,
                                                           st::ecu::uds::kRidEraseMemory, opt),
                       {0x71, 0x01, 0xFF, 0x00}, "eraseMemory");

            // 3b. RequestDownload. Reported max_block_length = 0x1000.
            write_pair(st::ecu::uds::build_request_download(plan.data_format, w.sector.address,
                                                            w.sector.length),
                       {0x74, 0x20, 0x10, 0x00}, "RequestDownload (max_block_length=0x1000)");

            // 3c. Chunked TransferData. block_payload =
            //     min(reported - 2, block_size_hint), or just reported - 2
            //     when block_size_hint == 0. Matches choose_block_payload.
            std::uint32_t const block_payload =
                (plan.block_size_hint == 0 || kReportedPayloadCap < plan.block_size_hint)
                    ? kReportedPayloadCap
                    : plan.block_size_hint;
            std::uint8_t counter = 1;
            std::size_t offset = 0;
            while (offset < w.data.size()) {
                std::size_t const remaining = w.data.size() - offset;
                std::size_t const this_block =
                    remaining < block_payload ? remaining : block_payload;
                std::span<std::uint8_t const> chunk{w.data.data() + offset, this_block};
                write_pair(st::ecu::uds::build_transfer_data(counter, chunk), {0x76, counter},
                           offset == 0 ? "TransferData" : nullptr);
                offset += this_block;
                counter = static_cast<std::uint8_t>(counter + 1U);
            }

            // 3d. RequestTransferExit.
            write_pair(st::ecu::uds::build_request_transfer_exit(), {0x77}, "RequestTransferExit");

            // 3e. checkProgrammingDependencies.
            write_pair(st::ecu::uds::build_routine_control(
                           st::ecu::uds::kRcStart, st::ecu::uds::kRidCheckProgrammingDependencies),
                       {0x71, 0x01, 0xFF, 0x01}, "checkProgrammingDependencies");

            // 3f. Verify readback (only when verify_after_write).
            if (plan.verify_after_write) {
                std::uint32_t const chunk_size = plan.verify_chunk_size == 0
                                                     ? static_cast<std::uint32_t>(w.data.size())
                                                     : plan.verify_chunk_size;
                std::size_t off = 0;
                while (off < w.data.size()) {
                    std::size_t const remaining = w.data.size() - off;
                    std::size_t const this_chunk = remaining < chunk_size ? remaining : chunk_size;
                    auto const req = st::ecu::uds::build_read_memory_by_address(
                        w.sector.address + static_cast<std::uint32_t>(off),
                        static_cast<std::uint32_t>(this_chunk));
                    std::vector<std::uint8_t> resp;
                    resp.reserve(1 + this_chunk);
                    resp.push_back(0x63);
                    resp.insert(resp.end(), w.data.begin() + static_cast<std::ptrdiff_t>(off),
                                w.data.begin() + static_cast<std::ptrdiff_t>(off + this_chunk));
                    write_pair(req, resp, off == 0 ? "verify readback" : nullptr);
                    off += this_chunk;
                }
            }
        }
    }

    // 4. CC on.
    if (plan.silence_bus) {
        write_pair({0x28, 0x00, 0x03}, {0x68, 0x00}, "CommunicationControl: re-enable RX+TX");
    }
}

int cmd_flash_trace(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> output_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-trace: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--plan") {
            if (auto const *v = require_arg("--plan"); v)
                plan_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-trace: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "flash-trace: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !output_path.has_value()) {
        std::fputs("flash-trace: missing required arguments:", stderr);
        if (!plan_path.has_value())
            std::fputs(" --plan", stderr);
        if (!output_path.has_value())
            std::fputs(" --output", stderr);
        std::fputs("\nUsage: subuwutuner-cli flash-trace --plan <FILE.toml> "
                   "--output <FILE.uds>\n",
                   stderr);
        return 2;
    }
    auto const plan = st::flash::read_plan(*plan_path);
    if (!plan.has_value()) {
        std::fprintf(stderr, "flash-trace: %s\n", plan.error().to_string().c_str());
        return 1;
    }
    std::ofstream out{*output_path};
    if (!out) {
        std::fprintf(stderr, "flash-trace: cannot open output: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    emit_happy_path_trace(*plan, out);
    if (!out) {
        std::fprintf(stderr, "flash-trace: write failed: %s\n", output_path->string().c_str());
        return 1;
    }
    std::fprintf(stderr, "flash-trace: wrote %s\n", output_path->string().c_str());
    return 0;
}

// --- doctor: triage / "what to do next" composer -----------------------------
//
// Ship blocker #6 from docs/04-roadmap.md. Single subcommand that probes the
// install: tool/build identity, J2534 adapter registry, definition-pack
// health, and (with --rom + --pack-dir) CID identification. Each section
// prints a traffic-light status; the closing block surfaces actionable hints.
//
// Composes existing primitives:
//   - st::transport::j2534::discover_adapters() (transport-list logic)
//   - discover_pack_paths() + Definition::from_file (pack-list logic)
//   - Definition::matches(Rom) (rom-identify logic)
//
// Exit codes:
//   0 = healthy or advisory warnings only
//   1 = any FAIL (bad --pack-dir, every pack failed to parse, ROM doesn't
//       match any pack, --rom unreadable)
//   2 = argument errors

enum class DoctorStatus : std::uint8_t { Info, Ok, Warn, Fail };

char const *doctor_glyph(DoctorStatus s) noexcept {
    switch (s) {
    case DoctorStatus::Info:
        return "[INFO]";
    case DoctorStatus::Ok:
        return "[ OK ]";
    case DoctorStatus::Warn:
        return "[WARN]";
    case DoctorStatus::Fail:
        return "[FAIL]";
    }
    return "[ ?? ]";
}

// Tracks worst-severity status seen + the accumulated "what to do next"
// hints. Info is informational (does not worsen Ok); Warn > Ok; Fail > Warn.
struct DoctorReport {
    DoctorStatus worst{DoctorStatus::Ok};
    std::vector<std::string> hints;

    void note(DoctorStatus s) noexcept {
        if (s == DoctorStatus::Info)
            return;
        if (static_cast<std::uint8_t>(s) > static_cast<std::uint8_t>(worst))
            worst = s;
    }
    void note(DoctorStatus s, std::string hint) {
        note(s);
        if (!hint.empty())
            hints.push_back(std::move(hint));
    }
};

char const *doctor_os_label() noexcept {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown OS";
#endif
}

void doctor_section_system(DoctorReport &r) {
    std::printf("%s System\n", doctor_glyph(DoctorStatus::Ok));
    std::printf("       %.*s %.*s on %s\n", static_cast<int>(st::Version::name().size()),
                st::Version::name().data(), static_cast<int>(st::Version::string().size()),
                st::Version::string().data(), doctor_os_label());
    r.note(DoctorStatus::Ok);
}

void doctor_section_adapters(DoctorReport &r) {
    auto const adapters = st::transport::j2534::discover_adapters();
    if (!adapters.empty()) {
        std::printf("%s J2534 adapters\n", doctor_glyph(DoctorStatus::Ok));
        std::printf("       %zu registered\n", adapters.size());
        for (auto const &a : adapters) {
            std::printf("       - %s  (%s)\n", a.name.c_str(), a.function_library.c_str());
        }
        std::puts("       OBDX Pro VX + doc-18 handheld use USB CDC, not J2534;");
        std::puts("       this section doesn't enumerate them.");
        r.note(DoctorStatus::Ok);
        return;
    }
#if defined(_WIN32)
    std::printf("%s J2534 adapters\n", doctor_glyph(DoctorStatus::Warn));
    std::puts("       No J2534 vendor DLLs registered under");
    std::puts("       HKLM\\Software\\PassThruSupport.04.04.");
    std::puts("       (Irrelevant if your adapter is OBDX Pro VX — it uses USB CDC.)");
    r.note(DoctorStatus::Warn,
           "If you'll use a J2534 adapter (Tactrix OpenPort, MongoosePro), install "
           "the vendor DLL and re-run `doctor`.");
#else
    std::printf("%s J2534 adapters\n", doctor_glyph(DoctorStatus::Info));
    std::puts("       Non-Windows host — J2534 vendor DLLs only register under");
    std::puts("       the Windows registry. Use OBDX or the doc-18 handheld here.");
    r.note(DoctorStatus::Info);
#endif
}

void doctor_section_packs(DoctorReport &r,
                          std::optional<std::filesystem::path> const &pack_dir,
                          std::vector<st::Definition> &loaded_packs) {
    if (!pack_dir.has_value()) {
        std::printf("%s Definition packs\n", doctor_glyph(DoctorStatus::Info));
        std::puts("       --pack-dir not given; skipping pack health check.");
        std::puts("       Pass --pack-dir <dir> to inspect a pack collection.");
        r.note(DoctorStatus::Info);
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*pack_dir, ec) || ec) {
        std::printf("%s Definition packs\n", doctor_glyph(DoctorStatus::Fail));
        std::printf("       --pack-dir is not a directory: %s\n", pack_dir->string().c_str());
        r.note(DoctorStatus::Fail, "Provide a real directory path for --pack-dir.");
        return;
    }
    auto const paths = discover_pack_paths(*pack_dir, "doctor");
    if (paths.empty()) {
        std::printf("%s Definition packs\n", doctor_glyph(DoctorStatus::Fail));
        std::printf("       No .toml pack files found under %s.\n", pack_dir->string().c_str());
        r.note(DoctorStatus::Fail,
               "Generate packs from community XML via tools/defgen, "
               "or drop a community pack bundle in there.");
        return;
    }
    std::size_t failed = 0;
    std::vector<std::string> failure_paths;
    for (auto const &p : paths) {
        auto def = st::Definition::from_file(p);
        if (def.has_value()) {
            loaded_packs.push_back(std::move(*def));
        } else {
            ++failed;
            if (failure_paths.size() < 3)
                failure_paths.push_back(p.string());
        }
    }
    DoctorStatus const s = loaded_packs.empty() ? DoctorStatus::Fail
                           : failed != 0        ? DoctorStatus::Warn
                                                : DoctorStatus::Ok;
    std::printf("%s Definition packs\n", doctor_glyph(s));
    std::printf("       %zu found, %zu loaded, %zu failed\n", paths.size(), loaded_packs.size(),
                failed);
    if (!failure_paths.empty()) {
        std::printf("       Failing packs (first %zu):\n", failure_paths.size());
        for (auto const &fp : failure_paths) {
            std::printf("         - %s\n", fp.c_str());
        }
    }
    if (s == DoctorStatus::Fail) {
        r.note(s, "Every pack in --pack-dir failed to parse. Run "
                  "`pack-list <dir>` for the per-file error messages.");
    } else if (s == DoctorStatus::Warn) {
        r.note(s, "Some packs failed to load. Run `pack-list <dir>` "
                  "for per-file errors.");
    } else {
        r.note(s);
    }
}

void doctor_section_rom(DoctorReport &r, std::optional<std::filesystem::path> const &rom_path,
                        std::vector<st::Definition> const &loaded_packs) {
    if (!rom_path.has_value()) {
        std::printf("%s ROM identification\n", doctor_glyph(DoctorStatus::Info));
        std::puts("       --rom not given; skipping CID match.");
        std::puts("       Pass --rom <FILE.bin> alongside --pack-dir to check");
        std::puts("       which packs match a real ROM dump.");
        r.note(DoctorStatus::Info);
        return;
    }
    auto rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::printf("%s ROM identification\n", doctor_glyph(DoctorStatus::Fail));
        std::printf("       Cannot read ROM: %s\n", rom.error().to_string().c_str());
        r.note(DoctorStatus::Fail,
               "Check the --rom path; the file must exist and be readable.");
        return;
    }
    if (loaded_packs.empty()) {
        std::printf("%s ROM identification\n", doctor_glyph(DoctorStatus::Info));
        std::puts("       No loaded packs to match against (see above).");
        r.note(DoctorStatus::Info);
        return;
    }
    std::vector<std::pair<std::string, std::string>> matches;
    for (auto const &def : loaded_packs) {
        if (auto m = def.matches(*rom); m.has_value()) {
            matches.emplace_back(def.pack().id, *m);
        }
    }
    if (matches.empty()) {
        std::printf("%s ROM identification\n", doctor_glyph(DoctorStatus::Fail));
        std::printf("       %zu-byte ROM, no CID match across %zu loaded packs.\n", rom->size(),
                    loaded_packs.size());
        r.note(DoctorStatus::Fail,
               "No matching definition. The ROM may be an unsupported cal "
               "ID, an encrypted / partial dump, or need a new pack via "
               "tools/defgen.");
        return;
    }
    std::printf("%s ROM identification\n", doctor_glyph(DoctorStatus::Ok));
    std::printf("       %zu-byte ROM, %zu match%s:\n", rom->size(), matches.size(),
                matches.size() == 1 ? "" : "es");
    for (auto const &[id, name] : matches) {
        std::printf("       - %s  (%s)\n", id.c_str(), name.c_str());
    }
    r.note(DoctorStatus::Ok);
}

char const *doctor_status_str(DoctorStatus s) noexcept {
    switch (s) {
    case DoctorStatus::Info:
        return "info";
    case DoctorStatus::Ok:
        return "ok";
    case DoctorStatus::Warn:
        return "warn";
    case DoctorStatus::Fail:
        return "fail";
    }
    return "unknown";
}

// `doctor --json` output. Same checks as the text mode but emitted as a single
// JSON object on stdout (single line, no pretty-print — scripts that want
// indentation can pipe through `jq`). Exit code is unchanged: 1 on Fail, 0
// otherwise. Schema is `subuwutuner.doctor.v1` — extending with new fields
// is non-breaking; renaming or removing existing fields bumps the schema.
int cmd_doctor_json(std::optional<std::filesystem::path> const &pack_dir,
                    std::optional<std::filesystem::path> const &rom_path) {
    DoctorReport report;
    std::string out;
    out.reserve(2048);
    out.append("{\"schema\":\"subuwutuner.doctor.v1\",");

    // tool + version + platform — mirrors the text-mode "System" line content
    // but as named fields so callers don't have to parse free text.
    out.append("\"tool\":");
    json_escape(out, st::Version::name());
    out.append(",\"version\":");
    json_escape(out, st::Version::string());
    out.append(",\"platform\":");
    json_escape(out, doctor_os_label());
    out.append(",\"sections\":[");

    bool first_section = true;
    auto const section_start = [&](char const *name, DoctorStatus s) {
        if (!first_section)
            out.append(",");
        first_section = false;
        out.append("{\"name\":");
        json_escape(out, name);
        out.append(",\"status\":");
        json_escape(out, doctor_status_str(s));
    };

    // System section — always Ok, just records the tool identity (already
    // emitted above as the top-level fields; this section is a placeholder
    // for symmetry with text mode).
    section_start("system", DoctorStatus::Ok);
    out.append("}");
    report.note(DoctorStatus::Ok);

    // J2534 adapters section. Same Windows/non-Windows branch shape as text
    // mode but the registered-adapter list ships as a JSON array of objects.
    {
        auto const adapters = st::transport::j2534::discover_adapters();
        if (!adapters.empty()) {
            section_start("j2534_adapters", DoctorStatus::Ok);
            out.append(",\"count\":");
            out.append(std::to_string(adapters.size()));
            out.append(",\"adapters\":[");
            for (std::size_t i = 0; i < adapters.size(); ++i) {
                if (i != 0)
                    out.append(",");
                out.append("{\"name\":");
                json_escape(out, adapters[i].name);
                out.append(",\"function_library\":");
                json_escape(out, adapters[i].function_library);
                out.append("}");
            }
            out.append("]}");
            report.note(DoctorStatus::Ok);
        } else {
#if defined(_WIN32)
            section_start("j2534_adapters", DoctorStatus::Warn);
            out.append(",\"count\":0,\"adapters\":[]}");
            report.note(DoctorStatus::Warn, "If you'll use a J2534 adapter (Tactrix OpenPort, "
                                            "MongoosePro), install the vendor DLL and re-run "
                                            "`doctor`.");
#else
            section_start("j2534_adapters", DoctorStatus::Info);
            out.append(",\"count\":0,\"adapters\":[]}");
            report.note(DoctorStatus::Info);
#endif
        }
    }

    // Definition packs section. Walks pack-dir if given; reports load
    // counts + failure paths as structured fields.
    std::vector<st::Definition> loaded_packs;
    if (!pack_dir.has_value()) {
        section_start("definition_packs", DoctorStatus::Info);
        out.append("}");
        report.note(DoctorStatus::Info);
    } else {
        std::error_code ec;
        if (!std::filesystem::is_directory(*pack_dir, ec) || ec) {
            section_start("definition_packs", DoctorStatus::Fail);
            out.append(",\"pack_dir\":");
            json_escape(out, pack_dir->string());
            out.append(",\"error\":\"not_a_directory\"}");
            report.note(DoctorStatus::Fail, "Provide a real directory path for --pack-dir.");
        } else {
            auto const paths = discover_pack_paths(*pack_dir, "doctor");
            if (paths.empty()) {
                section_start("definition_packs", DoctorStatus::Fail);
                out.append(",\"pack_dir\":");
                json_escape(out, pack_dir->string());
                out.append(",\"found\":0,\"loaded\":0,\"failed\":0,\"failure_paths\":[]}");
                report.note(DoctorStatus::Fail,
                            "Generate packs from community XML via tools/defgen, or drop a "
                            "community pack bundle in there.");
            } else {
                std::size_t failed = 0;
                std::vector<std::string> failure_paths;
                for (auto const &p : paths) {
                    auto def = st::Definition::from_file(p);
                    if (def.has_value()) {
                        loaded_packs.push_back(std::move(*def));
                    } else {
                        ++failed;
                        failure_paths.push_back(p.string());
                    }
                }
                DoctorStatus const s = loaded_packs.empty() ? DoctorStatus::Fail
                                       : failed != 0        ? DoctorStatus::Warn
                                                            : DoctorStatus::Ok;
                section_start("definition_packs", s);
                out.append(",\"pack_dir\":");
                json_escape(out, pack_dir->string());
                out.append(",\"found\":");
                out.append(std::to_string(paths.size()));
                out.append(",\"loaded\":");
                out.append(std::to_string(loaded_packs.size()));
                out.append(",\"failed\":");
                out.append(std::to_string(failed));
                out.append(",\"failure_paths\":[");
                for (std::size_t i = 0; i < failure_paths.size(); ++i) {
                    if (i != 0)
                        out.append(",");
                    json_escape(out, failure_paths[i]);
                }
                out.append("]}");
                if (s == DoctorStatus::Fail) {
                    report.note(s, "Every pack in --pack-dir failed to parse. Run "
                                   "`pack-list <dir>` for the per-file error messages.");
                } else if (s == DoctorStatus::Warn) {
                    report.note(s, "Some packs failed to load. Run `pack-list <dir>` for "
                                   "per-file errors.");
                } else {
                    report.note(s);
                }
            }
        }
    }

    // ROM identification section.
    if (!rom_path.has_value()) {
        section_start("rom_identification", DoctorStatus::Info);
        out.append("}");
        report.note(DoctorStatus::Info);
    } else {
        auto rom = st::Rom::from_file(*rom_path);
        if (!rom.has_value()) {
            section_start("rom_identification", DoctorStatus::Fail);
            out.append(",\"rom_path\":");
            json_escape(out, rom_path->string());
            out.append(",\"error\":");
            json_escape(out, rom.error().to_string());
            out.append("}");
            report.note(DoctorStatus::Fail,
                        "Check the --rom path; the file must exist and be readable.");
        } else if (loaded_packs.empty()) {
            section_start("rom_identification", DoctorStatus::Info);
            out.append(",\"rom_size\":");
            out.append(std::to_string(rom->size()));
            out.append(",\"matches\":[]}");
            report.note(DoctorStatus::Info);
        } else {
            std::vector<std::pair<std::string, std::string>> matches;
            for (auto const &def : loaded_packs) {
                if (auto m = def.matches(*rom); m.has_value()) {
                    matches.emplace_back(def.pack().id, *m);
                }
            }
            DoctorStatus const s = matches.empty() ? DoctorStatus::Fail : DoctorStatus::Ok;
            section_start("rom_identification", s);
            out.append(",\"rom_size\":");
            out.append(std::to_string(rom->size()));
            out.append(",\"matches\":[");
            for (std::size_t i = 0; i < matches.size(); ++i) {
                if (i != 0)
                    out.append(",");
                out.append("{\"pack_id\":");
                json_escape(out, matches[i].first);
                out.append(",\"cid\":");
                json_escape(out, matches[i].second);
                out.append("}");
            }
            out.append("]}");
            if (s == DoctorStatus::Fail) {
                report.note(s, "No matching definition. The ROM may be an unsupported cal "
                               "ID, an encrypted / partial dump, or need a new pack via "
                               "tools/defgen.");
            } else {
                report.note(s);
            }
        }
    }
    out.append("]");

    // Overall status + hints.
    out.append(",\"overall_status\":");
    json_escape(out, doctor_status_str(report.worst));
    out.append(",\"hints\":[");
    for (std::size_t i = 0; i < report.hints.size(); ++i) {
        if (i != 0)
            out.append(",");
        json_escape(out, report.hints[i]);
    }
    out.append("]}");
    out.push_back('\n');

    std::fputs(out.c_str(), stdout);
    return report.worst == DoctorStatus::Fail ? 1 : 0;
}

int cmd_doctor(int argc, char *argv[]) {
    std::optional<std::filesystem::path> pack_dir;
    std::optional<std::filesystem::path> rom_path;
    bool json_mode = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--pack-dir") {
            if (i + 1 >= argc) {
                std::fputs("doctor: --pack-dir requires a path\n", stderr);
                return 2;
            }
            pack_dir = std::filesystem::path{argv[++i]};
        } else if (a == "--rom") {
            if (i + 1 >= argc) {
                std::fputs("doctor: --rom requires a path\n", stderr);
                return 2;
            }
            rom_path = std::filesystem::path{argv[++i]};
        } else if (a == "--json") {
            json_mode = true;
        } else {
            std::fprintf(stderr, "doctor: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (json_mode) {
        return cmd_doctor_json(pack_dir, rom_path);
    }

    DoctorReport report;
    doctor_section_system(report);
    std::puts("");
    doctor_section_adapters(report);
    std::puts("");
    std::vector<st::Definition> loaded_packs;
    doctor_section_packs(report, pack_dir, loaded_packs);
    std::puts("");
    doctor_section_rom(report, rom_path, loaded_packs);
    std::puts("");

    if (report.hints.empty()) {
        std::puts("All checks passed.");
    } else {
        std::puts("What to do next:");
        for (auto const &h : report.hints) {
            std::printf("  - %s\n", h.c_str());
        }
    }

    return report.worst == DoctorStatus::Fail ? 1 : 0;
}

int cmd_transport_list_json() {
    auto const adapters = st::transport::j2534::discover_adapters();
    std::string out;
    out.reserve(512);
    out.append("{\"schema\":\"subuwutuner.transport-list.v1\",\"j2534\":{");
    out.append("\"count\":");
    out.append(std::to_string(adapters.size()));
    out.append(",\"adapters\":[");
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (i != 0)
            out.append(",");
        auto const &a = adapters[i];
        out.append("{\"name\":");
        json_escape(out, a.name);
        out.append(",\"function_library\":");
        json_escape(out, a.function_library);
        out.append(",\"vendor\":");
        json_escape(out, a.vendor);
        out.append(",\"protocols\":");
        json_escape(out, st::transport::j2534::format_protocols_supported(a.protocols_supported));
        out.append(",\"registry_view\":");
        json_escape(out, st::transport::j2534::registry_view_name(a.view));
        out.append(",\"subkey\":");
        json_escape(out, a.subkey);
        out.append("}");
    }
    out.append("]},");
    // Other transport families always ship as available with --device <port>;
    // we report them so callers know the full set without scraping help text.
    out.append("\"other_transports\":[");
    out.append("{\"id\":\"obdx\",\"description\":\"OBDX Pro VX (USB CDC + DVI codec)\","
               "\"requires\":\"--device <port>\"},");
    out.append("{\"id\":\"native\",\"description\":\"Doc-18 handheld (USB CDC + native)\","
               "\"requires\":\"--device <port>\"},");
    out.append("{\"id\":\"mock\",\"description\":\"MockTransport for trace replay\","
               "\"requires\":\"--trace <file>\"}");
    out.append("]}\n");
    std::fputs(out.c_str(), stdout);
    return 0;
}

// Per-transport decision text — surfaces "when to pick this" prose so
// a user staring at OBDX / J2534 / native doesn't have to read the
// design doc. Returned by --explain; also referenced in the JSON
// envelope so downstream tooling can render the same guidance.
struct TransportExplain {
    char const *id;
    char const *headline;
    char const *when_to_pick;
    char const *notes;
};

constexpr TransportExplain kTransportExplains[] = {
    {
        "obdx",
        "OBDX Pro VX — USB CDC + DVI codec",
        "Daily-driver SSM-A8 polling, UDS-side flashing, datalog "
        "capture. Default choice for the in-hand 2017 WRX rig.",
        "Needs --device COM5 (per the field-commands doc). DVI codec "
        "handles ISO-15765 framing in adapter firmware so the host "
        "speaks one-PDU-at-a-time."
    },
    {
        "j2534",
        "Generic J2534 v04.04 — vendor DLL",
        "Cross-vendor compatibility across J2534 adapter brands. "
        "Pick when the user has an existing OEM-grade adapter that "
        "speaks the v04.04 PassThru API.",
        "Windows-only registration channel. CLI surface is currently "
        "a stub for v1 — discovery works (see --transport j2534 "
        "rom-info, list), reads/writes wire next."
    },
    {
        "native",
        "Doc-18 handheld — USB CDC + native framing",
        "The portable Teensy/ESP32 handheld from docs/18. Same "
        "byte-channel as OBDX but expects the host-side firmware to "
        "produce ISO-15765-framed PDUs.",
        "Same --device <port> shape as OBDX. Detection logic in "
        "transport::discover handles the firmware-id handshake."
    },
    {
        "mock",
        "MockTransport — trace replay (no hardware)",
        "Replay a captured datalog / SSM-A8 / UDS exchange against "
        "the framers. Drives every Catch2 integration test that "
        "doesn't have a real adapter on hand.",
        "Needs --trace <file>. Hex format defined in "
        "fixtures/demo-trace.hex; see docs/24-sniff-workflows.md."
    },
};

int cmd_transport_list(int argc, char *argv[]) {
    bool json_mode = false;
    bool explain_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a == "--explain") {
            explain_mode = true;
        } else {
            std::fprintf(stderr, "transport-list: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (explain_mode) {
        // Always text — pairs with the human-readable transport list,
        // not the JSON envelope. JSON consumers get the same prose
        // through "when_to_pick" + "notes" in the v1.1 envelope below.
        std::puts("Transport selection guide:");
        std::puts("");
        for (auto const &e : kTransportExplains) {
            std::printf("  %s — %s\n", e.id, e.headline);
            std::printf("    Pick when: %s\n", e.when_to_pick);
            std::printf("    Notes:     %s\n", e.notes);
            std::puts("");
        }
        std::puts("CLI flag shape: --transport <id> [--device <port> | "
                  "--dll <path> | --trace <file>]");
        std::puts("Run `subuwutuner-cli transport-list` (no flag) for "
                  "host-discovered adapters.");
        return 0;
    }
    if (json_mode) {
        return cmd_transport_list_json();
    }
    auto const adapters = st::transport::j2534::discover_adapters();
    if (adapters.empty()) {
        std::puts("J2534 v04.04 adapters: (none registered on this host)");
#ifndef _WIN32
        std::puts("  (non-Windows host — J2534 vendor DLLs only register "
                  "under the Windows registry; Linux/macOS adapters use "
                  "a different distribution channel)");
#else
        std::puts("  Install a vendor DLL (e.g. Tactrix OpenPort 2.0's");
        std::puts("  J2534 driver from tactrix.com) and re-run this");
        std::puts("  command.");
#endif
    } else {
        std::printf("J2534 v04.04 adapters: %zu registered\n", adapters.size());
        std::size_t i = 1;
        for (auto const &a : adapters) {
            std::printf("\n  [%zu] %s\n", i++, a.name.c_str());
            std::printf("      DLL:        %s\n", a.function_library.c_str());
            if (!a.vendor.empty()) {
                std::printf("      Vendor:     %s\n", a.vendor.c_str());
            }
            std::string const protos =
                st::transport::j2534::format_protocols_supported(a.protocols_supported);
            std::printf("      Protocols:  %s\n", protos.c_str());
            std::printf("      Registry:   %s (subkey: %s)\n",
                        st::transport::j2534::registry_view_name(a.view), a.subkey.c_str());
        }
        std::printf("\nTo use one, eventually:\n");
        std::printf("  subuwutuner-cli rom-pull --transport j2534 "
                    "--dll \"<DLL path above>\" ...\n");
    }
    std::puts("");
    std::puts("Other transport families:");
    std::puts("  - OBDX Pro VX (USB CDC + DVI codec):  "
              "--transport obdx --device <port>   [shipped — runs via the");
    std::puts("                                                              "
              "    platform serial byte channel]");
    std::puts("  - Doc-18 handheld (USB CDC + native): "
              "--transport native --device <port> [shipped — same byte chan]");
    std::puts("  See docs/13-transport.md for the byte-channel design.");
    return 0;
}

// did-datalog-preset — prints the SSM-extended DID polling protocol
// shape + wide-layout signal map. Sourced clean-room from bus
// captures + 18 datalog CSV exports. Useful when a tuner wants to
// replay an aftermarket-datalogger trace in SubuwuTuner's future
// live-gauge cluster or wants a paper trail of "what byte is what"
// for a captured trace.
int cmd_did_datalog_preset(int argc, char *argv[]) {
    bool json_mode = false;
    bool toml_mode = false;
    std::string firmware_arg = "wide";
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else if (a == "--toml") {
            toml_mode = true;
        } else if (a == "--firmware") {
            if (i + 1 >= argc) {
                std::fputs("did-datalog-preset: --firmware requires a value\n",
                           stderr);
                return 2;
            }
            firmware_arg = argv[++i];
        } else {
            std::fprintf(stderr,
                         "did-datalog-preset: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    namespace cb = st::ecu::extended_did_datalog;
    cb::DidLayoutVersion firmware = cb::DidLayoutVersion::Wide;
    char const *firmware_label = "wide layout";
    // Accept the new "narrow"/"wide" tags plus legacy firmware-version
    // aliases for one release cycle.
    if (firmware_arg == "narrow" || firmware_arg == "v1_7_4_2" ||
        firmware_arg == "v1.7.4.2") {
        firmware = cb::DidLayoutVersion::Narrow;
        firmware_label = "narrow layout";
    } else if (firmware_arg != "wide" && firmware_arg != "v1_7_6_0" &&
               firmware_arg != "v1.7.6.0") {
        std::fprintf(stderr,
                     "did-datalog-preset: --firmware must be 'narrow' or "
                     "'wide' (got '%s')\n",
                     firmware_arg.c_str());
        return 2;
    }
    auto const layout = cb::ap_layout(firmware);
    if (toml_mode) {
        // SubuwuTuner-ready pack fragment for the wide-layout Verified
        // rows so a user can drop it into a definitions tree.
        // Verified-only: Hypothesized rows are excluded because their
        // byte positions aren't ground-truth.
        std::printf("# Aftermarket-datalogger F3xx polled-DID PIDs — generated by\n"
                    "# `subuwutuner-cli did-datalog-preset --toml "
                    "--firmware %s`.\n"
                    "# Source: st::ecu::extended_did_datalog — see\n"
                    "# src/ecu/include/st/ecu/extended_did_datalog.hpp.\n"
                    "# Each [[scaling]] block is the wire-side scale; "
                    "decode raw bytes via\n"
                    "#   value_eng = raw * factor + offset.\n"
                    "# ram_address_status: authoritative (SSM_* — OEM "
                    "RAM) or candidate\n"
                    "#   (RAM_* — tester-defined; on-car SSM-0xA8 "
                    "needed to confirm).\n\n",
                    firmware_arg.c_str());
        auto const storage_str = [](cb::SignalStorage s) -> char const * {
            switch (s) {
            case cb::SignalStorage::Uint8:    return "u8";
            case cb::SignalStorage::Int8:     return "i8";
            case cb::SignalStorage::Uint16:   return "u16_be";
            case cb::SignalStorage::Int16:    return "i16_be";
            case cb::SignalStorage::Uint16Le: return "u16_le";
            case cb::SignalStorage::Int16Le:  return "i16_le";
            }
            return "u8";
        };
        std::size_t emitted = 0;
        for (auto const &s : layout) {
            if (s.verification != cb::Verification::Verified)
                continue;
            ++emitted;
            std::printf("[[pid]]\n");
            std::printf("monitor_id        = \"%.*s\"\n",
                        static_cast<int>(s.monitor_id.size()),
                        s.monitor_id.data());
            std::printf("name              = \"%.*s\"\n",
                        static_cast<int>(s.name.size()), s.name.data());
            std::printf("did               = 0x%04X\n", s.did);
            std::printf("byte_offset       = %u\n",
                        static_cast<unsigned>(s.byte_offset));
            std::printf("data_type         = \"%s\"\n", storage_str(s.storage));
            std::printf("unit              = \"%.*s\"\n",
                        static_cast<int>(s.unit.size()), s.unit.data());
            std::printf("scaling.formula   = \"linear\"\n");
            std::printf("scaling.factor    = %.10g\n", s.scale);
            std::printf("scaling.offset    = %.4f\n", s.offset);
            std::printf("scaling_ssm       = \"%.*s\"\n",
                        static_cast<int>(s.scaling.size()), s.scaling.data());
            std::printf("ssm_address       = 0x%08X\n", s.ram_address);
            std::printf("ram_address_status = \"%s\"\n",
                        s.ram_authoritative ? "authoritative" : "candidate");
            std::puts("");
        }
        std::printf("# %zu Verified PIDs emitted.\n", emitted);
        std::puts("# RAM addresses (`ssm_address`) above are LF79103P-specific.");
        std::puts("# For other A-series CIDs, the byte positions + wire scales");
        std::puts("# port directly; the RAM addresses do not. Per-CID packs");
        std::puts("# exist for LF75404H, LF75404S, LF75600H, LF79103P, LF9C102P,");
        std::puts("# LF9D012H, LF9G003T, LF9L000E; LF75600H has 3 monitors with");
        std::puts("# no high-confidence catalog match.");
        return 0;
    }
    if (json_mode) {
        std::string out;
        out.reserve(8192);
        out.append("{\"schema\":\"subuwutuner.did-datalog-preset.v1\",");
        out.append("\"firmware\":");
        json_escape(out, firmware_label);
        out.append(",");
        out.append("\"request_can_id\":\"0x");
        char hexbuf[16];
        std::snprintf(hexbuf, sizeof hexbuf, "%03X", cb::kRequestCanId);
        out.append(hexbuf);
        out.append("\",\"response_can_id\":\"0x");
        std::snprintf(hexbuf, sizeof hexbuf, "%03X", cb::kResponseCanId);
        out.append(hexbuf);
        out.append("\",\"median_poll_interval_ms\":");
        out.append(std::to_string(cb::kMedianPollIntervalMs));
        out.append(",\"did_payloads\":[");
        for (std::size_t i = 0; i < cb::kDidPayloads.size(); ++i) {
            if (i != 0) out.append(",");
            std::snprintf(hexbuf, sizeof hexbuf, "0x%04X",
                          cb::kDidPayloads[i].did);
            out.append("{\"did\":\"");
            out.append(hexbuf);
            out.append("\",\"bytes\":");
            out.append(std::to_string(cb::kDidPayloads[i].bytes));
            out.append("}");
        }
        out.append("],\"total_payload_bytes\":");
        out.append(std::to_string(cb::total_payload_bytes(firmware)));
        out.append(",\"signals\":[");
        auto const storage_name = [](cb::SignalStorage s) -> char const * {
            switch (s) {
            case cb::SignalStorage::Uint8:    return "uint8";
            case cb::SignalStorage::Int8:     return "int8";
            case cb::SignalStorage::Uint16:   return "uint16_be";
            case cb::SignalStorage::Int16:    return "int16_be";
            case cb::SignalStorage::Uint16Le: return "uint16_le";
            case cb::SignalStorage::Int16Le:  return "int16_le";
            }
            return "unknown";
        };
        bool first = true;
        char addrbuf[16];
        char numbuf[32];
        for (auto const &s : layout) {
            if (!first) out.append(",");
            first = false;
            std::snprintf(hexbuf, sizeof hexbuf, "0x%04X", s.did);
            out.append("{\"did\":\"");
            out.append(hexbuf);
            out.append("\",\"byte_offset\":");
            out.append(std::to_string(s.byte_offset));
            out.append(",\"storage\":\"");
            out.append(storage_name(s.storage));
            out.append("\",\"ram_address\":\"");
            std::snprintf(addrbuf, sizeof addrbuf, "0x%08X", s.ram_address);
            out.append(addrbuf);
            out.append("\",\"name\":");
            json_escape(out, s.name);
            out.append(",\"unit\":");
            json_escape(out, s.unit);
            out.append(",\"scaling\":");
            json_escape(out, s.scaling);
            out.append(",\"verification\":\"");
            out.append(s.verification == cb::Verification::Verified
                           ? "verified"
                           : "hypothesized");
            out.append("\",\"scale\":");
            std::snprintf(numbuf, sizeof numbuf, "%.10g", s.scale);
            out.append(numbuf);
            out.append(",\"offset\":");
            std::snprintf(numbuf, sizeof numbuf, "%.10g", s.offset);
            out.append(numbuf);
            out.append(",\"monitor_id\":");
            json_escape(out, s.monitor_id);
            out.append(",\"ram_authoritative\":");
            out.append(s.ram_authoritative ? "true" : "false");
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }
    std::printf("Aftermarket-datalogger SSM-extended DID protocol — %s\n",
                firmware_label);
    std::puts("");
    std::printf("  Request CAN id:    0x%03X (tester → ECU)\n",
                cb::kRequestCanId);
    std::printf("  Response CAN id:   0x%03X (ECU → tester)\n",
                cb::kResponseCanId);
    std::printf("  UDS service:       0x%02X ReadDataByIdentifier\n",
                cb::kReadDataByIdentifier);
    std::printf("  Poll cadence:      ~%u ms median (~25 Hz)\n",
                cb::kMedianPollIntervalMs);
    std::printf("  Request shape:     22 F3 00 F3 01 F3 02 F3 03 F3 04 (13 bytes)\n");
    std::printf("  Response payload:  %zu bytes total (ISO-TP multi-frame)\n",
                cb::total_payload_bytes(firmware));
    std::puts("");
    std::puts("  DID payload widths:");
    for (auto const &p : cb::did_payloads(firmware)) {
        std::printf("    0x%04X — %2u bytes\n", p.did, p.bytes);
    }
    std::puts("");
    std::printf("Signal layout (%zu signals):\n", layout.size());
    std::uint16_t last_did = 0;
    for (auto const &s : layout) {
        if (s.did != last_did) {
            std::printf("\n  0x%04X:\n", s.did);
            last_did = s.did;
        }
        char const *storage = "?";
        switch (s.storage) {
        case cb::SignalStorage::Uint8:    storage = "u8";    break;
        case cb::SignalStorage::Int8:     storage = "i8";    break;
        case cb::SignalStorage::Uint16:   storage = "u16be"; break;
        case cb::SignalStorage::Int16:    storage = "i16be"; break;
        case cb::SignalStorage::Uint16Le: storage = "u16le"; break;
        case cb::SignalStorage::Int16Le:  storage = "i16le"; break;
        }
        char const *verification =
            (s.verification == cb::Verification::Verified) ? "✓" : "?";
        std::printf("    [%2u] %s %-5s 0x%08X  %.*s (%.*s)\n"
                    "         catalog: %.*s\n",
                    s.byte_offset, verification, storage, s.ram_address,
                    static_cast<int>(s.name.size()), s.name.data(),
                    static_cast<int>(s.unit.size()), s.unit.data(),
                    static_cast<int>(s.scaling.size()), s.scaling.data());
        if (s.verification == cb::Verification::Verified) {
            std::printf("         wire:    raw * %.6g + %.3f\n",
                        s.scale, s.offset);
            std::printf("         monitor: %.*s (%s RAM)\n",
                        static_cast<int>(s.monitor_id.size()),
                        s.monitor_id.data(),
                        s.ram_authoritative ? "authoritative" : "candidate");
        }
    }
    std::puts("");
    std::puts("RAM addresses are LF79103P-specific (the same logical signal");
    std::puts("on a different CID may live at a different address). Scaling");
    std::puts("is the catalog raw→engineering expression; variable 'x' is the");
    std::puts("raw value (sign-extended for signed storage).");
    std::puts("Protocol shape confirmed; per-byte mapping high-confidence");
    std::puts("pending on-car driving-data ground-truth.");
    return 0;
}

// list-validators — surfaces the static inventory of st::policy
// flash-preflight validators. Answers "what does the policy gate
// actually check?" without having to read the source. JSON envelope:
// subuwutuner.list-validators.v1.
int cmd_list_validators(int argc, char *argv[]) {
    bool json_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--json") {
            json_mode = true;
        } else {
            std::fprintf(stderr, "list-validators: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    auto const validators = st::policy::available_validators();
    if (json_mode) {
        std::string out;
        out.reserve(2048);
        out.append("{\"schema\":\"subuwutuner.list-validators.v1\",\"count\":");
        out.append(std::to_string(validators.size()));
        out.append(",\"validators\":[");
        for (std::size_t i = 0; i < validators.size(); ++i) {
            if (i != 0)
                out.append(",");
            auto const &v = validators[i];
            out.append("{\"category\":");
            json_escape(out, v.category);
            out.append(",\"name\":");
            json_escape(out, v.name);
            out.append(",\"description\":");
            json_escape(out, v.description);
            out.append(",\"default_thresholds\":");
            json_escape(out, v.default_thresholds);
            out.append(",\"skip_when\":");
            json_escape(out, v.skip_when);
            out.append("}");
        }
        out.append("]}\n");
        std::fputs(out.c_str(), stdout);
        return 0;
    }
    std::printf("Flash pre-flight validators (default_pipeline order — "
                "%zu validators):\n",
                validators.size());
    std::puts("");
    for (auto const &v : validators) {
        std::printf("  %.*s — %.*s\n",
                    static_cast<int>(v.name.size()), v.name.data(),
                    static_cast<int>(v.category.size()), v.category.data());
        std::printf("    %.*s\n",
                    static_cast<int>(v.description.size()), v.description.data());
        if (!v.default_thresholds.empty()) {
            std::printf("    Thresholds: %.*s\n",
                        static_cast<int>(v.default_thresholds.size()),
                        v.default_thresholds.data());
        }
        if (!v.skip_when.empty()) {
            std::printf("    No-op when: %.*s\n",
                        static_cast<int>(v.skip_when.size()),
                        v.skip_when.data());
        }
        std::puts("");
    }
    std::puts("Every validator runs (no fail-fast). Blocker-tier "
              "diagnostics refuse the flash regardless of profile.");
    std::puts("See docs/05-improvements.md §4 + docs/06-legal-ethics.md "
              "for the policy posture.");
    return 0;
}

int cmd_rom_pull(int argc, char *argv[]) {
    std::optional<std::uint32_t> addr;
    std::optional<std::uint32_t> size;
    // Default --max-chunk = 0x1000 (4 KB) — the documented Gen-A.2
    // ECU response-buffer ceiling per `findings/uds-read-workflow.md`
    // §6. With the upfront chunk-size probe in `Flasher::read_full_rom`,
    // this is the START of the ladder; the probe walks 0x1000 → 0x800
    // → 0x400 → 0x200 → 0x100 until the ECU accepts a size.
    std::uint32_t max_chunk = 0x1000;
    bool auto_probe = true;   // --no-probe disables the upfront ladder
    bool probe_only = false;  // --probe-only runs probe, prints size, exits
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::string> transport_kind;
    std::string dll_path;
    std::string device_path;
    // SA / DSC defaults: real-hardware (--transport) path needs both; the
    // trace-replay (--trace) path expects the captured exchange to be
    // exactly what gets sent, so both default OFF there. Either can be
    // toggled with --authenticate / --no-authenticate and --enter-dsc /
    // --no-enter-dsc.
    std::optional<bool> authenticate_flag;
    std::optional<bool> enter_dsc_flag;
    std::uint8_t security_level = 0x01;
    std::optional<std::string> sa_variant;  // --sa-variant <name>.

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rom-pull: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--addr") {
            auto const *v = require_arg("--addr");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val)) {
                std::fprintf(stderr, "rom-pull: --addr must be a hex or decimal "
                                     "integer\n");
                return 2;
            }
            addr = val;
        } else if (a == "--size") {
            auto const *v = require_arg("--size");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fprintf(stderr, "rom-pull: --size must be a positive "
                                     "hex or decimal integer\n");
                return 2;
            }
            size = val;
        } else if (a == "--max-chunk") {
            auto const *v = require_arg("--max-chunk");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fprintf(stderr, "rom-pull: --max-chunk must be a positive "
                                     "hex or decimal integer\n");
                return 2;
            }
            max_chunk = val;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v)
                trace_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--transport") {
            if (auto const *v = require_arg("--transport"); v)
                transport_kind = std::string{v};
            else
                return 2;
        } else if (a == "--dll") {
            if (auto const *v = require_arg("--dll"); v)
                dll_path = v;
            else
                return 2;
        } else if (a == "--device") {
            if (auto const *v = require_arg("--device"); v)
                device_path = v;
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--authenticate") {
            authenticate_flag = true;
        } else if (a == "--no-authenticate") {
            authenticate_flag = false;
        } else if (a == "--enter-dsc") {
            enter_dsc_flag = true;
        } else if (a == "--no-enter-dsc") {
            enter_dsc_flag = false;
        } else if (a == "--security-level") {
            auto const *v = require_arg("--security-level");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0 || val > 0xFFU) {
                std::fprintf(stderr, "rom-pull: --security-level must be a "
                                     "positive 8-bit hex or decimal integer\n");
                return 2;
            }
            security_level = static_cast<std::uint8_t>(val);
        } else if (a == "--no-probe") {
            auto_probe = false;
        } else if (a == "--probe") {
            auto_probe = true;
        } else if (a == "--probe-only") {
            probe_only = true;
            auto_probe = true; // implied — there's nothing else to do
        } else if (a == "--sa-variant") {
            if (auto const *v = require_arg("--sa-variant"); v) {
                sa_variant = std::string{v};
            } else {
                return 2;
            }
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-pull: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "rom-pull: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!addr.has_value() || !size.has_value()) {
        std::fputs("rom-pull: missing --addr / --size\n"
                   "Usage: subuwutuner-cli rom-pull --addr <hex> --size <hex>\n"
                   "         [--output <FILE.bin>]                "
                   "(default: <rom_dump_root>/subuwutuner-rom-<UTC-timestamp>.bin)\n"
                   "         (--trace <FILE.uds> | --transport <kind>\n"
                   "                               [--dll <path>] [--device <path>])\n"
                   "         [--max-chunk <hex>]                  "
                   "(default 0x1000 — upper bound for the probe ladder)\n"
                   "         [--no-probe|--probe]                 "
                   "(default ON — auto-shrink chunk size at boot)\n"
                   "         [--probe-only]                       "
                   "(run probe, print discovered size, exit)\n"
                   "         [--authenticate|--no-authenticate]   "
                   "(default ON for --transport, OFF for --trace)\n"
                   "         [--enter-dsc|--no-enter-dsc]         "
                   "(default ON for --transport, OFF for --trace)\n"
                   "         [--security-level <hex>]             "
                   "(default 0x01 — bootloader unlock)\n"
                   "         [--sa-variant <name>]                "
                   "(default | aftermarket[-l1] | aftermarket-l3; see docs/23)\n",
                   stderr);
        return 2;
    }
    // Resolve --sa-variant to a key function. Allow `default` as an
    // explicit synonym for the factory L1 path so scripts can be
    // explicit about which variant they're using.
    //
    // Deprecated aliases (`fehr-active{,-l1,-l3}`) are accepted for one
    // release cycle but emit a stderr warning. New tooling should pass
    // the `aftermarket{,-l1,-l3}` names directly.
    st::ecu::SecurityKeyFn sa_variant_fn;
    if (sa_variant.has_value()) {
        if (*sa_variant == "default" || *sa_variant == "factory") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_key_stub;
        } else if (*sa_variant == "aftermarket" ||
                   *sa_variant == "aftermarket-l1") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket;
        } else if (*sa_variant == "aftermarket-l3") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l3_aftermarket;
        } else if (*sa_variant == "fehr-active" ||
                   *sa_variant == "fehr-active-l1") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket;
            std::fputs("rom-pull: warning: --sa-variant 'fehr-active[-l1]' "
                       "is a deprecated alias for 'aftermarket[-l1]'; will "
                       "be removed next release.\n",
                       stderr);
        } else if (*sa_variant == "fehr-active-l3") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l3_aftermarket;
            std::fputs("rom-pull: warning: --sa-variant 'fehr-active-l3' "
                       "is a deprecated alias for 'aftermarket-l3'; will "
                       "be removed next release.\n",
                       stderr);
        } else if (*sa_variant == "aftermarket-v1-flash") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket_v1_flash;
        } else if (*sa_variant == "aftermarket-v1-maf-sd") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket_v1_maf_sd;
        } else if (*sa_variant == "ssmv-factory") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_ssmv_factory;
        } else if (*sa_variant == "ecutek" || *sa_variant == "ecutek-l1") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l1_ecutek;
            std::fputs(
                "rom-pull: note: --sa-variant 'ecutek' is EXPERIMENTAL "
                "per ζ2 (no captured\n"
                "  (seed, key) pair on file). The S-box differential "
                "+ determinism are pinned by\n"
                "  unit tests but no live-ECU validation. Use at your "
                "own risk; report\n"
                "  successes / failures so the variant can be promoted "
                "out of experimental.\n",
                stderr);
        } else if (*sa_variant == "ecutek-l3" || *sa_variant == "ecutek-l35") {
            sa_variant_fn = &st::ecu::subaru::ssmcan1_l35_ecutek;
            std::fputs(
                "rom-pull: note: --sa-variant 'ecutek-l3' is "
                "EXPERIMENTAL per ζ2.\n",
                stderr);
        } else {
            std::fprintf(stderr,
                         "rom-pull: --sa-variant '%s' not recognized "
                         "(expected one of: default, aftermarket, "
                         "aftermarket-l1, aftermarket-l3, cobb-flash, "
                         "cobb-maf-sd, ssmv-factory, ecutek[-l1], "
                         "ecutek-l3).\n",
                         sa_variant->c_str());
            return 2;
        }
    }
    // Default --output to <rom_dump_root>/subuwutuner-rom-<UTC>.bin per
    // docs/25 §7. The CID isn't known until after the read, so the
    // timestamp goes in the filename; the user (or downstream tooling)
    // can rename to <CID>.bin once rom-identify confirms it. Creates the
    // parent directory if it doesn't exist (rom_dump_root is typically
    // under %USERPROFILE%\\Documents\\..., which is writable but may
    // not have been created yet on a fresh install).
    if (!output_path.has_value()) {
        auto const dump_root = st::config::rom_dump_root();
        // UTC ISO 8601-like compact form, safe for filenames.
        std::time_t const now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y%m%dT%H%M%SZ", &tm);
        std::string fname = "subuwutuner-rom-";
        fname += ts;
        fname += ".bin";
        output_path = dump_root / fname;
        std::error_code ec;
        std::filesystem::create_directories(dump_root, ec);
        if (ec) {
            std::fprintf(stderr,
                         "rom-pull: cannot create rom_dump_root %s: %s\n",
                         dump_root.string().c_str(), ec.message().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "rom-pull: no --output given, using "
                     "rom_dump_root from config: %s\n",
                     output_path->string().c_str());
    }
    bool const have_trace = trace_path.has_value();
    bool const have_transport = transport_kind.has_value();
    if (have_trace == have_transport) {
        std::fputs("rom-pull: must pass exactly one of --trace or "
                   "--transport (they're mutually exclusive — --trace is "
                   "the offline MockTransport-replayed path; --transport "
                   "is the real-adapter path).\n",
                   stderr);
        return 2;
    }

    // Owning storage for whichever transport we end up using. Mock
    // path stays as a stack object below; the factory path stashes
    // its unique_ptr here. `chosen` is the non-owning pointer the
    // rest of the function uses regardless.
    st::transport::MockTransport mock;
    std::unique_ptr<st::transport::ITransport> owned;
    st::transport::ITransport *chosen = nullptr;

    if (have_trace) {
        std::vector<UdsTracePair> pairs;
        std::string err;
        if (!parse_uds_trace(*trace_path, pairs, err)) {
            std::fputs(err.c_str(), stderr);
            std::fputc('\n', stderr);
            return 1;
        }
        if (auto s = mock.open({}); !s.has_value()) {
            std::fprintf(stderr, "rom-pull: mock open failed: %s\n", s.error().to_string().c_str());
            return 1;
        }
        for (auto &p : pairs) {
            mock.expect_send_recv(std::move(p.request), std::move(p.response));
        }
        chosen = &mock;
    } else {
        // --transport path. parse kind, build a TransportSpec, ask
        // the factory. obdx + native go through the platform serial
        // byte channel today; j2534 still routes through the
        // NotImplemented J2534Library::load() — surface either error
        // verbatim so the user sees exactly what's missing.
        auto const kind = st::transport::parse_kind(*transport_kind);
        if (!kind.has_value()) {
            std::fprintf(stderr,
                         "rom-pull: --transport '%s' not recognized "
                         "(expected one of: j2534, obdx, native).\n",
                         transport_kind->c_str());
            return 2;
        }
        st::transport::TransportSpec const spec{*kind, dll_path, device_path};
        auto t = st::transport::open_transport(spec);
        if (!t.has_value()) {
            std::fprintf(stderr, "rom-pull: %s\n", t.error().to_string().c_str());
            return 1;
        }
        // Default LinkConfig: assume UDS-over-CAN with the standard
        // Subaru OBD-II IDs. Once we have more transport-aware ECU
        // detection (or `--link kline|can` + baud flags) this comes
        // off the command line. For VB WRX this is the right shape;
        // for VA WRX you'd use --trace today (and --link kline once
        // K-Line is plumbed end-to-end).
        st::transport::LinkConfig const link{st::transport::LinkKind::CanIso15765, 500000,
                                             0x000007E0, 0x000007E8};
        if (auto s = (*t)->open(link); !s.has_value()) {
            std::fprintf(stderr, "rom-pull: transport open failed: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        owned = std::move(*t);
        chosen = owned.get();
    }

    // Defaults: --transport (real hardware) needs DSC + SA; --trace
    // (replay) expects the captured exchange to drive the wire byte-for-
    // byte, so both default OFF there. Explicit flags override.
    bool const authenticate = authenticate_flag.value_or(have_transport);
    bool const enter_dsc = enter_dsc_flag.value_or(have_transport);

    // Real hardware over CAN needs ISO-TP SSM framing too — but
    // read_full_rom uses UDS RMBA, not SSM, so SSM framing doesn't apply
    // here. Leaving the Flasher's default KLine setting alone since this
    // path never touches the SSM client.
    st::flash::Flasher flasher{*chosen};
    if (sa_variant_fn) {
        flasher.set_security_key_fn(sa_variant_fn);
        std::fprintf(stderr, "rom-pull: SA variant: %s\n", sa_variant->c_str());
    }
    // --probe-only short-circuit: drive DSC + SA so the ECU is in the
    // right state to honor RMBA, then walk the chunk-size ladder once,
    // print the discovered size, and exit. This is a characterization
    // tool; intentionally separate from --no-probe (which dumps with a
    // fixed user-supplied size).
    if (probe_only) {
        if (enter_dsc) {
            if (auto s = flasher.client().diagnostic_session_control(
                    st::ecu::uds::kDscExtendedDiagnostic, std::chrono::milliseconds{1000});
                !s.has_value()) {
                std::fprintf(stderr, "rom-pull --probe-only: DSC entry failed: %s\n",
                             s.error().to_string().c_str());
                return 1;
            }
        }
        if (authenticate) {
            auto seed = flasher.client().security_access_request_seed(
                security_level, std::chrono::milliseconds{1000});
            if (!seed.has_value()) {
                std::fprintf(stderr, "rom-pull --probe-only: SA requestSeed failed: %s\n",
                             seed.error().to_string().c_str());
                return 1;
            }
            // Reuse whatever SA fn the Flasher was configured with above.
            auto key = sa_variant_fn ? sa_variant_fn(*seed)
                                     : st::ecu::subaru::ssmcan1_key_stub(*seed);
            if (!key.has_value()) {
                std::fprintf(stderr, "rom-pull --probe-only: SA key derivation failed: %s\n",
                             key.error().to_string().c_str());
                return 1;
            }
            auto const send_sub = static_cast<std::uint8_t>(security_level + 1U);
            if (auto s = flasher.client().security_access_send_key(
                    send_sub, *key, std::chrono::milliseconds{1000});
                !s.has_value()) {
                std::fprintf(stderr, "rom-pull --probe-only: SA sendKey failed: %s\n",
                             s.error().to_string().c_str());
                return 1;
            }
        }
        auto probed = flasher.probe_max_chunk(*addr, max_chunk, std::chrono::milliseconds{1000});
        if (!probed.has_value()) {
            std::fprintf(stderr, "rom-pull --probe-only: %s\n",
                         probed.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stdout,
                     "rom-pull: probe at 0x%08X (hint 0x%X) settled on chunk 0x%X\n",
                     *addr, static_cast<unsigned>(max_chunk),
                     static_cast<unsigned>(*probed));
        return 0;
    }

    auto const r = flasher.read_full_rom(*addr, *size, max_chunk,
                                          std::chrono::milliseconds{1000},
                                          /*progress=*/nullptr,
                                          /*cancel=*/nullptr,
                                          enter_dsc, authenticate, security_level,
                                          auto_probe);
    if (!r.has_value()) {
        std::fprintf(stderr, "rom-pull: %s\n", r.error().to_string().c_str());
        return 1;
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "rom-pull: cannot open output: %s\n", output_path->string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(r->data()), static_cast<std::streamsize>(r->size()));
    if (!out) {
        std::fprintf(stderr, "rom-pull: write failed: %s\n", output_path->string().c_str());
        return 1;
    }

    // Trace-only warning: any unused trace entries indicate the
    // captured exchange was longer than the requested read shape.
    // For the --transport path the underlying ITransport doesn't
    // have an "exhausted" notion, so skip the warning there.
    if (have_trace && !mock.exhausted()) {
        std::fprintf(stderr, "rom-pull: warning: %zu trace entries unused\n", mock.remaining());
    }
    std::fprintf(stderr,
                 "rom-pull: read %u bytes from 0x%08X in chunks of %u; "
                 "wrote %s\n",
                 static_cast<unsigned>(*size), *addr, static_cast<unsigned>(max_chunk),
                 output_path->string().c_str());
    return 0;
}

// ReadDataByIdentifier (UDS SID 0x22) one-shot CLI. The shipped GUI
// Read ROM flow always does RMBA (SID 0x23) which gets gated behind
// SecurityAccess on a stock Subaru. RDBI for well-known emissions
// DIDs (VIN, calibration ID, software version) is allowed in the
// default session on every OBD-II-compliant ECU, so this is the best
// way to ground-truth the transport + UDS stack against live hardware
// without needing SA.
//
// More importantly, the canonical VIN read (`22 F1 90`) returns 17 ASCII
// bytes + 3 header bytes = 20 bytes total, which exceeds the 7-byte
// CAN single-frame limit and forces the ECU into multi-frame ISO-TP.
// That validates the strip_iso_tp First-Frame path in the OBDX
// transport which is otherwise untested against real hardware.
int cmd_rdbi(int argc, char *argv[]) {
    std::optional<std::string> transport_kind;
    std::string dll_path;
    std::string device_path;
    std::optional<std::uint32_t> did;
    std::optional<std::filesystem::path> output_path;
    bool enter_dsc = true;
    bool verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rdbi: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--transport") {
            if (auto const *v = require_arg("--transport"); v)
                transport_kind = std::string{v};
            else
                return 2;
        } else if (a == "--dll") {
            if (auto const *v = require_arg("--dll"); v)
                dll_path = v;
            else
                return 2;
        } else if (a == "--device") {
            if (auto const *v = require_arg("--device"); v)
                device_path = v;
            else
                return 2;
        } else if (a == "--did") {
            auto const *v = require_arg("--did");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val > 0xFFFFU) {
                std::fputs("rdbi: --did must be a 16-bit hex value (e.g. 0xF190 for VIN)\n",
                           stderr);
                return 2;
            }
            did = val;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--no-dsc") {
            enter_dsc = false;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rdbi: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "rdbi: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!transport_kind.has_value() || !did.has_value()) {
        std::fputs(
            "rdbi: missing --transport / --did\n"
            "Usage: subuwutuner-cli rdbi --transport <kind> --did <hex>\n"
            "                            [--device <path>] [--dll <path>]\n"
            "                            [--output FILE.bin] [--no-dsc] [--verbose]\n"
            "\n"
            "Read one UDS DataByIdentifier (SID 0x22). Defaults enter the\n"
            "extendedDiagnosticSession (DSC 0x03) before the request, then\n"
            "RDBI. Output is hex + ASCII to stderr; raw response bytes to\n"
            "--output if given. --no-dsc skips the session entry (some\n"
            "emissions DIDs work in the default session).\n"
            "\n"
            "Useful DIDs (mandatory OBD-II, not SA-gated):\n"
            "  0xF190  VIN (17 ASCII bytes - forces multi-frame ISO-TP RX)\n"
            "  0xF188  ECU software number (calibration ID)\n"
            "  0xF195  System Supplier ECU Software Version Number\n"
            "  0xF196  System Supplier ECU HW Version Number\n"
            "  0xF18C  ECU serial number\n"
            "  0xF18A  System Supplier Identifier\n",
            stderr);
        return 2;
    }

    auto const kind = st::transport::parse_kind(*transport_kind);
    if (!kind.has_value()) {
        std::fprintf(stderr,
                     "rdbi: --transport '%s' not recognized (expected j2534|obdx|native).\n",
                     transport_kind->c_str());
        return 2;
    }
    st::transport::TransportSpec const spec{*kind, dll_path, device_path};
    auto t = st::transport::open_transport(spec);
    if (!t.has_value()) {
        std::fprintf(stderr, "rdbi: %s\n", t.error().to_string().c_str());
        return 1;
    }
    if (verbose)
        st::transport::obdx::set_trace_enabled(true);

    st::transport::LinkConfig link{};
    link.kind = st::transport::LinkKind::CanIso15765;
    link.baud = 500000;
    if (auto s = (*t)->open(link); !s.has_value()) {
        std::fprintf(stderr, "rdbi: transport open failed: %s\n", s.error().to_string().c_str());
        return 1;
    }

    st::ecu::uds::UdsClient client{**t};

    if (enter_dsc) {
        if (auto s = client.diagnostic_session_control(st::ecu::uds::kDscExtendedDiagnostic);
            !s.has_value()) {
            std::fprintf(stderr, "rdbi: DSC extended session entry failed: %s\n",
                         s.error().to_string().c_str());
            (void)(*t)->close();
            return 1;
        }
    }

    auto const r = client.read_data_by_identifier(static_cast<std::uint16_t>(*did));
    if (!r.has_value()) {
        std::fprintf(stderr, "rdbi: %s\n", r.error().to_string().c_str());
        (void)(*t)->close();
        return 1;
    }
    auto const &data = *r;

    // Format: header line + hex dump + ASCII rendering. ASCII swap
    // unprintables for '.' so VIN-style responses are immediately
    // recognizable while non-printable responses don't trash the
    // terminal.
    std::fprintf(stderr, "rdbi: DID 0x%04X returned %zu bytes\n",
                 static_cast<unsigned>(*did), data.size());
    std::fprintf(stderr, "  hex:   ");
    for (std::size_t i = 0; i < data.size(); ++i) {
        std::fprintf(stderr, "%02X%s", static_cast<unsigned>(data[i]),
                     (i + 1 < data.size()) ? " " : "");
    }
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  ascii: \"");
    for (auto b : data) {
        std::fputc((b >= 0x20 && b <= 0x7E) ? static_cast<int>(b) : '.', stderr);
    }
    std::fprintf(stderr, "\"\n");

    if (output_path.has_value()) {
        std::ofstream out{*output_path, std::ios::binary};
        if (!out) {
            std::fprintf(stderr, "rdbi: cannot open output: %s\n",
                         output_path->string().c_str());
            (void)(*t)->close();
            return 1;
        }
        out.write(reinterpret_cast<char const *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            std::fprintf(stderr, "rdbi: write failed: %s\n", output_path->string().c_str());
            (void)(*t)->close();
            return 1;
        }
        std::fprintf(stderr, "rdbi: wrote %zu bytes to %s\n", data.size(),
                     output_path->string().c_str());
    }

    (void)(*t)->close();
    return 0;
}

// OBD-II Mode 09 vehicle-info query. Most useful Subaru-side for
// fetching the calibration ID (CAL ID), which lets the user identify
// which tune is currently active on the ECU without dumping the full
// ROM. Mode 09 is in the default session and SA-free, so the command
// is intentionally short: open transport, send `09 PID`, print
// response.
int cmd_obd_info(int argc, char *argv[]) {
    std::optional<std::string> transport_kind;
    std::string dll_path;
    std::string device_path;
    std::optional<std::string> pid_name;
    bool verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "obd-info: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--transport") {
            if (auto const *v = require_arg("--transport"); v)
                transport_kind = std::string{v};
            else
                return 2;
        } else if (a == "--dll") {
            if (auto const *v = require_arg("--dll"); v)
                dll_path = v;
            else
                return 2;
        } else if (a == "--device") {
            if (auto const *v = require_arg("--device"); v)
                device_path = v;
            else
                return 2;
        } else if (a == "--pid") {
            if (auto const *v = require_arg("--pid"); v)
                pid_name = std::string{v};
            else
                return 2;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "obd-info: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "obd-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!transport_kind.has_value() || !pid_name.has_value()) {
        std::fputs(
            "obd-info: missing --transport / --pid\n"
            "Usage: subuwutuner-cli obd-info --transport <kind> --pid <name>\n"
            "                                [--device <path>] [--dll <path>] [--verbose]\n"
            "\n"
            "Send OBD-II Mode 0x09 (vehicle information) and print the\n"
            "ECU's response. Lives in the default session, no SA required.\n"
            "Supported PIDs:\n"
            "  cal-id   PID 0x04 — 16-byte calibration ID (e.g. 'LF79102P')\n"
            "  cvn      PID 0x06 — 4-byte calibration verification number\n"
            "  vin      PID 0x02 — 17-byte vehicle identification number\n",
            stderr);
        return 2;
    }

    std::uint8_t pid_byte = 0;
    std::size_t pid_width = 0;
    if (*pid_name == "cal-id" || *pid_name == "calid") {
        pid_byte = st::ecu::uds::kObd2PidCalibrationId;
        pid_width = 16;
    } else if (*pid_name == "cvn") {
        pid_byte = st::ecu::uds::kObd2PidCvn;
        pid_width = 4;
    } else if (*pid_name == "vin") {
        pid_byte = st::ecu::uds::kObd2PidVin;
        pid_width = 17;
    } else {
        std::fprintf(stderr,
                     "obd-info: --pid '%s' not recognized (expected cal-id|cvn|vin).\n",
                     pid_name->c_str());
        return 2;
    }

    auto const kind = st::transport::parse_kind(*transport_kind);
    if (!kind.has_value()) {
        std::fprintf(stderr,
                     "obd-info: --transport '%s' not recognized (expected j2534|obdx|native).\n",
                     transport_kind->c_str());
        return 2;
    }
    st::transport::TransportSpec const spec{*kind, dll_path, device_path};
    auto t = st::transport::open_transport(spec);
    if (!t.has_value()) {
        std::fprintf(stderr, "obd-info: %s\n", t.error().to_string().c_str());
        return 1;
    }
    if (verbose)
        st::transport::obdx::set_trace_enabled(true);

    st::transport::LinkConfig link{};
    link.kind = st::transport::LinkKind::CanIso15765;
    link.baud = 500000;
    if (auto s = (*t)->open(link); !s.has_value()) {
        std::fprintf(stderr, "obd-info: transport open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }

    st::ecu::uds::UdsClient client{**t};
    auto const r = client.obd2_vehicle_info(pid_byte, pid_width);
    if (!r.has_value()) {
        std::fprintf(stderr, "obd-info: %s\n", r.error().to_string().c_str());
        (void)(*t)->close();
        return 1;
    }

    // Print each message: hex line + ASCII rendering (NULs and
    // non-printables shown as '.' so CAL ID padding doesn't trash the
    // terminal but the actual ID stays readable).
    std::fprintf(stderr, "obd-info: PID 0x%02X returned %zu message(s)\n",
                 static_cast<unsigned>(pid_byte), r->size());
    for (std::size_t i = 0; i < r->size(); ++i) {
        auto const &msg = (*r)[i];
        std::fprintf(stderr, "  [%zu] hex:   ", i);
        for (std::size_t j = 0; j < msg.size(); ++j) {
            std::fprintf(stderr, "%02X%s", static_cast<unsigned>(msg[j]),
                         (j + 1 < msg.size()) ? " " : "");
        }
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "      ascii: \"");
        for (auto b : msg) {
            std::fputc((b >= 0x20 && b <= 0x7E) ? static_cast<int>(b) : '.', stderr);
        }
        std::fprintf(stderr, "\"\n");
    }

    (void)(*t)->close();
    return 0;
}

// SIGINT handler for cmd_sniff. The shape (function-pointer that flips
// an atomic) is the only safe thing to do inside a signal handler per
// POSIX — and Windows's SIGINT runs in a separate thread anyway, so the
// atomic is the cross-platform sync primitive that works for both.
//
// Function-scope rather than file-scope so it doesn't bleed into other
// commands; cmd_sniff installs + restores around its blocking wait.
namespace {
std::atomic<bool> g_sniff_stop{false};
void sniff_sigint_handler(int /*sig*/) {
    g_sniff_stop.store(true, std::memory_order_release);
}
} // namespace

int cmd_sniff(int argc, char *argv[]) {
    std::optional<std::string> transport_kind;
    std::string dll_path;
    std::string device_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::uint32_t> duration_seconds;
    std::vector<std::uint32_t> id_filter;
    bool verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sniff: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--transport") {
            if (auto const *v = require_arg("--transport"); v)
                transport_kind = std::string{v};
            else
                return 2;
        } else if (a == "--dll") {
            if (auto const *v = require_arg("--dll"); v)
                dll_path = v;
            else
                return 2;
        } else if (a == "--device") {
            if (auto const *v = require_arg("--device"); v)
                device_path = v;
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--duration") {
            auto const *v = require_arg("--duration");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fputs("sniff: --duration must be a positive integer (seconds)\n", stderr);
                return 2;
            }
            duration_seconds = val;
        } else if (a == "--filter") {
            auto const *v = require_arg("--filter");
            if (v == nullptr)
                return 2;
            // Comma-separated CAN IDs in hex or decimal. The sniff
            // captures everything from the bus regardless (no adapter
            // filter), but we apply this filter in the writer callback
            // to keep the output file small. Example for SA capture:
            // --filter 0x7E0,0x7E8 keeps only engine ECU traffic.
            std::string_view rest{v};
            while (!rest.empty()) {
                auto const comma = rest.find(',');
                std::string_view const tok =
                    comma == std::string_view::npos ? rest : rest.substr(0, comma);
                std::string const tok_str{tok};
                std::uint32_t val = 0;
                if (!parse_uint32_arg(tok_str.c_str(), val)) {
                    std::fprintf(stderr, "sniff: --filter token '%s' is not a valid CAN ID\n",
                                 tok_str.c_str());
                    return 2;
                }
                id_filter.push_back(val);
                if (comma == std::string_view::npos)
                    break;
                rest = rest.substr(comma + 1);
            }
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "sniff: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "sniff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!transport_kind.has_value() || !output_path.has_value()) {
        std::fputs("sniff: missing --transport / --output\n"
                   "Usage: subuwutuner-cli sniff --transport <kind> --output <FILE.log>\n"
                   "                             [--device <path>] [--dll <path>]\n"
                   "                             [--filter <id>[,<id>...]] [--duration <sec>]\n"
                   "                             [--verbose|-v]\n"
                   "\n"
                   "Passive CAN bus monitor. Opens the adapter in LISTEN-ONLY mode\n"
                   "(no TX), records every CAN frame to --output until Ctrl+C\n"
                   "(or --duration seconds elapsed). Output format is one frame\n"
                   "per line: '<elapsed_ms> 0xCANID <hex bytes>'.\n"
                   "\n"
                   "Intended for SecurityAccess seed/key capture (run while any\n"
                   "other active tester is connected via a Y-cable), DBC reverse\n"
                   "engineering, or general bus exploration. Safe to run on a live\n"
                   "vehicle — listen-only mode means the adapter never transmits.\n"
                   "\n"
                   "--verbose dumps every OBDX DVI exchange (open handshake, filter\n"
                   "  install, EnableNetwork, unsolicited RX pushes) to stderr.\n"
                   "  Use when sniff reports 'captured 0 frames' so we can see\n"
                   "  whether the adapter / filter / network path is at fault.\n",
                   stderr);
        return 2;
    }

    if (verbose) {
        st::transport::obdx::set_trace_enabled(true);
    }

    auto const kind = st::transport::parse_kind(*transport_kind);
    if (!kind.has_value()) {
        std::fprintf(stderr,
                     "sniff: --transport '%s' not recognized "
                     "(expected one of: j2534, obdx, native).\n",
                     transport_kind->c_str());
        return 2;
    }
    st::transport::TransportSpec const spec{*kind, dll_path, device_path};
    auto t = st::transport::open_transport(spec);
    if (!t.has_value()) {
        std::fprintf(stderr, "sniff: %s\n", t.error().to_string().c_str());
        return 1;
    }
    // listen_only=true: skip CAN filter setup (capture everything) +
    // EnableNetwork LISTEN-ONLY (adapter never TXes, safe to share the
    // bus with another active tester via a Y-cable).
    st::transport::LinkConfig link{};
    link.kind = st::transport::LinkKind::CanIso15765;
    link.baud = 500000;
    link.listen_only = true;
    if (auto s = (*t)->open(link); !s.has_value()) {
        std::fprintf(stderr, "sniff: transport open failed: %s\n", s.error().to_string().c_str());
        return 1;
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "sniff: cannot open output: %s\n", output_path->string().c_str());
        return 1;
    }
    // Header — `tools/extract_subaru_sa.py` parses this so the version
    // bump signals a format change. Comment lines start with `#`.
    out << "# subuwutuner sniff log v1\n";
    out << "# format: <elapsed_ms> 0x<can_id> <hex bytes...>\n";

    auto const start_time = std::chrono::steady_clock::now();
    std::mutex out_mutex; // serializes the writer callback + the final stats append
    std::atomic<std::uint64_t> frames_captured{0};

    // Frame callback fires on the OBDX streaming thread — keep it fast.
    auto const writer = [&](st::transport::Frame const &f) {
        // Apply CLI-side filter (the adapter is wide-open in sniff mode).
        if (!id_filter.empty()) {
            bool matched = false;
            for (auto wanted : id_filter) {
                if (wanted == f.can_id) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return;
        }
        auto const now = std::chrono::steady_clock::now();
        auto const elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

        std::string line;
        line.reserve(48 + 3 * f.data.size());
        line.append(std::to_string(elapsed_ms));
        line.push_back('\t');
        // Right-pad CAN ID to 3 hex digits for 11-bit so columns align
        // for typical Subaru traffic; longer IDs render naturally.
        char id_buf[12];
        std::snprintf(id_buf, sizeof id_buf, "0x%03X", f.can_id);
        line.append(id_buf);
        line.push_back('\t');
        for (std::size_t i = 0; i < f.data.size(); ++i) {
            char b_buf[4];
            std::snprintf(b_buf, sizeof b_buf, "%02X", static_cast<unsigned>(f.data[i]));
            line.append(b_buf);
            if (i + 1 < f.data.size())
                line.push_back(' ');
        }
        line.push_back('\n');

        std::scoped_lock lk{out_mutex};
        out << line;
        frames_captured.fetch_add(1, std::memory_order_relaxed);
    };

    if (auto s = (*t)->start_streaming(writer); !s.has_value()) {
        std::fprintf(stderr, "sniff: start_streaming failed: %s\n", s.error().to_string().c_str());
        return 1;
    }

    // Install SIGINT handler so Ctrl+C signals a clean exit. Save the
    // previous handler and restore after — be polite to whoever's
    // shelling out to this CLI.
    g_sniff_stop.store(false, std::memory_order_release);
    auto const prev_handler = std::signal(SIGINT, sniff_sigint_handler);

    std::fprintf(stderr,
                 "sniff: capturing to %s — press Ctrl+C to stop%s\n",
                 output_path->string().c_str(),
                 duration_seconds ? (" (or wait " + std::to_string(*duration_seconds) +
                                     "s for auto-stop)")
                                        .c_str()
                                  : "");

    auto const deadline =
        duration_seconds
            ? std::optional<std::chrono::steady_clock::time_point>{
                  start_time + std::chrono::seconds{*duration_seconds}}
            : std::nullopt;
    while (!g_sniff_stop.load(std::memory_order_acquire)) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    (void)std::signal(SIGINT, prev_handler);

    if (auto s = (*t)->stop_streaming(); !s.has_value()) {
        std::fprintf(stderr, "sniff: stop_streaming failed: %s\n", s.error().to_string().c_str());
        // Continue anyway — we want the file flushed.
    }
    if (auto s = (*t)->close(); !s.has_value()) {
        std::fprintf(stderr, "sniff: close failed: %s\n", s.error().to_string().c_str());
    }

    auto const total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    {
        std::scoped_lock lk{out_mutex};
        out << "# captured " << frames_captured.load(std::memory_order_relaxed) << " frames in "
            << total_ms << " ms\n";
    }
    std::fprintf(stderr,
                 "sniff: captured %" PRIu64 " frames in %lld ms -> %s\n",
                 frames_captured.load(std::memory_order_relaxed),
                 static_cast<long long>(total_ms), output_path->string().c_str());
    return 0;
}

// SIGINT handler for cmd_ssm_a8_poll. Same shape as cmd_sniff — atomic
// flag flipped from the signal handler, polled by the main loop. Kept
// command-local so it doesn't bleed into other commands.
namespace {
std::atomic<bool> g_ssm_a8_poll_stop{false};
void ssm_a8_poll_sigint_handler(int /*sig*/) {
    g_ssm_a8_poll_stop.store(true, std::memory_order_release);
}
} // namespace

int cmd_ssm_a8_poll(int argc, char *argv[]) {
    std::optional<std::string> transport_kind;
    std::string dll_path;
    std::string device_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::uint32_t> duration_seconds;
    std::uint32_t interval_ms = 333;  // ~3 Hz, matches typical aftermarket-flasher F3xx cadence
    std::uint32_t timeout_ms = 500;
    std::vector<std::uint32_t> addresses;
    bool verbose = false;

    // SA-prelude flags (default OFF — preserves the original "just send A8"
    // behavior on factory ECUs where SSM-A8 isn't gated). Opt in when the
    // ECU returns NRC 0x33 on every poll, indicating aftermarket-style lockdown.
    std::optional<bool> authenticate_flag;
    std::optional<bool> enter_dsc_flag;
    std::optional<std::uint8_t> security_level_flag;
    std::optional<std::string> sa_variant;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ssm-a8-poll: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--transport") {
            if (auto const *v = require_arg("--transport"); v)
                transport_kind = std::string{v};
            else
                return 2;
        } else if (a == "--dll") {
            if (auto const *v = require_arg("--dll"); v)
                dll_path = v;
            else
                return 2;
        } else if (a == "--device") {
            if (auto const *v = require_arg("--device"); v)
                device_path = v;
            else
                return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v)
                output_path = std::filesystem::path{v};
            else
                return 2;
        } else if (a == "--duration") {
            auto const *v = require_arg("--duration");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fputs("ssm-a8-poll: --duration must be a positive integer (seconds)\n",
                           stderr);
                return 2;
            }
            duration_seconds = val;
        } else if (a == "--interval-ms") {
            auto const *v = require_arg("--interval-ms");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fputs("ssm-a8-poll: --interval-ms must be a positive integer\n", stderr);
                return 2;
            }
            interval_ms = val;
        } else if (a == "--timeout-ms") {
            auto const *v = require_arg("--timeout-ms");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fputs("ssm-a8-poll: --timeout-ms must be a positive integer\n", stderr);
                return 2;
            }
            timeout_ms = val;
        } else if (a == "--addr") {
            // Accept repeated --addr AND comma-separated lists in one
            // flag, so the natural FastECU-style multi-monitor recipe
            // ('--addr 0xE,0xF,0x8,0x9,0xFE7') is as ergonomic as the
            // long form.
            auto const *v = require_arg("--addr");
            if (v == nullptr)
                return 2;
            for (auto const &tok : split_csv_list(v)) {
                if (tok.empty()) {
                    std::fputs("ssm-a8-poll: --addr contains an empty token\n", stderr);
                    return 2;
                }
                std::uint32_t val = 0;
                if (!parse_uint32_arg(tok.c_str(), val) || val > st::ecu::ssm::kMaxAddress) {
                    std::fprintf(stderr,
                                 "ssm-a8-poll: --addr '%s' is not a 24-bit hex/decimal address\n",
                                 tok.c_str());
                    return 2;
                }
                addresses.push_back(val);
            }
        } else if (a == "--authenticate") {
            authenticate_flag = true;
        } else if (a == "--no-authenticate") {
            authenticate_flag = false;
        } else if (a == "--enter-dsc") {
            enter_dsc_flag = true;
        } else if (a == "--no-enter-dsc") {
            enter_dsc_flag = false;
        } else if (a == "--security-level") {
            auto const *v = require_arg("--security-level");
            if (v == nullptr)
                return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0 || val > 0xFFU) {
                std::fputs("ssm-a8-poll: --security-level must be a positive "
                           "8-bit hex or decimal integer\n",
                           stderr);
                return 2;
            }
            security_level_flag = static_cast<std::uint8_t>(val);
        } else if (a == "--sa-variant") {
            if (auto const *v = require_arg("--sa-variant"); v) {
                sa_variant = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "ssm-a8-poll: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "ssm-a8-poll: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!transport_kind.has_value() || !output_path.has_value() || addresses.empty()) {
        std::fputs(
            "ssm-a8-poll: missing --transport / --output / --addr\n"
            "Usage: subuwutuner-cli ssm-a8-poll --transport <kind> --output <FILE.log>\n"
            "                                   --addr <hex>[,<hex>...] [--addr <hex> ...]\n"
            "                                   [--device <path>] [--dll <path>]\n"
            "                                   [--duration <sec>] [--interval-ms <ms>]\n"
            "                                   [--timeout-ms <ms>] [--verbose|-v]\n"
            "                                   [--authenticate|--no-authenticate]\n"
            "                                   [--enter-dsc|--no-enter-dsc]\n"
            "                                   [--security-level <hex>]\n"
            "                                   [--sa-variant <name>]\n"
            "\n"
            "Reads one byte per --addr via OEM SSM-0xA8 over ISO-15765 every\n"
            "--interval-ms (default 333). Writes a comment-headered log with\n"
            "per-poll lines '<elapsed_ms>\\t<HH HH ...>' for downstream\n"
            "correlation against captured aftermarket-datalogger F3xx response\n"
            "bytes.\n"
            "\n"
            "If the ECU returns NRC 0x33 on every poll, SSM-A8 is SA-gated\n"
            "(observed on aftermarket-tuned ECUs that null-redirect OEM service\n"
            "handlers). Pass --authenticate to do the UDS DSC + SA dance\n"
            "before polling starts. --sa-variant names the key function\n"
            "(default | aftermarket[-l1] | aftermarket-l3; see docs/23).\n"
            "--security-level overrides the SA level (default tracks the\n"
            "variant: 0x03 for aftermarket-l3, 0x01 otherwise).\n",
            stderr);
        return 2;
    }

    if (verbose) {
        st::transport::obdx::set_trace_enabled(true);
    }

    auto const kind = st::transport::parse_kind(*transport_kind);
    if (!kind.has_value()) {
        std::fprintf(stderr,
                     "ssm-a8-poll: --transport '%s' not recognized "
                     "(expected one of: j2534, obdx, native).\n",
                     transport_kind->c_str());
        return 2;
    }

    // Resolve SA defaults + the variant key function up front so bad
    // flags fail fast without opening the transport. Final decision on
    // whether to run the preamble lives at the open-transport call site.
    bool const authenticate = authenticate_flag.value_or(false);
    bool const enter_dsc = enter_dsc_flag.value_or(authenticate);
    auto const variant_uses_l3 =
        sa_variant.has_value() &&
        (*sa_variant == "aftermarket-l3" || *sa_variant == "fehr-active-l3");
    std::uint8_t const security_level = security_level_flag.value_or(
        variant_uses_l3 ? 0x03 : 0x01);

    st::ecu::SecurityKeyFn sa_fn = &st::ecu::subaru::ssmcan1_key_stub;
    char const *sa_label = "factory L1";
    if (sa_variant.has_value()) {
        if (*sa_variant == "default" || *sa_variant == "factory") {
            // already set above
        } else if (*sa_variant == "aftermarket" ||
                   *sa_variant == "aftermarket-l1") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket;
            sa_label = "aftermarket L1";
        } else if (*sa_variant == "aftermarket-l3") {
            sa_fn = &st::ecu::subaru::ssmcan1_l3_aftermarket;
            sa_label = "aftermarket L3";
        } else if (*sa_variant == "fehr-active" ||
                   *sa_variant == "fehr-active-l1") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket;
            sa_label = "aftermarket L1";
            std::fputs("ssm-a8-poll: warning: --sa-variant "
                       "'fehr-active[-l1]' is a deprecated alias for "
                       "'aftermarket[-l1]'.\n",
                       stderr);
        } else if (*sa_variant == "fehr-active-l3") {
            sa_fn = &st::ecu::subaru::ssmcan1_l3_aftermarket;
            sa_label = "aftermarket L3";
            std::fputs("ssm-a8-poll: warning: --sa-variant "
                       "'fehr-active-l3' is a deprecated alias for "
                       "'aftermarket-l3'.\n",
                       stderr);
        } else if (*sa_variant == "aftermarket-v1-flash") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket_v1_flash;
            sa_label = "COBB-flash L1";
        } else if (*sa_variant == "aftermarket-v1-maf-sd") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_aftermarket_v1_maf_sd;
            sa_label = "COBB-MAF-SD L1";
        } else if (*sa_variant == "ssmv-factory") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_ssmv_factory;
            sa_label = "SSM-V factory L1";
        } else if (*sa_variant == "ecutek" || *sa_variant == "ecutek-l1") {
            sa_fn = &st::ecu::subaru::ssmcan1_l1_ecutek;
            sa_label = "EcuTek L1 (experimental)";
            std::fputs("ssm-a8-poll: note: --sa-variant 'ecutek' is "
                       "EXPERIMENTAL per ζ2 (no captured pair).\n",
                       stderr);
        } else if (*sa_variant == "ecutek-l3" || *sa_variant == "ecutek-l35") {
            sa_fn = &st::ecu::subaru::ssmcan1_l35_ecutek;
            sa_label = "EcuTek L3 (experimental)";
            std::fputs("ssm-a8-poll: note: --sa-variant 'ecutek-l3' is "
                       "EXPERIMENTAL per ζ2 (no captured pair).\n",
                       stderr);
        } else {
            std::fprintf(stderr,
                         "ssm-a8-poll: --sa-variant '%s' not recognized "
                         "(expected: default | aftermarket[-l1] | "
                         "aftermarket-l3 | cobb-flash | cobb-maf-sd | "
                         "ssmv-factory | ecutek[-l1] | ecutek-l3).\n",
                         sa_variant->c_str());
            return 2;
        }
    }

    // Build the request once and report it back so the user can sanity-
    // check the wire bytes before the loop fires — same A8 frame every
    // poll, MockTransport-friendly for tests.
    auto const req = st::ecu::ssm::build_a8_request(addresses, st::ecu::ssm::Framing::IsoTp);
    if (!req.has_value()) {
        std::fprintf(stderr, "ssm-a8-poll: cannot build A8 request: %s\n",
                     req.error().to_string().c_str());
        return 1;
    }

    st::transport::TransportSpec const spec{*kind, dll_path, device_path};
    auto t = st::transport::open_transport(spec);
    if (!t.has_value()) {
        std::fprintf(stderr, "ssm-a8-poll: %s\n", t.error().to_string().c_str());
        return 1;
    }
    st::transport::LinkConfig link{};
    link.kind = st::transport::LinkKind::CanIso15765;
    link.baud = 500000;
    if (auto s = (*t)->open(link); !s.has_value()) {
        std::fprintf(stderr, "ssm-a8-poll: transport open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }

    // SA preamble. authenticate / enter_dsc / sa_fn / sa_label /
    // security_level were resolved before the transport opened so bad
    // flags failed fast. Default authenticate=false leaves the original
    // "send A8 directly" path intact for factory ECUs.
    if (enter_dsc || authenticate) {
        st::ecu::uds::UdsClient uds_client{**t};
        if (enter_dsc) {
            if (auto s = uds_client.diagnostic_session_control(
                    st::ecu::uds::kDscExtendedDiagnostic);
                !s.has_value()) {
                std::fprintf(stderr,
                             "ssm-a8-poll: DSC extendedDiagnostic failed: %s\n",
                             s.error().to_string().c_str());
                (void)(*t)->close();
                return 1;
            }
            std::fputs("ssm-a8-poll: entered extendedDiagnosticSession (0x03)\n", stderr);
        }
        if (authenticate) {
            std::fprintf(stderr,
                         "ssm-a8-poll: SecurityAccess: variant=%s level=0x%02X\n",
                         sa_label, static_cast<unsigned>(security_level));
            auto const seed = uds_client.security_access_request_seed(security_level);
            if (!seed.has_value()) {
                std::fprintf(stderr, "ssm-a8-poll: requestSeed failed: %s\n",
                             seed.error().to_string().c_str());
                (void)(*t)->close();
                return 1;
            }
            auto const key = sa_fn(*seed);
            if (!key.has_value()) {
                std::fprintf(stderr, "ssm-a8-poll: SA key fn failed: %s\n",
                             key.error().to_string().c_str());
                (void)(*t)->close();
                return 1;
            }
            // sendKey sub_function is requestSeed + 1 per ISO 14229.
            if (auto s = uds_client.security_access_send_key(
                    static_cast<std::uint8_t>(security_level + 1), *key);
                !s.has_value()) {
                std::fprintf(stderr, "ssm-a8-poll: sendKey failed: %s\n",
                             s.error().to_string().c_str());
                (void)(*t)->close();
                return 1;
            }
            std::fputs("ssm-a8-poll: SecurityAccess unlocked\n", stderr);
        }
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "ssm-a8-poll: cannot open output: %s\n",
                     output_path->string().c_str());
        (void)(*t)->close();
        return 1;
    }

    // Header. Schema-versioned so a downstream consumer can refuse on
    // unrecognized format bumps. Address list goes in a single comment
    // line for grep-ability; per-poll lines carry only the bytes (one
    // per address, request order) so the file stays compact at high
    // poll rates.
    out << "# subuwutuner ssm-a8-poll log v1\n";
    out << "# transport: " << *transport_kind << "\n";
    out << "# framing: ssm-a8 over iso15765 (tester 0x7E0, ecu 0x7E8, 500 kbps)\n";
    out << "# interval_ms: " << interval_ms << "\n";
    if (authenticate) {
        out << "# sa: variant=" << sa_label
            << " level=0x" << std::hex << static_cast<unsigned>(security_level)
            << std::dec << "\n";
    } else {
        out << "# sa: none\n";
    }
    out << "# addresses(BE24):";
    for (auto a : addresses) {
        char buf[12];
        std::snprintf(buf, sizeof buf, " 0x%06X", static_cast<unsigned>(a));
        out << buf;
    }
    out << "\n";
    out << "# format: <elapsed_ms>\\t<HH HH ...>  (one byte per address, request order)\n";

    st::ecu::ssm::SsmClient client{**t, st::ecu::ssm::Framing::IsoTp};

    g_ssm_a8_poll_stop.store(false, std::memory_order_release);
    auto const prev_handler = std::signal(SIGINT, ssm_a8_poll_sigint_handler);

    std::fprintf(stderr,
                 "ssm-a8-poll: polling %zu address%s every %u ms to %s — Ctrl+C to stop%s\n",
                 addresses.size(), addresses.size() == 1 ? "" : "es",
                 static_cast<unsigned>(interval_ms), output_path->string().c_str(),
                 duration_seconds
                     ? (" (auto-stop in " + std::to_string(*duration_seconds) + "s)").c_str()
                     : "");

    auto const start_time = std::chrono::steady_clock::now();
    auto const deadline =
        duration_seconds
            ? std::optional<std::chrono::steady_clock::time_point>{
                  start_time + std::chrono::seconds{*duration_seconds}}
            : std::nullopt;

    std::uint64_t polls_ok = 0;
    std::uint64_t polls_err = 0;
    auto next_poll = start_time;

    while (!g_ssm_a8_poll_stop.load(std::memory_order_acquire)) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline)
            break;

        auto const r =
            client.read(addresses, std::chrono::milliseconds{timeout_ms});
        auto const now = std::chrono::steady_clock::now();
        auto const elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

        if (r.has_value()) {
            ++polls_ok;
            out << elapsed_ms << '\t' << hex_bytes_line(*r) << '\n';
        } else {
            ++polls_err;
            // Log errors inline so the correlator can skip them without
            // losing time-position. '!' prefix distinguishes from data
            // rows (which start with a decimal digit).
            out << elapsed_ms << "\t! " << r.error().to_string() << '\n';
        }

        // Schedule next poll on a fixed grid (not "interval since last
        // response") so jitter doesn't pile up. If a single poll
        // overran the interval, reset the grid to (now + interval) so
        // we don't run flat-out catching up — a struggling bus would
        // otherwise see N back-to-back zero-sleep polls after one slow
        // response, making things worse.
        next_poll += std::chrono::milliseconds{interval_ms};
        if (next_poll < std::chrono::steady_clock::now()) {
            next_poll =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{interval_ms};
        }
        auto const sleep_until_time = std::max(next_poll, std::chrono::steady_clock::now());
        while (!g_ssm_a8_poll_stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < sleep_until_time) {
            if (deadline && std::chrono::steady_clock::now() >= *deadline)
                break;
            // Wake every 50 ms so Ctrl+C / deadline get noticed promptly.
            auto const slice = std::min<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds{50},
                sleep_until_time - std::chrono::steady_clock::now());
            std::this_thread::sleep_for(slice);
        }
    }

    (void)std::signal(SIGINT, prev_handler);
    (void)(*t)->close();

    auto const total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    out << "# polled " << polls_ok << " times, " << polls_err << " errors, in " << total_ms
        << " ms\n";

    std::fprintf(stderr,
                 "ssm-a8-poll: %" PRIu64 " polls (%" PRIu64 " errors) in %lld ms -> %s\n",
                 polls_ok, polls_err, static_cast<long long>(total_ms),
                 output_path->string().c_str());
    return polls_err > 0 && polls_ok == 0 ? 1 : 0;
}

int cmd_lint_graph(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("lint-graph: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli lint-graph <FILE.stmod> [--strict]\n", stderr);
        return 2;
    }
    bool strict = false;
    std::optional<std::filesystem::path> path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a = argv[i];
        if (a == "--strict") {
            strict = true;
            continue;
        }
        if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "lint-graph: unknown flag '%.*s'\n", static_cast<int>(a.size()),
                         a.data());
            return 2;
        }
        if (path.has_value()) {
            std::fprintf(stderr, "lint-graph: unexpected extra argument '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        path = std::filesystem::path{a};
    }
    if (!path.has_value()) {
        std::fputs("lint-graph: missing path\n", stderr);
        return 2;
    }
    std::ifstream ifs{*path, std::ios::binary};
    if (!ifs) {
        std::fprintf(stderr, "lint-graph: cannot open '%s'\n", path->string().c_str());
        return 1;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    auto g = st::feature::from_toml(buf.str());
    if (!g.has_value()) {
        std::fprintf(stderr, "lint-graph: parse failed: %s\n", g.error().to_string().c_str());
        return 1;
    }
    if (auto v = g->validate(); !v.has_value()) {
        std::fprintf(stderr, "lint-graph: structural error: %s\n", v.error().to_string().c_str());
        return 1;
    }
    auto const findings = st::feature::lint(*g);
    if (findings.empty()) {
        std::printf("OK (%zu node%s, %zu edge%s)\n", g->nodes().size(),
                    g->nodes().size() == 1 ? "" : "s", g->edges().size(),
                    g->edges().size() == 1 ? "" : "s");
        return 0;
    }
    std::printf("%zu warning%s:\n", findings.size(), findings.size() == 1 ? "" : "s");
    for (auto const &f : findings) {
        std::printf("  - %s\n", f.message.c_str());
    }
    return strict ? 3 : 0;
}

int cmd_dump_ir(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("dump-ir: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli dump-ir <FILE.stmod>\n", stderr);
        return 2;
    }
    std::filesystem::path const path{argv[0]};
    std::ifstream ifs{path, std::ios::binary};
    if (!ifs) {
        std::fprintf(stderr, "dump-ir: cannot open '%s'\n", path.string().c_str());
        return 1;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    auto g = st::feature::from_toml(buf.str());
    if (!g.has_value()) {
        std::fprintf(stderr, "dump-ir: parse failed: %s\n", g.error().to_string().c_str());
        return 1;
    }
    auto m = st::feature::ir::lower(*g);
    if (!m.has_value()) {
        std::fprintf(stderr, "dump-ir: lowering failed: %s\n", m.error().to_string().c_str());
        return 1;
    }
    std::fputs(st::feature::ir::dump(*m).c_str(), stdout);
    return 0;
}

int cmd_lint_ir(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("lint-ir: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli lint-ir <FILE.stmod> "
                   "[--budget N] [--strict]\n",
                   stderr);
        return 2;
    }
    std::filesystem::path path;
    bool strict = false;
    std::size_t budget = 200;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--strict") {
            strict = true;
        } else if (a == "--budget") {
            if (i + 1 >= argc) {
                std::fputs("lint-ir: --budget needs a value\n", stderr);
                return 2;
            }
            char *end = nullptr;
            auto const v = std::strtoull(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0') {
                std::fprintf(stderr,
                             "lint-ir: --budget '%s' not a non-negative "
                             "integer\n",
                             argv[i + 1]);
                return 2;
            }
            budget = static_cast<std::size_t>(v);
            ++i;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "lint-ir: unknown flag '%.*s'\n", static_cast<int>(a.size()),
                         a.data());
            return 2;
        } else if (path.empty()) {
            path = argv[i];
        } else {
            std::fprintf(stderr, "lint-ir: unexpected positional '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    if (path.empty()) {
        std::fputs("lint-ir: missing path\n", stderr);
        return 2;
    }
    std::ifstream ifs{path, std::ios::binary};
    if (!ifs) {
        std::fprintf(stderr, "lint-ir: cannot open '%s'\n", path.string().c_str());
        return 1;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    auto g = st::feature::from_toml(buf.str());
    if (!g.has_value()) {
        std::fprintf(stderr, "lint-ir: parse failed: %s\n", g.error().to_string().c_str());
        return 1;
    }
    auto m = st::feature::ir::lower(*g);
    if (!m.has_value()) {
        std::fprintf(stderr, "lint-ir: lowering failed: %s\n", m.error().to_string().c_str());
        return 1;
    }
    auto const findings = st::feature::ir::lint(*m, st::feature::ir::LintOptions{budget});
    if (findings.empty()) {
        std::printf("lint-ir: clean (cost %zu cycles, budget %zu)\n",
                    st::feature::ir::estimate_cost(*m), budget);
        return 0;
    }
    std::printf("lint-ir: %zu finding%s\n", findings.size(), findings.size() == 1 ? "" : "s");
    for (auto const &f : findings) {
        if (f.instruction_index.has_value()) {
            std::printf("  - [%zu] %s\n", *f.instruction_index, f.message.c_str());
        } else {
            std::printf("  - %s\n", f.message.c_str());
        }
    }
    return strict ? 3 : 0;
}

namespace {

// Render a PatchObject as a per-hook hex dump. Header lines start with
// '#'; bytes lay out 16 per row with an offset column. Each hook
// trailer summarizes its RAM claims.
std::string render_patch_hex(st::feature::codegen::PatchObject const &p) {
    std::string out;
    out.reserve(256 + 80 * p.hooks.size());
    char buf[96];
    std::snprintf(buf, sizeof(buf), "# arch: %s\n", st::feature::codegen::arch_name(p.arch));
    out.append(buf);
    if (p.hooks.empty()) {
        out.append("# (no hooks)\n");
        return out;
    }
    for (auto const &h : p.hooks) {
        std::snprintf(buf, sizeof(buf), "# hook: %s @ 0x%zX (%zu bytes)\n", h.symbol.c_str(),
                      h.splice_address, h.code.size());
        out.append(buf);
        for (std::size_t i = 0; i < h.code.size(); i += 16) {
            std::snprintf(buf, sizeof(buf), "%04zX:", i);
            out.append(buf);
            std::size_t const end = std::min(i + 16, h.code.size());
            for (std::size_t j = i; j < end; ++j) {
                std::snprintf(buf, sizeof(buf), " %02X", static_cast<unsigned>(h.code[j]));
                out.append(buf);
            }
            out.append("\n");
        }
        for (auto const &rc : h.ram_claims) {
            std::snprintf(buf, sizeof(buf), "# ram_claim: 0x%zX size=%zu align=%zu\n", rc.address,
                          rc.size, rc.alignment);
            out.append(buf);
        }
    }
    return out;
}

} // namespace

int cmd_feature_compile(int argc, char *argv[]) {
    std::optional<std::filesystem::path> stmod_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::string> arch_override;
    std::string format = "hex";
    bool validate_only = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("feature-compile: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--output") {
            if (i + 1 >= argc) {
                std::fputs("feature-compile: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a == "--arch") {
            if (i + 1 >= argc) {
                std::fputs("feature-compile: --arch requires a value\n", stderr);
                return 2;
            }
            arch_override = std::string{argv[++i]};
        } else if (a == "--format") {
            if (i + 1 >= argc) {
                std::fputs("feature-compile: --format requires a value\n", stderr);
                return 2;
            }
            format = std::string{argv[++i]};
            if (format != "hex" && format != "toml" && format != "raw" && format != "stmod") {
                std::fprintf(stderr,
                             "feature-compile: --format must be one of "
                             "hex|toml|raw|stmod (got '%s')\n",
                             format.c_str());
                return 2;
            }
        } else if (a == "--validate-only") {
            // Parse + lower + compile, then exit with a status code
            // without emitting any output. Lets CI / pre-commit hooks
            // confirm a .stmod still builds cleanly against a pack
            // without producing files or dumping hex on the terminal.
            // Distinct from "--output /dev/null", which would still
            // render the patch + traverse the format dispatch.
            validate_only = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "feature-compile: unknown flag '%.*s'\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        } else if (!stmod_path.has_value()) {
            stmod_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "feature-compile: unexpected positional '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!stmod_path.has_value()) {
        std::fputs("feature-compile: missing .stmod path\n", stderr);
        std::fputs("Usage: subuwutuner-cli feature-compile <FILE.stmod> "
                   "--def <pack.toml> [--arch sh2a|rh850] "
                   "[--format hex|toml|raw|stmod] [--output <FILE>] "
                   "[--validate-only]\n",
                   stderr);
        return 2;
    }
    if (!def_path.has_value()) {
        std::fputs("feature-compile: --def is required\n", stderr);
        return 2;
    }
    if (validate_only && output_path.has_value()) {
        std::fputs("feature-compile: --validate-only and --output cannot "
                   "be combined (validate-only suppresses all output)\n",
                   stderr);
        return 2;
    }
    if (format == "raw" && !output_path.has_value() && !validate_only) {
        std::fputs("feature-compile: --format=raw requires --output "
                   "(refusing to write binary to a terminal)\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(resolve_def_path(*def_path));
    if (!def.has_value()) {
        return print_def_load_error("feature-compile", *def_path, def.error());
    }

    std::ifstream ifs{*stmod_path, std::ios::binary};
    if (!ifs) {
        std::fprintf(stderr, "feature-compile: cannot open '%s'\n", stmod_path->string().c_str());
        return 1;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    auto g = st::feature::from_toml(buf.str());
    if (!g.has_value()) {
        std::fprintf(stderr, "feature-compile: parse failed: %s\n", g.error().to_string().c_str());
        return 1;
    }
    auto m = st::feature::ir::lower(*g);
    if (!m.has_value()) {
        std::fprintf(stderr, "feature-compile: lowering failed: %s\n",
                     m.error().to_string().c_str());
        return 1;
    }

    std::unique_ptr<st::feature::codegen::IBackend> backend;
    if (arch_override.has_value()) {
        // Explicit arch name — bypass platform detection. Keeps the
        // CLI usable against demo packs whose platform string doesn't
        // map through select_backend()'s VA/VB recognizer.
        std::string a = *arch_override;
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (a == "sh2a") {
            backend = std::make_unique<st::feature::codegen::Sh2aBackend>();
        } else if (a == "rh850") {
            backend = std::make_unique<st::feature::codegen::Rh850Backend>();
        } else {
            std::fprintf(stderr,
                         "feature-compile: --arch must be sh2a or rh850 "
                         "(got '%s')\n",
                         arch_override->c_str());
            return 2;
        }
    } else {
        auto sel = st::feature::codegen::select_backend(*def);
        if (!sel.has_value()) {
            std::fprintf(stderr, "feature-compile: %s\n", sel.error().to_string().c_str());
            std::fputs("(pass --arch sh2a|rh850 to override platform "
                       "detection)\n",
                       stderr);
            return 1;
        }
        backend = std::move(*sel);
    }

    auto patch = backend->compile(*m, *def);
    if (!patch.has_value()) {
        std::fprintf(stderr, "feature-compile: compile failed: %s\n",
                     patch.error().to_string().c_str());
        return 1;
    }

    if (validate_only) {
        // Reached only when parse + lower + compile all succeeded. Exit
        // 0 silently — CI hooks key on the exit code, not stdout.
        return 0;
    }

    if (format == "raw") {
        if (patch->hooks.size() != 1) {
            std::fprintf(stderr,
                         "feature-compile: --format=raw needs exactly one "
                         "hook (got %zu)\n",
                         patch->hooks.size());
            return 1;
        }
        std::ofstream ofs{*output_path, std::ios::binary | std::ios::trunc};
        if (!ofs) {
            std::fprintf(stderr,
                         "feature-compile: cannot open '%s' for "
                         "writing\n",
                         output_path->string().c_str());
            return 1;
        }
        auto const &code = patch->hooks.front().code;
        ofs.write(reinterpret_cast<char const *>(code.data()),
                  static_cast<std::streamsize>(code.size()));
        if (!ofs) {
            std::fprintf(stderr, "feature-compile: write to '%s' failed\n",
                         output_path->string().c_str());
            return 1;
        }
        return 0;
    }

    // Format dispatch (raw already handled above):
    //   hex   → per-hook hex dump (default)
    //   toml  → just the [patch] TOML section
    //   stmod → graph TOML + [patch] TOML, single bundled file —
    //           pairs with feature::from_toml + patch_from_toml on
    //           the read side (graph loader ignores [patch], patch
    //           loader ignores [graph]). The graph is re-serialized
    //           through feature::to_toml so the output is canonical
    //           rather than a verbatim copy of the input (loses
    //           hand-edited comments + whitespace; structurally
    //           equivalent).
    std::string text;
    if (format == "toml") {
        text = st::feature::codegen::patch_to_toml(*patch);
    } else if (format == "stmod") {
        text = st::feature::to_toml(*g);
        text.append("\n");
        text.append(st::feature::codegen::patch_to_toml(*patch));
    } else {
        text = render_patch_hex(*patch);
    }

    if (output_path.has_value()) {
        std::ofstream ofs{*output_path, std::ios::binary | std::ios::trunc};
        if (!ofs) {
            std::fprintf(stderr,
                         "feature-compile: cannot open '%s' for "
                         "writing\n",
                         output_path->string().c_str());
            return 1;
        }
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!ofs) {
            std::fprintf(stderr, "feature-compile: write to '%s' failed\n",
                         output_path->string().c_str());
            return 1;
        }
    } else {
        std::fputs(text.c_str(), stdout);
    }
    return 0;
}

// Renders one row of `config show` output with the value + provenance
// (env var / config file / default).
namespace {
void print_config_row(char const *key, std::filesystem::path const &resolved,
                      char const *env_name,
                      std::filesystem::path const &default_value,
                      std::filesystem::path const &config_source_path,
                      bool config_file_present) {
    char const *source = "default";
    std::string source_detail;
    if (char const *e = std::getenv(env_name); e != nullptr && *e != '\0') {
        source = "env";
        source_detail = env_name;
    } else if (config_file_present && resolved != default_value) {
        source = "config";
        source_detail = config_source_path.string();
    }
    if (source_detail.empty()) {
        std::printf("%-18s = \"%s\"  (source: %s)\n", key,
                    resolved.string().c_str(), source);
    } else {
        std::printf("%-18s = \"%s\"  (source: %s %s)\n", key,
                    resolved.string().c_str(), source, source_detail.c_str());
    }
}
} // namespace

int cmd_config(int argc, char *argv[]) {
    constexpr std::string_view kHelp =
        "Usage: subuwutuner-cli config <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  path                       print the config file path "
        "(honors ST_CONFIG_FILE)\n"
        "  show                       dump resolved values + provenance\n"
        "  get <key>                  print one resolved value\n"
        "  set <key> <value>          write one key to the config file\n"
        "  reset                      restore defaults (overwrites the config file)\n"
        "\n"
        "Keys: definitions_root, rom_dump_root\n"
        "\n"
        "See docs/25-config-system.md for precedence rules and the schema.\n";

    if (argc < 1) {
        std::fputs(kHelp.data(), stderr);
        return 2;
    }
    std::string_view const sub{argv[0]};

    if (sub == "-h" || sub == "--help") {
        std::fputs(kHelp.data(), stdout);
        return 0;
    }

    if (sub == "path") {
        std::printf("%s\n", st::config::default_config_path().string().c_str());
        return 0;
    }

    if (sub == "show") {
        auto const cfg_path = st::config::default_config_path();
        bool const present = std::filesystem::exists(cfg_path);
        std::printf("config file: %s%s\n", cfg_path.string().c_str(),
                    present ? "" : "  (not present — defaults in use)\n");
        if (!present) {
            std::printf("\n");
        }

        // Load the config so we can compare resolved value vs default to
        // infer "did the file explicitly set this key" without parsing twice.
        auto cfg_r = st::config::Config::load();
        if (!cfg_r.has_value()) {
            std::fprintf(stderr, "config show: %s\n",
                         cfg_r.error().to_string().c_str());
            return 1;
        }
        auto const defaults = st::config::Config::defaults();

        std::printf("[paths]\n");
        print_config_row("definitions_root",
                         st::config::definitions_root(),
                         "ST_DEFINITIONS_ROOT",
                         defaults.paths().definitions_root,
                         cfg_r->source_path(), present);
        print_config_row("rom_dump_root", st::config::rom_dump_root(),
                         "ST_ROM_DUMP_ROOT",
                         defaults.paths().rom_dump_root,
                         cfg_r->source_path(), present);
        return 0;
    }

    if (sub == "get") {
        if (argc < 2) {
            std::fputs("config get: missing key (definitions_root | rom_dump_root)\n",
                       stderr);
            return 2;
        }
        std::string_view const key{argv[1]};
        if (key == "definitions_root") {
            std::printf("%s\n", st::config::definitions_root().string().c_str());
            return 0;
        }
        if (key == "rom_dump_root") {
            std::printf("%s\n", st::config::rom_dump_root().string().c_str());
            return 0;
        }
        std::fprintf(stderr, "config get: unknown key '%s' "
                              "(valid: definitions_root, rom_dump_root)\n",
                     argv[1]);
        return 2;
    }

    if (sub == "set") {
        if (argc < 3) {
            std::fputs("config set: usage: subuwutuner-cli config set "
                       "<definitions_root|rom_dump_root> <path>\n",
                       stderr);
            return 2;
        }
        std::string_view const key{argv[1]};
        std::filesystem::path const value{argv[2]};
        auto cfg_r = st::config::Config::load();
        if (!cfg_r.has_value()) {
            std::fprintf(stderr, "config set: load failed: %s\n",
                         cfg_r.error().to_string().c_str());
            return 1;
        }
        if (key == "definitions_root") {
            cfg_r->paths().definitions_root = value;
        } else if (key == "rom_dump_root") {
            cfg_r->paths().rom_dump_root = value;
        } else {
            std::fprintf(stderr, "config set: unknown key '%s' "
                                  "(valid: definitions_root, rom_dump_root)\n",
                         argv[1]);
            return 2;
        }
        auto s = cfg_r->save();
        if (!s.has_value()) {
            std::fprintf(stderr, "config set: save failed: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote %s -> %s\n", argv[1], value.string().c_str());
        return 0;
    }

    if (sub == "reset") {
        auto c = st::config::Config::defaults();
        c.set_source_path(st::config::default_config_path());
        auto s = c.save();
        if (!s.has_value()) {
            std::fprintf(stderr, "config reset: save failed: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "config reset: wrote defaults to %s\n",
                     c.source_path().string().c_str());
        return 0;
    }

    std::fprintf(stderr, "config: unknown subcommand '%s'\n", argv[0]);
    std::fputs(kHelp.data(), stderr);
    return 2;
}

// ---------------------------------------------------------------------------
// `ap3` subcommand: COBB AccessPort V3 file vault over USB.
//
// Capability A only — opaque blob-level ls/pull/push/rm + a state
// summary read. Cipher-gated `.ptm` introspection (`ap3 inspect`,
// `ap3 patch-list`) lands behind ST_ENABLE_COBB_AP_CIPHER in a
// follow-up tier (T3 of specs/cobb-ap3-implementation-checklist.md;
// not implemented in this session).
//
// Marriage-state gate: spec §15 leaves the exact byte offset of the
// "Installed / Not Installed" marker inside the cmd 0x03
// DeviceSettings blob undocumented. Until a follow-up analyst session
// pins that down, query_state() returns `married = std::nullopt`
// (meaning "unknown") and this CLI emits a one-line warning rather
// than refusing outright. `--allow-unpaired-vehicle` is wired through for
// forward-compat; today it changes nothing behavioral. When the spec
// is extended, the gate flips on automatically.
// ---------------------------------------------------------------------------

// Runtime arming flag for the .ptm + .img cipher path. Defined here
// (above ets_cli + ptm_cli, both of which consume it) and flipped
// from main's pre-pass on --enable-cobb-ap-cipher.
static bool g_cobb_ap_cipher_armed = false;

namespace ets_cli {

struct CommonOpts {
    bool allow_unpaired{false};
    std::uint16_t vid{st::transport::ets::kVendorId};
    std::uint16_t pid{st::transport::ets::kProductId};
};

// Consume `--format <text|json|toml>` from argv (compacting in place)
// and return the parsed format string. Defaults to "text". Reports the
// caller through stderr on bad input but returns "text" anyway — the
// subcommands treat unknown formats as text. This is the per-subcommand
// helper; consume_common does the same for ap3-wide flags.
std::string consume_format_flag(int &argc, char **argv) {
    std::string out{"text"};
    int write = 0;
    for (int read = 0; read < argc; ++read) {
        std::string_view const a{argv[read]};
        if (a == "--format" && read + 1 < argc) {
            std::string_view const v{argv[read + 1]};
            if (v == "text" || v == "json" || v == "toml") {
                out.assign(v);
            } else {
                std::fprintf(stderr,
                             "ap3: --format must be text|json|toml (got '%.*s'); using text\n",
                             static_cast<int>(v.size()), v.data());
            }
            ++read; // skip the value too
            continue;
        }
        argv[write++] = argv[read];
    }
    argc = write;
    return out;
}

// Parse common flags that may appear in any ap3 subcommand. Consumes
// them out of `argv` in-place by compaction; the surviving args are
// the subcommand-specific positionals.
bool consume_common(int &argc, char **argv, CommonOpts &opts) {
    int write = 0;
    for (int read = 0; read < argc; ++read) {
        std::string_view const a{argv[read]};
        if (a == "--allow-unpaired-vehicle") {
            opts.allow_unpaired = true;
            continue;
        }
        if (a == "--vid" && read + 1 < argc) {
            std::uint32_t v = 0;
            if (!parse_uint32_arg(argv[read + 1], v) || v > 0xFFFFU) {
                std::fputs("ap3: --vid must be a 16-bit hex/decimal value\n", stderr);
                return false;
            }
            opts.vid = static_cast<std::uint16_t>(v);
            ++read;
            continue;
        }
        if (a == "--pid" && read + 1 < argc) {
            std::uint32_t v = 0;
            if (!parse_uint32_arg(argv[read + 1], v) || v > 0xFFFFU) {
                std::fputs("ap3: --pid must be a 16-bit hex/decimal value\n", stderr);
                return false;
            }
            opts.pid = static_cast<std::uint16_t>(v);
            ++read;
            continue;
        }
        argv[write++] = argv[read];
    }
    argc = write;
    return true;
}

st::Result<std::unique_ptr<st::transport::IByteChannel>>
open_channel(CommonOpts const &opts) {
    st::transport::ets::ChannelConfig cfg{};
    cfg.vendor_id = opts.vid;
    cfg.product_id = opts.pid;
    return st::transport::ets::open_channel(cfg);
}

// Walk through query_state, print the summary, and check the marriage
// gate. Returns true when the caller should proceed (with any
// destructive op), false when refused.
bool gate_marriage(st::devices::ets::DeviceState const &state, CommonOpts const &opts) {
    if (state.vehicle_paired.has_value() && !*state.vehicle_paired && !opts.allow_unpaired) {
        std::fputs(
            "ap3: the connected AccessPort reports Not Installed. SubuwuTuner has not\n"
            "     been bench-tested against unmarried devices. Pass --allow-unpaired-vehicle\n"
            "     to bypass this check (at your own risk).\n",
            stderr);
        return false;
    }
    if (!state.vehicle_paired.has_value()) {
        // cmd 0x28 is the sole marriage source per RE2; this branch
        // means its ASCII payload didn't parse (truncation, shape
        // drift in a future firmware, etc.). Warn but proceed —
        // refusing on a parse failure would lock the user out for a
        // CLI-side bug, not an unmarried device.
        std::fputs(
            "ap3: warning — marriage state could not be parsed from the cmd 0x28\n"
            "     UserInfo response. The ASCII payload may be truncated or have\n"
            "     drifted shape. Proceeding anyway. Pass --allow-unpaired-vehicle to\n"
            "     silence this warning.\n",
            stderr);
    }
    return true;
}

int run_state(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 state: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (!state.has_value()) {
        std::fprintf(stderr, "ap3 state: %s\n", state.error().to_string().c_str());
        return 1;
    }
    auto const serial   = state->ap_serial.value_or(std::string{"(unparsed)"});
    auto const firmware = state->firmware_version.value_or(std::string{"(unparsed)"});
    auto const vehicle  = state->vehicle_descriptor.value_or(std::string{"(unparsed)"});
    auto const hw_type  = state->hardware_type.value_or(std::string{"(unparsed)"});
    auto const veh_mfr  = state->vehicle_manufacturer.value_or(std::string{"(unparsed)"});
    auto const ap_mfr   = state->ap_manufacturer.value_or(std::string{"(unparsed)"});
    char const *marriage_str =
        state->vehicle_paired.has_value() ? (*state->vehicle_paired ? "Installed" : "Not Installed")
                                   : "(unparsed)";
    if (fmt == "json") {
        std::printf("{\n");
        std::printf("  \"vid\": %u,\n  \"pid\": %u,\n", opts.vid, opts.pid);
        std::printf("  \"serial\": \"%s\",\n", serial.c_str());
        std::printf("  \"firmware\": \"%s\",\n", firmware.c_str());
        std::printf("  \"vehicle\": \"%s\",\n", vehicle.c_str());
        std::printf("  \"hardware_type\": \"%s\",\n", hw_type.c_str());
        std::printf("  \"vehicle_manufacturer\": \"%s\",\n", veh_mfr.c_str());
        std::printf("  \"ap_manufacturer\": \"%s\",\n", ap_mfr.c_str());
        std::printf("  \"married\": %s,\n",
                    state->vehicle_paired.has_value() ? (*state->vehicle_paired ? "true" : "false") : "null");
        std::printf("  \"user_info_body_bytes\": %zu,\n", state->user_info_body.size());
        std::printf("  \"firmware_body_bytes\": %zu,\n", state->firmware_body.size());
        std::printf("  \"device_settings_body_bytes\": %zu,\n", state->device_settings_body.size());
        std::printf("  \"hardware_type_body_bytes\": %zu,\n", state->hardware_type_body.size());
        std::printf("  \"vehicle_manufacturer_body_bytes\": %zu,\n",
                    state->vehicle_manufacturer_body.size());
        std::printf("  \"ap_manufacturer_body_bytes\": %zu\n",
                    state->ap_manufacturer_body.size());
        std::printf("}\n");
    } else if (fmt == "toml") {
        std::printf("vid                      = %u\n", opts.vid);
        std::printf("pid                      = %u\n", opts.pid);
        std::printf("serial                   = \"%s\"\n", serial.c_str());
        std::printf("firmware                 = \"%s\"\n", firmware.c_str());
        std::printf("vehicle                  = \"%s\"\n", vehicle.c_str());
        if (state->hardware_type.has_value()) {
            std::printf("hardware_type            = \"%s\"\n", hw_type.c_str());
        }
        if (state->vehicle_manufacturer.has_value()) {
            std::printf("vehicle_manufacturer     = \"%s\"\n", veh_mfr.c_str());
        }
        if (state->ap_manufacturer.has_value()) {
            std::printf("ap_manufacturer          = \"%s\"\n", ap_mfr.c_str());
        }
        if (state->vehicle_paired.has_value()) {
            std::printf("married                  = %s\n", *state->vehicle_paired ? "true" : "false");
        }
        std::printf("user_info_body_bytes     = %zu\n", state->user_info_body.size());
        std::printf("firmware_body_bytes      = %zu\n", state->firmware_body.size());
        std::printf("device_settings_body_bytes = %zu\n", state->device_settings_body.size());
        std::printf("hardware_type_body_bytes = %zu\n", state->hardware_type_body.size());
        std::printf("vehicle_manufacturer_body_bytes = %zu\n",
                    state->vehicle_manufacturer_body.size());
        std::printf("ap_manufacturer_body_bytes = %zu\n",
                    state->ap_manufacturer_body.size());
    } else {
        std::printf("AccessPort                 : VID 0x%04X PID 0x%04X\n", opts.vid, opts.pid);
        std::printf("Serial                     : %s\n", serial.c_str());
        std::printf("Firmware                   : %s\n", firmware.c_str());
        std::printf("Vehicle                    : %s\n", vehicle.c_str());
        std::printf("Hardware type              : %s\n", hw_type.c_str());
        std::printf("Vehicle manufacturer       : %s\n", veh_mfr.c_str());
        std::printf("AP manufacturer            : %s\n", ap_mfr.c_str());
        std::printf("Marriage                   : %s\n", marriage_str);
        std::printf("UserInfo body bytes        : %zu\n", state->user_info_body.size());
        std::printf("Firmware response bytes    : %zu\n", state->firmware_body.size());
        std::printf("DeviceSettings body bytes  : %zu\n", state->device_settings_body.size());
    }
    (void)gate_marriage(*state, opts); // state subcommand never refuses; warning only.
    return 0;
}

int run_ls(int argc, char **argv, CommonOpts const &opts) {
    std::string subdir = "/maps/";
    std::string format = "text";
    std::string filter_regex;
    std::string sort_key; // empty = no sort
    bool subdir_set = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--format" && i + 1 < argc) {
            format = argv[++i];
            if (format != "text" && format != "json" && format != "toml") {
                std::fprintf(stderr,
                             "ap3 ls: --format must be text|json|toml (got '%s')\n",
                             format.c_str());
                return 2;
            }
            continue;
        }
        if (a == "--filter" && i + 1 < argc) {
            filter_regex.assign(argv[++i]);
            continue;
        }
        if (a == "--sort" && i + 1 < argc) {
            sort_key.assign(argv[++i]);
            if (sort_key != "name" && sort_key != "size" && sort_key != "mtime") {
                std::fprintf(stderr,
                             "ap3 ls: --sort must be name|size|mtime (got '%s')\n",
                             sort_key.c_str());
                return 2;
            }
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ap3 ls: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        if (subdir_set) {
            std::fprintf(stderr,
                         "ap3 ls: only one subdir positional allowed (got %.*s)\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        subdir.assign(a);
        subdir_set = true;
    }
    if (subdir_set) {
        if (subdir.empty() || subdir.front() != '/') {
            subdir.insert(subdir.begin(), '/');
        }
        if (subdir.back() != '/') {
            subdir.push_back('/');
        }
    }
    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 ls: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (!state.has_value()) {
        std::fprintf(stderr, "ap3 ls: query_state: %s\n", state.error().to_string().c_str());
        return 1;
    }
    if (!gate_marriage(*state, opts)) {
        return 1;
    }
    auto records = client.ls(subdir);
    if (!records.has_value()) {
        std::fprintf(stderr, "ap3 ls: %s\n", records.error().to_string().c_str());
        return 1;
    }
    // Apply --filter and --sort if requested. Filter on `name` (the
    // basename) since the full `path` always includes the subdir which
    // would over-match a regex like `^Stage1`.
    if (!filter_regex.empty()) {
        try {
            std::regex const re(filter_regex);
            records->erase(
                std::remove_if(records->begin(), records->end(),
                               [&](st::devices::ets::FileInfo const &r) {
                                   return !std::regex_search(r.name, re);
                               }),
                records->end());
        } catch (std::regex_error const &e) {
            std::fprintf(stderr, "ap3 ls: --filter regex error: %s\n", e.what());
            return 2;
        }
    }
    if (!sort_key.empty()) {
        std::sort(records->begin(), records->end(),
                  [&](st::devices::ets::FileInfo const &x,
                      st::devices::ets::FileInfo const &y) {
                      if (sort_key == "size") {
                          return x.size < y.size;
                      }
                      if (sort_key == "mtime") {
                          return x.mtime < y.mtime;
                      }
                      return x.name < y.name; // name default
                  });
    }
    if (format == "json") {
        std::printf("{\n  \"subdir\": \"%s\",\n  \"files\": [\n", subdir.c_str());
        for (std::size_t i = 0; i < records->size(); ++i) {
            auto const &r = (*records)[i];
            std::printf("    {\"path\":\"%s\",\"name\":\"%s\",\"size\":%llu,\"mtime\":%llu}%s\n",
                        r.path.c_str(), r.name.c_str(),
                        static_cast<unsigned long long>(r.size),
                        static_cast<unsigned long long>(r.mtime),
                        i + 1 < records->size() ? "," : "");
        }
        std::printf("  ]\n}\n");
    } else if (format == "toml") {
        std::printf("subdir = \"%s\"\n\n", subdir.c_str());
        for (auto const &r : *records) {
            std::printf("[[file]]\n");
            std::printf("path  = \"%s\"\n", r.path.c_str());
            std::printf("name  = \"%s\"\n", r.name.c_str());
            std::printf("size  = %llu\n", static_cast<unsigned long long>(r.size));
            std::printf("mtime = %llu\n\n", static_cast<unsigned long long>(r.mtime));
        }
    } else {
        std::printf("%zu file%s in %s\n", records->size(), records->size() == 1 ? "" : "s",
                    subdir.c_str());
        for (auto const &r : *records) {
            // VaultFileMetadata records in a ListFiles response use `path` for the
            // full relative path (e.g. "maps/Stage1.ptm") and `name` for
            // just the basename ("Stage1.ptm") — different from the request-
            // side convention. Print path-only; prepending `name` would
            // duplicate the basename as observed live 2026-06-12.
            std::printf("  %12llu  %s\n", static_cast<unsigned long long>(r.size),
                        r.path.c_str());
        }
    }
    return 0;
}

int run_pull(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    if (argc < 1) {
        std::fputs("ap3 pull: missing <ap-path>\n"
                   "Usage: subuwutuner-cli ap3 pull <ap-path> [--into <local-path>] "
                   "[--format text|json|toml]\n",
                   stderr);
        return 2;
    }
    std::string ap_path = argv[0];
    if (!ap_path.empty() && ap_path.front() != '/') {
        ap_path.insert(ap_path.begin(), '/');
    }
    std::filesystem::path out_path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--into") {
            out_path = argv[i + 1];
            ++i;
        }
    }
    if (out_path.empty()) {
        // Default: write into CWD using the filename component of the
        // AP-side path.
        auto slash = ap_path.rfind('/');
        out_path = (slash == std::string::npos) ? ap_path : ap_path.substr(slash + 1);
    }
    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 pull: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (state.has_value() && !gate_marriage(*state, opts)) {
        return 1;
    }
    auto bytes = client.read_file(ap_path);
    if (!bytes.has_value()) {
        std::fprintf(stderr, "ap3 pull: %s\n", bytes.error().to_string().c_str());
        return 1;
    }
    std::error_code ec;
    if (auto parent = out_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out{out_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "ap3 pull: open %s for writing failed\n",
                     out_path.string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(bytes->data()),
              static_cast<std::streamsize>(bytes->size()));
    out.close();
    if (fmt == "json") {
        std::printf("{\"ap_path\":\"%s\",\"into\":\"%s\",\"bytes\":%zu}\n",
                    ap_path.c_str(), out_path.string().c_str(), bytes->size());
    } else if (fmt == "toml") {
        std::printf("ap_path = \"%s\"\ninto    = \"%s\"\nbytes   = %zu\n",
                    ap_path.c_str(), out_path.string().c_str(), bytes->size());
    } else {
        std::printf("ap3 pull: wrote %zu bytes to %s\n", bytes->size(), out_path.string().c_str());
    }
    return 0;
}

int run_push(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    if (argc < 1) {
        std::fputs("ap3 push: missing <local-path>\n"
                   "Usage: subuwutuner-cli ap3 push <local-path> [--as <ap-path>] "
                   "[--format text|json|toml]\n",
                   stderr);
        return 2;
    }
    std::filesystem::path local{argv[0]};
    std::string ap_path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--as") {
            ap_path = argv[i + 1];
            ++i;
        }
    }
    if (ap_path.empty()) {
        // Default: drop into /maps/ with the local file's basename.
        // /maps/ is the only sane default for SubuwuTuner-managed
        // pushes; bytes destined for /presets/ or /datalog/ require
        // an explicit --as.
        ap_path = "/maps/" + local.filename().string();
    }
    if (!ap_path.empty() && ap_path.front() != '/') {
        ap_path.insert(ap_path.begin(), '/');
    }
    std::ifstream in{local, std::ios::binary | std::ios::ate};
    if (!in) {
        std::fprintf(stderr, "ap3 push: open %s failed\n", local.string().c_str());
        return 1;
    }
    auto const size = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    in.close();
    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 push: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (state.has_value() && !gate_marriage(*state, opts)) {
        return 1;
    }
    auto now_secs = static_cast<std::uint64_t>(std::time(nullptr));
    auto status = client.write_file(ap_path, bytes, now_secs);
    if (!status.has_value()) {
        std::fprintf(stderr, "ap3 push: %s\n", status.error().to_string().c_str());
        return 1;
    }
    if (fmt == "json") {
        std::printf("{\"local\":\"%s\",\"ap_path\":\"%s\",\"bytes\":%zu}\n",
                    local.string().c_str(), ap_path.c_str(), bytes.size());
    } else if (fmt == "toml") {
        std::printf("local   = \"%s\"\nap_path = \"%s\"\nbytes   = %zu\n",
                    local.string().c_str(), ap_path.c_str(), bytes.size());
    } else {
        std::printf("ap3 push: wrote %zu bytes to AP:%s\n", bytes.size(), ap_path.c_str());
    }
    return 0;
}

int run_rm(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    if (argc < 1) {
        std::fputs("ap3 rm: missing <ap-path>\n", stderr);
        return 2;
    }
    std::string ap_path = argv[0];
    if (!ap_path.empty() && ap_path.front() != '/') {
        ap_path.insert(ap_path.begin(), '/');
    }
    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 rm: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (state.has_value() && !gate_marriage(*state, opts)) {
        return 1;
    }
    auto status = client.remove_file(ap_path);
    if (!status.has_value()) {
        std::fprintf(stderr, "ap3 rm: %s\n", status.error().to_string().c_str());
        return 1;
    }
    if (fmt == "json") {
        std::printf("{\"ap_path\":\"%s\",\"removed\":true}\n", ap_path.c_str());
    } else if (fmt == "toml") {
        std::printf("ap_path = \"%s\"\nremoved = true\n", ap_path.c_str());
    } else {
        std::printf("ap3 rm: removed AP:%s\n", ap_path.c_str());
    }
    return 0;
}

int run_backup(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    std::filesystem::path out_dir = std::filesystem::current_path() / "ap3-backup";
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--into") {
            out_dir = argv[i + 1];
            ++i;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 backup: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    if (state.has_value() && !gate_marriage(*state, opts)) {
        return 1;
    }
    // Walk the well-known subdirectories per spec §6.8.
    static constexpr std::array<std::string_view, 4> kSubdirs{
        "/maps/", "/datalog/", "/presets/", "/images/",
    };
    struct BackupRec {
        std::string ap_path;
        std::string local_path;
        std::size_t bytes;
    };
    std::vector<BackupRec> manifest;
    std::size_t total_files = 0;
    std::uint64_t total_bytes = 0;
    for (auto sub : kSubdirs) {
        auto records = client.ls(sub);
        if (!records.has_value()) {
            std::fprintf(stderr, "ap3 backup: ls %.*s: %s\n", static_cast<int>(sub.size()),
                         sub.data(), records.error().to_string().c_str());
            continue;
        }
        auto sub_dir = out_dir / std::string{sub.substr(1, sub.size() - 2)};
        std::filesystem::create_directories(sub_dir, ec);
        for (auto const &rec : *records) {
            std::string const abs_path = "/" + rec.path;
            auto bytes = client.read_file(abs_path);
            if (!bytes.has_value()) {
                std::fprintf(stderr, "ap3 backup: read %s: %s\n", abs_path.c_str(),
                             bytes.error().to_string().c_str());
                continue;
            }
            auto const dst = sub_dir / rec.name;
            std::ofstream out{dst, std::ios::binary};
            if (!out) {
                std::fprintf(stderr, "ap3 backup: open %s failed\n", rec.name.c_str());
                continue;
            }
            out.write(reinterpret_cast<char const *>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
            manifest.push_back({abs_path, dst.string(), bytes->size()});
            ++total_files;
            total_bytes += bytes->size();
        }
    }
    for (auto const *name : {"settings", st::devices::ets::kVirtualBackupChecksumPath.data()}) {
        std::string const path = std::string{"/"} + name;
        auto bytes = client.read_file(path);
        if (!bytes.has_value()) {
            std::fprintf(stderr, "ap3 backup: read %s: %s\n", path.c_str(),
                         bytes.error().to_string().c_str());
            continue;
        }
        auto const dst = out_dir / name;
        std::ofstream out{dst, std::ios::binary};
        out.write(reinterpret_cast<char const *>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
        manifest.push_back({path, dst.string(), bytes->size()});
        ++total_files;
        total_bytes += bytes->size();
    }
    if (fmt == "json") {
        std::printf("{\n  \"into\": \"%s\",\n  \"total_files\": %zu,\n  \"total_bytes\": %llu,\n  \"files\": [\n",
                    out_dir.string().c_str(), total_files,
                    static_cast<unsigned long long>(total_bytes));
        for (std::size_t i = 0; i < manifest.size(); ++i) {
            auto const &m = manifest[i];
            std::printf("    {\"ap_path\":\"%s\",\"local\":\"%s\",\"bytes\":%zu}%s\n",
                        m.ap_path.c_str(), m.local_path.c_str(), m.bytes,
                        i + 1 < manifest.size() ? "," : "");
        }
        std::printf("  ]\n}\n");
    } else if (fmt == "toml") {
        std::printf("into        = \"%s\"\ntotal_files = %zu\ntotal_bytes = %llu\n\n",
                    out_dir.string().c_str(), total_files,
                    static_cast<unsigned long long>(total_bytes));
        for (auto const &m : manifest) {
            std::printf("[[file]]\nap_path = \"%s\"\nlocal   = \"%s\"\nbytes   = %zu\n\n",
                        m.ap_path.c_str(), m.local_path.c_str(), m.bytes);
        }
    } else {
        std::printf("ap3 backup: pulled %zu files (%llu bytes) into %s\n", total_files,
                    static_cast<unsigned long long>(total_bytes), out_dir.string().c_str());
    }
    return 0;
}

// Helper for `ap3 raw` — decodes a hex string (whitespace + 0x prefix
// tolerated) into raw bytes. Returns nullopt on malformed input.
std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view in) {
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 2);
    std::uint8_t cur = 0;
    bool have_nibble = false;
    for (auto c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ':') {
            continue;
        }
        if (c == '0' && have_nibble == false) {
            // Allow leading "0x" prefix on a byte. cur stays 0.
        }
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == 'x' || c == 'X') {
            // Allow "0x" inside the hex string; reset for the next byte.
            cur = 0;
            have_nibble = false;
            continue;
        }
        else {
            return std::nullopt;
        }
        if (!have_nibble) {
            cur = static_cast<std::uint8_t>(v << 4);
            have_nibble = true;
        } else {
            cur = static_cast<std::uint8_t>(cur | static_cast<std::uint8_t>(v));
            out.push_back(cur);
            cur = 0;
            have_nibble = false;
        }
    }
    if (have_nibble) {
        return std::nullopt;
    }
    return out;
}

// Debug subcommand: send a raw cmd byte + optional body, read the
// response, dump everything as hex. The encode_packet/decode_packet
// layer still handles sync/wire_len/CRC framing — this just lets the
// caller pick the cmd type + body without having to write a one-off
// Python script. Useful for protocol investigation.
int run_raw(int argc, char **argv, CommonOpts const &opts) {
    auto const fmt = consume_format_flag(argc, argv);
    std::string cmd_hex;
    std::string body_hex;
    unsigned int timeout_ms = 5000U;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--body" && i + 1 < argc) {
            body_hex.assign(argv[++i]);
            continue;
        }
        if (a == "--timeout-ms" && i + 1 < argc) {
            std::uint32_t v = 0;
            if (!parse_uint32_arg(argv[++i], v)) {
                std::fputs("ap3 raw: --timeout-ms must be a u32\n", stderr);
                return 2;
            }
            timeout_ms = v;
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ap3 raw: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
        if (!cmd_hex.empty()) {
            std::fprintf(stderr, "ap3 raw: multiple cmd args (%s, %.*s)\n",
                         cmd_hex.c_str(), static_cast<int>(a.size()), a.data());
            return 2;
        }
        cmd_hex.assign(a);
    }
    if (cmd_hex.empty()) {
        std::fputs("ap3 raw: missing <cmd-byte-hex>\n"
                   "Usage: subuwutuner-cli ap3 raw <cmd-hex> [--body <hex>] "
                   "[--timeout-ms N] [--format text|json|toml]\n"
                   "Example: subuwutuner-cli ap3 raw 0x28\n",
                   stderr);
        return 2;
    }
    auto const cmd_bytes = decode_hex(cmd_hex);
    if (!cmd_bytes.has_value() || cmd_bytes->size() != 1) {
        std::fprintf(stderr,
                     "ap3 raw: cmd must decode to exactly 1 byte (got '%s')\n",
                     cmd_hex.c_str());
        return 2;
    }
    std::vector<std::uint8_t> body;
    if (!body_hex.empty()) {
        auto decoded = decode_hex(body_hex);
        if (!decoded.has_value()) {
            std::fprintf(stderr, "ap3 raw: --body hex decode failed: '%s'\n",
                         body_hex.c_str());
            return 2;
        }
        body = std::move(*decoded);
    }

    auto channel = open_channel(opts);
    if (!channel.has_value()) {
        std::fprintf(stderr, "ap3 raw: %s\n", channel.error().to_string().c_str());
        return 1;
    }
    // Encode the request packet so we can hex-dump exactly what goes
    // on the wire.
    auto packet = st::transport::ets::encode_packet((*cmd_bytes)[0], body);
    if (!packet.has_value()) {
        std::fprintf(stderr, "ap3 raw: encode_packet: %s\n",
                     packet.error().to_string().c_str());
        return 1;
    }
    if (auto s = (*channel)->write_bytes(*packet); !s.has_value()) {
        std::fprintf(stderr, "ap3 raw: write_bytes: %s\n", s.error().to_string().c_str());
        return 1;
    }
    // Read up to 64 KB of response within the deadline.
    std::vector<std::uint8_t> resp;
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        auto chunk = (*channel)->read_bytes(65536, remaining);
        if (!chunk.has_value()) {
            std::fprintf(stderr, "ap3 raw: read_bytes: %s\n",
                         chunk.error().to_string().c_str());
            break;
        }
        if (chunk->empty()) {
            // Short timeout on this attempt; if total deadline still
            // has slack, try one more burst — but only once, so we
            // don't spin forever waiting for nothing.
            if (resp.empty()) {
                continue;
            }
            break;
        }
        resp.insert(resp.end(), chunk->begin(), chunk->end());
        // First burst arrived; give the AP a small grace window for
        // any tail and then return.
        if (resp.size() >= 16384) {
            break;
        }
    }

    auto dump_hex = [](char const *tag, std::vector<std::uint8_t> const &bytes) {
        std::printf("%s (%zu bytes)\n", tag, bytes.size());
        constexpr std::size_t kPerRow = 16;
        for (std::size_t i = 0; i < bytes.size(); i += kPerRow) {
            std::printf("  %04zx:", i);
            std::size_t const row = std::min(kPerRow, bytes.size() - i);
            for (std::size_t j = 0; j < row; ++j) {
                std::printf(" %02x", bytes[i + j]);
            }
            std::printf("\n");
        }
    };
    if (fmt == "json") {
        auto print_arr = [](std::vector<std::uint8_t> const &v) {
            std::printf("[");
            for (std::size_t i = 0; i < v.size(); ++i) {
                std::printf("%u%s", v[i], i + 1 < v.size() ? "," : "");
            }
            std::printf("]");
        };
        std::printf("{\"sent\":");
        print_arr(*packet);
        std::printf(",\"received\":");
        print_arr(resp);
        std::printf("}\n");
    } else {
        dump_hex("OUT", *packet);
        dump_hex("IN ", resp);
    }
    return 0;
}

int run_decrypt_img(int argc, char **argv, CommonOpts const &opts) {
    (void)opts; // decrypt-img is hardware-free; the device opts are unused
    if (!g_cobb_ap_cipher_armed) {
        std::fputs(
            "ap3 decrypt-img: PolicyDenied — runtime gate not armed. Pass "
            "--enable-cobb-ap-cipher\n"
            "  before the subcommand (the two-step arming model documented in "
            "docs/34).\n",
            stderr);
        return 2;
    }
    if (argc < 1) {
        std::fputs(
            "Usage: subuwutuner-cli --enable-cobb-ap-cipher ap3 decrypt-img "
            "<input.img>\n"
            "                       [-o <output-path>]\n"
            "\n"
            "Decrypt a COBB AP firmware .img file (Blowfish-ECB in custom CTR,\n"
            "T3 Session 3 cipher chain). Default output path is <input>.decrypted\n"
            "alongside the input file.\n"
            "\n"
            "Standalone CLI surface — no device needed; cipher key + algorithm\n"
            "live in the gated TU. See findings/handoffs/\n"
            "HANDOFF-from-subuwutuner-2026-06-12-blowfish-cleanroom.md for the\n"
            "provenance trail.\n",
            stderr);
        return 2;
    }
    std::filesystem::path const input_path{argv[0]};
    std::filesystem::path output_path;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "-o" || a == "--output") {
            output_path = std::filesystem::path{argv[i + 1]};
            ++i;
        }
    }
    if (output_path.empty()) {
        output_path = input_path;
        output_path += ".decrypted";
    }
    // Read input .img bytes.
    std::ifstream in{input_path, std::ios::binary | std::ios::ate};
    if (!in) {
        std::fprintf(stderr, "ap3 decrypt-img: cannot open input %s\n",
                     input_path.string().c_str());
        return 1;
    }
    auto const sz = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> img(static_cast<std::size_t>(sz));
    if (!in.read(reinterpret_cast<char *>(img.data()), sz)) {
        std::fprintf(stderr, "ap3 decrypt-img: input read truncated at byte %zu\n",
                     static_cast<std::size_t>(in.gcount()));
        return 1;
    }
    // Decrypt.
    auto plaintext = st::devices::ets::cipher::decrypt_ota_img(img);
    if (!plaintext.has_value()) {
        std::fprintf(stderr, "ap3 decrypt-img: %s\n",
                     plaintext.error().to_string().c_str());
        return plaintext.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }
    // Write output.
    std::ofstream out{output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "ap3 decrypt-img: cannot open output %s\n",
                     output_path.string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(plaintext->data()),
              static_cast<std::streamsize>(plaintext->size()));
    // Summary — hint at SquashFS / boot-stream magics so the user can
    // tell at a glance which kind of image they decrypted.
    std::printf("ap3 decrypt-img: %s\n", input_path.string().c_str());
    std::printf("  Input bytes:   %zu\n", img.size());
    std::printf("  Output bytes:  %zu\n", plaintext->size());
    std::printf("  Output path:   %s\n", output_path.string().c_str());
    if (plaintext->size() >= 4) {
        std::printf("  Magic:         %02X %02X %02X %02X",
                    (*plaintext)[0], (*plaintext)[1], (*plaintext)[2],
                    (*plaintext)[3]);
        if ((*plaintext)[0] == 'h' && (*plaintext)[1] == 's' &&
            (*plaintext)[2] == 'q' && (*plaintext)[3] == 's') {
            std::printf("  (SquashFS — mount with unsquashfs)");
        } else if (plaintext->size() >= 0x18 && (*plaintext)[0x14] == 'S' &&
                   (*plaintext)[0x15] == 'T' && (*plaintext)[0x16] == 'M' &&
                   (*plaintext)[0x17] == 'P') {
            std::printf("  (i.MX28 bootstream SB v1 — magic at offset 0x14)");
        }
        std::printf("\n");
    }
    return 0;
}

} // namespace ets_cli

int cmd_ap3(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("Usage: subuwutuner-cli ap3 <subcommand> [args]\n"
                   "\n"
                   "Subcommands:\n"
                   "  state [--format text|json|toml]\n"
                   "                     Print AP serial / firmware / marriage state\n"
                   "  ls [subdir] [--format text|json|toml] [--filter <regex>] [--sort name|size|mtime]\n"
                   "                     List files in /user/ap-user/<subdir> (default /maps/)\n"
                   "  pull <ap-path> [--into <local>] [--format text|json|toml]\n"
                   "                     Pull a file off the AP\n"
                   "  push <local> [--as <ap-path>] [--format text|json|toml]\n"
                   "                     Push a local file to the AP\n"
                   "  rm <ap-path> [--format text|json|toml]\n"
                   "                     Remove a file from the AP\n"
                   "  raw <cmd-hex> [--body <hex>] [--timeout-ms N] [--format text|json|toml]\n"
                   "                     Send a raw cmd byte + body; dump request + response as hex.\n"
                   "                     Debug aid for protocol investigation.\n"
                   "  backup [--into <dir>] [--format text|json|toml]\n"
                   "                     Pull /maps + /datalog + /presets + /images +\n"
                   "                     /settings + /backupcksum into <dir>\n"
                   "  decrypt-img <input.img> [-o <output>]\n"
                   "                     Decrypt a COBB AP firmware .img file. Standalone\n"
                   "                     (no device needed). Requires --enable-cobb-ap-cipher.\n"
                   "\n"
                   "Common flags:\n"
                   "  --allow-unpaired-vehicle   Don't refuse if marriage state reports Not\n"
                   "                         Installed (default policy refuses; see docs/34)\n"
                   "  --vid <hex16> --pid <hex16>\n"
                   "                         Override the VID/PID match (default: 0x1A84/0x0121)\n"
                   "\n"
                   "See docs/34-cobb-ap-as-tune-vault.md for the gating model and the\n"
                   ".ptm cipher tier (off by default).\n",
                   stderr);
        return 2;
    }
    ets_cli::CommonOpts opts;
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;
    if (!ets_cli::consume_common(sub_argc, sub_argv, opts)) {
        return 2;
    }
    std::string_view const sub{argv[0]};
    if (sub == "state") {
        return ets_cli::run_state(sub_argc, sub_argv, opts);
    }
    if (sub == "ls") {
        return ets_cli::run_ls(sub_argc, sub_argv, opts);
    }
    if (sub == "pull") {
        return ets_cli::run_pull(sub_argc, sub_argv, opts);
    }
    if (sub == "push") {
        return ets_cli::run_push(sub_argc, sub_argv, opts);
    }
    if (sub == "rm") {
        return ets_cli::run_rm(sub_argc, sub_argv, opts);
    }
    if (sub == "backup") {
        return ets_cli::run_backup(sub_argc, sub_argv, opts);
    }
    if (sub == "raw") {
        return ets_cli::run_raw(sub_argc, sub_argv, opts);
    }
    if (sub == "decrypt-img") {
        return ets_cli::run_decrypt_img(sub_argc, sub_argv, opts);
    }
    std::fprintf(stderr,
                 "ap3: unknown subcommand: %s\n"
                 "Run `subuwutuner-cli ap3` for the list.\n",
                 argv[0]);
    return 2;
}

// Runtime arming flag for the `.ptm` cipher path. The CLI gate is the
// "second key" of the two-step arming model described in docs/34 — the
// build flag `ST_ENABLE_COBB_AP_CIPHER=ON` is the first key. Even with
// the build flag on, every `ptm` subcommand refuses until this flag
// flips to true. Set from main's pre-pass on `--enable-cobb-ap-cipher`,
// read in `cmd_ptm`.
// (g_cobb_ap_cipher_armed defined above ets_cli for visibility from
// both ets_cli::run_decrypt_img and ptm_cli below.)

namespace ptm_cli {

using st::library::TableRange;
using st::library::TableHit;
using st::library::compute_table_ranges;
using st::library::find_table_for;
using st::library::aggregate_table_hits;

st::Result<std::vector<std::uint8_t>> read_file_bytes(std::string const &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return st::failure(st::ErrorCode::ParseError,
                           "ptm: file '" + path + "' not found or unreadable");
    }
    f.seekg(0, std::ios::end);
    auto const sz = f.tellg();
    if (sz < 0) {
        return st::failure(st::ErrorCode::ParseError,
                           "ptm: '" + path + "' tellg failed");
    }
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    if (sz > 0) {
        f.read(reinterpret_cast<char *>(bytes.data()), sz);
        if (!f) {
            return st::failure(st::ErrorCode::ParseError,
                               "ptm: '" + path + "' read failed");
        }
    }
    return bytes;
}

enum class Format { Text, Json, Toml };

struct ListPatchesOpts {
    std::string ptm_path;
    std::string def_path;
    Format format{Format::Text};
};

bool parse_list_patches_opts(int argc, char **argv, ListPatchesOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def" && i + 1 < argc) {
            out.def_path.assign(argv[++i]);
            continue;
        }
        if (a == "--format" && i + 1 < argc) {
            std::string_view const v{argv[++i]};
            if (v == "text") {
                out.format = Format::Text;
            } else if (v == "json") {
                out.format = Format::Json;
            } else if (v == "toml") {
                out.format = Format::Toml;
            } else {
                std::fprintf(stderr,
                             "ptm list-patches: --format must be text|json|toml (got '%.*s')\n",
                             static_cast<int>(v.size()), v.data());
                return false;
            }
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm list-patches: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (!out.ptm_path.empty()) {
            std::fprintf(stderr,
                         "ptm list-patches: only one .ptm path may be given (got '%s' and '%.*s')\n",
                         out.ptm_path.c_str(), static_cast<int>(a.size()), a.data());
            return false;
        }
        out.ptm_path.assign(a);
    }
    if (out.ptm_path.empty()) {
        std::fputs("ptm list-patches: missing required <file.ptm> argument\n", stderr);
        return false;
    }
    return true;
}

void render_text(std::vector<st::library::PatchEntry> const &patches,
                 std::vector<st::devices::ets::PatchClassification> const &classifications,
                 std::vector<TableRange> const &ranges) {
    bool const has_table_col = !ranges.empty();
    if (has_table_col) {
        std::printf("%-12s  %8s  %-20s  %s\n", "rom_offset", "length", "layer", "table");
    } else {
        std::printf("%-12s  %8s  %s\n", "rom_offset", "length", "layer");
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        auto const &p = patches[i];
        auto const layer_text = classifications.empty()
                                    ? std::string_view{"?"}
                                    : st::devices::ets::layer_label(classifications[i].layer);
        if (has_table_col) {
            auto const *t = find_table_for(ranges, p.rom_offset);
            std::printf("  0x%08X  %8u  %-20.*s  %s\n", p.rom_offset, p.length,
                        static_cast<int>(layer_text.size()), layer_text.data(),
                        t == nullptr ? "" : t->name.c_str());
        } else {
            std::printf("  0x%08X  %8u  %.*s\n", p.rom_offset, p.length,
                        static_cast<int>(layer_text.size()), layer_text.data());
        }
    }
}

void render_json(std::vector<st::library::PatchEntry> const &patches,
                 std::vector<st::devices::ets::PatchClassification> const &classifications,
                 std::vector<TableRange> const &ranges) {
    std::printf("[\n");
    for (std::size_t i = 0; i < patches.size(); ++i) {
        auto const &p = patches[i];
        auto const layer_text = classifications.empty()
                                    ? std::string_view{"?"}
                                    : st::devices::ets::layer_label(classifications[i].layer);
        auto const *t = ranges.empty() ? nullptr : find_table_for(ranges, p.rom_offset);
        if (t != nullptr) {
            std::printf(R"(  {"rom_offset":%u,"length":%u,"layer":"%.*s","table_id":"%s","table_name":"%s"}%s)" "\n",
                        p.rom_offset, p.length,
                        static_cast<int>(layer_text.size()), layer_text.data(),
                        t->id.c_str(), t->name.c_str(),
                        i + 1 < patches.size() ? "," : "");
        } else {
            std::printf(R"(  {"rom_offset":%u,"length":%u,"layer":"%.*s"}%s)" "\n",
                        p.rom_offset, p.length,
                        static_cast<int>(layer_text.size()), layer_text.data(),
                        i + 1 < patches.size() ? "," : "");
        }
    }
    std::printf("]\n");
}

void render_toml(std::vector<st::library::PatchEntry> const &patches,
                 std::vector<st::devices::ets::PatchClassification> const &classifications,
                 std::vector<TableRange> const &ranges) {
    for (std::size_t i = 0; i < patches.size(); ++i) {
        auto const &p = patches[i];
        auto const layer_text = classifications.empty()
                                    ? std::string_view{"?"}
                                    : st::devices::ets::layer_label(classifications[i].layer);
        std::printf("[[patches]]\n");
        std::printf("rom_offset = %u\n", p.rom_offset);
        std::printf("length = %u\n", p.length);
        std::printf("layer = \"%.*s\"\n",
                    static_cast<int>(layer_text.size()), layer_text.data());
        if (!ranges.empty()) {
            if (auto const *t = find_table_for(ranges, p.rom_offset); t != nullptr) {
                std::printf("table_id = \"%s\"\n", t->id.c_str());
                std::printf("table_name = \"%s\"\n", t->name.c_str());
            }
        }
        std::printf("\n");
    }
}

struct InspectOpts {
    std::string ptm_path;
    std::string def_path;
    Format format{Format::Text};
};

bool parse_inspect_opts(int argc, char **argv, InspectOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def" && i + 1 < argc) {
            out.def_path.assign(argv[++i]);
            continue;
        }
        if (a == "--format" && i + 1 < argc) {
            std::string_view const v{argv[++i]};
            if (v == "text") {
                out.format = Format::Text;
            } else if (v == "json") {
                out.format = Format::Json;
            } else if (v == "toml") {
                out.format = Format::Toml;
            } else {
                std::fprintf(stderr,
                             "ptm inspect: --format must be text|json|toml (got '%.*s')\n",
                             static_cast<int>(v.size()), v.data());
                return false;
            }
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm inspect: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (!out.ptm_path.empty()) {
            std::fprintf(stderr,
                         "ptm inspect: only one .ptm path may be given (got '%s' and '%.*s')\n",
                         out.ptm_path.c_str(), static_cast<int>(a.size()), a.data());
            return false;
        }
        out.ptm_path.assign(a);
    }
    if (out.ptm_path.empty()) {
        std::fputs("ptm inspect: missing required <file.ptm> argument\n", stderr);
        return false;
    }
    return true;
}

// Format an integer with thousand separators for the inspect summary.
// The spec's example uses `90,060 bytes total`; matching that adds a
// little human-readable polish without pulling in <locale>.
std::string with_commas(std::uint64_t n) {
    auto s = std::to_string(n);
    std::string out;
    out.reserve(s.size() + s.size() / 3);
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count > 0 && count % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(*it);
        ++count;
    }
    return {out.rbegin(), out.rend()};
}

void render_inspect_text(std::string const &path,
                         std::size_t file_size,
                         st::library::DecodedPtm const &decoded,
                         std::vector<st::devices::ets::LayerSummary> const &summary,
                         std::vector<TableHit> const &top_tables) {
    std::uint64_t total_bytes = 0;
    for (auto const &p : decoded.patches) {
        total_bytes += p.length;
    }
    auto const slash = path.find_last_of("/\\");
    std::string const basename =
        slash == std::string::npos ? path : path.substr(slash + 1);

    std::printf("Inspect: %s (%s bytes)\n", basename.c_str(), with_commas(file_size).c_str());
    std::printf("\n");
    std::printf("Identity\n");
    std::printf("  Vendor:           %s\n", decoded.metadata.vendor_id.c_str());
    std::printf("  Vehicle:          %s\n", decoded.metadata.vehicle_id.c_str());
    std::printf("  Lock mask:        %u\n", decoded.metadata.lock_mask);
    std::printf("  ROM sum:          %s\n", decoded.metadata.rom_sum.c_str());
    std::printf("  Save date:        %s\n", decoded.metadata.save_date_time.c_str());
    std::printf("  Patches:          %zu (%s bytes total)\n",
                decoded.patches.size(), with_commas(total_bytes).c_str());
    std::printf("\n");

    if (!summary.empty()) {
        std::uint32_t total_patches = 0;
        for (auto const &row : summary) {
            total_patches += row.patch_count;
        }
        std::printf("Architectural breakdown\n");
        for (auto const &row : summary) {
            auto const layer_text = st::devices::ets::layer_label(row.layer);
            double const pct =
                total_bytes == 0 ? 0.0
                                 : 100.0 * static_cast<double>(row.total_bytes) /
                                       static_cast<double>(total_bytes);
            std::printf("  %-30.*s %5u patches  %5.1f%%  (%s bytes)\n",
                        static_cast<int>(layer_text.size()), layer_text.data(),
                        row.patch_count, pct, with_commas(row.total_bytes).c_str());
        }
        std::printf("\n");
    }

    if (!top_tables.empty()) {
        std::printf("Top tables modified\n");
        std::size_t const show = std::min<std::size_t>(top_tables.size(), 12);
        for (std::size_t i = 0; i < show; ++i) {
            auto const &h = top_tables[i];
            std::printf("  %5u  %5s  %s\n", h.patch_count,
                        with_commas(h.total_bytes).c_str(), h.table_name.c_str());
        }
        if (top_tables.size() > show) {
            std::printf("  ... (%zu more — pass --format json to see all)\n",
                        top_tables.size() - show);
        }
        std::printf("\n");
    }

    std::printf("Cross-references\n");
    std::printf("  List every patch:                ptm list-patches %s\n", basename.c_str());
    // Diff / import lines deliberately omitted until those subcommands
    // ship — keeps cross-refs honest with what's actually wired.
}

void render_inspect_json(std::string const &path,
                         std::size_t file_size,
                         st::library::DecodedPtm const &decoded,
                         std::vector<st::devices::ets::LayerSummary> const &summary,
                         std::vector<TableHit> const &top_tables) {
    (void)path;
    std::uint64_t total_bytes = 0;
    for (auto const &p : decoded.patches) {
        total_bytes += p.length;
    }
    std::printf("{\n");
    std::printf("  \"identity\": {\n");
    std::printf("    \"vendor\": \"%s\",\n", decoded.metadata.vendor_id.c_str());
    std::printf("    \"vehicle_id\": \"%s\",\n", decoded.metadata.vehicle_id.c_str());
    std::printf("    \"lock_mask\": %u,\n", decoded.metadata.lock_mask);
    std::printf("    \"rom_sum\": \"%s\",\n", decoded.metadata.rom_sum.c_str());
    std::printf("    \"save_date\": \"%s\"\n", decoded.metadata.save_date_time.c_str());
    std::printf("  },\n");
    std::printf("  \"patches\": { \"total\": %zu, \"total_bytes\": %llu },\n",
                decoded.patches.size(),
                static_cast<unsigned long long>(total_bytes));
    std::printf("  \"file_size_bytes\": %llu,\n",
                static_cast<unsigned long long>(file_size));
    std::printf("  \"layers\": [\n");
    for (std::size_t i = 0; i < summary.size(); ++i) {
        auto const &row = summary[i];
        auto const layer_text = st::devices::ets::layer_label(row.layer);
        std::printf("    {\"layer\":\"%.*s\",\"patches\":%u,\"bytes\":%llu}%s\n",
                    static_cast<int>(layer_text.size()), layer_text.data(),
                    row.patch_count,
                    static_cast<unsigned long long>(row.total_bytes),
                    i + 1 < summary.size() ? "," : "");
    }
    std::printf("  ],\n");
    std::printf("  \"top_tables\": [\n");
    for (std::size_t i = 0; i < top_tables.size(); ++i) {
        auto const &h = top_tables[i];
        std::printf("    {\"id\":\"%s\",\"name\":\"%s\",\"patches\":%u,\"bytes\":%llu}%s\n",
                    h.table_id.c_str(), h.table_name.c_str(), h.patch_count,
                    static_cast<unsigned long long>(h.total_bytes),
                    i + 1 < top_tables.size() ? "," : "");
    }
    std::printf("  ]\n");
    std::printf("}\n");
}

void render_inspect_toml(std::string const &path,
                         std::size_t file_size,
                         st::library::DecodedPtm const &decoded,
                         std::vector<st::devices::ets::LayerSummary> const &summary,
                         std::vector<TableHit> const &top_tables) {
    (void)path;
    std::uint64_t total_bytes = 0;
    for (auto const &p : decoded.patches) {
        total_bytes += p.length;
    }
    std::printf("[identity]\n");
    std::printf("vendor = \"%s\"\n", decoded.metadata.vendor_id.c_str());
    std::printf("vehicle_id = \"%s\"\n", decoded.metadata.vehicle_id.c_str());
    std::printf("lock_mask = %u\n", decoded.metadata.lock_mask);
    std::printf("rom_sum = \"%s\"\n", decoded.metadata.rom_sum.c_str());
    std::printf("save_date = \"%s\"\n", decoded.metadata.save_date_time.c_str());
    std::printf("\n[patches]\n");
    std::printf("total = %zu\n", decoded.patches.size());
    std::printf("total_bytes = %llu\n", static_cast<unsigned long long>(total_bytes));
    std::printf("file_size_bytes = %llu\n", static_cast<unsigned long long>(file_size));
    for (auto const &row : summary) {
        auto const layer_text = st::devices::ets::layer_label(row.layer);
        std::printf("\n[[layers]]\n");
        std::printf("layer = \"%.*s\"\n",
                    static_cast<int>(layer_text.size()), layer_text.data());
        std::printf("patches = %u\n", row.patch_count);
        std::printf("bytes = %llu\n", static_cast<unsigned long long>(row.total_bytes));
    }
    for (auto const &h : top_tables) {
        std::printf("\n[[top_tables]]\n");
        std::printf("id = \"%s\"\n", h.table_id.c_str());
        std::printf("name = \"%s\"\n", h.table_name.c_str());
        std::printf("patches = %u\n", h.patch_count);
        std::printf("bytes = %llu\n", static_cast<unsigned long long>(h.total_bytes));
    }
}

enum class DiffGroup { ByLayer, ByTable };

struct DiffOpts {
    std::string a_path;
    std::string b_path;
    std::string def_path;
    Format format{Format::Text};
    DiffGroup group{DiffGroup::ByLayer};
};

bool parse_diff_opts(int argc, char **argv, DiffOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def" && i + 1 < argc) {
            out.def_path.assign(argv[++i]);
            continue;
        }
        if (a == "--by-layer") {
            out.group = DiffGroup::ByLayer;
            continue;
        }
        if (a == "--by-table") {
            out.group = DiffGroup::ByTable;
            continue;
        }
        if (a == "--format" && i + 1 < argc) {
            std::string_view const v{argv[++i]};
            if (v == "text") {
                out.format = Format::Text;
            } else if (v == "json") {
                out.format = Format::Json;
            } else if (v == "toml") {
                out.format = Format::Toml;
            } else {
                std::fprintf(stderr,
                             "ptm diff: --format must be text|json|toml (got '%.*s')\n",
                             static_cast<int>(v.size()), v.data());
                return false;
            }
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm diff: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (out.a_path.empty()) {
            out.a_path.assign(a);
        } else if (out.b_path.empty()) {
            out.b_path.assign(a);
        } else {
            std::fprintf(stderr,
                         "ptm diff: too many positional args (got '%s', '%s', '%.*s')\n",
                         out.a_path.c_str(), out.b_path.c_str(),
                         static_cast<int>(a.size()), a.data());
            return false;
        }
    }
    if (out.a_path.empty() || out.b_path.empty()) {
        std::fputs("ptm diff: missing <a.ptm> <b.ptm> arguments\n", stderr);
        return false;
    }
    return true;
}

// Per-architectural-layer diff tally. The shared / a_only / b_only
// breakdown is at the rom_offset level — patches with the same
// rom_offset are considered "shared"; bytes-different among shared is
// the changed_at_common bucket.
// Pull the shared diff types from st::library; the CLI's local
// compute_diff used to duplicate them. The renderers downstream still
// reference the unqualified names via these aliases.
using LayerDiff = st::library::LayerDiff;
using TableDiff = st::library::TableDiff;
using DiffResult = st::library::TuneDiffResult;

DiffResult compute_diff(st::library::DecodedPtm const &a, st::library::DecodedPtm const &b,
                        std::vector<TableRange> const &table_ranges = {}) {
    return st::library::compute_tune_diff(a, b, table_ranges);
}


void render_diff_text(std::string const &a_path, std::string const &b_path,
                      DiffResult const &r, DiffGroup group) {
    auto basename = [](std::string const &p) {
        auto s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    };
    std::printf("ptm diff: %s → %s\n\n", basename(a_path).c_str(), basename(b_path).c_str());
    std::printf("Patch envelope: a=%u  b=%u  shared=%u  a_only=%u  b_only=%u  "
                "changed_at_shared=%u\n",
                r.a_total_patches, r.b_total_patches, r.shared_patches,
                r.a_only_patches, r.b_only_patches, r.changed_at_shared);
    std::printf("Bytes:          a_only=%s  b_only=%s  changed=%s\n\n",
                with_commas(r.a_only_bytes).c_str(),
                with_commas(r.b_only_bytes).c_str(),
                with_commas(r.changed_bytes).c_str());

    if (group == DiffGroup::ByTable && !r.by_table.empty()) {
        std::printf("By table (top 20)\n");
        std::size_t const show = std::min<std::size_t>(r.by_table.size(), 20);
        for (std::size_t i = 0; i < show; ++i) {
            auto const &td = r.by_table[i];
            std::printf("  %s\n", td.table_name.c_str());
            std::printf("    a_only=%u/%s  b_only=%u/%s  shared=%u  changed=%u/%s\n",
                        td.a_only_patches, with_commas(td.a_only_bytes).c_str(),
                        td.b_only_patches, with_commas(td.b_only_bytes).c_str(),
                        td.shared_patches, td.changed_at_shared,
                        with_commas(td.changed_bytes).c_str());
        }
        if (r.by_table.size() > show) {
            std::printf("  ... (%zu more — pass --format json to see all)\n",
                        r.by_table.size() - show);
        }
        std::printf("\n");
    } else if (!r.by_layer.empty()) {
        // Per-layer view sorted by total impact (a_only + b_only +
        // changed) descending — top-of-list is where the tune
        // diverges most. Each layer gets two lines: a headline naming
        // the layer + total impact, then a breakdown showing
        // direction (a_only / b_only / changed-at-shared).
        std::printf("By layer (sorted by impact)\n");
        for (auto const &ld : r.by_layer) {
            auto const label = st::devices::ets::layer_label(ld.layer);
            std::uint64_t const total_bytes =
                ld.a_only_bytes + ld.b_only_bytes + ld.changed_bytes;
            std::uint32_t const total_patches =
                ld.a_only_patches + ld.b_only_patches + ld.shared_patches;
            std::printf("  %-26.*s  %s bytes across %u patches\n",
                        static_cast<int>(label.size()), label.data(),
                        with_commas(total_bytes).c_str(), total_patches);
            std::printf("    a_only=%u/%s  b_only=%u/%s  shared=%u  "
                        "changed=%u/%s\n",
                        ld.a_only_patches, with_commas(ld.a_only_bytes).c_str(),
                        ld.b_only_patches, with_commas(ld.b_only_bytes).c_str(),
                        ld.shared_patches, ld.changed_at_shared,
                        with_commas(ld.changed_bytes).c_str());
        }
        // Totals footer aggregating across layers — at-a-glance "this
        // tune changes N bytes across M layers" headline.
        std::uint64_t const total_all_bytes =
            r.a_only_bytes + r.b_only_bytes + r.changed_bytes;
        std::printf("  ----\n");
        std::printf("  Total                       %s bytes across %zu layers\n",
                    with_commas(total_all_bytes).c_str(), r.by_layer.size());
        std::printf("\n");
    }

    std::printf("Cross-references\n");
    std::printf("  Per-row patch listing:           ptm list-patches <file>\n");
    std::printf("  Identity + summary per file:     ptm inspect <file>\n");
}

void render_diff_json(std::string const &a_path, std::string const &b_path,
                      DiffResult const &r) {
    (void)a_path;
    (void)b_path;
    std::printf("{\n");
    std::printf("  \"envelope\": {\n");
    std::printf("    \"a_total\": %u,\n", r.a_total_patches);
    std::printf("    \"b_total\": %u,\n", r.b_total_patches);
    std::printf("    \"shared\": %u,\n", r.shared_patches);
    std::printf("    \"a_only\": %u,\n", r.a_only_patches);
    std::printf("    \"b_only\": %u,\n", r.b_only_patches);
    std::printf("    \"changed_at_shared\": %u\n", r.changed_at_shared);
    std::printf("  },\n");
    std::printf("  \"bytes\": {\n");
    std::printf("    \"a_only\": %llu,\n", static_cast<unsigned long long>(r.a_only_bytes));
    std::printf("    \"b_only\": %llu,\n", static_cast<unsigned long long>(r.b_only_bytes));
    std::printf("    \"changed\": %llu\n", static_cast<unsigned long long>(r.changed_bytes));
    std::printf("  },\n");
    std::printf("  \"by_layer\": [\n");
    for (std::size_t i = 0; i < r.by_layer.size(); ++i) {
        auto const &ld = r.by_layer[i];
        auto const label = st::devices::ets::layer_label(ld.layer);
        std::printf("    {\"layer\":\"%.*s\","
                    "\"a_only_patches\":%u,\"b_only_patches\":%u,"
                    "\"shared_patches\":%u,\"changed_at_shared\":%u,"
                    "\"a_only_bytes\":%llu,\"b_only_bytes\":%llu,"
                    "\"changed_bytes\":%llu}%s\n",
                    static_cast<int>(label.size()), label.data(),
                    ld.a_only_patches, ld.b_only_patches, ld.shared_patches,
                    ld.changed_at_shared,
                    static_cast<unsigned long long>(ld.a_only_bytes),
                    static_cast<unsigned long long>(ld.b_only_bytes),
                    static_cast<unsigned long long>(ld.changed_bytes),
                    i + 1 < r.by_layer.size() ? "," : "");
    }
    std::printf("  ],\n");
    std::printf("  \"by_table\": [\n");
    for (std::size_t i = 0; i < r.by_table.size(); ++i) {
        auto const &td = r.by_table[i];
        std::printf("    {\"table_id\":\"%s\",\"table_name\":\"%s\","
                    "\"a_only_patches\":%u,\"b_only_patches\":%u,"
                    "\"shared_patches\":%u,\"changed_at_shared\":%u,"
                    "\"a_only_bytes\":%llu,\"b_only_bytes\":%llu,"
                    "\"changed_bytes\":%llu}%s\n",
                    td.table_id.c_str(), td.table_name.c_str(),
                    td.a_only_patches, td.b_only_patches, td.shared_patches,
                    td.changed_at_shared,
                    static_cast<unsigned long long>(td.a_only_bytes),
                    static_cast<unsigned long long>(td.b_only_bytes),
                    static_cast<unsigned long long>(td.changed_bytes),
                    i + 1 < r.by_table.size() ? "," : "");
    }
    std::printf("  ]\n");
    std::printf("}\n");
}

void render_diff_toml(std::string const &a_path, std::string const &b_path,
                      DiffResult const &r) {
    (void)a_path;
    (void)b_path;
    std::printf("[envelope]\n");
    std::printf("a_total = %u\n", r.a_total_patches);
    std::printf("b_total = %u\n", r.b_total_patches);
    std::printf("shared = %u\n", r.shared_patches);
    std::printf("a_only = %u\n", r.a_only_patches);
    std::printf("b_only = %u\n", r.b_only_patches);
    std::printf("changed_at_shared = %u\n", r.changed_at_shared);
    std::printf("\n[bytes]\n");
    std::printf("a_only = %llu\n", static_cast<unsigned long long>(r.a_only_bytes));
    std::printf("b_only = %llu\n", static_cast<unsigned long long>(r.b_only_bytes));
    std::printf("changed = %llu\n", static_cast<unsigned long long>(r.changed_bytes));
    for (auto const &ld : r.by_layer) {
        auto const label = st::devices::ets::layer_label(ld.layer);
        std::printf("\n[[by_layer]]\n");
        std::printf("layer = \"%.*s\"\n", static_cast<int>(label.size()), label.data());
        std::printf("a_only_patches = %u\n", ld.a_only_patches);
        std::printf("b_only_patches = %u\n", ld.b_only_patches);
        std::printf("shared_patches = %u\n", ld.shared_patches);
        std::printf("changed_at_shared = %u\n", ld.changed_at_shared);
        std::printf("a_only_bytes = %llu\n", static_cast<unsigned long long>(ld.a_only_bytes));
        std::printf("b_only_bytes = %llu\n", static_cast<unsigned long long>(ld.b_only_bytes));
        std::printf("changed_bytes = %llu\n", static_cast<unsigned long long>(ld.changed_bytes));
    }
    for (auto const &td : r.by_table) {
        std::printf("\n[[by_table]]\n");
        std::printf("table_id = \"%s\"\n", td.table_id.c_str());
        std::printf("table_name = \"%s\"\n", td.table_name.c_str());
        std::printf("a_only_patches = %u\n", td.a_only_patches);
        std::printf("b_only_patches = %u\n", td.b_only_patches);
        std::printf("shared_patches = %u\n", td.shared_patches);
        std::printf("changed_at_shared = %u\n", td.changed_at_shared);
        std::printf("a_only_bytes = %llu\n", static_cast<unsigned long long>(td.a_only_bytes));
        std::printf("b_only_bytes = %llu\n", static_cast<unsigned long long>(td.b_only_bytes));
        std::printf("changed_bytes = %llu\n", static_cast<unsigned long long>(td.changed_bytes));
    }
}

// Decrypt + decode one .ptm. Helper shared by run_diff (and could be
// shared by run_inspect / run_list_patches but those have one-shot
// callers and grew their inline paths first).
struct LoadResult {
    st::library::DecodedPtm decoded;
};
st::Result<LoadResult> load_ptm(std::string const &path) {
    auto bytes = read_file_bytes(path);
    if (!bytes.has_value()) {
        return st::failure(std::move(bytes).error());
    }
    auto contents = st::devices::ets::cipher::decrypt_ptm(*bytes);
    if (!contents.has_value()) {
        return st::failure(std::move(contents).error());
    }
    auto decoded = st::library::decode_ptm_xml(contents->private_data_xml);
    if (!decoded.has_value()) {
        return st::failure(std::move(decoded).error());
    }
    return LoadResult{std::move(*decoded)};
}

int run_diff(int argc, char **argv) {
    DiffOpts opts;
    if (!parse_diff_opts(argc, argv, opts)) {
        return 2;
    }
    auto a = load_ptm(opts.a_path);
    if (!a.has_value()) {
        std::fprintf(stderr, "ptm diff: a: %s\n", a.error().to_string().c_str());
        return a.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }
    auto b = load_ptm(opts.b_path);
    if (!b.has_value()) {
        std::fprintf(stderr, "ptm diff: b: %s\n", b.error().to_string().c_str());
        return b.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }
    std::vector<TableRange> ranges;
    if (!opts.def_path.empty()) {
        auto def = st::Definition::from_file(std::filesystem::path{opts.def_path});
        if (!def.has_value()) {
            std::fprintf(stderr,
                         "ptm diff: --def: %s (continuing without table grouping)\n",
                         def.error().to_string().c_str());
        } else {
            ranges = compute_table_ranges(*def);
        }
    }
    if (opts.group == DiffGroup::ByTable && ranges.empty()) {
        std::fputs("ptm diff: --by-table requires --def; falling back to --by-layer\n",
                   stderr);
        opts.group = DiffGroup::ByLayer;
    }

    auto const result = compute_diff(a->decoded, b->decoded, ranges);

    switch (opts.format) {
    case Format::Text:
        render_diff_text(opts.a_path, opts.b_path, result, opts.group);
        break;
    case Format::Json:
        render_diff_json(opts.a_path, opts.b_path, result);
        break;
    case Format::Toml:
        render_diff_toml(opts.a_path, opts.b_path, result);
        break;
    }
    return 0;
}

struct VerifyOpts {
    std::string ptm_path;
    std::string project_dir;
};

bool parse_verify_opts(int argc, char **argv, VerifyOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm verify: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (out.ptm_path.empty()) {
            out.ptm_path.assign(a);
        } else if (out.project_dir.empty()) {
            out.project_dir.assign(a);
        } else {
            std::fprintf(stderr, "ptm verify: too many positional args (got '%s', '%s', '%.*s')\n",
                         out.ptm_path.c_str(), out.project_dir.c_str(),
                         static_cast<int>(a.size()), a.data());
            return false;
        }
    }
    if (out.ptm_path.empty() || out.project_dir.empty()) {
        std::fputs("ptm verify: usage: ptm verify <file.ptm> <project.stune>\n", stderr);
        return false;
    }
    return true;
}

int run_verify(int argc, char **argv) {
    VerifyOpts opts;
    if (!parse_verify_opts(argc, argv, opts)) {
        return 2;
    }
    // Decode the .ptm.
    auto loaded = load_ptm(opts.ptm_path);
    if (!loaded.has_value()) {
        std::fprintf(stderr, "ptm verify: %s\n", loaded.error().to_string().c_str());
        return loaded.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }
    auto const &ptm_patches = loaded->decoded.patches;

    // Read project's ptm_patches.toml.
    std::filesystem::path const dir{opts.project_dir};
    auto const patches_toml = dir / "ptm_patches.toml";
    if (!std::filesystem::exists(patches_toml)) {
        std::fprintf(stderr, "ptm verify: %s missing (only projects from `ptm import` carry "
                             "this file)\n", patches_toml.string().c_str());
        return 1;
    }
    toml::table pat;
    try {
        pat = toml::parse_file(patches_toml.string());
    } catch (toml::parse_error const &e) {
        std::fprintf(stderr, "ptm verify: ptm_patches.toml parse error: %s\n",
                     e.description().data());
        return 1;
    }
    auto const *arr = pat["patch"].as_array();
    if (arr == nullptr) {
        std::fputs("ptm verify: ptm_patches.toml has no [[patch]] entries\n", stderr);
        return 1;
    }

    // Build a parallel project_patches vector. Includes ram_offset
    // because some .ptm files have multiple <patch> entries at the
    // same rom_offset (e.g. before/after byte pairs); collapsing by
    // rom_offset alone silently drops duplicates and overcounts
    // false-positive byte mismatches.
    struct PP {
        std::uint32_t rom_offset;
        std::int32_t ram_offset;
        std::uint32_t length;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<PP> project_patches;
    project_patches.reserve(arr->size());
    for (auto const &node : *arr) {
        auto const *p = node.as_table();
        if (p == nullptr) {
            continue;
        }
        auto const rom_off = (*p)["rom_offset"].value_or<std::int64_t>(-1);
        auto const ram_off = (*p)["ram_offset"].value_or<std::int64_t>(0);
        auto const length  = (*p)["length"].value_or<std::int64_t>(-1);
        auto const b64     = (*p)["bytes_b64"].value_or<std::string>("");
        if (rom_off < 0 || length < 0 || b64.empty()) {
            continue;
        }
        auto decoded = st::devices::ets::cipher::base64_decode(b64);
        if (!decoded.has_value()) {
            continue;
        }
        project_patches.push_back({static_cast<std::uint32_t>(rom_off),
                                   static_cast<std::int32_t>(ram_off),
                                   static_cast<std::uint32_t>(length),
                                   std::move(*decoded)});
    }

    // Build sortable (rom_offset, ram_offset, length, bytes) tuples
    // for both sides. Multiset-style comparison: a patch matches iff
    // ALL four fields equal. Sorting both sides and using
    // std::set_intersection / std::set_difference handles the
    // duplicate-rom_offset case correctly — pre-fix, the dict
    // collapsed duplicates and reported 1 false byte-mismatch per
    // duplicate-offset pair (the 78-byte phantom diff on Stage1 was
    // really 78 legitimate duplicate-offset patches).
    struct PT {
        std::uint32_t rom_offset;
        std::int32_t ram_offset;
        std::uint32_t length;
        std::vector<std::uint8_t> bytes;
        auto operator<=>(PT const &o) const = default;
    };
    std::vector<PT> ptm_sorted, proj_sorted;
    ptm_sorted.reserve(ptm_patches.size());
    proj_sorted.reserve(project_patches.size());
    for (auto const &p : ptm_patches) {
        ptm_sorted.push_back({p.rom_offset, p.ram_offset, p.length, p.bytes});
    }
    for (auto const &p : project_patches) {
        proj_sorted.push_back({p.rom_offset, p.ram_offset, p.length, p.bytes});
    }
    std::sort(ptm_sorted.begin(), ptm_sorted.end());
    std::sort(proj_sorted.begin(), proj_sorted.end());

    std::vector<PT> ptm_only_v, proj_only_v;
    std::set_difference(ptm_sorted.begin(), ptm_sorted.end(),
                        proj_sorted.begin(), proj_sorted.end(),
                        std::back_inserter(ptm_only_v));
    std::set_difference(proj_sorted.begin(), proj_sorted.end(),
                        ptm_sorted.begin(), ptm_sorted.end(),
                        std::back_inserter(proj_only_v));
    // "Bytes-different" is the legacy diagnostic: same (rom_offset,
    // ram_offset, length) but different bytes. Compute it from the
    // residuals — patches that didn't match exactly but DO have a
    // counterpart at the same (rom_offset, ram_offset, length).
    auto same_key = [](PT const &a, PT const &b) {
        return a.rom_offset == b.rom_offset && a.ram_offset == b.ram_offset &&
               a.length == b.length;
    };
    std::uint32_t bytes_different = 0;
    for (auto const &a : ptm_only_v) {
        for (auto const &b : proj_only_v) {
            if (same_key(a, b)) {
                ++bytes_different;
                break;
            }
        }
    }
    auto const ptm_only = static_cast<std::uint32_t>(ptm_only_v.size()) - bytes_different;
    auto const proj_only = static_cast<std::uint32_t>(proj_only_v.size()) - bytes_different;
    auto const matched = static_cast<std::uint32_t>(ptm_sorted.size() - ptm_only_v.size());

    bool const matches = (ptm_only == 0 && proj_only == 0 && bytes_different == 0 &&
                          ptm_sorted.size() == proj_sorted.size());
    std::printf("ptm verify: %s vs %s\n", opts.ptm_path.c_str(), opts.project_dir.c_str());
    std::printf("  .ptm patches:        %zu\n", ptm_patches.size());
    std::printf("  project patches:     %zu\n", project_patches.size());
    std::printf("  matched (full):      %u\n", matched);
    std::printf("  .ptm-only:           %u\n", ptm_only);
    std::printf("  project-only:        %u\n", proj_only);
    std::printf("  byte-different:      %u\n", bytes_different);
    std::printf("  match:               %s\n", matches ? "YES (byte-identical)" : "NO");
    return matches ? 0 : 1;
}

struct ImportOpts {
    std::string ptm_path;
    std::string into_dir;     // optional; defaults to <basename>.stune in CWD
    std::string base_rom;     // required for v1 (no auto-discovery yet)
    std::string display_name; // optional; defaults to basename of ptm_path
    std::string def_path;     // optional; populates table_id/table_name in ptm_patches.toml
};

bool parse_import_opts(int argc, char **argv, ImportOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--into" && i + 1 < argc) {
            out.into_dir.assign(argv[++i]);
            continue;
        }
        if (a == "--base-rom" && i + 1 < argc) {
            out.base_rom.assign(argv[++i]);
            continue;
        }
        if (a == "--name" && i + 1 < argc) {
            out.display_name.assign(argv[++i]);
            continue;
        }
        if (a == "--def" && i + 1 < argc) {
            out.def_path.assign(argv[++i]);
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm import: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (!out.ptm_path.empty()) {
            std::fprintf(stderr,
                         "ptm import: only one .ptm path may be given (got '%s' and '%.*s')\n",
                         out.ptm_path.c_str(), static_cast<int>(a.size()), a.data());
            return false;
        }
        out.ptm_path.assign(a);
    }
    if (out.ptm_path.empty()) {
        std::fputs("ptm import: missing required <file.ptm> argument\n", stderr);
        return false;
    }
    if (out.base_rom.empty()) {
        // --base-rom is optional ONLY when ST_PTM_BASE_ROM_DIR is set
        // (auto-discovery via vehicle_id → <env>/<vehicle_id>.bin in
        // run_import after decode). If neither is set, refuse early
        // rather than burning a full decode just to error at the
        // base-rom step.
        char const *env = std::getenv("ST_PTM_BASE_ROM_DIR");
        if (env == nullptr || *env == '\0') {
            std::fputs("ptm import: --base-rom <path> is required (or set\n"
                       "  ST_PTM_BASE_ROM_DIR for auto-discovery via vehicle_id;\n"
                       "  see docs/install.md).\n",
                       stderr);
            return false;
        }
    }
    return true;
}

// When --base-rom wasn't given, try ST_PTM_BASE_ROM_DIR/<vehicle_id>.bin
// as the conventional auto-discovery path. Returns empty string if the
// env var isn't set or the file doesn't exist.
std::string discover_base_rom(std::string const &vehicle_id) {
    char const *root = std::getenv("ST_PTM_BASE_ROM_DIR");
    if (root == nullptr || *root == '\0' || vehicle_id.empty()) {
        return {};
    }
    auto const candidate = std::filesystem::path{root} / (vehicle_id + ".bin");
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) || ec) {
        return {};
    }
    return candidate.string();
}

// Escape a string for TOML's basic-string literal form. Handles `"`,
// `\`, and control chars. Adequate for the well-formed metadata
// content typical of .ptm vendorID / vehicleID fields.
std::string toml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                (void)std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<int>(c));
                out.append(buf);
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

std::string basename_no_ext(std::string const &path) {
    auto const slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    auto const dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base.erase(dot);
    }
    return base;
}

int run_import(int argc, char **argv) {
    ImportOpts opts;
    if (!parse_import_opts(argc, argv, opts)) {
        return 2;
    }

    // Decrypt + decode the .ptm.
    auto loaded = load_ptm(opts.ptm_path);
    if (!loaded.has_value()) {
        std::fprintf(stderr, "ptm import: %s\n", loaded.error().to_string().c_str());
        return loaded.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }
    auto &decoded = loaded->decoded;

    // Auto-discover base ROM if --base-rom wasn't passed. Uses
    // ST_PTM_BASE_ROM_DIR/<vehicle_id>.bin per the convention in
    // docs/install.md.
    if (opts.base_rom.empty()) {
        opts.base_rom = discover_base_rom(decoded.metadata.vehicle_id);
        if (opts.base_rom.empty()) {
            std::fprintf(stderr,
                         "ptm import: --base-rom <path> is required.\n"
                         "  Auto-discovery: set ST_PTM_BASE_ROM_DIR to a directory\n"
                         "  containing '%s.bin', or pass --base-rom explicitly.\n",
                         decoded.metadata.vehicle_id.c_str());
            return 2;
        }
        std::fprintf(stderr, "ptm import: auto-discovered base ROM at %s\n",
                     opts.base_rom.c_str());
    }

    // Read base ROM.
    auto rom_bytes = read_file_bytes(opts.base_rom);
    if (!rom_bytes.has_value()) {
        std::fprintf(stderr, "ptm import: --base-rom: %s\n",
                     rom_bytes.error().to_string().c_str());
        return 1;
    }
    auto const rom_crc = st::crc32(*rom_bytes);

    // Resolve output directory. Default to <basename>.stune in CWD.
    std::filesystem::path out_dir;
    if (!opts.into_dir.empty()) {
        out_dir = std::filesystem::path{opts.into_dir};
    } else {
        out_dir = std::filesystem::path{basename_no_ext(opts.ptm_path) + ".stune"};
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "ptm import: create_directories(%s): %s\n",
                     out_dir.string().c_str(), ec.message().c_str());
        return 1;
    }

    // Write source.bin (the stock base ROM, unmodified).
    {
        std::ofstream out{out_dir / "source.bin", std::ios::binary};
        if (!out) {
            std::fprintf(stderr, "ptm import: write source.bin failed\n");
            return 1;
        }
        out.write(reinterpret_cast<char const *>(rom_bytes->data()),
                  static_cast<std::streamsize>(rom_bytes->size()));
    }

    // Apply all decoded patches to a working copy of the ROM. This
    // gives Project::open a meaningful "working state" — the imported
    // tune as the user sees it. Patches that fall past the ROM size or
    // overflow the buffer are skipped with a warning (rare; usually
    // means the wrong base ROM was passed).
    auto working_bytes = *rom_bytes;
    std::size_t patches_applied = 0;
    std::size_t patches_skipped = 0;
    for (auto const &p : decoded.patches) {
        if (p.bytes.empty()) {
            continue;
        }
        if (p.rom_offset >= working_bytes.size() ||
            p.rom_offset + p.bytes.size() > working_bytes.size()) {
            ++patches_skipped;
            continue;
        }
        std::memcpy(working_bytes.data() + p.rom_offset, p.bytes.data(), p.bytes.size());
        ++patches_applied;
    }
    // Refuse when every patch overflowed the base ROM — almost always
    // means the user passed the wrong base. Silently producing an
    // unmodified working.bin + success exit would let an automation
    // script chain forward into a broken project. Some-but-not-all
    // skipped is still a flag (different ROM revision, corrupt tune)
    // but doesn't preclude the user from continuing if they understand
    // the trade-off — emit a stderr warning and proceed.
    if (patches_applied == 0 && patches_skipped > 0) {
        std::fprintf(stderr,
                     "ptm import: refusing — all %zu patches fall outside the "
                     "base ROM (%zu bytes). The .ptm probably targets a different\n"
                     "  base than `%s`. Check the vehicle_id "
                     "in `ptm inspect <file>` against your base ROM's CID.\n",
                     patches_skipped, rom_bytes->size(), opts.base_rom.c_str());
        return 1;
    }
    if (patches_skipped > 0) {
        std::fprintf(stderr,
                     "ptm import: warning — %zu of %zu patches fell outside the base "
                     "ROM (%zu bytes) and were skipped. The working.bin may not "
                     "match\n  the source tune. Verify with `ptm verify`.\n",
                     patches_skipped, patches_applied + patches_skipped,
                     rom_bytes->size());
    }
    auto const working_crc = st::crc32(working_bytes);
    {
        std::ofstream out{out_dir / "working.bin", std::ios::binary};
        if (!out) {
            std::fprintf(stderr, "ptm import: write working.bin failed\n");
            return 1;
        }
        out.write(reinterpret_cast<char const *>(working_bytes.data()),
                  static_cast<std::streamsize>(working_bytes.size()));
    }

    // Pre-classify patches so the [[patch]] entries can carry the layer
    // tag without a second pass.
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> lengths;
    offsets.reserve(decoded.patches.size());
    lengths.reserve(decoded.patches.size());
    for (auto const &p : decoded.patches) {
        offsets.push_back(p.rom_offset);
        lengths.push_back(p.length);
    }
    auto classifications = st::devices::ets::classify_patches(
        st::devices::ets::default_lf79103p_layer_map(), offsets, lengths);

    // Optional --def: load the pack and compute table ranges so each
    // patch can carry table_id + table_name in ptm_patches.toml.
    std::vector<TableRange> ranges;
    if (!opts.def_path.empty()) {
        auto def = st::Definition::from_file(std::filesystem::path{opts.def_path});
        if (!def.has_value()) {
            std::fprintf(stderr,
                         "ptm import: --def: %s (continuing without table mapping)\n",
                         def.error().to_string().c_str());
        } else {
            ranges = compute_table_ranges(*def);
        }
    }

    // Write project.toml.
    {
        std::string const display =
            opts.display_name.empty() ? basename_no_ext(opts.ptm_path) : opts.display_name;
        std::ofstream out{out_dir / "project.toml"};
        if (!out) {
            std::fprintf(stderr, "ptm import: write project.toml failed\n");
            return 1;
        }
        out << "[project]\n";
        out << "schema_version = 1\n";
        out << "display_name   = \"" << toml_escape(display) << "\"\n";
        out << "notes          = \"imported from " << toml_escape(opts.ptm_path) << "\"\n";
        out << "policy_profile = \"motorsport-only\"\n";
        out << "active_rom_id  = \"\"\n";
        out << "\n";
        out << "[project.source_rom]\n";
        out << "path  = \"source.bin\"\n";
        out << "crc32 = " << rom_crc << "\n";
        out << "\n";
        out << "[project.working_rom]\n";
        out << "path  = \"working.bin\"\n";
        out << "crc32 = " << working_crc << "\n";
        out << "\n";
        // [project.definition] is required by Project::open. When --def
        // was provided we point at the absolute path (the project may
        // live anywhere on disk relative to the pack). When --def was
        // NOT provided we write a placeholder that Project::open will
        // reject — gives the user a clear error if they try to open
        // the project without first wiring up the pack.
        out << "[project.definition]\n";
        if (!opts.def_path.empty()) {
            auto const abs = std::filesystem::weakly_canonical(
                std::filesystem::path{opts.def_path});
            out << "path = \"" << toml_escape(abs.string()) << "\"\n";
        } else {
            out << "path = \"\"  # --def not provided; set this before opening the project\n";
        }
        out << "\n";
        // Round-trip preservation block — spec §"Project metadata".
        out << "[ptm_metadata]\n";
        out << "vendor_id      = \"" << toml_escape(decoded.metadata.vendor_id) << "\"\n";
        out << "vehicle_id     = \"" << toml_escape(decoded.metadata.vehicle_id) << "\"\n";
        out << "lock_mask      = " << decoded.metadata.lock_mask << "\n";
        out << "rom_sum        = \"" << toml_escape(decoded.metadata.rom_sum) << "\"\n";
        out << "save_date_time = \"" << toml_escape(decoded.metadata.save_date_time) << "\"\n";
        out << "imported_from  = \"" << toml_escape(opts.ptm_path) << "\"\n";
    }

    // Write an empty edits.toml so Project::open's history loader has
    // something to chew on. The patches themselves are recorded in
    // ptm_patches.toml (round-trip preserving raw form); edits.toml is
    // the user's local-edit history layered on top.
    {
        std::ofstream out{out_dir / "edits.toml"};
        if (!out) {
            std::fprintf(stderr, "ptm import: write edits.toml failed\n");
            return 1;
        }
        out << "schema_version = 2\n";
        out << "cursor = 0\n";
    }

    // Write ptm_patches.toml — one [[patch]] per decoded patch. Kept
    // distinct from canonical edits.toml because Project::open's edit
    // schema has more constraints; ptm_patches.toml is the v1 round-
    // trip-preserving raw form the future `ptm export` will read back.
    {
        std::ofstream out{out_dir / "ptm_patches.toml"};
        if (!out) {
            std::fprintf(stderr, "ptm import: write ptm_patches.toml failed\n");
            return 1;
        }
        out << "# Generated by `subuwutuner-cli ptm import`. One entry per\n";
        out << "# <patch> element from the source .ptm's PrivateData XML.\n";
        out << "# Round-trip-preserving raw form (rom_offset + ram_offset + bytes).\n";
        out << "# Layer is computed at import time via the architectural classifier.\n";
        out << "\n";
        for (std::size_t i = 0; i < decoded.patches.size(); ++i) {
            auto const &p = decoded.patches[i];
            auto const layer_text = i < classifications.size()
                                        ? st::devices::ets::layer_label(classifications[i].layer)
                                        : std::string_view{"?"};
            auto const bytes_b64 = st::devices::ets::cipher::base64_encode(p.bytes);
            out << "[[patch]]\n";
            out << "rom_offset = " << p.rom_offset << "\n";
            out << "ram_offset = " << p.ram_offset << "\n";
            out << "length     = " << p.length << "\n";
            out << "bytes_b64  = \"" << bytes_b64 << "\"\n";
            out << "layer      = \"" << std::string{layer_text} << "\"\n";
            if (!ranges.empty()) {
                if (auto const *t = find_table_for(ranges, p.rom_offset); t != nullptr) {
                    out << "table_id   = \"" << toml_escape(t->id) << "\"\n";
                    out << "table_name = \"" << toml_escape(t->name) << "\"\n";
                }
            }
            out << "\n";
        }
    }

    // Summary to stdout.
    std::uint64_t total_bytes = 0;
    for (auto const &p : decoded.patches) {
        total_bytes += p.length;
    }
    std::printf("ptm import: %s\n", opts.ptm_path.c_str());
    std::printf("  Decrypted: %s patches, %s bytes total\n",
                with_commas(decoded.patches.size()).c_str(),
                with_commas(total_bytes).c_str());
    std::printf("  Vendor:    %s\n", decoded.metadata.vendor_id.c_str());
    std::printf("  Vehicle:   %s\n", decoded.metadata.vehicle_id.c_str());
    std::printf("  Base ROM:  %s (%s bytes, CRC32=0x%08X)\n",
                opts.base_rom.c_str(),
                with_commas(rom_bytes->size()).c_str(), rom_crc);
    std::printf("  Project:   %s/\n", out_dir.string().c_str());
    std::printf("    project.toml + source.bin + working.bin + edits.toml + ptm_patches.toml\n");
    std::printf("    Patches applied to working.bin: %zu (skipped %zu out-of-bounds)\n",
                patches_applied, patches_skipped);
    if (opts.def_path.empty()) {
        std::printf("    Note: --def was not given; project.toml has a placeholder\n"
                    "          [project.definition].path. Set it before opening the\n"
                    "          project in the GUI.\n");
    }
    if (decoded.metadata.lock_mask != 0) {
        std::printf("  Lock:      mask=%u — preserved for round-trip; do not redistribute\n",
                    decoded.metadata.lock_mask);
    }
    return 0;
}

struct ExportOpts {
    std::string project_dir;
    std::string out_path;
    std::uint32_t seed{0x12345678U};
};

bool parse_export_opts(int argc, char **argv, ExportOpts &out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--as" && i + 1 < argc) {
            out.out_path.assign(argv[++i]);
            continue;
        }
        if (a == "--seed" && i + 1 < argc) {
            std::string_view const v{argv[++i]};
            std::uint32_t parsed = 0;
            if (!parse_uint32_arg(v.data(), parsed)) {
                std::fprintf(stderr, "ptm export: --seed must be a hex/decimal u32 (got '%.*s')\n",
                             static_cast<int>(v.size()), v.data());
                return false;
            }
            out.seed = parsed;
            continue;
        }
        if (a.starts_with("--")) {
            std::fprintf(stderr, "ptm export: unknown flag: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        if (!out.project_dir.empty()) {
            std::fprintf(stderr,
                         "ptm export: only one project dir may be given (got '%s' and '%.*s')\n",
                         out.project_dir.c_str(),
                         static_cast<int>(a.size()), a.data());
            return false;
        }
        out.project_dir.assign(a);
    }
    if (out.project_dir.empty()) {
        std::fputs("ptm export: missing required <project.stune> argument\n", stderr);
        return false;
    }
    if (out.out_path.empty()) {
        std::fputs("ptm export: --as <out.ptm> is required\n", stderr);
        return false;
    }
    return true;
}

// The inner/outer PTM XML builders moved to st::library::ptm_xml_builder
// (2026-06-12 PM consolidation). Both surfaces share the same helper now.

int run_export(int argc, char **argv) {
    ExportOpts opts;
    if (!parse_export_opts(argc, argv, opts)) {
        return 2;
    }

    std::filesystem::path const dir{opts.project_dir};
    auto const proj_toml = dir / "project.toml";
    auto const patches_toml = dir / "ptm_patches.toml";

    if (!std::filesystem::exists(proj_toml)) {
        std::fprintf(stderr, "ptm export: %s missing\n", proj_toml.string().c_str());
        return 1;
    }
    if (!std::filesystem::exists(patches_toml)) {
        std::fprintf(stderr, "ptm export: %s missing (only projects from `ptm import` "
                             "carry this file)\n", patches_toml.string().c_str());
        return 1;
    }

    // Parse project.toml [ptm_metadata].
    toml::table proj;
    try {
        proj = toml::parse_file(proj_toml.string());
    } catch (toml::parse_error const &e) {
        std::fprintf(stderr, "ptm export: project.toml parse error: %s\n", e.description().data());
        return 1;
    }
    auto const *meta = proj["ptm_metadata"].as_table();
    if (meta == nullptr) {
        std::fputs("ptm export: project.toml is missing the [ptm_metadata] block — "
                   "only projects from `ptm import` can be exported back to a .ptm.\n",
                   stderr);
        return 1;
    }
    std::string const vendor_id    = (*meta)["vendor_id"].value_or<std::string>("");
    std::string const vehicle_id   = (*meta)["vehicle_id"].value_or<std::string>("");
    std::uint32_t const lock_mask  = (*meta)["lock_mask"].value_or<std::uint32_t>(0);
    std::string const rom_sum      = (*meta)["rom_sum"].value_or<std::string>("");
    std::string const save_date    = (*meta)["save_date_time"].value_or<std::string>("");

    // Parse ptm_patches.toml [[patch]] entries.
    toml::table pat;
    try {
        pat = toml::parse_file(patches_toml.string());
    } catch (toml::parse_error const &e) {
        std::fprintf(stderr, "ptm export: ptm_patches.toml parse error: %s\n", e.description().data());
        return 1;
    }
    auto const *arr = pat["patch"].as_array();
    if (arr == nullptr) {
        std::fputs("ptm export: ptm_patches.toml has no [[patch]] entries\n", stderr);
        return 1;
    }
    std::vector<st::library::PtmExportPatch> patches;
    patches.reserve(arr->size());
    for (auto const &node : *arr) {
        auto const *p = node.as_table();
        if (p == nullptr) {
            continue;
        }
        auto const rom_off = (*p)["rom_offset"].value_or<std::int64_t>(-1);
        auto const ram_off = (*p)["ram_offset"].value_or<std::int64_t>(0);
        auto const length  = (*p)["length"].value_or<std::int64_t>(-1);
        auto const b64     = (*p)["bytes_b64"].value_or<std::string>("");
        if (rom_off < 0 || length < 0 || b64.empty()) {
            std::fprintf(stderr,
                         "ptm export: malformed [[patch]] entry "
                         "(rom_offset=%lld length=%lld b64-empty=%d) — skipping\n",
                         static_cast<long long>(rom_off),
                         static_cast<long long>(length),
                         b64.empty() ? 1 : 0);
            continue;
        }
        patches.push_back({static_cast<std::uint32_t>(rom_off),
                           static_cast<std::int32_t>(ram_off),
                           static_cast<std::uint32_t>(length),
                           b64});
    }
    // Mirror of the import-side OOB refuse: if every [[patch]] in the
    // file was malformed and got rejected by the loop above, we'd
    // emit a syntactically-valid .ptm with zero patches. That's a
    // corrupt-project signal, not a user choice — refuse instead of
    // silently producing a do-nothing tune.
    if (patches.empty()) {
        std::fputs("ptm export: refusing — every [[patch]] entry in "
                   "ptm_patches.toml was malformed and dropped.\n"
                   "  Check the parse warnings above and confirm the file "
                   "hasn't been hand-edited.\n",
                   stderr);
        return 1;
    }

    auto const inner = st::library::build_ptm_inner_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date, patches);
    auto const outer = st::library::build_ptm_outer_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date);

    auto encrypted = st::devices::ets::cipher::encrypt_ptm(inner, outer, opts.seed);
    if (!encrypted.has_value()) {
        std::fprintf(stderr, "ptm export: encrypt_ptm: %s\n",
                     encrypted.error().to_string().c_str());
        return encrypted.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }

    // Write output.
    std::ofstream out{opts.out_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "ptm export: cannot open output %s\n", opts.out_path.c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(encrypted->data()),
              static_cast<std::streamsize>(encrypted->size()));

    // Summary.
    std::printf("ptm export: %s\n", opts.project_dir.c_str());
    std::printf("  Patches:   %s\n", with_commas(patches.size()).c_str());
    std::printf("  Vendor:    %s\n", vendor_id.c_str());
    std::printf("  Vehicle:   %s\n", vehicle_id.c_str());
    std::printf("  Seed:      0x%08X\n", opts.seed);
    std::printf("  Output:    %s (%s bytes)\n",
                opts.out_path.c_str(), with_commas(encrypted->size()).c_str());
    return 0;
}

int run_inspect(int argc, char **argv) {
    InspectOpts opts;
    if (!parse_inspect_opts(argc, argv, opts)) {
        return 2;
    }

    auto bytes = read_file_bytes(opts.ptm_path);
    if (!bytes.has_value()) {
        std::fprintf(stderr, "ptm inspect: %s\n", bytes.error().to_string().c_str());
        return 1;
    }
    auto const file_size = bytes->size();

    auto contents = st::devices::ets::cipher::decrypt_ptm(*bytes);
    if (!contents.has_value()) {
        std::fprintf(stderr, "ptm inspect: %s\n", contents.error().to_string().c_str());
        return contents.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }

    auto decoded = st::library::decode_ptm_xml(contents->private_data_xml);
    if (!decoded.has_value()) {
        std::fprintf(stderr, "ptm inspect: %s\n", decoded.error().to_string().c_str());
        return 1;
    }

    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> lengths;
    offsets.reserve(decoded->patches.size());
    lengths.reserve(decoded->patches.size());
    for (auto const &p : decoded->patches) {
        offsets.push_back(p.rom_offset);
        lengths.push_back(p.length);
    }
    auto classifications = st::devices::ets::classify_patches(
        st::devices::ets::default_lf79103p_layer_map(), offsets, lengths);
    auto summary = st::devices::ets::summarize(classifications);

    // Optional --def integration. When provided, compute per-table
    // patch counts and pass them through to the renderer for the
    // "Top tables modified" section. Failure to load the pack is
    // surfaced but doesn't fail the whole inspect (caller can still
    // see identity + architectural breakdown without table names).
    std::vector<TableHit> top_tables;
    if (!opts.def_path.empty()) {
        auto def = st::Definition::from_file(std::filesystem::path{opts.def_path});
        if (!def.has_value()) {
            std::fprintf(stderr,
                         "ptm inspect: --def: %s (continuing without table mapping)\n",
                         def.error().to_string().c_str());
        } else {
            auto const ranges = compute_table_ranges(*def);
            top_tables = aggregate_table_hits(*decoded, ranges);
        }
    }

    switch (opts.format) {
    case Format::Text:
        render_inspect_text(opts.ptm_path, file_size, *decoded, summary, top_tables);
        break;
    case Format::Json:
        render_inspect_json(opts.ptm_path, file_size, *decoded, summary, top_tables);
        break;
    case Format::Toml:
        render_inspect_toml(opts.ptm_path, file_size, *decoded, summary, top_tables);
        break;
    }
    return 0;
}

int run_list_patches(int argc, char **argv) {
    ListPatchesOpts opts;
    if (!parse_list_patches_opts(argc, argv, opts)) {
        return 2;
    }

    auto bytes = read_file_bytes(opts.ptm_path);
    if (!bytes.has_value()) {
        std::fprintf(stderr, "ptm list-patches: %s\n", bytes.error().to_string().c_str());
        return 1;
    }

    auto contents = st::devices::ets::cipher::decrypt_ptm(*bytes);
    if (!contents.has_value()) {
        std::fprintf(stderr, "ptm list-patches: %s\n", contents.error().to_string().c_str());
        return contents.error().code() == st::ErrorCode::PolicyDenied ? 2 : 1;
    }

    auto decoded = st::library::decode_ptm_xml(contents->private_data_xml);
    if (!decoded.has_value()) {
        std::fprintf(stderr, "ptm list-patches: %s\n", decoded.error().to_string().c_str());
        return 1;
    }

    // Classifier takes parallel offset+length spans rather than the
    // PatchEntry struct, so the library and classifier headers stay
    // decoupled. Build them once here.
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> lengths;
    offsets.reserve(decoded->patches.size());
    lengths.reserve(decoded->patches.size());
    for (auto const &p : decoded->patches) {
        offsets.push_back(p.rom_offset);
        lengths.push_back(p.length);
    }
    auto classifications = st::devices::ets::classify_patches(
        st::devices::ets::default_lf79103p_layer_map(), offsets, lengths);

    std::vector<TableRange> ranges;
    if (!opts.def_path.empty()) {
        auto def = st::Definition::from_file(std::filesystem::path{opts.def_path});
        if (!def.has_value()) {
            std::fprintf(stderr,
                         "ptm list-patches: --def: %s (continuing without table names)\n",
                         def.error().to_string().c_str());
        } else {
            ranges = compute_table_ranges(*def);
        }
    }

    switch (opts.format) {
    case Format::Text:
        render_text(decoded->patches, classifications, ranges);
        break;
    case Format::Json:
        render_json(decoded->patches, classifications, ranges);
        break;
    case Format::Toml:
        render_toml(decoded->patches, classifications, ranges);
        break;
    }
    return 0;
}

} // namespace ptm_cli

int cmd_ptm(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("Usage: subuwutuner-cli --enable-cobb-ap-cipher ptm <subcommand> [args]\n"
                   "\n"
                   "Subcommands:\n"
                   "  inspect <file.ptm> [--format text|json|toml]\n"
                   "                     Identity block + architectural breakdown.\n"
                   "  diff <a.ptm> <b.ptm> [--format text|json|toml] [--def <p>] [--by-table]\n"
                   "                     Layer or table-grouped diff between two tunes.\n"
                   "  import <file.ptm> --base-rom <path> [--into <dir>] [--name <s>] [--def <p>]\n"
                   "                     Write a .stune project from a .ptm + base ROM.\n"
                   "  export <project.stune> --as <out.ptm> [--seed <hex>]\n"
                   "                     Reverse of import: round-trip the project back to .ptm.\n"
                   "                     Requires ST_ENABLE_COBB_AP_PTM_REWRITE=ON at build time.\n"
                   "  verify <file.ptm> <project.stune>\n"
                   "                     Check that a project's ptm_patches.toml matches the\n"
                   "                     .ptm byte-for-byte. Exit 0 on match, 1 on mismatch.\n"
                   "  list-patches <file.ptm> [--format text|json|toml] [--def <pack-path>]\n"
                   "                     Decode a .ptm and emit one row per patch.\n"
                   "\n"
                   "Common flags:\n"
                   "  --def <pack-path>  Definition pack TOML for table-name resolution\n"
                   "                     (inspect, list-patches, import).\n"
                   "\n"
                   "Cipher gating:\n"
                   "  All ptm subcommands require ST_ENABLE_COBB_AP_CIPHER=ON at build\n"
                   "  time AND --enable-cobb-ap-cipher at runtime. See docs/34 for the\n"
                   "  two-step arming model and rationale.\n",
                   stderr);
        return 2;
    }
    if (!g_cobb_ap_cipher_armed) {
        std::fputs(
            "ptm: PolicyDenied — runtime gate not armed. Pass --enable-cobb-ap-cipher\n"
            "     anywhere in argv to arm the .ptm cipher path. See docs/34.\n",
            stderr);
        return 2;
    }
    std::string_view const sub{argv[0]};
    if (sub == "inspect") {
        return ptm_cli::run_inspect(argc - 1, argv + 1);
    }
    if (sub == "diff") {
        return ptm_cli::run_diff(argc - 1, argv + 1);
    }
    if (sub == "import") {
        return ptm_cli::run_import(argc - 1, argv + 1);
    }
    if (sub == "export") {
        return ptm_cli::run_export(argc - 1, argv + 1);
    }
    if (sub == "verify") {
        return ptm_cli::run_verify(argc - 1, argv + 1);
    }
    if (sub == "list-patches") {
        return ptm_cli::run_list_patches(argc - 1, argv + 1);
    }
    std::fprintf(stderr,
                 "ptm: unknown subcommand: %s\n"
                 "Run `subuwutuner-cli ptm` for the list.\n",
                 argv[0]);
    return 2;
}

int main(int argc, char *argv[]) {
    // Pre-pass: pluck out global options that can appear anywhere in argv
    // and that the subcommand dispatchers shouldn't see. Currently:
    // --enable-bulk-reflash-cipher and --enable-cobb-ap-cipher. We
    // compact argv in place after stripping.
    {
        bool wants_bulk_reflash_cipher = false;
        bool wants_cobb_ap_cipher = false;
        int write = 1;  // argv[0] (program name) stays put
        for (int read = 1; read < argc; ++read) {
            if (std::string_view{argv[read]} == "--enable-bulk-reflash-cipher") {
                wants_bulk_reflash_cipher = true;
                continue;  // strip from argv
            }
            if (std::string_view{argv[read]} == "--enable-cobb-ap-cipher") {
                wants_cobb_ap_cipher = true;
                continue;  // strip from argv
            }
            argv[write++] = argv[read];
        }
        // C standard guarantees argv[argc] == nullptr; preserve that after
        // compaction so subcommands that walk argv defensively still see
        // the terminator at the new end.
        argv[write] = nullptr;
        argc = write;

        if (wants_bulk_reflash_cipher) {
#ifdef ST_ENABLE_BULK_REFLASH_CIPHER
            st::ecu::bulk_reflash::arm();
#else
            // Flag accepted but no-op in this build. We don't hard-error
            // because the user might invoke an unrelated command. Any
            // operation that actually needs the capability will refuse
            // with a docs reference at that point.
            std::fprintf(stderr,
                         "subuwutuner-cli: --enable-bulk-reflash-cipher: not built in\n");
#endif
        }
        if (wants_cobb_ap_cipher) {
            g_cobb_ap_cipher_armed = true;
        }
    }

    if (argc <= 1) {
        print_usage();
        return 0;
    }

    if (arg_matches(argv[1], "-h", "--help")) {
        print_usage();
        return 0;
    }
    if (arg_matches(argv[1], "-V", "--version")) {
        print_version();
        return 0;
    }

    std::string_view const cmd{argv[1]};
    if (cmd == "rom-identify") {
        return cmd_rom_identify(argc - 2, argv + 2);
    }
    if (cmd == "checksum-verify") {
        return cmd_checksum_verify(argc - 2, argv + 2);
    }
    if (cmd == "checksum-repair") {
        return cmd_checksum_repair(argc - 2, argv + 2);
    }
    if (cmd == "pack-list") {
        return cmd_pack_list(argc - 2, argv + 2);
    }
    if (cmd == "rom-info") {
        return cmd_rom_info(argc - 2, argv + 2);
    }
    if (cmd == "dump-axis") {
        return cmd_dump_axis(argc - 2, argv + 2);
    }
    if (cmd == "dump-table") {
        return cmd_dump_table(argc - 2, argv + 2);
    }
    if (cmd == "rom-diff") {
        return cmd_rom_diff(argc - 2, argv + 2);
    }
    if (cmd == "diff") {
        return cmd_diff(argc - 2, argv + 2);
    }
    if (cmd == "analyze-tune-iterations") {
        return cmd_analyze_tune_iterations(argc - 2, argv + 2);
    }
    if (cmd == "diff-load") {
        return cmd_diff_load(argc - 2, argv + 2);
    }
    if (cmd == "table-edit") {
        return cmd_table_edit(argc - 2, argv + 2);
    }
    if (cmd == "project-new") {
        return cmd_project_new(argc - 2, argv + 2);
    }
    if (cmd == "project-info") {
        return cmd_project_info(argc - 2, argv + 2);
    }
    if (cmd == "project-edit") {
        return cmd_project_edit(argc - 2, argv + 2);
    }
    if (cmd == "project-edit-csv") {
        return cmd_project_edit_csv(argc - 2, argv + 2);
    }
    if (cmd == "project-export-csv") {
        return cmd_project_export_csv(argc - 2, argv + 2);
    }
    if (cmd == "project-set-profile") {
        return cmd_project_set_profile(argc - 2, argv + 2);
    }
    if (cmd == "project-add-rom") {
        return cmd_project_add_rom(argc - 2, argv + 2);
    }
    if (cmd == "project-list-roms") {
        return cmd_project_list_roms(argc - 2, argv + 2);
    }
    if (cmd == "project-validate") {
        return cmd_project_validate(argc - 2, argv + 2);
    }
    if (cmd == "project-set-active-rom") {
        return cmd_project_set_active_rom(argc - 2, argv + 2);
    }
    if (cmd == "stats") {
        return cmd_stats(argc - 2, argv + 2);
    }
    if (cmd == "project-clone") {
        return cmd_project_clone(argc - 2, argv + 2);
    }
    if (cmd == "completion") {
        return cmd_completion(argc - 2, argv + 2);
    }
    if (cmd == "table-grep") {
        return cmd_table_grep(argc - 2, argv + 2);
    }
    if (cmd == "changelog") {
        if (argc < 3 || std::string_view{argv[2]} != "show") {
            std::fputs("changelog: missing subcommand. Try `changelog show [--all]`.\n",
                       stderr);
            return 2;
        }
        return cmd_changelog_show(argc - 3, argv + 3);
    }
    if (cmd == "project-history") {
        return cmd_project_history(argc - 2, argv + 2);
    }
    if (cmd == "project-flash") {
        return cmd_project_flash(argc - 2, argv + 2);
    }
    if (cmd == "project-diff") {
        return cmd_project_diff(argc - 2, argv + 2);
    }
    if (cmd == "project-autotune-maf") {
        return cmd_project_autotune_maf(argc - 2, argv + 2);
    }
    if (cmd == "project-autotune-knock-pull") {
        return cmd_project_autotune_knock_pull(argc - 2, argv + 2);
    }
    if (cmd == "pack-info") {
        return cmd_pack_info(argc - 2, argv + 2);
    }
    if (cmd == "pack-lint") {
        return cmd_pack_lint(argc - 2, argv + 2);
    }
    if (cmd == "table-list") {
        return cmd_table_list(argc - 2, argv + 2);
    }
    if (cmd == "primitive-list") {
        return cmd_primitive_list(argc - 2, argv + 2);
    }
    if (cmd == "workflow-list") {
        return cmd_workflow_list(argc - 2, argv + 2);
    }
    if (cmd == "hook-list") {
        return cmd_hook_list(argc - 2, argv + 2);
    }
    if (cmd == "pack-dtcs") {
        return cmd_pack_dtcs(argc - 2, argv + 2);
    }
    if (cmd == "project-disable-dtc") {
        return cmd_project_dtc_toggle(argc - 2, argv + 2, /*enable*/ false);
    }
    if (cmd == "project-enable-dtc") {
        return cmd_project_dtc_toggle(argc - 2, argv + 2, /*enable*/ true);
    }
    if (cmd == "policy") {
        return cmd_policy(argc - 2, argv + 2);
    }
    if (cmd == "project-undo") {
        return cmd_project_step(argc - 2, argv + 2, /*forward=*/false);
    }
    if (cmd == "project-redo") {
        return cmd_project_step(argc - 2, argv + 2, /*forward=*/true);
    }
    if (cmd == "log") {
        return cmd_log(argc - 2, argv + 2);
    }
    if (cmd == "knock-snapshot") {
        return cmd_knock_snapshot(argc - 2, argv + 2);
    }
    if (cmd == "adaptive-history") {
        return cmd_adaptive_history(argc - 2, argv + 2);
    }
    if (cmd == "ai-drift") {
        return cmd_ai_drift(argc - 2, argv + 2);
    }
    if (cmd == "ai-narrate") {
        return cmd_ai_narrate(argc - 2, argv + 2);
    }
    if (cmd == "coldstart-analyze") {
        return cmd_coldstart_analyze(argc - 2, argv + 2);
    }
    if (cmd == "ebcs-analyze") {
        return cmd_ebcs_analyze(argc - 2, argv + 2);
    }
    if (cmd == "can-replay") {
        return cmd_can_replay(argc - 2, argv + 2);
    }
    if (cmd == "can-diff") {
        return cmd_can_diff(argc - 2, argv + 2);
    }
    if (cmd == "can-discover") {
        return cmd_can_discover(argc - 2, argv + 2);
    }
    if (cmd == "can-export-dbc") {
        return cmd_can_export_dbc(argc - 2, argv + 2);
    }
    if (cmd == "can-decode") {
        return cmd_can_decode(argc - 2, argv + 2);
    }
    if (cmd == "flash-plan-info") {
        return cmd_flash_plan_info(argc - 2, argv + 2);
    }
    if (cmd == "flash-manifest-info") {
        return cmd_flash_manifest_info(argc - 2, argv + 2);
    }
    if (cmd == "flash-delta") {
        return cmd_flash_delta(argc - 2, argv + 2);
    }
    if (cmd == "flash-resume") {
        return cmd_flash_resume(argc - 2, argv + 2);
    }
    if (cmd == "flash-apply") {
        return cmd_flash_apply(argc - 2, argv + 2);
    }
    if (cmd == "list-validators") {
        return cmd_list_validators(argc - 2, argv + 2);
    }
    if (cmd == "did-datalog-preset") {
        return cmd_did_datalog_preset(argc - 2, argv + 2);
    }
    if (cmd == "cobb-datalog-preset") {
        // Deprecated alias kept for one release cycle. Emits a stderr
        // warning so scripts can be updated; semantics are identical.
        std::fputs("cobb-datalog-preset: deprecated alias for "
                   "did-datalog-preset; please update your scripts.\n",
                   stderr);
        return cmd_did_datalog_preset(argc - 2, argv + 2);
    }
    if (cmd == "transport-list") {
        return cmd_transport_list(argc - 2, argv + 2);
    }
    if (cmd == "doctor") {
        return cmd_doctor(argc - 2, argv + 2);
    }
    if (cmd == "rom-pull") {
        return cmd_rom_pull(argc - 2, argv + 2);
    }
    if (cmd == "sniff") {
        return cmd_sniff(argc - 2, argv + 2);
    }
    if (cmd == "ssm-a8-poll") {
        return cmd_ssm_a8_poll(argc - 2, argv + 2);
    }
    if (cmd == "rdbi") {
        return cmd_rdbi(argc - 2, argv + 2);
    }
    if (cmd == "obd-info") {
        return cmd_obd_info(argc - 2, argv + 2);
    }
    if (cmd == "config") {
        return cmd_config(argc - 2, argv + 2);
    }
    if (cmd == "profile") {
        if (argc < 3) {
            std::fputs("profile: missing subcommand. Try `profile list|show|import|export`.\n",
                       stderr);
            return 2;
        }
        std::string_view const sub{argv[2]};
        auto const profile_dir_opt = [&]() -> std::filesystem::path {
            // Allow --dir <path> override via the remaining args (cheap
            // scan; subcommands do their own arg parse below).
            for (int i = 3; i + 1 < argc; ++i) {
                if (std::string_view{argv[i]} == "--dir") {
                    return std::filesystem::path{argv[i + 1]};
                }
            }
            return st::profile::default_profile_dir();
        }();

        if (sub == "list") {
            auto profiles = st::profile::list(profile_dir_opt);
            if (!profiles.has_value()) {
                std::fprintf(stderr, "profile list: %s\n",
                             profiles.error().to_string().c_str());
                return 1;
            }
            std::printf("Profile dir: %s\n", profile_dir_opt.string().c_str());
            std::printf("%zu profile%s\n", profiles->size(),
                        profiles->size() == 1 ? "" : "s");
            for (auto const &p : *profiles) {
                std::printf("  %s — %s\n", p.id.c_str(),
                            p.display_name.empty() ? "(no display name)"
                                                   : p.display_name.c_str());
                if (!p.ecus.empty() && !p.ecus[0].cal_id.empty()) {
                    std::printf("       ecu: %s (%s)\n", p.ecus[0].cal_id.c_str(),
                                p.ecus[0].ecu_part.c_str());
                }
            }
            return 0;
        }
        if (sub == "show") {
            if (argc < 4) {
                std::fputs("profile show: missing <id-or-path>\n"
                           "Usage: subuwutuner-cli profile show <id-or-path> "
                           "[--dir <dir>]\n",
                           stderr);
                return 2;
            }
            std::filesystem::path src{argv[3]};
            if (!src.has_extension())
                src = profile_dir_opt / (src.string() + ".stprofile");
            auto p = st::profile::load(src);
            if (!p.has_value()) {
                std::fprintf(stderr, "profile show: %s\n", p.error().to_string().c_str());
                return 1;
            }
            std::printf("Profile:     %s\n", p->id.c_str());
            std::printf("Display:     %s\n", p->display_name.c_str());
            if (!p->vin.empty())
                std::printf("VIN:         %s\n", p->vin.c_str());
            if (!p->year.empty() || !p->make.empty() || !p->model.empty()) {
                std::printf("Vehicle:     %s %s %s\n", p->year.c_str(), p->make.c_str(),
                            p->model.c_str());
            }
            if (!p->transmission.empty())
                std::printf("Trans:       %s\n", p->transmission.c_str());
            if (!p->transport_hint.empty())
                std::printf("Transport:   %s\n", p->transport_hint.c_str());
            for (auto const &e : p->ecus) {
                std::printf("ECU:         %s — cal %s, part %s, pack %s\n", e.role.c_str(),
                            e.cal_id.c_str(), e.ecu_part.c_str(), e.pack_id.c_str());
            }
            if (p->last_flash.has_value()) {
                auto const &lf = *p->last_flash;
                std::printf("Last flash:  %s\n", lf.timestamp_iso.c_str());
                std::printf("             src CRC32 0x%08X, target CRC32 0x%08X\n",
                            lf.source_rom_crc32, lf.target_rom_crc32);
                if (!lf.backup_path.empty())
                    std::printf("             backup: %s\n", lf.backup_path.c_str());
                if (!lf.operator_notes.empty())
                    std::printf("             notes:  %s\n", lf.operator_notes.c_str());
            }
            if (!p->notes.empty())
                std::printf("Notes:       %s\n", p->notes.c_str());
            return 0;
        }
        if (sub == "create") {
            // profile create --id <slug> [--display-name "..."] [--year ...]
            //                [--make ...] [--model ...] [--transmission ...]
            //                [--transport-hint ...] [--dir <dir>]
            st::profile::VehicleProfile vp;
            for (int i = 3; i < argc; ++i) {
                std::string_view const a{argv[i]};
                auto const need_val = [&](char const *name) -> char const * {
                    if (i + 1 >= argc) {
                        std::fprintf(stderr, "profile create: %s requires a value\n", name);
                        return nullptr;
                    }
                    return argv[++i];
                };
                if (a == "--id") {
                    if (auto const *v = need_val("--id"); v) vp.id = std::string{v};
                    else return 2;
                } else if (a == "--display-name") {
                    if (auto const *v = need_val("--display-name"); v) vp.display_name = std::string{v};
                    else return 2;
                } else if (a == "--year") {
                    if (auto const *v = need_val("--year"); v) vp.year = std::string{v};
                    else return 2;
                } else if (a == "--make") {
                    if (auto const *v = need_val("--make"); v) vp.make = std::string{v};
                    else return 2;
                } else if (a == "--model") {
                    if (auto const *v = need_val("--model"); v) vp.model = std::string{v};
                    else return 2;
                } else if (a == "--transmission") {
                    if (auto const *v = need_val("--transmission"); v) vp.transmission = std::string{v};
                    else return 2;
                } else if (a == "--transport-hint") {
                    if (auto const *v = need_val("--transport-hint"); v) vp.transport_hint = std::string{v};
                    else return 2;
                } else if (a == "--notes") {
                    if (auto const *v = need_val("--notes"); v) vp.notes = std::string{v};
                    else return 2;
                } else if (a == "--dir") {
                    if (auto const *v = need_val("--dir"); v) (void)v; // consumed by up-front scan
                    else return 2;
                } else if (a.starts_with("--")) {
                    std::fprintf(stderr, "profile create: unknown option: %s\n", argv[i]);
                    return 2;
                }
            }
            if (vp.id.empty()) {
                std::fputs("profile create: --id is required\n", stderr);
                return 2;
            }
            if (vp.display_name.empty()) {
                vp.display_name = vp.id;
            }
            std::error_code ec;
            std::filesystem::create_directories(profile_dir_opt, ec);
            if (ec) {
                std::fprintf(stderr, "profile create: mkdir %s: %s\n",
                             profile_dir_opt.string().c_str(),
                             ec.message().c_str());
                return 1;
            }
            auto const target = profile_dir_opt / (vp.id + ".stprofile");
            if (std::filesystem::exists(target)) {
                std::fprintf(stderr, "profile create: already exists: %s\n",
                             target.string().c_str());
                return 1;
            }
            if (auto s = st::profile::save(vp, target); !s.has_value()) {
                std::fprintf(stderr, "profile create: %s\n", s.error().to_string().c_str());
                return 1;
            }
            std::printf("Created %s\n", target.string().c_str());
            return 0;
        }
        if (sub == "import") {
            if (argc < 4) {
                std::fputs("profile import: missing <FILE.stprofile>\n", stderr);
                return 2;
            }
            std::filesystem::path src{argv[3]};
            auto p = st::profile::load(src);
            if (!p.has_value()) {
                std::fprintf(stderr, "profile import: %s\n", p.error().to_string().c_str());
                return 1;
            }
            auto const dst = profile_dir_opt / (p->id + ".stprofile");
            if (auto s = st::profile::save(*p, dst); !s.has_value()) {
                std::fprintf(stderr, "profile import: %s\n", s.error().to_string().c_str());
                return 1;
            }
            std::printf("Imported profile '%s' → %s\n", p->id.c_str(), dst.string().c_str());
            return 0;
        }
        if (sub == "export") {
            if (argc < 5) {
                std::fputs("profile export: missing <id> <FILE.stprofile>\n", stderr);
                return 2;
            }
            std::string const id{argv[3]};
            std::filesystem::path const dst{argv[4]};
            auto src = profile_dir_opt / (id + ".stprofile");
            auto p = st::profile::load(src);
            if (!p.has_value()) {
                std::fprintf(stderr, "profile export: %s\n", p.error().to_string().c_str());
                return 1;
            }
            if (auto s = st::profile::save(*p, dst); !s.has_value()) {
                std::fprintf(stderr, "profile export: %s\n", s.error().to_string().c_str());
                return 1;
            }
            std::printf("Exported '%s' → %s\n", id.c_str(), dst.string().c_str());
            return 0;
        }
        if (sub == "delete") {
            if (argc < 4) {
                std::fputs("profile delete: missing <id>\n"
                           "Usage: subuwutuner-cli profile delete <id> [--dir <dir>] "
                           "[--yes]\n",
                           stderr);
                return 2;
            }
            std::string id{argv[3]};
            bool confirmed = false;
            for (int i = 4; i < argc; ++i) {
                std::string_view const a{argv[i]};
                if (a == "--yes") {
                    confirmed = true;
                } else if (a == "--dir") {
                    if (i + 1 >= argc) {
                        std::fprintf(stderr, "profile delete: --dir requires a value\n");
                        return 2;
                    }
                    ++i; // already consumed via the up-front --dir scan
                } else if (a.starts_with("--")) {
                    std::fprintf(stderr, "profile delete: unknown option: %s\n", argv[i]);
                    return 2;
                }
            }
            if (!confirmed) {
                std::fputs("profile delete: refusing without --yes confirmation.\n"
                           "  Re-run with --yes to delete the file.\n",
                           stderr);
                return 2;
            }
            auto const target = profile_dir_opt / (id + ".stprofile");
            std::error_code ec;
            if (!std::filesystem::exists(target, ec)) {
                std::fprintf(stderr, "profile delete: not found: %s\n",
                             target.string().c_str());
                return 1;
            }
            if (!std::filesystem::remove(target, ec) || ec) {
                std::fprintf(stderr, "profile delete: %s\n", ec.message().c_str());
                return 1;
            }
            std::printf("Deleted %s\n", target.string().c_str());
            return 0;
        }
        std::fprintf(stderr, "profile: unknown subcommand '%s' "
                             "(try list / show / import / export / delete)\n",
                     argv[2]);
        return 2;
    }
    if (cmd == "audit") {
        if (argc < 3) {
            std::fputs("audit: missing subcommand. Try `audit show <project>`, "
                       "`audit stats <project>`, `audit verify <project>`, or "
                       "`audit append <project> --kind <name> --description <text>`.\n",
                       stderr);
            return 2;
        }
        std::string_view const sub{argv[2]};
        if (sub == "append") {
            // audit append <project-dir> --kind <name> --description <text>
            //                            [--source <text>] [--field key=value ...]
            std::optional<std::filesystem::path> project_dir;
            std::string kind_name = "custom";
            std::string description;
            std::string source = "cli.audit_append";
            std::vector<std::pair<std::string, std::string>> fields;
            for (int i = 3; i < argc; ++i) {
                std::string_view const a{argv[i]};
                auto const need_val = [&](char const *name) -> char const * {
                    if (i + 1 >= argc) {
                        std::fprintf(stderr, "audit append: %s requires a value\n", name);
                        return nullptr;
                    }
                    return argv[++i];
                };
                if (a == "--kind") {
                    if (auto const *v = need_val("--kind"); v)
                        kind_name = std::string{v};
                    else
                        return 2;
                } else if (a == "--description" || a == "--desc") {
                    if (auto const *v = need_val("--description"); v)
                        description = std::string{v};
                    else
                        return 2;
                } else if (a == "--source") {
                    if (auto const *v = need_val("--source"); v)
                        source = std::string{v};
                    else
                        return 2;
                } else if (a == "--field") {
                    if (auto const *v = need_val("--field"); v) {
                        std::string_view const kv{v};
                        auto const eq = kv.find('=');
                        if (eq == std::string_view::npos) {
                            std::fprintf(stderr,
                                         "audit append: --field expects key=value, got '%s'\n", v);
                            return 2;
                        }
                        fields.emplace_back(std::string{kv.substr(0, eq)},
                                            std::string{kv.substr(eq + 1)});
                    } else {
                        return 2;
                    }
                } else if (a.starts_with("--")) {
                    std::fprintf(stderr, "audit append: unknown option: %s\n", argv[i]);
                    return 2;
                } else if (!project_dir.has_value()) {
                    project_dir = std::filesystem::path{argv[i]};
                } else {
                    std::fprintf(stderr, "audit append: extra positional: %s\n", argv[i]);
                    return 2;
                }
            }
            if (!project_dir.has_value() || description.empty()) {
                std::fputs("audit append: missing required arguments\n"
                           "Usage: subuwutuner-cli audit append <project-dir> \\\n"
                           "         --kind <kind-name>  (default: custom)\n"
                           "         --description \"What happened\"  (required)\n"
                           "         [--source <text>]  (default: cli.audit_append)\n"
                           "         [--field key=value]  (repeatable)\n",
                           stderr);
                return 2;
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(*project_dir, ec) || ec) {
                std::fprintf(stderr, "audit append: not a project dir: %s\n",
                             project_dir->string().c_str());
                return 1;
            }
            auto const log_path = *project_dir / "audit.log";
            auto log_r = st::audit::AuditLog::open(log_path);
            if (!log_r.has_value()) {
                std::fprintf(stderr, "audit append: %s\n",
                             log_r.error().to_string().c_str());
                return 1;
            }
            auto const kind = st::audit::kind_from_name(kind_name);
            auto status = log_r->log(kind, std::move(source), std::move(description),
                                     std::move(fields));
            if (!status.has_value()) {
                std::fprintf(stderr, "audit append: %s\n",
                             status.error().to_string().c_str());
                return 1;
            }
            std::printf("Appended to %s\n", log_path.string().c_str());
            return 0;
        }
        if (sub == "verify") {
            // audit verify <project-dir> [--json]
            // Walks every entry and reports any whose on-disk CRC32
            // doesn't match the recomputed value. Exit nonzero when
            // any entry fails so CI / cron can gate on integrity.
            std::optional<std::filesystem::path> project_dir;
            bool json_mode = false;
            for (int i = 3; i < argc; ++i) {
                std::string_view const a{argv[i]};
                if (a == "--json") {
                    json_mode = true;
                } else if (a.starts_with("--")) {
                    std::fprintf(stderr, "audit verify: unknown option: %s\n", argv[i]);
                    return 2;
                } else if (!project_dir.has_value()) {
                    project_dir = std::filesystem::path{argv[i]};
                } else {
                    std::fprintf(stderr, "audit verify: extra positional: %s\n", argv[i]);
                    return 2;
                }
            }
            if (!project_dir.has_value()) {
                std::fputs("audit verify: missing <project-dir>\n", stderr);
                return 2;
            }
            auto const log_path = *project_dir / "audit.log";
            if (!std::filesystem::exists(log_path)) {
                if (json_mode) {
                    std::printf("{\"schema\":\"subuwutuner.audit-verify.v1\","
                                "\"log_path\":\"%s\",\"present\":false,\"entries\":0,"
                                "\"bad_checksum\":0}\n",
                                log_path.string().c_str());
                } else {
                    std::printf("Audit log: %s\n  (not present — verify OK)\n",
                                log_path.string().c_str());
                }
                return 0;
            }
            auto entries = st::audit::read_all(log_path);
            if (!entries.has_value()) {
                std::fprintf(stderr, "audit verify: %s\n",
                             entries.error().to_string().c_str());
                return 1;
            }
            std::size_t bad = 0;
            std::vector<std::size_t> bad_indices;
            for (std::size_t i = 0; i < entries->size(); ++i) {
                if (!(*entries)[i].checksum_valid) {
                    ++bad;
                    bad_indices.push_back(i);
                }
            }
            if (json_mode) {
                std::string out;
                out.append("{\"schema\":\"subuwutuner.audit-verify.v1\",\"log_path\":");
                json_escape(out, log_path.string());
                out.append(",\"present\":true,\"entries\":");
                out.append(std::to_string(entries->size()));
                out.append(",\"bad_checksum\":");
                out.append(std::to_string(bad));
                out.append(",\"bad_indices\":[");
                for (std::size_t j = 0; j < bad_indices.size(); ++j) {
                    if (j > 0)
                        out.append(",");
                    out.append(std::to_string(bad_indices[j]));
                }
                out.append("]}\n");
                std::fputs(out.c_str(), stdout);
            } else {
                std::printf("Audit log: %s\n", log_path.string().c_str());
                std::printf("  Entries:        %zu\n", entries->size());
                std::printf("  Bad checksum:   %zu\n", bad);
                if (bad > 0) {
                    std::printf("  Affected lines (0-indexed):");
                    for (auto idx : bad_indices) {
                        std::printf(" %zu", idx);
                    }
                    std::printf("\n");
                    std::printf("  Result: TAMPERED OR CORRUPTED\n");
                } else {
                    std::printf("  Result: OK\n");
                }
            }
            return bad == 0 ? 0 : 1;
        }
        if (sub == "stats") {
            // audit stats <project-dir> [--json]
            // Aggregate per-kind counts + bad-checksum count + first/
            // last timestamp. Read-only summary; no integrity gate
            // (that's `verify`'s job). Useful "how active is this
            // project's audit log" snapshot without paging through
            // every entry.
            std::optional<std::filesystem::path> project_dir;
            bool json_mode = false;
            for (int i = 3; i < argc; ++i) {
                std::string_view const a{argv[i]};
                if (a == "--json") {
                    json_mode = true;
                } else if (a.starts_with("--")) {
                    std::fprintf(stderr, "audit stats: unknown option: %s\n", argv[i]);
                    return 2;
                } else if (!project_dir.has_value()) {
                    project_dir = std::filesystem::path{argv[i]};
                } else {
                    std::fprintf(stderr, "audit stats: extra positional: %s\n", argv[i]);
                    return 2;
                }
            }
            if (!project_dir.has_value()) {
                std::fputs("audit stats: missing <project-dir>\n", stderr);
                return 2;
            }
            auto const log_path = *project_dir / "audit.log";
            if (!std::filesystem::exists(log_path)) {
                if (json_mode) {
                    std::string out;
                    out.append("{\"schema\":\"subuwutuner.audit-stats.v1\","
                               "\"log_path\":");
                    json_escape(out, log_path.string());
                    out.append(",\"present\":false,\"entries\":0,\"bad_checksum\":0,"
                               "\"by_kind\":{}}\n");
                    std::fputs(out.c_str(), stdout);
                } else {
                    std::printf("Audit log: %s\n  (not present)\n",
                                log_path.string().c_str());
                }
                return 0;
            }
            auto entries = st::audit::read_all(log_path);
            if (!entries.has_value()) {
                std::fprintf(stderr, "audit stats: %s\n",
                             entries.error().to_string().c_str());
                return 1;
            }
            // Aggregate. Tally is small (< 30 kinds), linear is fine.
            std::vector<std::pair<std::string_view, std::size_t>> by_kind;
            std::size_t bad = 0;
            std::int64_t ts_min = 0;
            std::int64_t ts_max = 0;
            bool ts_initialized = false;
            for (auto const &e : *entries) {
                if (!e.checksum_valid) {
                    ++bad;
                }
                if (!ts_initialized || e.timestamp_ns < ts_min)
                    ts_min = e.timestamp_ns;
                if (!ts_initialized || e.timestamp_ns > ts_max)
                    ts_max = e.timestamp_ns;
                ts_initialized = true;
                auto const kn = st::audit::kind_name(e.kind);
                bool hit = false;
                for (auto &p : by_kind) {
                    if (p.first == kn) {
                        ++p.second;
                        hit = true;
                        break;
                    }
                }
                if (!hit) {
                    by_kind.emplace_back(kn, std::size_t{1});
                }
            }
            // Sort by count desc, kind name asc for ties — stable
            // ordering across runs.
            std::sort(by_kind.begin(), by_kind.end(),
                      [](auto const &x, auto const &y) {
                          if (x.second != y.second)
                              return x.second > y.second;
                          return x.first < y.first;
                      });
            if (json_mode) {
                std::string out;
                out.append("{\"schema\":\"subuwutuner.audit-stats.v1\",\"log_path\":");
                json_escape(out, log_path.string());
                out.append(",\"present\":true,\"entries\":");
                out.append(std::to_string(entries->size()));
                out.append(",\"bad_checksum\":");
                out.append(std::to_string(bad));
                if (ts_initialized) {
                    out.append(",\"first_ts_ns\":");
                    out.append(std::to_string(ts_min));
                    out.append(",\"last_ts_ns\":");
                    out.append(std::to_string(ts_max));
                }
                out.append(",\"by_kind\":{");
                for (std::size_t i = 0; i < by_kind.size(); ++i) {
                    if (i > 0)
                        out.append(",");
                    json_escape(out, by_kind[i].first);
                    out.append(":");
                    out.append(std::to_string(by_kind[i].second));
                }
                out.append("}}\n");
                std::fputs(out.c_str(), stdout);
            } else {
                std::printf("Audit log: %s\n", log_path.string().c_str());
                std::printf("  Entries:      %zu\n", entries->size());
                std::printf("  Bad checksum: %zu\n", bad);
                if (ts_initialized) {
                    // ISO-8601-ish formatting. Use the same helper if
                    // available; fall back to bare ns count.
                    auto fmt = [](std::int64_t ns) {
                        char buf[40];
                        std::time_t const sec = ns / 1'000'000'000LL;
                        std::tm tm{};
#if defined(_WIN32)
                        gmtime_s(&tm, &sec);
#else
                        gmtime_r(&sec, &tm);
#endif
                        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
                        return std::string{buf};
                    };
                    std::printf("  First entry:  %s\n", fmt(ts_min).c_str());
                    std::printf("  Last entry:   %s\n", fmt(ts_max).c_str());
                }
                std::printf("\nBy kind:\n");
                for (auto const &p : by_kind) {
                    std::printf("  %-32.*s  %zu\n", static_cast<int>(p.first.size()),
                                p.first.data(), p.second);
                }
            }
            return 0;
        }
        if (sub != "show") {
            std::fprintf(stderr,
                         "audit: unknown subcommand '%s' (try 'show', 'append', 'verify', "
                         "or 'stats')\n",
                         argv[2]);
            return 2;
        }
        // audit show <project-dir> [--json]
        std::optional<std::filesystem::path> project_dir;
        bool json_mode = false;
        for (int i = 3; i < argc; ++i) {
            std::string_view const a{argv[i]};
            if (a == "--json") {
                json_mode = true;
            } else if (a.starts_with("--")) {
                std::fprintf(stderr, "audit show: unknown option: %s\n", argv[i]);
                return 2;
            } else if (!project_dir.has_value()) {
                project_dir = std::filesystem::path{argv[i]};
            } else {
                std::fprintf(stderr, "audit show: extra positional argument: %s\n", argv[i]);
                return 2;
            }
        }
        if (!project_dir.has_value()) {
            std::fputs("audit show: missing <project-dir>\n", stderr);
            return 2;
        }
        auto const log_path = *project_dir / "audit.log";
        auto entries = st::audit::read_all(log_path);
        if (!entries.has_value()) {
            std::fprintf(stderr, "audit show: %s\n", entries.error().to_string().c_str());
            return 1;
        }
        if (json_mode) {
            std::string out;
            out.reserve(1024);
            out.append("{\"schema\":\"subuwutuner.audit.v1\",\"project_dir\":");
            json_escape(out, project_dir->string());
            out.append(",\"log_path\":");
            json_escape(out, log_path.string());
            out.append(",\"count\":");
            out.append(std::to_string(entries->size()));
            out.append(",\"entries\":[");
            for (std::size_t i = 0; i < entries->size(); ++i) {
                if (i != 0)
                    out.append(",");
                // Re-serialize each entry through the canonical
                // serializer to ensure on-wire identity.
                out.append(st::audit::serialize_entry((*entries)[i]));
            }
            out.append("]}\n");
            std::fputs(out.c_str(), stdout);
        } else {
            std::printf("Audit log: %s\n", log_path.string().c_str());
            std::printf("%zu entries\n\n", entries->size());
            for (auto const &e : *entries) {
                // Format the ns timestamp as ISO-8601 UTC for human reading.
                auto const seconds = e.timestamp_ns / 1'000'000'000LL;
                std::time_t const tt = static_cast<std::time_t>(seconds);
                std::tm tm_utc{};
#if defined(_WIN32)
                gmtime_s(&tm_utc, &tt);
#else
                gmtime_r(&tt, &tm_utc);
#endif
                char ts[24];
                std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
                char const *tag = e.checksum_valid ? "    " : "[TAMPERED] ";
                std::printf("%s%s  %.*s  %s\n", tag, ts,
                            static_cast<int>(st::audit::kind_name(e.kind).size()),
                            st::audit::kind_name(e.kind).data(),
                            e.description.c_str());
                if (!e.source.empty()) {
                    std::printf("            source: %s\n", e.source.c_str());
                }
                for (auto const &[k, v] : e.fields) {
                    std::printf("            %s = %s\n", k.c_str(), v.c_str());
                }
            }
        }
        return 0;
    }
    if (cmd == "flash-trace") {
        return cmd_flash_trace(argc - 2, argv + 2);
    }
    if (cmd == "lint-graph") {
        return cmd_lint_graph(argc - 2, argv + 2);
    }
    if (cmd == "dump-ir") {
        return cmd_dump_ir(argc - 2, argv + 2);
    }
    if (cmd == "lint-ir") {
        return cmd_lint_ir(argc - 2, argv + 2);
    }
    if (cmd == "feature-compile") {
        return cmd_feature_compile(argc - 2, argv + 2);
    }
    if (cmd == "ap3") {
        return cmd_ap3(argc - 2, argv + 2);
    }
    if (cmd == "ptm") {
        return cmd_ptm(argc - 2, argv + 2);
    }
    if (cmd == "autotune") {
        // Match the `config` subcommand-help shape: list all known
        // subcommands when none was passed, point at the top-level
        // --help for the full per-subcommand flag set.
        if (argc < 3) {
            std::fputs("Usage: subuwutuner-cli autotune <subcommand> [args]\n"
                       "\n"
                       "Subcommands:\n"
                       "  maf          MAF-scaling correction from a CSV log\n"
                       "  knock-pull   Knock-pull (ignition retract) from a CSV log\n"
                       "\n"
                       "Run `subuwutuner-cli --help` for the full flag set on each.\n",
                       stderr);
            return 2;
        }
        std::string_view const sub{argv[2]};
        if (sub == "maf") {
            return cmd_autotune_maf(argc - 3, argv + 3);
        }
        if (sub == "knock-pull") {
            return cmd_autotune_knock_pull(argc - 3, argv + 3);
        }
        std::fprintf(stderr,
                     "autotune: unknown subcommand: %s\n"
                     "Run `subuwutuner-cli autotune` for the list of subcommands.\n",
                     argv[2]);
        return 2;
    }

    std::fprintf(stderr, "subuwutuner-cli: unknown argument: %s\n", argv[1]);
    std::fprintf(stderr, "Try 'subuwutuner-cli --help'.\n");
    return 2;
}
