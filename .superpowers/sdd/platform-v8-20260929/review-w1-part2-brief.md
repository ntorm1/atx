# Wave 1 adversarial review, part 2 (completing the review the owner stop cut short)

Read-only. You are one of three readers. You read the root worktree `C:/atx-wt/pool-2` at commit `81abfa12`
(code identical to `237486fe`). You never edit source, never build, never run real data, never dispatch subagents,
never open anything dated 2024-01-01 or later. You write exactly one file: your review file (named below) in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/`. Do not commit.

Context: the atx platform v8 sprint (plan `docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md`; rulings E-1..E-34,
W0-a..m in `.superpowers/sdd/platform-v8-20260929/progress.md`; global constraints in `global-constraints.md`).
Part 1 of this review is in `review-w1-A.md`, `review-w1-B.md`, `review-w1-C.md`: read the one nearest your area
for its method and its finding format, and do not repeat its findings (cite them by id if you see the same defect
elsewhere). Fixes for part 1's I and M findings are in flight in other lanes; do not review those lanes.

Adversarial stance: the question is "which number produced by this code could be wrong, and which pre-registration
rule could this code silently break". Look for: stale cache hits, a flag that is not identity-preserving when off,
hidden-data leaks (anything reading past the seal), N or trial miscounts, a test that asserts nothing or pins a
wrong value, silent fall-backs, integer / float traps, and wrong statistics.

Finding format (same as part 1): id `<AREA>-<n>`, severity I / M / m, `file:line`, the defect, what it blocks
(which cell, report or gate), and "verified" or "unverified" with the reason. End with a table of what you read
(file, lines read, clean / findings) so coverage is auditable. Findings only; no praise.

## Area T (`review-w1-T.md`): every test file of the v8 work

The test files added or modified between `41ac94fd` and `81abfa12`
(`git diff --stat 41ac94fd 81abfa12 -- '*test*'` and `'*tests/*'`), C++ gtest and Python. For each: does the test
pin the behaviour its brief claims (find the brief `task-<ID>-brief.md` and report in the sprint directory), does any
test assert nothing, use a tolerance wide enough to hide a defect, pin a value derived by the code under test, or
skip silently. Note especially: the DSR N test, the composition rule tests (`ew-theme-std-v1`), the hold-band and
ADV-cap tests, the spo-v3 tests, the era pooling tests, the field tests (147), the report-block tests, the
identity tests and digest pins (which pins are placeholders).

## Area N (`review-w1-N.md`): the files part 1 never read

`atx-engine/tools/research_fields_sec.py`, `research_fields_holdings.py`, `research_fields_v8.py`;
`fit_composition_weights.py`; `alpha_report_card.py`; `generate_library.py`; `atx-impl/src/strategy_live*`,
`strategy_holdings*`, `strategy_nav_v7*` (locate them with git ls-files). For the field builders: point-in-time
discipline (every value available at the session it is stamped on), the reuse fingerprint, the manifest rows.
For composition fitting: does the fit read anything past the seal; does `ew-theme-std-v1` in Python match the C++.

## Area P (`review-w1-P.md`): everything merged after `7af37e9d`

`git log --oneline 7af37e9d..81abfa12` and `git diff 7af37e9d 81abfa12` (excluding test files, which area T reads,
and the sprint directory). This is: F-C and F-D fields, the era shards tooling (era_pool, pooled fitter, nav_summ,
ledger, roles loop, era_data_audit), the report blocks (legacy book blocks, header, final check, pitch config,
scorecard template), spo-v3 under E-26 (shaping flags; spo-v1/v2 refusals), and integration fixes. Check each
against its ruling (E-17, E-20, E-26, E-28, E-30) and against the identity claims in its report.
