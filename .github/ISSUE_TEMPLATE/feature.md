---
name: Feature request
about: Propose a new capability or workflow improvement.
title: '[feature] '
labels: enhancement
---

<!-- Before filing: check docs/04-roadmap.md to see if your idea is already on the roadmap, and findings/handoffs/ for in-flight design work. -->

## User-visible benefit

<!-- One sentence: who does this help, doing what? "A user trying to X currently has to Y; this would let them Z." -->

## Workflow today

<!-- What does the user do now (the workaround / the pain point). Be specific — actual commands, actual clicks. -->

## Proposed change

<!-- What you'd like SubuwuTuner to do differently. UI-side, CLI-side, both. -->

## Alternatives considered

<!-- What other approaches would also solve this, and why this one is better. (Or note that you don't care about implementation, just the outcome.) -->

## Scope / trade-offs

- [ ] Touches `src/flash/` (safety-critical; bench-rig validation gate)
- [ ] Requires hardware to test (bench rig / live AP / live ECU)
- [ ] Needs an analyst-mode RE pass for facts that aren't public
- [ ] Cross-platform implications (Windows-only? J2534? libusb?)
- [ ] None of the above — fully orthogonal

## Out of scope

<!-- Things you explicitly don't want this issue to cover. Helps the scope conversation. -->

## References

<!-- docs/ pages this relates to, prior issues, prior PRs, analyst handoffs in findings/. -->
