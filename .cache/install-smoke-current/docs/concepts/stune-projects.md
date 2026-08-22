# `.stune` projects

A `.stune` project is a **directory** (not a zip, not a database) that
holds everything in flight for a single tune: the source ROM, the
working ROM, the edit history, optional datalogs, and project metadata.

## On-disk layout

```
my-tune.stune/
├── project.toml      # project metadata, def-pack reference, ROM list
├── source.bin        # original ROM (never modified)
├── working.bin       # source + applied edits (re-derived on open)
├── edits.toml        # structured edit history (canonical source)
├── histories/        # per-ROM history (multi-ROM projects)
│   └── <rom_id>.toml
├── datalogs/         # optional, drop CSVs here for the log panel
├── snapshots/        # named snapshots for branching (optional)
└── audit.log         # CRC32-protected append-only audit (optional)
```

**Why a directory, not a single file?**

- Every file is diffable in git. Reviewing a tune as a PR is the same
  workflow as reviewing code.
- Large blobs (`source.bin`, `working.bin`) and tiny structured files
  (`edits.toml`, `project.toml`) coexist without one bloating the other.
- The audit log can append without rewriting the whole project.

## The replay model

`working.bin` is **always re-derived** from `source.bin` + `edits.toml`
on project open. The on-disk `working.bin` is a cache, not the source
of truth — `edits.toml` is.

This has three consequences worth understanding:

1. **Hand-editing `edits.toml` works.** Delete an edit record, reopen
   the project, the edit is gone.
2. **Source ROMs are never mutated.** A misbehaving plugin or auto-tune
   kernel can't accidentally clobber your reference.
3. **Branching is cheap.** Copy `edits.toml`, snip the divergent edits
   out of one copy, and you have two parallel tunes off the same source.

## Working with projects

### From the CLI

```bash
# Create a new project from a ROM
subuwutuner-cli project-new --source path/to/rom.bin \
                            --def path/to/pack-dir/ \
                            my-tune.stune

# What's in a project?
subuwutuner-cli project-info my-tune.stune

# Validate (replay edits + pack hygiene)
subuwutuner-cli project-validate my-tune.stune

# Clone (branch)
subuwutuner-cli project-clone my-tune.stune my-tune-b.stune
```

### From the GUI

Welcome panel → **New project** → pick a `.bin` + a `pack.toml`. The GUI
creates the directory, copies the ROM in as `source.bin`, materializes
`working.bin`, and stamps an empty `edits.toml`.

## The audit log

`audit.log` is an append-only CRC32-protected log of every
domain-significant action: ROM identification, pack load, every edit,
every UDS request, every Flasher state transition. Used for forensics
("what did this tool actually do to this ROM?") and for tamper detection.

The log lives inside the project directory as `audit.log` and is written
automatically — there is no flag to turn it on. Read it with
`subuwutuner-cli audit show <project-dir>`; `audit verify`, `audit stats`
and `audit append --kind <name> --description "text"` round out the verb
family, and `project-info --audit-summary <dir>` folds a summary into the
project overview.

NDJSON export:

```bash
subuwutuner-cli audit stats path/to/audit.log
subuwutuner-cli audit export --format ndjson --pinned-only \
                             path/to/audit.log > pinned-events.ndjson
```

## Deeper detail

- [`docs/21-stune-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/21-stune-format.md){ target="_blank" }
  — full TOML schema for `project.toml`, `edits.toml`, and the history
  format.
- [`docs/02-architecture.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/02-architecture.md){ target="_blank" }
  §`st::Project` — module-level design.
