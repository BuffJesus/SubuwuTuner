# 42 — Bench-rig validation runbook

> Picks up where `docs/28-bench-rig-build.md` ends. Assumes the rig is **assembled, powered, and has produced a first ROM read.** Sequences the hardware-side validation that retires the open hardware gates (`docs/04` ship-blockers + `docs/31` / `docs/40` brick-protection items + `docs/16` patch-insertion).

## Why this doc exists

`docs/28` is the assembly + first-boot doc. After that, the question is: **what gets validated first, and in what order, so each test produces signal that informs the next test**? This doc answers that — concrete sequence, expected pass criteria, what counts as "move on" vs "stop and investigate."

The implementer + analyst session pair runs this together. Most steps are CLI-driven; the rig only needs to produce ECU bytes for the host to verify against expected.

## Reading order before you start

The implementer side has shipped a lot since `docs/28` was last updated. Re-read these in this order before powering the rig:

1. **`docs/04-roadmap.md` §"Sequencing the open v1.1 items"** — current state of what's done vs what's gated.
2. **`docs/31-brick-protection-by-isa.md`** — per-ISA recovery model. Pre-loads the SH-2A boot-signature gate facts so the rig's reset behavior is predictable.
3. **`docs/40-delta-flash-brick-protection.md`** — v1.5 delta-flash extension; runbook below validates this AFTER the basic flash path lands.
4. **`docs/37-subaru-flash-protocol.md`** — clean-room sequence summary of the UDS flash flow. Authoritative for the byte-level expectations the runbook compares against.
5. **`docs/38-subaru-sa-variants.md`** — which SA variant matches which install state. Loadbearing for step 3 below.
6. **`findings/handoffs/HANDOFF-to-analyst-2026-06-13-ap-browser-threading.md`** — context for why the bench-rig path uses sync I/O; the AP browser's async refactor lands separately.

The analyst's most relevant RE staging lives at `findings/re-2026-06-12-pm/` (cmd dispatch table, COBB SA keys, flash protocol surface). Cross-reference those for step 4+ below.

## Step 0 — Prerequisites checklist

Before running anything: confirm the rig has these. Each is a hard gate.

