# 18 — Standalone Handheld Tuner (Master Plan, v6)

Companion plan for the portable Teensy device. Slots into `docs/` as
`18-…`; **subordinate to** `docs/04`, `docs/05`, `docs/09`, `docs/13`,
`docs/15` — if it conflicts with them, they win.

> **v6 absorbs the 2026-06-10/11 AP3 reverse-engineering findings.** The Teensy + ESP32 architecture (v5) is unchanged. What changes is the **tune representation model** and the **handheld's UX surface** — both substantially richer now that we understand how the leading mainstream device organizes tunes. Patch-set composition, differential flash, library management, and an opt-in USB→OBD bridge mode all land in v6. The §4 safety rule remains absolute; nothing in v6 moves wireless into the flash path. See §13 for the full "what AP3 taught us" summary.

> **v5 added the ESP32 wireless co-processor.** v4's safety spine (the
> device runs the same audited orchestrator, cross-compiled) is unchanged.
> v5 adds one hard rule (§4) that keeps wireless entirely out of the
> existential-risk path.

---

## 0. Prime directive (unchanged)

> A failed flash must never leave an ECU in a non-recoverable state.

Project's existential risk: `docs/09` R1, `docs/05` §4, `docs/08` Tier 4.

---

## 1. Product vision

A standalone handheld in the spirit of the existing aftermarket
flasher platforms: flash tune files **without a PC**, gauges,
datalog, DTC read/clear, restore stock — *and* act as a PC↔car
interface, now also **wirelessly** (phone dash, wireless log
offload, wireless package delivery) like the OBDX.

Clean-room: handheld-tuner and wireless-OBD-adapter are *concepts*, free
to implement per `docs/15`. No competitor firmware/protocol is read. The
ESP32 BT/WiFi stack is Espressif's; Subaru comms remain the project's own
clean-room UDS/SSM.

---

## 2. Architectural spine (unchanged from v4)

The standalone executor is `st::flash` + `st::ecu::uds` + `st::ecu::ssm`
+ `st::log`, cross-compiled to the Teensy, driving an on-chip
`ITransport`. Same code path, same 37 flash tests, same Manifest/journal/
`plan_resume`, same mutation gate, same Phase 4 bench gate. The Teensy is
the sole safety brain.

---

## 3. Wireless via an ESP32 co-processor

**The ESP32 is a dedicated radio peripheral, not the brain.** It connects
to the Teensy over a narrow UART/SPI message protocol. The Teensy treats
it as just another untrusted requester — identical trust level to a USB
host or an SD-borne package. Nothing the ESP32 says bypasses the
Manifest / CID-stock / `st::policy` / brownout interlocks.

Rejected: using the ESP32's own TWAI/CAN and letting it run the show.
That co-locates a large WiFi/BT attack surface and a non-deterministic
RTOS with the flash path. Not acceptable for an R1-class operation.

Hardware: a **pre-certified module** (ESP32-S3-WROOM class — IC/FCC
pre-cert; user is in Canada). ESP32-S3 = mature BLE5 + WiFi4; ESP32-C6 if
WiFi6/Thread is wanted later. A pre-cert module shrinks RF design risk to
antenna placement + keep-out only.

What wireless is *for* (all non-flash-critical):

- Phone/PC as a remote gauge + datalog viewer (`st::log` stream sink over
  BLE/WiFi, in addition to / instead of the local screen).
- Wireless delivery of signed tune packages onto SD.
- Wireless log offload.
- Signed OTA updates of the ESP32 firmware.

---

## 4. The hard rule

> **The wireless link is never load-bearing during a flash.**

A flash is always executed locally by the Teensy over wired CAN/K-Line,
from a package already on SD, journaled to SD, brownout-protected.
Wireless is **observe-and-command only**; "command" = "start a
locally-executed, locally-gated operation," never "carry the bytes."

Consequences:

- A wireless dropout the instant a flash starts is a **non-event** — the
  flash is local and journaled; the phone just reconnects to the
  progress/journal.
