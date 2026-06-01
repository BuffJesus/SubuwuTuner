# 25 — Config system (Phase 2 installer / runtime)

> Design for a config-file-driven path layer so end-users can choose where
> definitions and ROM dumps live without recompiling. Drives the
> per-directory pickers in the NSIS installer and the post-install
> Settings dialogs in the GUI/CLI.

**Status:** core `st::config` shipped — `Paths { definitions_root,
rom_dump_root }`, layered precedence (CLI flag → env var → config file →
built-in defaults), atomic save, `load()` / `load_from()` / `defaults()` /
`save()`. Phase 1 packaging (single-install-root via `cmake --install` +
CPack ZIP/NSIS) shipped in commits `f9b9885` / `73a2908` / `1911f1f` on
2026-05-24. Still pending: NSIS custom pages for path picking,
`subuwutuner-cli config` subcommand, GUI Settings dialog wiring to
`st::config::Config::save()`, and the `[security_access].handheld_serial`
key — see §11 below.

## 1 — Why this exists

Phase 1 puts everything in one place: the user picks an install root and
`subuwutuner-gui.exe`, `subuwutuner-cli.exe`, `definitions/*.zip` all land
there. That works for a portable extract-and-run, but:

- **ROM dumps need a writable directory.** Install root is typically
  `C:\Program Files\SubuwuTuner\` — admin-only writes. Dumping a ROM
  there fails for a normal-user invocation.
- **Definition customization wants a separate location.** A user who
  wants to drop loose `<id>.toml` overrides alongside the bundled
  archives (per the overlay model in `docs/17` §7 and commit
  `571ca22`) shouldn't have to edit files under `Program Files`.
- **Multi-user / shop scenarios.** Two techs on the same Windows box
  shouldn't share each other's ROM dumps or tune drafts.

Phase 2 introduces a config file that captures the user's chosen paths
and a runtime layer (`st::config`) that resolves them at startup.

## 2 — Configurable paths (Phase 2 scope)

Two paths in this phase. Adding more later is a schema additive.

| Key                   | Default (Windows)                                       | Default (Linux/Mac)                                                | Used by                                              |
|-----------------------|---------------------------------------------------------|---------------------------------------------------------------------|-------------------------------------------------------|
| `definitions_root`    | `<install>/definitions`                                 | `<install_prefix>/share/SubuwuTuner/definitions`                    | `st::defs::PackRegistry::from_default()`              |
| `rom_dump_root`       | `%USERPROFILE%\Documents\SubuwuTuner\roms`              | `$XDG_DATA_HOME/SubuwuTuner/roms`<br>(`~/.local/share/...` fallback) | `subuwutuner-cli rom-pull` default `--output` parent  |

Not Phase 2 (deferred):
- Project default directory (where `.stune/` lives) — users pick per-project today.
- Cache directory (auto-update downloads) — auto-update itself isn't built yet (see `docs/22`).
- Trace/log destination — currently stderr; piping is fine.

## 3 — Config file location

```
Windows : %APPDATA%\SubuwuTuner\config.toml
          (= C:\Users\<user>\AppData\Roaming\SubuwuTuner\config.toml)
Linux   : $XDG_CONFIG_HOME/SubuwuTuner/config.toml
          (= ~/.config/SubuwuTuner/config.toml; XDG_CONFIG_HOME unset → ~/.config)
macOS   : ~/Library/Application Support/SubuwuTuner/config.toml
```

Per-user, not system-wide. Two techs on the same machine get separate
configs naturally. (System-wide config — `C:\ProgramData\SubuwuTuner\` or
`/etc/SubuwuTuner/` — is a future addition with precedence
`user > system > built-in default`.)

## 4 — Config file schema

```toml
# SubuwuTuner runtime config — generated at install time by the NSIS
# installer; editable directly or via the GUI Settings dialog / CLI
# `subuwutuner-cli config` subcommand.

[paths]
# Where PackRegistry looks for <platform>.zip archives and per-platform
# loose <id>.toml overrides. See docs/17 §7 and the overlay loader in
# src/defs/include/st/defs/pack_registry.hpp.
definitions_root = "C:/Program Files/SubuwuTuner/definitions"

