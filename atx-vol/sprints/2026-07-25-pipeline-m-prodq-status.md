# `feat/pipeline-m` production-quality remediation — status at operator pause

Date: 2026-07-25
Branch: `feat/pipeline-m` @ `2b8d9d5` (trunk), `feat/pipeline-m-b` @ `15a4cb7` (lane B)
Work source: `atx-vol/docs/reviews/2026-07-25-pipeline-m-code-review.md` (905 lines, added in `15a4cb7`)
Prior closeout: `atx-vol/sprints/2026-07-25-pipeline-sota-sprint-final.md`

## What this effort is

The pipeline-SOTA sprint closed on 2026-07-25 with a green `atx_vol` gate and a candid final status.
A subsequent production review of the same branch returned **request changes**: 34 findings —
**C-1..C-16** correctness/integrity/availability, **P-1..P-10** performance and scale, **F-1..F-8**
feature gaps and open sprint evidence. No finding was rated Critical; thirteen were rated High.

This document is the status of the work to close them. It was written at an operator-requested pause,
**one commit into a twelve-task plan**. It is therefore a plan-and-position report, not a results
report. Almost nothing here is finished.

## Position

| | |
|---|---|
| Trunk | `feat/pipeline-m` @ `2b8d9d5` in `C:\atx-wt\wt-pipe-m` |
| Lane B | `feat/pipeline-m-b` @ `15a4cb7` in `C:\atx-wt\wt-pipe-e` (branched off the trunk tip) |
| Reserve | `C:\atx-wt\wt-pipe-t` @ `1994487` — untouched |
| Local `main` | **`2858cab`**, never written to, verified after every commit |
| Containment | `git rev-list --count 15a4cb7..main` = **0** — `main` is fully contained in the trunk |

`C:\atx` and `C:\atx\.claude\worktrees\surface-db-prod` belong to a different concurrent session and
are off limits to this one.

### Gate baseline

At `15a4cb7`, `ctest -L atx_vol -j 1 --output-on-failure`, full label, no `-R`:

| Quantity | Result |
|---|---:|
| Enumerated | 2,279 |
| Counted | 2,272 |
| Passed | 2,225 |
| Failed | 0 |
| Skipped | 47 |
| Disabled | 7 |

with the `atx-vol-python` lane **Passed**. Every task must account for its delta against this by name.

## How the work is partitioned

Two lanes, **file-disjoint by construction**, so two implementers can run concurrently without ever
touching the same file. Lane A works on the trunk directly; lane B works on a side branch that merges
back. Concurrency is capped at two because the host has 15.7 GB of RAM and is shared with another
Claude session; builds run at `-j 2`.

**Lane A** — `dispersion_run.cpp`, `backtest.cpp/.hpp`, `tearsheet.cpp/.hpp`, `dispersion_strategy.cpp`,
`dispersion_backtest.cpp`, `strategy.hpp`, `opra_batch.cpp`, `run_archive.cpp`

| task | findings | state |
|---|---|---|
| A1 | C-2 multiplier-dependent vega limit; C-3 gross-vega statistics fed signed net vega | **C-2 landed, C-3 in flight** |
| A2 | C-1 projected VaR prices a stale book; C-15 the route hardcodes side and multiplier | brief written, not dispatched |
| A3 | C-4 impact discards a vol-tick spread; C-6 benchmark stats align by index, not date | brief written, not dispatched |
| A4 | P-1 VaR retains every full archive; P-2 corpus ingest resident before batching; P-9 subset miss loads the full board | not written |
| A5 | C-5 RunArchive carry-forward has no dependency fingerprint | not written |
| A6 | C-14 projected-VaR verification accepts stale or garbage companions | not written |

**Lane B** — `portfolio_pricer.*`, `surface_db_populate.cpp`, `corpus.cpp`, `surface_archive.cpp`,
`detail/archive_util.cpp`, `scenario_grid.cpp`, Python bindings and report