- The PC-passthrough wireless `ITransport` is **read/datalog only** —
  mirrors `docs/13`'s ELM327 "no unproven write path on principle"
  stance. Live UDS/ISO-TP flash timing never travels over a lossy radio.
- PC-initiated flashing = the PC tells the device to flash a package
  *that is on the device's SD*; the device runs it locally and streams
  progress back. The PC never pushes UDS frames over WiFi.
- Wireless-delivered packages are safe because the signed-Manifest +
  CID-stock + on-device policy gate treat every package as
  untrusted-until-verified regardless of arrival channel. Delivery
  bypasses no interlock.

---

## 5. Tune package + marriage interlock (v6 — patch-set composable)

The tune model is **richer in v6 than it was in v5**. The handheld stores tunes as **patch sets** per `docs/36-tune-as-patch-set.md`, not as full ROM images. This is a denormalization, not a replacement — the audited orchestrator still writes byte-addressable PDUs to flash. What changes is what the handheld's SD card holds and how the user composes the next flash.

### Tune-package payload (v6)

- **Format:** signed `FlashPlan` (TOML + external `data_file` binary form) + tamper-evident `Manifest`. **CRC32 → BLAKE3 Manifest upgrade is now MANDATORY for v6** (was "pull forward" in v5; the patch-set model has more moving parts, integrity guarantees are load-bearing).
- **What's in `data_file`:** either a full ROM (legacy v5 mode) OR a serialized patch-set (v6 mode) — the orchestrator handles both. A patch-set is `[(rom_offset, length, bytes)*]` per `docs/36`, with `vendor_id`, `vehicle_id`, `rom_sum`, `lock_mask`, `save_date_time` metadata.
- **Patch-set sourcing:** the handheld can hold:
  - Stock ROM per CID (one per supported platform, ~2 MB each on SD)
  - N patch sets (~50-100 KB each on SD — see `docs/35` for the typical size distribution)
  - User-composed projects (each project = ordered list of patch-set references + local edits)
- **Patch-set sources:** `.ptm` files imported via SubuwuTuner desktop (`docs/34` cipher gating); SubuwuTuner-authored exports from `.stune` projects; community patches; tuner-delivered ETune deltas.

### Composition at flash time

When the user selects "flash this project," the orchestrator:

1. Loads the base ROM from SD (CID-keyed)
2. Applies each referenced patch-set in declared order per `specs/patch-composition-algebra.md` (private analyst-side spec)
3. Layer-aware conflict detection runs; LOW conflicts auto-resolve, MEDIUM conflicts surface to user (on the handheld screen — yes/no), HIGH conflicts (Layer 3 code overlap) abort by default
4. The composed final ROM is hashed; hash compared against the project's stored hash (catches in-flight SD corruption)
5. The flash plan goes through the same brick-protection / mutation gate / brownout interlock that the v5 full-ROM path uses

### Differential flash (NEW in v6)

Per `docs/36` + `findings/SUBUWUTUNER_STRATEGIC_APPLICATIONS_2026_06_11.md` #5:

- After composition, the handheld reads the **currently-flashed ROM identity** via UDS Mode 0x09 (CAL ID / CVN) — same lightweight read it uses today for `docs/29` SSM a8 polling.
- Compute the byte-level delta between composed and currently-flashed.
- If delta < 100 KB (typical tune-to-tune transitions), flash ONLY the differing pages.
- If delta > 1 MB OR currently-flashed is unknown OR currently-flashed didn't match any locally-stored ROM, fall back to full reflash.

Estimated wins: typical tune iteration goes from ~3-5 minutes (full 2 MB) to ~30 seconds (delta only). Brick-risk window shrinks proportionally — fewer bytes written = fewer chances for a corrupted PDU.

