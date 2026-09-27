# atx alpha engine: code review and research findings (2026-09-24)

This is the companion to `2026-09-24-alpha-engine-production-swarm.md` (the sprint plan). Every lane in the plan cites defect IDs from this file. Its lane brief must close those defects or explicitly defer them.

**Sources**
- Read-only review of `main@2e0d738f` plus lane-6 `feat/qps-l6-optim@1cf59cb7`, across 7 domains:
  - alpha/factory/parallel/library
  - eval/combine/learn
  - risk/cost/portfolio/book
  - atx-impl combine/risk/allocation
  - atx-impl equity stages
  - engine data headers
  - atx-db warehouse (queried read-only, dates before 2020 only)
- Web research on alpha discovery/combination/ML and on portfolio construction/costs/capacity/risk. Citations are in §3.

Nothing was built or run during the review.

---

## 0. Headline

1. **The development data is fully consumed.**
   - The 2019 "holdout" has been read at least 4 times (`equity_mine_l9_guard_20260923/gate_report.json`: `holdout.prior_reads: 3` plus the guard run).
   - Checkpoints cp16–cp22 pooled 2013–2019 under at least 570 declared trials.
   - The only clean out-of-sample data left is **≥ 2020-01-01**. From now on, 2013–2019 is development data (walk-forward + CPCV).
2. **Several current numbers contain look-ahead or survivorship bias.** Named:
   - year-union membership
   - survivor-conditioned CIK bridge
   - zero delisting returns
   - adjusted-price dollar_volume/adv
   - FINRA lag
   - full-panel risk/ADV/capacity in atx-impl
   - IC forward return from the signal close
   - NN checkpoint chosen on the test fold
   - label-maturity leak in `learn/latent.cpp`
   - combine "OOS" reusing the discover lockbox
3. **The size-proxy failure has concrete mechanical causes:**
   - rank ties broken by instrument index
   - no neutralization in the mining scorer
   - frictionless delay-0 search fitness
   - fidelity racing that changes the alpha
   - a WQ fitness turnover floor sitting above the target band
4. **The pieces that are sound:**
   - `FactorModel` (factored V, Woodbury)
   - the L7 hybrid estimator
   - the augmented ADMM
   - lane-6 factor-space ADMM, GP-Riccati and 3/2-cones
   - multiple-testing math: BH/BY/Holm/RW/SPA/RC/PSR/DSR formulas and MinTRL
   - LW2003
   - PIT universe builder, `asof_field`, panel manifests, trial ledger
   - VM determinism and trailing-window causality
5. **Where it breaks at scale:**
   - dense signal store: 600 GB at 10k signals
   - library storing positions: 1.2 TB
   - combine at 3×na·D·N: 180 GB
   - IC capped at 4096 names or dates
   - `equity_allocation` fails above 2316 names
   - dense constraint matrix, O(M²)
6. **The production path is weaker than the parts it could use.** It runs:
   - a legacy risk builder with no market, industry or cap factors
   - a heuristic "fast" optimizer that is not an MV optimum
   - flat-bps replay
   - a capacity metric that charges the whole position every period

---

## 1. Defect register

Severity: **B** = blocker, **H** = high/major, **M** = medium, **L** = low. "Lane" is the plan lane that owns the fix.

### 1.1 Alpha / factory / library

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| A-01 | H | `alpha/cs_ops.hpp:250-267`; test pins it `tests/alpha/alpha_cs_test.cpp:256-272` | `rank` breaks ties by instrument index. `rank(sign(..))`, `rank(group_count(..))` and tied fundamentals turn into an index/size proxy. `ts_rank` averages ties, so the two are inconsistent. | W0-A0 |
| A-02 | H | `alpha/state_ops.hpp:79-87`, `vm.hpp:1651-1662` | `hump` starting from NaN stays NaN forever. The oracle has the same bug. Stale values carry indefinitely after universe exit. | W0-A0 |
| A-03 | H | `vm.hpp:1284-1288,1653`; `typecheck.cpp:327-402`; `crossover.hpp:67-80` | The scalar operands of scale/winsorize/quantile/hump read panel cell [0,0]. Crossover can splice panels into those slots. | W0-A0 |
| A-04 | H | `alpha/streams.hpp:29-37,260-271,40-58` | Search fitness is delay-0 (trades at the signal close) and nearly gross, so it rewards bounce reversal. Admission re-scores delay-1 net, so search and admission optimize different objectives. | W2-A4 |
| A-05 | H | `factory/fitness.cpp:200-244` | CPCV "OOS" fitness on a parameter-free formula is in-sample. `deflate_selection=false` by default. | W2-A4 |
| A-06 | H | `factory/fidelity.cpp:94+`, `fidelity.hpp:48` | Date-striding rungs change the alpha: `delta(x,1)` becomes a 4-day delta and returns become 4-day returns. About ⅔ of candidates are killed on a different alpha. | W2-A4 |
| A-07 | H | `library/record.hpp:232-238`, `library.hpp:165` | The library stores dense f64 positions: 120 MB per alpha, 1.2 TB at 10k. The memtable holds about 123 GB. | W1-A5 |
| A-08 | H | `library/corr_index.hpp:139,174,249` | The SimHash gate misses anti-correlated alphas. Recall at \|corr\|=0.7 is about 0.56. T is fixed at construction. | W1-A5 |
| A-09 | H | `ts_ops.hpp:170-182,935-941,1094-1104` | Flat windows give ts_zscore ≈ ±0.97 and noisy corr/slope/skew in AuditExact. This hits every forward-filled fundamental. | W0-A0 |
| A-10 | H | `ts_ops.hpp:968-981` | `ts_decay_exp` is O(d) with a `pow` per element (about 75 s per op at 15M cells), and it is the canonical slow smoother. | W1-A1 |
| A-11 | H | `vm.hpp:1470-1485,1595-1616,1503-1511` | Time-series sweeps stride 24 KB per cell on a date-major layout, about 8× bandwidth amplification. `delay` is not a memmove. | W1-A1 |
| A-12 | H | `vm.hpp:1228-1245`, `panel.hpp:69-75` | The tradable mask is applied at LoadField, so the any-NaN window rule resets history on every membership change. | W4-A6 |
| A-13 | M | `vm.hpp:301-303` vs `ts_ops.hpp:366-387` | AuditExact ts_sum/ts_mean use an online uncompensated sum, so output depends on the panel start and is not oracle-exact. | W0-A0 |
| A-14 | H | `vm.hpp:484-488,1084-1089`; `fitness.cpp:211-227,320`; `panel.hpp:213-215` | Each candidate allocates a 120 MB buffer, the weak-panel pass builds a fresh Engine (0.5 GB), and positions are copied about 15×. That is ~1 GB per worker. | W3-A2 |
| A-15 | L | `global_dag_eval.hpp:76-93` | The union DAG has no memory budget. | W3-A2 |
| A-16 | M | `metrics.hpp:225-230` | The WQ fitness turnover floor of 0.125/day flattens the objective across the 2–5%/day target band. | W2-A4 |
| A-17 | M | `fitness.hpp:228-306` | Turnover and capacity objectives use last-date weights only. Fold slices include a trade-in from flat. | W2-A4 |
| A-18 | L | `factory/canonical.hpp` (`CanonSet`) | A 64-bit FNV collision silently reuses another genome's score. | W0-A0 |
| A-19 | L | `library/lifecycle.hpp:80-90` | The lifecycle is a linear spine: no Decaying→Live and no Admitted→Dead. | W1-A5 |
| A-20 | L | `ts_ops.hpp:947-953` | `ts_quantile` always returns the median (no q). | W2-A3 |
| A-21 | B | `factory/fitness_cost_selection.hpp:79` (turnover=1.0) + `risk/capacity.hpp` | The cost/capacity objective assumes full turnover every period, which penalizes low-turnover alphas. See R-19. | W2-A4 / W4-B3 |

