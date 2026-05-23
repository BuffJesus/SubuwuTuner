# Handoff — 2026-05-23 (SSM-on-CAN framing bug found and fixed; awaiting Read ROM retest)

**Next action: when the user retests, ask for the new `[trace][obdx-tx]` + `[trace][obdx-rx]` lines.** Expected new shape:

```
[trace][obdx-tx] A8 00 00 00 00 00 00 01 ... 00 4F  (~242 B, NO 80/10/F0/LEN prefix, NO CSUM suffix)
[trace][obdx-rx] E8 <byte_0> <byte_1> ... <byte_79>  (~81 B)
```

If the trace still shows `80 10 F0 F2 A8 ...` on the TX line, the user is running the **old** GUI binary — they need to transfer the freshly-built `subuwutuner-gui.exe` from `build/win-mingw/bin/` to the laptop (Syncthing handles this at home; they were planning a manual transfer for next-time on the work-network).

## Root cause and fix (resolved this session)

The prior session's `80 10 F0 F2 A8 ... 4F 72` (247 B) trace was diagnosed: Subaru SSM-over-CAN **strips the K-Line/serial wrapper**. The ECU on CAN ID 0x7E0 expects only the bare SSM payload — `A8 00 + 24-bit addresses` — with ISO-TP carrying length, CAN IDs carrying addressing, and CAN's own frame CRC superseding the SSM checksum. We were emitting the full K-Line frame (`80 10 F0 LEN A8 00 ... CSUM`) as the ISO-TP payload; the ECU saw `0x80` as a leading byte instead of the expected `0xA8`, didn't recognize the request, and silently dropped the First Frame without sending Flow Control. Adapter parked → 750 ms TX deadline expired.

Public sources (Autosport Labs SSM-over-OBDII forum thread, src0x/LibSSM2 README): wire example for a single-byte read at 0x000008 is `00 00 07 E0 A8 00 00 00 08` — CAN ID 0x7E0 + 5 bytes data, no wrapper.

The fix is a `Framing` enum threaded through SSM:

- `st::ecu::ssm::Framing::KLine` (default) — full wrapper, what ISO 9141 / Tactrix OpenPort needs.
- `st::ecu::ssm::Framing::IsoTp` — bare payload (`A8 00 + addrs` / response `E8 + data`), what every 2008+ Subaru on HS CAN needs.

Threaded through: `build_a8_request` / `parse_a8_response` / `build_b0_request` / `parse_b0_response` / `build_b8_request` / `parse_b8_response` / `SsmClient` ctor / `Flasher` ctor / `LogSession` ctor. UI selects `IsoTp` for real-hardware SSM mode; trace mode and CLI replay keep `KLine` so existing fixtures replay unchanged.

## Test state: **909/909 green**

- +10 new ISO-TP unit tests in `tests/unit/ecu/test_ssm.cpp` covering build/parse for A8 / B0 / B8 IsoTp + SsmClient IsoTp round-trip.
- +1 new Flasher integration test in `tests/unit/flash/test_flash.cpp` exercising bare-payload `read_full_rom_ssm` end-to-end.
- +1 new LogSession IsoTp test in `tests/unit/log/test_log.cpp`.
- The previously-failing `tests/unit/ecu/test_ssm_properties.cpp:191` (B0 size assertion off by one — author forgot the data byte slot) is **fixed**. File is still untracked in git; promote to tracked when committing the rest of the bundle.
- +4 `Definition::validate` duplicate-name tests in `tests/unit/defs/test_defs.cpp` covering hook / primitive / writable_region.
- +3 OBDX open() validation tests in `tests/unit/transport/test_obdx_transport.cpp` covering EnableNetwork state byte (ON / OFF / LISTEN-ONLY) and sub-op echo enforcement. Existing open() tests updated to mock realistic `02 01` shape.
- +2 Flasher CC-restore-on-bail tests in `tests/unit/flash/test_flash.cpp`; one existing NRC-on-RequestDownload test updated to expect the new CC restore and assert `restored_bus`.

## Pre-emptive LogSession framing plumb

