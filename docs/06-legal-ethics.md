# 06 — Legal & Ethical Considerations

This is a port motivated by curiosity and software craft, but ECU tuning has real legal and safety implications. The rules vary wildly by jurisdiction. This document captures the project's posture; the **user** is the one responsible for knowing what's legal where they operate.

## Emissions — jurisdiction-aware, user-configurable

Atlas takes a hard prohibition stance: it refuses outright to assist with emissions-equipment changes, period. That stance makes sense for a Delaware-incorporated entity selling globally, but it is more conservative than the law in many places where SubaruTuner will be used.

The primary developer's jurisdiction is **Alberta, Canada**, where:

- There is no provincial light-duty vehicle emissions inspection program.
- The federal *Canadian Environmental Protection Act* (CEPA) and the *On-Road Vehicle and Engine Emission Regulations* technically prohibit modifying a "vehicle's emission control system" once in use, but enforcement against individual private owners modifying their own vehicles is in practice minimal.
- Track-only, motorsport, and off-road use carry their own carve-outs that vary case by case.

So instead of Atlas's blanket refusal, SubaruTuner takes the position that **the tool exposes what the ECU exposes, and trusts the user to know their local rules**. We are not in the business of being a regulator inside the user's own software.

### What that means concretely

- **No hard refusal** on emissions-relevant edits. The user can change anything the ECU lets us change.
- **Editor still flags** emissions-relevant maps in the UI (yellow corner badge, tooltip). The flag is informational, not blocking. The user can dismiss it once per project and never see it again.
- **Per-jurisdiction profiles** ship in-box: `alberta-ca`, `california-us`, `eu-roadworthy`, `motorsport-only`, etc. Each profile decides:
  - Which warnings are shown
  - Whether the EmissionsLinter blocks-flash, warns-on-flash, or stays silent
  - Whether DTC-suppression for emissions codes is one click or hidden behind a confirmation
  - Default value of "is this car road-registered?"
- **Default profile** on first run is `motorsport-only` (least restrictive). The first-run wizard asks the user to pick the profile that matches them.
- **No "delete preset" tunes shipped in-box.** Reason: distributing a finished, ready-to-flash anti-emissions calibration is a different legal category in most places than building a tool that can produce one. Community feature-packs that do this are allowed but unsigned and marked as such; that's the user's choice.

### What we still won't do

- We will not ship calibrations targeted at specific defeat goals (e.g., a packaged "EGR delete + cat delete + O2 spoof" preset) as a first-party feature.
- We will not strip or obscure emissions-related markers from a ROM (e.g., editing the calibration ID string to hide that it's been modified). Tuning is fine; lying about it is not.
- We will not assist with evading inspection systems that *do* exist (e.g., faking readiness monitors during an active OBD-II inspection cycle). That's a different problem than calibration editing.

### EmissionsLinter, redefined

`EmissionsLinter` still exists as a module but its default mode is **advisory** rather than blocking:

| Profile | Behavior on emissions-flagged edit |
|---|---|
| `motorsport-only` (default) | Silent. No badge, no warning. |
| `alberta-ca` | Yellow badge in UI. No flash-time prompt. |
| `eu-roadworthy` | Warning on save. Confirmation on flash. |
| `california-us` | Confirmation on save *and* on flash. Reason field required. |

The user can override per-profile. The point is to give a knowledgeable user accurate context, not to hide behind a refusal.

## Intellectual property

Atlas is closed-source and free-for-personal-use. We must not:

- Decompile Atlas and translate its source
- Embed Atlas's encrypted definition files in any redistributable
- Copy Atlas's icon set, screenshots, or UI text verbatim
- Use the name "Atlas" in our branding

What we **can** do:

- Re-implement features we observe Atlas providing — features are not copyrightable, expression is
- Use **RomRaider** (GPL) protocol code as a reference, provided we credit it and respect GPL; **easier path:** treat it as a spec, write our code clean-room, and ship under our own permissive license
- Use public Subaru ECU documentation, SAE standards, ISO 14229 / 15765
- Use the user's own legally-obtained ROM dumps and the user's own purchase of `.atlas` files as private test fixtures (not redistributed)

## License of SubaruTuner itself

Two real options:

1. **MIT or Apache 2** — maximal community, hosts can build commercial spinoffs, we cannot easily prevent a competitor from forking.
2. **AGPL** — keeps any hosted or modified fork open; deters commercial freeloading but reduces business-friendly adoption.

**Recommendation:** Apache 2.0 with a `NOTICE` file. Aligns with most C++ ecosystem libraries, doesn't poison downstream, and the patent grant matters in an industry with real patents.

## Warranty and safety

Reflashing an ECU can damage an engine if the calibration is wrong, and can brick the ECU if the flash routine itself is wrong. We will:

- Ship with a prominent `DISCLAIMER.md` and an in-app first-run consent screen
- Refuse to flash without the user passing a one-time "what is brick protection" knowledge check
- Refuse to flash on a known-bad battery voltage or known-bad cable handshake
- Refuse to flash a calibration the linter has open *unsafe-for-engine* warnings on (e.g., AFR way off, ignition advance with no knock-protection map adjustment) — these are *engine safety* warnings, distinct from emissions warnings, and they stay blocking by default in every profile

## Distribution channels

- GitHub Releases — source + binaries, code-signed on Win/Mac
- No app stores in v1 (Apple's review process is hostile to OBD tools)
- No anonymous binary mirrors — every published binary has a BLAKE3 hash in the release notes and a GPG signature on the tag

## Export controls

Standard "no embargoed countries" clause. ECU tuning is not on US dual-use lists, but be mindful if we ever add encryption beyond standard TLS.

## A note on this project's posture vs Atlas's

Atlas's repo carries strong emissions-prohibition language. SubaruTuner takes a different view because:

1. The relevant law is **the user's local law**, not the developer's preferred policy.
2. Refusing to expose ECU functionality the ECU itself exposes is paternalism, not safety.
3. The actual safety issue with tuning is **damaging the engine or bricking the ECU**, and we put significant engineering into preventing both (see `05-improvements.md` and `08-testing-strategy.md`). That's where our refusal-to-act lives.

If a user is in California, they will pick the `california-us` profile and the tool will warn them appropriately. If a user is on a track car in Alberta, they will pick `motorsport-only` and the tool will get out of their way.
