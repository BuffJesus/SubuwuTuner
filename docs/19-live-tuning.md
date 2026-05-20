# 19 — Live tuning

Live tuning means editing a calibration value while the engine is running, with the change taking effect on the very next ECU loop iteration — no flash cycle, no key-off. It is the canonical dyno-tuner workflow: pull on the dyno, AFR is two points lean in cell X, tuner clicks the cell, types a value, the next pull is correct. Atlas supports this; nothing in the public Subaru-tuning open-source ecosystem does (RomRaider has the *protocol* support but ships no UI for it).

This document is the design + roadmap for adding it to SubuwuTuner. **It is forward-looking; live tuning is not implemented yet.** Building it now would land before Phase 4 hardware validation, which is the wrong order — see §"Roadmap placement" below.

## What it is, precisely

Live tuning is *not* a different way of flashing. It is a runtime override of specific calibration regions backed by two underlying ECU mechanisms:

1. **RAM-shadow override.** Subaru's bootloader (and most modern OEM bootloaders) supports rerouting reads from a flash region to a RAM region instead. The tool sends a command meaning "for the calibration tables that live at flash addresses A1..A2, read from RAM addresses R1..R2 instead." From that moment until power-off, those tables are mutable at runtime.
2. **SSM / UDS write to RAM.** With the override in place, the tool writes new values to that RAM region via the standard SSM Write Memory (older Subaru) or UDS WriteDataByIdentifier (newer Subaru) command. The ECU reads the new value on its next control loop — typically within 10–50 ms.

Once the tuner is happy, the new calibration values are also written to flash so they persist past key-off. The flash step is identical to a normal flash; it's not part of "live tuning" proper.

**What the user perceives:** "I clicked a cell, the engine responded immediately." What is happening: the tool sent one short UDS frame and the ECU's read of that table on the next 10 ms cycle returned the new value.

## Why it matters

Three concrete workflows, in descending order of "what tuners actually do":

- **Dyno tuning.** Single biggest motivation. Without live tuning, every cell edit costs ~30 s of "stop the pull, save, flash, restart, pull again." With live tuning it is conversational.
- **Limp-home recovery experiments.** Trying a value to see if it un-sticks a knock-induced limp; the moment you find it, you reflash. Without live tuning the experiment cycle is too slow to bracket.
- **On-road triage.** A tuner is in the passenger seat, customer drives, tuner adjusts. Less common but valuable for cold-start and idle work where dynos don't help.

Live tuning is not "auto-tune live." Auto-tune (`docs/12`) proposes edits from a recorded log; live tuning lets the user apply manual edits at speed. They are complementary — auto-tune for the bulk, live tuning for the long-tail polish.

## Architectural fit

Live tuning is the first SubuwuTuner subsystem that is *bidirectional*. Every prior subsystem either reads from the ECU (`st::log`, `rom-pull`), or writes to it as a discrete event (`st::flash`). Live tuning is a sustained two-way conversation: the tool publishes a new value, the ECU acknowledges, the tool may then immediately publish another value. The module's threading model is more like a chat session than a flash plan.

```
        +----------------+        UDS WriteDataByIdentifier        +-------+
        | st::live_tune  | -----------------------------------> |  ECU   |
        |    Session     | <----------------------------------- |        |
        +----------------+        UDS PositiveResponse / NRC      +-------+
              |  ^
              |  |   address resolution: which RAM page is this
              v  |   table mirrored to right now?
        +----------------+
        |  st::defs +    |
        |  RAM-shadow    |
        |  map (per CID) |
        +----------------+
```

New module: `st::live_tune` (new directory `src/live_tune/`). Public surface (provisional):

```cpp
namespace st::live_tune {

struct Session {
    static Result<Session> open(ITransport &t, Definition const &def);
    Status close();

    // Enter RAM-shadow mode for a set of tables. ECU now reads those
    // tables from RAM. Idempotent.
    Status enable_overrides(std::span<std::string_view> table_ids);

    // Write a new cell value to the RAM-shadow for `table_id`. Returns
    // when the ECU has acknowledged the write (or NACK'd it).
    Status write_cell(std::string_view table_id,
                      std::size_t        row,
                      std::size_t        col,
                      double             value);

    // Read back what the ECU currently has in RAM for that cell.
    Result<double> read_cell(std::string_view table_id,
                             std::size_t        row,
                             std::size_t        col);
};

}  // namespace st::live_tune
```