`src/log/src/log.cpp::io_loop` constructs an internal `SsmClient` per session. With the bare default it would hit the **same** silent-drop bug on real CAN hardware during datalogging. `LogSession` ctor now takes a `Framing` parameter (default `KLine` keeps existing tests/CLI replay working). When the GUI eventually wires up live datalogging against the OBDX, the call site picks `IsoTp` from `LinkConfig::kind` — same one-liner as the Flasher path.

## Side work this session (not OBDX-related)

- **Docs:** `docs/16-custom-features.md` got a new "Motivating use cases" section with the FA20→FA24 swap into a VA WRX as a worked example. Distinguishes the cam-angle/VVT remap (custom feature) from the HPFP / VE / injector / knock work (table edits). Cites public sources only. `docs/04-roadmap.md` engine-swapper persona row updated to reference the worked example. Triggered by the user mentioning Atlas's role in FA24 swaps; no code change.

## Quick file map for this session's changes (additive on top of yesterday's bundle)

Today only:
- `src/ecu/include/st/ecu/ssm.hpp` — `Framing` enum + `framing` param threaded through builders/parsers; `SsmClient` ctor + `framing()` accessor.
- `src/ecu/src/ssm.cpp` — each builder/parser branches on framing; IsoTp paths emit/expect bare payloads.
- `src/flash/include/st/flash.hpp` — `Flasher` ctor takes `ssm_framing` (default KLine).
- `src/flash/src/flash.cpp` — bail() now attempts CC restore best-effort when the bus was silenced (mirrors cancel-cleanup; closes the non-cancel failure-path gap).
- `src/log/include/st/log.hpp` — `LogSession` ctor takes `ssm_framing` (default KLine); new private member.
- `src/log/src/log.cpp` — ctor stores framing, `io_loop` passes it to internal `SsmClient`.
- `src/transport/src/obdx_transport.cpp` — EnableNetwork response strict validation: sub-op echo must be 0x02, state must be ON or LISTEN-ONLY (warn-on-listen). The verbose-only warning was lifted to a hard open() failure.
- `src/defs/src/defs.cpp` — `Definition::validate()` now flags duplicate hook / primitive / writable_region names. `<unordered_set>` added.
- `src/ui/src/main.cpp` — Read ROM modal picks `IsoTp` for real-hardware SSM, `KLine` for trace mode.
- `tests/unit/_helpers/erase_opt.hpp` — new shared helper consolidating two duplicate definitions across `test_flash.cpp` and `test_cancellation_invariants.cpp`.
- `tests/unit/ecu/test_ssm.cpp` — +10 ISO-TP framing test cases.
- `tests/unit/flash/test_flash.cpp` — +1 Flasher IsoTp integration test; +2 CC-restore-on-bail tests; existing NRC test updated to expect CC restore; switched to shared `erase_opt` helper.
- `tests/unit/flash/test_cancellation_invariants.cpp` — switched to shared `erase_opt` helper.
- `tests/unit/log/test_log.cpp` — +1 LogSession IsoTp test.
- `tests/unit/ecu/test_ssm_properties.cpp` — fixed B0 size assertion (was 9, is 10); added data-byte index check.
- `tests/unit/defs/test_defs.cpp` — +4 duplicate-name validate tests (hook, primitive, writable_region, happy-path sanity).
- `tests/unit/transport/test_obdx_transport.cpp` — +3 EnableNetwork strict-validation tests; existing handshake tests updated to mock realistic `02 01` responses.
- `docs/16-custom-features.md` — "Motivating use cases" section + FA20→FA24 worked example. Subsequent revision corrected the technical picture (cam-trigger-wheel issue is hardware-solved by RS Motors kit; HPFP capacity is the actual software-relevant concern). Added working-project table (Prime Motoring 502whp, Six Star SPF 450whp/E85, two YouTube series), tuning-path matrix (COBB / Atlas / EcuTek / Link), "Atlas custom features make cams work" claim resolved (Path A hardware kit vs Path B software cam-signal interpretation), and pointer to `sqrt_float` as the next natural IR primitive for Bernoulli-based fuel-pressure correction.
- `docs/04-roadmap.md` — engine-swapper persona row references the worked example.
- `fixtures/demo-pack/pack.toml` — added two `[[writable_region]]` entries. **Independent bugfix** the FA24 work surfaced: before this, none of the bundled sample `.stmod` files (clutch-kill, flat-foot-shift, launch-control, map-selector-int) compiled against the demo-pack because the codegen address gate was failing closed.
- `fixtures/demo-pack/hooks.toml` — added `override_vvt_target` (generic VVT value-override hook), `override_hpfp_target` (HPFP commanded-target override), `read_aux_fuel_pressure` (3rd-party CAN-bus fuel pressure sensor read).
- `fixtures/samples/vvt-override-demo.stmod` (new) — generic VVT-override pattern demo (initially scaffolded as the FA24 fix before research clarified the cam-side concern is hardware-solved).
- `fixtures/samples/fa24-hpfp-clamp.stmod` (new) — runtime clamp of FA20 ECU's commanded HPFP target to a hard ceiling the FA24 stock pump can hit. Compiles to 76 bytes SH-2A.
- `fixtures/samples/fa24-aux-pressure-clamp.stmod` (new) — same clamp shape but threshold driven by a live 3rd-party CAN-bus fuel pressure sensor reading. Mirrors COBB's Differential Fuel Pressure Compensation pattern + Atlas's 3rd-party-sensor-integration pattern. Compiles to 76 bytes SH-2A.
- **All seven `.stmod` samples in `fixtures/samples/` now compile end-to-end against `fixtures/demo-pack` via the SH-2A backend.** Previously: zero. (Updated: now eight samples, with `fa24-bernoulli-comp.stmod` added once `sqrt_float` landed — see below. `flex-fuel.stmod` remains blocked on the curve / table-lookup primitive.)
- **`sqrt_float` IR primitive shipped end-to-end:**
   - `src/feature_codegen/src/sh2a.hpp` — `enc_fsqrt(FReg frn)` returning `0xF06D | (n << 8)` per the SH-2A FSQRT spec
   - `src/feature_codegen/src/feature_codegen.cpp` — `FragmentEmitter::fsqrt`, `emit_sqrt_float_fragment`, dispatch case, primitive_shape entry, updated error-message strings
   - `src/feature/src/feature_ir.cpp` — `sqrt_float` cycle cost added (15, between FDIV's 18 and FMUL's 5; FSQRT latency dominates)
   - `tests/unit/feature_codegen/test_sh2a.cpp` — +2 tests (FSQRT encoding emitted + wrong-arity rejection)
   - `fixtures/samples/fa24-bernoulli-comp.stmod` — new sample exercising `divide_float → sqrt_float → multiply_float` for differential fuel pressure correction; targets `override_injector_pw` (compiles to 108 bytes of SH-2A)
   - `docs/16-custom-features.md` — `sqrt_float` added to the SH-2A primitive coverage table; the "next IR primitive" subsection rewritten to describe what shipped + what remains
