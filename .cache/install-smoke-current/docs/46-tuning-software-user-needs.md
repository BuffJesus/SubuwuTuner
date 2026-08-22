# Tuning-software user needs and SubuwuTuner priorities

This is a product-design synthesis from public tuner discussions and current
vendor material. It is intentionally about user-facing workflow and does not
use protected implementation details as product requirements.

**Implementation update (2026-08-02):** the project now has an offline
Readiness/Flash Review path with exact-CID approved-region visibility, plus
Tables-sidebar Map Explorer facets for text, category, and safety/emissions
discovery. Live ECU identity and recovery evidence remain explicit blockers.

## What users consistently value

- **Openness and ownership.** RomRaider users value a free, open workflow, but
  newer ECU coverage and unavailable definitions are recurring pain points.
  Users also describe commercial seed/key and definition ecosystems as a form
  of lock-in. See the recent [WRX RomRaider/ECUFlash discussion](https://www.reddit.com/r/WRX/comments/1p3pnr5/can_romraider_populate_and_ecutek_tune_if_plugged/)
  and the [EcuTek versus Cobb discussion](https://www.reddit.com/r/WRX/comments/188t0nu/ecutek/).
- **A map editor that explains itself.** EcuTek explicitly promotes logical
  map trees, real engineering values, ordering by class/user level/function,
  swappable axes, and a rotatable 3D view. Its product sheet also makes ROM
  compare and copy-across changes first-class operations. See the
  [ProECU feature sheet](https://download.ecutek.com/marketing/factsheets/EcuTek%20Fact%20Sheet%20-%20ProECU.pdf).
- **Logging that answers a tuning question.** Users want more parameters,
  custom/user-defined PIDs, external wideband and auxiliary sensor inputs,
  good gauges, and the ability to log without carrying a laptop. The HP Tuners
  community specifically discusses unsupported parameters, user-defined PIDs,
  external analog inputs, and laptop-free logging in its
  [VCM Scanner discussion](https://forum.hptuners.com/showthread.php?50083-Questions-to-those-who-tune-E78-PCM=)
  and [Subaru HPTuners discussion](https://www.reddit.com/r/WRXSTi/comments/1gd0qdd/).
- **Compare as a learning tool.** Newer users repeatedly mention stock-versus-
  tuned comparison as one of the most useful ways to learn what a calibration
  changed. The same pattern appears in discussions of [tune-file comparison
  for beginners](https://www.reddit.com/r/ECU_Tuning/comments/1fobo6o/hey_everyone_im_new_to_ecu_tuning/).
- **Coverage without fear.** People want one approachable program that can
  grow across vehicles, but do not want an automated “stage” button to hide
  the actual calibration decisions. A recent community discussion describes
  the basic workflow as logging, read/write, understanding binaries, and
  reviewing targets versus delivered values; that is the right mental model
  for SubuwuTuner too.

## Comparison with SubuwuTuner

| User need | Current position | Priority |
|---|---|---|
| Open, local, inspectable workflow | Strong foundation: offline ROM inspection, public definition packs, audit/history, and hardware-independent editing | Preserve |
| Safe mutation and flash review | Strong foundation: policy gates, checksum/status checks, typed confirmation, and semantic changed-table preview | Finish the project/readiness surface |
| Logical map organization | Present in parts, but table role, confidence, related maps, and learning level are not yet a unified experience | P0/P1 |
| Stock/current/tune comparison | Diff groundwork exists; make checkpoints and side-by-side semantic comparison first-class | P0 |
| High-quality logs and graphing | The UI has a Log workspace, but live acquisition, signal profiles, markers, map tracing, external channels, and derived PIDs need a complete workflow | P1 after hardware recovery |
| User-defined parameters | Definition facts and aliases are developing; add safe user-defined signals/derived channels without implying ECU-code changes | P1 |
| Beginner-to-expert progression | Command palette, help, glossary, and demo path are good foundations; guided tuning tasks and “why this matters” field notes are missing | P1 |
| Coverage across ECU families | VA/VB packs and analyst-side family work are active; exact-CID confidence and architecture-specific gates are the differentiator | Continuous |
| Freedom from opaque licensing | Strong product opportunity: keep local artifacts, hashes, provenance, and capability status visible | Core principle |

## The product opportunity

SubuwuTuner should be the **calm, trustworthy calibration notebook** that can
eventually become a full tuning instrument. The differentiator is not trying to
out-neon a commercial editor. It is making every important answer visible:

1. What calibration am I looking at?
2. What changed, and why?
3. What evidence supports this definition or signal?
4. What will this edit affect?
5. Is the current artifact internally valid, policy-allowed, and actually safe
   to send to this identified ECU?

That gives the classical side of the design its hierarchy and repeatability,
while the romantic side comes from field notes, good typography, meaningful
history, and a sense that the software respects the craft.

## Recommended implementation order

### 1. Project Readiness

Make the opening project view answer identity, definition-pack status, source
and working ROM hashes, changed-table count, checksum state, policy blockers,
and the one best next action. This is useful now with a bricked or disconnected
ECU.

### 2. Calibration Compare

Add named checkpoints (`stock`, `baseline`, `after-maf`, `ready-to-flash`),
semantic changed-table summaries, delta heatmaps, and a comparison mode that
can align sibling CIDs only when the pack attests the alignment.

### 3. Map Explorer

Organize tables by purpose and task, with search, aliases, units, axis meaning,
risk, provenance, confidence, and related tables. Keep the expert grid one
click away; do not replace it with wizard-only editing.

### 4. Log Explorer

Build signal profiles, synchronized plots, event markers, map-cell tracing,
derived channels, external wideband support, and a clean import/export format.
When the hardware is back, add live acquisition and read-back validation behind
explicit capability gates.

### 5. Coverage and collaboration

Make exact-CID packs, sibling comparisons, definition lint, provenance, and
reproducible research artifacts easy to share. Coverage should expand through
attested evidence, not by making uncertain names look authoritative.

## Immediate hardware-independent work

- Keep decompiling the permitted sibling corpus and seed only architecture-level
  facts until names are independently supported.
- Turn the existing VA/VB/EP5G/EZ1G evidence into a searchable local signal and
  table catalog.
- Implement the Project Readiness and Calibration Compare surfaces against ROM
  files and fixtures.
- Design Log Explorer around imported logs first, so the workflow can be tested
  without a functioning ECU.
