# Handoff — 2026-05-23 end-of-day (sniff mode + SA plug-in + rdbi shipped; pushed to origin/main)

**Five pushes today: ~18:40 SA/sniff/rdbi cycle + ~20:00 docs follow-up + ~21:30 solver scaffold + ~22:30 SA NRC guidance + ~23:00 sniff-log extractors.** 22 commits landed on `origin/main` (range `0ccf9e0..0e5cddc`): the 6 from yesterday's SSM-on-CAN bundle that had been sitting unpushed + 7 from today's transport hardening / SA plug-in / sniff toolchain / icon / handoff / rdbi cycle + 3 evening-docs commits + 2 handoff refreshes + 2 commits scaffolding `tools/solve_ssmcan1.py` (algorithm + Z3 solver + C++ emitter + linearity-check) + 1 SA NRC recovery-guidance commit (`reject_if_sa_negative` translating 0x33/0x35/0x36/0x37 to actionable text) + 1 three-extractor commit (sniff-log Workflows 1/2/3 from docs/24). Tree clean. https://github.com/BuffJesus/SubuwuTuner

**Two next-action paths, parallelizable:**

1. **Pre-Y-cable, anytime the user plugs in**: run `subuwutuner-cli rdbi --transport obdx --device COMx --did 0xF190 --verbose`. Multi-frame ISO-TP RX validation — if the VIN comes back clean, the entire UDS+ISO-TP stack is hardware-validated end-to-end. If garbled, we know precisely what to fix.

2. **When the Y-cable arrives** (Vgate B015649DHA ordered, arrives 2026-05-24): SA capture flow per `docs/23-security-access.md`:

   ```sh
   subuwutuner-cli sniff --transport obdx --device COM3 \
       --output capture.log --filter 0x7E0,0x7E8
   # In parallel: 10-15 COBB AP power cycles OR plug/unplug from Y-cable
   # Ctrl+C the sniffer when done
   python tools/extract_subaru_sa.py capture.log --output pairs.json
   ```

   `pairs.json` feeds straight into `python tools/solve_ssmcan1.py solve pairs.json --output ssmcan1.cpp` (scaffold shipped tonight, see new "## Continued evening" section below). Most likely tomorrow's first solve returns UNSAT — the encoded algorithm structure is provably GF(2)-linear, which the empirical finding under `## Continued evening` argues real SSMCAN1 cannot be. UNSAT is itself the useful signal: it tells us which structural assumption to refine (probable culprit: KeyPartsTable indexed by state bits S-box-style, not XOR'd in positionally).

## What landed today (7 commits, all pushed)

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

### 5. HANDOFF refresh

Previous in-session handoff. Captured the bundle as planned. (This file
is one more refresh on top of that — see end-of-section commit `a84843f`.)

### 6. rdbi CLI subcommand (post-handoff add)

User asked whether a dump/read attempt before the Y-cable arrives would
be useful. Highest-value pre-Y-cable test: RDBI for the VIN DID (0xF190)
— 17 ASCII bytes + 3-byte header = 20 bytes total, exceeds CAN single-
frame, forces multi-frame ISO-TP RX. That validates the
`strip_iso_tp` First-Frame path (PCI `0x1XYY` → strip 2) which ships
but has never been exercised against real ECU + real OBDX firmware.

- `cmd_rdbi` in `src/cli/main.cpp` — ~200 LoC, mirrors `cmd_rom_pull`
  shape (real-transport only, no trace replay)
- Args: `--transport`, `--device`, `--dll`, `--did <16-bit hex>`,
  optional `--output FILE.bin`, `--no-dsc` (default enters extended
  session), `--verbose` (toggles obdx trace)
- Output: hex dump + ASCII rendering side-by-side, so VIN-style
  responses are immediately recognizable while non-printable responses
  don't trash the terminal
- Useful-DID inline reference in the help text (0xF190 VIN, 0xF188 cal
  ID, 0xF195 SW version, etc.)
