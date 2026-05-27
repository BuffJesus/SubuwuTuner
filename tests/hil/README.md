# tests/hil — Hardware-in-the-loop tests

This directory holds tests that exercise SubuwuTuner against **real
hardware** — the OBDX Pro VX adapter, a junkyard 2017 WRX ECU on a
bench harness, and a USB-controlled relay board for automated
brick-recovery cycling. See `docs/08-testing-strategy.md` §Tier 4
and `docs/28-bench-rig-build.md` for the runbook.

## How they're gated

These tests do NOT run in the standard CI matrix. They compile only
when CMake is configured with `-DST_BUILD_HIL_TESTS=ON`. The default
(OFF) keeps the regular `st_unit_tests` binary fast and hardware-free
on every developer machine + every CI runner.

```
cmake -B build/hil -S . -DST_BUILD_HIL_TESTS=ON
cmake --build build/hil --target st_unit_tests
build/hil/bin/st_unit_tests.exe "[hil]"
```

The `[hil]` Catch2 tag selects only this directory's tests. Each
individual test SKIPs gracefully if it can't find the hardware it
needs (OBDX adapter, relay board, bench ECU), so a partially-wired
bench can still run the subset of tests that don't depend on the
missing piece.

## What goes here

| Topic | When it lands |
|---|---|
| **Brick-recovery loop (Phase 4 ship gate)** — N consecutive `flash → pull → resume → verify` cycles against the bench ECU | Once the bench rig + relay are wired (per `docs/28` + the bench-rig handoff) |
| **OBDX adapter contract tests** — open/probe/send/recv against COM5, no ECU required | Anytime — only needs the adapter |
| **Bench ECU comms** — `rom-info`, `rom-pull`, `rom-diff` against the bench | Once the bench ECU is responding |
| **Bench ECU full flash** — write a known-modified cal, verify it stuck | Once brick-recovery has demonstrated 100 cycles clean |

## Why not just put these in `tests/unit/`?

Three reasons:

1. **Speed.** Unit tests run in ~5 seconds. HIL tests can take minutes
   (a full ROM read is ~7 minutes; a 100-cycle brick-recovery batch is
   over an hour). Mixing them would push the inner-loop test time past
   the patience threshold.

2. **Determinism.** Unit tests are deterministic by construction. HIL
   tests depend on a physical bench, a specific adapter on a specific
   COM port, and an ECU in a known state. Failures here often
   represent rig drift, not code bugs.

3. **CI cleanliness.** Every CI runner that builds the codebase would
   otherwise have to either skip-by-tag or fail-because-no-hardware on
   every HIL test. The CMake gate removes the test from the binary
   entirely on those runners.

## What lives where

```
tests/
├── _helpers/
│   ├── bench_power.hpp          # IBenchPower interface
│   ├── mock_bench_power.{hpp,cpp}   # MockBenchPower (no hardware)
│   └── brick_recovery_fixture.{hpp,cpp}   # cycle orchestrator
├── unit/flash/
│   └── test_brick_recovery_fixture.cpp   # always runs; validates the
│                                          # fixture using MockBenchPower
│                                          # + MockTransport
└── hil/                          # YOU ARE HERE
    ├── README.md                 # this file
    └── test_bench_smoke.cpp      # gated; SKIPs unless real hardware present
```

When the first real concrete `IBenchPower` lands (e.g.
`NumatoBenchPower` driving a Numato Labs 4-channel USB relay board),
it goes under `tests/hil/` as `numato_bench_power.{hpp,cpp}` and is
referenced from the HIL tests that need automated power cycling.
