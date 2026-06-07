# Handoff — 2026-06-04 (night session, part 2)

**`origin/main = 078911c`** — five commits added since the part-1
handoff at `1ce0a4d`. Tree clean except the prior HANDOFF files.
Tests green throughout; 1367 cases / ~159k assertions on the
final run. Both binaries link clean.

```
078911c tools(mutation): skip operator matches inside C++ string literals
1acdacc feat(ui): first-run wizard per-step keyboard focus
a8b6a2c feat(ui): welcome chip → click to re-validate pack
23d7175 feat(ui): accessibility second pass — Settings / Flash / CSV Import / Autotune
92d6d2b test(flash): kill mutation survivor at flash.cpp:502 (integrity-offset bounds)
```

---

## How the session ran

Part 1 left the floor at "stop here, write handoff" → handoff
written → "push it and then proceed". Picked up the follow-on
items the part-1 handoff itself called out as well-scoped:

1. **Kill the surviving mutant** flagged by the new mutation CI
   lane (flash.cpp:502 integrity-check-offset boundary).
2. **Accessibility second pass** against the four modals first
   pass deferred.
3. **Clickable pack-lint chip** on welcome recents (item 3 from
   the part-1 fresh task list).
4. **First-run wizard step focus** (handoff's accessibility
   second-pass candidate).
5. **Broader mutation scan** across the rest of `Flasher::execute`
   to find more survivors. Found 2 false-positive survivors caused
   by harness limitation; **fixed the harness instead** of writing
   throwaway tests against string-literal characters.

No architectural sign-off was needed for any of these; the
multi-day items from the part-1 handoff (`src/diff/` panel rebuild,
VehicleProfile, cross-session AuditLog, AI Tier 2 narration) are
untouched and still want a direction call.

---

## What's on `origin/main` that wasn't there at part 1

### `92d6d2b` — kill flash.cpp:502 mutation survivor

The 2026-06-04 mutation lane surfaced one surviving mutant:
swapping `<=` to `<` on the integrity-check-offset predicate at
src/flash/src/flash.cpp:502. The existing "writes the
integrity-check sector LAST" test used an offset strictly inside
a sector, which left the lower bound unexercised.

Two new boundary tests at `tests/unit/flash/test_flash.cpp`,
both tagged `[boundary]`:

- **`...the sector base address`** — `offset == sector.address`
  exact lower bound. Pins `<=` against the `<` mutant.
- **`...the sector's first byte past end`** — `offset == address +
  length`. Pins `<` against a `<=` mutant on the upper bound.

Re-running the same line range:

  | Before        | After         |
  |---------------|---------------|
  | KILLED   = 2  | KILLED   = 4  |
  | SURVIVED = 1  | SURVIVED = 0  |
  | Score   66.7% | Score    100% |

### `23d7175` — accessibility second pass

Extends the modal initial-focus pattern to the remaining
high-traffic dialogs. Each modal sets a per-modal one-shot
`focus_pending_*` flag right after OpenPopup and consumes it
inside the body just before its first interactive widget.

Per-modal focus targets, chosen to match the dialog's gesture:

  | Modal           | Focus target                                |
  |-----------------|---------------------------------------------|
  | Settings        | Definitions root input (Paths tab)          |
  | CSV Import      | "Apply edits" primary button                |
  | Flash           | Confirm checkbox (Confirm / ConfirmWithReason branches only) |
  | Autotune MAF    | Target table input                          |
  | Autotune Knock  | Target table input                          |

Flash modal deliberately does NOT focus the Verify button —
upstream comment at flash.cpp:267 (now :268) explicitly rules
out Enter on Verify as too footgun-y for a flash workflow.
Silent / Badge / Warn / Block branches clear the focus_pending
flag without focusing.

### `a8b6a2c` — clickable welcome pack-lint chip

Welcome panel recents now show "Pack OK · <date>" or
"Pack: N issues · <date>" chips from disk. Making the chip
clickable closes the validate→edit→re-validate loop without
forcing the user to open the project and click Settings.

Implementation: chip text wrapped in `ImGui::Selectable` sized to
the chip width with transparent Header / faint HeaderHovered so
it doesn't look like a sidebar selection cell. Click captured
into a post-loop `revalidate_idx` (mid-loop project loads would
invalidate iteration); handler opens the project headlessly,
calls `Definition::validate()`, persists the new snapshot, and
refreshes the parallel chip cache slot so the visual update
lands the same frame. Project-open failure is silent — the chip
stays as-is and a separate open attempt would surface the error
visibly anyway.

Tooltip on hover spells out the gesture.

### `1acdacc` — first-run wizard per-step focus

Each `draw_step` now calls `SetKeyboardFocusHere` on its first
interactive widget when `state.first_run_focused_step` disagrees
with `state.first_run_step` — fires on first open AND on every
Back/Next that lands the user on a new step.

