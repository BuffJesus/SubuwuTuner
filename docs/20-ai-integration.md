# 20 — AI integration

AI in tuning is mostly marketing in 2026. "AI tunes your car" pitches from commercial vendors are either branding over deterministic statistics, or — when they're real — a category of feature SubuwuTuner explicitly refuses to build (see `docs/12-auto-tuning.md` *What we will not ship*). This document is about what AI *can* contribute that is **actually useful, technically honest, and safe** — and how the project intends to layer it in starting v2.0.

The short version: AI sits on the *interpretation* side of the workflow, not the *modification* side. It helps the user understand what their car is telling them. It does not decide what bytes to write to the ECU.

This doc is forward-looking. **Nothing in `st::ai` is implemented yet.** v1.0–v1.5 has no AI surface. The earliest landing is v2.0.

## The spectrum

Useful → speculative → don't-ship, in descending order of "we should actually build this":

| Tier | Feature | What it does | Why it works |
|---|---|---|---|
| 1 | **Drift classifier** (rules-based) | Classifies LTFT/STFT/DAM drift patterns against a known taxonomy (vacuum leak vs. injector aging vs. MAF aging vs. O2 sensor failing). Deterministic — no ML. | Community has decades of accumulated pattern→cause lore. Encoding it as rules over the existing `st::log::adaptive` snapshots gets 80% of the value with debuggable code. |
| 2 | **LLM explanation layer** | Translates classifier output into a human narrative ("your idle cells are drifting four times faster than your load cells — signature of unmetered air"). Pure prose generation, no diagnosis logic. | Auditable: the classifier did the reasoning; the LLM does the talking. |
| 3 | **"Explain this log" assistant** | User uploads a CSV, asks free-form questions. LLM walks through anomalies, points at relevant tables, suggests checks. | Structured snapshots (`KnockSnapshot` etc.) are pre-digested context; the LLM doesn't have to parse CSVs from scratch. |
| 4 | **Custom-feature-from-description** | "Add launch control at 4000 RPM when brake is pressed" → LLM drafts a `.stmod` graph using our IR / node taxonomy. The graph-linter + RT-budget check + engine-safety policy run normally. | LLM drafts; deterministic toolchain validates. Hard parts (does this compile? is it real-time-safe?) stay in our code. |
| 5 | **Definition-pack development acceleration** | Pattern-recognition over decoded ROM bytes — "this 256-byte aligned region looks like a 16×16 fuel map at this scaling." Output is a draft TOML for human review. | Closes the def-pack gap with commercial tools faster. Wrong classification just means a failed import attempt; no engine risk. |
| 6 | **CAN signal naming proposals** | After `docs/14`'s discovery loop labels enough events, LLM proposes a human-readable signal name. | Low stakes (user reviews the name). Reuses existing discovery infrastructure. |
| 7 | **Trained drift classifier** (ML, v2.x+) | Replaces / augments Tier 1 with a small classifier trained on community-contributed labeled examples. | Handles edge cases the rules miss. Bar to ship: demonstrably better than Tier 1 on a holdout set. |
| 8 | **Cipher / encoding classification** | ML over byte-distribution features to identify ROM-encryption schemes (EpifanSoft swap-bit vs FA-DIT per-CID vs unknown). | Currently hand-classified; mid-stakes; wrong classification = failed decode attempt. |
| — | ~~"AI-generated tune from scratch"~~ | Marketing pitch. Not real. Not safe. | Refused. Same red line as `docs/12`. |
| — | ~~Auto-flash from AI suggestions~~ | Closes the loop observation → flash without explicit user review. | Refused. Out of scope regardless of whether AI is involved. |

Tiers 1–3 are the v2.0 landing. Tiers 4–6 are v2.1+. Tier 7 needs a training-data pipeline that doesn't exist yet. Tier 8 is research.

## The canonical example: drift classifier

Fuel-trim drift is the textbook case. Working through it specifies the shape every later AI feature follows.

### Inputs

The classifier consumes data we already produce:
- `st::log::adaptive::HistorySnapshot` — LTFT / DAM / IdleAdapt time-series with drift slopes
- (Optional, firmware-dependent) per-cell LTFT array — some FA-DIT ROMs expose it via SSM extended PIDs; older ECUs do not
- (Optional) STFT noise statistics from the same log — variance / autocorrelation at idle

### Rules

Each diagnosis is a deterministic rule over snapshot features:

| LTFT signature | STFT signature | Diagnosis |
|---|---|---|
| Idle cells more negative than load cells, slow weekly drift | Steady, low noise | **Vacuum / EVAP leak** |
| WOT cells more negative than idle, slow drift | Steady | **Injector aging or fuel pressure decay** |
| All cells equally negative, fast drift | Higher than usual | **MAF sensor aging / contamination** |
| LTFT stable but STFT noise floor 3× typical | Bouncing ±10% at idle | **O2 sensor failing / exhaust leak** |
| LTFT positive trend across all cells | Noisy | **Fuel quality drift / ethanol content change** |
| Sudden step change | Noisy at first then settles | **Recent flash or sensor disconnect** |

When the rules say "ambiguous" (multiple causes overlap within tolerance), the classifier returns `DriftDiagnosis::Ambiguous` with the candidate causes listed in order of evidence strength. The UI surfaces this as "could be either; here's what to check first."

### Output shape

```cpp
namespace st::ai::drift {

enum class Confidence : std::uint8_t { Likely, Possible, Ambiguous, NoSignal };

struct DriftDiagnosis {
    std::string_view  cause;            // "vacuum_leak", "injector_aging", …
    Confidence        confidence;
    std::vector<std::string> evidence;  // pre-rendered reasons
    std::vector<std::string> alternatives;  // "could also be …"
    std::vector<std::string> recommended_checks;
};

[[nodiscard]] DriftDiagnosis classify(
    st::log::adaptive::HistorySnapshot const &history,
    DiagnosisInputs const                    &extras);

}  // namespace st::ai::drift
```

Deliberately no LLM dependency here. `classify()` is a pure function the test suite can pin against synthetic snapshots.

### LLM layer (Tier 2)

When the user asks "why does it think vacuum leak?", `st::ai::Explain::why(diagnosis)` runs the diagnosis evidence through a local LLM with a prompt template that says "explain this diagnosis to a tuner; don't speculate beyond the evidence." The output is descriptive, never additive — if the LLM hallucinates a sixth piece of evidence not in `diagnosis.evidence`, the system filters it out and shows a "show raw evidence" toggle so the user can audit.

Why this separation matters: the classifier is testable in CI without a model loaded. The LLM is replaceable (Ollama / OpenAI / Anthropic / future). Their failure modes are independent.

## Composite: goal-conditioned tuning coach (v2.1+)

Tiers 1–3 are point features ("here's what your log says"). A natural composite layers them into a multi-step guided workflow: the user states their tuning goals up front, the coach walks them through a step-by-step plan, evaluates each step's log before suggesting the next, and proposes specific edits with rationale at every gate. **This is what most users mean by "AI tuning"** when they ask — and it can be built safely from the existing tier substrate without slipping into the auto-tune / auto-flash red lines.

The coach is not a new tier; it's a goal-conditioned wrapper around Tiers 1+2+3 (with Tier 4 optional for advanced "add a feature" steps once that tier lands).

### Phases

1. **Goal elicitation** — structured intake form: target peak power, fuel grade, hardware modifications, intended use, risk tolerance, jurisdiction profile. Saved to `[ai.goals]` in `project.toml`. The coach refuses to plan against absent fields — no implicit defaults for "what fuel".
2. **Gap analysis** — deterministic checks against the goals before any tuning begins: stock-injector saturation against target power, fuel-system flow headroom, fuel-grade vs. knock-margin envelope, hardware mismatches (e.g., a Stage-3 boost target on a Stage-1 intake). Rules decide; the LLM narrates the result. Blockers are surfaced as "fix this before continuing" rather than auto-relaxed.
3. **Playbook generation** — an ordered list of tuning steps with explicit success criteria per step. Example for a +40 hp / 93-octane / Stage-2-hardware project: (1) baseline WOT log, (2) MAF scaling pull, (3) knock-margin sweep at high load, (4) boost target stairstep, (5) wastegate-duty re-trim, (6) final verification pull. User reviews the playbook and edits / reorders / skips steps before starting — saved to `<project>.stune/ai/playbook-*.toml`.
4. **Per-step interpretation loop** — user runs a step, uploads the resulting log, the coach evaluates the log against that step's success criteria via Tier 1, narrates findings via Tier 2, suggests specific cell edits with rationale, and the user reviews + applies each edit through the existing edit dialog (which records to `st::edit::History` like any other edit). The coach advances to the next step only when the user marks the current step's criteria as met.

### What the coach is *not*

