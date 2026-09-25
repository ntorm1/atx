# W0 replay consumer disclosure follow-up

Status: **DONE for this scoped report correction**; the final integrated whole-impl
gate remains root-owned and is not waived.

Code commit: `1bdeed388b8819d539175df65c202fd33b6f2d51` on
`feat/w0-replay-integration-codex-20260925`, worktree `C:/atx-wt/pool-4`.
This commit is separate from the ASan gate and can be imported on its own after the
earlier replay repair.

## Defect and correction

G0's corrected baseline exposed a presentation boundary: nested
`report/summary.json` marked assumed liquidations as ineligible alpha evidence,
but the top-level baseline summary copied the numerical `full` performance only.
The constrained book had no equivalent top-level summary. A consumer could thus
read the prominent metrics without the relevant assumption flags.

Both stages now publish the same ten eligibility/count/PnL fields at the top
level: `usable_for_alpha_evidence`, `performance_evidence_eligibility`,
`terminal_liquidation_count`, `evidenced_delisting_count`, `flagged_delistings`,
`flagged_short_delistings`, `flagged_short_pnl_dollars`,
`assumed_liquidation_count`, `assumed_liquidation_pnl_dollars`, and
`gap_carry_count`. Signed stress losses remain signed. The numerical replay
performance object is copied unchanged.

An assumed liquidation sets `qualification=failed` and adds the explicit
`ineligible-assumed-missing-price-liquidation` reason. Top-level manifests and
stage result key/value metadata agree with that qualification. Cases without
assumptions retain the existing `unknown` qualification; they still explicitly
say `usable_for_alpha_evidence=false` and `unverified-research-diagnostic`.
Baseline shape failures retain their independent failure reason.

The book stage verifies the nested summary SHA against the replay manifest before
copying any fields. Its new top-level `summary.json` is included in the book
manifest's hashed files before final publication. Baseline retains its existing
equivalent binding. No replay math, recipe, allocation policy, or real-data input
changed.

## Verification

Implementation preceded the fixture additions. The existing synthetic baseline/
constrained-book fixture now checks both levels' ten fields, exact numerical
performance equality, negative assumed PnL, failed qualification/reason, and the
manifest binding of each top summary. The no-gap fixture also checks zero
assumptions and the unverified evidence state.

Both changed production TUs passed wrapper-driven PCH-off checks, one TU at a time
to prevent Ninja fan-out. Only line wrapping changed afterward. The normal
`equity-dev` cache was then restored to PCH ON with exactly
`FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/equity-dev`. A one-worker synchronized
build of `atx-impl-tests` and `atx-impl` passed (43 steps; free RAM 5.2545 GiB before
the successful build). A preliminary invocation used the nonexistent target name
`atx-impl-worker`; that stopped before compilation, was corrected to `atx-impl`,
and is retained in the setup log.

```powershell
& C:\atx-wt\pool-4\scripts\atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 `
  -R '^(ReplayReport|StageEquityBaseline|BookLegacyReportStage|ImplReplayPolicy)'
```

Result: **32/32 passed**, **0 failed**, **40.32 seconds**, exit **0**.
`git diff --check` passed. No real market data was read. Root/G0 own the separate
real-data rerun; this report does not claim its result in advance.

Logs under `build-equity/`:

- `w0-summary-pch-off-baseline.log`, `w0-summary-pch-off-book.log`.
- `w0-summary-configure.log`, `w0-summary-build-final.log`.
- `w0-summary-focused.log`.

Root independently reviewed the production/test diff without a static blocker.
The previously approved replay repair remains qualified by its own 128/128 book
and 32/32 focused-impl receipts; this report adds consumer disclosure coverage,
not another claim of a whole-engine or full-impl test run.