**Operator gaps (W2-A3)**
- No multi-factor residualize, because of the 3-operand cap in `parser.hpp:89-91`.
- Missing ops:
  - Grouping: `bucket`, `group_median/max/min/sum/std/backfill`.
  - Robust scaling: robust z-score (median/MAD), `truncate`.
  - Sparse-data: `days_since_change`, `last_diff_value`, `ts_delta_event`, staleness.
  - Recursive smoothers: recursive `ts_ewm`, `hump_decay`/`jump_decay`, a q parameter on `ts_quantile`.
  - Elementwise: `exp/sqrt/inverse/is_nan/fill_nan/sumac`.
  - Alpha191: `SMA(n,m)`, `REGRESI`, `FILTER`.
- About 35% of the BRAIN operator set is missing.
- The generator samples windows uniformly on [1,60], excludes hparam ops, and has no smoothing productions and no fundamental fields.

### 1.2 Data (engine headers, tools, atx-db)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| D-01 | H | `data/history_panel.cpp:216,247-261`; `alpha/augment.hpp:169`; used at `stage_equity_mine.cpp:1375` | `dollar_volume`/`adv{d}`/`vwap` are built from **adjusted close** × raw volume. The adjustment is a snapshot backward factor that already contains future splits and dividends, so the Alpha101 family has look-ahead. | W0-D0 |
| D-02 | H | `data/finra_short.cpp:242`, `finra_short.hpp:61`; `atx-impl/src/config.hpp:399` (`si_publication_lag=2`) | Short-interest publication is placed at settlement + 10 calendar days, which leaks 1–2 sessions on about 40% of observations. | W0-D0 (+W0-I0b default) |
| D-03 | H (latent) | `data/real_panel.cpp:388-398` | close = TRI while O/H/L/vwap stay raw: mixed price bases in one panel. | W0-D0 |
| D-04 | M | `data/adjust.cpp:88-92` (test locks it in: `data_adjust_test.cpp:393`) | The TRI re-anchors after a gap and drops accumulated dividends. A 3% payer 5 years in shows a phantom −14%. | W0-D0 |
| D-05 | M | `data/align.cpp:97-113` | The as-of join forward-fills event columns (dividend, cum_adj) with no staleness cap, double-counting dividends and freezing splits. | W0-D0 |
| D-06 | M | `data/corporate_actions.cpp:262-291`; `universe.cpp:102` | Shares are not rebased to the split basis. A same-filed-date tie resolves to a row dated after d. | W0-D0 (rebase) / W2-D3 |
| D-07 | M | `data/finra_short.hpp:105`; `finra_short.cpp:295-316` | The ticker→instrument map is static (recycled tickers). `si_util` mixes fractions and days in one column. | W2-D3 |
| D-08 | M | `data/context.cpp:163` | `ATX_ASSERT` is a no-op in release, so a reused DataContext with an earlier as_of returns future-realized candidates. | W0-D0 |
| D-09 | L | `universe.hpp:93-95` vs `universe.cpp:207-208` | A NaN floor excludes names even when "floor 0 disables". | W0-D0 |
| D-10 | B | `tools/export_fundamental_fields.py:512-540`; atx-db `ticker_history.py:356`, `security_master.py:325-366` | The CIK bridge is survivor-conditioned (non-survivors 1.6–11% mapped vs 55–74% for survivors) and uses current tickers. At least 82 IDs are linked through recycled tickers to the wrong company. | W1-D1 |
| D-11 | B | `stage_equity_mine.cpp:489-494,1864`; `stage_equity_ic.cpp:1173,150-153,91` | There are no delisting returns: a missing return contributes 0, and IC uses `DropMissingForward` with 3 hardcoded 2013 deals. The warehouse delisting tables are empty. | W2-D2 (+ consumers E2, B2, I3) |
| D-12 | B | `stage_panel.cpp:318-346,294` → `equity_baseline_views.cpp:302,205-221` → `stage_equity_ic.cpp:546`; mine fallback `:1370-1373` | IC and baseline use the year-union membership mask (a within-year selection look-ahead). Cross-sectional ops see a future-selected cross-section. | W0-I0b |
| D-13 | M | `atx-impl/src/sector_groups.hpp:29-37`; atx-db `reference_classifications.py:1091` | Sector is static (first non-NaN over the full history). There is no PIT classification anywhere, and the vendor GICS is "reserved". | W2-D4 |
| D-14 | M | `history_panel.hpp:115-118`; views `:82,84`; `stage_panel.cpp:274` | market_cap uses vendor shares with an unknown vintage. The PIT alternative is dei `EntityCommonStockSharesOutstanding` (9,876 CIKs). | W2-D3 |
| D-15 | H | atx-db `fundamentals.py:390` vs bars | Facts and bars resolve identity differently: only 3,056 securities share a `security_id`, and 9,462 fact CIKs are unresolved. | W1-D1 |
| D-16 | M | atx-db `sec_company_facts.available_at` | The clock is filing-date 22:00 UTC, not acceptance time. `sec_submissions.acceptance_datetime` matches 94.6% of accessions. | W2-D3 |
| D-17 | M | `data/tickerhistory_training_20120326_20191231_qa2_20260922` is partial | The qa-v2 hole repair never finished. 19 corrupt pre-holiday sessions in 2016–2018. | W1-D5 |
| D-18 | M | `data/point_in_time_universe.hpp:54-56`; `stage_equity_universe.cpp:1261` | No common-stock filter (ETFs/ADRs/preferreds/funds rank in). Minimum ADV is $0 and the price floor is $1. | W1-D5 |
| D-19 | info | atx-db | Standardized/derived fundamentals: 0 rows (the `statement_points` stage ran out of memory). `is_latest_revision` is useless. All 14 `est_*` tables have 0 rows (no estimates vendor). 13F/insider: 0 rows. Corporate actions: 0 rows. Only 12,959 of 20,390 companyfacts CIKs are loaded, and IFRS filers are dropped. | S-level note; the plan avoids depending on these |
| D-20 | info | `C:\atx\data\finra_short_interest` | Exchange-listed short interest exists only from 2017-12. The 2017-12 to 2021-05 files are a re-publication (vintage). | constraint |

