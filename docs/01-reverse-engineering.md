# 01 — Reverse Engineering Strategy

We cannot ship a working tuner without solving three reversing problems. All three must be done in clean-room fashion: one team member documents observable behavior, a second writes code from the spec, so we can credibly say no Atlas-derived source was used. The same hygiene applies to RomRaider — see "Clean-room boundary" below.

## Problem 1 — The `.atlas` container

**Easy.** It's a plain ZIP. Done.

```
Definitions-VA_WRX_MT.atlas
├── project.aproj            (binary, encrypted)
├── .gitignore
└── <uuid>_0.acf  × N        (binary, encrypted)
```

The UUIDs in filenames almost certainly key into a manifest stored inside `project.aproj` — i.e. the project file lists logical calibrations and points at their `.acf` blobs.

## Problem 2 — The `.aproj` / `.acf` encryption

`project.aproj` starts with 16 bytes of `0x00` and then ~2.4 MB of high-entropy data. Plausible formats:

| Hypothesis | Evidence | Next step |
|---|---|---|
| AES‑CBC, IV = first 16 bytes (here all-zero) | 16-byte alignment, entropy after byte 16 | Try AES‑128/192/256‑CBC with several candidate keys; check for known plaintext (UUIDs, "ATLAS", XML tags, zlib magic `78 9C`) after decrypt |
| AES‑GCM, nonce-then-ciphertext | Auth tag would sit at file tail | Examine final 16 bytes for high entropy distinct from body |
| Compressed-then-encrypted | Entropy uniform throughout | After decrypt, look for `zlib`/`zstd`/`lz4` magic |

The key is embedded in the Atlas JAR. Extracting it requires running the published Atlas binary in a debugger or using a JVM bytecode tool — both are reversing tasks subject to the EULA. **Recommended path:** treat the existing `.atlas` files as opaque seed data and build our own native definition pack format (see Problem 4). The encrypted Atlas files become useful only if we want round-trip interop with users coming from Atlas, which is a v2 concern.

## Problem 3 — The ECU protocol

This is the part that actually matters and is well-trodden ground in the Subaru tuning community:

- **Transport:** ISO 15765-2 (CAN-TP) on VB, K-Line / ISO 14230 (KWP2000) on early VA via Tactrix
- **Application layer:** Subaru SSM (`A8` read, `B0` write) over KWP, UDS-style services on CAN
- **Flashing:** seed/key challenge (Subaru's algorithm has been public for years), erase + program in 64KB/256KB sectors, CRC at end
- **Datalogging:** continuous SSM `A8` polling of RAM addresses; PID definitions are the same data Atlas's "memory parameters" describe

References that are legitimate to study (see the clean-room boundary section for hygiene rules):

- The **RomRaider** project (GPL, open source) — VA and earlier protocol coverage
- The **EcuFlash** XML definitions on RomRaider's wiki — public domain map locations
- ISO 14229 (UDS) and ISO 15765 specs — paid but standard
- Tactrix's published J2534 API — vendor-documented
- The **RomRaider forum** archives — twenty years of map-discovery threads, stock-dump trades, and protocol debugging notes

### Specific RomRaider artifacts we will study

| File / location | What it teaches us | How we use it |
|---|---|---|
| `RomRaider/src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java` (and CAN variant) | SSM frame layout, command bytes, checksum | Spec input for `st::ecu::ssm` — write fresh C++ from observed framing |
| `RomRaider/src/main/java/com/romraider/io/connection/...` | Tactrix J2534 wrapping | Spec input for `st::transport::j2534` |
| `RomRaider/src/main/java/com/romraider/io/protocol/ssm/iso9141/command/...` | Seed/key request flow, address-table queries | Spec input for `st::ecu::ssm::SeedKey` |
| `RomRaider/src/main/resources/definitions/` (or wherever the XML lives in current builds) | Per-CID table addresses, axes, scaling formulas | **Translate to our TOML schema via the `defgen` tool** (see below) |
| `RomRaider/.../logger/`*.xml | Logger PID addresses and scalings | Translate the same way |
| `bludgod/RomRaider` and other forks | Newer or WIP definitions and features | Diff against mainline; cherry-pick concepts (not code) into our spec docs |
| RomRaider forum sticky threads on VA / VB | Stock ROM dumps by CID, new-map announcements | Source of test fixtures (private to `fixtures/private/`, not redistributed) |

## Problem 4 — Our own definition format

Rather than reverse the encrypted Atlas format end-to-end, define a SubaruTuner-native format that we can populate from public sources (RomRaider definitions, our own RAM-poking research, owner contributions). The full schema sketch lives in `11-definition-format.md`.

Proposal: **TOML for human-edited bits, FlatBuffers for the binary calibration payload.**

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

Hand-writing 100+ table definitions for each ECU would be a colossal waste of time when RomRaider has done it already. The `defgen` tool, kept in `tools/defgen/` and **not part of the shipped product**, automates the translation:

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
RomRaider XML (GPL'd, public)
        │
        ▼
   defgen.py  ── reads XML structure, applies our schema mapping rules
        │
        ▼
  definitions/<platform>-<cid>.toml   (Apache 2.0, in our repo)
```

`defgen` extracts the *facts* (addresses, scalings, axis ranges) — which are not copyrightable in most jurisdictions because they are objective measurements of the ECU — and re-encodes them in our schema. It does **not** copy any RomRaider code, comments, or documentation strings verbatim. Where RomRaider's XML carries free-form descriptions, defgen strips them; descriptions in our TOML come from our own writing.

## Clean-room boundary — rules of engagement

To keep our Apache 2.0 license uncontaminated by GPL RomRaider source:

1. **No source copies.** No file in this repo may be derived by translating or transcribing a RomRaider source file. If a maintainer needs to read RomRaider source to understand a protocol detail, they document the *protocol* in plain English in our docs, and a different contributor writes the C++ from that document.
2. **Facts are facts.** Memory addresses, scaling coefficients, axis breakpoints, and CRC polynomials are factual data about the ECU. Reproducing them in a different format is not copying expression. `defgen` operates on this principle.
3. **Strings are expression.** Description text, map names, and comments in RomRaider XML are creative expression. We do not copy them. `defgen` strips them; we write our own descriptions, often shorter and project-specific.
4. **Attribution.** Every TOML file generated from RomRaider XML carries a header noting the data source and license context. Our `THIRD_PARTY_NOTICES.md` lists RomRaider as a data source, not as a code dependency.
5. **Binary does not entangle.** Linking against RomRaider, embedding their JAR, or shipping their XML in our installer would create a derivative work. None of those happen. We ship our TOML; users who want RomRaider can run RomRaider.

The same boundary applies to Atlas: do not decompile, do not transcribe.

## Suggested order of attack

1. ZIP-walk the existing `.atlas` files and dump every entry's header for the wiki
2. Sketch the TOML definition schema (`docs/11-definition-format.md`) so we know what `defgen` is targeting
3. Pull RomRaider's VA/VB definitions and translate to our TOML via `defgen`
4. Implement read-only ROM open + table render against a known stock dump — no ECU connection needed yet
5. Add a J2534 transport, then SSM read, then datalogging, then writing, then full flash
6. Optionally, later: tackle Atlas file decryption for import compatibility
