# 16 — Custom Features (node-graph extensions)

A **custom feature** is a piece of new ECU behavior the user authors visually, that SubuwuTuner compiles into a ROM patch and flashes alongside the calibration. Table edits change *parameters*; custom features change *control flow*. The two are different categories, and the latter is structurally more dangerous — which is why this is Phase 5, well after manual editing, datalogging, and the brick-protection work in `docs/05-improvements.md` §4.

> **Terminology bridge.** In the community-XML tuning ecosystem this is the equivalent of what's commonly called **"software patches"** or **"ECU patches"** — hand-written SH-2A assembly snippets injected at known ROM offsets to add behaviors like 2-step / flat-foot shift / clutch kill that aren't in the stock cal. SubuwuTuner's contribution is the *authoring layer above* those patches: a visual node-graph designer + IR + linter + codegen that produces the same byte-level output without requiring the user to write assembly directly. The output `.stmod` is the SubuwuTuner-native equivalent of a hand-rolled patch file, and it flashes through the same `st::flash` pipeline as any other ROM change.

This document captures the design. Phase 5 status: the authoring data
model, IR lowerer, **SH-2A codegen for VA** (22 primitives recognized;
fan-out dedup; FPU bridge for Float ops; address-gate refuses splices
outside declared writable regions), CLI (`feature-compile`), and `.stmod`
file format have shipped end-to-end. **RH850 codegen for VB** shipped
2026-06-01 at SH-2A parity: all 3 IR shapes wired (LoadConstant→Store,
LoadHookInput→Store, CallPrimitive) and all 22 leaf-operand primitives
recognized — int arithmetic (add/sub/mul/div), int compares (lt/gt/eq),
bool logic (and/or/not), branchless select (int/bool/float via
mask-merge), float arithmetic (add/sub/mul/div), float unary (`sqrt_float`,
`flex_fuel_scale`), float compares (lt/gt/eq via CMPF.S + TRFSR + SETF) —
plus nested CallPrimitive operands (topological walk + 4-byte RAM slot
per non-root primitive, JMP[lp] tail on root only). One unverified
assumption remains: the float-compare path assumes TRFSR direct-copies
FPU.FCB → PSW.Z; one Cond::Z → Cond::NZ swap reverses it if the
bench rig finds inversion.
**The single biggest remaining open feature** is the patch-insertion layer
(`src/feature_patch/` — finds free RAM, writes the hook table, splices
into existing vector tables). RH850 codegen + patch insertion both gate
on bench-rig work against a real ECU vector table for hardware
validation. See *Current state* below for the granular matrix.

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
| **Compiler (SH-2A)** | Graph → IR → SH-2A machine code → PatchObject. Covers Int arithmetic (add/sub/mul/`divide_int` via FPU bridge), Int compares (lt/gt/eq), Bool ops (and/or/not), select (int/bool/float), Float arithmetic via FPU (FADD/FSUB/FMUL/FDIV), `sqrt_float`, Float compares (FCMP/EQ + FCMP/GT), `flex_fuel_scale`. Handles nested CallPrimitive trees with SSA spill, cross-hook value flow, and fan-out dedup. | ✅ |
| **Compiler (RH850)** | All three IR shapes wired at SH-2A parity. LoadConstant→Store (24 bytes) and LoadHookInput→Store (28 bytes) cover the load/store slices. CallPrimitive covers all 22 leaf-operand primitives: int arithmetic (add/sub/mul/div), int compares (lt/gt/eq), bool logic (and/or/not), branchless select (int/bool/float via mask-merge), float arithmetic (add/sub/mul/div), float unary (`sqrt_float`, `flex_fuel_scale`), float compares (lt/gt/eq via CMPF.S + TRFSR + SETF). Nested CallPrimitive operands wired via topological walk + 4-byte RAM slot per non-root primitive, JMP[lp] tail on root only. `not_bool` lowers as `x XOR 1`; `select_*` uses branchless `(true & mask) \| (false & ~mask)` with `mask = -cond` to preserve the 0/1-normalized Bool invariant. Encodings sourced from public Renesas reference (RH850G3K SW Architecture User's Manual) cross-verified against the markok314/qemu RH850 instmap. The float-compare path assumes TRFSR direct-copies FPU.FCB → PSW.Z; one Cond::Z → Cond::NZ swap reverses it if the bench rig finds inversion. NOT yet validated against a real VB WRX ECU — any RH850 PatchObject is "best effort" until the bench rig signs off. | 🟡 |
| **CLI** | `subuwutuner-cli feature-compile <stmod> --def <pack> [--arch sh2a\|rh850] [--format hex\|toml\|raw\|stmod] [--output <file>] [--validate-only]`. Plus `dump-ir`, `lint-graph`, `lint-ir`. `--format=stmod` bundles graph + patch in a single TOML; `--validate-only` runs parse + lower + compile and exits 0/non-zero without producing output — for CI / pre-commit hooks. | ✅ |
| **Patch format** | `.stmod` — TOML document carrying both the source graph (`[graph]` + `[[node]]` + `[[edge]]`) and the compiled patch (`[patch]` + `[[patch.hook]]` + `[[patch.hook.ram_claim]]`). Single-file, diffable, round-trippable. Signable is a future concern. | ✅ |
| **Linter** | `feature::lint(Graph)` flags undriven inputs + orphan nodes; `feature::ir::lint(Module)` flags duplicate hook overrides + RT-budget overruns. Per-primitive cycle costs in `estimate_cost` (e.g. `divide_int` = 18 cycles, `add_int` = 1) — derived from public SH-2A spec; bench profiling will refine. Unknown symbols default to 3 cycles. | ✅ |
| **Sample packs** | `clutch-kill` (Bool-only synthetic smoke), `flat-foot-shift` (3-sensor AND chain), `launch-control` (4-sensor 3-compare AND tree), `map-selector-int` (`divide_int` end-to-end smoke), `flex-fuel` (`flex_fuel_scale` 1-input curve → `multiply_float` override path), `coolant-gated-flex-fuel` (production-shape composition — flex_fuel_scale + compare_gt_float + select_float + multiply_float, only applies the flex scaling above a 60°C warmup gate). All six compile end-to-end through SH-2A. | ✅ |
| **Patch insertion** | Finding free RAM, writing the hook table, splicing into existing interrupt vectors. Bench-rig-blocked — requires a real ECU to develop against. | ⬜ |
| **Flashing** | Loading a `.stmod` and burning the patch to an ECU. Gates on Patch insertion + Phase 3 transport. | ⬜ |

What does not ship:

- A general-purpose scripting language exposed to users (`Lua` was floated in the Phase 5 roadmap line but is intentionally not on the v1.0 plate — too easy to footgun, too hard to lint). The graph compiler emits machine code directly; the Lua-runtime layer is a possible v2.0+ optimization if performance demands it.
- Auto-import of features from any third-party tool's format. Specs are SubuwuTuner-native; the user re-authors anything they want to carry over.

## Motivating use cases

What problem is custom features solving that table editing alone cannot?

The distinction matters because every "I need to change how my ECU behaves" question has two possible shapes:

1. **The stock control loop is right, just calibrated for different hardware** → edit a *table*. The control law in firmware stays untouched; only the numbers it reads change.
2. **The stock control loop is wrong for what's downstream of it** → write a *custom feature*. The control flow itself has to bend.

The case below is a worked example that hits both categories — the same swap project needs table edits *and* a custom feature, and naming which is which is most of the work.

### Worked example: FA20 → FA24 engine swap into a VA WRX

This is a recurring community project: take a stock 2015–2021 VA WRX (FA20DIT) and bolt in the 2.4L FA24 from a 2022+ VB WRX or Ascent. The result is a bigger, torquier engine in the older chassis with the original ECU and harness retained.

The instructive thing about this case isn't that it needs a custom feature — it's that the **easy story is wrong**. Community shorthand says "custom features handle the FA24 swap," which makes it sound like the swap is a custom-feature problem. After researching what the actual work is, the breakdown is:

| Concern | Reality | Where it lives in SubuwuTuner |
|---|---|---|
| **Cam timing wheels** | FA20 and FA24 trigger wheels emit different signals with a 6° timing offset. The FA20 ECU rejects the FA24's signals outright. | **Hardware** — solved by physically swapping FA20-pattern cam trigger wheels onto the FA24 cams (RS Motors kit, ~$700). Not a software problem. |
| **HPFP control** | The HPFP-driving intake cam lobe is a triangle shape — and between FA20 and FA24 **the triangle got flipped**. The FA20 ECU's commanded HPFP solenoid energize angle lands on the FA24 lobe's *downstroke* instead of its upstroke. The pump runs max duty against the commanded target, climbs to ~4000 psi (the pump can in fact exceed FA20 targets — the failure isn't capacity, it's phase), and the ECU's ~4200 psi safety cutoff kills the engine.<sup>[ntm-2025]</sup> This is a **calibration on HPFP command crank angle**, not on duty / pressure. Subaru community evidence: Atlas is the only currently-tunable platform that exposes this surface; EcuTek and COBB tunes must run FA20 cams to dodge the issue. | **Table edit (preferred) or runtime custom feature** — expose the HPFP command-angle phase-offset calibration (a scalar, possibly per-RPM table) once its FA20 firmware address is in the VA pack. A constant phase offset (~180° crank for a flipped triangle, geometrically) is sufficient as a static edit. For RPM- or load-dependent phase tuning, the runtime path is `fixtures/samples/fa24-hpfp-phase-offset.stmod` over the `override_hpfp_command_angle` hook. Note: rescaling `hpfp_duty_*` / `hpfp_target_pressure_*` tables does **not** fix this — it only changes the magnitude the wrong-stroke command is fighting toward. |
| **VE / MAF scaling** | More displacement = more air per cycle at the same MAP/RPM. | **Table edit** — `st::edit` + v1.1 MAF autotune kernel covers this. |
| **Cam profile differences (LSA, overlap)** | FA24 cams have different duration/lift/centerline than FA20 cams. Affects spark timing, knock margin, AFR targets in transient regions. | **Table edits + definition-pack analyticals** — pack supplies precise FA24 cam specs as constants; derived readouts (LSA, overlap) computed from commanded intake/exhaust VVT angles surface in the datalog UI for tuning visibility. Not a runtime control change. |
| **Injectors** | Most swap kits keep FA20 direct injectors via rail spacers — no rescaling. If different injectors go in, scale flow. | **Table edit.** |
| **3rd-party sensor integration** (aftermarket fuel pressure sensor, oil pressure sensor, ethanol-content sensor) | OEM ECU has no slot for these. To use them in any control decision, the value has to be brought into ECU code at runtime. | **Custom feature.** This is where the node-graph designer earns its keep on a swap project — *not* in the cam-angle or HPFP fix, but in wiring a CAN-bus 3rd-party signal into the FA20 ECU's existing control loops. |

So **the swap is dominated by table edits**, with custom features filling a real but narrower role (3rd-party sensor integration). The common misconception "the cam-angle remap is a custom-feature thing" conflates three separate things:

1. The cam *trigger wheel* signal mismatch (solved mechanically by the hardware swap kit)
2. The cam *profile* differences (covered by table edits + analytical readouts using known FA24 cam specs)
3. The HPFP cam-lobe phase mismatch — the FA24's flipped triangle lobe — fixed by a phase-offset calibration on the FA20 ECU's commanded HPFP solenoid energize crank angle. SubuwuTuner can do this either via a table edit (preferred) or, for RPM/load-dependent phase, via the `override_hpfp_command_angle` runtime hook. Earlier framings (including past versions of this doc) that treated this as a pump-capacity / pressure-target problem are wrong: the FA24 pump readily exceeds FA20-commanded pressures — it just gets driven on the wrong stroke.

**Implications for SubuwuTuner's swap-support story:**

- The infrastructure to support an FA24-into-VA swap already exists in v1.0 (`st::edit`, `Definition` schema with `[[table]]` entries, v1.1 MAF autotune kernel).
- What's missing is **firmware RE work** — finding the FA20 ECU address for the HPFP commanded energize-angle calibration (scalar or table) and adding it to the VA pack. That's a definition-pack contribution, not a code change.
- The custom-features designer is the right tool for **3rd-party sensor integration** patterns the swap user might pursue separately (aftermarket fuel pressure sensor → CAN bus → override decision), and for the RPM/load-dependent HPFP phase-offset variant (`fa24-hpfp-phase-offset.stmod`). The earlier samples `fa24-hpfp-clamp.stmod`, `fa24-aux-pressure-clamp.stmod`, and `fa24-bernoulli-comp.stmod` were drafted under the wrong premise that the FA24 pump can't reach FA20 targets — useful as template runtime-clamp / Bernoulli-PW shapes for other applications, but not the actual fix for this swap.

> <sup>[ntm-2025]</sup> Nick Taormina (NTMotorsports), *Budget VA FA24DIT Swap*, December 2025 — page 5 §"Cams". Local copy: `D:/Subuwu/findings/external-refs/NTMotorsports_VA_FA24_Swap_BaseMap.pdf`. Author also offers a paid VA FA24 swap base map starting Atlas 2026.1, which is the ground-truth reference for the actual phase-offset value (placeholder in `fa24-hpfp-phase-offset.stmod` is 180.0° crank).

### Public references for the swap

Hardware kits + harnesses:

- [iBuildRacecars FA24 Engine Swap Kit](https://www.ibuildracecars.com/store/fa-24-manifold-swap-kit) — production kit; keeps FA20 ECU + FA20 injectors via rail spacers
- [RS Motors FA24 swap kit (Facebook product post)](https://www.facebook.com/rs.motors.burnsville/posts/) — supplies the four FA20-pattern cam trigger wheels + crank reluctor + fuel pressure sensor; **this is what makes the cam-trigger-signal problem hardware-only instead of software-only**
- [Hachi Electronics FA24 Engine Swap Harness](https://hachielectronics.com/products/fa24-engine-swap-harness) — adapter harness for sensor-connector differences
- [NTMotorsports FA24 Swap Harness Kit](https://www.ntmotorsports.com/shop/p/q6vuqrz3imvb13rkyryh74l2m24lt5) — injector / temperature sensor / MAP sensor harness for VA WRX FA24 swap
- [Nostrum FA24 HPFP product page](https://nostrum.mybigcommerce.com/subaru-fa24-upgraded-high-pressure-fuel-pump/) — confirms the FA24 stock pump's flow capacity vs FA20's, useful when sizing an aftermarket pump upgrade

Forum threads + community discussion:

- [Subaru WRX Forum — FA20 to FA24 swap thread](https://www.clubwrx.net/threads/fa20-to-fa24-swap.134622150/) — community-level discussion of both install paths
- [GR86 Forum — FA24 swap into 1st gen FRS/86/BRZ, page 4](https://www.gr86.org/threads/how-to-fa24-swap-into-1st-generation-frs-86-brz.12044/page-4) — has the most detailed HPFP / cam-lobe-lift technical detail
- [FA24 Swap into 2015 WRX forum thread (paywalled — search-indexed only)](https://forums.nasioc.com/forums/showthread.php?t=2952254) — primary VA-WRX-specific build thread; full body behind a paywall but appears in search results

### Real-world VA-gen FA24 swap projects (2017+)

The "only 2 documented projects" line in community threads is outdated. Confirmed working VA-gen FA24 swaps as of early 2026:

| Project | Donor car | Tuner | Software | Result |
|---|---|---|---|---|
| **[Prime Motoring + JrTuned](https://forums.nasioc.com/forums/showthread.php?t=2907007)** | 2017 WRX, FA24 from Ascent, stock motor | JrTuned | **aftermarket handheld flasher + WRX (FA20) ECU** | **502 whp / 484 lb-ft @ 22.1 psi**, hit fueling ceiling, planning speed-density + port injection |
| **[Six Star SPF](https://www.facebook.com/SixStarSPF/videos/659521719420930/)** | **2020 WRX**, FA24 swap | Six Star Performance Fab | (unspecified — likely an aftermarket handheld flasher) | **450 WHP / 440 WTQ on stock turbo, E85**, Killer B header, iterative dyno sessions |
| **["FIRST DRIVE in FA24 Swapped VA WRX"](https://m.youtube.com/watch?v=1mq5QGwBNF0)** | VA WRX (Mar 2025) | (in-video) | (in-video) | Single driving-impressions video |
| **["Cheapest VA FA24 Engine Swap" series](https://www.youtube.com/watch?v=4g8HSnPVn2Q)** | VA WRX (Part 1 → Part 3 Feb 2026) | (in-video) | (in-video) | Multi-part low-budget build documentation |

**Key takeaway:** Prime Motoring's 502 whp build runs on the **stock FA20 ECU + a mainstream aftermarket handheld flasher alone** — no alternative-vendor tools, no standalone ECU. The HPFP ceiling is the documented wall at that power level (~500 whp), with the standard workaround being speed-density mode or port injection — not a custom-feature compensation. So the swap is **less software-dependent than community shorthand implies**, at least on the mainstream-flasher path.

### Tuning-path matrix

| Path | What it does | FA24-swap fit |
|---|---|---|
| **Mainstream commercial handheld flasher + matching desktop tuner app** | Stock FA20 ECU; full Subaru factory table catalog; vendor-shipped custom-features pack (flex fuel, 2-step, etc.); **speed-density mode** bypasses the MAF range limit; **differential fuel-pressure compensation** uses an aftermarket fuel pressure sensor to scale injector pulse-width via Bernoulli's principle (`flow ∝ √Δp`). Does **not** expose the HPFP command-angle phase calibration, so FA24-cam installs on this path must accept the wrong-stroke behavior or run FA20 cams. | Production-validated to ≥500 whp on 2017 WRX (Prime Motoring), but with FA20 cams installed. The mainstream path for FA20-cam swaps; not viable as-is for FA24-cam swaps. |
| **Free open-source competing tuner (Atlas)** | Stock FA20 ECU; full factory table catalog; **HPFP command-angle phase-offset exposed** (the only currently-tunable platform that does — confirmed by NTMotorsports<sup>[ntm-2025]</sup>); custom-features designer can do cam-trigger signal interpretation in software if the RS Motors hardware kit isn't installed; analyticals for FA24 cam profile (LSA, overlap) | The path that lets FA24 cams stay installed. NTMotorsports' Atlas 2026.1 base map ships on this platform. |
| **Another commercial tuning tool** | Stock FA20 ECU; **does not expose HPFP command-angle phase calibration** — FA24-cam installs run the pump out of phase, hit the ~4200 psi safety cutoff, and the engine shuts down. Must run FA20 cams. | Workable for FA20-cam swaps only. FA24 cams force a platform change to Atlas. |
| **Standalone ECU** (e.g. Link, Haltech, MoTeC) | Replaces the FA20 ECU entirely with a fully tunable standalone | Most flexible, most expensive, most invasive — usually a last resort |

### Resolving the "custom features make the FA24 cams work" claim

The forum statement *"the swap kit alone is not enough to make the FA24 cams work with the FA20 ECU — that will need custom features"* turns out to be conditional, not absolute. Two paths to the same outcome:

- **Path A — Hardware fix:** install the RS Motors swap kit's four FA20-pattern cam trigger wheels on the FA24 cams. The FA24 cams now emit cam-position signals the FA20 ECU recognizes. **No software cam-signal interpretation needed.** Prime Motoring + Six Star SPF take this path.
- **Path B — Software fix:** skip the RS Motors trigger-wheel swap, leave the FA24 cams emitting their native pattern, and use a custom-features patch to interpret the FA24 trigger pattern in firmware code. Cheaper hardware bill, more involved software work.

Both produce a running car. The "custom features are required" framing applies only to Path B.

### Implications for SubuwuTuner's swap-support story

The infrastructure to support an FA24-into-VA swap is **mostly in v1.0 already**:

- `st::edit` covers VE / AVCS / spark / fuel table edits — same shape as any other Subaru calibration table
- `st::edit` also covers the HPFP command-angle phase-offset calibration (the actual FA24 swap fix, see "HPFP control" row in the worked example above) once its FA20 firmware address is in the VA pack
- v1.1 MAF autotune kernel covers the air-mass rescaling for the larger displacement
- The custom-features designer (`src/feature` + SH-2A codegen) covers two patterns relevant to the swap: (a) the **3rd-party-sensor-integration pattern** — the same shape as the differential-fuel-pressure compensation that mainstream commercial flashers ship (Bernoulli-based injector-scale correction over an aftermarket fuel-pressure sensor), and (b) **RPM/load-dependent HPFP phase tuning** if a constant offset isn't sufficient. See `fixtures/samples/fa24-hpfp-phase-offset.stmod` for the phase shape and `fa24-bernoulli-comp.stmod` for the Bernoulli shape.

What's missing is **firmware-RE work**: finding the FA20 ECU addresses for the HPFP command-angle phase calibration plus the VE / AVCS / fueling tables. That's a definition-pack contribution, not a code change. Once the HPFP-angle address is in the VA pack, FA24-cam swaps are supported via a static table edit (no runtime patch needed); the runtime hook is reserved for users who want RPM/load-conditional behavior. The other table edits make the broader swap calibration tractable.

For Path B (no RS Motors kit) support — software cam-trigger-signal interpretation — there's a real custom-features design problem still open: the IR doesn't yet have a primitive for "decode this cam-position pulse train and emit a derived cam-angle value." That would join `flex_fuel_scale` and any future curve / table-lookup primitives on the "primitives we'd add when a user pulls them" list. **Not a v1.x gate.**

### IR primitives the FA24 swap pattern pulled on

> **Note (2026-06-07):** The Bernoulli pipeline below was originally motivated by the belief that the FA24-into-VA swap needs pressure-undershoot compensation on injector PW. We now know — per NTMotorsports<sup>[ntm-2025]</sup> — that the actual FA24 swap fix is HPFP command-angle phase, not pressure compensation, and the FA24 pump has no capacity shortfall to compensate for. The primitives (`sqrt_float` etc.) still shipped and remain correct + useful for *real* pressure-undershoot scenarios (aftermarket pump upgrades pushed past spec, MAF-out-of-range rich-region fallbacks, etc.). The FA24 framing on the sample fixture is the historical artifact; the math and primitives stand.

The Bernoulli-based differential-fuel-pressure correction the mainstream commercial flashers ship is one multiplication and one square-root over runtime sensor values. The math:

```
correction = sqrt( actual_rail_pressure_bar / commanded_rail_pressure_bar )
```

is one `divide_float` plus a `sqrt_float` plus a `multiply_float`. **`sqrt_float` shipped alongside this doc** — single-operand FPU primitive, emits `FSQRT FRn` (`0xF06D | (n << 8)`), cycle cost ~15 (FSQRT latency dominates, similar to FDIV). The full pipeline is exercised end-to-end by [`fixtures/samples/fa24-bernoulli-comp.stmod`](../fixtures/samples/fa24-bernoulli-comp.stmod), which compiles to 108 bytes of SH-2A machine code against the demo pack.

Still missing from the "really do an FA24 swap" custom-features story:

- **Curve / table-lookup primitive** — needed for `flex_fuel_scale` and for any production-quality compensation whose reference values depend on RPM × load (e.g. a load-dependent Bernoulli `nominal_dp` baseline, or a temperature-dependent fueling trim). Sample `flex-fuel.stmod` is the canonical blocker.
- **CAN-RX hook type** — the aux sensor pattern in this doc piggybacks on the OEM ECU's existing CAN driver depositing values at a known RAM address. A dedicated CAN-RX node type would let SubuwuTuner emit the CAN-driver-config bytes directly, removing the configuration handoff.
- **Low-pass filter primitive** — on-throttle transient compensation needs the actual-pressure input filtered (the aux sensor's reading lags reality by several ms during a step transient). One-state IIR filter; a `filter_lpf_float(input, alpha)` primitive would express the standard `y[n] = α·x[n] + (1−α)·y[n−1]` shape and need IR + codegen support for per-instance persistent state.

What **isn't** missing anymore:

- `override_injector_pw` hook — Bernoulli compensation properly lives on the injector PW side; HPFP-target compensation would just fight the OEM PID.
- `sqrt_float` primitive — `FSQRT FRn`, see SH-2A primitive coverage table above.
- **Manifold-pressure-aware ΔP** in `fa24-bernoulli-comp.stmod` — uses `(rail − manifold)` for both commanded and actual pressures, the physically-correct quantity for orifice flow.
- **Bilateral correction-factor clamping** in `fa24-bernoulli-comp.stmod` — clamps correction to `[0.8, 1.4]` via two `compare/select` pairs, guarding against transient sensor faults driving the injector to absurd duty.

### Other use cases for the custom-features designer

- **EJ20 / EJ25 platform → newer-sensor retrofits** (`docs/04-roadmap.md` v1.3) — older Subarus retrofitted with later sensor packages have analogous gaps where the OEM ECU has no slot for the new sensor's signal.
- **CAN-bus aftermarket sensors** (oil pressure, ethanol content, MAP from an aftermarket sensor, EGT) — generally referred to in the community as "3rd-party sensor integration." Hook = CAN RX + override target. This is the *primary* swap-related use case for the custom-features designer.
- **Anti-lag bang-bang fuel cut + custom rev limiter + clutch kill + flat-foot shift** — pure motorsport features that don't fit OEM behavior. The existing samples (`clutch-kill`, `flat-foot-shift`, `launch-control`) cover this category.

These are not v1.0 ship-gate items — the use cases live at user-pull speed. But the *design surface* is the same and the doc lives here, not in a separate "engine swap" file.

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
| `sqrt_float` | (Float,) | Float | `FSQRT FRn` |
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

Five starter `.stmod` files ship in `fixtures/samples/`. All paired
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
| `map-selector-int.stmod` | 40 | `LoadConstant(int)` × 2 → `divide_int` → store into `set_active_map.map_index`. Exercises the SH-2A FPU bridge (FLOAT → FDIV → FTRC) and the Int store path end-to-end. | ✅ |
| `flex-fuel.stmod` | 80 | Read ethanol-content sensor → `flex_fuel_scale` curve (E0=1.00 → E85=1.28 linear, hardcoded) → multiply commanded fuel pulse width → write back. The 1-arity curve form fits the existing 3-slot primitive-shape table; a general N-point lookup primitive lands in a future bundle. | ✅ |

All five rehydrate cleanly into the designer canvas via `File → Open`
once a project with the demo pack is loaded.

## Safety considerations

Custom features are categorically more dangerous than table edits:

1. **Brick exposure.** A bad table value can over-fuel or knock the engine. A bad patch can corrupt the ECU's stack, fault the watchdog, or refuse to boot. The brick-protection work in `docs/05-improvements.md` §4 (signed-section verification, recovery-mode boot ROM) is a hard prerequisite for shipping this.
2. **Real-time budget.** Inject too much logic into a 5ms ECU loop and the loop overruns, degrading every other function the ECU performs. The linter enforces a worst-case-execution-time budget per hook.
3. **RAM allocation.** The compiler claims scratch RAM from the pack's declared `free_ram` region. Conflicts between two simultaneously loaded features must be detected at load time, not at runtime.
4. **Emissions interaction.** A custom feature that overrides commanded fuel triggers the same jurisdiction-profile linter that emissions-flagged tables trigger (see `docs/06-legal-ethics.md`). Engine-safety refusals still apply unconditionally; jurisdiction refusals follow the user's profile.
5. **Update channel.** A flash gone wrong while a `.stmod` is loaded must be recoverable by un-flashing the patch without losing the user's calibration. The patch format is therefore additive — it never overwrites the original calibration bytes.
6. **Codegen address gate — the pack is the source of truth.** `st::feature::codegen::gate_patch` refuses to emit a `PatchObject` whose per-hook `[splice_address, splice_address + code.size())` range is not fully contained inside one `[[writable_region]]` of the loaded `Definition` (schema in `docs/11-definition-format.md`). RAM is gated separately by each hook's `free_ram` declaration + `RamAllocator`. The graph cannot widen the surface; the pack does. This is a security boundary, not a sanity check — a malicious or buggy `.stmod` graph that bypassed this would target arbitrary flash addresses through the same `st::flash` pipeline table edits use. Fail-closed: a pack with zero `[[writable_region]]` entries rejects every patch. Wired into `Sh2aBackend::compile()`'s exit path so callers can't bypass it. Coverage at `tests/unit/feature_codegen/test_address_gate.cpp` (15 cases: vacuous-pass, fail-closed, single-region containment + boundary cases, multi-region disjoint coverage, hook-spans-two-regions refusal, multi-hook violation reporting).

## Live-toggleable features

A feature can declare a **RAM-mapped enable flag** so it can be turned on or off without a reflash. The pack's `[[feature]]` block carries an optional `enable_ram_address` (and optional `scalar_param_ram_address`); the generated SH-2A/RH850 code reads those addresses every loop iteration and gates behavior accordingly. Toggling becomes a single UDS `WriteDataByIdentifier` to the RAM address — the same primitive `docs/19-live-tuning.md` uses for live calibration writes.

This is what makes the handheld-tuner "turn Launch Control on from the device screen" UX possible without a reflash. See `docs/18-standalone-master-plan.md` §12 for the handheld-side UX design and the safety constraints that wrap it (engine-safety toggles do not surface on the hardware screen even with a RAM-mapped flag — those route through the desktop GUI with explicit confirmation).

Not every feature is live-toggleable. Features that wire deep into a hook chain (anti-lag bang-bang fuel cut, custom rev limiter) can be — they branch on the flag and fall through to OEM behavior when off. Features that *replace* a stock function (custom shift logic, a full-replacement boost controller) generally cannot be, because the OEM code path they replace no longer exists. That distinction is per-feature in the pack metadata.

## Scope and timing

The original sizing — 4-6 weeks minimum for editor + IR + one
codegen backend + samples; ~10 weeks with both backends — was
conservative. Actual progress:

| Slice | Original estimate | Actual |
|---|---|---|
| Graph data structure + persistence + editor | 2–3 wk | shipped (Phase 5 designer is in `View → Custom features designer (preview)`; editor canvas is functional with pin labels, defaults, pin-context menus, wire dragging) |
| IR + linter + RT-budget analyzer | 2 wk | shipped; RT-budget uses placeholder per-Op costs (real per-ISA cycle counts wait on bench profiling) |
| One codegen backend (SH-2A) | 2–3 wk | shipped: Int + Bool + control flow + Float + Float compares + cross-hook flow + fan-out dedup + `divide_int` (FPU bridge) + `sqrt_float` (FSQRT). Open gaps: table-lookup primitives, per-ISA cycle costs. |
| Patch insertion + free-RAM management | 1–2 wk | not started — bench-rig-blocked. Needs a real ECU's vector table and the firmware's known free-RAM map to develop against. |
| Sample packs + docs | 1 wk | 4 samples ship (3 compile end-to-end, 1 waits on `flex_fuel_scale` curve primitive). This doc you're reading is the design + current-state ref. |
| Second backend (RH850) | 2–3 wk | shipped: LoadConstant + LoadHookInput slices (24 + 28 B respectively) plus a 22-primitive CallPrimitive slice at SH-2A parity — int arithmetic (`add_int`, `subtract_int`, `multiply_int`, `divide_int`), int compares (`compare_lt_int`, `compare_gt_int`, `compare_eq_int`), bool logic (`and_bool`, `or_bool`, `not_bool`), branchless select (`select_int`, `select_bool`, `select_float`), float arithmetic (`add_float`, `subtract_float`, `multiply_float`, `divide_float`), float unary (`sqrt_float`, `flex_fuel_scale`), and float compares (`compare_lt_float`, `compare_gt_float`, `compare_eq_float`). Nested CallPrimitive operands supported — the nested driver allocates a 4-byte RAM slot per non-root primitive, emits each fragment in topological order (no JMP[lp] tail), and the root emits with terminator. Bench-rig HIL is the only remaining gate before any RH850 PatchObject reaches a real ECU — in particular the TRFSR-direction caveat on float compares (we assume direct copy of FPU.FCB → PSW.Z; bench data decides). |

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
