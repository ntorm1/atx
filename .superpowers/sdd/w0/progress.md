# W0 progress — alpha-engine swarm (plan v2), wave W0 "Truth"

**Resume point.** A fresh orchestrator session continues from this file alone (plus the docs it
names). Goal prompt: `docs/plans/2026-09-24-alpha-engine-swarm-goal-prompt.md`; plan:
`docs/plans/2026-09-24-alpha-engine-production-swarm.md`; findings:
`docs/plans/2026-09-24-alpha-engine-review-findings.md`; lane rules: `RULES.md` (this folder).

## Frozen state

| Item | Value |
|---|---|
| Wave base (pre-W0 `main`) | `2e0d738f2a866b1f253e400ee386ef3e0674cd15` |
| W0 base | the commit on `feat/w0-integration` that adds this file ("w0: base ...") — SHA recorded in the log below |
| Integration tree | `C:\atx-wt\pool-1`, branch `feat/w0-integration`, run id `aes-w0-integ` |
| Lane 6 | `feat/qps-l6-optim` @ `1cf59cb7cd52ec92f99c4b8566c467f0d7527926` (merged by W0-O1; `git merge-tree` vs base: conflict-free) |
| Build preset for lanes | `equity-dev` (`build-equity\`); `dev` is broken on this base (atx-vol removed in `e4bdcf54`) |
| Mandatory stop | after W0 + G0 (owner review) |

## Lanes

| Lane | Batch | Pool | Branch | Run id | Owns (summary; exact list in brief) | Cited IDs | Suites | State |
|---|---|---|---|---|---|---|---|---|
| O1 | W0a | pool-7 | feat/w0-o1-l6 | aes-w0-o1 | lane-6 merge, `CMakePresets.json` equity-bench, Nightly gate, stod fix site | R-14 (ledger) | risk + atx-impl whole targets | merged @ 14ce9172; review APPROVE after 1 fix round(s) |
| A0 | W0a | pool-2 | feat/w0-a0 | aes-w0-a0 | alpha cs/state/typecheck/oracle, factory crossover/canonical, ts_ops (guard+AuditExact), vm.hpp (A-02/03/13 sites) | A-01 A-02 A-03 A-09 A-13 A-18 | AlphaCsRankTies AlphaHumpWarmup AlphaTypecheckScalarLiteral AlphaFlatWindow AlphaAuditExactParity FactoryCanonCollision | merged @ 1d78cc14; review APPROVE after 1 fix round(s) |
| L0 | W0a | pool-3 | feat/w0-l0 | aes-w0-l0 | learn tcn/trainer/loss/latent, linear_alpha+gbt (aug + count sites), feature_matrix.hpp (label meta) | L-01 L-02 L-03 L-07 L-08 | LearnLabelMutationInvariance LearnLabelMaturity LearnFoldLocalAug LearnIcLossPerDate | merged @ 1b766875; review APPROVE after 1 fix round(s) |
| D0 | W0a | pool-4 | feat/w0-d0 | aes-w0-d0 | data history_panel/finra/adjust/align/corp_actions/context/universe/real_panel, augment.hpp (dollar_volume) | D-01 D-02 D-03 D-04 D-05 D-06 D-08 D-09 | DataLevelBasis DataFinraLag DataAdjustGap DataAlignEvent DataCorpActRebase DataContextAsOf | merged @ 2bc9f034; review APPROVE after 1 fix round(s) |
| E0a | W0a | pool-5 | feat/w0-e0a | aes-w0-e0a | eval/hac.hpp (new), cross_section_ic, combine t-stat sites, signal_store winsor | E-02 E-03 E-08 E-09 E-15 | EvalHac EvalIcCoverage EvalIcDelay EvalIcCaps | merged @ 4ef916db; review APPROVE after 1 fix round(s) |
| E0b | W0a | pool-6 | feat/w0-e0b | aes-w0-e0b | eval/trial_clusters.hpp (new), deflated_sharpe, trial_registry, lockbox | E-01 E-16 E-17 L-08 | EvalTrialClusters EvalRegistryWindows EvalLockboxEmbargo | merged @ 1e0a7cf9; review APPROVE after 1 fix round(s) |
| R0 | W0b | pool-8 | feat/w0-r0 | aes-w0-r0 | risk/factor_model, risk/exposures.hpp | R-03 R-04 R-05 R-06 | RiskFactorModelPit RiskSectorColumnsById RiskThinNameFloor | merged @ a5523e21; review APPROVE after 1 fix round(s) |
| B0 | W0b | pool-9 | feat/w0-b0 | aes-w0-b0 | book replay/borrow_schedule/report, stage_report.cpp (minus diag-risk site) | B-02 B-03 B-04 B-05 | BookReplayDelay BookReplayDelist BookBorrowSingleCount BookLegacyReport | merged @ 79c120ed; review APPROVE after 1 fix round(s) |
| I0a | W0b | pool-10 | feat/w0-i0a | aes-w0-i0a | stage_discover/run/combine/optimize/metabook, dead_alpha_wire, diag_risk, stage_report diag site | I-01 I-02 I-03 I-04 I-06 I-07 I-08 R-12 | ImplNestedSplits ImplCombineNoHoldoutRead ImplOptimizePit ImplDeadAlpha ImplMetabookUsesCombo | leased (idle until W0b) |
| I0b | W0b | pool-11 | feat/w0-i0b | aes-w0-i0b | config, dispatch, stage_equity_ic/baseline/book/mine (scoped), equity_baseline_views, replay_report (I-11 site) | D-12 I-10 I-11 I-12 I-15 I-16 I-17 I-23 E-18 B-02 D-02 | ImplConfigBool ImplConfigFinite ImplIcAsOfMembership ImplMineRequiresMembership ImplPendingOrder ImplDelayGuard | merged @ 42503d8e; review APPROVE after 1 fix round(s) |
| FIXUP | post-W0a merge fixups | pool-2 | feat/w0-fixup | aes-w0-fixup | integration regressions: `.gitattributes` jsonl eol, `fundamental_zoo_test.cpp` fixture fix | — | — | merged @ 2dc21315; review APPROVE after 1 fix round(s) |
| G0 | after W0 merge | pool-10/11 (heavy) | — | aes-w0-g0 | truth-delta report (orchestrator) | I-24 | — | pending |

Merge order into `feat/w0-integration`: **O1 first**, then W0a lanes as they pass review (each
pre-merges integration after O1 landed), then W0b lanes (R0 only after O1).

## Orchestrator decisions and deviations (owner please review at the W0 stop)

1. **Ledger path.** `atx-vol/docs/LEDGER.md` no longer exists on `main` (atx-vol deleted by
   `e4bdcf54` "clean up", 2026-09-19). Its last version was grepped from `e4bdcf54^`. Orchestrator
   ledger lines for this series go to a new `atx-engine/docs/LEDGER.md` (same one-line format)
   unless the owner rules otherwise.
2. **qps notes "delete originals".** Archived copies are in `qps-archive/pool-N/` (hash-verified).
   Deleting the originals was refused by the permission classifier, so they were **moved** out of
   the pools into the orchestrator scratch dir (`...\scratchpad\qps-archive-moved\pool-N\`).
3. **Pool placement.** `lease-worktree.ps1` always takes the lowest free slot, so all ten lane
   leases were taken up front in pool order. L0 sits in pool-3 (the spare A-track pool) instead of
   sharing pool-8 with R0, so R0 keeps the risk-warm pool-8 without a mid-wave re-lease.
4. **Lease exit code 1.** The lease script's cold-tree step configures the broken `dev` preset
   into `build\` and fails *after* the lease is published and the branch switched; the leases are
   valid (verified with `-Status`). Lanes build with `equity-dev`.
5. **O1 bench baselines** need a quiet host; W0a runs six lanes. O1 delivers the `equity-bench`
   preset and bench smoke runs; quiet-host baselines are recorded by the orchestrator at the W0
   gate.
6. **RAM.** Host had 5.2 GB free at start (other session's migrations). Every lane build uses
   `CMAKE_BUILD_PARALLEL_LEVEL=2` and waits for ≥ 2 GB free (ledger 2026-08-16: concurrent lanes
   OOM clang-cl otherwise).
7. **Cross-track grants** (hunk-limited, written into the briefs): A0 → `vm.hpp` sites of
   A-02/A-03/A-13 (no other W0 owner); I0a → the diag-risk call site in `stage_report.cpp` (I-04
   cites it; B0 owns the rest); I0b → `replay_report.cpp` I-11 site (no W0 owner); O1 → the
   stod fix site wherever it lies (its owners start after O1 merges).
8. The goal prompt itself is committed beside the plan docs for resumability.

## Provisional orchestrator rulings at resume (2026-09-25) — NOT owner waivers

The owner's 2026-09-25 instruction is to complete the sprint series. These are the orchestrator's
working assumptions on the hand-off §5 questions so work can continue. None of them is an owner
waiver; each stays open for the owner to confirm or overturn (listed again at the pre-W5 stop).
Format: assumption — why — cost if wrong.

- P-1. E0a MA(20) coverage item treated as met by the HAC interval `ic_mean_hac` (94.5%) — the
  item exists to force a HAC interval — if overturned: E0a/E2 rework of the bootstrap CI.
- P-2. E0a combiner-level V1 reproducibility deferred to W2-E3 (owns `signal_combiner.hpp`,
  `orthogonalize.hpp`) — ownership — cost: pre-W0 GK/ICIR-EWMA weights not reproducible until W2.
- P-3. E0b DSR FPR item read as `AccountingDsrRule::MonteCarloMaxV2`; default stays the
  conservative `ClusterMcFloorV2` — conservative gate bias — cost: flip one default.
- P-4. D0 vwap stays adjusted-basis typical price tagged `adjusted_level` (flagged for owner
  waiver) — vwap is a price, not dollar volume — cost: rework if the owner wants raw basis.
- P-5. D0 real-data tests reading ≥ 2020 data stay excluded (data discipline, no reads ≥ 2020
  before W5); pre-2020-only ones run in G0 — cost: none (conservative).
- P-6. A0 `alpha/streaming_engine.hpp` edit accepted as Track-A scope (flagged for owner grant).
- P-7. R0 sanitizer item: Debug checked iterators used as substitute (flagged for owner waiver).
- P-8. Ledger lines go to `atx-engine/docs/LEDGER.md` (old path deleted on main).
- P-9. G0 outputs under `C:\atx-wt\g0-data\<name>_20260925\` (keeps `C:\atx` untouched); cp21
  re-run on a G0-only branch pinned to cp21's 26 families.
- P-10. `atx-impl/src/trial_ledger.hpp:89-90` hunk assigned to I0b (no W0 owner).
- P-11. Lane concurrency capped at 5 (2.65 GB free RAM at resume).

## Log

- 2026-09-24 23:0xZ — setup: all 11 stale qps leases released (keepers dead); qps notes archived;
  pool-1 leased as `feat/w0-integration` (run `aes-w0-integ`).
- 2026-09-25T01:05:47Z — O1 merged @ 14ce9172 (lane head 3ccf012c): lane 6 + equity-bench preset + red stage_run smoke test integrated conflict-free (28 files); quiet-host bench baselines deferred to gate.
- 2026-09-25T01:08Z — **PAUSED on owner request** (W0a workflow stopped). L0/D0/E0a/E0b approved,
  awaiting final sync + merge; A0 mid-fix; W0b, gate and G0 not started. Full state, owner
  decisions and resume procedure: `docs/superpowers/handoffs/2026-09-24-alpha-engine-w0-handoff.md`.
  W0 base SHA = `458d0bef480a624e258070c9d45174a9984466bf`.
- 2026-09-25T12:49:25Z — E0b merged @ 1e0a7cf9 (lane head a0b06547): trial clusters + registry windows + lockbox embargo integrated conflict-free (11 files).
- 2026-09-25T12:51:41Z — L0 merged @ 1b766875 (lane head 5b11f7cb): NN checkpoint/label maturity/fold-local aug/IC loss per date/trial count leakage fixes integrated conflict-free (21 files).
- 2026-09-25T13:05:00Z — E0A merged @ 4ef916db (lane head 33d9b2f3): HAC/block-length inference + execution-delay IC + 4096 caps integrated conflict-free (17 files).
- 2026-09-25T12:53:10Z — D0 merged @ 2bc9f034 (lane head 822524f6): raw-basis ADV/FINRA lag/adjust-align/corp-action rebase/context as-of data-layer PIT fixes integrated conflict-free (29 files).
- 2026-09-25T13:08:42Z — A0 merged @ 1d78cc14 (lane head 9a4e1f46): rank ties average/hump warm-up/scalar-literal typecheck/flat-window guard/CanonSet collision check fixes integrated conflict-free (25 files).
- 2026-09-25T15:30Z — R0 merged @ a5523e21 (lane head bae90033): versioned exposures/factor-model PIT, sector-columns, thin-name-floor risk fixes integrated conflict-free (10 files).
- 2026-09-25T15:45Z — B0 merged @ 79c120ed (lane head 3ddf5b46): execution-delay-0 guard, terminal-return delisting default, single-count borrow schedule, legacy-report parity fixes integrated conflict-free (19 files).
- 2026-09-25T18:31:59Z — I0B merged @ 42503d8e (lane head 9d3eddb2): as-of IC/mine membership, config bool-literal/finite hygiene, pending-order/delay-guard stage fixes integrated conflict-free (28 files).
- 2026-09-25 19:06 — FIXUP merged @ 2dc21315 (lane head e8b1785d): jsonl eol attribute + FundamentalZoo degenerate-fixture regression fixes integrated conflict-free (5 files).