- No new tests — thin shim over already-tested UDS RDBI builders

### 7. HANDOFF refresh (this file, post-push)

## Continued evening: SSMCAN1 solver scaffold + structural-degeneracy finding (3 commits)

Pre-Y-cable scaffolding so tomorrow's capture is "feed real `pairs.json` in, get C++ source out" rather than "start writing the solver from scratch." Three commits.

### 8. HANDOFF mid-evening fix (commit `71ecb8a`)

Surgical patches to this file for the docs-push delta. No code changes.

### 9. `tools/solve_ssmcan1.py` scaffold (commit `0fd6d21`)

Clean-room implementation of the algorithm (16-round XOR cipher per public structural docs), synthetic generator + verifier, Z3-symbolic solver with ambiguity detection, C++ emitter producing a fork-namespaced drop-in source file matching `st::ecu::SecurityKeyFn`. Three CLI subcommands: `generate` / `verify` / `solve`. Z3 is lazy-imported (only required by `solve`); `generate` and `verify` work standalone. Tests in `tools/tests/test_solve_ssmcan1.py` — establishes that directory as the home for non-defgen Python tests. Round-trip on synthetic data confirmed (Z3 recovers a 24-pair fixture's encryption in <1s).

The clean-room boundary held: algorithm encoded from a paragraph of public structural prose, no GPL source consulted. Emitted C++ leaves SPDX as `<fork-author-choose>` — correct, since the fork's license choice depends on what other sources they integrate.

### 10. Linearity diagnostic + EMPIRICAL FINDING docstring (commit `ae6cf7d`)

200-pair smoke test on commit `0fd6d21` exposed severe structural degeneracy — recovered tables differ from ground truth in 30/32 IKB bytes and 32/32 KPT bytes despite the solver self-verifying against all 200 captured pairs. Diagnosed: `ssmcan1_encrypt` as encoded is provably GF(2)-linear in (seed, tables), confirmed empirically via `encrypt(s, T) == encrypt(s, 0) XOR encrypt(0, T)` holding for every test seed.

Consequence: the 512-bit table space maps to AT MOST 32 bits of effective constant. ~480 bits are mathematically invisible — multiple table-sets produce the identical encryption function across all 2^32 seeds. No number of pairs can fix this; it's structural.

Added:
- `EMPIRICAL FINDING (2026-05-23)` section in the script docstring documenting the linearity result, the 200-pair numbers, and the S-box-indexing hypothesis for refinement
- `check_linearity()` + `LinearityResult` dataclass (reusable API)
- `linearity-check` CLI subcommand (diagnostic, always exits 0)
- 3 tests in `TestLinearityCheck` with an explicit expectation-reversal note for when the algorithm is eventually refined to be nonlinear
- ASCII hyphens in the printed RESULT (em-dashes mangle under Windows cp1252)

**What this means for tomorrow's capture:** with high probability the real-data `solve` returns UNSAT against the current encoded structure. Don't read that as "the tooling is broken"; read it as "the encoded structure is wrong, refine it." The most likely refinement is adding S-box-style table indexing (where state bits select which table entry gets XOR'd, making table content observable in the output). That refinement is gated on real data — guessing further from prose alone would compound speculation.

## Continued late evening: pre-Y-cable polish (2 commits)

Both commits close pre-Y-cable follow-up items so tomorrow's first capture has the most ergonomic experience possible.

### 11. SA NRC named guidance — closes follow-up #5 (commit `7e03771`)

The bare `UDS NRC=0x36` propagated by `reject_if_negative` is opaque mid-stuck-SA session. Added `reject_if_sa_negative` helper in `src/ecu/src/uds.cpp` that translates the four SA-relevant NRCs (0x33/0x35/0x36/0x37) to named + actionable messages. NRC 0x36 in particular now reads:

> `UDS NRC=0x36 (exceededNumberOfAttempts) - ECU has locked SecurityAccess after too many bad-key attempts. To reset: power-cycle the ECU (ignition off, wait 10s, ignition on) OR wait ~10 minutes for automatic unlock.`

NRC 0x35 also warns about the lockout that follows ~3 cumulative bad keys (citing ISO 14229 §10.4.2.3 so the figure isn't arbitrary). Generic `reject_if_negative` stays untouched — SA-specific guidance lives only in SA parsers. Both `parse_security_access_seed` and `parse_security_access_key_ack` rewired to the new helper. 4 new unit tests in `test_uds.cpp` covering all four guidance cases, 1 new Flasher-level integration test in `test_flash.cpp` confirming the recovery message survives propagation through the SA preamble. Test assertions use substring keyword anchors so the wording can be tuned without breaking suite. **923/923 C++ green** (+4 from 919).

### 12. Three sniff-log extractors — closes follow-up #4 (commit `0e5cddc`)

(Commit message says "#5" but it closes #4 — the three extractors `docs/24-sniff-workflows.md` had been promising as TODOs. The previous commit `7e03771` was the actual #5. Cosmetic typo, no impact.)

Three Python scripts in `tools/`:
- `extract_uds_transfer.py` — Workflow 1, sniff-during-flash payload recovery. Pairs RequestDownload (0x34) with TransferData (0x36), tracks blockSequenceCounter wrap (0xFF→0x00), emits per-transfer `.bin` payloads + manifest JSON.
- `extract_rmba_polls.py` — Workflow 2, datalogger RAM-address discovery. Pairs RMBA/RDBI requests with responses, aggregates per-(kind,address) stats. Defensive handling for NRC 0x78 ("still working, wait") so a busy ECU doesn't make us drop the in-flight request.
- `decode_uds_capture.py` — Workflow 3, protocol learning. Walks the log producing a human-readable timeline with named ISO 14229 SIDs, sub-functions, and NRCs. Text or `--json` output. Anomaly section at the end flags uncatalogued SIDs/NRCs.

Shared infrastructure in `tools/sniff_common.py`:
- `Frame` + `parse_log` (SubuwuTuner sniff-log format reader)
- `IsoTpReassembler` with full SF / standard FF / **escape FF** (FF_DL=0 with 32-bit length per ISO 15765-2 — important for COBB ProTuner blocks >4095 bytes) / CF / per-CAN-ID concurrent state. Out-of-sequence CFs drop the in-flight message rather than emit corrupted bytes.

Schema names match `docs/24` exactly: `subuwutuner.flash.v1`, `subuwutuner.poll.v1`. Workflow 3 uses a new `subuwutuner.uds_timeline.v1`. **46/46 Python green** (+30 from 16).

## Test state: **923/923 C++ green** + **46/46 Python green** (`tools/tests/`)

C++ side: morning batch added +5 sniff transport tests + +4 SA preamble tests; commit `7e03771` adds +4 SA-NRC-guidance tests and tightens +2 existing. Python side: solver scaffold added 16 tests (`test_solve_ssmcan1.py`); extractor commit `0e5cddc` adds 30 more across `test_sniff_common.py`, `test_extract_uds_transfer.py`, `test_extract_rmba_polls.py`, `test_decode_uds_capture.py` (ISO-TP edge cases incl. escape-FF and out-of-sequence CF, RMBA/RDBI aggregation incl. NRC 0x78 handling, UDS timeline rendering, anomaly detection). Run with `python -m unittest discover -s tools/tests`.

## Binary artifacts (Syncthing-distributed)

- `subuwutuner-gui.exe` — 14.84 MB (icon + sniff + SA wiring), built 2026-05-23 18:23
- `subuwutuner-cli.exe` — 5.96 MB (new `sniff` + `rdbi` subcommands), built 2026-05-23 18:37

Both at `D:\Documents\JetBrains\SubaruTuner\build\win-mingw\bin\`. Syncthing should have propagated to the laptop.

## Commit log (pushed, `0ccf9e0..0e5cddc`)

In chronological push order — bottom to top is yesterday's bundle, top is tonight's late-evening polish:

```
0e5cddc feat(tools): three sniff-log extractors for docs/24 Workflows 1/2/3
7e03771 feat(ecu+flash): named NRCs + recovery guidance for SecurityAccess failures
613303a docs(handoff): refresh for solver scaffold + structural-degeneracy finding
─── (boundary: ~23:00 extractors push, ~22:30 SA NRC push above ~21:30 solver push) ──
ae6cf7d feat(tools): linearity-check subcommand + EMPIRICAL FINDING docstring
0fd6d21 feat(tools): SSMCAN1 solver scaffold + synthetic round-trip harness
71ecb8a docs(handoff): note evening docs push + Y-cable arrival date
─── (boundary: ~21:30 solver push above, ~20:00 docs push below) ────────
b1bbd8e docs(ai): goal-conditioned tuning coach as v2.1 composite
3e77b20 docs(sniff): clarify format-byte envelope caveat for sniff-during-flash
d192f4c docs(sniff): catalog non-SA Y-cable workflows
─── (boundary: ~20:00 docs push above, ~18:40 SA/sniff/rdbi push below) ──
c5602cb feat(cli): rdbi subcommand for ReadDataByIdentifier ground-truth
a84843f docs(handoff): refresh end-of-session state
0117495 feat(ui): SA authentication checkbox + program icon
45cf795 feat(cli+tools+docs): CAN sniff subcommand + SA capture toolchain
cf878ef feat(ecu+flash): UDS SecurityAccess plug-in architecture
9730be8 fix(transport+uds): OBDX CAN-ISO15765 hardening for VA WRX hardware
─── (boundary: yesterday's bundle below) ──────────────────────────────
20211ae docs(handoff): replace stale uncommitted-bundle prose with commit list
cf70a5f docs(handoff): refresh for end-of-session state
85d0fb5 docs: FA20→FA24 swap worked example + IR primitive notes
668a247 feat(fixtures): demo-pack writable_region + 4 FA24-themed hooks + 4 samples
d474d1d feat(feature_codegen): sqrt_float IR primitive (SH-2A FSQRT)
64e5693 feat(defs): validate flags duplicate hook/primitive/writable_region
19eb16c fix(transport+ecu+flash+log): SSM-on-CAN bare-payload framing
```

22 commits total. `origin/main` is now at `0e5cddc`. No more pending work waiting to push.

## Pre-existing untracked / leave-alone

- `SubaruTuner.zip` (120 MB, user-dropped backup)
- `definitions/legacy/.stfolder/` (Syncthing marker)
- `fixtures/projects/` (user's GUI-created test project "BigTittyGothGF")
- `tests/unit/ecu/test_ssm_properties.cpp` (still untracked from yesterday — promote to tracked when the SSM-on-CAN bundle commits land for real)

## Reference: Pre-Y-cable activities the user can run now

The Y-cable arrives 2026-05-24 (Vgate B015649DHA). Until then, the user can run these on the existing OBDX VX (no Y-cable needed) for additional validation. In suggested priority order:

1. **RDBI VIN read** (highest-value) — exercises an untested code path.

   ```sh
   subuwutuner-cli rdbi --transport obdx --device COMx --did 0xF190 --verbose
   ```

   UDS SID 0x22 DID 0xF1 0x90. Not gated behind SA. Returns 17-byte ASCII VIN which forces multi-frame ISO-TP RX (the OBDX transport's First-Frame strip path is shipped but never validated against real hardware). If the VIN comes back clean, our entire UDS+ISO-TP stack is hardware-validated end-to-end. If garbled (extra bytes, `?` characters, wrong length), we have a precise failure mode to fix.

   Quick follow-up: `--did 0xF188` for the calibration ID (smaller response, single-frame — should work even if VIN doesn't). Currently flashed COBB cal ID would come back.

2. **SA-enabled Read ROM** with Authenticate checkbox ON. GUI → Tools → Read ROM, leave Authenticate checked (default ON), Size = 0x100, click Read. Will fail with `NotImplemented` at the key step but the trace shows:

   ```
   [trace][obdx-tx] 27 01
   [trace][obdx-rx] 67 01 NN NN NN NN   ← real ECU seed bytes captured
   [err][read-rom] flash: read_full_rom SecurityAccess key derivation
                   failed: subaru security key stub — algorithm not
                   provided. See src/ecu/include/st/ecu/subaru_security.hpp...
   ```

   Confirms the new SA plumbing reaches the wire correctly AND gives us one real seed value from the ECU on disk (a starting data point even before the Y-cable arrives). ~2 min.

3. **Address-range probing** — `subuwutuner-cli rom-pull --addr 0x080000 --size 0x10 ...` to see if any cal region is readable without SA. Low probability of success but quick to try.

## Reference: SSMCAN1 algorithm structure (publicly documented)

Per `fenugrec/nisprog/SubaruSIDs.txt` (GPL-3 — read for facts, don't lift code):

- 16-round XOR cipher
- Two lookup tables: `IndexKeyBase[16]` of 2-byte values (32 B) + `KeyPartsTable[32]` of 1-byte values (32 B) — **64 bytes total**
- 3-bit barrel-roll right per round
- Final top/bottom byte swap on the 4-byte output

`tools/solve_ssmcan1.py` (commit `0fd6d21`) encodes one plausible per-round operation order matching this structural description. The script's `EMPIRICAL FINDING` docstring section (commit `ae6cf7d`) documents the catch: the encoded operation order is GF(2)-linear, so as encoded, ~480 of the 512 table bits are mathematically invisible. The script's `linearity-check` subcommand is the diagnostic. Tomorrow's first real-data `solve` against this encoding most likely returns UNSAT, which signals the structure needs refinement — the leading hypothesis is S-box-style indexing of `KeyPartsTable` by state bits rather than positional XOR. See the script's docstring for the full reasoning. Once the algorithm structure is refined to be nonlinear, 16+ pairs should over-determine the 512-bit table space; until then, no number of pairs will recover unique tables.

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

1. ~~**`tools/solve_ssmcan1.py`** — algorithm-solver.~~ **Done** (commit `0fd6d21`); scaffold encoded one plausible structure but the encoding is GF(2)-linear, so post-capture refinement is needed — see commit `ae6cf7d`'s EMPIRICAL FINDING. Algorithm-structure refinement is now the gated item; wait for real data.
2. ~~**`subuwutuner-cli rdbi --did <hex>`**.~~ **Done** (commit `c5602cb`).
3. **CY1 AES implementation** — `jglim/UnlockECU/SubaruSecurityAccess2018CY1.cs` is MIT-licensed, can be re-implemented in `subaru_security.cpp` without contamination. Targets 2018+ Subarus, not the dev's 2017 — defer until Path B for VB packs.
4. ~~**Three missing extractors from `docs/24`**~~ **Done** (commit `0e5cddc`); `tools/extract_uds_transfer.py` / `extract_rmba_polls.py` / `decode_uds_capture.py` shipped with shared `sniff_common.py` (incl. ISO-TP escape-FF for large blocks). 46/46 Python tests green.
5. ~~**NRC 0x36 (exceededNumberOfAttempts) error message + test**~~ **Done** (commit `7e03771`); `reject_if_sa_negative` translates 0x33/0x35/0x36/0x37 to actionable messages with recovery guidance. 923/923 C++ tests green.
6. **Algorithm-structure refinement** for `tools/solve_ssmcan1.py` — gated on real-capture data per the EMPIRICAL FINDING in commit `ae6cf7d`. Leading hypothesis is S-box-style indexing of `KeyPartsTable` by state bits rather than positional XOR. Don't refine speculatively; wait for first real `pairs.json`.
7. **Ship blocker #10**: CI performance gate.
8. **Ship blocker #7**: Frozen `defgen` binary.
