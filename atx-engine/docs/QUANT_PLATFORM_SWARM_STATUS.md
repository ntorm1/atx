# Quant-platform swarm — status (stopped 2026-09-23 on owner request)

Goal: grow atx-engine/atx-impl into a state-of-the-art low/medium-frequency equity long/short platform:
high-throughput alpha DSL evaluation and search, advanced combination, optimization, and an alpha-zoo → final
portfolio path. Work ran as a swarm of parallel lanes in leased pool worktrees, each lane built → adversarially
reviewed → fixed. Plan: `SWARM_PLAN.md` (lanes 1-8) and `SWARM_WAVE2.md` (lanes 9-10) at the pool-1 worktree root.

## Where the code is

- Integration branch **`feat/quant-platform-swarm-20260922`** (worktree `C:\atx-wt\pool-1`), head = this commit.
  Contains: Lane 0 scaffold `334a7939` + lanes **1, 2, 3, 4, 5, 7, 8, 9, 10** (all reviewed, review fixes applied).
- **Not merged: lane 6 (optimizer)**, branch `feat/qps-l6-optim` (pool-7). Build done, review returned
  fix-required, fix pass not run (stopped). See "Open items".
- Local **`main` is not updated**. A fast-forward (`git fetch . feat/quant-platform-swarm-20260922:main`) was
  blocked by the permission classifier; it needs owner approval. `C:\atx` checkout (`feat/tier1-parity`, dirty
  atx-db files) was never touched.
- All lane branches `feat/qps-l1-kernels` … `feat/qps-l10-fundzoo` remain; pool-2..pool-11 leases still held
  (release: `scripts\lease-worktree.ps1 -Release pool-N -RunId qps-<lane>`). Each worktree has an untracked
  `SWARM_STATUS.md` with lane detail.

## Verification of the integrated tree (equity-dev, Debug)

| Target | Result |
|---|---|
| atx-engine-alpha-tests | 678/678 |
| atx-engine-parallel-tests | 136/136 |
| atx-engine-factory-tests | 294/294 |
| atx-engine-eval-tests | 195/195 |
| atx-engine-combine-tests | 177/177 |
| atx-engine-book-tests | 90/90 |
| atx-engine-risk-tests | 374/374 (excluding the known >5 min `RiskQpAugment.MatchesDenseOracleAcrossBattery`) |
| atx-engine-data-tests | 199/199 |
| atx-impl-tests | 520/521 — the 1 failure `StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` ("invalid stod argument") is **pre-existing**: it fails identically on C:\atx's existing build |

All merges were conflict-free. Bench numbers below come from lane reports on a heavily shared host (up to 10
concurrent lanes) and are noisy; they were not re-measured on a quiet machine.

## Per-lane outcome

| # | Lane | Delivered | Key measurement | Main gaps |
|---|---|---|---|---|
| 1 | Rolling/CS kernels, streaming | Sliding co-moment (corr/cov/regression), linear-decay recurrence, order-stat window (ts_rank/median/quantile), radix rank, `StreamingEngine` | rank@3000 23-27 ns/cell (3.6x vs stable_sort ref); ts_rank(20) kernel 18 ns; corr ~20, decay_linear ~9 ns/cell | ResearchFast pair ops not yet routed in `vm.hpp` (needs L2 owner); live `VmSignalSource` → `StreamingEngine` swap not done |
| 2 | VM planner / subtree cache / cross-worker CSE | `SubtreeCache`, element-wise fusion, Cs date pool, strategy-B union DAG, WQ101 throughput bench + `scripts/bench-gate.ps1` + baseline | WQ101 (70 alphas, 2520x500) 8 threads: B-fused 1.63x vs strategy A (target 3x); warm cache 159 ms, 100% hits (~5.3x vs old) | 3x cold target unmet (blocked on L1 routing); 70/101 alphas in battery; bench at 500 not 3000 names |
| 3 | Search throughput | Bit-exact semantic rewrite, output fingerprint dedup, multi-fidelity racing, PnL sketch index + farthest-point archive | Distinct trials/sec 3.4x (1 worker), 2.55x (4 workers); front hypervolume unchanged | 4x target unmet at 4+ workers; bench not at 3000x2500 |
| 4 | Multiple-testing control plane | Durable `TrialRegistry` (N_eff, V[SR]), BH/BY/Holm, Romano-Wolf, Hansen SPA/White RC (block bootstrap), single-use audited lockbox, MinTRL, Harvey-Liu haircut, weight stability | N_eff ≈ 1 for identical trials, 48.6-49.8 for 50 independent | Tamper-evident chain head outside log; multi-writer lock |
| 5 | Signal-space zoo combiner | IC-EWMA, Grinold-Kahn, Fama-MacBeth ridge, Kakushadze regression, residualize/Löwdin/marginal IC, LW2020 nonlinear shrinkage, HRP/NCO, walk-forward PIT weights, decay fit, marginal-IC gate flag | Synthetic OOS IR: FMB 0.346 > MV 0.332; GK 0.332; KY 0.327 | New `CombineMethod` wiring into `stage_combine`; mmap SignalStore |
| 6 | Optimizer (NOT merged) | Per-name κ L1 + 3/2-power impact cones + borrow split, deterministic ADMM schedule, discretize pass, GP Riccati, stacked MPC H≤3, production bench | M=1000 cold 514 ms; M=3000/5000 fail 1e-6 feasibility at 300 iters | Review majors: <100 ms warm M=3000 unmet (needs factor-space x-update); `discretize` max_names bug; cost_aware/borrow + optimizer.hpp integration undelivered |
| 7 | Risk model | Fundamental style factors (StyleMask), hybrid fundamental+APCA (Bai-Ng / MP edge), validation scorecard (bias, MinVar bias, Q), attribution | Build 3000x14x60x504: fund 622 ms, hybrid 1.8 s; real 2014 t1000 PIT scorecard produced (optimized-portfolio bias 1.043, 85% in band) | Real fundamental styles need L10 fields; momentum/beta need longer lookback; not wired into `stage_riskmodel` |
| 8 | Zoo → book (engine + atx-impl) | `ReplayCostModel` (flat bps bit-identical, sqrt-impact + participation cap with carried residual), `BorrowSchedule`, delisting/event batches, `PreferenceSource`, factor-bounded allocation, `research_cost_sim`, e2e synthetic test | Replay 3000x2520 sqrt-impact ~4.1-4.6 s | `stage_equity_book` still gated to slow-momentum; dummy 1-factor model in allocation; run_all not on policy replay; no real OOS/capacity run |
| 9 | Real-data DSL mining stage (`atx-impl equity-mine`) | Stage stitching identified yearly contexts, as-of top-1000 membership, WQ101 + literature seeds, SearchDriver with L3 racing/dedup on TRAIN, honest delay-1 net re-score, TrialRegistry, BY + Romano-Wolf gate on validation, holdout once, 2020 seal refusal, return-break guard | See "Real-data results" | Size/liquidity residualization; costs beyond flat 5 bps; turnover-aware books |
| 10 | PIT fundamental fields + zoo | `data/fundamental_fields.hpp` (available-at keyed, lagged, staleness-capped), SEC Company Facts export tool, 60-line literature zoo fixture, IC harness | See "Real-data results" | Only ~57% of top-1000 ids map to a CIK (needs PIT id↔CIK link); no estimates → no eps_revision |