# Where `rom-pull` writes captured ROM dumps when no --output is given.
# Must be writable by the running user — not `Program Files/`.
rom_dump_root = "C:/Users/Cornelio/Documents/SubuwuTuner/roms"
```

No schema-version field in Phase 2 — added when the first additive
migration lands. Until then, unknown keys are silently ignored and
missing keys default per §2.

## 5 — Override precedence

Most-specific wins. Standard Twelve-Factor App layering:

1. **CLI flag** for that one invocation
   (e.g., `subuwutuner-cli rom-pull --output <path>`)
2. **Environment variable**
   (`ST_CONFIG_FILE`, `ST_DEFINITIONS_ROOT`, `ST_ROM_DUMP_ROOT`)
3. **Config file value** at the user-config-path location
4. **Built-in default** from §2

The `ST_CONFIG_FILE` env var picks an alternate config path (useful for
sandboxed CI runs and test fixtures). The per-key env vars
(`ST_DEFINITIONS_ROOT`, etc.) override individual values without
needing a separate file.

## 6 — C++ module: `st::config`

A new top-level library sibling to `st::core`. Pure utility — no domain
deps. Other modules call it; nobody calls them.

```cpp
// src/config/include/st/config.hpp

namespace st::config {

struct Paths {
    std::filesystem::path definitions_root;
    std::filesystem::path rom_dump_root;
};

class Config {
public:
    // Locate + load the user-level config file at the canonical path
    // (§3). Missing-file is not an error — returns a Config filled with
    // §2 defaults. Parse errors are surfaced.
    [[nodiscard]] static Result<Config> load();

    // Explicit path. Used by tests and by `--config <path>` CLI flag.
    [[nodiscard]] static Result<Config> load_from(std::filesystem::path const &);

    // Hard-coded §2 defaults; no file consulted.
    [[nodiscard]] static Config defaults();

    // Atomic write (tmp-file + rename). Creates parent dirs.
    [[nodiscard]] Status save() const;

    [[nodiscard]] Paths const &paths() const noexcept { return paths_; }
    [[nodiscard]] Paths       &paths()       noexcept { return paths_; }

    [[nodiscard]] std::filesystem::path const &source_path() const noexcept;

private:
    Paths paths_;
    std::filesystem::path source_path_;
};

// One-time process-lifetime accessor. Caches the resolved config and
// applies env-var overrides. CLI flag overrides go through explicit
// setters below since the parse happens later than program start.
[[nodiscard]] Config const &current();

// Override individual paths from CLI flags. Affects subsequent
// `current()` reads; thread-safety: writers must serialize externally
// (we don't lock — main-thread-only pattern).
void override_definitions_root(std::filesystem::path);
void override_rom_dump_root(std::filesystem::path);

// Path lookups exposed individually so callers don't need the full
// Config to do one thing. Apply override precedence (§5).
[[nodiscard]] std::filesystem::path definitions_root();
[[nodiscard]] std::filesystem::path rom_dump_root();

// Inspectors useful for diagnostics and for `subuwutuner-cli config path`.
[[nodiscard]] std::filesystem::path default_config_path();
[[nodiscard]] std::filesystem::path default_definitions_root();
[[nodiscard]] std::filesystem::path default_rom_dump_root();

} // namespace st::config
```

### 6a. Default resolution

`default_definitions_root()` finds the running binary's path via
`GetModuleFileName` (Win32) / `/proc/self/exe` (Linux) /
`_NSGetExecutablePath` (Mac), then takes `<parent>/definitions/`. This
handles both the `C:\Program Files\SubuwuTuner\` install-tree layout
AND a portable-extract layout under `D:\Tools\Subuwu\Builds\`.

`default_rom_dump_root()` resolves `%USERPROFILE%` (Win32) or `$HOME`
(POSIX) and appends `Documents/SubuwuTuner/roms`. Creating the directory
is the caller's responsibility (rom-pull creates parents at first use).

### 6b. Why a free-function facade plus a class

`Config` carries state (the loaded values + source path) for the
Settings UI to mutate and save. The free functions (`definitions_root()`
etc.) layer overrides on top for the common read path. Most call sites
just want a path; only Settings and CLI `config` want the full object.

## 7 — Integration points

| Module             | Today                                                   | Phase 2                                                                                                                                                                  |
|--------------------|---------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `st::defs::PackRegistry` | `from_directory(path)` — explicit path required.        | Add `PackRegistry::from_default()` that calls `st::config::definitions_root()` and forwards.                                                                              |
| `cmd_rom_pull`     | `--output <FILE>` required.                             | If `--output` omitted, default to `st::config::rom_dump_root() / "<CID>-<timestamp>.bin"`. Print the resolved path to stderr so the user sees where it went.            |
| `cmd_pack_list`    | Takes `<dir>` positional today.                         | If omitted, default to `st::config::definitions_root()`. Existing positional still works (CLI flag layer in precedence).                                                  |
| `subuwutuner-cli config` (new) | —                                                       | New subcommand. `get [key]` / `set <key> <value>` / `path` / `show`.                                                                                                       |
| GUI `Tools → Settings...` (new) | — (no settings dialog yet)                              | New modal. Folder pickers for the two paths. Save writes `config.toml`. Reloads `PackRegistry` from the new `definitions_root`.                                            |
| GUI `Tools → Browse Definitions...` | Asks for a root via folder picker each time.            | Default the picker to `st::config::definitions_root()`. User can still browse elsewhere if they want.                                                                     |

Migration is incremental — each command can adopt `st::config` without
forcing the whole CLI to switch at once.

## 8 — NSIS installer pages

CPack supports custom NSIS via:
- `CPACK_NSIS_EXTRA_INSTALL_COMMANDS` — inline NSIS script injected into
  the install section. Good for "write config.toml" after the user's
  choices are known.
- `CPACK_NSIS_INSTALL_SUBDIR` plus a custom template at
  `cmake/NSIS.template.in` for full UI customization (custom pages
  via `Page custom MyPageCreate MyPageLeave`).

Phase 2 needs custom UI pages — minimum two:

1. **Page 1 (existing):** standard MUI Directory page for the install
   root. CPack's NSIS template already provides this.
2. **Page 2 (new):** "Definitions root" picker, default
   `$INSTDIR\definitions`. NSIS variable `$DefinitionsRoot`.
3. **Page 3 (new):** "ROM dump root" picker, default
   `$DOCUMENTS\SubuwuTuner\roms`. NSIS variable `$RomDumpRoot`.
4. **Section -Install (existing+modified):** after files are laid down,
   write `%APPDATA%\SubuwuTuner\config.toml` with the chosen paths via
   NSIS's `FileOpen` / `FileWrite` / `FileClose`.

The two new pages will live in a `cmake/NSIS.template.in` derived from
the stock CPack template. CPack uses
`CPACK_NSIS_TEMPLATE` / its built-in if unset.

Skeleton of the post-install config write:

```nsis
Section "-WriteConfig"
  ; %APPDATA% on Windows
  SetShellVarContext current
  CreateDirectory "$APPDATA\SubuwuTuner"
  FileOpen $0 "$APPDATA\SubuwuTuner\config.toml" w
  FileWrite $0 "# SubuwuTuner runtime config — generated by installer$\r$\n"
  FileWrite $0 "[paths]$\r$\n"
  FileWrite $0 'definitions_root = "$DefinitionsRoot"$\r$\n'
  FileWrite $0 'rom_dump_root    = "$RomDumpRoot"$\r$\n'
  FileClose $0
