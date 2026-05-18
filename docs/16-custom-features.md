# 16 — Custom Features (node-graph extensions)

A **custom feature** is a piece of new ECU behavior the user authors visually, that SubuwuTuner compiles into a ROM patch and flashes alongside the calibration. Table edits change *parameters*; custom features change *control flow*. The two are different categories, and the latter is structurally more dangerous — which is why this is Phase 5, well after manual editing, datalogging, and the brick-protection work in `docs/05-improvements.md` §4.

This document captures the design. Phase 5 work is in progress: the
authoring data model, IR lowerer, SH-2A codegen, CLI, and `.stmod`
file format have shipped end-to-end. **Flashing** is not yet wired —
patch insertion (`src/feature_patch/`) and real-hardware validation
gate on bench-rig work. See *Current state* below for the granular
matrix.

## Stance on third-party prior art

Several commercial and source-available tuning tools ship something in this category. The clean-room rule applies, restated:

- **The idea is free.** A node-graph designer that compiles to ECU patches is a category of feature, not a protected work. We build one without apology.
- **Expression is not.** Specific node taxonomies, graph file formats, IR designs, codegen pipelines, RAM allocators, or patch-insertion sequences from any other tool — copy none of them. Design from first principles against public engine-management and embedded-systems literature.

See `docs/15-clean-room-engineering.md` for the full methodology. The list of red flags applies double here: it would be very easy, while looking at a competitor's node hierarchy "just to understand the concept," to anchor on their decomposition. Don't.

## What ships

Status legend: ✅ shipped · 🟡 partial · ⬜ not yet.

| Layer | What it does | Status |
|---|---|---|
| **Graph editor** | ImGui-based 2D canvas; nodes are typed boxes with input/output pins; edges carry typed values. Pin labels show pack `label` (pretty) but route to canonical `name` underneath. Right-click pins for per-instance defaults; right-click empty canvas for the Insert palette. | ✅ |
| **Node library** | Hooks (splice points + sensor reads) and primitives (pure computation), both pack-declared. The library is per-platform and lives in the definition pack so a 2020 WRX and a 2008 STI can expose different hooks. | ✅ |
| **Type system** | Pin types — `Float`, `Int`, `Bool`, plus per-pin `unit` strings. Edges must type-match AND unit-match (empty unit acts as wildcard); the editor refuses invalid wires before compile time. Dimensional analysis stays string-equality for v1.x. | ✅ |
| **Compiler (SH-2A)** | Graph → IR → SH-2A machine code → PatchObject. Covers Int arithmetic (add/sub/mul), Int compares (lt/gt/eq), Bool ops (and/or/not), select (int/bool/float), Float arithmetic via FPU (FADD/FSUB/FMUL/FDIV), Float compares (FCMP/EQ + FCMP/GT). Handles nested CallPrimitive trees with SSA spill, cross-hook value flow, and fan-out dedup. | ✅ |
| **Compiler (RH850)** | Stub backend that returns NotImplemented. VB WRX support waits until SH-2A is bench-validated. | ⬜ |
| **CLI** | `subuwutuner-cli feature-compile <stmod> --def <pack> [--arch sh2a\|rh850] [--format hex\|toml\|raw\|stmod] [--output <file>] [--validate-only]`. Plus `dump-ir`, `lint-graph`, `lint-ir`. `--format=stmod` bundles graph + patch in a single TOML; `--validate-only` runs parse + lower + compile and exits 0/non-zero without producing output — for CI / pre-commit hooks. | ✅ |
| **Patch format** | `.stmod` — TOML document carrying both the source graph (`[graph]` + `[[node]]` + `[[edge]]`) and the compiled patch (`[patch]` + `[[patch.hook]]` + `[[patch.hook.ram_claim]]`). Single-file, diffable, round-trippable. Signable is a future concern. | ✅ |
| **Linter** | `feature::lint(Graph)` flags undriven inputs + orphan nodes; `feature::ir::lint(Module)` flags duplicate hook overrides + RT-budget overruns. Per-primitive cycle costs in `estimate_cost` (e.g. `divide_int` = 18 cycles, `add_int` = 1) — derived from public SH-2A spec; bench profiling will refine. Unknown symbols default to 3 cycles. | ✅ |
| **Sample packs** | `clutch-kill` (Bool-only synthetic smoke), `flat-foot-shift` (3-sensor AND chain), `launch-control` (4-sensor 3-compare AND tree). Compile end-to-end through SH-2A. `flex-fuel` exists but blocks on a curve-table primitive. | 🟡 |
| **Patch insertion** | Finding free RAM, writing the hook table, splicing into existing interrupt vectors. Bench-rig-blocked — requires a real ECU to develop against. | ⬜ |
| **Flashing** | Loading a `.stmod` and burning the patch to an ECU. Gates on Patch insertion + Phase 3 transport. | ⬜ |

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