### 1.3 Evaluation / combination

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| E-01 | H | `eval/deflated_sharpe.hpp:183-193`; `trial_registry.cpp:398-409` | The registry DSR double-discounts correlation: N_eff = N²/Σρ² is paired with a cross-trial variance V that has already removed the common component. For L9 this understates SR* about 2.7×. A test locks this in. | W0-E0b |
| E-02 | H | `eval/cross_section_ic.hpp:480-483` | Block length max(5, ⌈h/2⌉) on MA(h−1) overlapping returns makes CIs about 35% too narrow at h=21/63. | W0-E0a |
| E-03 | H | `signal_combiner.cpp:223-233,47-63`; orthogonalize `marginal_ic` | IID t-stats on overlapping IC series are inflated by about √h, so the haircut barely bites. | W0-E0a |
| E-04 | H | `signal_combiner.cpp:26-45,257-276` | Listwise deletion across K alphas: windows empty out at K ≥ 100 with mixed coverage. | W2-E3 |
| E-05 | H | `walk_forward_combiner.hpp:93-96` | Every refit recomputes the whole window: about 4.5e14 flops at K=1000. | W2-E3 |
| E-06 | H | `combiner.hpp:277-305` | LW intensity is O(TK²) with a K×K allocation per period. `cov_targets.hpp:103-104` already has the O(TK) identity. | W2-E3 |
| E-07 | H | `combiner.hpp:425-429` | ShrinkageMv uses raw signed μ̂, so noise flips signs at large K. | W2-E3 |
| E-08 | H | `cross_section_ic.hpp:95-96` | `kMaxIcDates/Instruments = 4096`, but the 2013–19 t3000 union is 6,624. | W0-E0a |
| E-09 | H | `cross_section_ic.cpp:330-352`; `stage_equity_ic.cpp:112-113` | The forward return starts at the signal close t while books trade at t+1. There is no knob. | W0-E0a |
| E-10 | H | `orthogonalize.cpp:51-121,194+` | A fresh per-date COD with complete-case rows makes greedy marginal-IC infeasible at P ≥ 500. | W2-E2 |
| E-11 | B | `combine/signal_store.hpp:185-186` | Dense in-RAM alpha-major f64: 60 GB at 1k signals, 600 GB at 10k. | W1-E1 |
| E-12 | L | `pbo.hpp:283`, `pbo.cpp`; `cpcv.cpp:55`; `breadth.cpp:24` | No size check in PBO. PBO gather costs O(C(S,S/2)·N·T). CPCV embargo is in observation units. breadth uses an O(K³) eigendecomposition. | W1-E6 |
| E-13 | L | `combined_source.hpp:42,247` | Per-cell renormalization makes variance ∝ 1/coverage (a small-cap tilt). | W1-E6 |
| E-14 | L | `regime_slice` | Vol-tercile cuts come from a full-sample sort. | W1-E6 |
| E-15 | L | `signal_store.hpp:82-96` | Restandardizing after winsorizing breaks the limit. Pearson IC uses unwinsorized returns. | W0-E0a |
| E-16 | M | `stage_equity_mine.cpp:742`; registry doc | The registry records train PnL, and a fixed `pnl_len` blocks trials over different windows. | W0-E0b |
| E-17 | L | `eval/lockbox.hpp` | The embargo is ⌈0.01·T⌉, not tied to the label horizon. | W0-E0b |
| E-18 | L | `stage_equity_ic.cpp:77` | `kMinNamesPerDate=2`. | W0-I0b |