**Brick-protection extension required:** the differential-flash path opens partial-overlay corner cases (interrupted mid-delta leaves a half-applied patch set on the ECU). `docs/31` §"Common safety properties" has been extended with safety-property #7 to cover the aggressive-overlay case; that property's verify-against-byte-level-diff requirement applies equally to differential flash. The orchestrator's resume-from-journal path (`Project::open() + Flasher::plan_resume`) handles mid-delta interruption the same way it handles mid-full-flash interruption today; what's new is the journal explicitly records which patches were applied so resume can re-derive the partial state.

### Stock-backup interlock (unchanged from v5)

- Stock backup still mandatory, CID-keyed (`read_full_rom`).
- No write without a verified stored stock for that exact CID.
- Restore-to-stock always one action away (a flat ROM image, not a composed patch set — restore is the simplest possible plan).
- `st::policy` engine-safety gate runs on-device, blocking in every jurisdiction profile.

### "Marriage state" — sense-only, not enforced

The AP3 ties hardware to one vehicle via a NAND-stored marriage byte; unmarrying requires JTAG. The handheld v6 adopts a **soft marriage** instead:

- Handheld remembers the last-installed CID (already does for stock-backup interlock)
- On connect to a different car, the handheld shows: "you're connected to a different vehicle than the last flash (last: 2017 USDM WRX MT; this: <observed CID>). Continue?"
- User can acknowledge and proceed; handheld updates the soft-marriage record
- No hard enforcement; no JTAG-recoverable lock state

Rationale: hard marriage is anti-user (locks a $XXX device to one car forever) without adding meaningful safety beyond the existing CID-keyed stock-backup interlock. A soft confirmation captures the safety value (catching "I forgot to grab the right cable / car / stock backup") without removing user agency.

---

## 6. Port surface (v4 + wireless + v6 patch-set features)

1. Embedded `ITransport` — FlexCAN_T4 + K-Line; on-wire ISO-TP here.
2. SD-backed path/IO + clock shim; SDIO (not SPI).
3. Heap/stack budget; streaming reads/writes (no whole-ROM buffering).
   **v6:** the composition step is streamable too — the orchestrator
   walks patches in offset order, accumulating into a sector-sized
   write buffer; no need to materialize the full composed ROM in RAM.
   Critical for the Teensy 4.1's RAM constraints.
4. LVGL UI sampling `st::log`'s ring; UI carries no tuning logic.
   **v6:** new screens for tune library + patch-set composition (see
   §12.5 below).
5. Hardware brownout interlock — input-V sense + cutout; the only safety
   job not in shared code.
6. **Inter-MCU link to ESP32** — framed UART/SPI; a defined command/
   telemetry protocol. The Teensy validates every ESP32 request through
   the same interlocks; the ESP32 holds no keys to the flash path.
7. **`st::devices::ap3` shared with desktop (NEW v6).** The handheld
   can run as a USB host accepting an AP3 over its USB OTG port — or
   as a USB device exposing its own `/maps/`-style file vault to a
   connected desktop. Same `st::devices::ap3` code path used by the
   desktop's `subuwutuner-cli ap3` (per `docs/34`); cross-compiled
   for the Teensy. **This is the v6 USB-bridge mode**, optional and
   opt-in (see §8.5).

---

## 7. Hardware / electrical

- Socket the Teensy 4.1 as a module (not bare i.MX RT1062).
- SD is SDIO, not SPI.
- **Brownout is the top electrical risk** — buck-boost + hold-up cap +
  §6.5 interlock.
- Automotive transient protection — reverse-polarity + input TVS
  (ISO 7637-2/16750); series R + TVS/CM-choke on CAN.
- Isolation mandatory for any write-capable build.
- **ESP32 RF power is separable.** WiFi TX spikes ~500 mA, but wireless
  is idle/off during the crank-brownout-critical flash window (it's
  non-load-bearing, §4), so keep the RF rail **off** the budget that
  must survive a flash.
- **Provision the ESP32 on PCB rev A** even though the wireless feature
  lands later (§9): module footprint, inter-MCU lines, RF keep-out,
  separate RF rail. Retrofitting a radio onto a later spin is wasteful.

### AI-assisted PCB workflow

