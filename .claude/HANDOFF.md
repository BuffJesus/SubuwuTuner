# Handoff — 2026-05-22 evening (3 v1.0 blockers closed, OBDX transport now spec-correct, live retest in progress)

**Tomorrow-first action: continue the OBDX live read flow.** The user has the adapter plugged into a 2017 WRX, the laptop pulled the rebuilt binaries via Syncthing, and is iterating on Tools → Read ROM from Car. Two OBDX bugs were diagnosed and fixed today against the published VT v1.06 PDF; the next failure mode (if any) is most likely UDS-layer or CAN-filter-related, NOT a repeat of the SetProtocol / RxSmall issues we already crushed.

If you're resuming this session and the user comes back with another OBDX error: the GUI now mirrors `read-rom` modal errors to the spawned console as `[err][read-rom] <text>`, so they can paste the exact string instead of re-typing from a screenshot.

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
| `send_recv: timeout / no response from ECU` | TX ack came back but no 0x08 push. Almost certainly the OBDX needs a CAN filter set for the ECU's response ID. The VT manual covers 0x33 for VPW only — VX adds CAN sub-commands we don't have a public spec for. | Speculative: add a `dvi::Opcode 0x33` filter command setting accept-id = 0x7E8 before phase 2 of `send_recv`. Don't guess the byte layout — pull the VX manual or iterate. |
| `expected unsolicited RxSmall/Large (0x08/0x09); got opcode 0x??` | Adapter returned something we don't expect. Read the opcode value. | Look at VT v1.06 §3 for the matching opcode; might be a config error we need to handle. |

The OBDX VX-specific reference manual is NOT publicly available at obdxpro.com without an account; the VT (VPW-only) manual at https://obdxpro.com/Downloads/ReferenceManuals/OBDX%20Pro%20VT%20Reference%20Guide%20v2.pdf is what we used today. The VX likely uses the same opcode set with additional CAN-specific sub-commands on 0x31 (already cracked) and 0x33 (filters — not yet exercised).

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

If OBDX read fails as "TX ack but no response" (filter issue):
1. Need the VX manual's 0x33 sub-command catalog for CAN.
2. Without it: iterate blind, but each iteration is a real-hardware run, not cheap.
3. Worth asking OBDX support directly via their contact page — they're responsive to developer requests.

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
