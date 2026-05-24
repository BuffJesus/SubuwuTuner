# 21 — `.stune` Project Format

A SubuwuTuner project (`*.stune`) is a **directory**, not a single archive file. This keeps projects Git-friendly — diffable, mergeable, reviewable — without forcing users into an opaque container that defeats line-based version control.

This document pins down the on-disk layout so users know what they're version-controlling and so tooling (CI, fleet management, project migration) has a stable contract.

## Layout

A minimal valid `.stune` directory:

```
mytune.stune/
├── project.toml          # required — project manifest
├── source.bin            # required — original ROM dump (immutable for the life of the project)
└── working.bin           # required — current edited ROM (mutates with every edit)
```

A project with edit history and a flash record adds:

```
mytune.stune/
├── project.toml
├── source.bin
├── working.bin
├── history/              # optional — edit-history journal (one TOML file per ByteEdit)
│   ├── 00000-set-cell.toml
│   ├── 00001-smooth.toml
│   └── …
└── flash/                # optional — flash session records
    ├── 2026-05-17-1330-manifest.toml      # tamper-evident BLAKE3 manifest
    └── 2026-05-17-1330-journal.toml       # crash-safe per-sector journal
```

The `fixtures/demo.stune/` directory in the repo is the canonical synthetic example.

## `project.toml` schema

```toml
[project]
schema_version = 1                          # required; bumps on incompatible changes
display_name   = "My VA WRX street tune"    # required; shown in GUI title bar
created        = "2026-05-12T03:29:40Z"     # required; ISO 8601 UTC
notes          = ""                         # optional; free-form

[project.source_rom]
path  = "source.bin"                        # relative to project directory
crc32 = 467298693                           # integrity check; recomputed on open

[project.working_rom]
path  = "working.bin"
crc32 = 467298693                           # changes as edits land

[project.definition]
path = "../demo-pack"                       # path to a definition pack directory or single TOML
                                            # may be relative (recommended for portability) or absolute

[security_access]                           # optional table — SA-related per-project state
handheld_serial = ""                        # aftermarket vendor programming-handheld serial, if any.
                                            # Used (eventually) by the SA key-derivation plug-in when
                                            # the recovered constants are keyed to a specific handheld.
                                            # Empty / table absent ⇒ stock ECU or unknown handheld.
                                            # See docs/23-security-access.md for the open question
                                            # this field exists to answer.
```

## Design choices

### Why a directory, not a zip

A zip-wrapped `.stune` (the obvious alternative) makes the ROM and history binary-opaque. Git stores it as one blob, two contributors editing the same project can't merge, and `git diff` is useless. A directory keeps each file independently diffable: `project.toml` line-merges, `history/*.toml` is per-edit so contributors don't collide, and the binary ROM lives as a single file that Git LFS (or a plain `.gitignore` exclusion) can handle on its own merits.

### Why `source.bin` is immutable

`source.bin` is the original ROM dump the project was created from. It never changes for the life of the project; all edits go to `working.bin`. This makes diff-from-stock trivial (`subuwutuner-cli rom-diff source.bin working.bin`) and lets the project be reverted to factory at any point.

### Path conventions

- Paths in `project.toml` are **relative to the project directory** wherever possible. This makes projects portable between machines (move the directory, paths still resolve).
- An absolute `definition.path` is allowed but discouraged — it ties the project to one machine.
- The `..` parent traversal is allowed for `definition.path` (so multiple projects can share a single pack at a sibling directory) but the loader refuses any path that escapes outside a configured "project root" tree in fleet / shop scenarios.

### Embedding vs referencing the ROM

The ROM is **referenced by path, not embedded**. Base64-encoding a 1 MB ROM into TOML would balloon the manifest, kill diffability, and prevent Git LFS handling. The trade-off is that moving a project requires moving the whole directory (which is what users do anyway — `cp -r` or zip-the-directory works fine).

### Git posture

A typical user's `.gitattributes` (recommended, not enforced):

```
*.bin filter=lfs diff=lfs merge=lfs -text
```

Or for repos that don't use LFS:

```
*.bin binary
```

Either way, the TOML files line-merge cleanly and the binary ROM is just one file that Git knows how to track.

## Compatibility

- **Schema versions:** the loader refuses any `schema_version` it doesn't understand. v1 is the only version today.
- **Forward compatibility:** unknown top-level tables in `project.toml` are ignored with a warning, so a v2 loader writing back a v1 project doesn't lose v2-only data on round-trip (provided the v2 fields go in their own table, not as keys under `[project]`).
- **Path migration:** `subuwutuner-cli project-info <dir>` reports any unresolved paths so users can fix them without opening the GUI.

## What's *not* in the project

These deliberately don't live in `.stune` because they'd defeat the version-control story:

- **User preferences / theme / window layout** — those live in the OS user-config directory (XDG / `~/Library/Application Support` / `%APPDATA%`), not in the project.
- **Adapter / transport configuration** — same. A `.stune` checked into Git should be portable across machines with different hardware.
- **Logs from the datalogger** — written next to the project on request, but not part of the project itself. Tuners often want to share a tune without sharing their drive history.

## Future extensions (not yet implemented)

- `manifest.toml` — pack-level integrity manifest for shipping a project + the exact pack snapshot it depends on. Useful for shop-handoff workflows.
- `flash/*-manifest.toml.sig` — Ed25519 signatures over flash manifests for verified-build chains. Pending the signed-update channel decision in `docs/03-tech-stack.md`.
- `.stune.zip` archive form for email/forum sharing — pure wrapper, expands to the directory layout on import. No new schema.

## See also

- `docs/02-architecture.md` — `st::project::Project` is the in-memory representation
- `docs/11-definition-format.md` — the pack format that `project.definition.path` points at
- `docs/05-improvements.md` §4b — how `flash/*.toml` powers crash-recovery and cancellation safety
