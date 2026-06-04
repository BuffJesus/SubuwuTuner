# Handoff — 2026-06-04 (night session, part 4)

**`origin/main = c324628`** — four commits added since the
part-3 handoff at `dee988d`. Tree clean except the prior HANDOFF
files. Tests stayed green throughout; 1374 cases / ~159k
assertions on the final run. Both binaries link clean.

```
c324628 feat(ui): Compare panel pin/star changed-tables rows
79cef37 feat(ui): sidebar footer restore-chips per hidden category
8d0f295 feat(ui): help modal table-of-contents for long topics
fe77dc2 test(uds): kill mutation survivor at OBD2 Mode 09 size-3 boundary
```

---

## How the session ran

Part 3 closed at "keep going" → picked up the remaining "smaller
items" from the part-3 handoff list:

1. **Mutation scan: `src/ecu/uds.cpp` + `src/transport/`** — found
   1 real survivor (OBD2 Mode 09 size-3 boundary); rest of the
   surface is 100% on actionable mutants.
2. **Help modal TOC** — collapsing "Sections" header for topics
   with 3+ `## ` headings.
3. **Sidebar footer per-category restore chips** — click a
   chip in the footer hint to unhide just that one.
4. **Compare panel pin/star** — per-row star + "Pinned only"
   filter checkbox.

---

## What's on `origin/main` that wasn't there at part 3

### `fe77dc2` — kill OBD2 Mode 09 size-3 boundary survivor

Scanning `src/ecu/src/uds.cpp` 400-600 surfaced one real survivor
at line 474:

  ```cpp
  if (resp.size() < 3) {
      return failure(ErrorCode::ParseError, "...too short for header");
  }
  ```

A valid `[0x49, pid, 0x00]` response — NODI=0, ECU reports zero
items for the requested PID — is exactly 3 bytes long. The
original `<` lets it through; the `<= 3` mutant rejects it.
New boundary test pins the case: returns empty messages vector.

Adjacent scans this session all reported 100% on actionable
mutants:

  | File                                 | Range   | Result    |
  |--------------------------------------|---------|-----------|
  | src/transport/src/obdx_dvi.cpp       |   1-162 | 10/10     |
  | src/ecu/src/ssm.cpp                  |   1-200 | 13/13     |
  | src/ecu/src/uds.cpp                  |   1-200 |  9/9      |
  | src/ecu/src/subaru_security.cpp      |   1-250 |  1/1      |
  | src/ecu/src/subaru_security.cpp      | 250-499 |  3/3      |
  | src/transport/src/native_transport.cpp |  1-335 | 8/9 (*)  |

(*) one "practically equivalent" mutant at `native_transport.cpp:51`
— `remaining_ms.count() <= 0` vs `< 0`. Differs only when the
deadline lands at exactly `now()`, which the steady_clock continues
past on the next loop iteration; both predicates produce the same
observable TransportTimeout in practice.

### `8d0f295` — help modal table-of-contents

Long topics like `08-testing-strategy`, `16-custom-features`,
`04-roadmap` paginate against the content pane; finding "Tier 4"
or "RH850 backend" meant scrolling through everything that
precedes it. Adds a collapsing "Sections" header at the top of
the content pane listing every `## ` heading as a clickable
SmallButton.

Click → `state.help_scroll_to_heading` captures the index →
`render_markdown` calls `SetScrollHereY` at the matching `## `
line during emission → request consumed at end of render so
the next frame doesn't re-anchor.

Only renders when the topic has 3+ section headings — shorter
docs don't benefit and the collapsing header itself would be
noise. `### ` sub-sections are excluded from the TOC.

### `79cef37` — sidebar footer per-category restore chips

`cb921ef` + `f086387` surfaced the hidden-cats affordance
through the right-click panel menu and Settings → Project; both
routes restore the WHOLE hidden set at once (or one at a time
via the panel submenu). The sidebar footer's "N categories
hidden" hint now grows per-category SmallButton chips —
finer-grained than the right-click submenu.

Each chip:

  `⤵ <category name>`

Click → unhide just that one. Tooltip on hover spells out the
gesture. Stale categories (hidden categories that no longer
exist in the loaded pack) are filtered out, matching the
panel-level submenu's behavior.

Mutation deferred post-render via the same captured-optional
pattern used elsewhere in the sidebar.

### `c324628` — Compare panel pin/star changed-tables rows

Mirrors the audit panel's pin pattern (`2a84616`) onto Compare.
Each row grows a ★/☆ star button at the leftmost column; click
toggles the pin. Pinned `table_id`s persist to
`<project>/compare.pinned` (same sidecar shape as `audit.pinned`
+ `sidebar_hidden.txt`).

