# Third-Party Inspirations

> This file is the **inspiration trail** for SubuwuTuner. It lists concepts, patterns, and design ideas that were learned from public references and re-implemented from scratch in this codebase. Nothing here represents copied code, copied UI, or copied data.
>
> Inspirations are facts and high-level *categories* of design. The boundary between inspiration and expression is set in `docs/15-clean-room-engineering.md`. When in doubt, treat the boundary as conservative.

This file is intentionally separate from `THIRD_PARTY_NOTICES.md`. That file is the **license attribution** for bundled libraries (Catch2, GLFW, Dear ImGui, ImPlot, tomlplusplus, tl::expected, nativefiledialog-extended, miniz, Inter, JetBrains Mono). This file is the **idea attribution** for design-level inspirations.

---

## Public open-source ECU tuning suites studied

### RomRaider (GPL-2.0)

- **Used for:** ECU protocol facts and the user-facing terms tuners already know (table, axis, ECU-ID, datalog, SSM PID). RomRaider's public protocol documentation and ECU definition XML supply factual data that is not copyrightable.
- **Clean-room boundary:** The RomRaider Java source is GPL and is **not** read by maintainers when authoring SubuwuTuner C++. The contribution boundary is enforced in process. Definition XML supplies addresses, scalings, and family layouts (facts); the prose descriptions inside are stripped during `tools/defgen/` and replaced with project-original prose.
- **Concept-level inspirations:**
  - "Definitions are external files the community maintains" — a category of design, not a file format. SubuwuTuner's TOML schema is original.
  - "Logger reads a separate XML/TOML describing parameters" — a category. SubuwuTuner's `ecuparams/` TOML schema is original.
  - "Table-tree on the left, editor tabs on the right" — common to many editors; not unique to RomRaider.
  - "ECU-ID detection + suggested-definition prompt" — a category.
- **What is NOT inspired by RomRaider:** the C++ class layout, the file-format byte layouts, the editor behavior at the cell-paste level, the icon set, any string literal.

### FastECU (GPL-3.0)

- **Used for:** Public discussion of OEM flashing workflows and the bench/BDM unbrick pathway.
- **Clean-room boundary:** FastECU's C++/Qt source is GPL-3 and is **not** read by maintainers. The protocol facts (UDS sub-functions, ISO-15765 framing) come from the ISO standards, not from FastECU code.
- **Concept-level inspirations:**
  - "Wizards for irreversible operations" — a category. SubuwuTuner's flash modal is original.
  - "Per-vehicle / per-family kernel abstraction" — a category. SubuwuTuner's `feature_codegen/` and `flash/` modules are original.
  - "Bench / BDM as the explicit unbrick path" — covered in `docs/31-brick-protection-by-isa.md`.
  - "Forced auto-backup before write" — a category. Implemented in SubuwuTuner as `st::flash::BackupStore` with original interface and persistence format.

### EcuFlash (closed-source, commercial)

- **Used for:** Public marketing pages, documentation, the published XML definition schema, the supported-vehicles list.
- **Clean-room boundary:** EcuFlash's binary is **not** decompiled or reverse-engineered for SubuwuTuner. The public XML schema is a fact about what the community publishes against EcuFlash; the SubuwuTuner TOML schema does not reproduce it byte-for-byte.
- **Concept-level inspirations:**
  - "Open ROM → edit → save" as a single-pane low-friction workflow — a category. SubuwuTuner's first-run experience is original.
  - "XML-driven definition file" — a category. SubuwuTuner uses TOML, not XML.

---

## Reverse-engineering / analysis platforms studied

### A Java-based reverse-engineering platform (used as an architectural reference for plugin and docking patterns)

- **Used for:** High-level architectural patterns of a long-lived multi-module desktop application: project-as-workspace, tool + plugin + ComponentProvider triad, docking framework, transactional change events, headless-vs-interactive duality.
- **Clean-room boundary:** Source is **not** read for the purpose of authoring SubuwuTuner C++. Pattern names like "plugin", "action", "dock layout", "domain object", "transaction with undo/redo" are general software-engineering vocabulary — they are not unique to any one tool.
- **Concept-level inspirations:**
  - The interfaces under `src/core/include/st/core/ext/` follow the *idea* of "small typed extension contracts" common to many plugin-supporting desktop applications.
  - The class names, method signatures, and lifecycles in `st/core/ext/*.hpp` are original.

### Atlas (source-available, All Rights Reserved)

- **Used for:** Public READMEs, marketing pages, and discussions of which platforms are supported.
- **Clean-room boundary:** The Atlas source is **off-limits** for authoring SubuwuTuner code, docs, specs, or test fixtures. See `CLAUDE.md` for the explicit rules; the boundary covers both the GitHub mirror and any local decompile. The license is *not* GPL — it is All Rights Reserved with source visibility, and we treat it as such.
- **What we use:** Public concept names ("custom feature", "node-graph designer") that are general vocabulary.
- **What we do NOT use:** Any class structure, file format, identifier, prose description, screenshot, or icon. Maintainers do not run `web_fetch` against the Atlas mirror beyond `README.md` and `LICENSE`.

---

## ISO / SAE / industry standards (factual references — not inspirations)

The following standards define facts about wire protocols. They are not "inspirations" in the sense above — they are the canonical source for what the protocols *are*, and any tuning tool must implement them to communicate with an ECU.

- **ISO 14229** (UDS) — diagnostic services, sessions, security access, transfer data.
- **ISO 15765** — CAN-bus transport for UDS (ISO-TP).
- **ISO 14230** (KWP2000) — K-line diagnostic services for older Subarus.
- **ISO 11898** — CAN physical/link layers.
- **SAE J2534** — PassThru API for OEM-tool-to-vehicle interfaces (Windows-only).
- **SAE J1979** — OBD-II services.
- **SAE J2012** — DTC formatting.

Source for these is the published standards documents. The implementation in `src/ecu/`, `src/transport/`, and `src/can/` follows the standards directly.

---

## Public engine-management literature

General engineering literature on engine control, fuel control, knock control, ignition timing, MAF scaling, and closed-loop control informs the auto-tune kernels under `src/autotune/`. No proprietary OEM calibration documents are used.

---

## Process discipline

- Maintainers do **not** decompile any commercial or closed-source tuning tool when authoring SubuwuTuner.
- AI assistants used during development are scoped to public, license-clean reference material; protected references (GPL source code, source-available competitors) are explicitly disallowed via `CLAUDE.md`.
- If a future maintainer needs to consult a protected reference for spec extraction, the work happens in an isolated *analyst session* whose outputs are spec documents (under `D:\Documents\SubuwuTuner-specs\`), never code that enters this repo.
- The boundary between inspiration and expression is reviewed when this file is updated.

---

## Updating this file

Add a section whenever:

- A new public reference is studied.
- A new design idea is adopted in this codebase that has an obvious public-reference origin.
- The clean-room boundary for an existing reference is revisited.

Do not add an entry whenever:

- A common idiom is used (RAII, mutex, atomic counter, etc.).
- A general computer-science concept is used (parser, validator, plugin, registry, etc.).
- An ISO/SAE/RFC standard is implemented (those go in `THIRD_PARTY_NOTICES.md` if a specific document is licensed for redistribution; otherwise just cite the standard number).

If you're unsure, err on the side of *over-attributing*. An over-attribution costs nothing; an under-attribution is harder to fix later.
