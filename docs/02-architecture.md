# 02 — Architecture

## Designed for breadth from day one

The longer-term scope is **all Subaru platforms we can brick-test**, not just VA/VB WRX MT. The architecture below is laid out so that adding a new platform is additive, not invasive:

- **New vehicle** = new TOML definition file under `definitions/`. `st::defs` doesn't care whether the file describes a 2019 WRX, a 2008 STI, or a 2015 Forester XT.
- **New ECU family / protocol variant** = new module under `st::ecu::<family>` and `st::flash::<family>`. Existing modules (`ssm`, `uds`, `denso_va`, etc.) are untouched.
- **New transport** = new module under `st::transport::<adapter>` implementing the same `ITransport` interface.

What this means in practice: every interface in the layering below takes a `Platform` or `Definition` argument; nothing is hard-coded to "WRX". v1.0 ships with one definition pack for VA-WRX-MT and one for VB-WRX-MT, but `subarutuner-cli rom-info` will work on any Subaru ROM the moment someone contributes a definition file.

See `04-roadmap.md` for the v1.x platform-expansion order.

## Layering

```
┌────────────────────────────────────────────────────┐
│  UI layer    (Qt 6 widgets/QML, or Dear ImGui)     │
├────────────────────────────────────────────────────┤
│  Application services  (project mgr, undo/redo,    │
│                         scripting host, plugins)   │
├────────────────────────────────────────────────────┤
│  Domain model  (ROM, Table, Axis, Definition,      │
│                 LogSession, FlashJob)              │
├────────────────────────────────────────────────────┤
│  Transport    (J2534, ELM327, OBDLink-STN,         │
│                Tactrix OP2.0 native USB)           │
├────────────────────────────────────────────────────┤
│  Platform abstraction  (USB, serial, FS, threads)  │
└────────────────────────────────────────────────────┘
```

Every layer depends only on layers below it. The domain model has no Qt or USB types in its public headers — that is what lets us unit-test it and reuse it from a CLI.

## Module map

| Module | Responsibility | Key types |
|---|---|---|
| `st::core` | Value types, error handling, units | `Result<T, Error>`, `Quantity`, `Span` |
| `st::rom` | Binary ROM I/O, CRC, sectoring | `Rom`, `Sector`, `Checksum` |
| `st::defs` | Calibration definitions, scaling | `Definition`, `Table`, `Axis`, `Scaling` |
| `st::project` | `.stune` project files | `Project`, `ProjectStore` |
| `st::transport` | ECU comms abstraction | `ITransport`, `Frame`, `Session` |
| `st::transport.j2534` | J2534 DLL/SO loader | `J2534Device` |
| `st::transport.elm` | ELM327 AT-command driver | `ElmDevice` |
| `st::transport.stn` | OBDLink STN extensions | `StnDevice` |
| `st::ecu.ssm` | Subaru SSM protocol | `SsmClient` |
| `st::ecu.uds` | UDS / KWP2000 | `UdsClient` |
| `st::flash` | Erase/program/verify, brick guard | `Flasher`, `FlashPlan` |
| `st::log` | Datalogging | `LogStream`, `Pid`, `Sample` |
| `st::script` | Embedded scripting (Lua) for custom maps | `ScriptHost` |
| `st::nodegraph` | Visual logic compiler (Atlas equivalent) | `Graph`, `Node`, `CodeGen` |
| `st::ui` | GUI shell (Qt or ImGui) | view models bound to domain |
| `st::cli` | Headless tool | argparse + same domain |

## Concurrency model

Three thread categories:

- **UI thread** — the only one allowed to touch UI widgets.
- **Worker pool** — `std::jthread` workers driven by a lightweight task system for CPU-bound work (compression, CRC, codegen).
- **I/O reactor** — one dedicated thread per transport device. ECU comms are inherently serial; multiplexing them onto a reactor avoids leaking USB/serial handles across threads.

Communication is via message passing (a typed `concurrent_queue<Cmd>` per worker, plus a `Result`-bearing future returned to the caller). No shared mutable state across thread boundaries.

## Error handling

`std::expected<T, st::Error>` everywhere in domain/transport code. Exceptions only at the UI boundary, and only to interrupt long-running operations through `std::stop_token`. Cancellation must propagate to in-flight flash operations safely — see `05-improvements.md` for the brick-protection design.

## Plugin / extension surface

Atlas has "Atlas Mods" — importable tables and custom features. We expose two extension points:

1. **Definition packs** — TOML files dropped into a search path. Discovered at startup. Hot-reloadable in dev mode.
2. **Lua scripts** — sandboxed (no `io`, no `os`, no FFI). Used by the node-graph compiler as an intermediate representation, and directly by power users for batch transformations on tables and logs.

Native (DLL/SO) plugins are explicitly **out of scope for v1** — they are a vector for malicious tunes and a portability hazard.