The address resolution layer maps a `(table_id, row, col)` to the runtime RAM-shadow address. This is per-CID data — every firmware has a specific mapping from the calibration's flash layout to its RAM-shadow layout. It lives in the pack alongside the table addresses.

### Definition-format extension

To carry RAM-shadow addresses, the existing `[[table]]` entries grow an optional `ram_shadow_address` field:

```toml
[[table]]
id              = "primary_open_loop_fuel"
address         = 0x023A40
ram_shadow_address = 0xFFFF8200  # internal RAM; only present when known
…
```

When absent, the table is not live-tuneable. The CLI surfaces this as a property of the pack: `pack-info` lists which tables support live tuning.

### Provenance

RAM-shadow addresses are per-firmware data. Same provenance rules as the calibration tables themselves — `docs/15-clean-room-engineering.md` for the wall, `docs/17-data-distribution-policy.md` for distribution. They are discovered by:

1. **Owner-side ROM analysis** — the user dumps their ROM via `rom-pull`, RAM-shadow tables are decoded by following the bootloader's RAM-redirect setup code. Documented as a workflow under `tools/defgen/`.
2. **Hardware capture** — observe which RAM addresses an existing live-tuning tool writes to, when in side-by-side mode against a logger.
3. **Forum-sourced RR XMLs** — RomRaider's logger XML carries live-tune metadata for some CIDs. Fact extraction via `tools/defgen/loggergen.py` (extend to consume the relevant tags).

Same red-line rules from `CLAUDE.md`: do not decompile commercial tools to obtain these. RomRaider's GPL source is the canonical fact source.

## Beyond calibration cells: feature toggles + scalar parameters

The same write primitives work for two adjacent surfaces:

- **Custom-feature enable flags** (`docs/16` §"Live-toggleable features"). A feature declares an `enable_ram_address`; toggling Launch Control on or off is a 1-byte UDS write to that address. The patch's main loop reads the flag every iteration; flag false = feature inert. This is the underlying mechanism for the COBB-AccessPort-style "toggle from the hardware screen" UX described in `docs/18` §12.
- **Scalar feature parameters.** A feature can also declare `scalar_param_ram_address` entries for runtime-tunable knobs (Launch Control target RPM, anti-lag overrun threshold). These are 2-byte or 4-byte writes to the same kind of RAM address.

The `st::live_tune::Session` API is the same: `write_cell(table_id, row, col, value)` for calibration cells; an additional `write_feature_param(feature_id, param_name, value)` (or equivalent) for feature-side writes. The plan-time linter runs on both — engine-safety verdicts apply to a "launch control RPM = 8500" write the same way they apply to a calibration-cell write.

The handheld surface for this (`docs/18` §12) is the user-facing payoff. Live tuning on a desktop GUI works during dyno tuning; live toggles on a standalone hardware screen work in the driver's seat.

## Safety regime

Live tuning has a *different* safety problem from flashing:

| Risk class | Flash | Live tuning |
|---|---|---|
| Brick the ECU permanently | Mid-flash power loss → unrecoverable. `st::flash` brick protection (`docs/05` §4) addresses this. | Cannot happen — RAM writes do not modify flash. |
| Blow the engine | Bad calibration written → flash → boot → engine event. Plan-time linter (`evaluate_plan_policy`) catches it. | Bad value written → engine event within 10 ms. Linter must run on every single write, not just at flash time. |
| Lose the work | Crash mid-edit → in-progress edits lost. Project autosave handles it. | Crash mid-edit → ECU is still running with the last-written RAM values. Need a "panic restore" path. |

The plan-time policy linter (`st::policy::evaluate_plan_policy`) is reused, run per-cell-write rather than once at flash time. Engine-safety verdicts stay blocking in every jurisdiction profile, same posture as flash. A `--no-safety-gate` flag does NOT exist; this is non-negotiable for the same reason brick protection is.

**Panic restore.** When `st::live_tune::Session::close` runs (or the host process exits / crashes), the session reverts the ECU's RAM-shadow tables to the values that were in flash. The ECU is left running on the unmodified calibration. The user explicitly opts into persistence via a subsequent flash.