Human owns the critical ~20%, locked before any AI pass: isolation
barrier, brownout/power path, transient clamps, CAN front-end,
arm-to-flash interlock, **and the RF/antenna section** (RF + controlled
impedance are explicitly manual). AI-assisted: carrier passives, connector
breakouts, **inter-MCU UART/SPI + ESP32 decoupling**, BOM/sourcing,
first-pass route of non-critical nets, context-aware review. Flux
all-in-one for a sub-100-part one-off; KiCad + Quilter to own the
schematic format.

---

## 8. Security (raised by adding a radio)

- **Physical arm-to-flash interlock earns its keep** — a remote attacker
  still cannot flash without someone physically present at the car.
- Signed packages → a compromised radio can't deliver a flashable tune.
- ESP32 OTA must itself be signed.
- **Backstop principle:** a fully compromised ESP32 must remain
  *insufficient* to brick or maliciously tune a car — the Teensy
  interlocks do not trust it.
- BLE pairing/auth; WiFi locked down (WPA2/3, no open services);
  optional physical wireless-enable.

### 8.5 USB-bridge mode — fail-closed surface

The v6 USB-bridge feature (§6.7) creates a new attack surface: the
handheld can be addressed by a connected PC via the AP3-style file
vault protocol. The same rules that govern the wireless surface apply:

- USB-bridge file operations target a separate SD partition (the
  "bridge spool"), NEVER the flash-plan partition or the stored-stock
  partition. A PC writing to the bridge spool cannot displace a stored
  stock backup.
- Flashing a file delivered via USB-bridge requires the same physical
  arm-to-flash interlock as any other flash. The PC can deliver bytes;
  it cannot trigger a write.
- Signed Manifest verify still mandatory. A tune file delivered via
  USB-bridge must pass the same BLAKE3 + signature check as a tune
  loaded from SD.
- USB-bridge mode is opt-in per session — defaults to OFF on boot.
  User enables via the handheld's hardware screen; the enable persists
  for one session only.

---

## 9. Roadmap — still rides docs/04

- **Pre-req (true today):** Phase 3 + Phase 4 orchestrator complete
  hardware-free. Device doesn't substitute for the OBDX you're waiting on
  for the Phase 1/3 *data* gates.
- **D1 — PC-passthrough bring-up** (wired USB-CDC `ITransport` + 2nd-Teensy
  ECU sim). Prove the PHY before trusting the on-device stack.
- **D2 — Cross-compile the stack** (`arm-none-eabi`; embedded `ITransport`
  + SD/clock shims). Gate: flash/UDS suite passes on target,
  byte-identical to PC build.
- **D3 — Phase 3 ride** (read/log on a real VA/VB; `read_full_rom` stock
  capture to SD). Same `docs/04` Phase 3 gate.
- **D4 — Phase 4 ride, device as flashing agent.** Device runs the
  cross-compiled orchestrator through the existing `docs/04` Phase 4 gate:
  100 bench flashes, zero bricks; power-cut-mid-`0x36` recovery;
  restore-from-SD-stock; no-stored-stock refusal; policy-block test.
  **No car until this gate. No wireless anywhere in this milestone.**
- **D5 — PCB rev A** (§7 critical blocks + ESP32 *provisioned*, firmware
  later); fab; bring-up; re-run D4.
- **D6 — Wireless feature (after D4, never near it).** ESP32 firmware +
  inter-MCU protocol; phone gauges/log offload; wireless package
  delivery; wireless read-only `ITransport`; signed OTA. Gated by the §4
  rule and §8.

---

## 10. Risk — folds into docs/09

R1 governs. Standalone + wireless additions:

| New | Failure | Mitigation |
|---|---|---|
| D-a | Bad/forged package (no PC operator) | Mandatory signed-Manifest verify; BLAKE3 pulled forward |
| D-b | Flash onto wrong/unknown ECU | CID-keyed stored-stock interlock |
| D-c | Brownout mid-write, nothing watching | §6.5 hardware interlock + hold-up cap; orchestrator error/journal path |
| D-d | Unsafe tune, no PC to warn | `st::policy` gate on-device, every profile |
| D-e | Wireless dropout mid-flash | Non-event by §4 — flash is local + journaled; radio not load-bearing |
| D-f | Remote attacker commands a flash | §4 (command ≠ bytes) + §8 (physical arm-to-flash + signed packages + ESP32-not-trusted backstop) |
| D-g | ESP32 firmware compromise | Backstop: insufficient to flash; signed OTA; isolated inter-MCU protocol |

Session timeout / ISO-TP / stale checksum / hung-mid-write remain
mitigated in the shared audited code, because the device runs that code.

---

## 11. Clean-room (firmware)

Mirror `docs/15`. Firmware ISO-TP/CAN/K-Line framing from public ISO
standards + public Teensy/FlexCAN APIs only. ESP32 side uses Espressif's
public SDK. Handheld-tuner and wireless-adapter are idea-side concepts;
no competitor firmware, capture, or scheme is read or mirrored.

---

## 12. Hardware feature-toggle UX

A defining UX feature of the mainstream aftermarket handheld
flashers is a hardware screen that toggles individual tuning
behaviors live — Launch Control on/off, Flat-Foot Shift on/off,
idle RPM up or down — without a reflash and without a PC.
Replicating that experience is *not* a new subsystem; it is the
**convergence** of three existing designs:

1. **`docs/16` (custom features)** declares each feature with a
   RAM-mapped enable flag (a `[[feature]]` block in the pack carries
   an `enable_ram_address` plus optional scalar-parameter addresses).
   The custom-features designer emits SH-2A/RH850 code that reads
   that flag every loop iteration; flag false = feature inert.
2. **`docs/19` (live tuning)** owns the write primitive. Toggling the
   flag is one UDS `WriteDataByIdentifier` to the RAM address. The
   same plan-time linter that runs on calibration-cell writes runs
   on a feature toggle — engine-safety still blocking.
3. **This document (handheld)** surfaces the toggles as LVGL screens
   bound to the pack's `[[feature]]` entries: one row per feature,
   with an on/off switch and (where applicable) a slider for the
   scalar parameter. The handheld writes via the same `ITransport`
   it uses for `st::log` and `rom-pull`; no new transport path.

Concrete toggleable surface a v1.5 handheld would carry, given a
typical FA-DIT WRX pack:

| Toggle | Mechanism | Underlying write |
|---|---|---|
| Launch Control on/off                 | feature enable flag | 1-byte RAM write |
| Launch Control target RPM             | feature scalar       | 2-byte RAM write |
| Flat-Foot Shift on/off                | feature enable flag | 1-byte RAM write |
| Flat-Foot Shift min-RPM threshold     | feature scalar       | 2-byte RAM write |
| Anti-lag on/off                       | feature enable flag | 1-byte RAM write |
| Idle target RPM offset                | live calibration cell | 2-byte RAM write to RAM-shadow of an Idle Target RPM cell |
| Boost target offset (e.g. -2..+2 PSI) | live calibration tweak | per-cell RAM-shadow write |
| Active map (Stock / Tune / Race)      | full reflash (NOT a live toggle — uses the normal flash path) | manifest-driven |

Map switching is the one exception. Switching the "active tune" is a
full flash cycle, not a RAM write, because the entire calibration
needs to change atomically. That matches what we observe on the wire
when a handheld flasher performs a map change — ~2 MB written to
flash, not a RAM toggle. The handheld surfaces map-switching as a
flash button, with the same brick-protection + brownout interlock
as any other flash from `docs/05` §4.

### Safety constraints specific to feature toggles

- **Engine-safety linter runs on every toggle.** A feature whose
  `enable_ram_address` is bound to a flag with `category =
  engine_safety` (e.g. "remove rev limiter") cannot be toggled on
  from this screen in any jurisdiction profile. That toggle has to
  go through the desktop GUI with explicit confirmation. Driving
  past the rev limiter on a public road is not a one-button affair.
