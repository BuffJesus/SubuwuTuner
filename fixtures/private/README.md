# `fixtures/private/` — local-only ROM dumps + provenance

This directory is **gitignored except for this README and
`PROVENANCE.template.toml`**. Anything else you drop in stays
on your machine — never pushed, never bundled with the public
release.

## What goes here

Real-world artifacts that are useful for development but can't
be redistributed:

- **Stock ROM dumps** — unmodified factory calibrations,
  pulled from a real ECU (or sourced from community archives).
  Naming convention: `<CID>_<source>_stock.bin`
  (e.g. `AS80U_aw0123_stock.bin`).
- **Tuned ROM dumps** — same files after a user has edited them.
  Useful for `rom-diff` testing. Naming: `<CID>_<note>_tuned.bin`.
- **Captured UDS / SSM traces** — recorded bus traffic. Naming:
  `<scenario>_<date>.uds` (e.g. `va_wrx_read_engine_2026-05-15.uds`).
- **Vendor-DLL captures** — J2534 debug logs (per the OBDX log
  toggle documented in `docs/13-transport.md`), pcap-style
  recordings. Naming: `<adapter>_<scenario>_<date>.log`.

## What does NOT go here

- Synthetic / hand-crafted test fixtures — those go in
  `fixtures/demo-pack/` or `fixtures/samples/` (both committed,
  both public, both deliberately small + non-copyrighted).
- Definition packs derived from ROMs — those live off-tree at
  `D:\Documents\SubuwuTuner-defs-private\` per
  `docs/17-data-distribution-policy.md` (Path B).
- Documentation about specific ROMs — the **fact** that a
  certain CID exists and what table addresses it carries is
  itself fine to document in `docs/`. The ROM bytes are what
  must stay private.

## Provenance discipline

Copy `PROVENANCE.template.toml` to `PROVENANCE.toml` (also
gitignored) and add an entry for every ROM you drop here. The
clean-room methodology in `docs/15-clean-room-engineering.md`
relies on every numeric fact being traceable to two
independent observations; without provenance metadata, the
ROMs are just bytes — with it, they're citable references.

Minimum per-ROM info worth recording:

- `file` — filename relative to this directory
- `sha256` — verify with the source (catches silent mutation)
- `size_bytes` — sanity-check (1.5 MiB = 1572864 for most VA;
  VB varies)
- `cid` — 8-char internal id at byte offset `0x00002000`
  (Subaru convention; same field RomRaider / ECUFlash use)
- `ecuid` — 5-byte hex returned by SSM init handshake; pulled
  from the dump if you have a CID-to-ecuid table, otherwise
  optional
- `source` — where you got the file. URL + date for community
  dumps; "bench capture <date>" for your own ECU; etc.
- `source_kind` — `community_dump` / `bench_capture` / `other`
- `notes` — anything you want future-you to remember

## How tests use these

Private integration tests live under `tests/private/` (same
gitignore convention). The build's CMake glob picks them up
automatically when present, so dropping a `*.cpp` in there
just works — no CMakeLists edits.

A typical test loads a known-CID stock ROM from this
directory and exercises one part of the codebase against it:

```cpp
// tests/private/test_va_wrx_rom_smoke.cpp (NOT committed)
auto const rom = st::Rom::from_file("fixtures/private/AS80U_aw0123_stock.bin");
REQUIRE(rom.has_value());
REQUIRE(rom->size() == 1572864);
// ... exercise dump-table / pack-info / rom-info etc.
```

See `tests/private/README.md` and the example template there.

## CRC sanity check

The CLI prints CRC32 of any ROM:

```
subuwutuner-cli rom-info fixtures/private/AS80U_aw0123_stock.bin
```

If a community source publishes CRC32 alongside the dump, run
this and compare. Mismatch → don't use the file.

## Legal posture

Subaru ECU calibration data is Subaru's copyright. Per
`docs/06-legal-ethics.md` and `docs/17-data-distribution-policy.md`,
this project's stance ("Path B") is that the public repo
ships infrastructure only and users supply their own data.
Personal-use staging of ROMs in this gitignored directory for
the purpose of developing the tool is the activity this
folder is for; redistribution is what stays out of scope.