**Battery + connection preflight.** Same as flash preflight (`docs/04` Phase 3 / `docs/05` §4 roadmap): refuse to start a live session if battery voltage is below a known-good threshold or the adapter handshake is degraded. Live writes that get NACK'd from poor signal integrity are how dyno operators end up with "the tool says I wrote 14 deg but the ECU is running 18 deg" — the prevention is a clean transport, not retry logic.

**No silent rewrites.** Every live write is recorded in a session journal (`live_session_<timestamp>.toml`) with `(timestamp, table_id, row, col, old_value, new_value, ack)`. The user can replay the session as a sequence of edits into a project — they get a normal `st::edit::History` after the fact, fully undoable.

## What we will not ship

- Live-tuning support without engine-safety linting. Same red line as flash.
- "Auto live tune" — runs auto-tune kernels and writes directly to the ECU without user review. The closest we'll go is *suggested writes* surfaced in the live-tune UI; the user must click each one to commit. Auto-tune-then-flash already covers the no-human-in-loop case for people who want that.
- ELM327-class adapter support. Live tuning requires consistent sub-50 ms round-trip latency; ELM's overhead does not hit that window reliably. J2534 / OBDX / native adapters only.
- Live-tuning a customer's car over the internet. We're not building a remote ECU tunnel; the safety story doesn't survive even a 200 ms WAN hop, much less a flaky one.
- Cross-vendor RAM-shadow exploits. We only target documented (Subaru's own or community-RE'd) RAM-shadow mechanisms. We do not invent novel ways to put the ECU into a state it wasn't designed to be in.

## Clean-room boundaries

Standard rules from `CLAUDE.md` + `docs/15` apply. Specific to live tuning:

- **Protocol facts (`WriteDataByIdentifier` framing, NRC responses, RAM-shadow enable command bytes) are facts** — ISO 14229 covers UDS, Subaru's SSM3 extensions are documented in public reverse-engineering threads. Both are valid analyst-side inputs.
- **The "live tuning mode" enable command for a given Subaru ECU class is reverse-engineered fact data.** RomRaider has it in their public GPL source as protocol bytes; we extract it as a fact, write our own client.
- **Do not decompile Atlas or any commercial tool to obtain the RAM-shadow address tables.** This is the explicit red flag from `CLAUDE.md`: "name SubuwuTuner types after Atlas's / RomRaider's / OEM internal identifiers" is also off-limits; pick our own.
- **Do not paraphrase the live-tuning user flow from any closed-source competitor's UI.** Design from first principles: what does a dyno operator need on screen? Build that.

The analyst-side workflow in `docs/analyst-mode-prompt.md` is the right way to bring protocol-fact data from a protected reference into a SubuwuTuner spec, if needed.

## Roadmap placement

Live tuning is **v1.5 or v2.0**. Specifically gated by:

- **Phase 4 hardware validation done.** We need 100+ successful flash cycles on the bench rig (`docs/04`) before we're allowed to start writing to running ECUs over UDS. The brick-risk landscape is different but the "do not blow up customer hardware" bar is the same.
- **Adapter latency characterized.** We need to know which adapters can hit sub-50 ms round-trip reliably. Phase 3 datalogging benchmarking (`docs/04`) is a prerequisite.
- **Plan-linter generalization.** `evaluate_plan_policy` currently runs on a `FlashPlan`. Live tuning needs to run the same logic on a single proposed cell write. The seam is there; the wiring is not.

There is no rush. Live tuning is a *dyno-tuner accelerator*; the engineering investment is only justified when there are real users on real hardware in real dyno bays. That is a v1.x late-stage population, not v1.0.

If a contributor wants to start work earlier than the schedule above, the natural staging is:
1. `st::live_tune::Session` skeleton with mock-transport implementation (zero hardware dependency, all paths testable).
2. `ram_shadow_address` definition-format extension.
3. Real-hardware connection lands when Phase 3/4 do.

That sequencing puts the design and the safety-linter integration in place before any byte gets written to a real ECU.

## References

- `docs/04-roadmap.md` — phase gates this depends on
- `docs/05-improvements.md` — under-served-coverage thesis live tuning fits into
- `docs/06-legal-ethics.md` — jurisdiction policy + safety posture
- `docs/12-auto-tuning.md` — sibling subsystem; live tuning is complementary, not a replacement
- `docs/13-transport.md` — adapter latency requirements
- `docs/15-clean-room-engineering.md` — fact-extraction wall
- `docs/17-data-distribution-policy.md` — RAM-shadow address provenance rules
