# 06 — Legal & Ethical Considerations

ECU tuning has real legal and safety implications, and the rules vary wildly by jurisdiction. This document captures the project's posture; the **user** is the one responsible for knowing what's legal where they operate.

## Emissions — jurisdiction-aware, user-configurable

Some tuning software takes a blanket-prohibition stance on emissions-equipment edits — refuse outright, regardless of where the user operates. That stance makes sense for some commercial vendors but it is more conservative than the law in many places SubuwuTuner will be used.

The primary developer's jurisdiction is **Alberta, Canada**, where:

- There is no provincial light-duty vehicle emissions inspection program.
- The federal *Canadian Environmental Protection Act* (CEPA) and the *On-Road Vehicle and Engine Emission Regulations* technically prohibit modifying a "vehicle's emission control system" once in use, but enforcement against individual private owners modifying their own vehicles is in practice minimal.
- Track-only, motorsport, and off-road use carry their own carve-outs that vary case by case.

So SubuwuTuner takes the position that **the tool exposes what the ECU exposes, and trusts the user to know their local rules**. We are not in the business of being a regulator inside the user's own software.

### What that means concretely

- **No hard refusal** on emissions-relevant edits. The user can change anything the ECU lets us change.
- **Editor still flags** emissions-relevant maps in the UI (yellow corner badge, tooltip). The flag is informational, not blocking. The user can dismiss it once per project and never see it again.
- **Per-jurisdiction profiles** ship in-box (`st::policy::Profile`): `motorsport-only`, `alberta-ca`, `eu-roadworthy`, `california-us`. Each profile decides:
  - Which warnings are shown
  - Whether the EmissionsLinter blocks-flash, warns-on-flash, or stays silent
  - Whether DTC-suppression for emissions codes is one click or hidden behind a confirmation
  - Default value of "is this car road-registered?"
- **Default profile** for a new project is `motorsport-only` (least restrictive). Today the choice is per-project rather than per-install: `subuwutuner-cli project-set-profile <dir> <profile>` from the shell, or click the status-bar jurisdiction chip in the GUI. A first-run install-wide wizard is a roadmap item, not shipped.
- **No "delete preset" tunes shipped in-box.** Reason: distributing a finished, ready-to-flash anti-emissions calibration is a different legal category in most places than building a tool that can produce one. Community feature-packs that do this are allowed but unsigned and marked as such; that's the user's choice.

### What we still won't do