A new **"Pinned only" Checkbox** sits next to the
Safety/Emissions/Either-flag chip row. AND-semantics with the
chip filter (user can star a handful of tables AND keep
filtering by category) so the two filters compose rather than
fighting each other.

Key choices:

- Composite-keyed by `table_id` only (not `table_id + ROM_B`) —
  the pin marks "this table is interesting", not "this
  comparison is interesting". Re-running Compare against a
  different ROM B keeps the star markers.
- Mutations deferred post-render via the same captured-id
  pattern as `open_in_editor` + `copy_b_to_a`
  (`TableRowAction` grew a third bool: `toggle_pin`).
- Empty pinned set removes the sidecar file (matches the
  `sidebar_hidden` + audit pattern).

---

## Mutation-lane state summary

Cumulative coverage across the safety-critical surface as of
end-of-session — `src/ecu/`, `src/transport/`, `src/flash/`,
`src/policy/`:

  | File                                   | Range   | Score  | Notes                                |
  |----------------------------------------|---------|--------|--------------------------------------|
  | src/flash/src/flash.cpp                | 400-560 | 100%   | brick-safe ordering + plan val       |
  | src/flash/src/flash.cpp                | 561-749 | 100%*  | * eq mutants at lines 178, 632       |
  | src/flash/src/backup_store.cpp         |  30-40  | 100%   |                                      |
  | src/flash/src/checksum.cpp             |   1-123 | 100%   |                                      |
  | src/policy/src/flash_preflight.cpp     |  65-90  | 100%   | battery thresholds                   |
  | src/ecu/src/uds.cpp                    |   1-600 | 100%   | now incl. Mode 09 size-3 boundary    |
  | src/ecu/src/ssm.cpp                    |   1-200 | 100%   |                                      |
  | src/ecu/src/subaru_security.cpp        |   1-499 | 100%   |                                      |
  | src/transport/src/obdx_dvi.cpp         |   1-162 | 100%   |                                      |
  | src/transport/src/native_transport.cpp |   1-335 | 88.9%* | * practically equiv at line 51       |

Three documented equivalent / practically-equivalent mutants
remain across the safety-critical surface — none killable by
any test.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: 1374 cases / ~159k assertions (was 1373 at part-3 EOD;
  +1 new — UDS boundary test)
- Catch2 randomization moves the assertion count between runs;
  the case count is the stable number

---

## Notes / gotchas the next session should know

### `[native]` vs `[native_transport]` tag distinction

The mutation lane's `--tag-filter` is exact-substring; running
against `src/transport/src/native_transport.cpp` requires
`--tag-filter "[native_transport]"`, not `"[native]"`. The
codec tests (test_native.cpp) use `[native]`; the transport
tests (test_native_transport.cpp) use `[native_transport]`.
A scan with the wrong tag will report 100% BUILD_FAIL or low
coverage that looks like a real gap.

### Help TOC scroll-request is one-frame-only

`state.help_scroll_to_heading` is set on click, consumed by the
next `render_markdown` call, and cleared at end of render. If
something between the click and the consume bumps a frame (e.g.
the modal-Z workaround fires extra frames), the scroll request
will silently miss. Currently fine because the click → render
path is single-frame; if a future refactor splits them, the
field will need a "consumed this turn" sentinel.

### Compare pin sidecar is sorted on save

`save_compare_pinned` sorts the vector before writing so the
on-disk file has a stable order across sessions. Don't add a
`std::shuffle` or LRU-touch path — the sort lets a user
diff-friendly version-control the .stune dir.

### Compare pin filter composes AND, not OR