- **`override_injector_pw` hook added to demo-pack:** Bernoulli compensation properly belongs on the injector pulse-width side (preserves OEM AFR closed-loop; HPFP-target compensation would just fight the OEM PID). The new hook takes commanded PW + RPM + load + manifold pressure + commanded rail pressure as inputs, returns an overridden PW. `fa24-bernoulli-comp.stmod` was refactored to use it correctly (no more passthrough-hook gymnastics).
- **`fa24-bernoulli-comp.stmod` upgraded to production-shape math:** added manifold-pressure-aware ΔP (uses `rail − manifold` for both pressures — the physically-correct quantity per Bernoulli's principle) and bilateral correction-factor clamping (`[0.8, 1.4]` via two `compare/select` pairs). Now 11 primitive nodes, compiles to 328 bytes of SH-2A with the IEEE 754 constants for the clamp thresholds (`3F 4C CC CD` = 0.8, `3F B3 33 33` = 1.4) baked into the literal pool. `docs/16` "remaining gaps" list updated — only curve primitive, CAN-RX hook, and low-pass filter primitive remain on the FA24 follow-up list.

Inherited from prior session, still uncommitted:
- `src/flash/include/st/flash.hpp`, `src/flash/src/flash.cpp` — `read_full_rom_ssm`.
- `src/transport/include/st/transport/obdx_transport.hpp`, `src/transport/src/obdx_transport.cpp` — open() instrumentation, EnableNetwork ACK validation, 50/50 TX budget split, `obdx_dvi.hpp` carries the codec the new property tests cover.
- `src/ui/src/main.cpp` — Protocol dropdown, Verbose checkbox, RAII trace-guard, yellow preflight notice.
- `tests/CMakeLists.txt`, `docs/09-risks.md` (cosmetic IDE-open edit), `docs/13-transport.md`.

## Uncommitted bundle — commit cadence reminder

The whole stack above is in the working tree but **not committed**. Per `feedback_caveman_cadence.md`, the gate before any `git push` is:

- `/caveman-review` over the diff
- `/caveman-commit` for the message

The natural split is **3–5 commits** rather than one. From narrow to broad scope:

1. **`fix(transport+ecu+flash+log): SSM-on-CAN framing — bare payload, no K-Line wrapper`** — the main SSM fix end-to-end: framing enum, all consumers, all tests. The B0 size assertion fix in `test_ssm_properties.cpp` rides here. Includes strict EnableNetwork validation in `obdx_transport.cpp::open()` since it's the same module and same incident class. About 14 files.
2. **`fix(flash): restore CC on non-cancel failure bail`** — independent flash-module change closing the gap flagged in `ddece0f` review. ~2 files.
3. **`feat(defs): validate flags duplicate hook/primitive/writable_region names`** — independent defs-module hardening. ~2 files.
4. **`refactor(tests): extract shared erase_opt helper to _helpers/`** — purely refactor, no behavior change. 3 files (new helper + 2 call sites).
5. **`docs(custom-features): FA20→FA24 swap motivating example + roadmap cross-ref`** — doc-only. 2 files.

Acceptable to bundle 2+3+4 into a single "follow-up cleanup" commit if the granularity feels excessive; the SSM fix and the docs commit are the two that should stay separate.

Status of unstaged files right now (`git status` snapshot):
```
M .claude/HANDOFF.md                                ← this file
M docs/04-roadmap.md                                ← FA24-swap persona row
M docs/09-risks.md                                  (cosmetic IDE-open edit; do not commit)
M docs/13-transport.md                              ← VT v1.06 §3.11 row clarified (single-line edit)
M docs/16-custom-features.md                        ← Motivating use cases + FA24 example
M src/defs/src/defs.cpp                             ← validate() duplicate-name check
M src/ecu/include/st/ecu/ssm.hpp                    ← Framing enum
M src/ecu/src/ssm.cpp                               ← IsoTp build/parse paths
M src/flash/include/st/flash.hpp
M src/flash/src/flash.cpp                           ← bail() CC restore
M src/log/include/st/log.hpp                        ← LogSession framing
M src/log/src/log.cpp                               ← LogSession framing
M src/transport/include/st/transport/obdx_dvi.hpp
M src/transport/include/st/transport/obdx_transport.hpp
M src/transport/src/obdx_transport.cpp              ← strict EnableNetwork validation
M src/ui/src/main.cpp
M tests/CMakeLists.txt
M tests/unit/defs/test_defs.cpp                     ← +4 duplicate-name validate tests
M tests/unit/ecu/test_ssm.cpp                       ← +10 IsoTp tests
M tests/unit/flash/test_cancellation_invariants.cpp ← uses shared erase_opt helper
M tests/unit/flash/test_flash.cpp                   ← +1 Flasher IsoTp, +2 CC-restore tests
M tests/unit/log/test_log.cpp                       ← +1 LogSession IsoTp test
M tests/unit/transport/test_obdx_transport.cpp      ← +3 EnableNetwork strict tests; existing tests use realistic 02 01
M tests/unit/feature_codegen/test_sh2a.cpp          ← +2 sqrt_float tests (FSQRT encoding, wrong-arity rejection)
M src/feature/src/feature_ir.cpp                    ← sqrt_float cycle cost (15)
M src/feature_codegen/src/feature_codegen.cpp       ← sqrt_float emit + dispatch + primitive_shape entry
M src/feature_codegen/src/sh2a.hpp                  ← enc_fsqrt encoding helper
M fixtures/demo-pack/hooks.toml                     ← +4 hooks (override_vvt_target, override_hpfp_target, override_injector_pw, read_aux_fuel_pressure)
M fixtures/demo-pack/pack.toml                      ← +2 writable_region entries (unblocks all bundled samples)
?? SubaruTuner.zip                                  (leave; per prior handoff)
?? definitions/legacy/.stfolder/                    (Syncthing marker)
?? fixtures/projects/                               (personal test project, leave)
?? fixtures/samples/vvt-override-demo.stmod         ← new sample (generic VVT-override pattern demo)
?? fixtures/samples/fa24-hpfp-clamp.stmod           ← new sample (HPFP clamp, hard ceiling)
?? fixtures/samples/fa24-aux-pressure-clamp.stmod   ← new sample (HPFP clamp, 3rd-party CAN sensor driven)
?? fixtures/samples/fa24-bernoulli-comp.stmod       ← new sample (Bernoulli diff-fuel-pressure correction, exercises sqrt_float)
?? tests/unit/_helpers/erase_opt.hpp                ← new shared test helper
?? tests/unit/ecu/test_ssm_properties.cpp           (now correct; promote to tracked on commit)
```

## If the retest succeeds

1. User gets a real ROM dump on disk → File → New project flow → first real-hardware end-to-end milestone. Phase 1 ship gate (≥20 maps from a real definition pack) becomes testable.
2. Memory entry `project_tuner_supplied_encrypted_rom.md` gets a follow-up note that the OBDX path produces the **tuned** cal (not stock — that's still locked in COBB AccessPort's encrypted store).
3. Commit the bundle per the cadence above; promote `test_ssm_properties.cpp` to tracked.

## If the retest still fails

The framing fix eliminated the most likely cause. New failure modes and where to look:

| `[trace][obdx-tx]` shape | Diagnosis | Next step |
|---|---|---|
| Still `80 10 F0 F2 A8 ...` | User is running the **old** binary on the laptop. | Confirm they transferred `subuwutuner-gui.exe` from today's build (timestamp 2026-05-23 ~08:04). |
| `A8 00 00 00 00 ...` (correct) but TX still times out | ECU isn't on CAN ID 0x7E0 for SSM (or ignition isn't actually in RUN, or wiring problem). Bus is fundamentally silent. | Bus sniffer to confirm whether the ECU IS responding; if yes, the CAN ID is wrong. RR's source has the CAN IDs as a clean-room reference candidate (we can read RR public protocol *documentation* per CLAUDE.md, just not its Java source). |
| `A8 00 ...` and TX succeeds, but RX comes back with `7F NN` (negative response) | ECU answered with a NRC. Most likely 0x33 (securityAccessDenied) — the COBB tune may have locked the SSM read addresses. | Add a session-escalation preamble (DSC + SecurityAccess), or pick a different address range to read first to confirm baseline connectivity. |
| `A8 00 ...` and RX comes back with `E8 <bytes...>` | Working. Run the full 2 MB dump. | Continue per "If the retest succeeds" above. |

