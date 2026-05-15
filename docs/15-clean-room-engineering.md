# 15 — Clean-Room Engineering Methodology

This document is the full version of the rules-of-engagement sketched in `docs/01-reverse-engineering.md` §"Clean-room boundary." Where `01` tells you *what* the rules are, this document tells you *how to live by them in practice* — including the awkward parts: when there is only one developer playing both roles, when the assistant in the loop has its own contamination channels, and when a third-party source is more tempting than usual.

If you only have time for the short version: read [Idea vs expression](#2--the-principle-idea-vs-expression) and [Red flags](#10--red-flags). Everything else is procedure.

## 1 — What clean-room is, and what it is not

"Clean-room engineering" is a discipline borrowed from US copyright law — most famously Compaq's 1982 BIOS reimplementation and the Phoenix BIOS work that followed. The pattern: one team reads the protected reference and writes a *specification* describing only the externally observable behavior; a second team, walled off from the original reference, builds an implementation from that specification. If a future lawsuit ever asks "did you copy?", the wall and its paper trail are the answer.

Three things clean-room **is**:

1. A way to derive *facts* (protocol bytes on the wire, register layouts, scaling formulas) from a protected reference without infringing its copyright.
2. A way to derive *concepts* (a node-graph designer, a tabbed map editor, a CRC-protected flash sequence) without copying expression.
3. A discipline that produces *evidence* — specs, dates, sign-offs — that survives litigation discovery.

Three things it is **not**:

1. A patent shield. Patents protect ideas, not expression; a clean-room reimplementation of a patented technique is still infringing. (We don't believe any of the references we study read on live patents in our jurisdiction, but this is a separate analysis.)
2. A trade-secret shield. If the reference was obtained under NDA or by circumventing access controls, no amount of wall changes the underlying breach. We only study sources we have the legal right to read.
3. A license-laundering trick. Pulling GPL source through an "analyst" and emitting Apache 2.0 on the other side does not actually relicense it; what protects the relicensing is the fact that *no expression crossed the wall*, only facts and concepts. The wall has to be real.

## 2 — The principle: idea vs expression

US copyright law (17 USC §102(b)) and the equivalent provisions in Canada, the EU, and most other jurisdictions all draw the same line: **ideas are not copyrightable; expression is.** Everything in this document is procedure for staying on the correct side of that line.

Concrete examples drawn from the kinds of references this project studies:

| Material from a reference | Side of the line | Why |
|---|---|---|
| Memory address `0xC11F0` for the WRX VA primary fuel table | Idea / fact | Observable property of the ECU; not authored expression |
| Scaling formula `y = 0.0078125 * x` for that table | Idea / fact | Mathematical relationship determined by the ECU's hardware |
| The string `"Primary Open Loop Fueling — Final"` in a definition's `description` attribute | Expression | Authored prose; we strip it and write our own description |
| The seed/key challenge algorithm (rotate-add-XOR with constant `0xA3B7`) | Idea / fact | Algorithm, expressible many ways; rewrite from observed behavior |
| A specific Java class hierarchy (`AbstractCommand` → `SsmReadCommand` → `SsmReadBlockCommand`) | Expression | Authored design; our `st::ecu::ssm` does not mirror this shape |
| The concept of "a table editor with row/column headers and an undo stack" | Idea | Generic concept, free to implement |
| Pixel-identical icons or screenshots from another tool | Expression | Copy outright |
| The numeric range `0..1024` for an axis whose hardware encodes 10 bits | Idea / fact | Determined by silicon, not authorial choice |
| A `.tool` or definition file's structural schema (what fields exist, where) | Borderline — see §10 | Format choices are partly functional, partly expressive |

When the line is unclear, the question to ask is: **could a second, independent engineer arrive at this from the same observations?** If yes, it is fact. If no — if the choice could have gone many other ways and the reference happens to choose one — it is expression.

## 3 — The two-role pattern: analyst and implementer

The textbook clean-room workflow assigns two distinct roles:

- **Analyst.** Reads the protected reference. Writes a *specification* describing only the externally observable behavior and the facts the reference reveals. Never writes implementation code. May write test vectors but not test *implementations*. Owns everything in `SubuwuTuner-specs/`.
- **Implementer.** Writes the implementation. Reads only the specification. **Never reads the protected reference**, never reads the analyst's working notes, never has the protected reference on the same machine while implementing. Owns everything in `SubuwuTuner/`.

The wall between them carries:

✅ Plain-English protocol descriptions
✅ Numeric facts (addresses, scalings, polynomials, bit positions)
✅ Black-box test vectors (`given input X, expected output Y`)
✅ Abstract concepts ("seed/key challenge with a 16-bit seed and a polynomial mixer")
✅ Standards references (`ISO 14229 §11.4.2`)

The wall **does not** carry:

❌ Source files, decompiled blobs, or excerpts thereof
❌ Identifier names from the protected source
❌ Class hierarchies, file layouts, or directory structures
❌ Comment text, log strings, error messages, or other authored prose
❌ Pseudocode that mirrors the reference's control flow
❌ Test *implementations* lifted from the reference's test suite

When the analyst writes spec prose, that prose is **the analyst's own writing**, not paraphrased from the reference. "Paraphrased" is a contamination channel: it preserves expression while disguising the source.

## 4 — The wall, in practice

What the wall actually consists of, mechanically:

| Mechanism | What it provides |
|---|---|
| Separate Git repositories | `SubuwuTuner-specs/` (private, analyst-owned) vs. `SubuwuTuner/` (public, Apache 2.0). No spec content is committed to the public repo. |
| Separate working directories | The analyst's checkout of `atlas-public-main/` or any decompiled artifacts lives on the analyst's machine in a directory the implementer's tooling does not index. |
| Time delay | A spec written today is not implemented from until at least 24 hours later, ideally longer. Mechanically forces re-reading the spec rather than working from short-term memory of the source. |
| Editor / tab hygiene | When implementing, no editor tab or terminal session has any reference file open. Close the reference repo, close the decompile, restart the editor. |
| Audit log | `SubuwuTuner-specs/AUDIT.md` records, per spec: date written, references consulted, author, and a hash of the finished spec. (See [§10 Provenance](#11--provenance-and-audit-trail).) |

The wall is **not** "I am being careful." It is mechanical separation that survives forgetting.

## 5 — Solo-developer adaptations

Real clean-room engineering assumes two people. This project has one person. That is workable but requires honesty.

The adaptations:

1. **The same person can play both roles, but never on the same day, and never with both contexts open simultaneously.** Spec session and implementation session are bracketed by a clean shutdown and a calendar gap.
2. **The analyst session has the reference open and emits spec prose.** The reference repo, decompile, etc. are present and indexed. Editor tabs may have third-party source open.
3. **The implementation session reads only the spec.** Before starting, the reference repos are *closed in the editor*, the working directory is the SubuwuTuner repo, and `git status` in the reference repo's parent directory is checked to confirm nothing leaked.
4. **The longer the time gap, the cleaner the implementation.** A week is excellent. Two days is acceptable. Same-day is contamination; the spec was written from short-term memory of expression, not from understanding of facts.
5. **If the spec turns out to be incomplete during implementation, do not "just check the source quickly."** The correct move is to stop implementing, return to analyst mode (separate session, separate day), extend the spec, then return to implementation. The temptation to peek is exactly what clean-room is designed to defeat.
6. **Borderline observations get recorded as questions, not as facts.** If the analyst is uncertain whether a detail is fact or expression — say, a specific bit ordering choice that could have gone two ways — the spec records it as `OPEN QUESTION: bit order observed as MSB-first; confirm against ISO 14229 §11.x rather than assume from reference`. The implementer resolves the question from a standards document, not from the reference.

The solo-dev pattern is weaker than two-team clean-room. The mitigation is paper trail: every spec entry that came from a reference cites the reference, every implementation choice cites the spec section, and the citations are auditable.

## 6 — AI-tool-specific channels

Coding assistants (Claude, Copilot, Cursor, etc.) introduce contamination channels that traditional clean-room doctrine never had to think about. There are at least three of them:

1. **Tool-driven retrieval.** The assistant has `web_fetch`, file-view, and shell access. If you ask it to "look at how Atlas handles X," it will pull the source into the conversation context, and from there into anything it writes for SubuwuTuner. The same session is now contaminated end-to-end.
2. **Latent training-data knowledge.** The model has very likely seen RomRaider's source and may have seen Atlas's during pretraining. Even with no retrieval at all, an instruction like "implement an SSM client" can produce code that echoes patterns learned from those sources. This is contamination through a slow, hard-to-prove channel.
3. **Multi-session memory.** Persistent memory features (this project's `memory/` directory, similar features in other tools) can carry context across sessions. A spec extracted in session A and saved to memory can resurface in implementation session B without an obvious paper trail.

The rules that follow from this:

- **`CLAUDE.md` is binding on the assistant.** The "Rules specific to you, Claude" subsection in `CLAUDE.md` §"Stance on third-party IP" is the operative policy. If the assistant is unsure whether a task crosses the line, the assistant stops and asks. See `CLAUDE.md` for the full set; the highlights are reproduced below for convenience:
  - Do not `web_fetch` files under `github.com/motorsportsresearch/atlas-public/` other than `README.md` and `LICENSE`.
  - Do not `web_fetch` RomRaider source files.
  - Do not `Read` or directory-list anything under `C:\Users\Cornelio\Desktop\jd-gui-master\atlas-decompiled\` — a jd-gui decompile of an Atlas distribution that is itself a Ghidra fork. ~99% of the ~20 000 `.java` files are stock Ghidra carrier; the only Atlas-original content is the `ghidra/MPC5746R/` and `ghidra/V850E3/` disassembler-support modules (instruction-set definitions, no ECU calibration data). The whole tree stays off-limits regardless.
  - Do not `Read` `D:\Documents\atlas-personal\romraider_va_wrx.xml` or `D:\Documents\atlas-personal\romraider_vb_wrx.xml`. Despite the file names these are not genuine community-authored RomRaider XMLs — they're Atlas-derived data transcoded into the RomRaider schema by a runtime-instrumentation pipeline against Atlas. Per Path B (see §11 of this doc and `docs/17-data-distribution-policy.md`), the calibration packs derived from them live off-tree at `D:\Documents\SubuwuTuner-defs-private\`, not in the public repo's `definitions/`.
  - The wall-clean derivatives `D:\Documents\atlas-personal\va_wrx.facts.xml`, `…\vb_wrx.facts.xml`, and the companion `*.name-mapping.tsv` files ARE permitted in analyst-mode sessions as QA inputs to the off-tree pack. They are the legitimate output of `scrub_names.py` and what `defgen` consumes by design.
  - Do not paste or paraphrase code, comments, identifiers, or string literals from any commercial tuning tool.
  - If the user pastes protected source into the chat, stop and flag it before incorporating it.
- **Analyst-side AI work and implementer-side AI work do not share a session.** If a Claude session has had Atlas source in its context (file view, web fetch, paste), that session does not subsequently write SubuwuTuner code. Start a new session for implementation work.
- **Persistent memory inherits the wall.** A memory entry derived from analyst-side work belongs in the *specs* repository's notes, not in `SubuwuTuner/memory/`. The public-repo memory directory may reference *that an analyst-side memory exists*, but does not store its contents.
- **When in doubt, the assistant declines and asks.** This is preferable to producing tainted output that has to be rewritten or removed under audit.
- **The assistant flags its own borderline outputs.** If implementing module X and a particular function "feels obvious because Atlas does it this way," that is a contamination signal — the assistant rewrites from standards or from the spec, and notes the rewrite.

## 7 — The `SubuwuTuner-specs` repository

The specs repository is the analyst's output and the implementer's only input. It is a separate Git repository, private (access-controlled), and structured to support both production and audit.

Suggested layout:

```
SubuwuTuner-specs/
├── README.md                       ; what this repo is and isn't
├── AUDIT.md                        ; provenance log (see §11)
├── CONTRIBUTING.md                 ; analyst workflow rules
├── ssm/
│   ├── 00-overview.md              ; SSM protocol in plain English
│   ├── 01-frame-format.md          ; bytes on the wire
│   ├── 02-a8-read.md               ; A8 command spec + test vectors
│   ├── 03-b0-write.md              ; B0 command spec + test vectors
│   └── 04-seed-key.md              ; seed/key challenge spec
├── uds/
│   ├── 00-overview.md
│   ├── 01-flash-flow.md            ; 34/36/37 download flow spec
│   └── ...
├── ecu-va-wrx-mt/
│   ├── 00-overview.md              ; per-platform observed facts
│   ├── 01-cid-list.md              ; observed calibration IDs
│   └── tables/                     ; per-table fact sheets
├── tests/
│   ├── ssm-a8-vectors.toml         ; black-box test vectors (input/output)
│   ├── uds-flash-vectors.toml
│   └── ...
└── references/
    └── consulted.md                ; running log of which references each spec drew from
```

Rules for what goes in:

- **Plain English first.** A spec opens with prose describing the behavior at a level a competent C++ engineer who has never seen the reference could implement from. Code-like notation (`A8 [addr_hi] [addr_mid] [addr_lo] [crc]`) is welcome; pseudocode that mirrors the reference's control flow is not.
- **Facts get cited.** Every numeric fact has a citation: `address 0xC11F0 (observed in RomRaider VA WRX 2019 def file; cross-checked against forum dump 'aw0123_stock.bin' offset 0xC11F0 → 0x3FFF byte pattern matches)`. Two independent observations are better than one.
- **Test vectors are black-box.** Input bytes, expected output bytes. No `assertEquals(parser.foo(), bar)` shape lifted from the reference's tests; the implementation tests in `SubuwuTuner/tests/` are written from these vectors fresh.
- **Open questions stay open.** A spec section may end with `OPEN QUESTIONS:` and a bulleted list. The implementer does not invent answers; questions are resolved by further analyst work, by consulting a standards document, or by a hardware experiment.

Rules for what does **not** go in:

- Source excerpts from any reference.
- Decompiled output from any closed-source tool.
- Identifier names, file paths, or class hierarchies from a reference.
- Copies of authored prose from a reference (description strings, comments).
- Screenshots of competitor UIs.

## 8 — Workflow

### Analyst-side, per spec

1. Open the specs repo. Open the reference(s). Note today's date.
2. Identify the scope of the spec (one protocol message, one table, one algorithm).
3. Read the reference. Take *facts-only* notes: addresses, byte sequences, control-flow at the protocol level, edge cases.
4. Cross-check with at least one other source — a standards document, a public forum post describing the same protocol, a hardware capture. Spec entries with single-source provenance are flagged in `AUDIT.md` as such.
5. Write the spec in plain English. **Do not have an editor tab open on a SubuwuTuner C++ file during this step.**
6. Write black-box test vectors derived from the spec, not from the reference's tests.
7. Commit the spec to the specs repo with a message citing the consulted references.
8. Append an `AUDIT.md` entry: date, spec section, references consulted, single- vs. multi-source.
9. **Wait.** Do not implement against this spec on the same day.

### Implementer-side, per module

1. Wait until at least one day has elapsed since the last analyst session touching this area.
2. Open the SubuwuTuner repo. **Close the reference repos in the editor.** Confirm no decompile or competitor source is in any open tab, terminal, or recent-files list.
3. Read only the spec section(s) for this module. Do not read the analyst's working notes or scratch files.
4. Implement in fresh C++ from the spec. Cite the spec section in commit messages or in a `// per spec/ssm/02-a8-read.md §3` comment when a non-obvious choice traces to a specific spec section. (Sparingly — comments are kept lean per house style.)
5. Write implementation tests from the spec's test vectors. The test file is structured the way our test suite is structured, not the way any reference's test suite is.
6. If during implementation the spec turns out to be incomplete or wrong, stop. Do not check the reference. Open a follow-up analyst session (separate day, separate context) to extend the spec.

### When the assistant is one of the roles

The assistant can play either role but never both in the same session. Practical pattern:

- **Analyst session with Claude.** "We are working on the specs repo. The reference is `~/dev/atlas-decompiled/`. Read the SSM module there and produce a facts-only spec of the A8 read flow into `SubuwuTuner-specs/ssm/02-a8-read.md`." Allowed: file view, paraphrasing into plain English, fact extraction, test-vector generation. Disallowed: any output destined for `SubuwuTuner/`.
- **Implementation session with Claude.** "Implement `st::ecu::ssm::SsmClient::read` per `SubuwuTuner-specs/ssm/02-a8-read.md`. The specs repo is on a separate drive; only the spec markdown for this section is in scope. Do not open or reference any other repository." Allowed: writing C++ from the spec. Disallowed: any retrieval of reference source, any paraphrase from training-data echoes of references.
- **Sessions do not cross.** If you accidentally start an implementation session with reference context still loaded — close it, start fresh. Memory entries from an analyst session do not appear in implementer-session memory.

## 9 — Standards as a contamination-free input

Whenever a behavior can be derived from a published standard rather than from a competitor's source, prefer the standard. The standards we work from:

- **ISO 14229-1** — UDS application layer
- **ISO 15765-2** — CAN-TP transport (network/transport for UDS over CAN)
- **ISO 14230** — KWP2000 (the K-Line application protocol underlying Subaru SSM on VA-era cars)
- **ISO 11898** — CAN physical/data link
- **SAE J2534** — Pass-Thru API (Tactrix etc.)
- **SAE J1979** — OBD-II diagnostic services
- **SAE J2012** — DTC numbering

Standards are paid documents and you have to actually purchase them (or use ISO's online preview). Treating them as the primary input — rather than RomRaider's interpretation of them — is the cleanest possible provenance for our code.

Implications for what gets cited:

- A function implementing UDS RDBI cites `ISO 14229 §11.x`, not "RomRaider".
- A function implementing Subaru's specific seed/key algorithm cites the analyst-side spec which itself cites the observation source, since this algorithm is **not** in any public standard. Provenance is honest.

## 10 — Red flags

Stop and re-think (and probably stop and ask) if you notice any of the following:

- A SubuwuTuner type name that echoes a competitor's type name. Rename.
- A SubuwuTuner class hierarchy or module layout that mirrors a competitor's. Restructure.
- A function whose control flow lines up too closely with a passage in a reference you read recently. Rewrite from the spec; if the spec doesn't support a different shape, the spec is leaking expression — revise the spec.
- A `.tool` / definition-file format choice ("the same fields in the same order with the same names") that mirrors the reference. Pick a different structure or a different naming convention; we have our own format (TOML, `docs/11`), use it.
- A description string, error message, or log line that "sounds like" the reference's. Rewrite in our voice.
- A test that was easier to write because you "remembered how RomRaider does it." Throw it out; write a fresh test from the spec.
- A pull request from a contributor whose other open-source history includes commits to a protected competitor. Not disqualifying, but means the wall has to be extra clean — analyst-side only, no implementation contributions in this area.

None of these are automatic disqualifications; some are unavoidable for technical reasons (e.g., SSM frame layout is what the silicon expects, and our framing code will look like everyone else's framing code at the byte level). The rule is **notice, audit, document the reason** — never silently allow.

## 11 — Provenance and audit trail

If this project's licensing is ever challenged, the artifacts that defend it are:

1. The Git history of `SubuwuTuner-specs/` and `SubuwuTuner/`, on separate timelines, showing spec preceded implementation.
2. `SubuwuTuner-specs/AUDIT.md`, listing per spec: date, author, references consulted, single- vs. multi-source.
3. `SubuwuTuner/THIRD_PARTY_NOTICES.md`, listing every dependency we link against (these are uncontroversial — Catch2, tomlplusplus, GLFW, etc.), and every reference we *studied* as a data source (RomRaider definitions, public protocol docs).
4. Commit messages and the occasional `// per spec/...` comment, linking implementation to spec.
5. This document, codifying the methodology so the audit trail is not retrospective rationalisation.

`AUDIT.md` template:

```markdown
## ssm/02-a8-read.md — 2026-06-01

- **Author:** Cornelio
- **References consulted:**
  - ISO 14230-3 (primary specification of KWP2000 framing)
  - RomRaider `SSMProtocol.java` (open-source GPL reference; protocol fact extraction only)
  - Public forum thread "VA SSM A8 byte order question", romraider.com, 2018-03
- **Multi-source:** yes (standards + open-source + community discussion)
- **Test vectors:** `tests/ssm-a8-vectors.toml` §1-§4
- **Open questions:** none

## ssm/04-seed-key.md — 2026-06-08

- **Author:** Cornelio
- **References consulted:**
  - RomRaider `SeedKey.java` (open-source GPL reference)
  - Forum thread "WRX seed/key algorithm" (multiple posts, 2014-2019)
- **Multi-source:** yes (open-source + community discussion); NOT corroborated by a standard, since this algorithm is Subaru-specific.
- **Test vectors:** `tests/ssm-seed-key.toml` §1-§3
- **Open questions:** algorithm constant `0xA3B7` confirmed against two independent forum dumps; confirm against a hardware capture once OBDX VX adapter lands.
```

Implementation-side commits reference the spec by path:

```
commit abc1234
    feat(ssm): implement A8 read per spec/ssm/02-a8-read.md §3

    Per spec/ssm/02-a8-read.md §3, the A8 read frame uses big-endian
    address bytes followed by a single-byte XOR checksum. Test vectors
    from spec/tests/ssm-a8-vectors.toml §1-§4 pass.
```

## 12 — What this methodology does not protect against

Be honest about the limits:

- **Patents.** As noted in §1, clean-room does not cure patent infringement. We do not believe our references read on live patents in our jurisdiction, but a competent patent analysis is a separate exercise.
- **Trade-secret claims from non-public sources.** Anything obtained under NDA, by circumventing access controls, or in violation of the source's terms of access is out of bounds regardless of the wall.
- **Trademark.** "ROMRAIDER" and "ATLAS" are trademarks we do not use in our own UI, packaging, or marketing.
- **Bad-faith claims.** A motivated litigant can file regardless. The methodology makes the defense substantially cheaper, not impossible.
- **The §1201 DMCA carve-out for vehicle software.** US law explicitly permits owners to modify their own vehicles' software for diagnosis, repair, and modification under a Librarian-of-Congress exemption renewed since 2015 (most recently 2024). Canada has an equivalent under the Copyright Act §41.12 with narrower scope. Both are about *circumventing access controls*, not about reproduction; clean-room handles the reproduction side. They are complementary, not redundant.

## 13 — Outstanding provenance question (resolved 2026-05-14)

The first twelve sections are about the **copyright** axis: how to study a protected reference without copying its expression, and how to produce a paper trail that defends against an infringement claim. That methodology assumes the references being studied are *legitimately accessible* — under their stated license, or in the public domain. The wall is downstream of access; it is not access control itself.

On 2026-05-14 the project surfaced a provenance question that does not live on the copyright axis. The analyst-side raw XMLs at `D:\Documents\atlas-personal\romraider_{va,vb}_wrx.xml` were generated by a Claude-assisted runtime-instrumentation workflow that attached to Atlas as it ran, intercepted the cleartext form of Atlas's protected calibration records, and transcoded the resulting data into RomRaider's XML schema using genuine community-authored RomRaider XMLs (for unrelated platforms) as the schema template. The phrase "decryption data" in the raw-XML header is literal: Atlas's runtime protection mechanism was circumvented by the instrumentation agent.

That circumvention is **DMCA §1201 / trade-secret territory**, not copyright. The wall in §§3–8 (`scrub_names.py`, the analyst/implementer split, the spec repo, the audit log) operates correctly on the copyright axis — strips Atlas's authorial naming and curation choices, leaves only facts, produces a defensible expression-level audit trail. But no amount of expression-axis cleanup cures an upstream access-control circumvention. Every artifact downstream of that pipeline carries the provenance regardless of how clean its expression is.

The project's response is **Path B**: the public Apache-2.0 release ships infrastructure (loader, format, edit/undo, project model, flash orchestrator, auto-tune kernels, CAN toolkit, GUI) and **does not bundle the contested calibration packs** as first-party content. Users supply their own definitions at install/runtime, sourced from public RomRaider community packs, generated from their own ROM dumps, or independently reverse-engineered. SubuwuTuner becomes structurally similar to TunerStudio / EFI Live / Ghidra in that respect: tool plus user-provided data, not tool-with-bundled-data.

Path B is forward-looking, not retrospective. It does not undo or re-litigate the prior pipeline; the analyst-side master copy (now at `D:\Documents\SubuwuTuner-defs-private\`) is retained for the developer's own tuning use. What changes is the **distribution boundary**. The public repo carries only artifacts that pass both the copyright filter (this document) and the §1201 filter (`docs/17-data-distribution-policy.md`).

For the parallel private-side audit-trail entry, see `D:\Documents\SubuwuTuner-specs\AUDIT.md` §"references/va-vb-definition-corpus.md — 2026-05-14".

## 14 — References

- `docs/01-reverse-engineering.md` — the day-to-day boundary rules and the `defgen` tool
- `docs/06-legal-ethics.md` — emissions, IP licensing, distribution
- `docs/17-data-distribution-policy.md` — what the public repo ships and why (Path B)
- `CLAUDE.md` — assistant-specific contamination channels and red flags
- Compaq, *Phoenix Technologies*, et al. — the foundational US clean-room cases
- *Sega Enterprises Ltd. v. Accolade, Inc.*, 977 F.2d 1510 (9th Cir. 1992) — fair-use carve-out for reverse engineering for interoperability
- *Sony Computer Entertainment, Inc. v. Connectix Corp.*, 203 F.3d 596 (9th Cir. 2000) — clean-room reimplementation upheld
- 17 USC §102(b) — idea / expression dichotomy
- Canada *Copyright Act* §41.12 — interoperability and vehicle software exception
- ISO 14229-1, ISO 15765-2, ISO 14230 — UDS, CAN-TP, KWP2000 specifications

---

**Maintenance.** This document is meant to be living. When a new reference enters the project's working set, or a new contamination channel is identified (a new AI tool, a new memory feature, a new public source), update §6 and §10. The point is that the methodology is honest about how this project actually operates, not aspirational about how a textbook two-team clean-room would.