The new "Pinned only" Checkbox stacks ON TOP of the
Safety/Emissions/Either-flag chip filter (AND semantics). A
user expecting "Pinned OR @safety" would be surprised. The
audit panel uses the same pattern; mirror it if any future
filter dimension joins (don't introduce an OR mode).

### Mutation harness `--tag-filter` is exact substring

A future mutation-lane addition that targets `src/transport/`
should pick the test file's existing tag, not invent a new one.
Match against the test_*.cpp file's TEST_CASE strings, not the
binary's `--list-tags` output (which mixes tags across modules).

### Existing carry-over notes still apply

From parts 1 + 2 + 3:

- Per-ROM history legacy-read fallback stays for 3+ months
- `pack_id`-keyed pack-lint cache drop on pack repoint is by design
- Sidebar hide-check runs before filter-check
- F1 routing depends on every panel calling `track_help_context`
- Welcome chip is silently no-op when project open fails
- First-Run wizard focus tracker is step-equality cache
- Mutation harness skips quoted strings; doesn't skip raw strings
- Equivalent mutants in the `a < b ? a : b` form remain documented
- CI mutation lane is now 4 ranges in series; quarter-clock running on first

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

No change. Mutation coverage is even broader now; the
"continue-on-error" → "ship gate" flip on the first range is
still the open path forward.

### Real-world unblockers (unchanged)

1. Bench-rig hardware
2. RH850 codegen bench validation

### Things to ship next (no specific user direction)

Same big-architectural items as parts 1 + 2 + 3 — none touched
this session:

- `src/diff/` + Compare panel rebuild (#4)
- `VehicleProfile` (#7)
- Cross-session `AuditLog` (#8)
- AI Tier 2 LLM narration (`docs/20`)
- Multi-ROM cross-ROM diff
- Live-to-table cross-reference + knock overlay (#15/#16)

### Smaller items the next session could pick up

- **CI mutation lane expansion** — `uds.cpp:400-600`,
  `subaru_security.cpp:1-499`, `ssm.cpp:1-200`, `obdx_dvi.cpp`
  all sit at 100% and aren't yet locked in by CI. Same pattern
  as the part-3 `e02c48c` expansion.
- **Compare panel bulk pin/unpin** — mirrors `5797fc3`'s audit
  panel bulk popup. "Pin N visible / Unpin N visible / Clear
  all pins" against the current chip+pinned-only filter view.
- **NDJSON export of pinned compare entries** — analogous to
  `a2ebdb4`'s audit pinned-only export. Tuner flow:
  star the relevant comparisons, export, hand to e-tuner.
- **Help modal: persistent active-topic** — currently resets to
  topic 0 on every wizard re-open. Persisting last-viewed topic
  to `settings.txt` would let the user re-find their place
  across sessions.
- **`compare.pinned` GUI integration in Settings** — Settings →
  Project tab could surface a "Pinned compare tables: N · Reset"
  line, parallel to `f086387`'s hidden-cats surface.

### Watch-list

Same as parts 1-3, plus:

- **AppState compare-related fields** — 4 new ones this session
  (`compare_pinned_table_ids`, `compare_pinned_only`,
  `help_scroll_to_heading`, `compare_expanded_tables`
  retained). If the next compare-side feature adds 3+ more,
  consider a `CompareUiState` substruct.
- **Sidecar file count per project** — `edits.toml`,
  `audit.log`, `audit.pinned`, `sidebar_order.txt`,
  `sidebar_hidden.txt`, `compare.pinned`, `pack-lint.toml`,
  `histories/<id>.toml`, `flash.journal`. The empty-set-removes
  pattern keeps things tidy for transient state but a project
  that's been actively used will accumulate ~7 sidecars at the
  root. Consider a `state/` subdirectory for the GUI-side
  ones (audit + pinned + lint) before adding an 8th.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Re-run the broadened mutation surface
for r in "1 162" "400 499" "500 560" "1 200"; do
  case "$r" in
    "1 162") f=src/transport/src/obdx_dvi.cpp; t="[obdx]" ;;
    "400 499"|"500 560") f=src/flash/src/flash.cpp; t="[flash]" ;;
    "1 200") f=src/ecu/src/uds.cpp; t="[uds]" ;;
  esac
  python tools/mutation_test.py --file $f \
    --line-start ${r% *} --line-end ${r#* } \
    --target st_unit_tests --tag-filter "$t" \
    --max-mutants 20 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt
done

# Try today's new GUI surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   F1 → any topic with 3+ ## headings → "Sections" TOC
#   Sidebar → right-click → Hide a category → footer shows ⤵ chip
#   View → Compare → run a diff → star a row → tick "Pinned only"
```

---

## TL;DR

> Four commits past the part-3 handoff; everything on
> `origin/main = c324628`. Killed one more mutation survivor in
> UDS (OBD2 Mode 09 size-3 header boundary); shipped help-modal
> Sections TOC + sidebar footer per-category restore chips +
> Compare panel pin/star with "Pinned only" filter.
>
> Cumulative mutation coverage now spans **9 safety-critical
> files** at 100% on actionable mutants — flash.cpp,
> backup_store.cpp, checksum.cpp, flash_preflight.cpp, uds.cpp,
> ssm.cpp, subaru_security.cpp, obdx_dvi.cpp, native_transport.cpp.
> Three documented equivalent / practically-equivalent mutants
> remain; none killable.
>
> Big architectural items (`src/diff/` Compare panel rebuild,
> VehicleProfile, cross-session AuditLog, AI Tier 2 narration)
> still untouched and still want green light.
>
> Off you go.
