# `tests/` — SubuwuTuner test organization

Catch2 v3 test tree. One binary (`st_unit_tests`) discovers every
`TEST_CASE` across `unit/` + `hil/` + `private/`. Filter by tag at
runtime; see [Tag conventions](#tag-conventions) below.

## Directory layout

```
tests/
├── _helpers/         Shared scaffolding (bench-power mocks,
│                     brick-recovery fixtures, loopback channels).
│                     Not test files — include from real tests.
├── unit/<module>/    Per-module unit tests. Public, run on every CI
│                     build. Mirrors src/ layout.
├── hil/              Hardware-in-the-loop tests. Public test code,
│                     but most cases self-skip when STT_HIL_* env
│                     vars aren't set. See `hil/README.md`.
├── private/          Gitignored except for README + .template.
│                     For tests that depend on user-private fixtures
│                     (real ROM dumps, AP captures with PII). See
│                     `private/README.md`.
└── fuzz/             libFuzzer harnesses. Gated behind
                      ST_BUILD_FUZZ_HARNESSES=ON. See `fuzz/README.md`.
```

## Tag conventions

Catch2 tags drive both organization (filter all `[transport]` cases)
and gating (hide `[.private]` from default runs). Conventions:

| Tag | Meaning | Default visibility |
|---|---|---|
| `[module]` | Module the test exercises (e.g. `[transport]`, `[ecu]`, `[devices][ets]`). Multiple per case. | Visible |
| `[ap3]` | AP-protocol-surface tests (codec, cipher, file vault). Note: the namespace was renamed to `ets` 2026-06-12, but the tag and CLI subcommand kept the `ap3` name for continuity. | Visible |
| `[cipher]` | `.ptm` cipher tests (XTEA / base64 / AES / bzip2). | Visible (build-flag-gated effectiveness) |
| `[fixtures]` | Tests pinned against canonical byte-level fixtures. Self-skip when fixtures are absent. | Visible |
| `[ptm_cli]` / `[ptm]` | `subuwutuner-cli ptm {…}` family. | Visible |
| `[.private]` | **Hidden by default.** Loads files from `fixtures/private/` (real ROMs, un-scrubbed AP captures). Self-skips when the fixture is absent. Invoke via `st_unit_tests "[.private]"`. | Hidden |
| `[.live]` | **Hidden by default.** Talks to live hardware (AP plugged in, OBDX adapter on COM5, etc.). Self-skips unless `STT_AP3_LIVE_TEST=1` (or similar per-suite env var) is set. Invoke via `st_unit_tests "[.live][ap3]"` or specific tag mix. | Hidden |
| `[!hide]` | Catch2 built-in: hide unconditionally. Reserved for staged WIP tests; if you see one in master, ask why. | Hidden |
| `[!shouldfail]` | Catch2 built-in: invert pass/fail (expects assertion failure). Reserved for fixture-pin disagreements that are openly tracked. Currently zero in tree. | Visible (counts as pass when it does fail) |

Both leading-dot (`.private`, `.live`) tags use Catch2's documented
hidden-by-default convention. The dot signals "intentionally not
part of the default suite," not "broken."

## Running

```bash
# Default build, all visible tests:
./build/win-mingw/bin/st_unit_tests.exe

# Filter by tag (Catch2 tag-expression syntax):
./build/win-mingw/bin/st_unit_tests.exe "[transport]"
./build/win-mingw/bin/st_unit_tests.exe "[devices][ets]~[cipher]"  # ets but not cipher

# Filter by test-name glob:
./build/win-mingw/bin/st_unit_tests.exe -#'ptm import*'

# Run hidden suites (each gates differently):
./build/win-mingw/bin/st_unit_tests.exe "[.private]"                # needs fixtures/private/ files
STT_AP3_LIVE_TEST=1 \
  ./build/win-mingw/bin/st_unit_tests.exe "[.live][ap3]"            # needs an AP plugged in
```

## CTest integration

`ctest --preset <preset>` runs the same binary via `catch_discover_tests`.
One quirk worth knowing: on Windows + MinGW, the discovery step captures
`--list-tests` stdout in the active code page (cp1252), so `TEST_CASE`
strings containing non-ASCII characters (`§`, `→`, `—`, `²`) get
mojibake'd and ctest registers names that don't match what the test
binary actually emits at runtime. **Convention: keep `TEST_CASE` names
ASCII** — use `->`, `--`, `sec`, `^2` instead. Tracked in the
2026-06-12 PM "test infra cleanup" session.

## Adding a test

1. Pick the right directory:
   - **Public, no special fixture** → `tests/unit/<module>/test_<topic>.cpp`
   - **Needs a private ROM/AP capture** → `tests/private/` (gitignored)
   - **Live hardware required** → tag with `[.live]` and gate on an env var
   - **Adversarial-input coverage** → `tests/fuzz/` harness
2. Use Catch2 v3 idiom (`TEST_CASE`, `SECTION`, `REQUIRE`, `CHECK`, `SKIP`).
3. Tags: module + any specialty (e.g. `[cipher]`, `[fixtures]`, `[ap3]`).
4. CMake glob picks up new files automatically — no manual list edits.
5. Run the new case explicitly to confirm before pushing:
   `st_unit_tests "Your test case name"`.

## What's NOT in here

- **GUI render tests.** ImGui code is mocked-out at the function-pointer
  level by the headless test binary, but full panel-render coverage
  (with mock viewport + assertion-on-draw-calls) is deferred. UI changes
  validated by humans + the live-recipe smoke test family.
- **End-to-end ECU integration.** Phase 4's bench-rig + Phase 5.5 HIL
  cycle live alongside the hardware itself; see
  `docs/08-testing-strategy.md` Tier 4 and `docs/28-bench-rig-build.md`.

## Cross-references

- `docs/08-testing-strategy.md` — overall testing tiers + acceptance gates.
- `tests/_helpers/` — shared mocks and fixtures.
- `tests/hil/README.md` — HIL test gating.
- `tests/private/README.md` — private-fixture test convention.
- `tests/fuzz/README.md` — libFuzzer harness build + run.
