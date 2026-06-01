# 12 — Auto-Tuning

Auto-tuning is one of the features where being a *headless, scriptable, modern-language* tool pays off structurally rather than just nicely. The math involved is statistics, not machine learning; the bottleneck in existing tools has historically been UI ergonomics and data-quality gating, not the algorithm itself.

This document captures the design. Auto-tune is **not** a v1.0 feature: **MAF + knock-pull kernels shipped as v1.1** (CSV reader + smoothing + lint + CLI + GUI modals, end-to-end via `edit::History`); closed-loop trim integration, boost-trim, and idle-target trim remain v1.2 design.

## Scope of "auto-tune"

What we will ship:

| Feature | What it does | Target version |
|---|---|---|
| **MAF auto-tune** | Per-cell correction to the MAF sensor scaling map so commanded AFR matches observed AFR | v1.1 |
| **Knock-based ignition pull** | Reduce ignition advance in cells with persistent negative knock-correction | v1.1 |
| **Closed-loop trim integration** | Roll observed long-term fuel trims back into the open-loop fuel map | v1.2 |
| **Boost auto-trim** | Adjust wastegate duty / target so observed boost tracks commanded boost | v1.2 |
| **Idle target trim** | Adjust idle air control parameters to hit target idle RPM | v1.2 |

What we will not ship:

- "AI-generated tunes from scratch." This is marketing in every commercial product that claims it. Not real, not safe.
- Anything that auto-flashes without explicit user confirmation. Auto-tune *proposes* edits; the user reviews and flashes manually.

## Architectural fit

Auto-tune is a function:

```
f : (LogStream, Definition, Tables, AutoTuneOptions)  →  ProposedTables
```

It has no dependency on the GUI, on hardware, or on an ECU connection. That means:

- It runs from `subuwutuner-cli autotune ...` headless. Drop a log on disk, point at a project, get a proposal back.
- It is unit-testable end-to-end with synthetic logs.
- It can run in CI on contributed log+ROM pairs (private fixtures) to detect regressions.
- It can run server-side or in a batch pipeline (e.g. a dyno operator processing a day's runs).

Each algorithm lives under `st::autotune::<algorithm>` and consumes the existing `st::log::LogStream` and `st::defs::Definition` types.

## MAF auto-tune — the canonical algorithm

Subaru WRX fueling is primarily MAF-based (with MAP/SD blending in some maps for transient response). The relevant table is the **MAF sensor scaling**, a 1D lookup that maps sensor voltage to grams/second of airflow.

### Inputs required from the log

| PID | Why |
|---|---|
| `MAF_voltage` | Independent axis of the table we're tuning |
| `Wideband AFR` | Source of truth for actual mixture |
| `Commanded AFR` (or AFR target) | What the ECU is asking for |
| `RPM`, `Engine load`, `Throttle position` | Data-quality gates |
| `Coolant temp`, `Air temp` | Data-quality gates |
| `Closed-loop status` | Disambiguates closed-loop vs open-loop samples |

A stock O2 sensor (narrowband) is **not** sufficient for open-loop auto-tune. The linter refuses to run MAF auto-tune if the log doesn't include a wideband PID.

### Algorithm

```
for each sample s in log:
    if not data_quality_ok(s):           # see gates below
        continue
    cell = nearest_maf_voltage_cell(s.maf_voltage)
    error[cell].push(s.actual_afr / s.commanded_afr)

for each cell c:
    n = len(error[c])
    if n < min_samples_per_cell:
        proposed[c] = current[c]          # leave alone, insufficient data
        confidence[c] = 0
        continue
    mean_error = trimmed_mean(error[c], trim=0.10)
    delta_pct  = (mean_error - 1.0) * gain
    delta_pct  = clamp(delta_pct, -max_delta_pct, +max_delta_pct)
    proposed[c] = current[c] * (1.0 + delta_pct)
    confidence[c] = quality_score(error[c], n)
```

Then a smoothing pass blends each cell with its neighbors, weighted by confidence — high-confidence cells "pull" low-confidence neighbors toward themselves rather than zero.

### Data-quality gates (the part that actually matters)

A bad gate is more dangerous than a bad algorithm. By default we require:

- Coolant temp ≥ 80 °C (engine fully warm)
- Air temp within calibration range
- RPM rate-of-change below threshold (no big transients)
- Throttle rate-of-change below threshold
- No active knock event in the last 250 ms
- ECU not in any limp/fail mode
- Sample is at least 100 ms after a closed-loop ↔ open-loop transition

These are user-configurable per profile but the defaults are conservative.

### Output

Without `--apply` the CLI prints the per-cell proposal (current → proposed
with retained-sample count and confidence) and exits without touching the
project. Re-run with `--apply` to commit the proposal as a single project
edit through `edit::History`:

```
$ subuwutuner-cli project-autotune-maf mytune.stune \
    --table maf_scaling --log run.csv
Loaded 1,847,221 samples from run.csv
After quality gates: 312,005 samples (16.9% retained)

MAF scaling proposal:
  Cells modified:        47 / 64
  Cells unchanged:       17 (insufficient data, <50 samples)
  Mean delta:            +1.8%
  Max delta:             +6.4% at MAF=2.31 V (n=4,219, σ=0.7%)
  Min delta:             -3.1% at MAF=0.87 V (n=189, σ=2.1%)

Re-run with --apply to commit as a project edit (undoable via
`subuwutuner-cli project-undo mytune.stune`).
```

The GUI shows the same information as a heatmap with confidence shading
inside the MAF Auto-tune modal (`render_maf_autotune_modal`).

## Knock-based ignition pull

Subaru ECUs expose `Feedback Knock Correction` and `Fine Knock Learning` PIDs. These are the ECU's own per-cell timing-pull values driven by the knock sensors. Algorithm:

```
for each cell c:
    if mean(feedback_knock[c]) < -trigger_degrees over n samples:
        proposed_timing[c] = current_timing[c] - pull_step_degrees
```

This is even more conservative than MAF auto-tune — the primary pass only ever subtracts timing, never adds. Per-cell pull step is `--pull-step-degrees` (default `0.75°`); trigger threshold is `--trigger-degrees` (default `1.5°` of mean knock correction). The user can opt into a follow-up pass via `--enable-add-back` that *adds back* timing in cells whose knock correction has stayed clean over a configurable sample count — off by default per the docs/12 spec.

## Engine-safety linting

Every auto-tune output goes through the same dangerous-tune linter as a manual edit:

- AFR proposed outside [10.5, 17.5] for any open-loop cell → block
- Ignition advance proposed beyond a per-cell ceiling derived from stock + a configurable margin → block
- Cell-to-cell discontinuity exceeding a smoothness threshold → block (auto-tune output should not look like noise)
- MAF scaling that implies an airflow curve that's non-monotonic in voltage → block

These checks are **always** on, regardless of jurisdiction profile (see `06-legal-ethics.md`). Auto-tune output that fails linting is held in the draft and shown with the failure reasons; the user can override case-by-case.

## CLI surface

Two flows ship per kernel: a **standalone** form that takes axis + current
values directly (for one-shot analysis without a project), and a
**project-aware** form that reads/writes the table through `edit::History`
inside a `.stune` directory.

```
# MAF — standalone
subuwutuner-cli autotune maf \
    --log <run.csv>                                  \
    (--axis <v,v,…> | --axis-file <path>)             \
    (--current <gs,gs,…> | --current-file <path>)     \
    [--gain 0.5] [--max-delta 8%]                     \
    [--min-samples-per-cell 50] [--require-open-loop] \
    [--no-smooth] [--strict-lint]

# MAF — bound to a project
subuwutuner-cli project-autotune-maf <dir>    \
    --table <id> --log <run.csv>              \
    [--gain N] [--max-delta P]                \
    [--min-samples-per-cell N] [--require-open-loop] \
    [--no-smooth] [--strict-lint] [--apply]

# Knock pull — standalone
subuwutuner-cli autotune knock-pull \
    --log <run.csv>                                \
    (--rpm-axis <…>  | --rpm-axis-file <path>)     \
    (--load-axis <…> | --load-axis-file <path>)    \
    (--current-timing <…> | --current-timing-file <path>) \
    [--trigger-degrees 1.5] [--pull-step-degrees 0.75]    \
    [--min-samples-per-cell 30] [--strict-lint]            \
    [--enable-add-back [--add-back-step-degrees 0.5]
                       [--add-back-min-clean-samples 50]
                       [--add-back-clean-threshold-degrees 0.05]]

# Knock pull — bound to a project
subuwutuner-cli project-autotune-knock-pull <dir>  \
    --table <id> --log <run.csv>                   \
    [...same options as standalone...] [--apply]
```

`--strict-lint` exits 3 on any docs/12 lint violation (monotonicity, step
discontinuity, neighbor-smoothness for knock). Logs are column-headered
CSV; required columns differ per kernel — `subuwutuner-cli autotune maf
--help` documents the exact set.

Same algorithms surfaced in the GUI as the MAF and Knock-Pull modals
(see `render_maf_autotune_modal` / `render_kp_autotune_modal` under
`src/ui/src/modals/`) with the diff summary above and a per-cell
heatmap-style ledger.

## Why this matters

A first-class, open, scriptable, jurisdiction-aware auto-tune is a feature the Subaru community does not currently have a great answer for. Existing options are either dated community builds, GUI-only, paid-and-closed, or simply absent. SubuwuTuner ships this as a first-party feature with engine-safety linting on by default.
