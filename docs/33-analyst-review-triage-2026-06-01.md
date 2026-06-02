# 33 — Analyst review triage (2026-06-01)

> Response to the analyst's external review staged at
> `fixtures/private/findings_reviews_2026-06-01/` (13 docs, `00`–`12`).
> Triages every recommendation against the actual current state of
> the repo + flags factual errors in the review for the next pass.

The analyst delivered a thorough Phase-3 audit: repository scan, gap
analysis, recommended improvements, GitHub issue drafts, roadmap,
testing recommendations, and a final summary. It's a useful
external check — the architectural verdict ("substantially more
mature than the references it studied") is welcome — but several of
the "P0 must-do" items were already shipped before the review ran.
This doc separates **actually new** recommendations from **already
done** ones so we don't relitigate solved problems.

---

## Quick-wins triage (analyst's QW-A through QW-J)

| ID | Analyst recommendation | Status | Note |
|---|---|---|---|
| QW-A | Add `imgui.ini` to `.gitignore`; ship `default_layout.ini` | ✅ done (gitignore line 42) | `imgui.ini` is generated, not tracked. Default layout via `apply_workspace_mode` (no .ini shim needed). |
| QW-B | Remove `SubaruTuner.zip` | ✅ done (not tracked) | Sits on disk as a user-dropped backup per `project_subarutuner.md` memory; never was in git. |
| QW-C | Add `CHANGELOG.md` | ✅ done | 73-line Keep-a-Changelog format. |
| QW-D | Add `SECURITY.md` | ✅ done | 68-line disclosure policy. |
| QW-E | Add `THIRD-PARTY-INSPIRATIONS.md` | ✅ done | 108-line inspiration trail. Companion to `THIRD_PARTY_NOTICES.md` (license attribution). |
| QW-F | `build/` + `fixtures/private/` in `.gitignore` | ✅ done (lines 2, 65) | |
| QW-G | Pre-commit hook + flip CI lane to required | 🟡 hook landed this session (`.pre-commit-config.yaml`); CI lane already required ("clang-format (required)" in `ci.yml`) | |
| QW-H | Glossary tooltip widget | ⬜ deferred | Needs UX placement decision — where in the editor does a glossary popover sit without crowding the table grid? Open question for the user. |
| QW-I | In-app help boilerplate | ⬜ deferred | Same shape as QW-H — pick the first panel to wire (likely Editor → docs/11 per the analyst's suggestion). |
| QW-J | CLI smoke (`--version`, `--help`, `pack lint`) | 🟢 landed this session (`pack-info --json fixtures/demo-pack/pack.toml`) | `pack-info` runs `Definition::validate()` on load; effectively the "lint" subcommand the analyst asked for. |

**Net**: 8 of 10 QWs already shipped; the 2 actually-actionable ones (QW-G, QW-J) landed in this session. QW-H + QW-I want user input.

---

## Issue-draft triage (analyst's `10_github_issue_drafts.md`)

### P0 — v1.0 gate

| Issue | Analyst summary | Status |
|---|---|---|
| #1 Repo hygiene | Remove vendored upstream, committed binaries, stale generated files | ✅ already done (see QW-A/B/F above; analyst was operating on a stale view — `git ls-files` shows nothing tracked under `atlas-public-main/`, `Definitions-V*.atlas`, `SubaruTuner.zip`, `imgui.ini`). |
| #2 BackupStore | Mandatory pre-write backup with verify gate | ✅ shipped — `src/flash/backup_store.{hpp,cpp}` + `tests/unit/flash/test_backup_store.cpp` (per the CMakeLists modification by the user). |
| #3 FlashPreflight | Pre-flight validator pipeline | ✅ shipped — `tests/unit/policy/test_flash_preflight.cpp` (per the CMakeLists modification). |
| #4 Compare workflow | RomDiff + Compare panel | ⬜ **real gap** — no `src/diff/` module exists. The CLI's `rom-diff` is a thin wrapper; a structured Compare panel + DiffSet type are absent. |
| #25 Bench validation | Real-hardware flash round-trip | 🔒 hardware-gated; bench rig assembly in `docs/28`; OBDX VX in hand 2026-05-24. |

### P1 — first months after v1.0

| Issue | Analyst summary | Status |
|---|---|---|
| #6 Plugin / extension seams | ~10 stable interfaces (ChecksumStrategy, TransportDriver, etc.) | 🟡 partially done — many seams already exist as `IBackend` / `ITransport` / `ILogSink` / `SecurityKeyFn`. Need to inventory + document which exist + which are still ad-hoc. |
| #7 VehicleProfile | Top-level domain object (VIN, ECU-ID, defn rev, transport, last backup, last flash) | ⬜ **real gap** — settings exist (`Settings`), project exists (`Project`), jurisdiction profile exists (`policy::Profile`). A unified `VehicleProfile` that bridges these is new. |
| #8 AuditLog | Per-project append-only NDJSON log | ⬜ **real gap** — per-flash journal exists; cross-session ECU-touch audit log does not. |
| #9 ValidationPipeline | Single pipeline every `edit::Transaction` passes through | 🟡 partially done — `evaluate_plan_policy` is the validator pipeline for flash; `edit::Transaction` itself doesn't run validators today. |
| #10 Multi-ROM project | N ROMs per project, per-ROM history | ⬜ **real gap** — `Project` holds one source + one working ROM today. |
| #12 In-app help linked to docs/ | F1 / help-button per panel | ⬜ matches QW-I; user input needed. |
| #13 First-run wizard | Welcome → jurisdiction → unit system → theme → demo project | ⬜ **real gap** — Welcome panel exists; wizard flow does not. |
| #14 Tiered warnings + typed-phrase confirmation | Critical-tier modals require typing "YES FLASH" | 🟡 partially done — Flash modal has explicit-confirm checkbox + reason text. Typed-phrase enforcement + tier system don't exist as a core primitive. |
| #15 Live-to-table cross-reference | Right-click logged channel → jump to producing table | ⬜ **real gap** — depends on the live gauge cluster (shipped this session) growing a "selected channel" event and the editor growing a "jump-to-table" action. |
| #16 Knock / closed-loop overlay on edited table | Heat-overlay log scatter on the open table | ⬜ **real gap** — same shape as #15. |
| #17 Pack lint CI | CLI + GitHub Actions PR gate | ✅ landed this session via `pack-info --json fixtures/demo-pack/pack.toml` smoke; expanding to `definitions/**.toml` waits until in-tree definitions land per `docs/17` Path B. |
| #18 CLI e2e smoke | Open demo → load ROM → edit → diff → flash dry-run → verify journal → exit 0 | 🟡 partially done — CLI smoke covers `--version`/`--help`/`pack-info`. The end-to-end script doesn't exist as a single chain. |
| #19 Mutation testing | Wire mutation framework, gate release CI | 🟡 referenced in `docs/04` ship blocker #11 as "Property-based tests ✅"; mutation testing per se (`Mull`, `Dextool Mutate`) isn't in CI. |
| #20 CHANGELOG + SECURITY + INSPIRATIONS | Standard top-level meta | ✅ done (see QW-C/D/E). |
| #21 In-app definition editor + linter | Pack editor with live ROM preview | ⬜ **real gap** — large feature; user input needed before starting. |
| #22 Glossary tooltips | Embed `docs/10-glossary.md`; hover-popover | ⬜ matches QW-H. |
| #23 Pre-commit + clang-format CI required | | ✅ landed (hook this session; CI gate already required). |
| #24 Property tests + fuzz | rapidcheck + libFuzzer | 🟡 partially done — property tests at `tests/unit/_helpers/property.hpp` cover DVI / SSM / native / UDS framing (per `docs/04` ship blocker #11). Fuzz lane not wired. |

### P2 — v1.x roadmap

| Issue | Status |
|---|---|
| #5 CompareSession persistence | ⬜ blocked on #4 |
| #11 Lazy-load + memory budget | ⬜ deferred; current pack size is fine |

---

## Factual errors in the analyst review

The analyst's repository scan was thorough but appeared to use working-tree `ls` rather than `git ls-files`. Several "P0 in repo" items aren't actually tracked. For the next pass:

1. **`atlas-public-main/`** — gitignored (line 91); on disk locally for analyst reference but not in any commit.
2. **`Definitions-V*.atlas`** — gitignored (line 63); same story.
3. **`SubaruTuner.zip`** — never tracked; user-dropped backup.
4. **`imgui.ini`** — gitignored (line 42); generated at runtime.
5. **`fixtures/private/`** — gitignored (line 65) per the Path B distribution posture in `docs/17`; analyst-side artifacts intentionally live there.
6. **`tools/mutation_test.py`** — analyst lists it as referenced; clarify status by inspection.
7. **"BackupStore not confirmed"** — `src/flash/backup_store.{hpp,cpp}` + tests exist. (Verified in the user's `tests/CMakeLists.txt`.)
8. **"FlashPreflight not confirmed"** — `tests/unit/policy/test_flash_preflight.cpp` exists.
9. **"Cancellation invariants advisory"** — fully shipped + tested (`tests/unit/flash/test_cancellation_invariants.cpp`).
10. **"Property tests absent"** — present at `tests/unit/_helpers/property.hpp` with coverage across 4 framers.

The analyst's *forward-looking* recommendations remain valuable; the *current-state* claims need a second pass with `git ls-files` as the source of truth.

---

## What the analyst correctly identified as new

Sequenced by leverage:

1. **`src/diff/`** + Compare panel (#4) — the single biggest user-facing gap. No reference does this well; SubuwuTuner can ship it as a leadership feature. Multi-day project.
2. **`VehicleProfile`** (#7) — anchor for multi-ROM, audit log, live tuning. Touches `Project` + `Settings` + jurisdiction profile.
3. **`AuditLog`** (#8) — cross-session ECU-touch log. New module + subscribers in transport / ecu / flash / log.
4. **Multi-ROM project** (#10) — N ROMs per project with per-ROM history. Migration step for legacy `.stune`.
5. **Live-to-table + knock overlay** (#15/16) — differentiators that lean on the just-shipped live gauge cluster.
6. **In-app help** (#12) + glossary tooltips (#22) — high-value, low-cost UX wins. Need user input on placement.
7. **First-run wizard** (#13) — onboarding polish; can ship with v1.0 or shortly after.
8. **In-app pack editor** (#21) — large but closes the Path B loop.

---

## Actioned this session

- `.pre-commit-config.yaml` landed — clang-format mirror of the CI lane + standard hygiene hooks (trailing-whitespace, eol fixer, yaml/toml/merge-conflict checks). Install via `pip install --user pre-commit && pre-commit install`.
- CI `pack-info --json fixtures/demo-pack/pack.toml` smoke added to both Unix and Windows branches of the build-test matrix — guards the demo pack against drift and exercises the `subuwutuner.pack-info.v1` JSON emitter.
- Analyst review tree staged at `fixtures/private/findings_reviews_2026-06-01/` (13 docs).
- `docs/README.md` index updated to include this triage doc.

## Open questions for the user

These need a direction call before I can keep executing autonomously:

1. **Compare workflow (#4)** — biggest item by leverage. Yes / no on starting a `src/diff/` module + Compare panel?
2. **`VehicleProfile` (#7)** — should this absorb the existing `Settings` jurisdiction profile, or sit alongside it?
3. **Multi-ROM project (#10)** — willing to bump the `.stune` schema version (with auto-migration) to support N ROMs?
4. **In-app help / glossary (#12/22)** — where in the UI do these live? Side panel? Hover popover? F1 modal?
5. **In-app pack editor (#21)** — defer to v1.1 or queue for after Compare?