- **Connection liveness preflight.** Same as `docs/19`: refuse to
  enter the toggle screen if battery V is low or the adapter
  handshake is degraded. A toggle that gets NACK'd silently is the
  failure mode that ends with "I clicked off and it didn't actually
  turn off."
- **Toggle journal.** Every live toggle write goes to the device's
  SD-backed session log, same shape as `docs/19`'s session journal.
  After the drive, the user can replay the session into a desktop
  project to see exactly what was toggled when.

### What this means for clean-room

Concept-side, this is the canonical "concept is fair game, expression
is not" division from `CLAUDE.md`. *Hardware screen that toggles
features live* is an idea — a mainstream commercial handheld
popularised it on Subaru, but it's descended from rally TC1 toggles
in physical switches. **Implementing it from our spec stack is
fine.** What is not fine: copying a competitor's specific feature
catalog (their named features, their preset RAM addresses, their UI
strings, their proprietary tune-file format). All of those are
expression. Per `CLAUDE.md`'s explicit red flags, we do not pull
any of that from a competitor's tool decompile.

The features in the table above are derived from public engine-
management literature + the existing `docs/16` sample packs
(`clutch-kill.stmod`, `flat-foot-shift.stmod`, `launch-control.stmod`).
Adding a sixth or seventh feature means designing it from first
principles, not from any competitor's list.

### Roadmap

This is **v1.5** alongside live tuning — same hardware-arrival gate,
same Phase 4 prerequisite. The pieces (docs/16 designer, docs/18
handheld firmware, docs/19 live writes) have to all reach maturity
before the cross-cutting UX makes sense to integrate. Until then,
the v1.0–v1.4 desktop GUI's custom-features-designer (`docs/16`)
ships the same feature graphs without the hardware-screen layer —
just less convenient for dyno work.

## 12.5 Tune library + patch-set composition (v6 additions)

Two new on-device screens land in v6, both directly enabled by the
patch-set tune model from `docs/36` and the AP3 file-vault UX
observations:

### 12.5.1 Tune library screen

Mirrors the AP3's `/maps/` model but with SubuwuTuner's own (cleaner,
signed) metadata:

```
Tune Library
  a personal tuner-on-FA24 v3            72 KB  2026-06-10  [Flash][Inspect][Delete]
  a personal tuner-on-FA24 v2            64 KB  2026-06-10  [Flash][Inspect][Delete]
  a personal tuner WRK3 (currently flashed) 72 KB  2026-06-09  [Restore][Inspect]
  Stage 1 + SF v401           51 KB  2026-01-15  [Flash][Inspect][Delete]
  ...
[Import from SD] [Compose new]
```

Each row shows the tune name, size, last-modified, action buttons.
"Currently flashed" badge derives from the soft-marriage record + the
backupcksum check at AP-connect equivalent (handheld-side: UDS Mode
0x09 read against the connected ECU).

The Inspect button opens the §12.5.3 inspect view (per-layer patch
breakdown).

### 12.5.2 Compose screen

For users building a tune by stacking patch sets:

```
Compose new tune
  Base ROM:   LF79103P stock                       [▾]
  Layer 1:    COBB Stage 1 + SF v401              [✓][▾]
  Layer 2:    a personal tuner WRX iteration 2                [✓][▾]
  Layer 3:    a community swap-basemap vendor FA24 mechanical                  [✓][▾]
  + Add layer
  Local edits: 3 cells changed                      [Review]

  Conflicts:  35 Layer-2 conflicts auto-resolved
              0 Layer-3 conflicts

  [Save as project] [Flash now]
```

Each layer is a reference to a patch set in the library. Layer order
matters — later layers override earlier. The compose screen runs the
composition algebra from `specs/patch-composition-algebra.md` (analyst-
side spec) and surfaces conflicts as the user navigates.

### 12.5.3 Inspect view

For a selected tune (whether from library or about-to-flash):

