# 16 — Custom Features (node-graph extensions)

A **custom feature** is a piece of new ECU behavior the user authors visually, that SubuwuTuner compiles into a ROM patch and flashes alongside the calibration. Table edits change *parameters*; custom features change *control flow*. The two are different categories, and the latter is structurally more dangerous — which is why this is Phase 5, well after manual editing, datalogging, and the brick-protection work in `docs/05-improvements.md` §4.

This document captures the planned design. Custom features are **not** a v1.0 feature — they land in Phase 5 (see `docs/04-roadmap.md`), once flashing is solid and we have a real bench rig that can recover a brick.

## Stance on third-party prior art

Several commercial and source-available tuning tools ship something in this category. The clean-room rule applies, restated:

- **The idea is free.** A node-graph designer that compiles to ECU patches is a category of feature, not a protected work. We build one without apology.
- **Expression is not.** Specific node taxonomies, graph file formats, IR designs, codegen pipelines, RAM allocators, or patch-insertion sequences from any other tool — copy none of them. Design from first principles against public engine-management and embedded-systems literature.

See `docs/15-clean-room-engineering.md` for the full methodology. The list of red flags applies double here: it would be very easy, while looking at a competitor's node hierarchy "just to understand the concept," to anchor on their decomposition. Don't.

## What ships

| Layer | What it does | Phase |
|---|---|---|
| **Graph editor** | ImGui-based 2D canvas; nodes are typed boxes with input/output pins; edges carry typed values. Pan/zoom, multi-select, copy/paste, undo/redo bound to the same `st::edit::History` substrate that maps use. | 5 |
| **Node library** | Sensors (RPM, MAF, MAP, …), arithmetic, conditional, state-machine, table-lookup, output (override commanded fuel, command additional injector pulse, …). The library is per-platform and lives in the definition pack so a 2020 WRX and a 2008 STI can expose different hooks. | 5 |
| **Type system** | Pin types — `float32`, `uint8`, `bool`, units (`rpm`, `kPa`, `°C`). Edges must type-match; the editor refuses invalid wires before compile time. | 5 |
| **Compiler** | Graph → SubuwuTuner IR → ECU machine code (Renesas SH-2A for VA, RH850 for VB) → ROM patch. Validation pass enforces real-time budget (max instructions per ECU loop iteration) before allowing flash. | 5 |
| **Patch format** | `.stmod` (SubuwuTuner Mod) — a TOML-and-binary bundle: the graph as authored, the compiled patch bytes, target ECU family, target free-RAM range, source-pack id, semver. Importable, exportable, signable. | 5 |
| **Sample packs** | Flat-foot shifting and rolling launch control ship in-box as reference implementations. Anything more aggressive is community-authored. | 5 |
| **Linter** | Refuses unsafe shapes — recursive output→input cycles, unbounded loops, writes to safety-critical regions, RAM allocations exceeding the platform's known free pool. | 5 |

What does not ship:

- A general-purpose scripting language exposed to users (`Lua` was floated in the Phase 5 roadmap line but is intentionally not on the v1.0 plate — too easy to footgun, too hard to lint). The graph compiler emits machine code directly; the Lua-runtime layer is a possible v2.0+ optimization if performance demands it.
- Auto-import of features from any third-party tool's format. Specs are SubuwuTuner-native; the user re-authors anything they want to carry over.

## Architectural fit

```
   Graph (authored)             IR (SubuwuTuner-native)         Patch (ECU-native)
   ┌──────────────┐             ┌──────────────────┐            ┌───────────────┐
   │ Nodes + pins │ ─compile─►  │ Typed dataflow + │ ─codegen─► │ SH-2A / RH850 │
   │ + edges      │             │ control-flow IR  │            │ machine code  │
   └──────────────┘             └──────────────────┘            └───────────────┘
                                         │                              │
                                         ▼                              ▼
                                  ┌──────────────┐              ┌──────────────┐
                                  │ Linter pass: │              │ Patch insert:│
                                  │ - cycles     │              │ free-RAM map │
                                  │ - RT budget  │              │ + hook table │
                                  │ - safety     │              │ from def pack│
                                  └──────────────┘              └──────────────┘
```

Module layout (planned):

```
src/
├── feature/        st::feature::Graph (data structure, validation, persistence)
├── feature_ir/     st::feature::ir::Module (typed dataflow + CFG)
├── feature_codegen/
│   ├── sh2a/       Renesas SH-2A backend (VA WRX et al.)
│   └── rh850/      Renesas RH850 backend (VB WRX et al.)
├── feature_patch/  Patch insertion — finds free RAM, writes hook table, splices into existing interrupt vectors
└── ui/feature/     The ImGui-based graph editor (links into the existing subuwutuner-gui)
```

Both backends speak a small subset of their respective ISAs — only what the compiler emits. We never need to *parse* SH-2A or RH850 instructions, only emit.

## Definition-pack hooks

The graph editor's available output nodes (where the user can splice in custom logic) come from the definition pack, not the tool. A pack declares:

```toml
[[hook]]
id           = "after_fuel_calc"
display_name = "After fuel calc"
description  = "After the ECU has computed commanded injector pulse width"
ecu_address  = 0x000ABCD0         # where the hook splices into the firmware
free_ram     = { base = 0x40000000, length = 256 }
inputs = [
  { name = "rpm",          label = "Engine RPM",        type = "float", unit = "rpm" },
  { name = "load",         label = "Engine load",       type = "float", unit = "%"   },
  { name = "commanded_pw", label = "Commanded fuel PW", type = "float", unit = "ms"  },
  { name = "coolant_temp", label = "Coolant temp",      type = "float", unit = "°C"  },
]
outputs = [
  { name = "commanded_pw_override", label = "Override fuel PW", type = "float", unit = "ms" },
]
```