Module layout (current):

```
src/
├── feature/             st::feature::Graph + st::feature::ir::Module
│                        (data structure, validation, persistence,
│                         IR lowering, graph-level + IR-level linters)
├── feature_codegen/     IBackend + Sh2aBackend + Rh850Backend (stub) +
│                        PatchObject + RamAllocator + select_backend +
│                        patch_to_toml / patch_from_toml
├── feature_patch/       (not yet — patch insertion, free-RAM map,
│                         hook table, vector-table splicing)
└── ui/src/main.cpp      The ImGui-based designer ("Custom features
                         designer (preview)" in the View menu) +
                         all other panels; designer is not yet a
                         separate module.
```

(The IR initially lived in its own module `src/feature_ir/` in the plan; in practice it ships inside `src/feature/` since it shares types with the Graph and Codegen is the natural fission point. It can be split out if it grows.)

Both backends speak a small subset of their respective ISAs — only what the compiler emits. We never need to *parse* SH-2A or RH850 instructions, only emit.

## Current state — SH-2A primitive coverage

The SH-2A backend recognizes the following CallPrimitive symbols.
Adding a new arithmetic primitive is one entry in the `primitive_shape`
table plus one `emit_*_fragment` function — see
`src/feature_codegen/src/feature_codegen.cpp`.

| Symbol | Operand types | Result | Underlying SH-2A op |
|---|---|---|---|
| `add_int` | (Int, Int) | Int | `ADD Rm, Rn` |
| `subtract_int` | (Int, Int) | Int | `SUB Rm, Rn` |
| `multiply_int` | (Int, Int) | Int | `MUL.L` + `STS MACL, Rn` |
| `compare_lt_int` | (Int, Int) | Bool | `CMP/GT Rn, Rm` + `MOVT` |
| `compare_gt_int` | (Int, Int) | Bool | `CMP/GT Rm, Rn` + `MOVT` |
| `compare_eq_int` | (Int, Int) | Bool | `CMP/EQ Rm, Rn` + `MOVT` |
| `and_bool` | (Bool, Bool) | Bool | `AND Rm, Rn` (canonical 0/1) |
| `or_bool` | (Bool, Bool) | Bool | `OR Rm, Rn` |
| `not_bool` | (Bool,) | Bool | `TST Rn, Rn` + `MOVT` |
| `select_int` / `select_bool` / `select_float` | (Bool, T, T) | T | `TST` + `BT` + `BRA` + `MOVT` |
| `add_float` | (Float, Float) | Float | `FADD FRm, FRn` (via FPUL transfer) |
| `subtract_float` | (Float, Float) | Float | `FSUB FRm, FRn` |
| `multiply_float` | (Float, Float) | Float | `FMUL FRm, FRn` |
| `divide_float` | (Float, Float) | Float | `FDIV FRm, FRn` |
| `compare_lt_float` | (Float, Float) | Bool | `FCMP/GT FRm, FRn` (swapped) + `MOVT` |
| `compare_gt_float` | (Float, Float) | Bool | `FCMP/GT FRm, FRn` + `MOVT` |
| `compare_eq_float` | (Float, Float) | Bool | `FCMP/EQ FRm, FRn` + `MOVT` |

Naming convention: typed primitives carry a `_int` / `_float` suffix
so the same operation name doesn't ambiguously dispatch. `add_int`
and `add_float` are different symbols emitting different instruction
sequences; the IR-side type checker (and PrimitiveShape table)
enforces that operand types match the symbol's declared shape.

`divide_int` ships via an FPU bridge: load each int operand → FPUL →
`FLOAT FPUL, FRn` (int-to-float conversion), `FDIV` on the float
side, then `FTRC FRm, FPUL` (truncate toward zero) → `STS FPUL, Rn`
back to the int register file. Truncation matches C int division
semantics. Trade-offs: ±2^24 precision ceiling (float32 mantissa),
saturation on overflow rather than C's UB, FPU exception on
divide-by-zero (same as `divide_float`). DIV1 iterative + DIVS
single-instruction were considered and rejected — DIV1 is ~32
instructions per call with sign-handling corner cases worth bench
validation; FPU is ~6 instructions and reuses the float code path
that already shipped.