```
a personal tuner-on-FA24 v3 — patch breakdown

  Total patches: 1,342 (90,060 bytes vs stock)

  Layer 1: OEM tables (editable)
    Throttle - Target Throttle: 16 tables, 352 cells
    Boost - Boost Targets: 5 tables, 80 cells
    Ignition - Compensation - Coolant: 22 cells
    ...

  Layer 2: Tuner additions (advanced)
    a community swap-basemap vendor HPFP retune @ 0x8000 (1.4 KB)
    a community swap-basemap vendor AVCS targets @ 0x89b8 (1.7 KB)
    ...

  Layer 3: Code patches (READ-ONLY)
    UDS dispatch retarget @ 0x1ff040 (2.7 KB) ⚠

[Back] [Flash this]
```

This is the architectural-classifier (`src/devices/ets/src/architectural_classifier.cpp`, shipped today) output, rendered on the handheld's LVGL screen. The cross-compiled classifier is byte-identical to the desktop's, so the same tune displays identically on both surfaces.

### Safety constraints specific to library + compose

- Patches in Layer 3 cannot be edited on the handheld. The inspect view shows them read-only; editing requires the desktop GUI with explicit confirmation (§16 live-tune model).
- Compose conflicts at Layer 3 abort by default; require an explicit `--force-code-conflicts`-equivalent screen gesture to override.
- Storage caps: max N=20 patch sets in the library; older entries auto-evict (LRU). The user can pin entries.
- The handheld's library is a CACHE of patch sets delivered from elsewhere (desktop SubuwuTuner, wireless OTA, USB-bridge import). The handheld doesn't author new patch sets standalone — that's a desktop-side workflow (use the existing `subuwutuner-cli` + GUI).

---

## 13. What the AP3 RE taught us (2026-06-11)

Between 2026-06-09 and 2026-06-11 the project reverse-engineered the
COBB AccessPort V3's complete tune format (`docs/34`) and USB protocol
(`specs/references/cobb-ap3-usb-protocol.md`). The findings reshaped
this v6 of the handheld plan in several specific ways.

### What AP3 got right (and we adopt)

| AP3 design | Why it's good | How we adopt |
|---|---|---|
| **File-vault model for tune library** (one folder, named entries, list + push + pull + delete) | Maps cleanly to user mental model; persistent across power cycles | §12.5.1 tune library screen + USB-bridge mode (§8.5) |
| **Tune-as-patch-set, not as ROM image** | ~50× storage efficiency (50 KB vs 2 MB per tune); enables composition | Whole v6 tune model (§5 + §12.5.2) |
| **Marriage-state interlock against wrong-car flash** | Catches "I forgot which car this AP is set up for" | Soft-marriage variant (§5 last subsection) — same safety value without the anti-user lock-in |
| **CID-keyed stock-backup interlock** | No flash without verified stock for THIS ECU | Already in v5; reaffirmed |
| **Plain-HTTP OTA model** (no DRM, signed `.img` files) | Simple, debuggable, third-party-validatable | ESP32 OTA path (§9 D6) — signed but otherwise open |
| **Per-cmd-byte dispatcher with explicit safe/unsafe classifications** | Clear surface area for safety review | Inter-MCU protocol §6 + USB-bridge protocol §6.7 each have a documented op list |
| **CSV.gz datalog format** | Open, portable, viewable in any tool | Handheld's SD-side datalog format adopts CSV+gzip (was already considered; AP3 confirms the choice) |

### What AP3 got wrong (and we avoid)