---

# Everything below this line is historical context from earlier sessions

The state described below was superseded by today's work, but the prose around motivation and decisions is still useful for future readers.

---

## What shipped today (9 commits, all on `origin/main`)

```
5b0ae89 fix(transport): receive OBDX frames as unsolicited push
3f9b27d feat(ui): mirror status + error messages to stderr
aaa1d69 fix(transport): correct OBDX SetProtocol payload per VT v1.06
1e9619f fix(ui): use gnu_printf archetype for text_subtle
39ee4a5 docs(updater): sketch st::updater design (Phase 6)
ddece0f feat(defs+codegen): add writable-region address gate          (#3 ✅)
1f4c5d9 feat(flash): execute() honors cancel between PDUs              (#2 ✅)
473b8f6 test(flash+ecu): pin cancellation + PDU-atomicity invariants
ccaca6d feat(cli): add 'doctor' triage subcommand                      (#6 ✅)
```

865/865 tests green throughout. Full-tree build clean on MinGW (the pre-existing `%zu` UI breakage is fixed in `1e9619f`).

### Ship-blocker grid

| # | Title | Status after today | Notes |
|---|---|---|---|
| 1 | Brick protection per-ISA | ⬜ hardware-blocked | Bench rig prerequisite |
| 2 | Cancellation invariants | ✅ | UDS path complete; SSM moot until v1.3 |
| 3 | Codegen writable-region gate | ✅ | Fail-closed, wired into Sh2aBackend |
| 4 | `[[table.role]]` schema | ✅ | PR #1 |
| 5 | `.stune` format spec | ✅ | PR #1 |
| 6 | `subuwutuner-cli doctor` | ✅ | Composes adapter probe + pack health + ROM CID |
| 7 | Frozen `defgen` binary | ⬜ packaging | PyInstaller / Nuitka choice pending |
| 8 | README platform matrix | ✅ | PR #1 |
| 9 | OFL font licenses | ✅ | PR #1 |
| 10 | CI performance gate | ⬜ | Aspirational thresholds, not enforced |
| 11 | Property-based codec tests | ⬜ | RapidCheck wire-up — next-up task in this session |