- [ ] Bench harness produces a clean `ap3 state` against the rig's USB AP (if one is attached). If not, the AP path is separate from the rig path; skip AP checks below.
- [ ] OBDX Pro VX adapter on COM5 (or equivalent) recognized by `subuwutuner-cli transport-list`.
- [ ] The rig's ECU produces a stable read on `subuwutuner-cli rom-pull --transport obdx --addr 0x0 --size 0x10` — i.e. the first 16 bytes come back without error 3 times in a row. (Smoke test: the rig's UDS is reachable.)
- [ ] A known-good stock ROM dump of the same CID is on disk for byte-vs-byte verification. (If the rig is LF79103P, drop the user's `2017-LF79103P-AP-uninstalled-2026-05-24.bin` into `fixtures/private/` alongside other reference dumps.)
- [ ] Backup power: a USB-controlled relay (`docs/28` Phase 6) is the eventual brick-protection-test enabler, but you don't need it for the first ~10 steps below. The relay specifically gates step 5.
- [ ] Software: `subuwutuner-cli` built with `-DST_ENABLE_BULK_REFLASH_CIPHER=OFF` for the first pass. Turn it on only at step 6.

If any item above isn't true, **don't proceed**. Go back to `docs/28`.

## Step 1 — Full-ROM read-back

**Goal:** confirm the rig's bytes are stable and `rom-pull` produces output that matches the reference dump.

```bash
$CLI rom-pull --transport obdx --device COM5 \
  --addr 0x0 --size 0x200000 \
  --output /tmp/bench-rig-rom.bin
md5sum /tmp/bench-rig-rom.bin fixtures/private/<reference>.bin
```

**Pass:** MD5s match.

**Fail and investigate:** any mismatch. Common causes:
- Wrong CID — the rig is a different cal-ID than the reference. Use `rom-identify` to confirm.
- Aftermarket SA layer — the rig was tuned by someone before becoming a donor. Read `/backupcksum` (if AP attached) or compare the bootloader region (first 0x6000 bytes) to confirm stock vs aftermarket.
- Transient bus errors — RMBA chunk gaps. Re-pull. Per `project_fa20dit_rmba_chunk_cap` memory, FA20DIT caps RMBA at 0x800 — `rom-pull` already chunks correctly, but a flaky harness can drop bytes.

**Time budget:** 8-12 minutes (2 MB read at ~2.5 KB/s).

## Step 2 — Boot-signature verification (host-side check)

**Goal:** confirm the host-side `st::flash::verify_boot_signatures_sh2a_2mb` (`docs/31`) agrees with the bench rig's actual signature bytes.

```bash
$CLI rom-info --def <pack> /tmp/bench-rig-rom.bin
# Output should report all three signatures: 0x5555 @ 0x6000, 0xAAAA @ 0x1FFFF2,
# and byte-equality @ 0x6C ↔ 0x6010.
```

The implementer-side mirror has been pinned against the analyst's `findings/APP_CHECKSUM_VERIFICATION.md` evidence. If the host-side check disagrees with what's actually in the ROM, **the bug is host-side** (or this is a CID we haven't seen). Don't proceed to writes until resolved.

**Pass:** three signatures all pass; host-side check returns Ok.

## Step 3 — SecurityAccess against the rig

**Goal:** confirm one of the catalogued SA variants (`docs/38`) actually works against the rig's ECU.

The rig is presumably stock (junkyard ECU); `--sa-variant factory` should be sufficient.

```bash
$CLI ssm-a8-poll --transport obdx --device COM5 \
  --authenticate --sa-variant factory \
  --did 0xF300,0xF301,0xF302,0xF303,0xF304 \
  --duration 5s --output /tmp/sa-trace.log
```

**Pass:** SA challenge-response succeeds (no NRC 0x35 / 0x37); the polling produces DID bytes.

**Fail:** if NRC 0x35 (invalid key) — the rig has an aftermarket SA layer; try `--sa-variant aftermarket-l1` or `--sa-variant cobb-flash` etc. Per the catalog. If NONE work, capture the seed/key pair to `findings/sa-trace-<date>.log` and hand off to analyst for new-variant analysis.

**Bystander check:** if the rig is genuinely stock, factory SA works without issue. If a previous tuner installed something, this is where you find out.

## Step 4 — Flash protocol sequence capture

**Goal:** capture an actual UDS flash sequence against the rig (read-only — no writes yet) using the analyst's `docs/37`-referenced sequence as the expected.

This is a tracing-only step. The orchestrator writes plan PDUs to a `.uds` trace file; we compare against the analyst's reference sequence.

```bash
# Synthesize a flash plan against the rig's ROM with NO actual writes.
$CLI project-new --source /tmp/bench-rig-rom.bin --def <pack> /tmp/bench-rig-test.stune
$CLI flash-trace --plan /tmp/bench-rig-test.stune/flash-plan.toml \
  --output /tmp/bench-rig-flash-trace.uds
# Compare against the analyst's reference:
diff -u /tmp/bench-rig-flash-trace.uds findings/re-2026-06-12-pm/sh_can_flash_reference_sequence.uds
```

**Pass:** the implementer's planned sequence matches the analyst's reference. Any structural deviations (extra DSC, missing CommControl, wrong byte order) get triaged before any write.

**Fail:** if structural deviation — STOP. Fix the orchestrator first.

## Step 5 — First deliberate write (sector-erase + write + verify of a known-safe region)

**Goal:** first actual write to the rig's flash. Picks a known-safe target (not a boot sector, not a signature region).

The safest write target on the LF79103P is the **dead-fill calibration region** at `0x10000..0x12000` (per `docs/35-tuner-overlay-architecture.md`'s region map — historically zero-fill, no tuner-additions, no signatures). Writing 0xAA across a few KB there and verifying read-back is the lowest-risk first write.

**Pre-write checklist:**
- [ ] Battery voltage > 12.5 V (rig has bench supply with current monitoring).
- [ ] USB relay (`docs/28` Phase 6) is in the loop, on, NOT armed for trip.
- [ ] Backup of the current ROM is on disk (the `/tmp/bench-rig-rom.bin` from step 1).
- [ ] `Flasher::execute` is set to dry-run mode first.

```bash
$CLI flash-plan-info --def <pack> --source /tmp/bench-rig-rom.bin \
  --target-bytes 0x10000=AAAAAAAA...  # synthetic delta
# Run dry-run first
$CLI project-flash /tmp/bench-rig-test.stune --dry-run --transport obdx --device COM5
# If dry-run sequence looks right, run for real (no --dry-run):
$CLI project-flash /tmp/bench-rig-test.stune --transport obdx --device COM5
```

**Pass:** write completes, read-back verifies, ECU resets cleanly, full ROM re-pull shows the delta in place + everything else unchanged.

**Fail (worst case):** ECU doesn't boot. Power-cycle. If it still doesn't respond to `ap3 state` / OBDX SA, this is the deliberate-brick scenario from `docs/31`. Recovery path: mode-pin serial boot. Document everything.

## Step 6 — Power-loss inject

**Goal:** validate `docs/31`'s "interrupted flash → bootloader holds in waiting-for-reprogram → retry succeeds" guarantee.

Uses the USB relay to kill VBat at a pseudo-random point during the flash from step 5.

```bash
# Arm the relay's kill-on-X-seconds path.
$BENCH_TOOLS/relay-arm --kill-at 7s --rearm-after 2s
# Re-run the flash; should be interrupted ~7s in.
$CLI project-flash /tmp/bench-rig-test.stune --transport obdx --device COM5
# Expected: SubuwuTuner reports a UDS timeout / connection lost.
# Power-up the ECU again:
$BENCH_TOOLS/relay-on
# Reconnect:
$CLI rom-pull --addr 0x0 --size 0x10 --transport obdx --device COM5
# Expected: ECU is in waiting-for-reprogram mode (boot signature failure
# means the app jump is suppressed; UDS responds, application doesn't run).
# Re-run the flash plan from step 5:
$CLI project-flash /tmp/bench-rig-test.stune --transport obdx --device COM5
# Expected: flash completes, boot signatures restore, ECU boots normally.
```

**Pass:** 100% recovery across N injects (target: ≥ 50 injects per `docs/31` Tier 4 step 2).

**Fail:** any inject where the ECU can't be re-flashed — STOP. The brick-protection model has a hole; analyst gets a handoff.

## Step 7 — Delta-flash validation (`docs/40`)

**Goal:** validate the six-state recovery enumeration in `docs/40-delta-flash-brick-protection.md`.

Pre-req: `docs/40`'s journal-extension format is implemented in `st::flash`. (Today: spec only; implementation gated on this step.)

Each of states 1-6 in `docs/40` gets its own test. The relay-controlled power kill targets specific moments — mid-erase of non-sig sector (state 1), mid-write of non-sig (state 2), mid-erase of signature sector (state 3), etc.

This is the largest test. Budget half a day.

**Pass:** all six states recover via the documented path. Validates the delta-flash brick-protection model end-to-end.

## Step 8 — Patch-insertion validation (`docs/30`)

**Goal:** validate `src/feature_patch/` end-to-end against a real ECU. The patch insertion layer has shipped + been tested against synthetic ROMs (`tests/unit/feature_patch/`); this is the first real-ECU integration.

```bash
# Pick a sample .stmod (clutch-kill is the simplest).
$CLI feature-compile fixtures/samples/clutch-kill.stmod \
  --def <pack> --arch sh2a --format stmod \
  --output /tmp/clutch-kill-compiled.stmod
# Apply via the patch-insertion layer (depends on the C++-side wiring
# of PatchObject → PatchedRom; today the host produces a PatchedRom
# but doesn't yet ship a CLI subcommand that flashes it).
$CLI ???  # bench-rig task: ship a `feature-flash` CLI subcommand here
```

**Pass:** the patch executes on the rig's running engine simulation (or, if no simulation, the inserted code is reachable + the rig boots cleanly with the patched ROM in place).

**Fail:** patch insertion doesn't compose with the existing flash plan. Likely a writable-region-gate disagreement or a vector-table address that didn't redirect. Capture + analyst handoff.

## Step 9 — RH850 path (when a VB ECU lands)

**Goal:** all steps 1-7 against an RH850 G3MH dual-bank ECU.

Different ISA, different brick model (`docs/31`'s RH850 recipe), different open questions (active-bank detection DID, option-byte mirror count, CAN ID for serial-boot recovery — all open per `docs/31`).

This is its own multi-session push. Likely a separate doc spawned from here when the first VB ECU is on the bench.

## What "done" looks like

After step 7 passes:
- [ ] `docs/04` ship-blocker #1 (brick protection per-ISA, SH-2A side) → ✅
- [ ] `docs/40` delta-flash brick-protection → host + bench-validated; ready for v1.5 ship
- [ ] `docs/31` Tier 4 SH-2A validation plan → green across all 5 happy + power-loss + cancel-inject + deliberate-brick + cross-CID tests
- [ ] One of the open SA variants confirmed live against the rig

This unlocks the v1.0 ship for the SH-2A WRX target. RH850 (VB WRX) is a separate gate.

## Open questions (for the analyst)

1. **Whose ECU is on the bench?** CID + stock vs aftermarket SA layer status. Drives step 3.
2. **Reference UDS sequence file format.** This doc assumes `findings/re-2026-06-12-pm/sh_can_flash_reference_sequence.uds` exists; analyst needs to confirm location + format.
3. **`feature-flash` CLI subcommand.** Step 8 references a subcommand that doesn't exist yet. Either implementer ships it (~half-day) or the test executes via an alternate path. Decision tracked separately.
4. **Per-CID cross-coverage** (`docs/31` Tier 4 step 5). The rig is one CID; cross-coverage needs ≥ 3 cal-IDs to retire the gate. Sourcing 2 more donor ECUs is its own logistics task.

## Cross-references

- `docs/28-bench-rig-build.md` — what this doc continues from.
- `docs/31-brick-protection-by-isa.md` — Tier 4 test plan this runbook executes.
- `docs/40-delta-flash-brick-protection.md` — six-state recovery model, validated at step 7.
- `docs/37-subaru-flash-protocol.md` — UDS reference sequence.
- `docs/38-subaru-sa-variants.md` — SA-variant catalog, indexed in step 3.
- `findings/re-2026-06-12-pm/` — analyst's flash-protocol + COBB SA + cmd-dispatch RE outputs; loaded for step 4 comparison.
- `findings/handoffs/HANDOFF-to-analyst-2026-06-13-ap-browser-threading.md` — explains why the bench-rig path is sync (it doesn't share the AP browser's worker-thread model).

---

*When the rig comes up, work this doc top-to-bottom. Don't skip ahead — each step produces signal that informs the next.*
