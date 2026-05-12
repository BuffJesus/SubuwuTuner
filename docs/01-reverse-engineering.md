# 01 — Reverse Engineering Strategy

To ship a working Subaru tuner we have to solve two reverse-engineering problems. Both are well-trodden ground in the public Subaru tuning community, and both should be approached clean-room: one contributor documents observable behavior in plain English, a separate contributor writes the C++ from that document. This way we can credibly say our code is original and our license (Apache 2.0) is uncontaminated by GPL or proprietary code we may have studied.

## Problem 1 — The ECU protocol

This is the part that actually matters and is the most reusable across platforms.

- **Transport:** ISO 15765-2 (CAN-TP) on VB, K-Line / ISO 14230 (KWP2000) on VA via a J2534-class adapter
- **Application layer:** Subaru SSM (`A8` read, `B0` write) over KWP, UDS-style services (`22 RDBI`, `2E WDBI`, `27 SecurityAccess`, `34/36 RequestDownload/TransferData`) on CAN
- **Flashing:** seed/key challenge (Subaru's algorithm has been public for years), erase + program in 64KB/256KB sectors, CRC at end
- **Datalogging:** continuous SSM `A8` polling of RAM addresses; PID definitions are the same data we already need for the editor

References that are legitimate to study (see the clean-room boundary section for hygiene rules):

- The **RomRaider** project (GPL, open source) — twenty years of VA and earlier protocol coverage
- The **EcuFlash** XML definitions on the RomRaider wiki — public map locations
- ISO 14229 (UDS) and ISO 15765 specs — paid but standard
- The Tactrix-published J2534 API — vendor-documented
- The **RomRaider forum** archives — map-discovery threads, stock-dump trades, protocol debugging notes
- Owner-supplied stock and tuned ROM dumps from forum communities (private fixtures, see `fixtures/private/`)

### Specific RomRaider artifacts we will study

| File / location | What it teaches us | How we use it |
|---|---|---|
| `RomRaider/src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java` (and CAN variant) | SSM frame layout, command bytes, checksum | Spec input for `st::ecu::ssm` — write fresh C++ from observed framing |
| `RomRaider/src/main/java/com/romraider/io/connection/...` | Tactrix J2534 wrapping | Spec input for `st::transport::j2534` |
| `RomRaider/src/main/java/com/romraider/io/protocol/ssm/iso9141/command/...` | Seed/key request flow, address-table queries | Spec input for `st::ecu::ssm::SeedKey` |
| `RomRaider/src/main/resources/definitions/` | Per-CID table addresses, axes, scaling formulas | **Translate to our TOML schema via the `defgen` tool** (see below) |
| `RomRaider/.../logger/`*.xml | Logger PID addresses and scalings | Translate the same way |
| Community forks (e.g. `bludgod/RomRaider`) | Newer or WIP definitions and features | Diff against mainline; cherry-pick concepts (not code) into our spec docs |
| RomRaider forum sticky threads on VA / VB | Stock ROM dumps by CID, new-map announcements | Source of test fixtures (private to `fixtures/private/`, not redistributed) |

## Problem 2 — Our own definition format

We define a SubuwuTuner-native format that we populate from public sources (RomRaider definitions, our own RAM-poking research, owner contributions). The full schema sketch lives in `11-definition-format.md`.

Approach: **TOML for human-edited bits, FlatBuffers for the binary calibration payload.**

```
my-project.stune/                      (directory or zip)
├── project.toml                        (vehicle, ECU id, included roms)
├── definitions/
│   ├── va-wrx-mt-2019.toml             (map locations, scaling, axes)
│   └── vb-wrx-mt-2022.toml
├── roms/
│   ├── stock_aw0123.bin                (raw ECU image)
│   └── mytune_v3.bin
└── logs/
    └── 2026-05-11_track.csv
```

Plain text everywhere a human might want to diff, version, or pull-request. Binary only where size matters.

## The `defgen` tool

Hand-writing 100+ table definitions for each ECU would be a colossal waste of time when public sources already have most of the data. The `defgen` tool, kept in `tools/defgen/` and **not part of the shipped product**, automates the translation:

```
tools/defgen/
├── defgen.py                     (Python 3.12+, single file)
├── README.md
├── tests/
└── mappings/
    ├── va-wrx-mt.yaml              (per-platform overrides: CID filters, address adjustments)
    └── vb-wrx-mt.yaml
```

Pipeline:

```
RomRaider XML (GPL, public)
        │
        ▼
   defgen.py  ── reads XML structure, applies our schema mapping rules
        │
        ▼
  definitions/<platform>-<cid>.toml   (Apache 2.0, in our repo)
```

`defgen` extracts the *facts* (addresses, scalings, axis ranges) — which are not copyrightable in most jurisdictions because they are objective measurements of the ECU — and re-encodes them in our schema. It does **not** copy any source code, comments, or documentation strings verbatim. Where the source XML carries free-form descriptions, defgen strips them; descriptions in our TOML come from our own writing.

## Clean-room boundary — rules of engagement

To keep our Apache 2.0 license uncontaminated by GPL or proprietary source:

1. **No source copies.** No file in this repo may be derived by translating or transcribing source from any other tuning tool. If a maintainer needs to read a third-party source to understand a protocol detail, they document the *protocol* in plain English in our docs, and a different contributor writes the C++ from that document.
2. **Facts are facts.** Memory addresses, scaling coefficients, axis breakpoints, and CRC polynomials are factual data about the ECU. Reproducing them in a different format is not copying expression. `defgen` operates on this principle.
3. **Strings are expression.** Description text, map names, and comments in third-party XML or source are creative expression. We do not copy them. `defgen` strips them; we write our own descriptions, often shorter and project-specific.
4. **Attribution.** Every TOML file generated from a public source carries a header noting the data source and license context. Our `THIRD_PARTY_NOTICES.md` lists those sources as data sources, not as code dependencies.
5. **Binary does not entangle.** We do not link against GPL libraries, embed their JARs, or ship their XML in our installer. We ship our TOML; users who want the original tool can run the original tool.

## Suggested order of attack

1. Sketch the TOML definition schema (`docs/11-definition-format.md`) so we know what `defgen` is targeting — **done**
2. Implement read-only ROM open + table render against a known stock dump — no ECU connection needed yet (Phase 1)
3. Pull public VA/VB definitions and translate to our TOML via `defgen` (Phase 1)
4. Add a J2534 transport, then SSM read, then datalogging, then writing, then full flash (Phases 3–4)
