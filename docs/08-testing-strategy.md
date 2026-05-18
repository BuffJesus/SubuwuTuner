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

### Tier 3 — Fuzzing (nightly + on parser-touching PRs)

- libFuzzer harnesses on:
  - ROM loader (random binaries, malformed)
  - `.stune` project loader (zip with junk inside)
  - TOML definition loader (malformed)
  - Datalog CSV replay
- Corpus is checked in; new findings auto-PR'd by the fuzz job

### Tier 4 — Hardware-in-the-loop (HIL) (nightly + before release)

- Two junkyard ECUs (one VA, one VB) wired to a bench rig with a J2534 device
- Automated suite:
  - Full read → write → verify cycle
  - **Brick-recovery validation**: pull power mid-flash, verify recovery shim restores
  - Datalogging soak test (1 hour @ 100 Hz, drift < 0.1%)
- HIL job is **manual-trigger only** to start; promoted to nightly once it stops finding things

### Tier 5 — Beta program (post-Phase 4)

- Closed beta with vetted tuners, each running a documented test plan on their own car
- Two consecutive bug-free beta cycles required to graduate to 1.0

## What we deliberately do not test in CI

- Real-car flashing in a hosted runner (no car, no consent, no point)
- GUI screenshot diffs (flaky, slows the loop more than it catches)
- Performance microbenchmarks as pass/fail gates (track them, don't block on noise)

## Coverage tooling

- `gcovr` on Linux GCC builds — uploaded to Codecov
- Coverage is **tracked**, not gated, except for the four core modules above

## Mutation testing

- `mull` on the four core modules, weekly job
- A surviving mutant on `st::flash` blocks the release of any version that touches `st::flash`. Other modules: file a ticket.

## Failure injection

The transport layer has a `FaultInjector` middleware (compile-flagged out of release builds) that can:

- Drop frames, duplicate frames, delay frames, corrupt frames
- Yank a "USB unplug" mid-session
- Simulate ECU NACK / busy / wrong-state

Used by Tier 2 and Tier 4 tests to ensure the flasher state machine is resilient.