Per-step targets:

  | Step | Focus target                                   |
  |------|------------------------------------------------|
  | 0    | Next button (no in-body widget; Enter advances) |
  | 1    | First RadioButton in jurisdiction choices       |
  | 2    | Metric RadioButton                              |
  | 3    | Dark RadioButton                                |
  | 4    | "Open the demo project on Finish" checkbox      |

Focus tracker resets to -1 on wizard close (Skip / Finish /
window-X) so a re-launched wizard re-focuses from step 0.

### `078911c` — mutation harness: skip C++ string literals

Sweep across `Flasher::execute` 400-749 surfaced two false-positive
survivors caused by the harness's regex matching `!=` / `>`
characters inside error-message string literals:

  ```cpp
  // line 483
  return bail(ErrorCode::InvalidArgument,
              "flash: SectorWrite.data.size() (" +
              std::to_string(w.data.size()) +
              ") != sector.length (" +    // ← survivor mutates this !=
              ...

  // line 486
  return bail(ErrorCode::InvalidArgument,
              "flash: SectorWrite.sector.length must be > 0");
  //                                          ↑ survivor mutates this >
  ```

`find_candidates` now feeds each match's column through a
single-line state machine that tracks whether the position falls
inside a double-quoted C++ string (handles backslash escapes).
Matches inside string literals skip at candidate-discovery time.

Doesn't handle raw strings (R"(...)..."), char literals, or
multi-line strings — good enough for the 90% case in src/flash.

Re-running the same 400-499 range:

  | Before                | After                  |
  |-----------------------|------------------------|
  | KILLED   = 2          | KILLED   = 4           |
  | SURVIVED = 2 (false+) | SURVIVED = 0           |
  | Score   = 50.0%       | Score   = 100%         |

---

## Mutation-lane state summary

Across the full `Flasher::execute` body (lines 345-749) scanned in
chunks this session:

  | Range   | Compiling | Killed | Surviving | Notes                  |
  |---------|-----------|--------|-----------|------------------------|
  | 345-400 | 0/5       | 0      | 0         | -Werror=nodiscard cascade |
  | 400-499 | 4/7       | 4      | 0         | post-92d6d2b + 078911c |
  | 500-560 | 4/4       | 4      | 0         | post-92d6d2b           |
  | 561-650 | 3/11      | 2      | 1         | line 632 = equivalent  |
  | 651-749 | 1/6       | 1      | 0         |                        |

**Real mutation score**: 11/11 actionable mutants killed in this
window. The lone "surviving" mutant at line 632 is equivalent —
`remaining < block_payload ? remaining : block_payload` and
`remaining <= block_payload ? remaining : block_payload` produce
identical values at every input (boundary case `remaining ==
block_payload` returns `block_payload == remaining`). No test
can distinguish them.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: 1367 cases / ~159k assertions (was 1365 at part-1 EOD;
  +2 new boundary tests). Catch2 randomization moves the
  assertion count between runs; the case count is the stable
  number.
- Local CI sanity: yaml syntax of `ci.yml` is unchanged; the
  mutation lane's runtime estimate for a 20-mutant cap remains
  ~20 min on the Linux runner.

---

## Notes / gotchas the next session should know

### Welcome chip is silently no-op when project open fails

The clickable chip's revalidate handler calls `st::Project::open`
and skips persisting on failure. There's no toast or status_msg.
Rationale in the commit body: a project that fails to open from
the welcome chip would also fail to open from a regular click on
the same recent, and that click DOES surface a toast. The chip
path stays quiet so the welcome panel doesn't grow a second
error-surfacing surface. **Don't add a toast there** — the
existing open-fail-from-click path is the canonical reporter.

### First-Run wizard focus tracker is a step-equality cache

`state.first_run_focused_step != state.first_run_step` is the
"refresh focus" signal. Updating the tracker happens AFTER the
step body + Next button so the inside-step `SetKeyboardFocusHere`
calls have already fired this frame. `step_at_render` pins to
whatever was drawn even if Back/Next changed `first_run_step`
during the footer. Don't reorder these — the tracker update must
be the final write for the step's "land focus once" semantics
to hold.

### Mutation harness skips quoted strings; doesn't skip raw strings

The single-line state machine in `_is_in_string_literal` handles
`"..."` and `"\"..."` escapes. It does NOT handle:

- Raw strings `R"(...)..."` / `R"foo(...)foo"`
- Char literals `'\''`
- Multi-line strings (every line evaluated independently; a
  `\"` continuation on the next line would be misread as code)

src/flash is currently free of all three forms in the windows
scanned, so the harness works. If the mutation lane expands to
src/feature (codegen has raw strings) or src/transport (some
char-literal byte constants), the heuristic needs strengthening.

### Mutation lane's CI 500-560 range now reports 100%

The CI workflow in `.github/workflows/ci.yml` still targets the
500-560 line range. With `92d6d2b`'s boundary tests landed, that
range will hit 100% mutation score every run. **Don't conclude
the lane is broken** if every run reports zero survivors —
that's the target state for the range. The plan from `0f8f171`'s
commit body: when the lane reports zero surviving mutants in
the targeted window for a quarter, flip continue-on-error off
and make it a ship gate. The quarter-clock starts now.

