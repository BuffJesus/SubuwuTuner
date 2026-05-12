# 09 — Risks & Mitigations

Ordered by expected impact × probability.

## R1 — A flash bug bricks a customer ECU

- **Impact:** existential — single most likely thing to end the project
- **Probability:** moderate without aggressive controls
- **Mitigations:**
  - Phase 4 gate of 100 successful bench flashes before any customer touches it
  - Brick-protection shim installed and read-back-verified before the first user write
  - HIL CI runs every night against real ECUs (Tier 4)
  - Dry-run mode mandatory in onboarding flow
  - Mutation testing on `st::flash` blocks releases (see `08-testing-strategy.md`)

## R2 — IP challenge from an existing tuning vendor

- **Impact:** high — could force a rewrite or shutdown
- **Probability:** low if we follow `06-legal-ethics.md`
- **Mitigations:**
  - Clean-room workflow documented per file; no decompilation of any third-party tool
  - All code original; data drawn only from public sources (RomRaider, SAE standards, vendor-documented APIs, owner-supplied test fixtures)
  - Apache 2.0 license keeps our own output unencumbered
  - When in doubt about whether a public artifact is safe to reference, document the question and get written legal advice before proceeding

## R3 — Emissions regulator action

- **Impact:** medium-to-high depending on jurisdiction
- **Probability:** low for a general-purpose tuning tool; higher if we ever ship pre-built defeat calibrations
- **Mitigations:**
  - Tool itself is jurisdiction-neutral and exposes ECU functionality; the legal exposure of *using* it sits with the user
  - We do not ship "delete preset" calibrations as first-party content
  - We do not strip emissions markers / calibration IDs from ROMs
  - First-run wizard sets a jurisdiction profile so the UI gives the user accurate context for where they are (see `06-legal-ethics.md`)
  - Project is not US-incorporated; legal exposure profile differs from US-based commercial tuning vendors

## R4 — Protocol reverse-engineering takes longer than expected

- **Impact:** schedule slip in Phase 3–4
- **Probability:** high — VB UDS particulars are not fully documented in public
- **Mitigations:**
  - Use RomRaider as a reference; many of the unknowns are already solved for VA
  - Ship Phase 1–3 features (read-only viewer, editor, ELM datalogging) without VB write support if needed
  - Recruit a community contributor with a VB and a J2534 device early

## R5 — Qt licensing/cost surprise

- **Impact:** medium — would force a UI rewrite
- **Probability:** low with LGPL dynamic linking
- **Mitigations:**
  - Confirm LGPL compliance plan with a lawyer before public release
  - Keep `st::ui` thin and porting-friendly so an ImGui fallback is feasible

## R6 — Maintainer burnout / bus factor of one

- **Impact:** high
- **Probability:** moderate for a hobby project
- **Mitigations:**
  - All planning lives in this repo (you are reading it now)
  - Definitions are TOML so the community can submit PRs without learning C++
  - Build is one CMake preset + vcpkg manifest — anyone can spin up in 30 minutes
  - Document a "minimum viable parking" mode: keep CI green, accept PRs only

## R7 — Tactrix or OBDLink change their wire protocol

- **Impact:** medium
- **Probability:** low (vendors care about stability)
- **Mitigations:**
  - `st::transport` is an interface; each adapter is a separate module
  - Keep current-vendor SDKs pinned; upgrade on a schedule, not on push

## R8 — A definition pack ships with a wrong scaling that damages an engine

- **Impact:** high
- **Probability:** real — community-contributed packs will have bugs
- **Mitigations:**
  - Every shipped pack is reviewed and signed by a project maintainer
  - In-app warning on packs without a maintainer signature
  - Definition format requires unit annotations; the editor refuses to display a map whose units don't match the column header

## R9 — Apple notarization friction

- **Impact:** medium for Mac users
- **Probability:** moderate
- **Mitigations:**
  - Start notarizing in CI from Phase 0, not at release time
  - Ship Homebrew cask as fallback distribution
