<!-- Thanks for the PR! A few quick checks. See CONTRIBUTING.md for full guidance. -->

## What this PR does

<!-- One or two sentences. User-visible change first; implementation second. -->

## Why

<!-- The problem this solves. If it's a bug fix: what was wrong. If it's a feature: who benefits. If it's a refactor: what got better. -->

## How to verify

<!-- Concrete steps a reviewer can run. Build commands, CLI invocations, GUI clicks. -->

## Checklist

- [ ] Tests added / updated for new behavior (`tests/unit/<module>/`)
- [ ] `ctest --preset <my-preset>` green locally
- [ ] `clang-format --dry-run --Werror` clean on changed files (or `git config core.hooksPath .githooks` + commit)
- [ ] If this touches a `docs/` page, the doc was updated
- [ ] If this touches the flash orchestrator (`src/flash/`) or other safety-critical code, the PR description names the bench-rig validation that ran (or explicitly notes that bench-rig validation is gated)
- [ ] No source excerpts from commercial tooling (COBB / EcuTek / HP Tuners / OEM software) included. See `CONTRIBUTING.md` → "The IP rules"
- [ ] No definition packs added to the repo (Path B distribution; see `docs/17`)

## Related

<!-- Link issues, prior PRs, docs pages, analyst handoffs. -->

## Notes

<!-- Anything the reviewer should know that doesn't fit above. Trade-offs you considered. Alternatives you rejected. Things you want feedback on. -->
