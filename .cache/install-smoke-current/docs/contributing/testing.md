# Testing

SubuwuTuner uses four tiers of testing. Each tier has a different
budget and a different blast radius.

## Tier 1 — Unit tests (Catch2)

Co-located with the module they test (`tests/unit/<module>/`). Run in
CI on every push, on every preset. **~1700 cases / ~220k assertions** as
of 2026-06-19.

```bash
ctest --preset win-mingw
./build/win-mingw/bin/st_unit_tests "[edit]"
./build/win-mingw/bin/st_unit_tests "[flash]"
```

Tags in use: `[edit]`, `[history]`, `[flash]`, `[uds]`, `[ssm]`,
`[transport]`, `[autotune]`, `[feature]`, `[codegen-sh2a]`,
`[codegen-rh850]`, `[ap3]`, `[ets]`, `[tune_export]`, `[ai]`, and many
more.

Hidden / live tags require explicit opt-in:

- `[.live][ap3]` — opt-in integration test against a connected AP, gated
  by `STT_AP3_LIVE_TEST=1`
- `[.bench]` — opt-in bench-rig tests, gated by their own env var

## Tier 2 — Python tests (`tools/defgen/`)

The defgen XML → TOML converter has its own pytest suite:

```bash
cd tools/defgen
python -m pytest
```

Runs in CI on the same trigger as the C++ tests.

## Tier 3 — Mutation tests (st::flash)

The orchestrator is treated as safety-critical. Mutation testing runs
locally before any `st::flash` change ships; release gates require a
green mutation suite. See
[`docs/08-testing-strategy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/08-testing-strategy.md){ target="_blank" } §3.

## Tier 4 — Hardware-in-the-loop (HIL)

Against real junkyard ECUs on the bench rig. The bench rig
build + runbook live at:

- [`docs/28-bench-rig-build.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/28-bench-rig-build.md){ target="_blank" }
- [`docs/42-bench-rig-validation-runbook.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/42-bench-rig-validation-runbook.md){ target="_blank" }

HIL tests are the gate for v1.0 release. Per-ISA brick-protection
recipes are validated against the rig:

- [`docs/31-brick-protection-by-isa.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/31-brick-protection-by-isa.md){ target="_blank" }
  — five tests for SH-2A and RH850 each.
- [`docs/40-delta-flash-brick-protection.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/40-delta-flash-brick-protection.md){ target="_blank" }
  — four additional tests for the v1.5 differential-flash extension.

## Authoring new tests

- Test sits next to the module: `tests/unit/<module>/<topic>_test.cpp`.
- Use Catch2 v3 `TEST_CASE("name", "[tag]")`.
- Prefer fixtures from `fixtures/` over generating data in the test —
  reproducibility wins.
- For UDS / SSM tests, use `MockTransport`. Scripted request/response
  pairs make the wire shape an asserted contract.
- For ROM tests, the synthetic `fixtures/demo-pack/demo-rom.bin` is
  fine for shape-correctness. For protocol-specific behavior (e.g.,
  boot integrity), use the analyst-staged fixtures.

## Coverage philosophy

Coverage is not a release gate — branch coverage above ~70% across
domain modules is the watermark. Some modules are intentionally
under-tested (UI binding glue, transport-specific quirks reachable only
on real hardware); some are heavily over-tested (`st::flash`,
`st::edit`, `st::tune_export`).

## Deeper detail

- [`docs/08-testing-strategy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/08-testing-strategy.md){ target="_blank" }
  — full strategy with per-tier acceptance criteria.
