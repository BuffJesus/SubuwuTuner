# `tests/private/` — local-only integration tests

This directory is **gitignored except for this README and
`example_private_test.cpp.template`**. `*.cpp` files dropped
here get picked up automatically by the build's CMake glob,
but never make it into commits or CI.

## When to put a test here

A test belongs in `tests/private/` (not `tests/unit/`) when
it depends on a file that can't ship with the public repo —
the obvious case is a real ROM dump in `fixtures/private/`.
Examples that fit here:

- "Loading my staged stock VA WRX ROM produces the right CRC
  + the right CID match against the demo-pack identification
  block."
- "`dump-table --def <my private pack> --table <id>` against
  a known stock dump renders sane values."
- "`rom-diff stock.bin mytune.bin` flags exactly the cells
  the tuning thread says were changed."
- "End-to-end: `feature-compile` a sample `.stmod` then
  inspect the patch bytes against a known-good golden image."

Tests that DON'T need a private fixture stay in `tests/unit/`
(committed, run on every CI build).

## How to add one

1. Copy `example_private_test.cpp.template` to a new
   `*.cpp` file in this directory. The new file is
   automatically gitignored.
2. Edit the test body — load a ROM from
   `fixtures/private/`, exercise some code path, assert.
3. Rebuild — CMake's CONFIGURE_DEPENDS glob in
   `tests/CMakeLists.txt` picks up the new file on the next
   `cmake --build`. Catch2 discovers the new `TEST_CASE`s
   on the next test run.

No CMakeLists edits required.

## How NOT to use it

- **Don't commit the cpp files** — they're already
  gitignored. If you find yourself wanting to push a
  test from here, that's the signal that the test should
  move to `tests/unit/` with a synthetic fixture instead
  of a private ROM dump.
- **Don't depend on these tests in CI.** The public CI
  doesn't have your fixtures and will skip the directory
  entirely; that's by design.
- **Don't hard-code paths to anywhere outside
  `fixtures/private/`** — tests that point at
  `D:\Subuwu\defs-private\` or similar
  user-specific paths break for the next person on the
  same machine.

## Running

The private tests build into the same `st_unit_tests`
binary as the unit tests. Filter to just the private ones
via the Catch2 tag convention used in the template:

```
./build/win-mingw/bin/st_unit_tests.exe [private]
```

If `fixtures/private/` is empty, the test bodies REQUIRE
the files to exist and will fail cleanly with a "file not
found" message — that's the prompt to drop something in
there.

## Convention rationale

This mirrors the `fixtures/private/` README's stance:
infrastructure ships, data doesn't. Tests that need real
data ride with the data.
