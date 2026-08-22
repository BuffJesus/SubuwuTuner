# 52 - OEM-ECU engine-swap integration

The goal is to run newer Subaru engines and their capable factory ECUs in
older or different chassis without requiring a standalone ECU. This requires
preserving owner-authorized start authorization and reconstructing every
chassis service the donor ECU expects. Flash SecurityAccess and aftermarket
anti-theft calibrations are separate systems.

## Implemented

- `[x]` Extract 2017 WRX facts from the official service manual.
- `[x]` Separate start authorization from UDS/SSM flash authentication.
- `[x]` Correct the unsupported claim that the keyed link is LIN.
- `[x]` Add keyed Type-B and push-button Type-D machine-readable profiles.
- `[x]` Add a tested module-presence and registration-readiness audit.
- `[x]` Record E158-43, immobilizer fuel-cut status, registration classes, and diagnostic codes.

## Authorization and module integration

- `[ ]` Complete connector-level topology for keyed and push-button systems.
- `[ ]` Extract complete IM(diag)/KPS(diag) DTC and symptom matrices.
- `[ ]` Map start authorization state machines without publishing secret material.
- `[ ]` Locate ECM consumers of immobilizer fuel-cut state in clean OEM ROMs.
- `[ ]` Identify RAM state, timing, output gates, and diagnostic publication.
- `[x]` Add project swap manifests for retained, replaced, donor-matched, and registered modules.
- `[ ]` Add no-start diagnosis from DTCs and live fuel-cut status.

## Chassis compatibility layer

- `[ ]` Inventory ignition, crank, clutch, brake, neutral, and accelerator inputs.
- `[ ]` Inventory fuel-pump, fan, alternator, starter, A/C, and main-relay control.
- `[ ]` Inventory tachometer, coolant, MIL, oil-pressure, cruise, and warning outputs.
- `[ ]` Map required CAN producers: BIU, combination meter, VDC/ABS, TCM, steering, and keyless modules.
- `[ ]` Classify each missing message as required, degradable, cosmetic, or diagnostic-only.
- `[ ]` Build a configurable gateway profile for older chassis signals.
- `[ ]` Define safe defaults and limp behavior for deliberately absent donor modules.
- `[ ]` Add per-swap readiness reports and wiring checklists.

## Replacement-firmware completeness

- `[ ]` Specify boot, hardware, interrupt, scheduler, watchdog, and NVRAM contracts.
- `[ ]` Specify crank/cam decoding, injection, ignition, throttle, AVCS, boost, and torque control.
- `[ ]` Specify calibration ABI, live-data ABI, diagnostics, flashing, and recovery.
- `[ ]` Specify OEM authorization and chassis-gateway interfaces.
- `[ ]` Track every unknown explicitly by CID, evidence, confidence, and validation gate.

## Hardware and authorized-service gates

- `[ ]` Capture accepted and rejected start traces on an owned vehicle or bench.
- `[ ]` Identify the E158-43 electrical/protocol layer from measurement.
- `[ ]` Exercise official Type-B and Type-D replacement registration.
- `[ ]` Validate matched donor-set, retained-BIU, and replacement-ECM swap paths.
- `[ ]` Confirm crank permission versus ECM fuel/spark inhibition per platform.
- `[ ]` Validate the chassis gateway under power loss and missing-message faults.
- `[ ]` Observe an authorized VA combination-meter replacement registration and
  key-programming session for the STI-meter/DCCD integration tracked in
  `docs/55-va-sti-dccd-integration.md`.

## Evidence and safety rules

Every profile cites an authoritative manual or owned-system observation.
Module presence does not prove registration. Calibration differences do not
prove immobilizer behavior. Generic defeat patches and secret/key material are
excluded from public artifacts; pairing state, diagnostics, replacement,
recovery, compatibility, and clean-room firmware contracts remain in scope.

## Swap-manifest audit

The machine-readable profile describes what a platform requires; a separate
project manifest records what is actually installed. Each `[[module]]` entry
declares `disposition = "retained"`, `"replaced"`, or `"donor_matched"`, plus
presence, registration state, and free-form evidence. Donor matching does not
implicitly prove registration.

```powershell
python tools/immobilizer_swap_audit.py `
  fixtures/security_domains/2017-wrx-keyed.toml `
  --manifest fixtures/security_domains/examples/2017-wrx-keyed-donor-swap.toml `
  --json
```

The command exits 0 only when all required modules are present and every
replaced or donor-matched identity-bearing module that requires registration
has `registration = "registered"` and a non-empty evidence reference. Exit 1 means blocked;
exit 2 means the profile or manifest is invalid.