**Pure-software blockers remaining: #7, #10, #11.** #1 is hardware-blocked.

---

## OBDX live-test decision tree

Status as of session-end: two OBDX firmware-layer bugs fixed; user is mid-retest. If a third error appears, classify it against this table BEFORE assuming new code is needed.

| Console text (after `[err][read-rom]`) | Diagnosis | Where to look |
|---|---|---|
| `Adapter open failed: serial open failed for ...` | Wrong COM port. | Device Manager → Ports. |
| `device returned 0x05 for opcode 0x31` | Would mean my SetProtocol fix regressed. **Should not happen.** | `src/transport/src/obdx_transport.cpp::set_protocol_payload` — confirm 2-byte format. |
| `device returned 0x01 for opcode 0x08` | Would mean the RxSmall fix regressed. **Should not happen.** | `src/transport/src/obdx_transport.cpp::Transport::send_recv` phase 2. |
| `read_full_rom: short read at 0x...` | UDS layer: ECU rejected the chunk size or refused mid-read. Look at the actual bytes received. | `src/ecu/src/uds.cpp::read_memory_by_address`, `src/flash/src/flash.cpp::read_full_rom`. |
| `read_full_rom: ... ecu negative response 0x7F 0x23 NN` | ECU said no to ReadMemoryByAddress (SID 0x23). NN explains why; 0x33 = securityAccessDenied (needs session escalation + seed/key); 0x31 = requestOutOfRange (address invalid for this ECU); 0x12 = subFunctionNotSupported. | Add a DSC(extended) + SecurityAccess preamble per the ECU's needs, OR adjust addr/length. |
| `send_recv: timeout / no response from ECU` | TX ack came back but no 0x08 push. Most likely the ECU genuinely didn't respond (wrong ignition state, wiring, wrong protocol on this car). Per VT v1.06 §3.3, "by default … all filters are set to off … to monitor all messages, disable all filters and enable network" — so the receive path is *open* by default, not *closed*. A missing filter is unlikely to be the cause. | Check ignition is in ACC/RUN; confirm OBD-II port wired to the engine ECU; verify HS CAN (Subaru) and not MS CAN. Only chase the CAN filter angle if a bus sniffer shows the ECU IS responding but the adapter isn't pushing the frame to us. |
| `expected unsolicited RxSmall/Large (0x08/0x09); got opcode 0x??` | Adapter returned something we don't expect. Read the opcode value. | Look at VT v1.06 §3 for the matching opcode; might be a config error we need to handle. |

