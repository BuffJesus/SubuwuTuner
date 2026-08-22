# 40 — Brick protection for delta flash

> Per-ISA recipe for the v1.5 differential-flash workflow (`docs/04`
> roadmap + UX improvement #5 in
> `findings/SUBUWUTUNER_UX_IMPROVEMENTS_2026_06_11.md`). Companion to
> `docs/31` — that doc covers full-ROM reflash; this one is the partial-
> overlay extension. Hardware-free design; bench-rig validation gates
> the ship.

## What "delta flash" means here

Given a currently-flashed ROM `A` (known either from a fresh AP3 read
of `/backupcksum` or from a prior SubuwuTuner flash's manifest) and a
target ROM `B`, the flash orchestrator computes the byte-level diff
and writes only the **flash sectors** that differ. Two implications
that change brick-protection semantics:

1. **Sectors that are byte-identical between `A` and `B` are never
   touched.** The flash controller's wear count for those sectors does
   not advance, the per-sector erase pulse never runs, the per-sector
   write timing budget is never consumed.
2. **A delta-flash session is not idempotent in the same way a full
   reflash is.** A full reflash that's interrupted leaves the ROM in
   "partly-target / partly-zero-fill" with the boot signatures broken.
   A delta-flash that's interrupted may leave the ROM in "partly-`A`,
   partly-`B`, partly-zero-fill" — the third state matters for the
   power-loss recovery story.

The goal of this doc: enumerate every shape of state a delta-flash
session can land in, and prove each one recovers cleanly.

## Why we don't just say "delta = N full-reflash steps"

Tempting to model delta as "N sector-scoped full reflashes." The
guarantee from `docs/31` — "interrupted full reflash leaves the
bootloader's signature-check gate firing, ECU waits for reprogram"
— would then carry over per-sector.

It doesn't, for two reasons:

1. **Boot signatures span sector boundaries.** The three SH-2A signatures
   (`0x5555 @ 0x6000`, `0xAAAA @ 0x1FFFF2`, `byte-equality @ 0x6C ↔ 0x6010`)
   live in two specific sectors. A delta that touches NEITHER of those
   sectors leaves the boot gate intact regardless of how badly the
   write goes elsewhere. A delta that touches EXACTLY ONE of them
   needs explicit handling so the signature post-condition holds when
   the session resumes.

2. **The aftermarket CRC slot table at `0x1FFF3C..0x1FFFA0` is NOT
   runtime-validated** (per `project_cobb_checksum_table_not_runtime_validated`
   memory + `docs/31` "What is not the boot-time integrity check").
   But the AP cares about it for tune-coherence after install. A delta
   that changes calibration bytes covered by a slot must either
   regenerate the corresponding CRC OR explicitly mark the slot
   stale so the AP doesn't think the tune is corrupt. Full reflash
   regenerates the whole table from scratch.

## SH-2A recipe (VA WRX, single bank)

### Pre-flash gates (additive to `docs/31`)

All gates from `docs/31` still apply (sector allow-list, boot-signature
host-side preflight, cancel-between-PDUs). Delta-specific additions:

1. **Currently-flashed identity hash MUST be known.** The delta plan
   resolves against a specific `A`. If the host has no fresh hash of
   `A` (because the ECU was flashed by a different tool since last
   SubuwuTuner contact), the orchestrator falls back to full reflash
   automatically — no silent risk. Source of truth for the hash:
   the AP3 `/backupcksum` MD5 (read on every connect when an AP is
   present), or `st::flash` manifest from the last SubuwuTuner write.
   Disagreement triggers fallback.

2. **Delta plan vs target identity self-check.** The orchestrator
   composes `A + planned_writes` byte-for-byte and verifies the
   result == `B`. Catches the "diff produced something other than
   `B`" bug class before any wire byte goes out. Cheap; runs in
   the working buffer.

3. **Signature-sector promotion.** Any delta that includes a write
   to a sector containing one of the three boot signatures gets
   that sector flagged "signature-critical." Signature-critical
   sectors get post-write verification within the same UDS
   session — not deferred to the closing verify. If the in-line
   verify fails on a signature sector, the orchestrator aborts
   the session immediately (we don't want the closing verify to
   be the first thing to notice we just bricked the boot).

### What can actually brick in delta mode

Same hardware-level surface as full reflash:
1. Custom kernel bypassing the FCU allow-list (we don't ship one).
2. FCU hardware fault.
3. FLMCR-poke from misbehaving application code (Subaru's stock app
   doesn't have this).

Plus one delta-specific surface:
4. **The orchestrator computes the wrong `A`.** If our `A` doesn't
   match what's actually on the ECU, the delta-applied result isn't
   `B` — it's `(actual_ECU) + (planned writes assuming A)`. This is
   the failure mode the identity gate (#1 above) prevents. Without
   the gate, this is the realistic source of bricks.

### Power-loss scenarios — sector-by-sector enumeration

Walk every state a delta-flash session can be in when power dies.

#### State 0 — before any erase

ECU has `A`. Boot signatures hold. Power-loss is benign. Resume =
restart the plan.

#### State 1 — mid-erase of a non-signature sector

Sector is partly `0xFF`. Boot signatures still hold (untouched).
Application MAY fail to run because erased calibration regions
return `0xFF` to runtime reads, which the application interprets
as out-of-range and faults. ECU boots, bootloader hands off to
application, application crashes or enters limp-home.

**Recovery:** ECU is "soft-bricked" in the sense that it boots but
runs poorly. UDS reflash session is fully reachable since the
bootloader signature gate passed. SubuwuTuner reconnects, computes
new delta (the partly-erased sector now reads as partly-`0xFF`,
which differs from BOTH `A` and `B`, so it gets queued for full
rewrite), and proceeds. **No bench tooling needed.**

#### State 2 — mid-write of a non-signature sector

Sector contains a mix of `0xFF`, `A` bytes, and `B` bytes. Same
recovery shape as State 1: limp-home boot, UDS reachable, retry
the plan. **No bench tooling needed.**

#### State 3 — mid-erase of a signature sector

Boot signature is `0xFFFF`. Bootloader's `FUN_00000C54` rejects the
app jump. ECU enters waiting-for-reprogram per the documented full-
reflash recovery in `docs/31` §"Recovery procedure — power-loss
mid-write." **Same recovery path as full reflash.**

#### State 4 — mid-write of a signature sector

Three sub-cases depending on which signature was being restored:

- **`0x5555 @ 0x6000`**: if `B` has the same signature value `0x5555`
  at this address (universal for SH-2A 2 MB ROMs per `docs/31`
  evidence), this write is "re-paint with same value." A partial
  write here leaves the signature still being `0x5555` if both bytes
  of the u16 land as `0x55`. The fixed-pattern check passes. ECU
  boots normal app. **Benign.**

- **`0xAAAA @ 0x1FFFF2`**: same logic — `B` has `0xAAAA` here too.
  Partial write that leaves both bytes as `0xAA` passes the check.
  **Benign.**

- **Sector-erased state hits the signature.** If the sector was
  erased in State 3 and we never reached the write, the signature
  is `0xFFFF` not `0x5555`. Same as State 3 — waiting-for-reprogram.

The "tricky" middle case is **partial signature write where one byte
flipped but the other didn't.** SH-2A flash word-size writes are
atomic at the FCU level (the FLMCR program command writes a whole
word in one timed pulse, not byte-by-byte). So the signature word
either holds `A`'s value, `0xFFFF`, or `B`'s value — never a half-
flipped intermediate. The two written-but-different states both pass
the fixed-pattern check (since `A` and `B` agree on signature value
universally). **Benign.**

#### State 5 — between sectors

All sectors written so far are byte-identical to `B`. Boot signatures
hold (if the signature sectors were in the "already done" set) or
hold from `A` (if they're in the "still to write" set, since `A`'s
signatures are identical to `B`'s). ECU boots `B`'s partial image,
which may or may not run cleanly depending on which sectors are
still pending. Practically the same as State 1/2 for runtime
behavior. **Recovery via plan restart.**

#### State 6 — after all sector writes, mid-final-verify

Writes are done. Verify is reading back. Power-loss here changes
nothing — the writes were committed at FCU level. Recovery: next
session re-runs verify, succeeds, session closes clean.

### State table summary

| State | Boot signature gate | App-runs? | UDS reachable | Recovery |
|---|---|---|---|---|
| 0 | passes (`A`) | yes (`A`) | yes | restart plan |
| 1 (non-sig erase) | passes (`A`) | limp-home | yes | restart plan |
| 2 (non-sig write) | passes (`A`) | limp-home | yes | restart plan |
| 3 (sig erase) | FAILS | no | bootloader-only | resume = re-flash the sig sector |
| 4 (sig write, atomic word) | passes (`A` or `B`) | yes (`A` or `B`) | yes | restart plan or skip |
| 5 (between sectors) | passes (universal) | depends | yes | restart plan |
| 6 (mid-verify) | passes | yes | yes | re-verify, done |

**Conclusion:** every state recovers via the standard UDS path.
State 3 is the same recovery shape as a full-reflash interrupt
(`docs/31` already covers it). Delta flash does NOT widen the brick
surface vs full reflash on SH-2A.

### Journal extensions

The `st::flash` journal (already used for power-loss resume across
full reflashes) gains delta-specific fields per session:

```
[session]
mode = "delta"
base_rom_hash = "<MD5 of A>"
target_rom_hash = "<MD5 of B>"
schema_version = 2

[[sector]]
index = 0x0A
state = "pending" | "erased" | "written" | "verified"
signature_critical = true
attempted_at = "..."
```

On `Project::open()`, if a journal exists with `mode = "delta"` and
state ≠ all `"verified"`, the GUI/CLI offers:

1. **Resume** — recompute current ECU state (re-read `/backupcksum`),
   confirm hash matches `base_rom_hash` OR matches a partial-write
   state predictable from the journal (e.g. sectors up to index K
   are at `target_rom_hash`'s value, sectors after K are at
   `base_rom_hash`'s value, sector K is at `0xFF` or partial-
   write), then continue from the next pending sector.
2. **Fall back to full reflash** — safe regardless of journal state;
   takes the full 3 minutes.
3. **Abort + clean disconnect** — UDS DSC = defaultSession; the ECU
   either runs `A` (limp-home if state 1/2) or holds in
   waiting-for-reprogram (state 3). User decides whether to drive
   it as-is or retry later.

### Bench-rig validation (Tier 4) — additive to `docs/31`

Same five-test plan from `docs/31` plus four delta-specific
additions. Pass on all nine = SH-2A delta-flash ship gate cleared.

1. **Delta happy-path 100 cycles.** Same as the `docs/31` happy-path
   but each iteration uses delta against the prior iteration's
   target. Verify each delta produces byte-identical final ROM.
   Pass = zero bricks, byte-identical verify.
2. **Power-loss inject in non-signature sectors.** Force kill during
   erase + write of cal-region-only deltas. Expected: State 1/2
   recovery. Pass = 100% recovery via plan restart across ≥ 50
   injects.
3. **Power-loss inject in signature sector.** Force kill during a
   delta that includes the signature sector. Expected: State 3
   recovery (same as full reflash power-loss). Pass = 100% recovery
   across ≥ 25 injects.
4. **Computed-`A` mismatch.** Manually corrupt the host's `A` hash,
   run delta. Expected: identity gate refuses to flash, falls back
   to full reflash or reports a clear "currently-flashed identity
   doesn't match" error. Pass = zero attempts to write a
   miscomputed delta.

## RH850 recipe (VB WRX, dual bank)

**Status: deeper deferment than the SH-2A side**. The dual-bank
substrate makes delta flash strictly easier — every byte written
lands in the inactive bank, the active bank stays valid until the
final option-byte swap, and the swap is atomic. That's the same
shape as full reflash on RH850; the delta savings are in flash-time
(less to erase + write) but the brick-protection model is identical.

Open questions same as `docs/31` § "Open questions (bench-rig
dependent)":

- Active-bank detection DID
- Option-byte mirror count
- CAN ID for serial-boot recovery

Plus one delta-specific:

- **Sector granularity on RH850 G3MH.** Some variants support
  smaller erase blocks; delta benefit scales with how fine the
  granularity gets. Bench rig captures this on the first read-out.

Until the bench rig is up for the VB target, RH850 delta is
flagged as v1.5 design-only; v1.0/v1.1 ship without it.

## Cross-cutting

### Lock-mask interactions

The `.ptm` envelope's `lock_mask` field (currently 0 for all observed
COBB tunes — see `findings/ptm-decrypt-2026-06-09/`) reserves bits
for per-region access gates. If a future tune sets bits that prohibit
writes to a region, the delta planner respects them by failing closed
at `gate_patch` (same as full reflash). No delta-specific surface.

### Aftermarket CRC slot table coherence

After delta-flash, the slot table at `0x1FFF3C..0x1FFFA0` may be
stale for slots whose calibration bytes changed. Per
`project_cobb_checksum_table_not_runtime_validated`, the ECU does
not consult this table at boot, so the freshness affects only AP
coherence. Two strategies:

1. **Always recompute.** Cheap; runs in working buffer; included
   in the delta plan if any byte in the slot's covered range
   changed. **Default for v1.5.**
2. **Skip recompute, mark slot stale.** Only useful when AP-side
   tune-coherence is undesired (e.g. SubuwuTuner-only workflow that
   never returns to the AP). Optional flag; off by default.

### Composability with the patch-set model (`docs/36`)

Delta flash and patch-set composition are orthogonal — composition
produces the target ROM `B` from a stack of patch sets; delta flash
computes the cheapest way to install `B` over the currently-flashed
`A`. They compose by sequencing: compose → compute target → delta-
flash. The orchestrator does not need to know which patch-set
contributed which byte; only the resulting `B`'s bytes vs `A`'s
matter for the flash plan.

## What this doc does NOT cover

- **Patch-set-aware partial flash.** A future variant might flash
  only one layer of a stacked patch set (e.g. "swap from Felix WRK2
  to Felix WRK3 without re-applying the COBB Stage 1 base layer that's
  underneath"). The orchestrator still computes `target_B`
  externally; delta-flash sees only the `(A, B)` byte difference.
  No new brick-protection surface vs this doc.
- **RH850 dual-bank delta in detail.** Deferred until the bench rig
  validates the basic RH850 full-reflash path per `docs/31`.
- **Wireless / OTA delivery of delta plans.** `docs/18` §6 covers
  the wireless surface; delta-flash inherits its safety properties
  unchanged (the brick-protection gates fire host-side regardless of
  how the plan was delivered).

## References

- `docs/04-roadmap.md` — v1.5 differential-flash line item.
- `docs/05-improvements.md` §4 — top-level brick-protection design.
- `docs/08-testing-strategy.md` Tier 4 — HIL test plan + bench-rig
  framing.
- `docs/13-transport.md` — UDS RoutineControl + RequestDownload
  surface that the per-sector erase + write uses.
- `docs/16-custom-features.md` §Safety #6 — `gate_patch`
  writable-region check.
- `docs/28-bench-rig-build.md` — bench-rig phase that validates
  delta flash (additive to the Phase-5.5 full-reflash gate).
- `docs/31-brick-protection-by-isa.md` — full-reflash brick
  protection. This doc extends `docs/31` for the partial-overlay
  case; both apply.
- `docs/36-tune-as-patch-set.md` — patch-set tune model that feeds
  the delta planner.
- `findings/APP_CHECKSUM_VERIFICATION.md` — three-signature gate
  the recovery story relies on.
- `findings/SUBUWUTUNER_UX_IMPROVEMENTS_2026_06_11.md` UX #5 —
  the user-facing "Flash (8s)" experience this enables.
