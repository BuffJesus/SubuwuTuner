# Patch: clean-room engineering policy in CLAUDE.md

**Target file:** `CLAUDE.md` (at repo root)
**Companion file (new):** `docs/15-clean-room-engineering.md`
**Patch author:** prepared for Cornelio / BuffJesus
**Date:** 2026-05-12
**Base revision:** `CLAUDE.md` as of `main` on the date above (136 lines, 12.6 KB)

---

## Why this patch

The existing **Stance on third-party IP** section in `CLAUDE.md` correctly handles RomRaider (GPL → clean-room reimplementation), but it does not address:

1. **Atlas** (`motorsportsresearch/atlas-public`). Source-available on GitHub, but the LICENSE is "All Rights Reserved" — strictly proprietary. Reading it the way you'd read RomRaider is a contamination risk because the license forbids reproduction. Needs its own named guidance.
2. **AI-tool channels.** Claude (and any other coding assistant) can introduce contamination through `web_fetch`, file viewing, or even latent training-data knowledge of protected sources. The current policy speaks to the *developer*, not to the *tool the developer is driving*.
3. **The companion design doc.** A standalone clean-room methodology document (`docs/15-clean-room-engineering.md`) is being introduced and needs to be discoverable from `CLAUDE.md`.

The patch is conservative: it extends the existing section rather than rewriting it, keeps the same voice, and adds one row to the quick-orientation table.

## Files affected

| File | Change |
|---|---|
| `CLAUDE.md` | 3 hunks — introductory paragraph, orientation table, and the third-party IP section |
| `docs/15-clean-room-engineering.md` | **New file.** The standalone clean-room methodology. Already drafted in a separate output. |

If you previously named the standalone document `docs/13-clean-room-engineering.md` (from the earlier conversation), rename it to `docs/15-...` to avoid colliding with your existing `docs/13` and `docs/14` references.

## Unified diff

```diff
--- a/CLAUDE.md
+++ b/CLAUDE.md
@@ -7,7 +7,7 @@
 
 **SubuwuTuner is a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23.** It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup.
 
-This is original work, not a port. Public references like RomRaider (GPL) are studied **clean-room** as protocol specifications — see `docs/01-reverse-engineering.md` for the boundary rules.
+This is original work, not a port. Public references like RomRaider (GPL) and source-available competitors like Atlas (All Rights Reserved) are studied **clean-room** — concepts and protocol facts only, never expression. See `docs/01-reverse-engineering.md` for the day-to-day boundary rules and `docs/15-clean-room-engineering.md` for the full methodology.
 
 ## What is already in the workspace
 
@@ -78,6 +78,7 @@
 | Plan a phase or milestone | `docs/04-roadmap.md` |
 | Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/08-testing-strategy.md` Tier 4 |
 | Reason about emissions / jurisdiction policy | `docs/06-legal-ethics.md` |
+| Reason about clean-room IP boundaries | `docs/15-clean-room-engineering.md` |
 | Reason about auto-tune | `docs/12-auto-tuning.md` |
 | Look up a tuning term you don't recognize | `docs/10-glossary.md` |
 
@@ -100,12 +101,49 @@
 
 ## Stance on third-party IP
 