### 1.4 Learn

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| L-01 | B | `learn/tcn_alpha.cpp:355`; `nn/trainer.cpp:163-170` | The CPCV **test fold** is the validation set, and the checkpoint is chosen on test loss, so OOF IC/DSR/PBO are biased toward passing. | W0-L0 |
| L-02 | B | `learn/latent.cpp:43-60` | `select_interactions` uses labels maturing after t (it checks embargo < H). `FeatureMatrix` carries no label horizon. | W0-L0 |
| L-03 | H | `linear_alpha.cpp:155,197`; `gbt.cpp:478,512` | Full-window augmentation (PCA/interactions) is copied into every fold. | W0-L0 |
| L-04 | H | `feature_matrix.hpp:185`; `linear_alpha fit_standardization` | Labels are raw returns and features are pooled-standardized raw fields, so models learn market timing and levels. | W1-L1 |
| L-05 | H | `feature_matrix.cpp:38,47` | `row_valid` requires every feature to be finite, so rows collapse at F ≈ 500. | W1-L1 |
| L-06 | H | `gbt.hpp:156`, `gbt.cpp:67-113` | usize bins, strided loop, no histogram subtraction, no threads, no missing branch, no early stopping. | W2-L2 |
| L-07 | M | `tcn_alpha.cpp:452` | The deployed checkpoint is chosen on training loss. | W0-L0 |
| L-08 | L | `linear_alpha.cpp:229`, `gbt.cpp:550`, `tcn_alpha.cpp:374`; `IcLoss` | The horizon blend uses pooled Pearson. `trial_count++` runs per fold. `IcLoss` is computed on shuffled mixed-date batches. The autoencoder is not GKX (it is mislabelled). | W0-L0 / W3-L4 |