## Real-data results (honest; nothing ≥ 2020-01-01 read)

**Lane 9 mining** (`C:\atx\data\equity_mine_l9_guard_20260923`, supersedes the unguarded run): train 2013-16,
validation 2017-18, reused 2019 development window (three prior reads). 378 seeds → 2,243 candidates → 2,065 scored trials, legacy N_eff 5.71,
de-correlated family of 52. **0 alphas admitted** (BY p = 1). Train winners (net SR 1.0-1.5) were mostly
size/liquidity proxies and collapsed on validation (mean net SR ≈ -0.3). The pre-registered equal-weight family
blend was not admitted on validation (net SR -1.218738, one-sided p 0.950334); its reused 2019
development result (descriptive only) was net SR 1.729687. These are the recorded guard-run
`gate_report.json` values, corrected under I-24 on 2026-09-25. The prior +0.6 validation claim
was incorrect. All 2013-2019 results are development evidence under ruling R-1; none establishes
out-of-sample performance or a tradeable alpha. W0/G0 remeasurement remains pending.

**Lane 10 fundamental zoo** (`C:\atx\data\equity_fund_zoo_ic_l10v2_20260923`, 60 expressions, 120 declared
trials, 2013-2018, 2019 never loaded): pooled h=21 rank IC top-1000 / top-3000 — gross profitability
(qual_gpa) +0.0165 (t 1.67) / +0.0232 (t 1.82); net issuance (inv_iss) +0.018 (t 1.37) / +0.016; PEAD/SUE
+0.013 (t 0.75). None individually significant after trial accounting; coverage limited by CIK mapping.

**Lane 7 risk model** real 2014 t1000 PIT scorecard: `C:\atx\data\l7_riskmodel_scorecard_pit_2014_t1000_20260923`.

## Open items / next wave (priority order)

1. Owner approval to fast-forward local `main` to the integration branch.
2. Lane 6: apply review fixes (discretize max_names, factor-space x-update for <100 ms warm M=3000, cost
   calibration shared with L8 `ReplayCostModel`, `optimizer.hpp` integration), then merge.
3. Cross-lane wiring (plan §9): L1 pair ops routed in `vm.hpp` (then L2 3x cold target); L2 `SubtreeCache` +
   L4 `TrialRegistry` into `SearchDriver`; L5 `cov_targets` into risk `shrinkage.hpp`; L6+L7 into
   `equity_allocation.cpp`; L1 `StreamingEngine` into the live signal source.
4. Real alpha: feed L10 fundamental fields into L9 mining; residualize candidates on size/liquidity; use L5
   walk-forward combiners (not equal weight) as the validation hypothesis; t3000 breadth; longer train window.
5. One end-to-end real OOS run: mined + fundamental zoo → L5 combine → L7 risk → L6 optimize → L8 costed replay,
   reporting net Sharpe, turnover and capacity points at $10m/$100m/$1bn.
6. Alpha191 / BRAIN operator lane (deferred: collides with L1/L2 kernel files, now merged).
7. Housekeeping: the `equity-bench` preset from Lane 0 was never actually written (CRLF mismatch in the scaffold
   script); lanes benchmarked with `equity-rel`. Add it, re-record bench baselines on a quiet host, append
   Release numbers to the ledger. Fix pre-existing `StageRunSyntheticSmoke` failure. Squash/reword WIP commits.