SectionEnd
```

The uninstaller correspondingly removes the config file (or leaves it,
prompting "remove personal settings?" — common installer UX).

## 9 — GUI Settings dialog

Trigger: `Tools → Settings...` menu item, alongside `Read ROM from
Car...` and `Browse Definitions...` from previous commits.

Modal contents:
- Read-only label: "Config file: %APPDATA%\SubuwuTuner\config.toml"
- Folder picker + InputText: "Definitions root"
- Folder picker + InputText: "ROM dump root"
- Buttons: "Save" / "Cancel" / "Restore Defaults"
- Status area: "Saved 2026-05-24 14:53" / parse-error message.

Save flow:
1. Write to a `.tmp` sibling of `config.toml`.
2. `std::filesystem::rename(tmp, real)` for atomicity.
3. Reload `st::defs::PackRegistry` so the new `definitions_root` takes
   effect without a restart.
4. Flash status "Saved" with the inline-error pattern from memory
   `feedback_modal_inline_errors` (errors stay inside the modal, not
   the hidden-behind-modal status bar).

## 10 — CLI `config` subcommand

```
subuwutuner-cli config path                # print %APPDATA%\SubuwuTuner\config.toml
subuwutuner-cli config show                # dump effective values + sources
subuwutuner-cli config get definitions_root
subuwutuner-cli config get rom_dump_root
subuwutuner-cli config set definitions_root "D:/somewhere/else"
subuwutuner-cli config set rom_dump_root    "C:/Users/Cornelio/Downloads/roms"
subuwutuner-cli config reset                # restore §2 defaults
```

`show` includes provenance:

```
[paths]
definitions_root = "C:/Program Files/SubuwuTuner/definitions"  (source: config)
rom_dump_root    = "D:/dumps"                                  (source: env ST_ROM_DUMP_ROOT)
```

So a confused user can see exactly which layer is winning.

## 11 — Environment variables (override layer 2)

```
ST_CONFIG_FILE       absolute path; overrides the canonical config location
ST_DEFINITIONS_ROOT  overrides config.paths.definitions_root
ST_ROM_DUMP_ROOT     overrides config.paths.rom_dump_root
```

`ST_CONFIG_FILE` is useful for CI / tests / sandboxed runs where the
user's `%APPDATA%` shouldn't bleed in.

## 12 — Error handling

| Condition                            | Behavior                                                                                                                |
|--------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| Config file does not exist           | Use defaults silently. (Fresh install on a system that never had the installer run; portable extract.)                  |
| Config file exists but TOML-invalid  | Error message to stderr; exit non-zero on CLI, modal banner in GUI; do NOT silently fall back (corrupt configs hide bugs). |
| `definitions_root` doesn't exist     | `PackRegistry::from_directory` already returns InvalidArgument with a clear message. Treat as soft warning — proceed with an empty registry. |
| `rom_dump_root` doesn't exist        | `rom-pull` creates it lazily at first use; chown / permission failures surface naturally.                              |
| `%APPDATA%` not writable             | Save fails with PermissionDenied; modal shows the error. Rare — `%APPDATA%` is user-writable by definition.            |

## 13 — Versioning / migration

No schema-version field in Phase 2. When the first migration is needed
(adding a field that changes default semantics, or renaming an
existing key), add a `schema_version` field. Until then, additive-only
schema changes work without versioning.

When that day comes:
- Embed `schema_version = N` at the top of the file when saving.
- On load, if `schema_version` is missing, treat as v0 and migrate.
- If higher than the binary supports, refuse to load and tell the user
  to upgrade the binary or downgrade the config.

## 14 — Implementation order (when this gets built)

Each step independently testable + commit-sized. Stop after any step
and the system is still consistent.

1. **`st::config` library** — `Config` class, defaults, load/save, tests.
   `current()` + free-function facade.
2. **CLI `config` subcommand** — exercises load/save end-to-end. Smallest
   user-visible surface.
3. **Env-var overrides** — wire `getenv("ST_DEFINITIONS_ROOT")` etc.
   into the free-function facade. Tests check precedence.
4. **`cmd_pack_list`** — make `<dir>` optional; default to
   `st::config::definitions_root()`. One command, low risk.
5. **`cmd_rom_pull`** — same treatment for `--output` (default to
   `<rom_dump_root>/<CID>-<timestamp>.bin`).
6. **GUI Settings dialog** — Tools → Settings... modal. Reloads
   `PackRegistry` after save.
7. **NSIS custom pages** — `cmake/NSIS.template.in` + the page hooks +
   the install-time config-write section. Verified by running the
   installer interactively (or via the `/S` silent mode with `/D=...`
   flags to set paths from the command line).

Each step is roughly a single commit. Phases 1-3 (lib + CLI subcommand
+ env vars) are pure C++ with no UI work; Phase 4-5 are surgical CLI
changes; Phase 6-7 are the bigger UI / installer pieces.

## 15 — Open questions

- **Should `%APPDATA%\SubuwuTuner\` also hold cached registry index +
  scratch state?** Argument for: avoids re-scanning `definitions/` on
  every launch (374 packs × archive open is fast but not free).
  Argument against: cache invalidation is hard; ship without cache,
  add if startup latency matters. Defer to a later optimization.
- **Per-user config under `%APPDATA%` vs `%LOCALAPPDATA%`?** `%APPDATA%`
  roams across machines via Windows profile sync; `%LOCALAPPDATA%` is
  machine-local. Config (paths to local directories) is machine-local
  by nature, so `%LOCALAPPDATA%` is technically more correct. But
  `%APPDATA%` is what most apps use and what users expect. Going with
  `%APPDATA%` — minimal cost if a path doesn't make sense on another
  machine (warning at startup + UI lets user fix it).
- **System-wide config layer?** Useful for fleet / shop deployments
  where the IT admin sets the definitions root once for all users.
  Path: `C:\ProgramData\SubuwuTuner\config.toml`. Precedence: env vars
  > user config > system config > defaults. Defer to Phase 3.
- **Should the installer accept silent-install path overrides?** NSIS
  supports `/D=<dir>` for the install root in silent mode. We'd want
  parallel `/DEFINITIONS=<dir>` and `/ROMS=<dir>` flags. Adds NSIS
  complexity; reasonable for CI/shop scenarios; defer to Phase 3.

## 16 — See also

- `docs/17-data-distribution-policy.md` — Path B and the `<platform>.zip`
  shipping decision the config system is built on.
- `src/defs/include/st/defs/pack_registry.hpp` — the overlay loader
  that `definitions_root` points at.
- `tools/build_definition_archives.py` — produces the archives the
  installer drops at `<install>/definitions/`.
- `docs/22-auto-update.md` — adjacent runtime-state design; the config
  system here might share storage with the update-channel state once
  auto-update lands.
