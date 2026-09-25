# Lane W0-FIXUP: Integration regressions

Host tag: **RAM:light** · Pool: **`C:\atx-wt\pool-2`** · Branch: **`feat/w0-fixup`** (run id
`aes-w0-fixup`) · Base: `feat/w0-integration` @ `16a2ae35` (O1, A0, L0, D0, E0a, E0b, R0 merged).

Orchestrator-created lane. The engine lanes ran only their own engine targets, so cross-target
regressions reached integration. Lane I0a's post-merge sync found four failures in the whole
`atx-impl-tests` executable.

## Scope

- Run every touched test executable whole on the integration head:
  `atx-engine-{alpha,factory,learn,data,eval,combine,risk,book}-tests`, `atx-impl-tests`.
- Fix regressions whose root cause is merged W0 code, outside files owned by in-flight lanes
  (I0a, I0b, B0 — see RULES/briefs). Report the others with root cause and owner.
- Owned: `.gitattributes`; the root-cause site of each fixed regression (minimal); test fixtures
  where the fixture itself is shown to be wrong.

## Acceptance

1. `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` passes from the repo root with
   `core.autocrlf=true` (`*.jsonl` checked out LF); no committed blob changes;
   `CMakePresets.json` keeps its CRLF working copy.
2. `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` passes, with a root cause proven on
   evidence: either the A-09 flat-window guard is over-broad (then fix it minimally, keeping
   A-09's legitimate 0/0 cases and the V1 reproduction), or the NaN is correct and the fixture is
   degenerate (then fix the fixture, never the expectation, with a written justification).
3. `StageEquityIc.*` and `EquityMineCli.*` failures are confirmed and attributed to I0b (not
   touched here).
4. Every other touched executable is green, or each failure is attributed with its root cause.
5. No test weakened; report at `.superpowers/sdd/w0/lane-fixup-report.md`.
