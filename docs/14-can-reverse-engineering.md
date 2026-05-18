# 14 — CAN Reverse-Engineering Toolkit

This is the design doc for a SubuwuTuner subsystem aimed at people doing engine swaps, cluster integration, or general "what does this byte mean?" reverse-engineering on a vehicle's CAN bus. The standard workflow today is SavvyCAN + manual diffing across captures, which is slow, error-prone, and tedious for the hundredth signal. This subsystem makes that loop *programmatic*: the tool watches the bus, detects deviations from baseline, prompts the user to describe what they were doing, and produces a structured discovery file that exports to a draft DBC.

**Status (2026-05-17):** the **replay path is shipped** — `st::can` (Frame, `.asc` reader/writer), `st::dbc` (parser/emitter/decoder), `st::discover` (`BaselineModel`, `ChangeDetector`, `.cdb` Bundle), and the five CLI subcommands `can-replay`, `can-diff`, `can-discover --from`, `can-export-dbc`, `can-decode` are all in `main` and unit-tested. The **live path** waits on a transport adapter implementing `start_streaming` with CAN-shaped frames; until then the workflow runs against `.asc` captures from SavvyCAN or another tool. See "Implementation order" below for the per-step status.

## Goals

- **Watch-and-label workflow.** User triggers a function (blinker, brake, gear lever); tool detects the change; user types one line of context; tool records a structured event. Repeat until the discovery file is full.
- **Replay-first.** Capture cheaply to disk as raw frames; run the discovery algorithm over the file. Live mode reuses the same algorithm, just streaming. This is the same MockTransport-vs-real-adapter pattern we use everywhere else.
- **Stable output format.** `.cdb` (CAN Discovery Bundle) is TOML, diffable, hand-editable, importable into the LLM-assisted refinement step described below.
- **Multi-bus from the start.** Most modern vehicles have at least HS-CAN and MS-CAN; the design tracks bus identity per frame so two-bus captures don't get confused.
- **Reuses `st::transport::ITransport`.** Live capture is just `start_streaming(FrameCallback)` with a CAN-shaped frame; no parallel transport layer.

## Non-goals

- **Not a SavvyCAN replacement** — we don't build a full visual signal-grapher or message-rate plotter. Use SavvyCAN for visual inspection; we focus on the scriptable / batch path.
- **No anti-theft / immobilizer reverse-engineering.** Different category, different legal posture, different problem. Out of scope.
- **Not a real-time fault-injection / fuzz-the-bus tool.** Read-only by default; writing to the bus is a separate Phase-6+ concern with its own safety story.
- **No CAN-FD optimization in v1 of this subsystem.** Designed to accommodate it (64-byte payloads) but the algorithm is tuned for the 8-byte classic-CAN case first.

## Module map

```
src/can/
├── include/st/can.hpp           (Frame, BusId, log read/write)
└── src/

src/discover/
├── include/st/discover.hpp      (BaselineModel, ChangeDetector,
│                                 DiscoverySession, DiscoveryEvent)
└── src/

src/dbc/
├── include/st/dbc.hpp           (Database, Message, Signal,
│                                 parser, emitter)
└── src/

src/cli (shipped subcommands; see `subuwutuner-cli --help` for the
authoritative usage)
  can-replay    out.asc                              # per-id stats summary
  can-diff      a.asc b.asc                          # SavvyCAN-style diff
  can-discover  --from out.asc [--baseline <secs>]
                [--bus <0..3>] [--output session.cdb]
  can-export-dbc session.cdb [--output draft.dbc]
  can-decode    --dbc subaru.dbc out.asc [--output signals.csv]

src/cli (future, gated on live transport)
  can-record    --bus hs --duration 60s out.asc      # live capture
  can-discover  ... --live session.cdb               # live discovery
```

Dependency direction:
- `st::can` depends only on `st::core` and `st::transport`
- `st::discover` depends on `st::can`
- `st::dbc` is standalone (no transport dep), used by the export step and by `can-decode`