| task | findings | state |
|---|---|---|
| B1 | C-7 Python quote mutation races GIL-released fitting and valuation | **in flight, RED under construction** |
| B2 | C-8 risk buckets accept a mismatched portfolio; P-4 quadratic reduction | brief written, not dispatched |
| B3 | C-12 documented report path cannot read CLI output; C-13 omitted TSV economics become zeroes; C-16 hardcoded delta band | brief written, not dispatched |
| B4 | C-9 populate can deadlock after scheduler launch failure; C-10 incremental rewrite deletes a valid cell | not written |
| B5 | C-11 corpus checkpoint pairs are not a transaction; P-5 resume reads the whole archive; P-10 publication durability | not written |
| B6 | P-3 dense `cells * unique_contracts` scratch | not written |

**Wave 3, after the lanes merge:** P-6, P-7, P-8, F-1..F-8, final status, and a full-repository gate.
Several F-items are de-scope decisions rather than code and are deliberately held until the
correctness work is done, so that what gets de-scoped is decided against a known-good tree.

## What has actually landed

**One commit.**

- `2b8d9d5` — *fix(vol): the gross-vega risk limit was multiplier-dependent [REVIEW C-2]*

Its independent review has not run. No gate has been recorded at the new tip. **Treat it as unverified
until both exist.**

## What is in flight

- **A1** is on C-3, the gross/net vega split in `BacktestResult` and the tearsheet. This one carries
  schema risk: `BacktestResult` is serialized into RunArchive sections and TSVs, so adding or renaming
  a column can move a schema hash. The brief instructs the implementer to check that before committing
  and to choose a shape that moves no golden.
- **B1** is building the pre-fix RED for the chain race. Its brief requires a **stated pre-fix failure
  rate** rather than a test that passes both before and after — a race retired by a test that never
  fails is worse than an open finding, because it leaves the list looking shorter than it is.

## Method

Every task is dispatched to a fresh implementer with a written brief, and every landed task gets an
**independent reviewer** that did not write the code. Shared binding constraints live in one file the
briefs reference rather than repeat.

Two rules in every brief are worth naming here because they are corrections to how the previous round
went wrong:

1. **Verify the finding against the tree before acting on it.** Reviews in this repository have twice
   been wrong in their evidence while right in their finding, and once wrong outright. **A disproved
   finding is a deliverable**, not a failure to deliver.
2. **A fix whose deletion breaks no test is not finished.** The previous round shipped exactly one of
   those — a load-bearing assignment with no gate — and had to go back for it.

Standing constraints, unchanged from the prior sprint: never write to `main` or to `C:\atx`; never
write to `C:\atx-data\**`; `git stash` and `git add -A` are banned; no new worktrees; foreground
target-scoped builds at `-j 2`; the gate is the full `atx_vol` label at `-j 1` with no `-R` filter;
**no golden may move**; and **no timing or throughput claim is citable** because the box is shared.

## Open, and not verified

- **Nothing in this effort has been independently reviewed yet.** The single landed commit is
  unreviewed and ungated at its own tip.
- The review's 34 findings are **claims**, not established defects. One commit's worth of verification
  has happened. The rest stand unexamined by this session.
- The gate remains scoped to the `atx_vol` label by earlier operator direction. Roughly 3,400 of the
  repository's ~5,700 tests are unobserved at this tip.
- **Counters-ON has 12 known failures and no CI lane**, diagnosis settled in the prior sprint (the
  gated counter is blind to the laned kernel: 29 is faithful, 4 is the miss). Untouched here.
- The ~19 MB populated RunArchive e2e fixture is still absent, so five production e2e tests skip. B3's
  brief requires its new gate to run **without** that fixture, precisely so it does not join them.
- No power-loss fault injection, sanitizer or ThreadSanitizer run is available on this Windows
  worktree — which matters most for C-7, C-9 and C-11, three findings whose failure modes are exactly
  what those tools detect.

## Next steps

1. Let A1 and B1 finish and commit; dispatch their independent reviewers.
2. Continue the lane order above, two tasks in flight at a time.
3. Merge lane B into the trunk and verify the merge reconstructs byte-identically — the same
   `git merge-tree --write-tree` check that caught a silently dropped hunk earlier in this sprint.
4. Wave 3: the remaining Mediums, the F-series, a final status, and a full-repository gate.

The merge to `main` is the operator's decision and has not been taken.
</content>
