# Disclaimer

**SubuwuTuner is provided "as is" with no warranty of any kind. Use at your own risk.**

## What can go wrong

ECU tuning is the act of reading, modifying, and writing the embedded computer that controls your engine. Things that can go wrong, listed from most to least common:

1. **Engine damage.** A calibration with the wrong air-fuel ratio, the wrong ignition timing, or the wrong boost target can cause knock, pre-ignition, lean conditions, and bearing failure. Engines can be ruined in seconds.
2. **Bricked ECU.** A failed flash, an unexpected power loss during a flash, or a corrupted image can leave the ECU unable to boot. Recovery usually requires bench equipment and may require replacement.
3. **Voided warranty.** Reflashing the ECU is detectable by the manufacturer and will typically void powertrain warranty coverage.
4. **Regulatory exposure.** Depending on your jurisdiction and the changes you make, your vehicle may no longer be legal to operate on public roads.
5. **Loss of insurance coverage.** Some insurers consider any ECU modification grounds to deny a claim.

## What SubuwuTuner does to reduce risk

- Installs a brick-protection recovery shim before the first user write
- Verifies recovery shim by read-back before proceeding
- Refuses to flash on bad battery voltage or a flaky cable handshake
- Refuses to flash a calibration with open engine-safety warnings (dangerous AFR/timing)
- Supports dry-run mode that exercises every step except the final write
- All flash operations produce a tamper-evident manifest you can keep

None of this changes the fact that **you are the one initiating the action**. You are responsible for understanding what you are doing.

## What SubuwuTuner does NOT do

- It does not verify that your calibration is safe for your specific engine, fuel, hardware, or environment.
- It does not check whether the modifications you are making are legal where you operate the vehicle.
- It does not insure against any of the above failure modes.

## Recommended practices

- Read a stock dump and keep it safe before doing anything else.
- Use the dry-run mode at least once before your first real flash.
- Have a battery charger on the vehicle during any flash operation.
- Don't tune on a car you can't afford to repair.
- Get a wideband O2 sensor and a knock-monitoring setup before pushing the engine.

## License

This software is licensed under the [Apache License, Version 2.0](LICENSE). The license explicitly disclaims all warranties; this document is informational and does not enlarge the license's warranty terms.
