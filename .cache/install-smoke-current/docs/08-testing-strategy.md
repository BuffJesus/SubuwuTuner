# 08 — Testing Strategy

Tuning software bricks hardware when it's wrong. Test budget is correspondingly large.

## Test tiers

### Tier 1 — Unit (every PR, <60 s total)

- `Catch2` test cases per module
- Pure functions only — no I/O, no threads
- ROM parsing, scaling math, CRC, axis interpolation, undo/redo, definition validation
- **Target:** ≥ 85% branch coverage on `st::core`, `st::rom`, `st::defs`, `st::project`

### Tier 2 — Integration (every PR, <5 min)

- Disk-backed `.stune` projects round-tripped
- Fixture ROM dumps loaded, edited, saved, diffed
- Transport with a **fake J2534** that replays canned ECU traces from real cars
- SSM and UDS client tested against scripted protocol scenarios
- **Cancellation invariant tests** (see below) — must pass on every PR that touches `st::flash`, `st::ecu::uds`, or `st::ecu::ssm`

### Tier 2a — Cancellation invariants (subset of Tier 2)

These are the testable safety properties around `std::stop_token` cancellation. A regression here can brick a real ECU; the suite enforces them via `MockTransport` + `FaultInjector` without hardware.

| Invariant | Test asserts |
|---|---|
| Mid-PDU UDS cancel is deferred | Drive `stop_token` to "stopped" while `TransferData` is on the wire; assert the orchestrator completes the in-flight PDU, then issues `RequestTransferExit` (0x37) before returning a cancelled `Result`. No bytes dropped, no half-PDU on the wire. |
| Mid-PDU SSM cancel is deferred | Drive `stop_token` to "stopped" mid-block (SSM B8 256-byte block write); assert the block completes, then the session exits. |
| Session-exit on cancel | After any cancelled flash, assert the final transmitted PDU is `DiagnosticSessionControl` → `defaultSession` (0x10 0x01). |
| Crash-mid-flash recovery | Open a `.stune`, start a flash, kill the process between PDU N and N+1, reopen; assert the project's journal flags the ECU as in-programming-session and `Project::open()` offers `plan_resume()` or clean session exit. |
| Resume idempotence | Run `plan_resume()` twice in a row on the same manifest; assert second call is a no-op (no double-write of completed sectors). |

The tests live at `tests/unit/flash/test_cancellation_invariants.cpp` and `tests/unit/ecu/test_uds_pdu_atomicity.cpp` — both shipped per Phase 4 (`docs/04`).

### Tier 3 — Fuzzing (planned; not yet implemented)

Planned libFuzzer harnesses (`tests/fuzz/`, not yet shipped — see `docs/03` §"Earlier plan listed … libFuzzer … None of them shipped"):

- ROM loader (random binaries, malformed)
- `.stune` project loader (directory walker fed pathological inputs — junk files, broken TOML, hostile symlinks)
- TOML definition loader (malformed)
- Datalog CSV replay

Corpus will be checked in alongside; new findings auto-PR'd by the fuzz job once Tier 3 lands.

### Tier 4 — Hardware-in-the-loop (HIL) (nightly + before release)

- Two junkyard ECUs (one VA, one VB) wired to a bench rig with a J2534 device
- **Authoritative wiring reference**: 2017 WRX (FA20DIT) FSM extract — ECM I/O signal table (FSM ~p.2171), power-supply circuit (FSM p.6723–6736), ground circuit (FSM p.6751–6760), CAN bus diagram (FSM p.6805–6807), full ECM schematic (FSM p.6846–6877), immobilizer diagram (FSM p.6936–6939). Bench-priority sequence: verify the harness against the ECM I/O signal table terminal-by-terminal **before applying power**. The extract lives at `D:\Subuwu\findings\2017_WRX_FA_ECU_Bench_Reference.pdf`. **Step-by-step assembly runbook**: `docs/28-bench-rig-build.md`.
- Automated suite:
  - Full read → write → verify cycle
  - **Brick-recovery validation**: pull power mid-flash, verify recovery shim restores
  - Datalogging soak test (1 hour @ 100 Hz, drift < 0.1%)