- **Not auto-applying edits.** The coach proposes; the user clicks Apply through the same dialog as any manual edit. `st::policy` engine-safety gates check coach-proposed edits identically to manual ones — no "AI override" channel.
- **Not generating calibration values from scratch.** Coach-proposed edits are derived by the same auto-tune kernels (`docs/12`) the user could invoke manually. The AI's contribution is *which kernel to run given the goal*, not the cal math itself.
- **Not paternalistic.** Experienced tuners can mark steps as already-done, override the playbook, skip the narrative, or feed raw logs directly. The coach has a "quiet mode" that strips the prose down to bare diagnoses + edit proposals.
- **Not bypassing jurisdiction policy.** A coach asked to plan around emissions equipment routes through `docs/06`'s profile layer — warned per profile, never silently fulfilled, never refused on regulatory grounds for an Alberta user.
- **Not closing the loop.** No flash decision passes through the coach. "Coach proposes edits → user applies → flash" is the workflow; the coach never sees the Flash button.

### Goal schema (sketch)

Saved to `<project>.stune/project.toml` under `[ai.goals]`:

```toml
[ai.goals]
schema = "subuwutuner.tuning_goals.v1"

target_peak_hp = 340                    # wheel hp
fuel = "93"                             # "91" | "93" | "e30" | "e40" | "e85"
hardware = [
    "3-port boost solenoid",
    "Cobb Stage 2 intake",
    "TMIC",
]
use = "daily-summer-only"               # | "track-only" | "drag-only" | "year-round-daily"
risk_tolerance = "conservative"         # | "moderate" | "aggressive"
jurisdiction = "ca-ab"                  # matches existing profile chip
notes = """
Pump 93 available locally year-round.
Owner plans E30 blend for summer pulls only.
"""
```

Each goal field has a deterministic check the coach can run during gap analysis. Missing fields block planning until filled.

### Playbook schema (sketch)

```toml
[playbook]
schema = "subuwutuner.tuning_playbook.v1"
goal_snapshot_hash = "sha256:..."       # invalidates the playbook if goals change;
                                        # algo follows whatever st::project hashing
                                        # currently uses (SHA-256 today, BLAKE3 once
                                        # the flash-hash upgrade lands)

[[step]]
id = "baseline"
title = "Baseline WOT log"
success_criteria = [
    "≥60s WOT pull in 3rd or 4th gear",
    "no fuel-cut DTCs",
    "AFR trace stable (no fuel-pump dropout)",
]
suggested_log_pids = ["RPM", "Load", "MAF", "STFT", "LTFT", "FBKC", "FLKC", "Boost", "WGDC"]
status = "pending"                      # | "in-progress" | "passed" | "needs-redo"
notes = ""
```

The playbook is regular TOML — the user can edit it directly, the coach re-reads it on every interaction.

### Why this composes safely

The coach introduces zero new write capabilities. Every action it can take, the user could already take manually:

- It reads logs through the same `st::log` snapshots the existing analyzers consume.
- It proposes edits through the same edit dialog that accepts manual cell changes.
- It runs the same auto-tune kernels (`docs/12`) the user can invoke from the menu.
- The only new logic is the goal-conditioned planning — and planning is suggestion-only, fully editable, fully overridable.

Failure modes of the AI layer (hallucination, misclassification, wrong step ordering) downgrade to "the coach suggested a wrong next step," which has the same severity as a human tuner suggesting a wrong next step. Engine-safety policy and jurisdiction policy remain authoritative, not advisory.

## Architectural fit

```
   Existing st::log::* snapshots
   ┌───────────────────────────┐
   │ KnockSnapshot             │
   │ HistorySnapshot           │ ── all already structured;
   │ ColdStartSnapshot         │    LLMs consume them as
   └───────────────────────────┘    pre-digested context
                │
                ▼
   ┌───────────────────────────┐
   │ st::ai::drift::classify   │  rules-based, no ML
   │ st::ai::knock::classify   │  rules-based, no ML
   │ …                         │
   └───────────────────────────┘
                │
                ▼
   ┌───────────────────────────┐  ┌─────────────────────────┐
   │ st::ai::Explain::why()    │──│ Pluggable backend       │
   │   (LLM, optional)         │  │  - Ollama (local)       │
   │                           │  │  - OpenAI               │
   │                           │  │  - Anthropic            │
   └───────────────────────────┘  └─────────────────────────┘
                │
                ▼
   GUI surface — advisory only, never auto-applied
```

New module: `st::ai` (new directory `src/ai/`). Compile-time-optional via a CMake flag (`-DST_AI=ON/OFF`); the default for v2.0 is OFF until the substrate is mature. The CLI binary stays usable without AI: every `st::ai::*` call site has a non-AI fallback ("AI subsystem disabled in this build").

### Backend plug