- We will not ship calibrations targeted at specific defeat goals (e.g., a packaged "EGR delete + cat delete + O2 spoof" preset) as a first-party feature.
- We will not strip or obscure emissions-related markers from a ROM (e.g., editing the calibration ID string to hide that it's been modified). Tuning is fine; lying about it is not.
- We will not assist with evading inspection systems that *do* exist (e.g., faking readiness monitors during an active OBD-II inspection cycle). That's a different problem than calibration editing.

### EmissionsLinter, jurisdiction-driven

`EmissionsLinter` is a module whose default mode is **advisory** rather than blocking:

| Profile | Behavior on emissions-flagged edit |
|---|---|
| `motorsport-only` (default) | Silent. No badge, no warning. |
| `alberta-ca` | Yellow badge in UI. No flash-time prompt. |
| `eu-roadworthy` | Warning on save. Confirmation on flash. |
| `california-us` | Confirmation on save *and* on flash. Reason field required. |

The user can override per-profile. The point is to give a knowledgeable user accurate context, not to hide behind a refusal.

## Intellectual property

The project operates under two distinct legal axes — see `docs/17-data-distribution-policy.md` §3 for the full framing.

### Copyright axis

No expression from a protected reference enters our codebase. The clean-room methodology, analyst/implementer wall, and audit trail live in `docs/15-clean-room-engineering.md` (which supersedes the narrower treatment in `docs/01-reverse-engineering.md` on this topic).

- **No decompilation of any third-party tuning tool.** We do not translate decompiled output into our codebase.
- **No verbatim copying of icons, screenshots, UI text, or brand assets** from any other tool.
- **No use of a third party's trademarks** in our branding.
- **Public-domain or open-license technical references are fair game** as specifications: SAE/ISO standards (ISO 14229, ISO 15765, SAE J2534/J1979/J2012), vendor-published APIs, public engine-management literature.
- **RomRaider** (GPL) is the legitimate reference for ECU protocol facts and definition data. Facts (addresses, scaling values, CRC polynomials) aren't copyrightable; we extract them via `tools/defgen/`. Expression (description prose, comments) is, and we strip it.
- **Atlas** (`motorsportsresearch/atlas-public`) is **source-available, not open-source**. Its LICENSE explicitly prohibits reproduction. Concepts derivable from public READMEs, marketing, user docs, and the user-facing Confluence wiki are fair game; source is off-limits regardless of GitHub visibility.

### §1201 / trade-secret axis

Even when expression is clean, the upstream *acquisition* of fact data can carry separate constraints — DMCA §1201 access-control circumvention, commercial-tool EULA restrictions, runtime instrumentation against protected processes. The clean-room wall in `docs/15` operates on what crosses it; it cannot cure an upstream §1201 problem.

The project's response is **Path B** (`docs/17-data-distribution-policy.md`): the public repo carries the tool plus older-Subaru community-sourced definition data (Impreza, Forester, Legacy, Liberty, Outback, Baja, Tribeca, Exiga), but does **not** bundle first-party calibration packs for the **VA WRX (2015–2021)** or **VB WRX (2022+)** platforms. Users obtain VA/VB packs themselves via `tools/defgen/` on a community RomRaider XML they've sourced, hardware capture from a ROM they legally possess, or import from a community publisher. Acceptance criteria for future first-party VA/VB packs are in `docs/17` §4.

**Owner-supplied ROM dumps** and similar legally-obtained user data are usable as private test fixtures; we do not redistribute them.

## License of SubuwuTuner itself

Two real options were considered:

1. **MIT or Apache 2** — maximal community, hosts can build commercial spinoffs, we cannot easily prevent a competitor from forking.
2. **AGPL** — keeps any hosted or modified fork open; deters commercial freeloading but reduces business-friendly adoption.

**Chosen:** Apache 2.0 with a `NOTICE` file. Aligns with most C++ ecosystem libraries, doesn't poison downstream, and the patent grant matters in an industry with real patents.

## Warranty and safety

Reflashing an ECU can damage an engine if the calibration is wrong, and can brick the ECU if the flash routine itself is wrong. The project's safety posture splits into what's shipped today and what's roadmap.

**Shipped today:**

- `DISCLAIMER.md` at the repo root.
- Plan-time policy linter: `evaluate_plan_policy` in `src/flash/include/st/flash.hpp` classifies a `FlashPlan` by engine-safety vs. emissions impact and returns a `PolicyDecision` whose `overall_action` the caller acts on. Engine-safety verdicts (`Action::Block` when any engine-safety-flagged table is touched) stay strict in every jurisdiction profile; emissions verdicts vary per profile (see above). UI and CLI surface this decision before initiating a flash.
- Per-jurisdiction policy profiles in `src/policy/` (matching the table in §Emissions above).

**Roadmap items, not yet shipped:**

- One-time "what is brick protection" knowledge check at first flash.
- Pre-flash refusal on a known-bad battery voltage or known-bad cable handshake. Transport stubs expose battery-voltage reads (`ReadVbatt` in `src/transport/include/st/transport/j2534.hpp`, `Adc` in the OBDX DVI header), but the preflight check in `src/flash/` is not yet wired against them.
- In-app first-run consent screen.

The engine-safety axis is non-negotiable: when wired, the brick-protection and battery checks will stay blocking in every profile, just like the unsafe-for-engine plan-time linter does today.

## Distribution channels (planned)

No public releases have been cut yet (`git tag --list` is empty). The intended release process when v1 ships:

- **GitHub Releases** — source + binaries.
- **No app stores in v1** (Apple's review process is hostile to OBD tools).
- **No anonymous binary mirrors.** Every published binary will carry a content hash in the release notes (BLAKE3 once `src/flash` adopts it as the firmware-hash primitive — see `src/flash/include/st/flash.hpp` for the upgrade path, currently CRC32 pending the bench rig) and a GPG signature on the tag.
- **Code-signing on Win/Mac** — not yet wired in `.github/workflows/ci.yml`; planned alongside the first release.

## Export controls

Standard "no embargoed countries" clause. ECU tuning is not on US dual-use lists, but be mindful if we ever add encryption beyond standard TLS.
