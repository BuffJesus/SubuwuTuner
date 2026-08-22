# 50 - Replacement bench ECU intake checklist

Use this before buying or powering a replacement ECU for the VA WRX bench
rig. The goal is a known, reversible test target—not merely an ECU that fits
the connector.

## Preferred target

- Same ECU family and board generation as the existing rig.
- Exact CID match to the available reference image and definition pack when
  possible. For the current bench path, the strongest known donor reference is
  the LF79002P 2 MiB FA20DIT image.
- Matching hardware/part number and transmission/market variant.
- Seller can provide label photographs and, ideally, a powered identification
  read before shipping.

Do not substitute a neighboring LF/LH CID merely because it is also a VA WRX.
That may be useful for a separate cross-CID research target, but it is not a
drop-in replacement for the first hardware validation run.

## Ask the seller for

- Clear photos of the ECU label, connectors, case, and any visible board
  revision/MCU marking.
- Calibration ID/CID, hardware part number, model year, transmission, and
  market.
- Whether it was previously tuned, recovered, opened, or exposed to water or
  over-voltage.
- Whether the ECU powers up and communicates, and what tool produced that
  result.
- A return window long enough to perform identity, readback, and bench-harness
  validation.

## Intake sequence after arrival

1. Photograph the package, label, connectors, and case before opening it.
2. Record CID/part number and compare them to the intended definition pack.
3. Inspect for corrosion, bent pins, board contamination, and previous rework.
4. Verify harness continuity and current-limited bench power before connection.
5. Power on with no write capability armed; observe current draw and regulator
   behavior.
6. Capture passive CAN traffic and request identity only.
7. Read a small immutable ROM slice three times and compare the bytes.
8. Read the full image, hash it, and preserve the original as read-only.
9. Compare the image against the exact-CID reference and run definition/boot
   signature checks.
10. Create the project Stock checkpoint and only then consider a deliberate
    write.

## Automatic rejection conditions

Stop and return/quarantine the donor if:

- CID or hardware identity is unknown;
- the part number/variant does not match the intended rig target;
- the ECU is already silent or intermittently drops communication;
- the ROM cannot be read repeatably;
- the only available reference is a neighboring CID;
- the source image has unexplained boot/signature/checksum differences;
- the seller cannot establish a return path.

## First safe milestone

The donor is accepted only after it passes:

- exact identity;
- stable read of a small slice;
- repeatable full-image read;
- hash and immutable Stock checkpoint;
- definition and boot-signature checks;
- power-cycle persistence.

No tune, swap workflow, custom feature, or checksum experiment belongs in the
first session with a replacement ECU.
