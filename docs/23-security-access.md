# Security Access — UDS SID 0x27 architecture and capture workflow

> Why this is a plug-in rather than a built-in algorithm, and how to derive a working Subaru key function from a Y-cable capture.

## TL;DR

Modern Subaru ECUs (SH7058 silicon, 2008–2017; RH850 silicon, 2018+) gate
ReadMemoryByAddress and most flash services behind UDS SecurityAccess
(SID 0x27). To dump a stock ROM, SubuwuTuner has to:

1. Request a 4-byte seed from the ECU (`27 01`)
2. Compute the matching 4-byte key from the seed via a Subaru-specific
   algorithm (a 16-round XOR cipher with two lookup tables = 64 bytes of
   constants)
3. Send the key back (`27 02 [key]`) — ECU unlocks the session
4. Proceed to RMBA

Steps 1, 3, and 4 are fully implemented and tested in this codebase.
**Step 2 is intentionally not.** The algorithm itself is a runtime
plug-in via `Flasher::set_security_key_fn(...)`.

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
`st::ecu::subaru::ssmcan1_key_stub`, which returns `NotImplemented` with
a clear error message pointing readers at this doc. Forks that supply a
real key function never see the stub.

The eras the project plans to support eventually:

| Era       | Silicon | Algorithm     | Seed/key bytes | Stub name              |
|-----------|---------|---------------|----------------|------------------------|
| pre-2008  | SH7055  | SSMK1         | 4              | `ssmk1_key_stub`       |
| 2008–2017 | SH7058  | SSMCAN1       | 4              | `ssmcan1_key_stub`     |
| 2018+     | RH850   | CY1 (AES-CBC) | 16             | `cy1_aes_key_stub`     |

The CY1 algorithm is publicly known (jglim/UnlockECU/SubaruSecurityAccess2018CY1.cs,
MIT) and could be implemented in-tree without contamination — we just
haven't yet. SSMK1 and SSMCAN1 don't have license-compatible references
we've found.

## Deriving SSMCAN1 from a Y-cable capture (the recommended path)

Best path for a vehicle owner with a working authenticated tool (COBB
AP, EcuTek, ECUFlash, etc.) already paired to their car:

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
    --transport obdx --device COM3 \
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
- `src/ecu/include/st/ecu/security_key.hpp` — the plug-in type
- `src/ecu/include/st/ecu/subaru_security.hpp` — the stub declarations
- `tools/extract_subaru_sa.py` — the capture parser
- `CLAUDE.md` "Stance on third-party IP" — the legal posture