### 1.5 Risk / portfolio

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| R-01 | B | `stage_riskmodel.cpp:162,261-264`; `stage_optimize.cpp:372,381` | The production factor model has no market factor, no industries (`group_id={}`) and no caps (Size is mapped to Liquidity). Market variance lands in D. | W3-R4 |
| R-02 | B | `risk/optimizer.hpp:330-358,372-388,397-431` | The default "fast" path is P·V⁻¹·Pα, not V⁻¹(α−μ1). λ only switches it on or off, gross is forced to equality, κ acts on a Euclidean surrogate, and demean+rescale moves every name (no no-trade region). | W3-R5 |
| R-03 | H | `factor_model.cpp:579-580,612-613`; `exposures.hpp:281-297,336-365` | Contemporaneous exposures: r_s is regressed on X built at s, whose windows include r_s. | W0-R0 |
| R-04 | H | `factor_model.cpp:627-630` | Sector columns misalign across dates, including an out-of-bounds read of `fit->beta[c]` (UB). Latent today. | W0-R0 |
| R-05 | H | `factor_model.cpp:263-266,688-695,94` | Names with fewer than 2 residual observations get D=1e-12, so the optimizer piles into them. | W0-R0 |
| R-06 | H | `exposures.hpp:477-506`; `factor_model.cpp:579` | Equal-weight z-scores with no winsorizing. The current cap/group is applied to every historical date. | W0-R0 |
| R-07 | H | `eigen_adjust.hpp:87-96` | Simulates T=max(100K,200) instead of the effective T, so v(k)≈1. `amplify` defaults to 1.0 (USE4 uses a=1.4). | W1-R2 |
| R-08 | H | `vol_regime.hpp:149-161`; `factor_model.cpp:468-471` | VRA is in-sample (standardized by today's F), and the factor λ² is applied to D. | W1-R2 |
| R-09 | H | `hybrid_factor_model.hpp:119-137` | The hybrid model has no eigen-adjust, no VRA, and no structural or Bayesian specific risk. Thin assets are silently dropped. | W1-R2 |
| R-10 | H | `factor_model.cpp:578-635`; `stage_optimize.cpp:377-393` | Exposures are rebuilt from scratch twice per build and per rebalance (about 1.5e9 return evaluations at M=3000). | W3-R4 |
| R-11 | H | `constraints.hpp:274,540-551,579-591`; `qp_augment.hpp:299,317-327`; `qp_factor_admm.hpp:170-185`; `discretize.hpp:113-120`; `equity_allocation.cpp:575-577` | Dense R×M constraint matrix, O(M²): about 600 MB at M=5000. | W1-R1 |
| R-12 | H | `stage_optimize.cpp:436-451` | The participation cap uses last-date ADV/price for all history (look-ahead). Names delisted before the end get cap 0. | W0-I0a |
| R-13 | H | `constraints.hpp:127-137,598-609` | ParticipationCap bounds position \|w\| rather than trade \|Δw\|. There is no trade box. | W1-R1 |
| R-14 | H | lane-6 `cost_terms.cpp:291-299`; `qp_factor_admm.hpp:111-114` | Costs never reach the factor-space solver (cones are ineligible and κ is scalar). The 58 ms figure is for a problem with no costs and no turnover. | W2-R3 |
| R-15 | H | lane-6 `optimizer_cost_terms.hpp:136-145` | Untradeable names get κ=c=0 (free to trade), and `max_trade` is computed but never used. | W2-R3 |
| R-16 | H | `multi_period.hpp:95,150,176-178`; `config.hpp:283` | `trade_rate` is a constant, not derived from cost and decay. It double-damps together with κ, and the cost is counted twice (round-trip × one-way). | W3-R5 / W3-R6 |
| R-17 | H | `equity_allocation.cpp:541-559,727-740,647-664,652-660` | An index bound fails above 2316 names. Tolerance is about 2e-13. The model is a dummy (risk is inert). Forced exits end the run. | W1-R1 |
| R-18 | L | `cov_ewma.hpp:180-211`; `specific_risk.hpp:241-258`; `qp_factor_admm.hpp:496-499`; `discretize.hpp:194-215,271-285`; `elasticity.hpp` | NW on top of EWMA Γ0. No specific-risk shrinkage. Gross gate tolerance ∝ M. Discretize liquidates everything outside the top N in one step and breaks neutrality. Elasticity is unwired and its doc disagrees with the code. | W1-R2 / W3-R5 |
| R-19 | B | `risk/capacity.hpp:241-253,186-215,287` | Capacity charges aum·\|w\|/ADV against the per-day edge using the whole position (not Δw) and an in-sample edge. At 3%/day turnover that overstates cost about 190×. | W4-B3 |

### 1.6 Book / cost

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| B-01 | M | `cost/calibration.hpp:239-240` | δ is clamped without refitting the intercept (+58% cost at 1% participation). The calibration is circular (it fits the simulator's own impact). | W1-B1 |
| B-02 | H | `book/replay.cpp:184`; `stage_equity_mine.cpp:1179,1244-1247`; `config.cpp:455-464` | Execution delay 0 is accepted everywhere (same-close fills). | W0-B0 + W0-I0b |
| B-03 | M | `book/report.hpp:172-178`; `stage_report.cpp:441-446,443-446,593-598` | The legacy report gives a delisted held name 0 return, charges borrow once per period regardless of length, and mis-annualizes weekly books. | W0-B0 |
| B-04 | H | `book/replay.hpp:72`; `replay.cpp:228-236,312-318` | A delisting (missing close) or a locate breach **aborts** the run, which pushes users toward a survivor-filtered universe. | W0-B0 |
| B-05 | L | `borrow_schedule.hpp:15-17` vs `replay.cpp:443-444` | The borrow fee is charged and the rebate credited, which double-counts. | W0-B0 |
| B-06 | L | lane-6 `add_borrow_terms` | Takes an annual fraction while replay uses bps, with no adapter. | W1-B1 |
| B-07 | H | whole stack | Costs are flat bps everywhere. `ReplayCostModel` sqrt-impact is used only in tests. There is no metaorder aggregation, spread estimation or borrow model. | W1-B1, W2-B2 |

### 1.7 atx-impl pipeline

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| I-01 | B | `stage_discover.cpp:548-551,610`; `stage_run.cpp:105-106,121` | Discover admits alphas on the last 25%, and combine then reports that same 25% as "OOS". The library reuses the lockbox. | W0-I0a |
| I-02 | B | `stage_combine.cpp:1030` | `apply_conviction` scores DSR and stability over the full stream, holdout included. | W0-I0a |
| I-03 | B | `stage_combine.cpp:327-337,345,289-317,369` | Capacity uses full-period mean PnL, the last book, and last-date ADV. Cost is charged on holdings, not trades, which is biased against low-turnover alphas. | W0-I0a |
| I-04 | B | `stage_optimize.cpp:370-374,217`; `stage_metabook.cpp:537`; `stage_report.cpp:499` | The diagonal risk model uses full-panel variance for every rebalance. | W0-I0a |
| I-05 | B | `stage_combine.cpp:911-950,537-559`; `stage_metabook.cpp:242-292,327-346` | Memory is at least 3×na·D·N doubles (about 180 GB at 3000 alphas). The stack Gram is O(na²TN) and clustering is O(n³). | W3-I6 |
| I-06 | M | `dead_alpha_wire.hpp:78-91`; `stage_optimize.cpp:364-367` | The "dead" set is actually the admitted/live alphas, taken as of the last period (inverted, and look-ahead). | W0-I0a |
| I-07 | M | `stage_metabook.cpp:482-483,521-523,352-366` | Metabook sleeves ignore the fitted combiner (they equal-weight). | W0-I0a |
| I-08 | M | `stage_combine.cpp:1416-1422` | Walk-forward fails for stack/RegimeStack, ignores the combiner config, and has no embargo. | W0-I0a |
| I-09 | M | `stage_equity_ic.cpp:838-850`; `stage_combine.cpp:206-207`; `stage_discover.cpp:745-751` | DSR's N is the pool size or a ledger ordinal. A failed sidecar write silently resets to 0. | W1-I1 |
| I-10 | M | `config.cpp:35-68,686`; `dispatch.cpp:131-139` | Boolean flags ignore their value (`metabook=false` turns it ON). `--config` is silently ignored in most stages. | W0-I0b |
| I-11 | M | `stage_run.cpp:161`; `config.hpp:451-452`; `replay_report.cpp:386-391` | The report defaults to 0 trade bps and 0 borrow, so headlines are frictionless. | W0-I0b |
| I-12 | L | `config.cpp:286-298,397-403` | "nan" is accepted for doubles (`--holdout-frac nan` silently disables the holdout). | W0-I0b |
| I-13 | B | `stage_equity_mine.cpp:465-476` | The mining scorer applies no size/sector/beta/liquidity neutralization. | W4-I3 |
| I-14 | H | `stage_equity_mine.cpp:847-851,857-864` | Search fitness is frictionless on the non-PIT context universe. Only `all_scored` genomes are registered (racing drop-outs are not). | W2-A4 + W4-I3 |
| I-15 | H | `stage_equity_ic.cpp:1173,150-153,91,480-496` | Terminal pricing covers 3 hardcoded 2013 deals, and the stage refuses to run without the 2013 audit. | W0-I0b (interface) / W2-D2 (data) |
| I-16 | M | `stage_equity_mine.cpp:1370-1373` | Without `--membership`, the mask silently falls back to the year-union. | W0-I0b |
| I-17 | L | `stage_equity_baseline.cpp:476-479,591-595`; `stage_equity_book.cpp:573-576` | `.pending` is removed before the manifest is written. | W0-I0b |
| I-18 | M | `equity_baseline_views.hpp` (`kEquityFamilyDsl`); `stage_equity_ic.cpp:100-102` | Families and trial counts are compile-time constants, so each checkpoint means a rebuild. | W1-I1 |
| I-19 | M | `stage_equity_ic.cpp:382-392`, `stage_equity_universe.cpp:338-350`, `stage_equity_baseline.cpp:79-88`, `stage_equity_book.cpp:105-116` | The "no override" guard is copy-pasted 4×. | W1-I1 |
| I-20 | M | `stage_run.cpp:173-181` | No run manifest: no config hash, git SHA or seed list. | W1-I1 |
| I-21 | H | `stage_equity_book.cpp:171-179` | The book accepts only slow momentum. `library.tsv` is orphaned, and no equity-book run has ever completed. | W4-I4 |
| I-22 | M | `atx-engine/reviews/trial-ledger*.jsonl` | Trial accounting is fragmented: at least 10 sidecar ledgers, and the canonical ledger has been untouched since cp14. | W1-I1 |
| I-23 | L | `stage_equity_mine.hpp:136-138`; `stage_equity_mine.cpp:1799-1817` | nw_lags, ic_horizons and ppy are hardcoded and not recorded. | W0-I0b |
| I-24 | L | `atx-engine/docs/QUANT_PLATFORM_SWARM_STATUS.md` | The L9 family blend is reported as "validation net SR ~0.6". The guard run's `gate_report.json` says **−1.219** (p=0.95); holdout 1.73. | G0 |

### 1.8 Research-discipline state

- **R16-8** is from cp16 design §3 (`atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md:78-83,166`):
  - The rule: the pooled h=21 non-overlapping net decile-spread Sharpe has bootstrap `ci_lo > 0` (2.5th percentile, B=2000, block 5) **and** the sign matches in every reportable year, on both the t1000 and t3000 cuts.
  - It is underpowered for combined books: n_obs = 74 gives a CI of about ±0.7 annualized SR. On 2017–18 alone (n≈24), only SR > 1.5 can pass.
- **Cumulative declared N:** 570 at cp21 and 630 registered at cp22, plus L9's 2065 raw (N_eff 5.71, which is miscomputed, see E-01) and L10's 120.
- **Best evidence to date:** nothing has cleared R16-8 across 29 families. The maximum DSR so far is ≤ 0.0006.

---

## 2. Warehouse and data reality (read-only audit, dates before 2020)

| Dataset | State | Usable path |
|---|---|---|
| XBRL facts `sec_company_facts` | 27.99M rows before 2020, 10,314 CIKs, 240 concepts, us-gaap+dei only | Export tool reads `companyfacts.zip` directly. Clock should move to `sec_submissions.acceptance_datetime`. |
| Standardized/derived fundamentals | 0 rows (OOM) | Do not depend on it. Derive in the export tool. |
| `sec_submissions` | 7.94M before 2020, 68k CIKs, acceptance time on 100% of rows | Exact clock. 25-NSE/15-12 delisting evidence. Form 4/13F metadata. |
| Bars `equity_daily_bars` | 14.2M rows before 2020 (SpiderRock) | No split/dividend/VWAP. Vendor shares have unknown vintage. |
| PIT shares | Vendor field is not PIT. dei `EntityCommonStockSharesOutstanding`: 202k rows, 9,876 CIKs | Use dei shares, rebased to the split basis. |
| Security master | Every CIK/ticker row is `valid_from=2026-09-20` (snapshot) | Rebuild dated links from SEC Insider Transactions `SUBMISSION.tsv` (2006Q1+) plus FSDS `sub.txt` (2009Q2+). |
| Sector | Warehouse empty. Vendor GICS is not PIT. | SIC per filing from FSDS `sub.txt`, mapped to FF49/FF12. |
| Estimates | 14 `est_*` tables, all 0 rows | None free. SUE from XBRL seasonal random walk plus 8-K Item 2.02 timing. |
| Delisting / corporate actions | 0 rows | Build from `sec_submissions` 25-NSE/15-12 plus each ID's last bar plus Shumway fallback. |
| Short interest | Exchange-listed from 2017-12 only | 2018–2019 only inside the development window. |
| 13F / insider | 0 rows (metadata only) | Out of scope for this series. |

---

## 3. Research digest (what the literature implies for a high-capacity, low-turnover cross-sectional book)

Figures marked ≈ are approximate or recalled by the research agents rather than re-read from the paper. The JKMP figures are from the 2022 SSRN draft and may differ in the RFS final.

### 3.1 Where the money is
- **Single anomalies are about dead at scale.**
  - Chen–Welch 2026 (arXiv 2607.06502): median 7 bp/month in the top 3000 / top 90% of cap after 2005. Profitability is the only healthy category (mean 25 bp).
  - Best single signals (bp/month): cash-based operating profitability 66, R&D-adjusted operating profitability 60, off-season momentum 54, net external financing 48, long-horizon seasonal momentum 48, gross profitability 44.
  - Chen–Velikov JFQA 2023: the average anomaly nets 4 bp/month after 2005; combinations net about 20 bp.
  - McLean–Pontiff: −26% out-of-sample, −58% post-publication.
  - PEAD is gone in large caps after 2006 (Martineau 2022).
- **Combination and cost-aware construction are the edge.**
  - JKMP "ML and the Implementable Efficient Frontier" (RFS 2026; code at github.com/theisij/ml-and-the-implementable-efficient-frontier): Portfolio-ML net SR **1.38** at $10bn, 32%/month turnover. Multiperiod-ML 1.16. Static two-stage 0.94. Markowitz-ML gross 2.0 but hugely negative net.
  - After costs, value and quality dominate feature importance.
  - DeMiguel et al. RFS 2020: with costs, significant characteristics rise from 6 to 15, because trades net across signals.
- **Fundamental law:** IR ≈ TC·IC·√breadth. With IC 0.02, N=3000 and monthly rebalancing, the ceiling is about 3.8·TC. A constrained low-turnover book has TC ≈ 0.3–0.5, so a realistic IR is about 1.

### 3.2 Alpha discovery
- **AlphaGen (KDD 2023):** the reward is the IC gain of the combination after adding the candidate, computed from cached alpha–alpha correlations and alpha–target ICs (Thm 3.1, O(K²)). The pool is capped (K≤100–200) and pruned by smallest |w|.
- **Other regularizers and variants:**
  - AlphaAgent (KDD 2025): originality via AST similarity to the zoo, a complexity cap, and hypothesis alignment.
  - QuantFactor REINFORCE: penalizes low-IR alphas with a ramping threshold.
  - AlphaForge: dynamic re-selection. It adds turnover, so avoid it for this book.
- **Evidence caveats:** most of this evidence is China A-shares, 5–20 day horizons, no costs. LLM miners have look-ahead contamination (validate only after the model's cutoff). Chen–Dim: mined predictability sits in accounting signals and small stocks.
- **Neutralize before scoring.** Daniel–Mota–Rottke–Santos (RFS 2020): hedging unpriced risk raises the squared Sharpe of FF5 from 1.17 to 2.13. Residual momentum (Blitz–Huij–Martens JEF 2011) earns about 2× the risk-adjusted return of raw momentum; the recipe is 36-month FF3 residuals, cumulated t−12..t−2, divided by residual vol.
- **Characteristic library:** the JKP 153 characteristics in 13 themes (Jensen–Kelly–Pedersen JF 2023; code at github.com/bkelly-lab/ReplicationCrisis).
  - Theme-level (hierarchical Bayes) evaluation; do not gate individual anomalies.
  - Standard preprocessing: rank each characteristic cross-sectionally to [−0.5, 0.5] and set missing to 0; keep characteristics with under 30% missing; drop nano caps.

### 3.3 ML for the cross-section
- **GKX (RFS 2020):** NN3 and GBT ensembles, annual refit. Dominated by fast, illiquid signals (Avramov–Cheng–Metzker: −52–77% once microcaps and distressed names are excluded).
- **RFF ridge:** features [sin(s·w), cos(s·w)] with w~N(0,σ²I), closed-form ridge, seed and σ ensembles (Kelly–Malamud–Zhou JF 2024; used in JKMP). Nagel 2025 critiques timing, not the cross-section.
- **AIPM (Kelly–Kuznetsov–Malamud–Xu 2025, transformer SDF):** gross OOS SR 3.6–4.6. The relative edge is largest in large caps.
- **Targets:** residual returns at multiple horizons (21/63/126d), a model per horizon, rolling 5–10 year windows. The development window here is only 7 years, so use expanding windows with a ≥3-year burn-in.

### 3.4 Combining many weak signals
- **Hierarchical design:**
  1. Within a theme: equal weights or EB-shrunk IC weights on neutralized signals.
  2. Across themes: KNS ridge b = (Σ̂ + γI)⁻¹μ̂ (Kozak–Nagel–Santosh JFE 2020; γ by CV), or Kakushadze–Yu residual weights.
  3. Across horizons: GP decay weights.
- **EB shrinkage:** α̃ = (1 − 1/Var(t))·α̂, hierarchical by theme. Chen–Welch estimate a shrink of about 0.08 after 2005. Chen–Dim find EB beats finance's usual multiple-testing rules at picking winners.
- **Trial counting:**
  - Count every evaluation, including racing drop-outs.
  - N = number of trial clusters (ONC); V = variance of cluster representatives.
  - Walk the miner forward (mine to T, validate after T).
  - Harvey–Liu–Zhu: t > 3. Harvey–Liu 2020: double-bootstrap FDR.

### 3.5 Low-turnover construction
- **Gârleanu–Pedersen (JF 2013):**
  - Trade rule: x_t = x_{t−1} + (a/λ)(aim_t − x_{t−1}).
  - aim_t = (γΣ)⁻¹ Σ_k B_k f_k / (1 + φ_k·a/γ), so each signal is shrunk by its own decay φ_k.
  - a = [−(γ(1−ρ)+λρ) + √((γ(1−ρ)+λρ)² + 4γλ(1−ρ)²)] / (2(1−ρ)).
  - Half-life fit: IC(h) = IC₀·e^{−h·ln2/τ}, with φ = ln2/τ.
- **JKMP Portfolio-ML:**
  - Policy: π_t = m·g_t·π_{t−1} + (I − m)·A_t.
  - The aim is learned directly: A_t = RF(s_t)′β, with β = (E[Ω̃] + λI)⁻¹E[r̃].
  - Costs: Λ_i = 0.2/ADV_i, i.e. 0.1% impact at 1% of ADV.
- **Stacked MPC:** H≤3 cannot see months-long decay. Add the Riccati cost-to-go as a terminal cost.
- **Cost mitigation:**
  - Buy/hold banding is the best single mitigation. Anomalies under 50%/month turnover survive (Novy-Marx–Velikov RFS 2016, FAJ 2019).
  - An L1 cost produces a no-trade region automatically.
  - Qian–Sorensen–Hua: include lagged signal values.
- **Rebalance timing luck** (Hoffstein et al.): over 100 bp/yr for concentrated calendar books; N tranches cut it about 1/N.

### 3.6 Costs, capacity, shorting
- **FIM "Trading Costs" (2018, AQR live data, $1.7tn):**
  - Model: MI(bp) = a + b·m + c·√m, with m = % of ADV. US: b=−0.53, c=11.21. Controls: −0.14·ln ME ($bn), +0.31·idio vol, +0.12·VIX.
  - Model output: about 13.7 bp at 2% of ADV and 32 bp at 10%.
  - Realized: mean 10 bp (median 6.2); large caps 8.9, small caps 19. About 85% of impact is permanent.
  - TAQ-calibrated models overstate cost 2–7×.
- **Square-root law:** ΔP/P = Y·σ_d·√(Q/V), with Y ≈ 0.55–0.75 for patient institutional flow. Engine prior: c_i(z) = (s_i/2 + 1bp)|z| + Y·σ_i·|z|^{3/2}/√(ADV_i/NAV).
- **Impact decay** (Bucci et al. 2019): about ⅔ of peak at the close, plateau at about ½ after roughly 50 days. Consecutive same-direction slices are one metaorder; charging each day independently understates cost by up to √n.
- **Spread from daily OHLC:** Corwin–Schultz, Abdi–Ranaldo (RFS 2017), EDGE (Ardia–Guidotti–Kroencke JFE 2024; github.com/eguidotti/bidask). The equal-weight composite has about 90% correlation with TAQ but is biased upward after 2003 (Chen–Velikov).
- **Volume forecasting** is worth as much as return forecasting in a cost-aware book (Goyenko et al. NBER w33037).
- **Capacity:**
  - Re-optimize at every AUM; never scale a fixed book.
  - Report A_be, A* and A_30%. For a fixed book with 3/2-power cost, A_be = (α/k)² and A* = (4/9)·A_be.
  - FIM break-evens: SMB $275bn, HML $214bn, UMD $56bn; cost-optimized 3–6× larger.
  - Landier–Simon–Thesmar: capacity ∝ 1/(λφ²), so persistent quality has about 10× the capacity (≈; the PDF layout was garbled).
  - Patton–Weller: paper capacity is an upper bound.
- **Shorting:**
  - D'Avolio 2002: 91% of names are general collateral at about 17 bp; 9% specials at about 4.3%; 2%/month recall.
  - Muravyev–Pearson–Pollet JF 2025: the average anomaly goes from 0.14% to −0.01%/month after borrow fees.
  - Model tiers: GC 25–30 bp, warm 1–5%, special 5–50%. Predictors: size, price < $5, SI/float, IPO age.
  - Borrow is a holding cost, not a trade cost. Recall is a hazard.

### 3.7 Risk models (USE4 reference; Menchero–Orr–Wang 2011)
- **Regression:** daily √cap WLS; country + industries + styles; cap-weighted industry sum-to-zero.
- **Factor covariance:** vol half-life 84d (S) or 252d (L); correlation half-life 504d; NW lags 5 (vol) and 2 (correlation).
- **Eigenfactor adjustment:** Monte Carlo at the effective T, parabola fit, a=1.4.
- **VRA:** B_t = √(mean_k (f_k/σ_k)²), where σ_k is the forecast available at t−1. λ² is an EWMA of B² with half-life 42d (S) or 168d (L). Applied to factor and specific risk separately.
- **Specific risk:** EWMA 84d + NW 5, blended with a structural model (regress ln σ on exposures, E0). Bayesian shrink toward the size-decile mean, v = q|σ−σ̄|/(Δσ + q|σ−σ̄|), with q=0.1.
- **Validation:** bias stat std(r/σ̂) with 95% CI 1±√(2/T); target MRAD ≈ 0.17. Without eigen-adjustment, optimized portfolios under-predict risk by up to about 40%.
- **Alpha Alignment Factor:** Renshaw/Stubbs/Saxena.
- **Crowding overlays:** valuation spread, SI spread, pairwise correlation.

### 3.8 Large-scale optimization
- **Scale reference:** factor structure gives O(nk²). MOSEK solves n=10k, k=100 in under 1 s (Boyd et al., "Markowitz at Seventy", 2024). Our 58 ms at 3000×64 is competitive once costs are actually included.
- **Closed-form prox of κ|u| + t|u|^{3/2}:**
  - v′ = |v| − κ/ρ; if v′ ≤ 0 then u = 0.
  - Otherwise s = [−1.5t + √(2.25t² + 4ρ²v′)]/(2ρ), and u = sign(v)·s².
- **Markowitz++:** robust mean penalty ρ′|w|, robust covariance, soft limits with priorities. Tune on realized net utility.
- **Relax-round-polish** for cardinality (Takapoui–Moehle–Boyd–Bemporad). Warm starts along the AUM and hyperparameter grids.