VT v1.06 PDF: https://obdxpro.com/Downloads/ReferenceManuals/OBDX%20Pro%20VT%20Reference%20Guide%20v2.pdf — the manual covers the full opcode catalog used by the VX, including HS CAN as protocol 0x02 under §3.10.1 SetProtocol (already in use) and the 0x33 filter sub-commands (§3.11.1–3.11.5). The section is titled "VPW Specific Settings" but sub-ops 0x00–0x04 are filter primitives (Set To Filter, Set From Filter, To/From Range Filter, Set Mask); sub-op 0x05 is unused; sub-ops 0x06–0x0F are VPW-scoped (4x speed, CRC, 1x/4x timings, error bits). The byte semantics of `MM` (1-byte filter ID) and `BB NN MM` (3-byte mask) are defined for VPW's 3-byte header; how those fields map to 11-bit / 29-bit CAN IDs on the VX is NOT documented in the VT PDF and would need either the VX manual (account-gated at obdxpro.com) or a clean-room read of `OBDXPro/OBDX-Templates` (C# samples, allowed per `docs/13-transport.md:209`).

---

## Background that's still load-bearing for tomorrow

### Syncthing desktop ↔ laptop

Installed and configured today. Two send-only folders (`build/win-mingw/bin/` and `definitions/legacy/`) mirror from desktop to laptop via Task Scheduler-spawned daemon. Replaces the old zip-and-send loop.

