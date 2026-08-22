# 55 - VA WRX OEM STI DCCD and combination-meter integration

This document tracks an owner-authorized, factory-style VA-generation WRX
drivetrain conversion using an STI six-speed transmission, OEM DCCD controller,
console controls, and optional STI combination meter. It is a module-integration
and CAN-discovery project, not an engine-ECU feature patch.

## Current conclusion

An OEM-style conversion is demonstrably possible within the VA generation, but
the exact year/trim compatibility matrix and required CAN signals are not yet
evidenced in this repository. The best available public build evidence separates
two operations that are often described together:

- The STI DCCD controller and supporting hardware operate the transmission's
  center-differential coil.
- A dealer registers the replacement combination meter to the vehicle and
  programs the keys. That is an immobilizer/replacement-module operation; it is
  not evidence that the dealer flashes a DCCD algorithm into the WRX ECM.

Reference build video: [Finishing Touches on STI Swap Interior on the FA WRX](https://www.youtube.com/watch?v=oRnOvXAnY2A).
The page discussion explicitly identifies combination-meter registration and key
programming as the dealer work. Treat the exact installed parts and wiring as
unverified until independently inventoried.

## System boundary

Keep these workstreams separate:

1. **Mechanical drivetrain:** STI six-speed, DCCD coil, final-drive-compatible
   rear differential, driveshaft, axles, clutch, and related hardware.
2. **DCCD control:** OEM STI DCCD controller, power/ground/relay circuits,
   console controls, direct inputs, CAN inputs, and center-differential output.
3. **Combination meter:** DCCD indication, warning behavior, donor compatibility,
   odometer handling, VIN/configuration, and immobilizer registration.
4. **WRX ECM calibration:** calculated-gear thresholds/ratios for the STI gearset.
   A correct gear display does not prove correct DCCD operation.
5. **BIU/key authorization:** replacement-meter and key registration described in
   `docs/52-immobilizer-swap-integration.md`.

Do not infer that the combination meter controls DCCD. The working architecture
to test is that the DCCD controller drives the differential and publishes state
used by the meter for display.

## What SubuwuTuner can do now

`subuwutuner-cli sniff` can capture raw CAN frames through an OBDX Pro VX in
hardware listen-only mode:

```powershell
subuwutuner-cli sniff `
  --transport obdx `
  --device COM5 `
  --output va-sti-dccd.log `
  --duration 300
```

For discovery captures, omit `--filter`. An engine-only filter such as
`0x7E0,0x7E8` would discard the periodic chassis frames most likely to carry
DCCD, VDC/ABS, steering-angle, and combination-meter state.

The sniffer writes one frame per line as
`<elapsed_ms> 0xCANID <hex bytes>`. The OBDX path has processed real CAN logs;
the J2534 streaming path is not the supported live-sniff path today.

### Known tooling gap

The offline CAN-discovery commands consume `.asc`, while `sniff` writes the
SubuwuTuner v1 text log. A lossless `sniff`-log-to-`.asc` adapter, or direct
`sniff_common`/`.cdb` ingestion, is required before the existing `can-diff` and
`can-discover` workflow is seamless for this project. Preserve raw logs as the
source artifact; do not hand-normalize timestamps or discard unknown IDs.

## Minimum running-STI capture set

A complete donor car is not required. A cooperative VA STI owner and a passive
OBD capture are sufficient for the first stage. Record exact model year, market,
keyed versus push-button ignition, combination-meter part number if accessible,
and DCCD controller part number if accessible. Avoid collecting or publishing
VIN/key identity data that is unnecessary for signal discovery.

Use separate files with a spoken or written action log:

1. Ignition on, engine off, controls untouched (30 seconds).
2. Engine idling, controls untouched (30 seconds).
3. Select Manual, then each lock level with a five-second pause.
4. Select Auto, Auto+, and Auto- where equipped, five seconds each.
5. Press/release brake, clutch, and parking brake separately.
6. Turn steering left, center, and right while stationary where safe.
7. Drive straight at low speed, then make gentle left and right turns.
8. Repeat the parked sequence on the recipient VA WRX as a baseline.

The logger is passive, but the operator must not interact with a laptop while
driving. Use a passenger or preconfigured timed capture. Dynamic tests belong on
a safe road, closed course, or chassis dynamometer as appropriate.

## Analysis plan

1. Inventory IDs, rates, DLCs, counters, and checksums in untouched baselines.
2. Diff one controlled action at a time. Start with Manual/Auto and discrete
   lock steps; these are easier to identify than continuously varying Auto mode.
3. Compare STI and WRX baselines to classify frames as shared, STI-only,
   gateway-filtered, or inconclusive.
4. Correlate candidate inputs with wheel speed, brake, clutch, steering angle,
   yaw/lateral acceleration, throttle/torque, and DCCD mode/command.
5. Validate every proposed signal against a second capture. A changing byte is
   a candidate, not a definition.
6. Export only validated signals to a draft DBC with provenance, vehicle year,
   bus location, and confidence.

If expected traffic is absent at the diagnostic connector, use official wiring
information to identify the DCCD-controller or combination-meter CAN pair and
capture there with a non-invasive breakout. The BIU may gateway-filter another
vehicle network; software cannot recover frames that never reach OBD pins 6/14.

## Loose-module and bench path

Low-cost salvage parts can advance the project before a running STI is available:

- DCCD controller with connector and generous pigtail
- Console DCCD controls with connector
- STI combination meter with connector pigtails
- Relevant harness sections and exact donor metadata
- Optional matched BIU/key components for authorized replacement-registration
  research; these are not required for initial passive DCCD CAN discovery

Before powering a loose controller, establish connector identity, supply pins,
grounds, wake/ignition inputs, bus pins, termination, expected voltage, and
current limiting from authoritative wiring information. First power-on is a
current-limited bench operation. Do not guess pins from a neighboring model year.

Bench questions, in order:

1. Does the controller communicate diagnostically and identify itself?
2. Does it broadcast with ignition/wake present and no other modules?
3. Which recorded stock-STI frames are necessary to leave fault/default mode?
4. Which control inputs are direct-wired versus CAN-carried?
5. What message drives the combination-meter DCCD indication?
6. Does any DCCD controller operation depend on VIN or immobilizer state?

Replay or injection is deliberately later than passive discovery and requires a
separate safety review. Never replay a whole capture onto a live vehicle bus.

## Combination-meter registration evidence

The meter/BIU/ECM/key relationship belongs to the owner-authorized replacement
workflow in `docs/52-immobilizer-swap-integration.md`. Required future evidence:

- Recipient and donor model years, trims, ignition types, and part numbers
- Pre/post replacement DTC and module-identity scans
- Exact authorized service operation names and results
- Where lawful and owner-authorized, pre/post non-secret configuration or memory
  comparisons that exclude reusable key/credential material from public output
- Odometer/VIN handling and applicable disclosure records

Do not describe meter registration as DCCD flashing without an observed DCCD
controller reprogramming transaction.

## Status and gates

- `[x]` Confirm shipped OBDX listen-only raw CAN capture path.
- `[x]` Separate DCCD control, cluster display, ECM gear calculation, and
  immobilizer registration in the research model.
- `[ ]` Add lossless v1 sniff-log ingestion to the CAN discovery pipeline.
- `[ ]` Obtain authoritative year-specific DCCD/controller/meter wiring facts.
- `[ ]` Capture a recipient VA WRX baseline using the scripted sequence.
- `[ ]` Obtain at least one stock VA STI capture with complete metadata.
- `[ ]` Repeat on a second capture before promoting any signal to a DBC.
- `[ ]` Inventory the public build's actual controller, meter, switch, and
  harness part numbers rather than inferring them from appearance.
- `[ ]` Bench-identify a loose DCCD controller with current-limited power.
- `[ ]` Observe an authorized combination-meter replacement/registration
  procedure or obtain equivalent owner-provided service evidence.
- `[ ]` Validate a factory-style conversion without suppressing safety DTCs or
  misrepresenting odometer/VIN state.
