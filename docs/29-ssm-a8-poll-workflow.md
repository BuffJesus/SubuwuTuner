# SSM-A8 polling — recovering the byte-layout of a tuner-pack DID response

> Use OEM SSM Command 0xA8 (Read-Multi-Address over ISO-15765) as a ground-truth RAM-read channel. Cross-correlate against captured tuner-pack DID polls (e.g. COBB's proprietary F3xx response) to recover the byte-to-RAM-address map without disassembling the tuner's polled-DID dispatcher in ROM.

## Why a separate doc

`docs/24-sniff-workflows.md` is about *passive capture* — listening to the bus while an active tuner-tool drives it. This doc is the *active counterpart*: SubuwuTuner is the tester. It also pairs with `docs/01-reverse-engineering.md`'s broader RE methodology and with `docs/23-security-access.md` if the SSM path turns out to be SA-gated on the target ECU (see §6 below).

The workflow exists because some commercial tuner-packs (notably COBB's calibration framework on Subaru SH-2A ECUs) inject custom polled-DID handlers — typically in the OEM `0x22 <DID-hi> <DID-lo>` Read-Data-By-Identifier service — that pack many monitor values into a few oversized DID responses (e.g. `F300..F304`, 75–86 bytes total). The packed-byte layout is proprietary, undocumented, and not derivable from the ECU's own definition files. Statically locating the dispatcher in ROM is possible but expensive — empirically tens of hours for the 2017 WRX FA20DIT case (see `fixtures/private/findings_dmann_sniff_*` if present). The SSM-A8 + correlation path retires that disassembly work in favour of a few hours of code and one driving session.

Status: **shipped and self-test-verified hardware-free.** End-to-end live-vehicle verification is one of the first targets once the bench rig from `docs/28-bench-rig-build.md` is up.

## How it works

Two captures from the same vehicle, same trajectory order, different transports:

1. **Tuner-pack DID capture** (already in hand if you've run an active tuner-tool session in `docs/24`). The active tool polls its proprietary DIDs at a few Hz; the OBDX VX in LISTEN-ONLY mode records every response frame to a `subuwutuner-cli sniff` log.
2. **OEM RAM-read capture** (the new session). `subuwutuner-cli ssm-a8-poll` polls a chosen set of 24-bit RAM addresses via SSM Command 0xA8 at a comparable rate. The active tuner-tool must NOT be on the bus during this session — two testers would contend on shared 0x7E0.

Drive the SAME ordered set of trajectories in both sessions — idle, cruises at increasing speeds, light pulls, one wide-open-throttle pull. The two captures don't share a wall clock, so the alignment is *shape-based*: each candidate value-series (one per `(DID, byte_offset, encoding)` for the tuner-pack side, one per `(RAM_addr, encoding)` for the OEM side) is sorted, binned into N quantiles, and Pearson-r²-compared against every same-width series on the other side. Pairs with R² ≥ 0.99 are candidate mappings.

Two filters make this tractable on real captures:

- **Same-width-only.** A u16-encoded F3xx byte slot is only compared against u16-encoded RAM windows. Mixing widths is geometric nonsense.
- **Featureless-series rejection.** A series with a sorted shape too close to a perfect linear ramp is dropped. Random byte noise sorts to a ramp regardless of its underlying signal, and two such series correlate at R² → 1.0 with each other — without this filter, pure noise dominates the candidate list. Real engine-state signals (RPM, MAF, coolant) have plateaus around idle and steep ramps during pulls; their quantile shapes are distinctly non-linear, so the filter doesn't discard them.

## The SSM-A8 wire shape

Same transport stack as everything else in `src/ecu/`:

- ISO-15765-2 over 500 kbps CAN, 11-bit addressing.
- Tester ID `0x7E0`, ECU response ID `0x7E8`.

Request (continuous read mode):

```
A8 00 <addr1_hi> <addr1_mid> <addr1_lo>  <addr2_hi> <addr2_mid> <addr2_lo>  ...
```

Each address is 24-bit big-endian. The ECU concatenates one byte from each requested address into the response, in request order:

```
E8 <byte1> <byte2> ...
```

Multi-byte values (e.g. an OEM u16 RPM at `0x00000E`) are read as two consecutive `--addr` slots and reassembled offline. Negative responses use the standard `7F <NRC>` pattern.

The protocol is implemented in `src/ecu/ssm.hpp` / `ssm.cpp` (`build_a8_request`, `parse_a8_response`, `SsmClient`) with `Framing::IsoTp` for the CAN-bus path. The K-Line variant in the same module is the original SSM2 wrapper for pre-CAN ECUs.

## Capture

Smoke test first — confirm SSM-A8 is open on the target ECU and produces sane values:

```sh
subuwutuner-cli ssm-a8-poll \
    --transport obdx --device <port> \
    --addr 0x00000E,0x00000F,0x000008 \
    --output ssm-a8-smoke.log --duration 10 --interval-ms 333
```

The three default addresses are RomRaider canonical OEM logger metric/logger.xml: `0x00000E + 0x00000F` is the u16-big-endian engine speed (scaled `x/4` rpm) and `0x000008` is the u8 coolant temperature (`x - 40` °C). At warm idle the first two bytes should decode to ~800 rpm and the third byte to ~90 °C.

If the smoke test returns NRC 0x33 on every poll, the path is SecurityAccess-gated on this ECU — see §6.

For the real session, broaden to a useful monitor set. A representative shortlist that covers the engine state most calibrations care about:

| OEM SSM address | Width   | Quantity                  | Conversion                  |
| --------------- | ------- | ------------------------- | --------------------------- |
| `0x000008`      | u8      | Coolant temperature       | `x - 40` °C                 |
| `0x00000E + 0F` | u16_be  | Engine speed              | `x / 4` rpm                 |
| `0x000010`      | u8      | Vehicle speed             | `x` km/h                    |
| `0x000011`      | u8      | Ignition timing total     | `(x - 128) / 2` deg         |
| `0x000012`      | u8      | Intake air temperature    | `x - 40` °C                 |
| `0x000013 + 14` | u16_be  | Mass airflow              | `x / 100` g/s               |
| `0x000015`      | u8      | Throttle position         | `x * 100 / 255` %           |
| `0x00001C`      | u8      | Battery voltage           | `x * 8 / 100` V             |
| `0x000022`      | u8      | Knock correction advance  | `(x - 128) / 2` deg         |

(All from RomRaider's canonical OEM-Subaru logger XML; the full list is in `build/scratch/SubaruDefs/RomRaider/logger/metric/logger.xml` after a build.)

Real run, ~30 minutes covering ordered trajectories:

```sh
subuwutuner-cli ssm-a8-poll \
    --transport obdx --device <port> \
    --addr 0x00000E,0x00000F,0x000008,0x000010,0x000011,0x000012, \
           0x000013,0x000014,0x000015,0x00001C,0x000022 \
    --output ssm-a8-poll.log --duration 1800 --interval-ms 333
```

Drive in this order — same order as the tuner-pack capture that this will correlate against:

1. Warm idle, 4 segments ~10 s each (AC off / off / on / on).
2. Steady-state cruise at 4 increasing speeds, 8–10 s each.
3. Light acceleration, 1st–4th gear, under 2 psi / 4000 rpm.
4. One wide-open-throttle pull, 2nd gear only, 2300 → 6000 rpm.

Pause ~5–10 s between segments — gives the rank-quantile correlator clearer shape boundaries.

## Correlate

After the session, run `tools/cross_ref_ssm_a8.py`:

```sh
python tools/cross_ref_ssm_a8.py \
    --sniff <path-to-the-existing-tuner-pack-sniff.log> \
    --ssm-a8 <path-to-the-new-ssm-a8-poll.log> \
    --out mapping.toml
```

Output is a TOML with one `[[mapping]]` table per F3xx byte slot, listing up to `--top-k` candidate RAM-address mappings (defaults to 5) above `--min-r2` (defaults to 0.99). A clean slot has one dominant candidate; a noisy slot has multiple high-R² hits and needs a second pass (re-poll with more or different addresses to break ties).

The script is self-testing — `python tools/cross_ref_ssm_a8.py --self-test` plants a synthetic RPM mapping at `F301:6 u16_be → 0x00000E u16_be`, runs the correlator, and exits 0 iff the planted pair is the top match at R² > 0.99. Useful as a regression check independent of any real capture data.

## SecurityAccess gating (NRC 0x33)

The OEM SSM-A8 path is canonical and not SA-gated stock. Some tuner-packs flip this — the empirical 2017 WRX finding is that COBB's calibration framework redirects a swath of OEM service handlers as part of its lockdown. If the smoke test returns NRC 0x33 on every poll, opt into the SA preamble:

```sh
subuwutuner-cli ssm-a8-poll \
    --transport <kind> --output <FILE.log> \
    --addr <hex>[,<hex>...] \
    --authenticate --sa-variant fehr-active-l3
```

The preamble does, before the poll loop starts:

1. `DiagnosticSessionControl 0x10 0x03` (extendedDiagnosticSession).
2. `SecurityAccess 0x27 <level>` requestSeed.
3. `<sa_variant_fn>(seed)` computes the key.
4. `SecurityAccess 0x27 <level+1> <key>` sendKey.

If the SA preamble fails (NRC on requestSeed or sendKey), the poll loop is not entered and the CLI exits non-zero. Diagnose by trying the other variants (`--sa-variant default | fehr-active | fehr-active-l3`) or falling back to a factory / uninstalled ECU on the bench rig per `docs/28`. The validated default level tracks the variant: 0x03 for `fehr-active-l3`, 0x01 otherwise; override with `--security-level <hex>`. See `docs/23-security-access.md` for the SA algorithm catalog and which variant fits which tune state.

## Limitations and design tradeoffs

- **Rank-quantile alignment is shape-only.** It survives pace mismatch between the two sessions but does require trajectory ORDER to match. If you drive the captures in different orders, all bets are off — re-run.
- **Tuner-pack transforms aren't identity.** The empirical finding on COBB-tuned 2017 WRXs is that the F3xx response packs values through derived per-monitor calculations rather than passing raw RAM bytes verbatim. A clean R² mapping says "this F3xx byte is some monotone function of this RAM byte" — useful as a placement answer, but for the value-conversion you still need a separate fit. The same data supports that fit; it's just not what this tool emits.
- **Featureless-series rejection is heuristic.** A real signal that happens to have a near-uniform distribution across the captured trajectory will get filtered out. In practice, real engine signals always have plateaus; if you suspect a real mapping was lost to the filter, pass `--no-reject-featureless` and inspect the higher-R² candidates manually.
- **Same-vehicle requirement.** Both captures must come from the same ECU on the same vehicle. The tuner-pack's per-monitor transforms are calibration-dependent; cross-vehicle mapping is a separate problem.

## Companion files

- `src/cli/main.cpp` — `cmd_ssm_a8_poll` (CLI surface) and `cmd_sniff` (the passive-capture counterpart).
- `src/ecu/ssm.hpp` / `ssm.cpp` — protocol layer; `SsmClient` with `Framing::IsoTp` is the production path for this workflow.
- `tools/cross_ref_ssm_a8.py` — offline correlator + `--self-test`.
- `docs/24-sniff-workflows.md` — sniff capture rig + log format the correlator consumes.
- `docs/23-security-access.md` — SA negotiation reference for the §6 fallback.
- `docs/28-bench-rig-build.md` — clean-ECU rig for the factory-ECU fallback.
