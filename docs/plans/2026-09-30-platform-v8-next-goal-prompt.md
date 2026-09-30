# Goal prompt for the next parent agent (paste as the /goal or first message)

Finish atx-impl v8 (the mega alpha US equity long/short book) from the handoff at
`docs/plans/2026-09-30-platform-v8-handoff-1.md`, following the sprint plan
`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` and the ledger `.superpowers/sdd/platform-v8-20260929/progress.md`.

Working method (owner directive, unchanged): subagent-driven development with parallel Opus 5.5 lanes, one pool
worktree per lane (pools 3, 4, 7, 8, 9, 10, 11; root is `C:/atx-wt/pool-2` on `feat/platform-v8-20260929`); no
test-driven development; no review agent after each task; adversarial reviews only at logical break points (end of a
wave, the optimiser, the mining verb). Children never build or run data; one integrator agent at a time builds, tests
and runs identities in root; the PM only coordinates and rules. You are approved for every recommendation in the plan
and the handoff; make rulings yourself in the form "decision -- why -- cost if wrong" and write them to the ledger
before the measurement they could bias. Focus on rapid development, high quality modular reusable code, building out
atx-engine for future work, and atx-impl proving it on the book.

Do, in this order:
1. Read handoff section 3 and `integration-log.md` "integration 3": finish any skipped identity checks; fold
   `ledger-pending*.md` into progress.md if present.
2. Integration 4: merge the eight unmerged lane branches listed in handoff section 2 (order R1, R45, G, R6, F3, REPORT,
   H3, H1), fix the F-1 reuse-interface defect, build, run every new suite and the identities named in handoff section 6
   step 2. Then one read-only adversarial review of the whole branch since ef11f462 on the most capable model; fix I and
   M findings with one fix lane and one scoped re-review.
3. Rule on handoff section 5 items 1-3 (S_prior for spo-v3, NAV in the ADV cap, hold-band decide parity) before the
   affected cells; recommendations are given there.
4. Wave 0 data build on TRAIN 2020-2023 per `w0-2-runbook.md` (root only, bounded runner, clean tree, caps as ruled),
   overlap reports under ruling W0-a, pins into `v8-prereg.md`, the chained `protocol` ledger line, then cells B0a, B0b,
   B0c with year tables and the Appendix A block. Diagnostics on B0c. Release A/B.
5. Research cells in registered order with identity first and one cell each: R-1, R-2 (7 READY members plus the 8
   `_f49` re-screens through `add-alpha` and `run --screen`), R-3, R-4, R-5, R-6 (after a lane writes the spo-v3 rule,
   part 2 of task-R-6-report.md), R-7 (after lane F3 finishes F-B..F-D). A rejected cell is never retried with other
   parameters. Hard budget N <= 51 construction cells, admission trials <= 15 plus the 8 re-screens.
6. V8-F cumulative test and freeze gate. Wave 5 tooling on the fixture only (H-3 parts 2-3, H-1 per its design, H-2
   measurement). Wave 6: scorecard v8, pitch config and render (0 unavailable blocks), handoff 2, and the merge command
   for the owner. Commit or push nothing outside pool worktrees; never touch `C:/atx` or `atx-db/`.

Hidden data rule: nothing dated 2024-01-01 or later is opened by any tool or agent; record any slip as a disclosure.
Stop only for an irreversible action, a security-sensitive action, a merge to main, or a plan defect that leaves every
path a guess. Otherwise rule, log, and continue.