**Not yet implemented:** per-ISA *measured* cycle costs (today's
numbers in `estimate_cost` reflect SH-2A instruction-issue counts +
documented FPU latencies from the public Renesas spec — accurate
enough to flag wildly over-budget graphs, not yet validated against
a real-silicon pipeline trace), and any curve / table-lookup
primitive (relevant for the unblocked `flex-fuel` sample).

## Current state — `.stmod` file format

A `.stmod` is a single TOML document with two halves:

```toml
[graph]
schema_version = 1

[[node]]
id    = 1
kind  = "hook.read_rpm"
label = "Engine RPM"
x     = 48.0
y     = 48.0
phase_break = true
pins  = [
  { id = 0, name = "rpm", label = "RPM", type = "float",
    direction = "output", unit = "rpm" },
]

[[edge]]
from_node = 1
from_pin  = 0
to_node   = 2
to_pin    = 0

[patch]
arch = "sh2a"

[[patch.hook]]
symbol = "ignition_cut"
splice_address = 0xABE00
code = "D009D10A30170129..."     # uppercase hex, no separators

[[patch.hook.ram_claim]]
address = 0x40000100
size = 4
alignment = 4
```

`feature::from_toml(text)` reads the `[graph]` half and ignores
`[patch]`; `feature_codegen::patch_from_toml(text)` does the reverse.
A bundled `.stmod` round-trips both halves; a graph-only `.stmod`
(no `[patch]`) parses cleanly with `patch_from_toml` returning
`nullopt`.

Pin field convention (load-bearing — silent failure if violated):
`name` is the canonical id used by codegen to resolve back to the
pack's `HookSignal.name`. `label` is the human-readable display
text. Mixing them — putting the label in `name` — produces a
"hook does not declare input pin '<label>'" error at compile time.
The designer's Insert palette sets both fields correctly; hand-
authored `.stmod` files need to follow the convention.

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

## Definition-pack primitives

Alongside `[[hook]]`s, packs declare reusable computation nodes via `[[primitive]]`. Primitives are pure functions — arithmetic, boolean logic, table lookup once that lands — that wire between hook outputs and hook inputs. They have no `ecu_address` or `free_ram` because there's nothing to splice; codegen lowers them into machine code that runs in the user's logic block.

```toml
[[primitive]]
id           = "multiply_float"
display_name = "Multiply"
description  = "Floating-point product of two inputs."
inputs = [
  { name = "a", type = "float" },
  { name = "b", type = "float" },
]
outputs = [
  { name = "out", type = "float" },
]
```

Signal fields are the same as hooks (`name`, optional `label`, `type`, optional `unit`). The direction convention, however, is the obvious one: primitive `inputs` become graph **Input** pins (consumed from upstream) and `outputs` become graph **Output** pins (produced for downstream). No inversion. This differs from hooks because primitives aren't splice points — there's no read/write phase distinction to flip.

The designer palette groups palette entries by source: a `Hooks` section and a `Primitives` section. Both sets come from the same loaded definition pack.

## Cycle detection and phase-break nodes

A naive view of the graph would flag the most common feature shape — `hook.out → user_logic → hook.in` — as a 2-cycle through the hook node. But the cycle is illusory: the hook's Output pins fire at splice-time T (ECU state read out for user logic to consume), and its Input pins fire at T+ε (user-driven overrides applied back). User logic runs in between. There is no real cycle in execution time.

The data model encodes this with `Node.is_phase_break`. When set (and the editor sets it automatically for every hook-derived node), cycle detection models the node as two vertices with no internal edge — output-side and input-side are independent. Primitives leave the flag false: a primitive's output IS a pure function of its inputs at the same time, so a path from a primitive's output back to its input is a real cycle.

## Per-instance constants

Every Input pin carries an optional `default_value` (stored as a double regardless of pin type — Int truncates, Bool reads `v > 0.5`). When no edge drives the pin, the default supplies the value. In the editor, right-clicking an unconnected Input pin opens a typed editor (InputFloat / InputInt / Checkbox per `PinType`) plus a "Clear value" entry; the value renders inline next to the pin label as ` = N` in the same row, so a glance reads the actual constants without opening anything.

`lint()` treats a defaulted Input pin as driven — no "not driven" warning surfaces. Codegen will read the default at compile time exactly like an edge-driven value would be.

## Sample packs

Four starter `.stmod` files ship in `fixtures/samples/`. All paired
with `fixtures/demo-pack/`. Compile from the shell:

```
subuwutuner-cli feature-compile fixtures/samples/<name>.stmod \
  --def fixtures/demo-pack --arch sh2a
```