- **`.ptm` cycle through APManager (Phase 5.5 of `docs/28`)**: pull a tune from the AP, `ptm import`/`export` through SubuwuTuner, push back, flash to the bench ECU via APManager (not `st::flash` yet — that's the Phase-6 gate), confirm CID + checksum + clean boot. Validates the `.ptm` format end-to-end without putting the SubuwuTuner flash orchestrator in the loop. Live-validated through the AP storage layer 2026-06-12; the ECU-flash leg waits on the bench rig coming up.
- HIL job is **manual-trigger only** to start; promoted to nightly once it stops finding things

### Tier 5 — Beta program (post-Phase 4)

- Closed beta with vetted tuners, each running a documented test plan on their own car
- Two consecutive bug-free beta cycles required to graduate to 1.0

## What we deliberately do not test in CI

- Real-car flashing in a hosted runner (no car, no consent, no point)
- GUI screenshot diffs (flaky, slows the loop more than it catches)
- Performance microbenchmarks as pass/fail gates (track them, don't block on noise)

## Local visual smoke loop

Windows developers can run `tools/gui-visual-smoke.ps1` after a GUI build. It
opens the real demo project, visits the Tune, Log, and Feature workspaces,
loads the bundled synthetic log, saves screenshots under `.cache/ui-smoke/`,
checks that the process remains responsive, and retains stdout/stderr. This is
an inspection aid, not a pixel-diff CI gate: review the images and logs for
layout, empty-state, rendering, and interaction regressions, fix concrete
findings, and rerun the loop. Pass `-Scenario Welcome` to exercise the
no-project entry path instead.

## Coverage tooling

- `gcovr` on Linux GCC builds — uploaded to Codecov
- Coverage is **tracked**, not gated, except for the four core modules above

## Mutation testing

Two tiers, light → heavy:

**Light tier: `tools/mutation_test.py` (shipped, local + ad-hoc CI).**
A small Python driver that applies a focused set of single-operator
mutations (`==↔!=`, `<↔<=`, `>↔>=`, `true↔false`) to a specified
line range in one file, rebuilds the affected target, and re-runs a
Catch2-tag-filtered test subset for each mutant. Reports KILLED /
SURVIVED / BUILD_FAIL / TIMEOUT and an overall mutation score.

```bash
py tools/mutation_test.py \
    --file src/flash/src/flash.cpp \
    --line-start 515 --line-end 544 \
    --target st_unit_tests \
    --tag-filter "[flash]"
```

A SURVIVED mutant is a coverage gap — the test suite didn't catch a
behaviorally-different version of the code. Hardening the suite to
kill survivors is the long-term work this tool scaffolds. Default
exit code is non-zero when survivors exist; CI uses it as an
informational signal, not a hard fail (until the suite is hardened
to 0 survivors on `st::flash`).

Limitations of the shipped driver (deliberate, to keep it small):
- Regex-based, not AST-based — false BUILD_FAILs on template `<...>`
  syntax overlapping with comparison operators
- One file, one line range, one rebuild per mutant — runtime scales
  with mutant count
- Mutation set is the four operator swaps above; not a comprehensive
  catalog

**Heavy tier: `mull` (planned, blocked on LLVM toolchain integration).**
Real AST-based mutation testing via the LLVM `mull` plugin. Catches
more mutation types (statement deletion, return-value swap, etc.),
runs faster via shared compilation, integrates with `lit` for
parallel execution. Weekly CI job. A surviving mutant on `st::flash`
blocks the release of any version that touches `st::flash`. Other
modules: file a ticket.

Migration path: as `mull` integration lands, the Python driver stays
useful for local spot-checks (no LLVM dependency) and for files where
the regex-vs-AST distinction is fine.

## Failure injection

The transport layer has a `FaultInjector` middleware (compile-flagged out of release builds) that can:

- Drop frames, duplicate frames, delay frames, corrupt frames
- Yank a "USB unplug" mid-session
- Simulate ECU NACK / busy / wrong-state

Used by Tier 2 and Tier 4 tests to ensure the flasher state machine is resilient.
