# Handoff — 2026-05-23 end-of-day (sniff mode + SA plug-in landed; awaiting Y-cable for SSMCAN1 derivation)

**Next action: when the user has the Y-cable, run the SA capture flow** documented in `docs/23-security-access.md`. CLI invocation:

```sh
subuwutuner-cli sniff --transport obdx --device COM3 \
    --output capture.log --filter 0x7E0,0x7E8
# In parallel: 10-15 COBB AP power cycles OR plug/unplug from Y-cable
# Ctrl+C the sniffer when done
python tools/extract_subaru_sa.py capture.log --output pairs.json
```

`pairs.json` is the input to the (yet-unwritten) algorithm-solver — that comes next.

## What landed today (uncommitted bundle, 5 logical commits ahead)

### 1. OBDX CAN ID prefix + ISO-TP filter setup (transport bug fix)

Diagnosed against the 2017 VA WRX morning of 2026-05-23 from the OBDX Pro Developers Reference Manual v3.00:
- Every `TxSmall` payload on CAN must start with `[4B BE CAN ID][user bytes]` (§3.6.3). We were sending raw protocol bytes with no ID prefix.
- Default adapter state is "all filters off → all frames dropped" (§3.4). We weren't setting up a Flow filter, so even when the ECU did respond on 0x7E8, the adapter dropped it.

Fix:
- `Opcode::CanProtocolSettings = 0x34` added to `obdx_dvi.hpp`
- `open()` step 4: Entire Filter setup before EnableNetwork — `34 11 00 00 00 01 01 00 00 07 E8 00 00 07 FF 00 00 07 E0 YY`
- `send_recv` / `send` prepend 4-byte BE CAN ID from `can_id_request_`
- `send_recv` RX strips 4-byte CAN ID + ISO-TP PCI (SF=0x0L → strip 1, FF=0x1XYY → strip 2)
- All open-handshake fixtures updated; +1 new write-log assertion verifying filter frame position

**Hardware validated** this afternoon: full handshake works end-to-end, DSC `10 03 → 50 03` succeeds, RMBA `23 24 ...` reaches ECU and gets structured `7F 23 33 (securityAccessDenied)` instead of silent timeout.

### 2. UDS SecurityAccess plug-in architecture

NRC 0x33 → need SA. Surveyed every clean-licensed reference for the Subaru SSMCAN1 algorithm: came up empty. RomRaider, ECUFlash, james-portman, LibSSM2 all GPL-3 (contamination risk for our Apache 2.0). The 2018+ AES algorithm is MIT-licensed in jglim/UnlockECU but doesn't apply to SH7058. Decided on a runtime-pluggable architecture:

- `st::ecu::SecurityKeyFn = std::function<Result<vector<u8>>(span<const u8> seed)>` typedef in `security_key.hpp`
- Three Subaru-era stubs in `subaru_security.hpp` (SSMK1/SSMCAN1/CY1-AES), all return `NotImplemented` with a clear pointer at the plug-in path
- `Flasher::set_security_key_fn(fn)` setter; defaults to `subaru::ssmcan1_key_stub`
- `Flasher::read_full_rom` gains `bool authenticate=false, std::uint8_t security_level=0x01` parameters
- When `authenticate=true`: request_seed → key_fn(seed) → send_key, with diagnostic error at each failure point
- GUI Read ROM modal: new "Authenticate (UDS SecurityAccess)" checkbox, default ON, tooltip explains the stub limitation
- +4 SA preamble tests in `test_flash.cpp` (happy path, stub→NotImplemented, NRC 0x35, authenticate=false skip)

**Hardware behavior with Authenticate ON + stub** (predicted, not yet tested): trace will show `27 01 → 67 01 SEED4` then `[err] flash: read_full_rom SecurityAccess key derivation failed: subaru security key stub — algorithm not provided...`. Strictly better than today's bare NRC 0x33: user sees the ECU's actual seed bytes in the trace AND gets an actionable error pointing at exactly what's missing.

### 3. Passive CAN sniffer mode (the path to a derivable SSMCAN1)

Asked the user to buy a CAN dongle; user pushed back asking why the OBDX itself can't sniff. Right answer — OBDX VX supports LISTEN-ONLY explicitly per §3.4 + §3.10.2. Built sniff mode end-to-end so the user only needs a $15 OBD-II Y-cable (Vgate B015649DHA, ordered):

