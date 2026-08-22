# Security Access — UDS SID 0x27 architecture and capture workflow

> Why this is a plug-in rather than a built-in algorithm, and how to derive a working Subaru key function from a Y-cable capture.

## TL;DR

Modern Subaru ECUs (SH-2A silicon — SH7055/58/59 — 2008–2017; RH850 silicon, 2018+) gate
ReadMemoryByAddress and most flash services behind UDS SecurityAccess
(SID 0x27). To dump a stock ROM, SubuwuTuner has to:

1. Request a 4-byte seed from the ECU (`27 01`)
2. Compute the matching 4-byte key from the seed via a Subaru-specific
   algorithm (a 16-round XOR cipher with two lookup tables = 64 bytes of
   constants)
3. Send the key back (`27 02 [key]`) — ECU unlocks the session
4. Proceed to RMBA

Steps 1, 3, and 4 are fully implemented and tested in this codebase.
**Step 2 is also implemented in-tree now** for the A-series SSMCAN1 family
— the analyst-mode RE finding in §"Algorithm structure recovered" below
made the constants firmware-derived (not GPL-contaminated), so the factory
algorithm + aftermarket variants ship in `src/ecu/src/subaru_security.cpp`.
The plug-in seam (`Flasher::set_security_key_fn(...)`) is still present
for forks, handhelds, and the not-yet-implemented Gen-B (RH850 / CY1) path.

## Why the key function is plug-in rather than built-in

Every plain-C / C++ / Python implementation of the SSMCAN1 algorithm we
surveyed lives behind a GPL-3 license (RomRaider, ECUFlash, james-portman/
subaru-ecu-flashing) or is closed-source-but-source-available (Atlas,
COBB). Lifting the tables into this Apache-2.0 codebase — or paraphrasing
the algorithm shape from a GPL-3 source — would force the whole project
under GPL-3 by viral contamination.