## Discovery workflow

### Baseline phase (default 10 seconds)

For each CAN ID observed:
- Record arrival rate (Hz).
- Per byte position, classify as:
  - **Stable** if all samples in baseline agree, OR if the byte stays within a small variance band (`< 4` distinct values, configurable).
  - **Cyclic** if the byte cycles through a deterministic pattern (counter, rolling cyclic value).
  - **Noisy** otherwise (analog signal, RPM, etc.).
- Cache the typical (mode) value of each stable byte.

This is `BaselineModel`. It's cheap to compute (single pass over the baseline) and survives serialization to the .cdb so subsequent sessions don't have to re-baseline if the user trusts a prior one.

### Watch phase

For each incoming frame:
1. If the ID is new (not in baseline) → emit a `NewIdEvent`.
2. For each known ID, walk its bytes:
   - If a **stable** byte differs from its baseline mode → that's a candidate change.
   - **Cyclic** bytes ignored.
   - **Noisy** bytes ignored unless they exceed a configurable rare-deviation threshold (default off).
3. If any candidate change is found, emit a `ChangeEvent` and pause the watch loop for user input.

### Labeling

When a change fires, the tool:
- Prints the affected ID, bytes, before/after values.
- Prompts on stdin: `> describe this event: `
- The user's line becomes the event's `description`.
- A short debounce window (default 500 ms) suppresses duplicate-event spam.

### Output: `.cdb` (CAN Discovery Bundle)

```toml
schema_version = 1
captured_at    = "2026-05-11T..."
bus            = "hs"
baseline_seconds = 10

[[baseline.id]]
can_id      = 0x140
freq_hz     = 100.0
stable_bytes = [0, 1, 6, 7]
cyclic_bytes = [4]
noisy_bytes  = [2, 3, 5]

[[event]]
timestamp   = "2026-05-11T12:34:56.789Z"
can_id      = 0x140
bytes       = [6, 7]                       # which byte positions changed
before      = [0x00, 0x00]
after       = [0x01, 0x00]
description = "left blinker on"

[[event]]
timestamp   = "..."
can_id      = 0x215
bytes       = [0]
bits        = [4]                           # optional bit-level refinement
before      = [0x80]
after       = [0x90]
description = "brake pressed"
```

This is the durable artifact. A subsequent run can pick up where the user left off, and the export step works against it offline.

### Export: `.cdb` → DBC

`can-export-dbc session.cdb > draft.dbc` produces a candidate DBC where:
- Each unique `can_id` becomes a `BO_` (message) entry.
- Each labeled event becomes a `SG_` (signal) entry whose:
  - **Name** = slugified `description` (e.g. `"left blinker on"` → `left_blinker_on`)
  - **Start bit, length** = inferred from `bytes` + (if present) `bits`
  - **Scaling** = identity (factor=1, offset=0) — user refines manually or via the LLM step
  - **Byte order, sign** = default to big-endian unsigned; user refines

The DBC is a *draft*. Loading it with cantools / SavvyCAN and re-decoding the original capture lets the user verify each signal against what they remember happening.

## Change-detection algorithm — details that matter

A few specifics worth pinning down before implementation:

- **Hysteresis on stability.** A byte declared "stable" in baseline gets a small allowed deviation band (e.g. `±1` for slowly-drifting values like coolant temp encoded as raw uint8). Without this, slow analog signals would spam changes during baseline.
- **Debouncing.** When a change fires, the watch loop pauses for a debounce window (default 500 ms) before resuming. Without this, a bit that toggles on/off rapidly (blinker LED state at 1 Hz) generates one event per cycle.
- **Multi-byte coalescing.** If multiple bytes of the same ID change in the same frame, they're surfaced as one event with multi-byte payload — usually they're parts of the same logical signal.
- **Cyclic counter detection.** Bytes that strictly increment-mod-N during baseline are tagged `cyclic` and ignored during watch. Cyclic counters are a common Subaru pattern; without filtering them we'd get a change event every frame.
- **Rate-of-change classifier.** "Noisy" bytes (RPM, MAF, etc.) need an opt-in to participate — by default they're filtered out because the user usually cares about discrete button presses first.