- Desktop device ID: `NSYHPXO-QVWHQT6-4XUNW2M-OET6FJY-CYP7Z7I-CM5HV7D-M7PEYEY-SIIJKQ4`
- Laptop device ID: `VZ6D4AZ-WUZL35M-UR5EOJ4-TPBMLFJ-6QOT7TR-R4BAHVR-5HBTKOH-3GHJ6AP`
- Web UI: http://127.0.0.1:8384/
- Full setup notes: `.claude/SYNCTHING-SETUP.md` (gitignored, lives on this machine only)
- Memory: see `project_syncthing_setup.md`

### Workflow

Desktop: edit + commit + `cmake --build build/win-mingw`. Laptop receives binaries within seconds via fs-watcher. No git pull or rebuild on the laptop is needed — Syncthing IS the propagation. The laptop is a test target, not a build host.

### Auto-updater (Phase 6 work)

`docs/22-auto-update.md` sketches the in-tool `Help → Check for Updates` flow that closes the v1.0 "Installer / codesigning / auto-update channel" row when Phase 6 polish starts. Channel model, GitHub-Releases manifest shape, Ed25519 signature verification, Windows helper-process swap pattern, UI flow — all there. Three open questions called out inline. Not for tomorrow; the file-sync above solves the dev-iteration problem in the meantime.

### Pre-existing notes still relevant

- The `fixtures/projects/Test/` untracked dir is the user's GUI-created test project ("BigTittyGothGF"). Leave alone — it's personal test data, not a repo asset.
- `SubaruTuner.zip` at the repo root (120 MB) is a backup the user dropped; HANDOFF history says "leave."
- `docs/09-risks.md` carries an unstaged two-blank-line edit from the user opening the file in their IDE. Cosmetic; don't include in commits.

---

## What's next after the OBDX live test settles

If OBDX read succeeds:
1. User gets a real ROM dump on disk → File → New project flow → first real-hardware end-to-end milestone for the project. Phase 1 ship gate (≥20 maps from a real definition pack) becomes testable.
2. Memory entry "COBB-encrypted 2017 WRX stock ROM" gets a follow-up note that the OBDX path produces the tuned cal (not stock — that's still locked in COBB AccessPort's encrypted store).

If OBDX read fails as a UDS-layer issue:
1. Likely needs session escalation (DSC 0x03 extendedDiagnostic) + possibly SecurityAccess before ReadMemoryByAddress on a tuned ECU. Forum threads on COBB-tuned VAs hint that the COBB tune may leave the security level partially open; that's why a dump is even attempted before seed/key implementation.
2. Add `Flasher::read_full_rom` an optional session-escalation preamble — or, simpler, expose DSC + SA primitives at the CLI/GUI level so the user can drive the escalation themselves.