-- Do **not** decompile any commercial or closed-source tuning tool.
-- Do **not** lift icons, screenshots, distinctive UI text, or trademarks from any other tool.
-- **RomRaider (GPL)** is the legitimate technical reference for ECU protocol facts. Use it clean-room: study, document the protocol in plain English, write fresh C++.
-- The `defgen` tool extracts *factual data* (addresses, scalings) from public XML — facts aren't copyrightable; expression (description text) is and gets stripped.
-- See `docs/01-reverse-engineering.md` for the full boundary rules.
+Two layers: what the developer does, and what *you, Claude,* do with your tools.
+
+### General rules (developer and assistant)
+
+- Do **not** decompile any commercial or closed-source tuning tool.
+- Do **not** lift icons, screenshots, distinctive UI text, or trademarks from any other tool.
+- **RomRaider (GPL)** is the legitimate technical reference for ECU protocol facts. Use it clean-room: study, document the protocol in plain English, write fresh C++.
+- **Atlas (`motorsportsresearch/atlas-public`, All Rights Reserved)** is *source-available, not open source*. The repo's own LICENSE file explicitly prohibits reproduction. Atlas is treated like any other proprietary competitor: concepts are fair game, source is off-limits. The fact that the source is visible on GitHub does not change this.
+- The `defgen` tool extracts *factual data* (addresses, scalings) from public XML — facts aren't copyrightable; expression (description text) is and gets stripped.
+- The line is **idea / expression**. A "node-graph custom feature designer" is an idea — build one freely. A specific node class hierarchy, file format, or compiler implementation copied from Atlas is expression — don't.
+- See `docs/01-reverse-engineering.md` for day-to-day boundaries and `docs/15-clean-room-engineering.md` for the full methodology, including the analyst/implementer wall and the solo-developer adaptations.
+
+### Rules specific to you, Claude
+
+You have tools (`web_fetch`, `view`, `bash_tool`, `conversation_search`) that can pull protected source into this session and from there into the SubuwuTuner codebase. Treat the following as off-limits for any task that will produce code, specs, or documentation destined for the repo:
+
+- **Do not `web_fetch`** any file under `github.com/motorsportsresearch/atlas-public/` other than the `README.md` and `LICENSE`. The README and LICENSE are fine — they're how you orient. Any `.java`, `.kt`, `.xml`, definition file, or screenshot of the Atlas editor is not.
+- **Do not `web_fetch`** RomRaider source files. RomRaider's public *protocol documentation* and its public ECU definition XML (factual data only) are acceptable; its Java source is not, because the result would be GPL contamination of an Apache 2.0 codebase.
+- **Do not paste or paraphrase** code, comments, identifier names, or string literals from any commercial tuning tool (COBB, EcuTek, HP Tuners, etc.), OEM tuning software (Subaru SSM, dealer tools), or OEM ECU firmware.
+- **If the user pastes** code or excerpts from any of the above into the chat, **stop and flag it** before incorporating it. Don't silently launder it into a SubuwuTuner contribution.
+- **Training-data knowledge is also a channel.** If you would have written a function a certain way "because that's how Atlas does it" or "because RomRaider does it like this," that origin disqualifies the implementation. Write from first principles or from the spec in `SubuwuTuner-specs/`.
+
+What you *should* do when you need to understand a competitor:
+
+- Read public README, marketing, and user-facing documentation (`motorsportsresearch.org`, `romraider.com`, etc.).
+- Read the Atlas Confluence wiki (`motorsportsresearch.atlassian.net`) — that's user-facing documentation, not source.
+- Read public posts, videos, and forum discussion that describe behavior at the user level.
+- Discuss concepts and architecture at the whiteboard level with the developer.
+- Propose SubuwuTuner designs derived from the standards (ISO 14229, ISO 15765, SAE J2534/J1979/J2012) and from public engine-management literature.
+
+### Red flags — if you see any of these, stop
+
+If a task would have you do any of the following, pause and check with the developer before continuing:
+
+- Fetching, viewing, or summarising specific source files from a closed-source or restrictively-licensed competitor.
+- Producing C++ that "matches" a competitor's class layout, API shape, or file format.
+- Naming SubuwuTuner types after Atlas's types, RomRaider's types, or any OEM internal identifiers.
+- Re-emitting a definition file's prose descriptions (factual scaling values are fine; OEM-authored prose is not).
+- Writing a flash routine or brick-recovery sequence "modeled on" Atlas's specifically.
+
+None of these are necessarily fatal — sometimes the user is doing an explicit analyst-side task and wants to extract facts. But you should not assume that; stop, ask, and route through the methodology in `docs/15-clean-room-engineering.md`.
 
 ## House style for the C++ code