These are knobs on `ChangeDetector`. Sensible defaults, all overridable from the CLI / config.

## LLM-assisted refinement (optional, post-capture)

The .cdb format is intentionally structured for LLM consumption. The workflow:

1. User runs `can-discover` over a drive; ends up with a session.cdb file containing ~50 labeled events.
2. User pastes the .cdb (or relevant excerpts) into a chat session with an LLM.
3. LLM proposes refined signal definitions: bit position within byte, sign convention, scaling, plausible engineering units based on the description.
4. User pastes refined output back into a `[[refinement]]` section of the .cdb, or applies via a future `can-refine` CLI command.
5. `can-export-dbc` consumes the refinements when producing the DBC.

The LLM is doing the part it's actually good at — pattern-matching against known signal layouts in similar vehicles, proposing scalings that produce plausible engineering values. The human is doing the part the LLM is bad at — saying with certainty whether `0xC4` at byte 0 of ID `0x140` corresponds to brake light on, off, or partially applied based on what they remember pressing in the car.

Never autonomous. The LLM produces candidates; the human validates against captured behavior.

## Implementation order

1. ✅ **`st::can::Frame`** type + `.asc` reader/writer (Vector's text log format — the lingua franca SavvyCAN, Wireshark, and cantools all consume).
2. ✅ **`st::dbc::Database`** parser + emitter (consumed by `can-export-dbc` and `can-decode`).
3. ✅ **`st::discover::BaselineModel`** + **`ChangeDetector`** — replay-mode, unit-tested with synthetic Frame sequences (no transport needed).
4. ✅ **`can-discover --from <log.asc>`** offline mode — works against any pre-captured log.
5. ✅ **`.cdb` writer + reader** (`Bundle::to_toml` / `from_toml`).
6. ✅ **`can-export-dbc`** producing draft DBCs.
7. ⬜ **Live mode** — once a real adapter implements `st::transport::ITransport::start_streaming` with CAN-shaped frames. Same algorithm, different input source. Gated on the bench rig.
8. ✅ **`can-diff`** — SavvyCAN-Discrepancies analog for users who prefer that workflow.

Each step is independently shippable and unit-testable. Items 1–6 + 8 are in `main`; item 7 (live capture) waits on transport platform wiring (see docs/13).

A hypothetical `can-record` CLI verb is *not* shipped — it would require live transport — so capture today routes through SavvyCAN (or any tool that writes `.asc`) and feeds offline into `can-discover --from`.

## Open questions

- **Extended (29-bit) vs standard (11-bit) IDs.** Default to supporting both; differentiate in the .cdb format with `extended = true`.
- **CAN-FD payloads (up to 64 bytes).** Design handles them; tuning the change detector for them may need work later.
- **Gateway-filtered buses.** Modern Subarus may not expose all CAN traffic on the OBDII connector. Document the workaround (back-probing at the CCM); no software fix possible.
- **Live capture buffering when the user is slow to label.** Should the watch loop keep buffering and queue events? Or block the bus consumer? Probably buffer; a 500-ms-debounced ring buffer is fine for the rate of human-driven events.

## Why this is worth building

The existing tools are great if you already know what you're doing. The discovery loop is what beginners need and even experienced reverse-engineers benefit from — it removes a layer of clerical work and produces an artifact you can hand back to a future session. The .cdb being LLM-friendly amplifies the benefit further: human is in the labeling loop where intuition matters, LLM helps with the bit-fiddling refinement, and the tool stays scriptable and testable on synthetic inputs throughout. That's the same pattern that's worked across the rest of SubuwuTuner so far.