- `LinkConfig::listen_only` flag (and `Frame::can_id` field — populated for CAN, 0 elsewhere)
- OBDX `open()` branches on `listen_only`: skip filter setup, EnableNetwork STATE=0x02 instead of 0x01
- `send` / `send_recv` reject with TransportUnavailable when `listen_only=true` (safe-by-default — can't accidentally TX while a tuner is bus-mastering)
- `start_streaming` actually implemented (was stub): spawns reader thread, parses every RxSmall push, populates `Frame{data, can_id, arrived}` (no ISO-TP strip — sniff sees raw bus bytes), invokes callback
- `stop_streaming` flips atomic + joins; destructor stops first
- `subuwutuner-cli sniff --transport obdx --device COM3 --output capture.log --filter 0x7E0,0x7E8 [--duration N]` with Ctrl+C clean shutdown
- `tools/extract_subaru_sa.py` — parses sniff log, extracts (seed, key) pairs via SF PCI strip + state machine, emits JSON; validated against synthetic log (correctly extracts 2 successful pairs, skips 1 NRC-0x35 rejected one)
- `docs/23-security-access.md` (new) — full architecture + Y-cable workflow + trace shapes + failure-mode NRC table
- `docs/13-transport.md` gains a "Sniff mode (passive bus monitor)" section
- +5 transport tests: listen_only handshake (no filter cmd, STATE=0x02), send/send_recv rejection, start_streaming round-trip with 2 frames, empty-callback rejection, stop-on-never-started no-op

### 4. Program icon

User added an AI-generated PNG (cute neon WRX with uwu face + gauge) to the desktop. Wired through:

- `assets/icon.png` (transparent rounded corners — masked via Pillow)
- `assets/icon.ico` (multi-res 16/24/32/48/64/128/256) → embedded into EXE via `src/ui/subuwutuner.rc` + CMake `enable_language(RC)`. Drives Explorer / taskbar / Alt+Tab.
- `src/ui/src/icon_data.hpp` (auto-gen 64×64 RGBA C++ literal) → `glfwSetWindowIcon` at startup. Drives the GLFW window title-bar icon.
- `scripts/embed_icon.py` regenerates both from `assets/icon.png` on demand
- GUI binary went from 14.6 → 14.8 MB (icon overhead)

### 5. HANDOFF refresh (this file)

## Test state: **919/919 green** (+9 from yesterday's 910)

+5 sniff transport tests, +4 SA preamble tests. All open-handshake fixtures updated to include the new CAN filter ACK between SetProtocol and EnableNetwork.

## Binary artifacts (Syncthing-distributed)

- `subuwutuner-gui.exe` — 14.84 MB (icon + sniff + SA wiring)
- `subuwutuner-cli.exe` — 5.96 MB (new `sniff` subcommand)

Both at `D:\Documents\JetBrains\SubaruTuner\build\win-mingw\bin\`, timestamps 17:33 / 18:23 on 2026-05-23.

## Commit posture

Five commits planned, in dependency order:

1. `fix(transport): OBDX CAN ID prefix + ISO-TP filter setup` — the morning's transport fix, hardware-validated
2. `feat(ecu+flash+ui): UDS SecurityAccess plug-in architecture` — SA infrastructure (key fn typedef, stubs, Flasher integration, UI checkbox, tests)
3. `feat(transport+cli): passive CAN sniffer mode + SA capture toolchain` — listen_only, start_streaming, sniff CLI, extractor, docs
4. `feat(ui): SubuwuTuner program icon` — assets + Windows .rc + GLFW glue
5. `docs(handoff): refresh end-of-session state` — this file

**Push posture**: hold until user runs the Y-cable capture and validates the toolchain. Already committed work from this morning's session (6 commits, `19eb16c` → `cf70a5f`) also still unpushed — push the whole batch together once the SA capture succeeds.

## Pre-existing untracked / leave-alone

- `SubaruTuner.zip` (120 MB, user-dropped backup)
- `definitions/legacy/.stfolder/` (Syncthing marker)
- `fixtures/projects/` (user's GUI-created test project "BigTittyGothGF")
- `tests/unit/ecu/test_ssm_properties.cpp` (still untracked from yesterday — promote to tracked when the SSM-on-CAN bundle commits land for real)

## Reference: Pre-Y-cable activities the user might want to try

Since the Y-cable is in transit (Vgate B015649DHA, ETA ~2 days), the user can run these on the existing OBDX VX (no Y-cable needed) for additional validation:

1. **SA-enabled Read ROM** with Authenticate checkbox ON. Will fail with `NotImplemented` at the key step but the trace shows the actual seed bytes from the ECU (one real data point) AND confirms the new SA plumbing reaches the wire correctly. ~5 min.

2. **RDBI VIN read** — UDS service 0x22 DID 0xF1 0x90. Not gated behind SA, returns 17-byte ASCII VIN which forces multi-frame ISO-TP RX (currently untested on real hardware). If it succeeds we know our PCI strip handles First Frame + Consecutive Frame correctly. If garbled, we have a concrete failure to fix. Would need a small CLI helper (`subuwutuner-cli rdbi --did 0xF190` or similar) since the current GUI Read ROM does RMBA only.

3. **Address-range probing** — `subuwutuner-cli rom-pull --addr 0x080000 --size 0x10 ...` to see if the calibration region is readable without SA. Low probability of success but quick to try.

## Reference: SSMCAN1 algorithm structure (publicly documented)

Per `fenugrec/nisprog/SubaruSIDs.txt` (GPL-3 — read for facts, don't lift code):

- 16-round XOR cipher
- Two lookup tables: `IndexKeyBase[16]` of 2-byte values (32 B) + `KeyPartsTable[32]` of 1-byte values (32 B) — **64 bytes total**
- 3-bit barrel-roll right per round
- Final top/bottom byte swap on the 4-byte output

With these 64 bytes + the operation sequence, any seed maps deterministically to its key. A future `tools/solve_ssmcan1.py` can derive the table values from ~10 captured (seed, key) pairs via brute-force / constraint solving (search space narrows dramatically per pair).

---

# Everything below this line is historical context from earlier sessions

The state described below was superseded by today's work, but the prose around motivation and decisions is still useful for future readers.

---

## Earlier today AM — OBDX CAN ID prefix + filter setup (now committed in commit 1 of the bundle above)

The morning's deep-dive into the OBDX Pro Developers Reference Manual v3.00 (`C:\Users\Cornelio\Desktop\OBDX-Pro-Developers-Reference-Manual.pdf`) revealed the two transport bugs above. Key sections used:

- **§3.2 Checksum Calculation** — sum + bitwise NOT (same as VT). Codec was already correct.
- **§3.4 Receive from Network Normal (0x08)** — adapter→PC push. CAN format: `08 [LEN] [4B BE CAN ID] [bus payload incl. PCI] [CHK]`. Filters default to OFF.
- **§3.6 Send to Network Normal (0x10)** — PC→adapter. CAN format: `10 [LEN] [4B BE CAN ID] [user bytes] [CHK]`.
- **§3.14 CAN Protocol Settings (0x34)** — Entire Filter sub-op 0x00 sets ID/mask/type/status/flow-id in one shot.

The fix unblocked everything from "silent timeouts everywhere" to "structured `7F 23 33` from ECU" which is what made today's SA work possible.

## Yesterday AM (2026-05-22) — SSM-on-CAN framing fix (6 commits committed locally, not pushed)

The first half of today's predecessor session fixed an SSM framing bug: K-Line wrapper bytes (`80 10 F0 LEN ... CSUM`) shouldn't be on the CAN bus. Implemented `st::ecu::ssm::Framing { KLine, IsoTp }` enum threaded through all SSM builders/parsers, SsmClient, Flasher, LogSession. UI selects `IsoTp` for real-hardware SSM mode.

Six commits landed (all on `main`, not pushed):
1. `19eb16c` — `fix(transport+ecu+flash+log): SSM-on-CAN bare-payload framing`. 19 files.
2. `64e5693` — `feat(defs): validate flags duplicate hook/primitive/writable_region`. 2 files.
3. `d474d1d` — `feat(feature_codegen): sqrt_float IR primitive (SH-2A FSQRT)`. 4 files.
4. `668a247` — `feat(fixtures): demo-pack writable_region + 4 FA24-themed hooks + 4 samples`. 6 files.
5. `85d0fb5` — `docs: FA20→FA24 swap worked example + IR primitive notes`. 2 files.
6. `cf70a5f` — `docs(handoff): refresh for end-of-session state`. 1 file.

## Syncthing desktop ↔ laptop

Installed 2026-05-22. Two send-only folders mirror desktop → laptop. Replaces the old zip-and-send loop.

- Desktop device ID: `NSYHPXO-QVWHQT6-4XUNW2M-OET6FJY-CYP7Z7I-CM5HV7D-M7PEYEY-SIIJKQ4`
- Laptop device ID: `VZ6D4AZ-WUZL35M-UR5EOJ4-TPBMLFJ-6QOT7TR-R4BAHVR-5HBTKOH-3GHJ6AP`
- Web UI: http://127.0.0.1:8384/ (API key in `.claude/SYNCTHING-SETUP.md`)
- Workflow: build on desktop; laptop receives binaries within seconds via fs-watcher. No git pull or rebuild on laptop.

## Pre-existing notes still relevant

- `fixtures/projects/Test/` — user's GUI-created test project. Personal test data; leave alone.
- `SubaruTuner.zip` at repo root — user-dropped backup; leave.
- `docs/09-risks.md` carries an unstaged two-blank-line edit from the user opening the file in their IDE. Cosmetic; don't include in commits.

## Pure-software follow-ups (not Y-cable-blocking, lower priority than the SA derivation)

1. **`tools/solve_ssmcan1.py`** — algorithm-solver. Takes `pairs.json` from the extractor + the publicly-documented algorithm structure; outputs the 64 bytes of table values. Write against real data once user has a capture.
2. **`subuwutuner-cli rdbi --did <hex>`** — small CLI helper for reading well-known UDS DIDs (VIN, cal ID, SW version). Most useful as the multi-frame ISO-TP RX validation path that doesn't require SA. Would surface ISO-TP PCI strip bugs (if any) without needing a successful flash unlock.
3. **CY1 AES implementation** — `jglim/UnlockECU/SubaruSecurityAccess2018CY1.cs` is MIT-licensed, can be re-implemented in `subaru_security.cpp` without contamination. Targets 2018+ Subarus, not the dev's 2017 — defer until Path B for VB packs.
4. **Ship blocker #10**: CI performance gate.
5. **Ship blocker #7**: Frozen `defgen` binary.