If OBDX read fails as "TX ack but no response":
1. First rule out the boring causes: ignition position, wiring, protocol mismatch. The adapter's default is "all filters off, accept all" per VT v1.06 §3.3 — a missing filter is unlikely to be the root cause.
2. If a bus sniffer proves the ECU IS responding but the adapter is silent, then it's a CAN filter / acceptance issue and we need the VX-specific CAN byte semantics for the 0x33 family (sub-op catalog is in the VT PDF; CAN-ID mapping isn't). Options: (a) ask OBDX support, (b) clean-room read of `OBDXPro/OBDX-Templates`'s C# CAN example per `docs/13-transport.md:209`.

---

## Pure-software follow-ups (not OBDX-blocking)

In rough priority order:

1. **Ship blocker #11**: property-based codec tests. Up next in this session per user direction.
2. **Ship blocker #10**: CI performance gate. Cold-start time + idle-RAM thresholds in CMake; fail the matrix build on regression past §1 in `docs/05-improvements.md`.
3. **Ship blocker #7**: Frozen `defgen` binary. PyInstaller is fine, Nuitka is slimmer. Either, then bundle into the installer when that lands.
4. **`Definition::validate()` duplicate-name check** for `[[writable_region]]` entries. Flagged in `ddece0f` review; same applies to `[[hook]]` / `[[primitive]]` which also don't enforce uniqueness today.
5. **Shared test-helper header** at `tests/unit/_helpers/`. `erase_opt`, `dvi_response_frame`, `dvi_unsolicited_frame`, `make_def_with_regions` are now duplicated across 2-3 test files each. Mechanical extraction.
6. **`EnableNetwork` response echo validation** in `obdx_transport.cpp::open()` — flagged in `aaa1d69` review. Verify the adapter actually flipped to ON instead of taking the ACK on faith.
7. **`Flasher::execute` cancel-cleanup CC restore** is already done; the equivalent on the happy-path failure paths (e.g. mid-sector erase failure) could also restore CC. Minor.

---

# Earlier-today + previous-day handoffs (preserved for context)

Everything below this line is historical. The state described in those sections has been superseded by the work in today's 9 commits, but the prose around motivation / decisions is still useful for future readers.

---

# Handoff — 2026-05-22 morning (OBDX adapter on hand, K-Line default fixed)

**Tomorrow's first action: re-run the GUI Read flow against the real OBDX adapter.** The user got the OBDX Pro VX in the mail late on 2026-05-21, plugged it in, clicked Tools → Read ROM from Car (Adapter=OBDX, COM port set), and got:

```
Adapter link open failed: obdx::Transport: OBDX VX doesn't support
K-Line / ISO9141. Subaru VA WRX needs Tactrix OpenPort.
```

This is a real coding bug I shipped (and a misleading error message to boot). The OBDX **is** the right adapter for VA/VB WRX — those cars run CAN ISO15765, not K-Line. Subaru switched to CAN with the 2008 OBD-II CAN mandate. Atlas's recommendation of OBDX is correct.

**Fix landed at `f3b7cc7`** (HEAD):
- `LinkConfig` default changed: `kind=CanIso15765`, `baud=500000`, `can_id_request=0x7E0`, `can_id_response=0x7E8` (standard Subaru engine-ECU OBD-II addressing).
- New `kSubaruEngineCanIdRequest`/`Response` constants in `src/transport/include/st/transport.hpp`.
- `LinkKind` enum comments rewritten to reflect actual Subaru bus history (pre-2008 K-Line, 2008+ CAN ISO15765 — including all VA/VB).
- The OBDX K-Line error message rewritten to redirect users to `CanIso15765` instead of pointing at Tactrix.

This morning's first-action was completed mid-session — the user retested and hit the SetProtocol payload bug (then the RxSmall bug). Both fixed in today's commits. See the new top-of-file section for current state.