**Field semantics:**

- `id` (required, unique) — short stable identifier referenced by `.stmod` files and codegen
- `display_name` (optional) — what the editor's hook palette shows; falls back to `id` when omitted
- `description` (optional) — human-readable text, surfaced in the editor's hook palette tooltip
- `inputs` (optional) — typed signals the hook **provides** (data the user's logic can read). On the graph node these appear as **output** pins (data flowing out of the hook node into the user's logic)
- `outputs` (optional) — typed signals the hook **requires** (values the user must drive). On the graph node these appear as **input** pins
- `ecu_address` (optional, codegen-only) — firmware address where the hook splices; the editor doesn't read this
- `free_ram` (optional, codegen-only) — scratch-RAM region the compiled patch may claim

**Signal fields:** each signal is `{ name, label, type, unit }`.
- `name` is the canonical codegen identifier (snake_case, stable across pack revisions).
- `label` (optional) is what the editor renders on the pin; falls back to `name` when omitted. Keep these short and in tuning vocabulary, not programmer vocabulary — `commanded_pw_override` is fine as `name`, but the pin should read `Override fuel PW`.
- `type` ∈ `float | int | bool` (matches `st::feature::PinType`).
- `unit` is informational metadata — dimensional analysis is future work (see "Type system" in the table above).

**Pin direction inversion** — the rationale is that at the splice point, the ECU is *about to make a decision* (e.g., commanded fuel). The hook offers the user the current ECU state (`inputs`, which the user reads) and asks for the override values (`outputs`, which the user writes). On the canvas, data flow follows the read/write semantics, not the pack-declaration nesting — which means the names "inputs" and "outputs" are inverted between the TOML and the graph. Pack-declaration view dominates because it matches the codegen side of the contract, and the editor flips signs when instantiating a node.

The pack author owns the responsibility of identifying valid hook points (where it's safe to splice without trashing the ECU's state). Untrusted packs would be a serious foot-gun; for v1.x, hook definitions are first-party only.

## Safety considerations

Custom features are categorically more dangerous than table edits:

1. **Brick exposure.** A bad table value can over-fuel or knock the engine. A bad patch can corrupt the ECU's stack, fault the watchdog, or refuse to boot. The brick-protection work in `docs/05-improvements.md` §4 (signed-section verification, recovery-mode boot ROM) is a hard prerequisite for shipping this.
2. **Real-time budget.** Inject too much logic into a 5ms ECU loop and the loop overruns, degrading every other function the ECU performs. The linter enforces a worst-case-execution-time budget per hook.
3. **RAM allocation.** The compiler claims scratch RAM from the pack's declared `free_ram` region. Conflicts between two simultaneously loaded features must be detected at load time, not at runtime.
4. **Emissions interaction.** A custom feature that overrides commanded fuel triggers the same jurisdiction-profile linter that emissions-flagged tables trigger (see `docs/06-legal-ethics.md`). Engine-safety refusals still apply unconditionally; jurisdiction refusals follow the user's profile.
5. **Update channel.** A flash gone wrong while a `.stmod` is loaded must be recoverable by un-flashing the patch without losing the user's calibration. The patch format is therefore additive — it never overwrites the original calibration bytes.

## Scope and timing

Phase 5 is sized at 4–6 weeks in the roadmap. That's the *minimum* for the editor + IR + one codegen backend + the sample packs; expect ~10 weeks realistic if both SH-2A and RH850 backends ship together. This is the single largest feature on the roadmap. The work breaks down roughly:

- Graph data structure + persistence + editor: 2–3 weeks
- IR + linter + RT-budget analyzer: 2 weeks
- One codegen backend (SH-2A, the simpler ISA): 2–3 weeks
- Patch insertion + free-RAM management: 1–2 weeks
- Sample packs (flat-foot, launch) + docs: 1 week
- Second codegen backend (RH850): 2–3 weeks if shipping at the same time

If timing pressure shows up, **drop the RH850 backend first** — VA WRX ships with custom features, VB WRX waits a release. Don't compromise the linter or the brick protection.

## Why not just ship Lua

The Phase 5 roadmap entry mentions "Lua → bytecode patches" as an option. The disadvantages of a scripting language exposed to users:

- **Lint surface is huge.** Catching "this script will fault the ECU" is harder than catching "this graph allocates too much RAM." Type errors in a graph are caught when the user drags the wire; type errors in a script are caught at compile or runtime.
- **The user has to learn a programming language.** That excludes most of the tuning audience.
- **Performance is unpredictable.** A naïve loop in Lua eats more cycles than the equivalent inlined machine code.
- **The graph compiles to the same machine code anyway.** Lua would be a step in the middle that buys us nothing for the v1.x workload.

Lua (or another scripting layer) may become useful for the long tail of features that are awkward in a graph — but not in the v1.x roadmap.

## Open questions

- Should custom features compose? (Can two `.stmod`s be loaded at once if their RAM regions don't conflict?) Leaning yes, but the merge semantics need design.
- Versioning across definition-pack updates. If a 2020 WRX pack gains a new hook in a later pack release, an old `.stmod` should still load — but if a hook is *removed*, what happens to the loaded feature?
- Sandboxing the editor itself. A loaded `.stmod` from an untrusted source could in principle have a malicious graph that exercises a codegen bug; the same defense-in-depth that protects the flash channel needs to apply here.

These are Phase 5 design problems, not blockers.