```

## Plain-text fallback

If the diff above doesn't apply cleanly — line numbers may have drifted since I fetched `CLAUDE.md` — here are the three changes as standalone edits.

### Edit 1: Introductory paragraph

**Find:**

> This is original work, not a port. Public references like RomRaider (GPL) are studied **clean-room** as protocol specifications — see `docs/01-reverse-engineering.md` for the boundary rules.

**Replace with:**

> This is original work, not a port. Public references like RomRaider (GPL) and source-available competitors like Atlas (All Rights Reserved) are studied **clean-room** — concepts and protocol facts only, never expression. See `docs/01-reverse-engineering.md` for the day-to-day boundary rules and `docs/15-clean-room-engineering.md` for the full methodology.

### Edit 2: Quick-orientation table

In the "Quick orientation for common tasks" table, add the following row immediately after the "Reason about emissions / jurisdiction policy" row:

```
| Reason about clean-room IP boundaries | `docs/15-clean-room-engineering.md` |
```

### Edit 3: Stance on third-party IP

Replace the entire current section (everything between `## Stance on third-party IP` and the next `##` heading) with the expanded version shown in the diff above. The expanded version has three subsections:

- **General rules (developer and assistant)** — extends the existing bullets with Atlas and the idea/expression distinction.
- **Rules specific to you, Claude** — new. Names the specific tools (`web_fetch`, `view`, etc.) and the off-limits sources.
- **Red flags — if you see any of these, stop** — new. Lists trigger patterns that should make Claude pause and ask before proceeding.

## Application

If working at the command line:

```sh
# 1. Save the diff block above to claude-md-clean-room.patch
#    (just the `--- a/CLAUDE.md` through the end of the diff hunks).
# 2. Apply with git:
git apply --check claude-md-clean-room.patch   # dry run, surfaces conflicts
git apply claude-md-clean-room.patch

# 3. Move the standalone clean-room doc into docs/ with the right number:
mv docs/13-clean-room-engineering.md docs/15-clean-room-engineering.md
# (skip if you hadn't yet added the earlier file)

# 4. Verify and commit:
git diff CLAUDE.md
git add CLAUDE.md docs/15-clean-room-engineering.md
git commit -m "docs: add clean-room engineering policy and CLAUDE.md IP guardrails"
```

If working in the IDE: paste the three edits from the **plain-text fallback** section directly.

## Verification

After applying, the following should be true:

1. `grep -c "Atlas" CLAUDE.md` returns at least `2` (introductory paragraph and IP section).
2. `grep "docs/15-clean-room-engineering.md" CLAUDE.md` returns two matches (intro paragraph and orientation table).
3. The "Stance on third-party IP" section contains three subsection headings: *General rules (developer and assistant)*, *Rules specific to you, Claude*, *Red flags — if you see any of these, stop*.
4. `docs/15-clean-room-engineering.md` exists and is referenced from `CLAUDE.md`.

## What this patch deliberately does not change

- The "Stance on emissions / jurisdiction" section. Unchanged on purpose — that's a separate policy axis and its boundaries are already well-stated for your jurisdiction.
- The "Stance on engine and ECU safety" section. Unchanged. The clean-room concerns and the brick-protection concerns are independent.
- House style, status, or the dependency list. Unchanged.
- The handling of public documentation sources (Atlas Confluence wiki, RomRaider's public docs, OEM owner's manuals). These remain allowed.

## Follow-ups not in this patch

A few related changes worth considering separately, none of which are blockers:

- **`docs/01-reverse-engineering.md`** likely needs a short addition naming Atlas alongside RomRaider so the two policy docs stay aligned. I haven't read your current `01-reverse-engineering.md` so I haven't drafted that patch.
- **`docs/06-legal-ethics.md`** could grow a paragraph on the §1201 vehicle-software exemption and on the distinction between source-available and open-source licensing. Currently the file covers emissions and jurisdiction but not source-licensing exposure.
- **A `SubuwuTuner-specs` repository** (private, access-controlled) is referenced by `docs/15-clean-room-engineering.md` §9. Creating that repo and seeding it with the first spec is its own follow-up.
- **One first specification document**, written end-to-end, would be a useful template for everything that follows. The node-graph designer is a natural candidate.

Let me know which of those you want next.