| Sample | Bytes | What it exercises | Compiles? |
|---|---|---|---|
| `clutch-kill.stmod` | 72 | `LoadConstant` + `compare_gt_int` + `not_bool` + `and_bool` + `StoreHookOutput`. Pure synthetic — all constants, no sensor reads. Smallest viable graph through every codegen path. | ✅ |
| `flat-foot-shift.stmod` | 124 | Clutch + throttle + RPM read from 3 different hooks, threshold compares with default-value constants, AND chain into `ignition_cut.cut_active`. Exercises cross-hook value flow + Float compares. | ✅ |
| `launch-control.stmod` | 184 | 4 sensor reads, 3 Float compares, 3 ANDs. Same general shape as flat-foot but one stage wider. | ✅ |
| `flex-fuel.stmod` | — | Read ethanol-content sensor → flex-fuel-scale curve → multiply commanded fuel pulse width → write back. **Blocked** on the `flex_fuel_scale` curve primitive (codegen has no table-lookup primitive yet). | ⬜ |

All four rehydrate cleanly into the designer canvas via `File → Open`
once a project with the demo pack is loaded.

## Safety considerations

Custom features are categorically more dangerous than table edits:

1. **Brick exposure.** A bad table value can over-fuel or knock the engine. A bad patch can corrupt the ECU's stack, fault the watchdog, or refuse to boot. The brick-protection work in `docs/05-improvements.md` §4 (signed-section verification, recovery-mode boot ROM) is a hard prerequisite for shipping this.
2. **Real-time budget.** Inject too much logic into a 5ms ECU loop and the loop overruns, degrading every other function the ECU performs. The linter enforces a worst-case-execution-time budget per hook.
3. **RAM allocation.** The compiler claims scratch RAM from the pack's declared `free_ram` region. Conflicts between two simultaneously loaded features must be detected at load time, not at runtime.
4. **Emissions interaction.** A custom feature that overrides commanded fuel triggers the same jurisdiction-profile linter that emissions-flagged tables trigger (see `docs/06-legal-ethics.md`). Engine-safety refusals still apply unconditionally; jurisdiction refusals follow the user's profile.
5. **Update channel.** A flash gone wrong while a `.stmod` is loaded must be recoverable by un-flashing the patch without losing the user's calibration. The patch format is therefore additive — it never overwrites the original calibration bytes.

## Scope and timing

The original sizing — 4-6 weeks minimum for editor + IR + one
codegen backend + samples; ~10 weeks with both backends — was
conservative. Actual progress:

| Slice | Original estimate | Actual |
|---|---|---|
| Graph data structure + persistence + editor | 2–3 wk | shipped (Phase 5 designer is in `View → Custom features designer (preview)`; editor canvas is functional with pin labels, defaults, pin-context menus, wire dragging) |
| IR + linter + RT-budget analyzer | 2 wk | shipped; RT-budget uses placeholder per-Op costs (real per-ISA cycle counts wait on bench profiling) |
| One codegen backend (SH-2A) | 2–3 wk | shipped: Int + Bool + control flow + Float + Float compares + cross-hook flow + fan-out dedup + `divide_int` (FPU bridge). Open gaps: table-lookup primitives, per-ISA cycle costs. |
| Patch insertion + free-RAM management | 1–2 wk | not started — bench-rig-blocked. Needs a real ECU's vector table and the firmware's known free-RAM map to develop against. |
| Sample packs + docs | 1 wk | 4 samples ship (3 compile end-to-end, 1 waits on `flex_fuel_scale` curve primitive). This doc you're reading is the design + current-state ref. |
| Second backend (RH850) | 2–3 wk | not started; stub returns NotImplemented. Per the original recommendation, RH850 drops first under timing pressure — VA users get custom features, VB waits a release. |

The pieces that REMAIN before custom features ship to a user:

1. **Patch insertion layer** (`src/feature_patch/`) — turns a
   PatchObject into bytes spliced into a ROM image at the right
   addresses. Bench-rig-blocked.
2. **Bench-rig validation** of the SH-2A bytes against a real
   ECU — every emission so far is byte-exact-tested against the
   public Renesas encoding but unverified on silicon. The handoff's
   standing caveat applies.
3. **Phase 3 transport** (`docs/13-transport.md`) — the channel
   that delivers a patched ROM to an ECU. OBDX adapter pending.
4. **Brick recovery story** (`docs/05-improvements.md` §4) —
   signed-section verification + recovery-mode boot ROM remain
   the hard ship gate for any user-facing flash.

The Phase 5 *design + tooling* surface is essentially complete and
the CLI lets you compile features today; the *delivery* path is
still gated on hardware.

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