The clean architectural fix: the SubuwuTuner project ships everything
*except* the 64 bytes of table values + the operation sequence, and
exposes a plug-in point. Downstream forks supply the key function under
whatever license they want (their own clean-room derivation, a commercial
SDK they've licensed, a GPL-3 subproject linked dynamically, etc.).

See `CLAUDE.md` "Stance on third-party IP" for the full rationale and
`src/ecu/include/st/ecu/security_key.hpp` for the plug-in shape.

## The plug-in interface

```cpp
// st/ecu/security_key.hpp
using SecurityKeyFn = std::function<
    Result<std::vector<std::uint8_t>>(std::span<std::uint8_t const> seed)>;
```

To use it:

```cpp
st::flash::Flasher flasher{transport, ecu::ssm::Framing::IsoTp};
flasher.set_security_key_fn(my_subaru_key_fn);
auto rom = flasher.read_full_rom(
    /*base=*/0, /*total=*/0x200000, /*max_chunk=*/0x100,
    /*per_chunk_timeout=*/1500ms,
    /*progress=*/nullptr, /*cancel=*/nullptr,
    /*enter_diagnostic_session=*/true,
    /*authenticate=*/true);
```

`Flasher` ships with a default `security_key_fn_` set to
`st::ecu::subaru::ssmcan1_key_stub`. The name is retained for source
compatibility, but the function is no longer a stub — it computes the
real 16-round Feistel against the in-tree round-key tables. CLI
`--sa-variant {factory,aftermarket,aftermarket-l1,aftermarket-l3}`
selects between the variants below; aftermarket variants share the same
Feistel structure with different round-key tables (and, for L3, a 5-byte
loop-reversal patch at flash 0xBE911 + 0xBE9C7..0xBE9CE). The legacy
spelling `fehr-active{,-l1,-l3}` is accepted as a deprecated alias
for one release cycle (`cobb-ap` was never wired in the CLI parser
and is rejected — historical doc reference only).

Era / variant catalog:

| Era       | Silicon | Algorithm     | Seed/key bytes | Variant in tree                                                       | Status         |
|-----------|---------|---------------|----------------|-----------------------------------------------------------------------|----------------|
| pre-2008  | SH7055  | SSMK1         | 4              | `ssmk1_key_stub`                                                      | ⬜ not yet     |
| 2008–2017 | SH7058  | SSMCAN1 (L1)  | 4              | `ssmcan1_key_stub` (factory)                                          | ✅ shipped     |
| 2008–2017 | SH7058  | SSMCAN1 (L1)  | 4              | `ssmcan1_l1_aftermarket`                                              | ✅ shipped     |
| 2008–2017 | SH7058  | SSMCAN1 (L3)  | 4              | `ssmcan1_l3_aftermarket`                                              | ✅ shipped     |
| 2018+     | RH850   | CY1 (AES-128 ECB) | 16             | `cy1_aes_key_stub`                                                    | ⬜ not yet     |

The CY1 algorithm is publicly known (jglim/UnlockECU/SubaruSecurityAccess2018CY1.cs,
MIT) and could be implemented in-tree without contamination — we just
haven't yet. SSMK1 doesn't have a license-compatible reference we've
found, but the analyst-mode workflow that recovered SSMCAN1 would apply
here too once pre-2008 ROM dumps are in hand.

## Algorithm structure recovered (2026-05-24)

Analyst-mode RE of the plaintext flash images for all 8 A-series CIDs (2015–2021 WRX 6MT) and all 16 B-series CIDs (2022–2026) has identified both generations' SecurityAccess primitives. The wall-clean spec lives off-tree at the analyst workspace (`Findings/algorithms/`, staged into `fixtures/private/findings_algorithms/` for reference). Headline state change:

- **Gen-A (SH-2A 1 MB / 2 MB, all `LF7x` / `LF9x` and predecessors):** 16-round Feistel on 32-bit blocks, with a 32-entry × 4-bit packed S-box providing non-linearity. Per-round F is *xor with round key → 5-bit S-box lookup per nibble (with bit 0 of x promoted to bit 4 of the high-nibble index, an asymmetric construction) → 16-bit rotate-left by 13*. L1 round-key table (16 × uint16) is **byte-identical across every A-series ROM sampled**; flash address varies per CID but the bytes are the same. Gen A.2 (2 MB) carries an additional L3/L5 round-key table sitting 32 bytes before the L1 table — these are the "deep diagnostic" sub-functions (`27 03/04`, `27 05/06`). Bootloader-unlock-only sessions still use L1.

- **Gen-B (RH850 4 MB, all `LHB*`):** **AES-128 in ECB mode**, NOT the Gen-A Feistel. Forward S-box and Te1/Te2/Te3 T-tables are present in the firmware at known flash addresses; inverse S-box is *absent* (the ECU only encrypts, which is what a seed-to-key one-way function needs). Three universal 16-byte master keys recovered byte-identical across all 16 B-series ROMs. K_secret #1 is confirmed `flash_write` (sub-fn `0x03/0x04`); #2 and #3 map to the `datalog` and `virginize` levels in TBD order.

**Implications:**

1. **Provenance is firmware, not GPL.** The constants and the structural description above were extracted from the plaintext ROMs themselves under the analyst-mode workflow (`docs/15`). That is a different upstream than the GPL-3 implementations surveyed when this plug-in was designed (RomRaider / ECUFlash / james-portman / LibSSM2 — see `Why the key function is plug-in rather than built-in` above). The original copyright-contamination argument for keeping the algorithm out of this Apache-2.0 codebase therefore does not apply to the analyst-mode output. A second, distinct decision still remains: even with copyright clean, should the algorithm be bundled in the public repo or distributed via the existing plug-in seam? This is the parallel question to `docs/17`'s Path B call for definitions, and the developer's call.

2. **`tools/solve_ssmcan1.py`'s UNSAT was structural, as the linearity-check predicted.** The solver scaffold encoded a 16-round XOR cipher per the publicly described fenugrec/nisprog shape (`IndexKeyBase[16]` + `KeyPartsTable[32]` + 3-bit barrel-roll + byte swap). That encoded structure is provably GF(2)-linear (`linearity-check` finding, commit `ae6cf7d`), so ~480 of 512 table bits are mathematically invisible. The actual primitive is a Feistel with a non-linear S-box, which is consistent with the linearity diagnostic: the "S-box-style indexing" refinement the HANDOFF flagged is exactly the right one. Two paths from here:
   - **Refine the solver** to encode the Feistel + S-box structure (without consulting the analyst-side constants), then verify it solves against tomorrow's Y-cable capture independently. This keeps the existing clean-room boundary intact for the in-tree code path and gives the public repo a derive-it-from-pairs flow.
   - **Skip the solve.** With the structure and constants already byte-verified across 8 ROMs analyst-side, the Y-cable capture becomes a *validation* step ("does the analyst-side answer round-trip against this car's seed?") rather than a derivation step.

3. **Plug-in API is unchanged.** `st::ecu::SecurityKeyFn` and `Flasher::set_security_key_fn` continue to be the integration seam regardless of which delivery path the algorithm takes. The stubs in `subaru_security.hpp` remain `NotImplemented` until a delivery decision lands.

4. **Per-CID round-key addresses are catalogued.** All eight FULL-decrypt A-series CIDs have their L1 table address tabulated (see `fixtures/private/findings_algorithms/generation-A-seed-to-key.md` § Constants). LF79103P (the 2017 USDM 6MT, the user's daily-driver family) is at flash `0x06E358` (L1) / `0x06E338` (L3/L5). Same for all 16 B-series CIDs' AES key blocks.

The Y-cable capture flow described below remains correct as a parallel / verification path — it just isn't the *only* path to a working SA function anymore.

## Deriving SSMCAN1 from a Y-cable capture (the parallel / verification path)

Best path for a vehicle owner with an aftermarket flasher already
paired to their car:

### Hardware

- **OBDX Pro VX** (already required for SubuwuTuner): acts as the
  passive sniffer.
- **OBD-II Y-splitter cable** (~$15): plugs both the OBDX VX and the
  active tuning tool into the same OBD-II port. CAN is a multi-drop bus
  so both adapters share CAN-H/CAN-L cleanly; no intermediate device or
  CAN gateway needed.

### Capture

```sh
# Terminal 1: start the sniffer. LISTEN-ONLY mode — the VX never
# transmits, so the active tool gets its bus all to itself.
subuwutuner-cli sniff \
    --transport obdx --device COM5 \
    --output capture.log \
    --filter 0x7E0,0x7E8

# Terminal 2 (or just at the car):
# Disconnect AP, reconnect, repeat 10–20 times. Each connect triggers a
# fresh SA exchange that the sniffer captures.

# Ctrl-C the sniff when you're done.
```

The OBDX is opened with `LinkConfig::listen_only=true` which means:

- No CAN filter is configured at the adapter level — every frame on the
  bus is pushed to the host.
- `EnableNetwork` uses STATE=0x02 (LISTEN-ONLY), so the adapter never
  transmits, ACKs, or otherwise touches the bus. Critical when sharing
  the OBD-II port with another tool — two transmitters fighting over the
  same CAN-H/CAN-L would collide.
- `send` / `send_recv` are disabled (return TransportUnavailable). The
  only way to interact with the bus in sniff mode is via
  `start_streaming` + a frame callback.

### Extract

```sh
python tools/extract_subaru_sa.py capture.log --output pairs.json
```

Output looks like:

```json
{
  "schema": "subuwutuner.sa.v1",
  "request_id": "0x7E0",
  "response_id": "0x7E8",
  "pairs": [
    { "seed": "DEADBEEF", "key": "12345678", ... },
    ...
  ]
}
```

The extractor handles ISO-TP unwrapping, ignores noise frames, and
discards partial exchanges (where the ECU rejected the key with NRC
0x35, or the tool gave up before sending the key). Output JSON contains
only successful (seed, key) tuples — the algorithm-solver's input.

### Solve

Future work: a Python script (`tools/solve_ssmcan1.py`) takes the JSON
of pairs and an algorithm structure description, searches for the
specific table values (64 bytes total) that satisfy every observed pair.
Algorithm structure is publicly documented (fenugrec/nisprog
SubaruSIDs.txt describes a 16-round XOR cipher with `IndexKeyBase[16]`
2-byte values + `KeyPartsTable[32]` 1-byte values + 3-bit barrel-roll +
top/bottom byte swap). With 10+ pairs the search space collapses fast.

Output of the solve: a C++ source file with the table literals filled
in, ready to drop into a downstream fork's build.

### Use

In your fork:

```cpp
#include "your_fork/subaru_ssmcan1_real.hpp" // your derived implementation

st::ecu::SecurityKeyFn key_fn = your_fork::subaru_ssmcan1_real;
flasher.set_security_key_fn(key_fn);
```

The upstream SubuwuTuner remains unaltered and Apache-2.0 clean; your
fork carries whatever license you derived under (you can choose Apache
2.0 since your derivation is clean-room from your own car's behavior).

## Trace shape on the wire

What a successful SA exchange looks like in `[trace][obdx-tx]` /
`[trace][obdx-rx]` lines (sniff mode would show the same CAN IDs):

```
[trace][obdx-tx] 10 03                      ← DSC extended session
[trace][obdx-rx] 50 03                      ← positive
[trace][obdx-tx] 27 01                      ← requestSeed level 0x01
[trace][obdx-rx] 67 01 DE AD BE EF          ← ECU's 4-byte seed
                                              (key_fn called here)
[trace][obdx-tx] 27 02 12 34 56 78          ← sendKey level 0x02
[trace][obdx-rx] 67 02                      ← positive (unlocked)
[trace][obdx-tx] 23 24 00 00 00 00 01 00    ← RMBA, addr=0x0, size=0x100
[trace][obdx-rx] 63 ...                     ← 256 bytes of ROM
```

Failure modes:

- `[trace][obdx-rx] 7F 27 35` — NRC 0x35 invalidKey. Algorithm
  implementation is wrong, or the seed → key derivation has a bug.
- `[trace][obdx-rx] 7F 27 36` — NRC 0x36 exceededNumberOfAttempts. ECU
  has locked SA for some time (usually 10 min on Subaru). Power-cycle
  the ECU (ignition off → wait 10s → on) to reset.
- `[trace][obdx-rx] 7F 23 33` — NRC 0x33 securityAccessDenied. SA ran
  but at the wrong level, or RMBA at this address needs a higher level
  than 0x01.

## See also

- `docs/13-transport.md` §sniff for the listen_only mode wire details
- `docs/24-sniff-workflows.md` — other uses of the same Y-cable rig (sniff-during-flash, datalogger RAM-address discovery, protocol learning, feature reverse-engineering for launch control / flat-foot shift / rev limits)
- `src/ecu/include/st/ecu/security_key.hpp` — the plug-in type
- `src/ecu/include/st/ecu/subaru_security.hpp` — the stub declarations
- `tools/extract_subaru_sa.py` — the capture parser
- `CLAUDE.md` "Stance on third-party IP" — the legal posture