| AP3 mistake | Why it's bad | What we do instead |
|---|---|---|
| **Cipher keys hardcoded in client + firmware** | Single shared key across millions of devices = compromise = full corpus deencrypted | SubuwuTuner uses BLAKE3 + per-package signature (`docs/05` §4) — no per-device shared secret |
| **No HMAC on DeviceSettings blob** | Single-byte XOR patches against at-rest encrypted settings can flip marriage state | Manifest BLAKE3 covers everything; no plain ciphertext at rest in v6 |
| **Multiple commands daze the firmware** (cmd 0x18 infinite loop; cmd 0x12 empty body wedges state machine; malformed body shape wedges USB pipe) | A bug in one handler can wedge the whole device; replug-only recovery | Teensy firmware uses a clean state-machine with timeout-based recovery; every command path has a defined-failure mode that returns control |
| **MSYS path-mangling-style ambiguities silently corrupt operations** | Caller's environment can mangle the request without the device knowing | All handheld file-vault paths are validated against a strict schema before processing |
| **No per-tune signature** (any .ptm with valid cipher + valid romSum can be installed) | A maliciously crafted .ptm flashes without provenance | Manifest signature is part of every flash; tunes from untrusted sources go through an explicit "untrusted source — review before flash" UX |
| **Hard marriage requires JTAG to undo** | Hardware is paywalled to one car forever; anti-user | Soft marriage (§5) |

### Specific RE artifacts we leverage

- **Cipher chain understanding** lets us implement `.ptm` import/export in `docs/34` Capability A.1 — without ever distributing keys (the implementation is gated behind `ST_ENABLE_COBB_AP_CIPHER`). The handheld inherits this via cross-compiled code.
- **Patch-format spec** (`specs/private-data-xml-to-stune-mapping.md`) gives us the schema for serializing patch sets on SD.
- **Composition algebra spec** (`specs/patch-composition-algebra.md`) gives us the formal model for §5 + §12.5.2.
- **Per-CID layer maps** (`src/devices/ets/src/architectural_classifier.cpp`, shipped) give us the §12.5.3 inspect rendering.
- **Live-USB protocol experience** (the AP3 USB byte-channel + dispatcher pattern) informs the handheld's USB-bridge mode (§6.7 + §8.5) — we know what works, what dazes the firmware, and what threat model to plan for.

### What the AP3 RE did NOT change

- §0 Prime directive (unchanged across all revisions)
- §2 Architectural spine (Teensy is sole safety brain)
- §3 ESP32 as dedicated radio peripheral
- §4 Wireless not in flash safety path (hard rule)
- §7 Hardware/electrical brick-protection (brownout interlock primacy)
- §8 (and §8.5 extension) physical arm-to-flash interlock
- §11 Clean-room methodology — `.ptm` import/export is clean per `docs/15` §6 (read the cipher from publicly-distributed binaries the user has on their machine; never paste the key into the SubuwuTuner repo; gate behind a build flag for users who knowingly opt in)

---

## Out of scope

- Reimplementing `st::ecu::*` / `st::flash` (shared, not rebuilt).
- Any flash path that bypasses the shared orchestrator, the stored-stock
  interlock, or the §4 rule.
- Wireless in the live flash transport path (the §4 rule).
- ~~Competitor-proprietary tune-file formats (encrypted `.ptm`-style containers) / locked-ECU decryption.~~ **Removed 2026-06-11** — the `.ptm` format is now reverse-engineered (per `docs/34` + `specs/references/cobb-ap3-usb-protocol.md`) and ingest/export is supported, gated behind `ST_ENABLE_COBB_AP_CIPHER`. The handheld's tune library can hold patch sets imported from `.ptm` files (the cipher chain runs desktop-side; the handheld receives signed SubuwuTuner-format patch sets, not raw `.ptm`).
- Locked-ECU decryption (out of scope; SubuwuTuner reads ROMs only from ECUs the user owns).
- Hard marriage / JTAG-recoverable hardware locks (we use soft marriage instead — §5).
- ELM327-style write paths (`docs/13` non-goal).
- Cloud / always-online dependency (`docs/00` non-goal). Wireless is
  local-link only — no telemetry servers.
- Custom AP3 firmware modification (handheld interoperates with AP3 as
  documented in `docs/34`; modifying COBB's firmware itself is JTAG-community
  work, not SubuwuTuner work).

---

*Update after each D-milestone. Subordinate to the real docs at all times.*
