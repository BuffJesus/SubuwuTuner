# Clean-room methodology

SubuwuTuner is a clean-room reimplementation. This page is the short
read; the authoritative doc is
[`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }.

## The wall

**Two roles, separated.**

- **Analyst.** Reads protected references (commercial-tool ROMs,
  decompilations, dispatcher disassemblies). Produces **factual
  specifications** in plain English, in a different repository
  (`SubuwuTuner-specs/`). Never touches SubuwuTuner code.
- **Implementer.** Reads the specs and the public protocol standards
  (ISO 14229, ISO 15765, SAE J2534, J1979, J2012). Writes the C++.
  Never reads the protected references.

The wall is enforced by:

- **Output isolation.** Analyst output lands in `SubuwuTuner-specs/`,
  never in this repo.
- **Tool restrictions.** Specific paths are off-limits to file-read /
  web-fetch tools for any task producing code, specs, or docs destined
  for the repo. The list is in CLAUDE.md.
- **Naming hygiene.** Implementer names types from first principles or
  from the spec, not from the protected reference's identifiers.

## What protected means here

- **RomRaider** (GPL) — protocol facts are fair game; Java source is
  off-limits (would be GPL contamination of Apache 2.0).
- **Atlas** (`motorsportsresearch/atlas-public`, All Rights Reserved) —
  source-available, not open source. Concepts are fair game; source is
  off-limits.
- **Commercial tools** (COBB, EcuTek, HP Tuners, OEM tuning software) —
  no source, no decompilation, no string-literal reuse.
- **OEM ECU firmware** — protocol facts are factual; OEM-authored
  identifiers and prose are not redistributed.

## The idea / expression line

**Ideas are free; expression is owned.**

- A node-graph custom-feature designer — idea — build one freely.
- A specific node class hierarchy or file format copied from Atlas —
  expression — off-limits.
- "Subaru's flash sequence uses UDS DSC 0x10 0x02 then RequestDownload
  0x34" — fact — usable.
- "OEM-authored description prose for table X" — expression —
  re-emit fresh.

## Solo-dev adaptations

The two roles need not be two people; one developer can play both,
**at different times**, with the wall enforced by output isolation and
session typing. Analyst-mode sessions launch from
[`docs/analyst-mode-prompt.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/analyst-mode-prompt.md){ target="_blank" } —
the only sanctioned way to bring protected references into an AI
context.

## AI-tool contamination channels

AI tools have their own contamination surface beyond what a human would.
Particular care points:

- **`web_fetch` and `Read`** can pull protected source into a session
  and from there into the codebase. Off-limits paths listed in
  CLAUDE.md.
- **Training-data knowledge.** If the AI would write something
  "because that's how Atlas does it," that origin disqualifies the
  implementation. Write from first principles or from the spec.
- **Pasted excerpts.** Refuse and flag. Don't silently launder.

## Path B distribution

Separate axis from clean-room compliance. The tool ships public; VA/VB
calibration packs are user-supplied. Reasoning:
[`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" }.

## Deeper detail

- [`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" } — full methodology.
- [`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" } — Path B distribution choice.
- [`docs/06-legal-ethics.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/06-legal-ethics.md){ target="_blank" } — emissions / jurisdiction policy.
- [`THIRD-PARTY-INSPIRATIONS.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/THIRD-PARTY-INSPIRATIONS.md){ target="_blank" } — what we studied, what we built.
