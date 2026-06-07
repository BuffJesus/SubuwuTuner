# Sniff workflows — using the Y-cable for everything that isn't SA capture

> A catalog of reverse-engineering recipes built on top of `subuwutuner-cli sniff`. The Security Access capture path lives in `docs/23-security-access.md`; this doc covers the *other* uses of the same hardware setup.

## Why a separate doc

The Y-cable + OBDX-Pro-VX-in-LISTEN-ONLY rig described in `docs/23` is general-purpose. SecurityAccess capture is the most license-load-bearing use (because it unblocks ROM dumps without GPL contamination), but the same rig can pull a tune off the wire during a flash, locate live RAM addresses for cal tables by watching what an active tuner-tool polls, and characterise unknown ECU behaviour by observing how the OEM-blessed tools session-shape their requests.

Status: **all workflows in this doc are hardware-gated.** Capture path (`sniff` subcommand, listen-only mode, ISO-TP-aware extractor) is shipped and tested against MockTransport. OBDX Pro VX is in hand (2026-05-24) and the install-flow captures + B6 cipher recovery have run end-to-end against real CAN logs (analyst-side findings); live-car verification of the documented workflows is in progress.

## Shared hardware setup

Identical for every recipe in this doc:

- **OBDX Pro VX** in LISTEN-ONLY (no TX, no ACK).
- **OBD-II Y-splitter cable** (passive, ~$15). Both adapters share CAN-H/CAN-L on the multi-drop bus.
- **The active tuning tool** (any aftermarket flasher or commercial ECU-tuning application) on the other branch of the Y, doing whatever the workflow needs to capture.

`subuwutuner-cli sniff` opens the VX with `LinkConfig::listen_only=true`, which both skips adapter-level CAN filtering (every frame on the bus reaches the host) and configures EnableNetwork STATE=0x02 so the VX physically cannot transmit. Two transmitters fighting over CAN-H/CAN-L would corrupt the active tool's session; LISTEN-ONLY makes the OBDX invisible to the bus.

Output format is one frame per line: `<elapsed_ms> 0xCANID <hex bytes>`. Plain-text, grep-friendly, parseable by the Python helpers under `tools/`. `--filter <id>[,<id>...]` trims the file by CAN ID before writing.

---

## Workflow 1 — Sniff-during-flash (extracting the tune from a write)

**Goal.** Recover the calibration bytes for the tune currently on the car without ever issuing an RMBA or RoutineControl ourselves. Captures whatever the active tuning tool sends down the wire during a flash. Particularly valuable when:

- ROM-read path is blocked (no SecurityAccess algorithm yet, RMBA gated at a higher SA level than 0x01, an aftermarket-tool-locked ECU rejects external reads).
- The on-disk copy of the tune is encrypted-at-rest (the aftermarket flasher's PC management app keeps customer ROMs encrypted; the tuner has the plaintext only transiently during a flash).
- We want ground truth for what a known-working flash sequence looks like before our own Flasher tries to replicate it.

### Capture

```sh
subuwutuner-cli sniff \
    --transport obdx --device COM5 \
    --output flash-capture.log \
    --filter 0x7E0,0x7E8
```

Then perform a flash through the active tool. End-to-end a Subaru CAN flash is ~30–90 s of solid bus traffic; the capture file ends up a few MB.

The frames of interest, in order:

1. **DiagnosticSessionControl** `10 02` (programmingSession) — `50 02` positive response.
2. **SecurityAccess** seed/key exchange (typically level 0x01, sometimes a separate level 0x03 or 0x05 for programming).
3. **RoutineControl** start-routine `31 01 FF 00` or similar — erase-flash routine. Subaru-specific routine IDs.
4. **RequestDownload** `34 00 44 <addr 4B> <size 4B>` — declares the upcoming transfer's address and length. `00 44` = format byte (compression=none, encryption=none) + address-and-length-format (4B address, 4B length).
5. **TransferData** `36 <block_seq> <data...>` — repeated, dozens to hundreds of times. Each block carries one chunk of the payload. Block sequence wraps at 0xFF.
6. **RequestTransferExit** `37` — closes the transfer.
7. (Possibly more RequestDownload/TransferData/RequestTransferExit cycles for different memory regions — RAM patches, calibration, kernel.)
8. **RoutineControl** check-programming-dependencies / verify-checksum.
9. **ECUReset** `11 01` or session return to defaultSession.

### Extract

The extractor `tools/extract_uds_transfer.py` walks the capture, ISO-TP-reassembles each direction, pairs each `34 …` declaration with its subsequent `36 …` blocks, and emits:

```json
{
  "schema": "subuwutuner.flash.v1",
  "session": "programmingSession",
  "transfers": [
    {
      "address": "0x00040000",
      "declared_size": 524288,
      "received_size": 524288,
      "format_byte": "0x00",
      "addr_len_format": "0x44",
      "block_count": 2048,
      "payload_sha256": "…",
      "payload_path": "flash-capture.region-0x00040000.bin"
    },
    …
  ]
}
```

Each `payload_path` is the reassembled bytes for that transfer. **Whether those bytes are the tune as-it-hits-flash depends on the active tool's payload envelope.** RequestDownload's dataFormatIdentifier (the `00` in `34 00 44 …`) is the OEM-side declaration of compression / encryption applied to the *transferred* bytes; a value of 0x00 means the ECU writes the wire bytes verbatim. Tuner tools often layer their own outer envelope on top (proprietary container formats) that the ECU's bootloader strips before flashing — or pre-process the calibration into a delta against a known base. Treat the extracted payload as "what the wire saw" and validate against a known-pre-flash baseline (or a same-CID stock ROM, if available) before trusting it as the calibration in flash. Non-zero `dataFormatIdentifier` makes this unambiguous: the wire bytes are not the flash bytes.

### Caveats

- **Encrypted flash payloads.** If the format byte is non-zero (compression or encryption), the bytes on the wire are *not* the bytes that hit flash. Subaru historically uses 0x00 (none) for OEM flash dataset writes but a tuner-tool might wrap its payload. Format byte is the discriminator.
- **Partial flashes.** Some tools only re-write the calibration region (~256 KB) and leave the kernel/bootloader alone. The capture reveals which addresses changed; addresses not seen in any `34 …` declaration retain their pre-flash content. Combine with a pre-flash dump if you want a full image.
- **Block counters wrap.** Subaru's blockSequenceCounter wraps at 0xFF → 0x00 (not 0xFF → 0x01). The extractor must track wrap count, not just the counter byte.
- **Bricking risk to the active tool's flash, not yours.** Sniffing is passive — the active tool is the one with brick exposure. Don't interrupt the active tool's flash mid-stream by yanking the Y-cable.

---

## Workflow 2 — Datalogger sniffing (RAM address discovery)

**Goal.** Identify the RAM addresses of live calibration "shadow" copies and engine state variables by watching what a tuner-tool polls when displaying its datalogger.

### How the discovery works

When an aftermarket flasher's PC app shows a live "Boost Target" gauge, it polls a fixed RAM address every 50–200 ms via ReadDataByIdentifier or ReadMemoryByAddress. The address is the live (current) value of that quantity — which is fed by, e.g., the boost target lookup table in calibration. Knowing the RAM address gives you:

1. **A grep target for the disassembly** — RAM address Y is referenced by code that reads from cal table Z, exposing Z's flash location even when the definition file doesn't list it.
2. **A correlation handle** — start a feature (launch control, flat-foot, rev limit), watch which polled values change in lockstep, infer which RAM address tracks the feature's state.
3. **A confirmation channel** — the in-tree definition packs claim cal table Z is at address W; sniffing confirms the active tool reads W's shadow value during normal operation.

### Capture

```sh
subuwutuner-cli sniff \
    --transport obdx --device COM5 \
    --output datalog-sniff.log \
    --filter 0x7E0,0x7E8 \
    --duration 60
```

While capturing, run the tuner-tool's datalogger with whatever PIDs you want to locate. Drive the car or idle it through a few interesting conditions (cold start, WOT pull, deceleration, gear changes).

### Extract

The extractor `tools/extract_rmba_polls.py` walks the capture and emits:

```json
{
  "schema": "subuwutuner.poll.v1",
  "polls": [
    {
      "address": "0xFFFF1234",
      "size": 4,
      "poll_count": 287,
      "poll_period_ms_p50": 100,
      "value_samples": ["00 00 0C 80", "00 00 0D 20", ...]
    },
    …
  ]
}
```

Per address: how often it was polled, period statistics, and a sliding window of returned values (so you can see what range it covers across the capture).

### Pairing addresses to features

The interesting part isn't the address list, it's correlating polls to driving conditions:

1. Capture a baseline (idle, neutral, no inputs) for 10 s.
2. Trigger the feature (engage launch-control conditions, hit the rev limiter, etc.) for 10 s.
3. Compare value distributions per address between the two phases.

Addresses whose value distribution **changes** between phases are the candidates for that feature's live state. The Workflow-4 feature recipes below all reduce to this loop.

### Cross-referencing the tuner-tool's CSV against the sniff

The "what tuner-tool was polling" question above tells you *which* RAM
addresses get touched but not *which monitor name* sits at each address.
If the active tuner-tool exports a labeled datalog CSV (any of the
aftermarket flashers' PC apps emits one), you can recover the
address-to-monitor mapping by joining the CSV against the sniff
capture in time.

The CSV gives you: per-row timestamp + labelled monitor values. The
sniff gives you: per-frame timestamp + raw poll-response bytes. The
join requires a per-log **time anchor** between sniff time and the
CSV's internal time (typical aftermarket-flasher CSVs start at t=0
when the user pressed "log", no wall-clock).

Practical anchor: find a strongly-varying CSV column (engine RPM is
ideal — fast dynamic range during a WOT pull) and search for a sniff
byte position whose value trajectory matches the CSV column's curve
over some sniff time window. The offset `sniff_t = csv_t + Δ` that
maximizes Pearson r² between the two curves is the per-log time anchor.

Once anchored, every other monitor in the CSV becomes a target:

1. For each AP CSV column `M` and each candidate `(DID, byte_offset,
   encoding)` in the captured polled-DID response:
2. Build `(raw_value, csv_value)` pairs across the anchored polls.
3. Fit `csv_value = a × raw + b` via OLS. Score by r².
4. Reject candidates whose extrapolated **idle-state prediction**
   differs from the CSV's idle value by more than ~10% of the
   monitor's dynamic range. (Many bytes correlate with RPM during
   driving but don't sit at the right value at idle; this culls them.)
5. Prefer candidates whose slope is close to a canonical Subaru
   scaling factor (×0.25, ×0.0625, ×0.0145, etc.) — non-canonical
   slopes are usually correlation-only fits.
6. Enforce 1-to-1 monitor↔byte assignment: greedy by descending
   score, but no two monitors can claim overlapping bytes in the same
   DID payload.

This methodology recovers the polled-byte layout *without needing a
ROM disassembly*. It complements the disassembly path: when the
disassembly arrives, it's the ground truth; the empirical layout is
how you know which monitors to look for in the handler.

**Bootstrapping the very first anchor**: the methodology is
chicken-and-egg — you need a verified RPM byte to find the anchor,
but you need the anchor to verify RPM. Resolve by checking two
extreme states. Engine idle (low RPM, ~800) and engine WOT peak
(~6000+ RPM) both have known CSV values. For any candidate byte
position + scaling, decode the value at the sniff's globally
quietest period (engine off / cranking) and at the sniff's most
RPM-correlated peak. If both extremes match the CSV's idle and WOT
values respectively, you've found RPM. Lock it in, then find the
per-log anchor offsets.

### Caveats

- Aftermarket flashers typically poll over UDS (`22 <DID>`) for
  everything. The DIDs in the OEM-reserved space (~0x0000–0x00FF)
  carry the standardised quantities; the DIDs in proprietary
  ranges (e.g. the 0xF3xx range observed on one mainstream
  handheld) carry batches of multiple monitors packed into one
  ~80-byte response. RMBA (`23 <addr>`) shows up for one-off RAM
  reads (e.g. during a flash dump) but is rare in normal
  datalogging on modern handheld-flasher firmware.
- **Multi-byte ISO-TP reassembly is adapter-firmware dependent.** The
  OBDX VX with its default `AutoProcess` flag reassembles
  multi-frame responses automatically — each line in the sniff log
  contains the full payload, not the constituent 8-byte CAN frames.
  This is convenient for analysis but means a future firmware change
  could silently break multi-frame captures. If you see only First
  Frames (length byte starts with `0x1`) without their continuation
  frames, the reassembly is off and you'll need to glue Consecutive
  Frames (`0x21`, `0x22`, …) together yourself.
- **Frame drops cap your usable polls per log.** A daily-driver
  laptop sniffing through a USB-serial adapter under contention
  (bus busy, other USB activity) can lose 80%+ of polls during
  high-cadence chassis CAN. Mitigation: drop the chassis CAN noise
  with `--filter "0x7E0,0x7E8"` (note the quotes — see "Quote
  PowerShell CLI args with hex / commas" in the field doc), use a
  dedicated capture machine, or fall back to a J2534 transport
  that buffers in adapter firmware rather than over the host USB.
- The address space varies by silicon era. SH7058 RAM is 0xFFFF0000–0xFFFFFFFF; RH850 is different. The extractor should annotate which silicon the addresses fit.
- Polling interval is tuner-tool-dependent and tunable. Don't assume 100 ms is universal.

---

## Workflow 3 — Protocol learning (session shape, service discovery)

**Goal.** Resolve "what session sequence does the OEM tool use before service X works?" without trial-and-error against a live ECU that might lock SA after 3 wrong tries.

### Discoveries this unblocks

- **Which DiagnosticSession (`10 XX`) gates which service.** `read_full_rom` failing with NRC 0x7F because the ECU is in defaultSession when RMBA needs extendedDiagnosticSession.
- **Which SA level (`27 01` vs `27 03` vs `27 05`) gates which address range.** Some Subaru ECUs gate calibration-region reads at level 0x01 but kernel-region reads at a higher level.
- **TesterPresent (`3E 80`) cadence.** Sessions auto-expire at 5 s of silence; the OEM tool's TP cadence reveals the actual S3 timeout in use (sometimes 2 s, sometimes 5 s).
- **Subaru-specific service IDs and routine IDs.** Anything in the 0xB0–0xBF or routine-ID 0xFF00 ranges that isn't ISO-standard.
- **CommunicationControl (`28`)** — does the OEM tool quiet the rest of the bus before flashing? Subaru BIU/TCM chatter can starve the engine ECU's ISO-TP reassembly if not silenced.

### Capture

Wide-net, no ID filter:

```sh
subuwutuner-cli sniff \
    --transport obdx --device COM5 \
    --output protocol-learn.log \
    --duration 120
```

Drive the OEM tool through whatever operation you're trying to characterise. Reconnect, read DTCs, do a key-on calibration ID query, dump a single map, etc.

### Extract

`tools/decode_uds_capture.py` walks the capture and emits a human-readable timeline:

```
00.000  → 7E0  10 02                   DiagnosticSessionControl programmingSession
00.012  ← 7E8  50 02 00 32 01 F4       positive, P2=50ms, P2*=5000ms
00.020  → 7E0  27 01                   SecurityAccess requestSeed level=0x01
00.034  ← 7E8  67 01 DE AD BE EF       seed
00.050  → 7E0  27 02 12 34 56 78       SecurityAccess sendKey
00.061  ← 7E8  67 02                   unlocked
…
```

NRCs decoded by table, routine IDs annotated with their meaning when known (per `docs/13`), session-state tracked across the timeline.

The same extractor should highlight **anomalies**: services we don't have framing for, NRCs we haven't documented, response timings that violate ISO-14229 expectations. Those rows are leads.

---

## Workflow 4 — Feature reverse-engineering

**Goal.** Locate the calibration tables that control specific tuner-visible features by combining Workflow 2 (RAM address discovery) with controlled driving inputs.

The unifying pattern: trigger condition X → sniff what RAM addresses change or get polled → cross-reference to known cal tables in the definition pack.

### 4a — Rev limiter (engine RPM cut)

**What the cal looks like.** A single scalar (max engine RPM, often two: fuel-cut RPM and resume RPM with hysteresis). Sometimes a 1D table indexed by coolant temperature (cold-engine softer limit). Sometimes a 1D table indexed by vehicle speed (no-rev-limiter-in-neutral exception).

**Sniff recipe.**

1. Baseline capture at idle, neutral, warm engine.
2. Rev the engine in neutral against the limiter (3 rapid touches).
3. In the capture: addresses whose value cycles up to a max and snaps back are the live RPM (or live limiter-active flag).
4. Find the address with a stable value matching the rev limit RPM during the touch — that's the active-limiter-target shadow address.
5. Grep the ROM disassembly for instructions referencing that address. The cal table that feeds it appears as a `MOV.W @(disp, GBR), Rn` (SH7058) or `ld.w` (RH850) from the cal table's flash address.

### 4b — Vehicle speed limiter

**What the cal looks like.** Single scalar (speed cap in km/h or specific units), enabled/disabled by a flag, sometimes a 1D table by gear.

**Sniff recipe.** Same as 4a but trigger by holding the car at the speed limiter on a closed course or rolling road. Watch for an address whose value pegs at the limit during the cap event and goes free below it.

### 4c — Launch control

**What the cal looks like.** A multi-input gate: clutch pedal pressed + brake pressed + WOT + neutral or 1st gear + vehicle speed < threshold → engage limiter at LC-target RPM (typically 4500–5500). The cal pack has clutch switch input address, brake switch input address, LC target RPM scalar, LC engage/disengage hysteresis, and the LC-active rev-cut implementation (often a more aggressive cut than the redline limiter — full ignition cut rather than fuel cut).

**Sniff recipe.**

1. Capture a baseline at idle in neutral, clutch out.
2. Trigger LC conditions: clutch in, brake on, neutral, throttle stab to WOT. Hold 3 s. Release.
3. Diff: addresses whose value transitions to a launch-specific value during the 3 s are the candidates. Expect to see:
   - The live target-RPM address change to the LC target.
   - A "feature-engaged" flag flip on.
   - Possibly an ignition-cut counter or timing-retard delta address show non-zero values.
4. The clutch / brake input bits are most likely already on CAN (BIU broadcasts them on a periodic frame); sniff the wider bus, not just 7E0/7E8, to find which CAN ID + bit they live in.

### 4d — Flat-foot / no-lift shift (FFS / NLS)

**What the cal looks like.** Modifier on the rev-limit / boost-control behaviour when the gate "throttle WOT + clutch in + speed > threshold" is true. Boost target is held instead of dropping (no-lift) and the rev limiter is softened or briefly disabled (flat-foot). Implementation varies — some tunes do "hold IAM and boost target, suppress ignition-cut rev limit for 500 ms post-clutch-in," others do full fuel-cut suppression.

**Sniff recipe.**

1. Baseline at WOT in a gear with the clutch out (recorded straight-line pull).
2. Trigger FFS conditions: WOT in gear, clutch in, hold 0.5 s, clutch out, continue WOT.
3. Diff: the addresses that change during the clutch-in window and *return to baseline* on clutch-out are the FFS-active modifiers. Look for:
   - Boost-target shadow holding flat instead of decaying.
   - Ignition-timing target holding instead of being cut.
   - A short-lived counter / timer address ticking during the suppression window.
4. The clutch switch state-change is the event of interest; if it's on CAN (it usually is — BIU broadcasts it for cluster lighting) you can timestamp the trigger precisely from the capture.

### 4e — Other features the same recipe locates

- **Cold-start enrichment** — capture cold-start vs warm-start, diff the addresses whose values differ during the first 60 s post-key-on.
- **Closed-loop / open-loop transition** — diff addresses during WOT pull vs cruise to find the open-loop flag and its trigger thresholds.
- **DCCD / DCCD-Auto (STI only)** — diff per-driving-mode (Auto-+/-, Manual full-lock vs full-open).
- **Map-switching** — Subaru ECUs often have multiple parallel cal tables (e.g., for Sport / S# / Intelligent modes). Diff captures across mode-switch events to find the map-selector index address.
- **Speed-density / MAF-blending region edges** — diff captures across MAP/RPM regions to identify where the blend factor changes; the blend factor's RAM address shadows the blend cal table.

The general recipe is always **baseline / trigger / diff / cross-reference**. Workflow 2's extractor (`tools/extract_rmba_polls.py`) is the engine for the diff step; what changes per feature is the trigger and the cross-reference target.

---

## Output formats and downstream tooling

Every workflow ends in JSON the rest of the toolchain can consume:

| Workflow | Extractor (status)               | Output schema                  | Downstream consumer                                   |
|----------|----------------------------------|--------------------------------|-------------------------------------------------------|
| 1        | `tools/extract_uds_transfer.py` (shipped) | `subuwutuner.flash.v1`         | `st::rom::load_buffer` → `st::edit` for delta tuning  |
| 2        | `tools/extract_rmba_polls.py` (shipped)   | `subuwutuner.poll.v1`          | Definition annotator: enrich `definitions/<era>/*.toml` with RAM shadow addresses |
| 3        | `tools/decode_uds_capture.py` (shipped)    | Plain-text timeline + anomaly list | `docs/13-transport.md` updates; ECU-quirk database    |
| 4        | (combination of 2 + manual diff)  | Per-feature notes              | Definition pack: new tables / new annotations         |

Workflow 1's payload bytes drop straight into the existing ROM-loading path; the others feed back into the definition packs and protocol docs.

---

## Clean-room and legal posture

All four workflows produce **facts about the car** — addresses, byte sequences on the wire, observed values. Facts aren't copyrightable. The line stays the same as everywhere else in the project (see `docs/15-clean-room-engineering.md`):

- **Capture and analyse** OEM-tool wire behaviour freely — it's behavioural observation, not source access.
- **Don't decompile** the OEM tool to figure out what to look for. Sniff first; if the answer isn't visible on the wire, that's a research-it-some-other-way problem, not a decompile-the-tool problem.
- **Don't paraphrase** OEM-tool UI strings, internal identifiers, or feature names into the SubuwuTuner codebase. "Launch Control" is a generic term used by every automaker; a vendor-specific spelling of an internal flag is not.
- **Captured ROM bytes from Workflow 1 belong to the vehicle owner.** Sniffing a flash on your own car gives you a calibration file you may use for your own tuning. Redistributing it depends on the upstream tuner's terms (cf. `project_ecutune_terms.md` for an example of a no-redistribute clause); SubuwuTuner-the-project never bundles user-captured tunes.
- **Path B distribution still applies.** Anything Workflow 1 / 2 / 4 produces about specific vehicle calibrations stays off the public repo; the *tooling and recipes* in this doc are public.

The plug-in pattern from `docs/23` (algorithm in a downstream fork, infrastructure in upstream SubuwuTuner) generalises: extractors that parse generic ISO-TP / UDS belong in upstream; extractors specialised to one OEM tool's quirks may belong in a fork.

## See also

- `docs/13-transport.md` — listen-only mode and transport adapter shape.
- `docs/14-can-reverse-engineering.md` — the broader CAN RE toolkit; Workflow 4 reuses BaselineModel / ChangeDetector for the diff step.
- `docs/23-security-access.md` — SA capture and the seed/key plug-in pattern.
- `docs/15-clean-room-engineering.md` — the wall: what you can and can't carry across.
- `docs/17-data-distribution-policy.md` — why captured calibration bytes don't ship in the public repo.
