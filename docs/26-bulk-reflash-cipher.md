# Optional capability: 0xB6 bulk-transfer write path

## Scope

This document describes an optional build configuration. It is **off by
default**. The information below is for the project's own contributors;
it is not a recommendation to enable the capability.

## What gets enabled

When the CMake option `ST_ENABLE_BULK_REFLASH_CIPHER` is `ON`, the build
includes the small cipher transform that the ECU's bootloader runs on
bytes received via the Subaru manufacturer-specific `0xB6` SID, plus the
single orchestrator branch that uses it on the write path when a flash
plan's `data_format` field is set to `0x04`. The cipher itself is
firmware-level fact recovered from analyzing the user's own ECU dump
(same provenance as the SA constants already in `src/ecu/src/subaru_security.cpp`;
see `docs/15-clean-room-engineering.md` for the facts-vs-expression
boundary).

When the option is `OFF` (the default), the cipher source is excluded
from the build and the orchestrator's `data_format=0x04` branch refuses
with `PolicyDenied`.

## Two-step arming

**Layer 2 — build:** `-DST_ENABLE_BULK_REFLASH_CIPHER=ON` at configure
time.

**Layer 1 — runtime:** `--enable-bulk-reflash-cipher` on the CLI
invocation. Process-scoped; no persistent state across invocations.

Both layers are required. Either alone yields `PolicyDenied`.

## Failure modes

| Build flag | Runtime flag | Behaviour |
|---|---|---|
| OFF | (any) | `PolicyDenied`. The CLI also notes when `--enable-bulk-reflash-cipher` is passed to a flag-OFF build. |
| ON  | omitted | `PolicyDenied`. |
| ON  | passed  | Write path proceeds. |

## Why gated

The write path is the single highest-risk operation in the codebase — a
malformed payload or interrupted transfer can brick the ECU. The
brick-protection model in `docs/05-improvements.md` §4 reduces but does
not eliminate the risk. Two intentional opt-in moves before any
invocation is the floor.

Public builds ship without the capability so the default cloned-and-built
binary has no ability to write via `0xB6`. Contributors who need it for
their own work flip one CMake option and pass one runtime flag per
invocation.

## Related

- `docs/05-improvements.md` §4 — brick-protection
- `docs/15-clean-room-engineering.md` — facts-vs-expression boundary
- `docs/23-security-access.md` — the SA Feistel from which this cipher's
  template is taken (4 rounds here, 16 there; same S-box; same primitive)