### Mutation lane survivors aren't always coverage gaps

This session surfaced three classes of "surviving" mutants the
harness can't distinguish from real gaps:

1. **Real coverage gap** (line 502) — fixed by writing a test.
2. **String-literal false positive** (lines 483, 486) — fixed
   by improving the harness.
3. **Equivalent mutant** (line 632) — neither test nor harness
   fix; the mutated expression is semantically identical.

When reviewing a future surviving-mutant report, walk through
the three classes before assuming a test-suite fix is needed.

### Existing carry-over notes still apply

From `HANDOFF-2026-06-04-night.md`:
- Per-ROM history legacy-read fallback stays for 3+ months
- `pack_id`-keyed pack-lint cache drop on pack repoint is by design
- Sidebar hide-check runs before filter-check
- F1 routing depends on every panel calling `track_help_context`
- AI drift classifier is advisory-only, no auto-apply channel ever

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

No change from part 1; #10 (binary-size CI gate) is still
partial. The mutation lane is now closer to "ship gate" status
than at part-1 close but isn't there yet (continue-on-error
remains true).

### Real-world unblockers (unchanged)

1. Bench-rig hardware
2. RH850 codegen bench validation

### Things to ship next (no specific user direction)

Same list as the part-1 handoff — none of these were touched
this session and all want a green light before starting:

- `src/diff/` + Compare panel rebuild (#4)
- `VehicleProfile` (#7)
- Cross-session `AuditLog` (#8)
- AI Tier 2 LLM narration (`docs/20`)
- Multi-ROM cross-ROM diff
- Live-to-table cross-reference + knock overlay (#15/#16)

### Smaller items the next session could pick up

- **Mutation lane expansion** — `Flasher::read_full_rom` (lines
  67-215) and the brick-recovery shim code haven't been scanned
  yet. Same advisory shape, separate line range in
  `.github/workflows/ci.yml`.
- **More CLI `--json` envelopes** — `dump-table` and `pack-info`
  are the obvious holdouts (already JSON-friendly; `pack-info`
  has a `--json` flag, `dump-table` does not).
- **Compare panel small polish** — Compare's chip filter doesn't
  yet glossary-tooltip "ROM" the way the picker fields now do
  (the rest of the panel got coverage in `cf0e15b`; the chip
  filter input is the holdout).
- **Sidebar hidden-set GUI integration** — `cb921ef` shipped the
  data layer. The Settings modal Project tab could surface
  "Hidden categories: N · Reset" so the user discovers the
  affordance without right-clicking the sidebar.

### Watch-list

Same as part 1, plus:

- **AppState focus_pending_* fields** — now 7. If the next
  accessibility round adds 3+ more, consider an enum +
  `focus_pending_modal` single field that handles routing.
- **Mutation harness false-positive surface** — the C++
  string-literal skip is one layer. If `find_candidates` keeps
  growing skip-rules (raw strings, char literals, line
  continuations, macro expansions), consider switching to a
  proper tokenizer (libclang) instead of regex.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Re-run the mutation lane against the now-100% range to confirm
# the 92d6d2b + 078911c kills hold
python tools/mutation_test.py \
    --file src/flash/src/flash.cpp \
    --line-start 400 --line-end 560 \
    --target st_unit_tests --tag-filter "[flash]" \
    --max-mutants 20 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt

# Try today's new GUI surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   First-Run wizard (Help → Welcome wizard) → Tab through steps
#                                              see focus refresh
#   Welcome panel pack-lint chip            → click to re-validate
#   Tools → Settings → Pack                 → Validate again, see
#                                              chip + tooltip
#   File → Open project / New project /
#     Read ROM / autotune-MAF / autotune-Knock → Tab nav lands
#                                                 on first widget
#   Flash modal under Confirm profile       → focus on checkbox

# Check today's CI run for the mutation lane
# https://github.com/BuffJesus/SubuwuTuner/actions
# Expected: "mutation testing (advisory) — st::flash" reports
# 0 surviving on the 500-560 range
```

---

## TL;DR

> Five commits past the part-1 handoff; everything on
> `origin/main = 078911c`. Killed the part-1 mutation survivor at
> flash.cpp:502 with two boundary tests; extended modal initial
> focus to Settings / CSV / Flash / Autotune; landed clickable
> welcome chip → revalidate; wired per-step focus on the
> first-run wizard; **improved the mutation harness to skip
> string literals** so future scans don't false-positive on
> error-message glyphs.
>
> Total mutation work brings the scanned window's score from
> 66.7% to 100% on actionable mutants — one equivalent mutant
> remains at line 632 (`remaining < block_payload ? remaining :
> block_payload` is mathematically equal to its `<=` variant).
>
> CI mutation lane is now closer to ship-gate status; the
> quarter-clock starts on the 500-560 range. Big architectural
> items (`src/diff/` Compare panel, VehicleProfile, cross-session
> AuditLog, AI Tier 2 narration) still untouched and still want
> green light.
>
> Off you go.