```cpp
namespace st::ai {

class Backend {
  public:
    virtual ~Backend() = default;
    virtual Result<std::string> complete(std::string_view system_prompt,
                                          std::string_view user_prompt) = 0;
    virtual BackendInfo info() const = 0;  // name, locality, cost-class
};

std::unique_ptr<Backend> make_ollama_backend(std::string_view model);
std::unique_ptr<Backend> make_openai_backend(std::string_view api_key);  // opt-in
std::unique_ptr<Backend> make_anthropic_backend(std::string_view api_key);  // opt-in

}  // namespace st::ai
```

`Backend::info()` tells the GUI whether output came from a local model (badge: "local"), an API call (badge: "API · provider · cost N tokens"), or a deterministic non-LLM path. The user sees the data flow before clicking.

## Privacy and safety posture

Strict by design:

1. **Local-first.** Default backend is Ollama running on the user's machine. No network call without explicit per-feature opt-in.
2. **Data preview before transmission.** Cloud calls show the exact prompt — including pasted snapshot data — in a confirm dialog before the request goes out. The user can edit or cancel.
3. **No telemetry tied to AI usage.** Crash-only opt-in remains the global stance (`docs/05` §10); AI features don't add a telemetry channel.
4. **Engine-safety boundary unchanged.** `st::policy::evaluate_plan_policy` runs on every proposed edit, whether the proposer is the user, an auto-tune kernel, or an AI suggestion. No exception. Engine-safety verdicts stay blocking in every jurisdiction profile (`docs/06`).
5. **Advisory output, never auto-applied.** No `st::ai::*` function returns a `FlashPlan` or writes to a `Project`. Output is a `DriftDiagnosis` / `Explanation` / `FeatureGraphDraft` that the user reviews and accepts. The reviewing UI is explicit — a "Accept AI proposal" button, not a silent merge.
6. **Provenance preserved.** Every AI-derived artifact in a `.stune` project carries metadata identifying the backend (model name, version, prompt hash, locality). When a flash is initiated against a project containing AI-derived changes, the policy gate surfaces this so the user is reminded.
7. **No model trained on protected references.** See *Clean-room* below.

### Specific to fuel-trim diagnosis

- The classifier output is *suggestion* prose ("consider checking fuel pressure"), not commands. It does not write to the cal.
- If the user wants to follow the suggestion ("re-run MAF auto-tune after fixing the vacuum leak"), they execute that workflow themselves through the existing `subuwutuner-cli autotune maf` flow. The classifier doesn't have a write path into it.
- Diagnoses are cached against the input snapshot's hash so they're reproducible — the same log + same backend → same diagnosis. Helps audit "what did the tool tell me last week?"

## Clean-room boundaries

The same red lines from `CLAUDE.md` / `docs/15` apply, with one notable addition:

**Training data must be clean.** Specifically:

- ✅ **Public engine-management literature**, SAE/ISO standards, public protocol documentation, peer-reviewed engine-research papers — fair use as training corpus.
- ✅ **Public RomRaider GPL source** — protocol facts and definitions are derivable from the GPL; their *prose* (description strings) is not. A model trained on RR XML *facts* is fine; a model that memorized RR description prose is not (it would reproduce GPL'd text into our Apache-2.0 outputs).
- ✅ **Community forum posts** that are explicitly licensed for reuse (Creative Commons, public domain, original-poster-permitted). When in doubt, exclude.
- ✅ **User-contributed labeled examples** with explicit opt-in. Format: `(input_snapshot, ground_truth_label)` pairs where the user diagnosed a real problem and is willing to contribute the trace. Provenance + consent recorded; users can revoke.
- ❌ **Commercial tuning tool decompiles** (COBB, EcuTek, HP Tuners, Atlas, etc.) — not as training data, not as RAG context, not as eval data. Same red line as everywhere else.
- ❌ **OEM service-tool reverse-engineered data** unless documented as a public ISO 14229 / SAE J2534 fact.
- ❌ **Synthetic data generated from an LLM** that was itself trained on protected content — laundering doesn't make it clean.

**Inference-time RAG / context injection** follows the same rules. If we ship a "ask questions about your tune" assistant, the retrieval corpus is `docs/`, `src/log/` snapshot specs, `definitions/` schemas, and explicitly licensed public references. Not COBB's published Subaru tune notes. Not EcuTek's customer documentation.

**A model's training-data origin is a property of the model, not the inference call.** If the user chooses to plug in a cloud backend (OpenAI, Anthropic, etc.), the cloud provider's model was trained on whatever they trained it on — we can't audit that. The mitigation:

- Local backends (Ollama with models we vetted or trained ourselves) are the default
- Cloud-backend output is tagged in the UI so the user knows the provenance is third-party
- Cloud-backend output is never automatically committed to a project; it's quoted in the suggestion dialog with attribution

## What we will not ship

- **Auto-tune driven by AI** — `docs/12`'s position holds: auto-tune is statistics. AI doesn't decide cal values.
- **AI-driven flash decisions** — no AI call sits between "user clicks Flash" and "bytes go to the ECU."
- **"AI tunes" or "machine-learned tunes" as marketing-named features** — every commercial tool that pitches this is marketing. We don't.
- **Telemetry / phone-home for AI usage** — even crash reporting stays opt-in and content-free.
- **Cloud-only features** — every AI feature has a local fallback (Ollama, or a deterministic non-AI path). Network failure must never block tuning work.
- **LLM-authored definition packs** for the `definitions/` shipped set — clean-room provenance for shipped data requires a deterministic trail. AI can draft, humans review, humans commit. The shipped pack's provenance is the human review.
- **AI as a substitute for the engine-safety linter or jurisdiction policy** — those are deterministic for a reason. Adding "but the AI said it's fine" as an override channel is a non-starter.
- **Models trained on protected references** (see clean-room above).

## Roadmap placement

- **v1.0–v1.5** — nothing. Foundational tuning + flash + log analyzers + live tuning + hardware support land first. No AI surface.
- **v2.0** — Tier 1 (drift classifier, rules-based) + Tier 2 (LLM explanation over classifier output, optional local-only via Ollama). New `st::ai` module with the Backend abstraction. CLI: `subuwutuner-cli diagnose-drift --log <CSV>`. GUI: a "Diagnostics" panel that consumes the existing adaptive-history snapshot.
- **v2.1** — Tier 3 (explain-this-log assistant). Extends Tier 2's LLM substrate to take free-form queries. Local-first; cloud backends opt-in. **Goal-conditioned tuning coach** lands the same release — it composes Tiers 1+2+3 plus the goal/playbook schemas above. Coach is the user-facing headline feature for v2.1; the bare Tier-3 free-form assistant is a power-user fallback for the same substrate.
- **v2.2+** — Tier 4 (custom-feature-from-description, drafts `.stmod` graphs). Requires the v1.0 custom-features designer (`docs/16`) to be hardware-validated. Coach gains "add launch control at 4000 RPM" style steps via Tier 4 in this release.
- **v2.x** — Tier 5 (def-pack-acceleration ML model), Tier 6 (CAN signal naming), Tier 7 (trained drift classifier) — each gated on training data + holdout-set wins.
- **never** — auto-tune as ML, auto-flash from AI, etc.

The v2.0 → v2.x staging is deliberate: build the *advisory* surface first, prove it's useful and safe, only then introduce *generative* (Tier 4) and *learned-model* (Tier 7+) layers. Each tier has to clear a real bar before the next one lands.

## Failure modes we accept

- **The LLM hallucinates.** Mitigation: the LLM doesn't classify; it explains. If the explanation drifts past the classifier's evidence, the user has a "show raw evidence" toggle that bypasses the prose entirely.
- **The classifier is wrong.** Mitigation: output is `Confidence::{Likely, Possible, Ambiguous}` — the UI shows the confidence, the user does the diagnostic check. False positives are cheap (a wasted check); false negatives are the same as today (the user doesn't notice the problem).
- **The cloud backend is down.** Mitigation: local backend is the default. Cloud features degrade to "AI explanation unavailable; here's the raw classifier output."
- **The user's machine can't run Ollama.** Mitigation: classifier (Tier 1) needs no model — it's deterministic. Tier 2 explanation is the optional add. Worst case: no narrative, still get the diagnosis.

## References

- `docs/04-roadmap.md` — phase gates this depends on
- `docs/05-improvements.md` §11 — under-served-coverage thesis; the snapshot surfaces this consumes
- `docs/06-legal-ethics.md` — jurisdiction policy + safety posture (unchanged by AI integration)
- `docs/12-auto-tuning.md` — what auto-tune IS (statistics) vs is NOT (ML)
- `docs/15-clean-room-engineering.md` — wall + AI-tool contamination channels (training data is one)
- `docs/16-custom-features.md` — IR / node taxonomy that Tier 4 would generate against
- `docs/19-live-tuning.md` — what AI does NOT touch (the write path)
- `CLAUDE.md` — the red flags that apply to AI training data the same way they apply to direct decompilation
