# Qualification policy v4 and its power study

Tier-1 v2 node 1.10. Policy file `src/atx_db/seeds/research_qualification_policy_v4.json`, loaded by
`atx_db.research.qualification.load_policy_v4()`; wave bookkeeping in `atx_db.research.trial_registry`;
power study `scripts/research_power_study.py`.

## 1. Frozen identity

| item | value |
|---|---|
| policy id | `r4-qualification-v4` |
| content sha256 (canonical JSON, `policy_sha256` excluded) | `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a` |
| file sha256 (LF / CRLF checkout) | `d606e12ba8fe9f9c79dd4c1217ff3d85398015be7a707383bb0533095f05f5b3` / `4e4110eef9fb4daad5374a17b622462abbcb300ebae426b2d1affb6f88bab3e2` |
| pinned | `qualification.FROZEN_POLICY_SHA256["r4-qualification-v4"]` |
| registered in the research store | `research_qualify.py freeze --policy <v4 file>`: `data/research/research.duckdb`, `research_qualification_policies`, registered_at 2026-09-26T15:26:01.654961 (UTC) |
| committed | `396550de` (2026-09-26 ~15:28Z) |
| first forward return read (the power study, selection window only) | 2026-09-26T15:35:38Z (a `--quick` run; the full run of record read at 15:50:00Z) |
| evaluation split | `r4-split-v1` (sha256 `b1271e3f...6f26`): selection = train + validation formations whose label window ends before 2024-01-01; holdout 2024-01 onward |

**Order (ruling R-6).** The policy was frozen and registered before any tier-1 v2 code read a forward
return. The power study was the first reader, and it loads only bars dated on or before 2023-12-31, so no
holdout-period price has been read. A wave (for example `w1_price` at node 1.13) must also be registered in
the trial registry, and its anchor committed, before its first label join. Grading enforces this order in
code (§2, *order and seal*).

**What the power study saw of the selection sample (review I1, disclosed in fix round 1).**

* **What it computed.** To build the null covariance, the study computed per-formation cross-sectional
  rank scores of the real 3-month forward returns over 2012-03 .. 2023-08.
* **What that amounts to.** The covariance's lag-k blocks, `sum_i u_i,t u_i,t+k`, are the cross-sectional
  rank correlation between a line's 3-month return and its 3-month return k months later. That is the rank
  IC of a lagged 3-month-return window, which belongs to the momentum / reversal / seasonality family of
  `w1_price`:
  * lag 3 ≈ a months −3..0 window;
  * lag 6 ≈ months −6..−4;
  * lag 12 ≈ `seas_1_1an`;
  * lags 13–57 ≈ `ret_36_13` / `ret_60_13`.
* **What was published.** Two of these values were printed and committed: the null IC autocorrelations at
  lags 3 and 6 (0.025 / 0.022, §5.2) and the model autocovariances in §5.1. After dividing out the latent
  kernel and the common-name share, they imply a selection-sample 3-month rank IC of about **+0.035 (lag 3)
  and +0.043 (lag 6)**.
* **What it did not compute.** No IC, spread or other statistic of any catalog feature was computed. The
  study's null features are independent of returns.

The `w1_price` ids, definitions and signs were fixed before this read, in the S1 plan and `task-1.12-brief.md`.
`register_wave` now refuses rows that differ from the pre-registration table (ids and signs; §2), so no
momentum, reversal or seasonality row can be added, dropped or re-signed after the fact. A future calibration
that must stay blind should use a train-only window or a label-free proxy.

v3 (`research_qualification_policy.json`, `r4-qualification-v3`) stays the legacy R3b/R4 DuckDB-oracle policy
(ruling R-5); v4 grades pre-registered waves. `qualify` / `qualify_run` refuse a v4 policy and
`grade_wave_v4` refuses anything that is not v4.

## 2. What v4 changes (rulings R-6, R-7, C-24, C-25)

* **Evidence classes** come from the catalog's `evidence_class` (`catalog.derive_evidence_class`):
  *replication* = a published anomaly or analogue with a pre-registered sign; *discovery* = an economic
  conjecture, a two-sided row, or a composite.
* **Gating family** = the primary cells (`rank_normal` x 3 months) of the wave's gating hypotheses
  (registered, research-eligible, not reported-only), each at its evidence-class p-value, plus every
  gating hypothesis of every earlier registered wave at p = 1 (`evaluation.gating_bh_q`). The **reported
  family** is every cell (the R3b family columns `bh_q`, `holm_p`, `dsr`), reported, never gating.
* **Trial registry**, cumulative across waves: `trials_so_far()` (every registered configuration =
  feature x variant x horizon) is the deflated-Sharpe `n_trials`; `gating_hypotheses_before(wave)` pads the
  gating BH family. The grade refuses evaluated cells that were not registered, so `n_trials` cannot be
  understated.
* **Registration binds the rows (fix round 1).** `register_wave(..., rows=<catalog entries>,
  preregistration=<table>)` refuses rows that differ from the pre-registration table on ids or signs. It
  stores each row's sign, evidence class, population and a definition digest (`trial_registry.ROW_DIGEST_FIELDS`:
  every field that computes or grades the row; prose excluded). The grade recomputes those digests from
  the catalog it is given, and a changed row is refused. The comparison covers only the wave's own rows, so
  later waves adding catalog rows never break an earlier wave's final grade.
* **The registry is tamper-evident (fix round 1).**
  * *In the file:* a hash chain catches an edited or reordered line, and a torn line is refused.
  * *Outside the file:* a dropped tail is caught by the head anchor. After every write the registry appends
    `(sequence, record_sha, event, wave)` to `src/atx_db/seeds/research_trial_registry_anchor.jsonl`, and the
    wave runner **commits** that file.
  * *At grading:* the grade reads the anchor from git `HEAD` and refuses an empty, unanchored, truncated or
    altered registry.
* **Order and seal (fix round 1).**
  * `evaluation_spec_kwargs_v4(policy, registration=...)` binds the run to the registration: the record
    sha, plus `created_at`, which must not precede `registered_at`.
  * By default it seals the holdout: the engine drops every label whose window ends on or after 2024-01-01,
    or whose end is unknown, before computing anything.
  * The grade refuses holdout-period statistics (an unsealed run, or frames that carry them) unless the
    registry recorded the wave's one opening for exactly the run's label set (`evaluation.label_set_sha256`).
    So `final_labels=True` is checked, not trusted.
* **EWC oversize flag (ruling C-50, reported only).** Every graded row carries `persistence_1m` (the feature's
  1-month rank autocorrelation) and `ewc_oversize_risk`. The flag is true at or above 0.945, the rank
  autocorrelation of a latent persistence of 0.95, where the complete-null FDR of the replication gate
  exceeds q (§5.3). It changes no threshold.
* **Investable co-primary cell**: price >= $5 and market cap >= the NYSE 20th percentile (supplied NYSE
  breakpoints, e.g. Fama-French `ME_Breakpoints`, else point-in-time NYSE names).
* **Holdout**: opened once per wave (`open_holdout(wave, final_labels=True, label_sha=<final label set>)`),
  only on final labels; the rule is *sign consistent and no significant shrink* instead of v3's
  `holdout z >= 1.5`. The `label_sha` is the label-matrix spec sha of the final labels
  (`label_matrix.compute_label_sha`, ruling C-52). The label matrix checks it before it lets a holdout read
  through, and the opening checks that set: its spec is final, it is complete, and it seals from 2024-01-01.
* **Coverage** against the catalog population (`population`), on the feature's history-eligible selection
  formations.
* **Long-horizon evidence**: Jegadeesh-Titman calendar-time portfolios for K = 6 and 12 months (reported).
* **C-24**: `beta_ew_252d`, `ivol_ew_21d`, `ivol_ew_252d` (equal-weight-market twins of the value-weight
  beta and the FF-residual ivol) are evaluated and reported only; they never enter the gating family but
  count as trials. A wave can register more reported-only rows at registration time.
* **C-25**: two-sided rows are graded under the discovery gate and fix their realized sign; they are excluded
  from signed composites and reported separately (node 4.4).

## 3. Gates (first failing tier sets the status)

All statistics on the primary cell over the selection sample; EWC fixed-b inference (`stats.mean_inference`,
`horizon_periods = 3`). Values are sign-oriented, so a signed hypothesis is a positive IC.

| order | gate | replication | discovery | failing status |
|---:|---|---|---|---|
| 0 | reported only | C-24 twins and rows registered reported-only | same | `reported_only` |
| 1 | coverage | primary cell tested; value coverage of the catalog population >= 0.60 on >= 90% of the history-eligible selection formations, and >= 36 such formations | same | `insufficient_coverage` |
| 2 | sign | one-sided p against the pre-registered sign <= 0.05 | signed conjecture: z <= -3 | `sign_reversed` |
| 3 | significance | one-sided p <= 0.05 **and** gating BH q <= 0.10 | HLZ \|z\| >= 3 (in the pre-registered direction when signed) **and** gating BH q <= 0.05 **and** DSR >= 0.95 | `not_significant` |
| 4 | investable | investable IC one-sided p <= 0.05 in the fixed direction (>= 36 formations, else `insufficient_evidence`) | same | `not_investable` |
| 5 | size buckets | small and large NYSE 20/50 bucket IC means keep the sign (>= 36 formations each, else `insufficient_evidence`) | not gated | `unstable` |
| 6 | holdout (final labels, opened) | sign consistent and Welch shrink one-sided p >= 0.05 | same | `holdout_failed` |
| - | pass | `selection_pass` while the holdout is sealed; `qualified_strict` / `qualified_reconstructed` after it | same | |

* One-sided p in direction d: p/2 when d*t > 0, else 1 - p/2 (p = the two-sided EWC p).
* DSR: deflated Sharpe probability of the direction-oriented `ls_ew10` series, `horizon_periods = 3`,
  `n_trials = trials_so_far()`, `sharpe_variance` = the cross-cell variance of the wave's tested cells at the
  primary horizon.
* Shrink test: t = d (m_hold - m_sel) / sqrt(se_hold^2 + se_sel^2), EWC standard errors, Welch-Satterthwaite
  df; p = P(T <= t).
* Resolution across bases (RX1): a basis that passes while another is `sign_reversed` makes the feature
  `unstable`; else a strict basis that qualifies wins; else the reference research basis (reconstructed).
* Grade basis: `provisional_labels` grades the selection sample only and never reads the holdout (statuses
  stop at `selection_pass`); `final_labels` grades the holdout only after the registry recorded its one opening.

## 4. How a wave uses it (node 1.13 and later)

```python
from atx_db.research import evaluation as ev, label_matrix as lm, qualification as rq, trial_registry as tr
from atx_db.research.catalog import anomaly_catalog_sha256, load_anomaly_catalog

policy = rq.load_policy_v4()
catalog = [e for e in load_anomaly_catalog() if e.wave == "w1_price"]
digest = anomaly_catalog_sha256()
cells = [(e.feature_id, v, h) for e in catalog for v in ev.expected_variants(e.anomaly_class) for h in (1, 3, 6, 12)]
prereg = {...}   # the pre-registration table (1.12 brief): feature_id -> expected sign
tr.register_wave("w1_price", digest, policy.sha256, [e.feature_id for e in catalog], cells,
                 rows=catalog, preregistration=prereg, first_formations={...})   # BEFORE any label join (R-6)
# commit src/atx_db/seeds/research_trial_registry_anchor.jsonl now: grading reads the anchor from git HEAD
registration = tr.TrialRegistry().require_registration("w1_price", catalog_digest=digest, policy_sha=policy.sha256)
spec = ev.EvaluationSpec(run_id="w1_price_provisional", verify_panels=False,
                         **rq.evaluation_spec_kwargs_v4(policy, registration=registration))   # sealed holdout
# bases: per basis, labels, maturity, info = lm.LabelMatrix().r3b_inputs(provisional_sha, ...) (sealed read);
#   ev.BasisInputs(..., meta={..., ev.LABEL_READ_META: ev.label_read_meta(info)})
tables = ev.evaluate_bases(bases, spec, catalog=ev.catalog_features(catalog))
ledger = rq.grade_wave_v4(tables.cells, tables.slices, tables.series, policy, wave="w1_price", spec=spec,
                          catalog=catalog, catalog_digest=digest, grade_basis=rq.GRADE_PROVISIONAL,
                          run_bases=tables.bases)
# final labels (node 3.8, later; ruling C-52): the holdout identity is the label-matrix spec sha, known
# before any holdout label is read:
#   final_sha = lm.compute_label_sha(final_label_spec)   # provisional=False, holdout_start="2024-01-01"
#   tr.open_holdout("w1_price", final_labels=True, label_sha=final_sha)   # checks: final, complete, sealed
#   commit the anchor; then per basis:
#   labels, maturity, info = lm.LabelMatrix().r3b_inputs(final_sha, (1, 3, 6, 12), calendar=..., securities=...,
#       label_cutoff=..., eom_before=<after the last formation>, allow_holdout=True, holdout_wave="w1_price")
#   ev.BasisInputs(..., meta={..., ev.LABEL_READ_META: ev.label_read_meta(info)})
#   spec = ev.EvaluationSpec(..., **rq.evaluation_spec_kwargs_v4(policy, registration=registration,
#                                                                  label_sha=final_sha))
#   grade with rq.GRADE_FINAL: the opening, the spec and every basis's recorded read name final_sha, and
#   each basis read the holdout through this opening
```

The engine options the spec turns on (all off by default; a pre-v4 spec gives byte-identical results):
the context needs `price`; the calendar may carry `nyse_me_p20` / `nyse_me_p50` (same units as
`market_cap`); a conditional population needs `population_<name>` flags in the context. A sealed run needs the
label maturity rows (`expected_end`): a label without a known end is dropped.

## 5. Power study

`scripts/research_power_study.py` (its docstring carries the full model). Run of record, 2026-09-26:

```
OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe scripts/research_power_study.py --runs 400 --json <out>.json
```

Seed 20260926. The null tables and the main power table use 400 runs x 200 features; each sensitivity uses
200 runs. The run took 446 s, with a peak working set of 348 MiB (private 334 MiB). The DuckDB extraction runs
one query per calendar year at 256MB and 1 thread.

### 5.1 Design

* **Real data, selection window only.** The retained vendor price file (`TickerHistory3.parquet`), bars dated
  on or before 2023-12-31 (every requested exit is asserted before 2024-01-01), and the Fama-French NYSE ME
  breakpoints (U6). No warehouse, no holdout-period price.
* **Formations**: month-end, 3-month label (entry at the close of the session after the month-end, exit at
  the close of the session after the third month-end), split-adjusted by the vendor's cumulative return
  factor, label ending before 2024-01-01 (the policy's purge rule).
* **Universe proxy (labeled)**: lines with an earnings event in the vendor's prior 504 sessions
  (`nEarnCnt_504d > 0`), positive close and shares, valid 3-month return. ME = close x vendor shares (current
  shares, A8/A9 caveat; only slice membership uses it). Investable = price >= $5 and ME >= NYSE p20; small /
  large = the NYSE 20 / 50 buckets.
* **Null and planted features** (Gaussian copula): a null feature is a latent AR(1) per line (monthly
  persistence rho) independent of returns. Conditional on the real returns, its per-formation rank IC in each
  slice and its EW decile long-short are jointly Gaussian with covariance `k(rho^|t-s|) * sum_i w_i,t w_i,s`,
  where `w` are the real per-line weights. The real panel's overlap, turnover of names and time-varying
  dispersion all enter through `sum w w`. A planted signal shifts every IC slice by `ICIR_3m x sd(IC)` and
  the long-short by its linear projection. The holdout (T = 30) is drawn independently from the covariance
  of the last 30 selection formations: a stand-in window, because the sealed holdout is never read.
* **Grading is the production code**: EWC fixed-b statistics of `stats.mean_inference` (vectorized; checked
  equal on a sample), graded by `qualification.grade_v4` under the frozen policy (hash checked).
  * Replication = a pre-registered +1 sign; discovery = two-sided.
  * DSR `n_trials` = 4,800 (200 features x 6 variants x 4 horizons, as a wave would register).
  * DSR `sharpe_variance` = the run's cross-feature variance.
  * v3 comparison paths:
    * HLZ |z| >= 3 alone.
    * v3 significance: HLZ, or two-sided BH q <= 0.05 with |z| >= 2.
    * v3 holdout gate: holdout z >= 1.5.
* **Model check**: a direct simulation (latent features ranked against the real returns with the evaluation's
  own grouped rank correlation, rho 0.9, 200 features) against the model.

| quantity | direct | model |
|---|---:|---:|
| var IC | 2.827e-4 | 2.925e-4 |
| var LS | 4.913e-4 | 5.069e-4 |
| IC autocovariance at lag 1 | 1.501e-4 | 1.603e-4 |
| IC autocovariance at lag 2 | 6.42e-5 | 7.23e-5 |
| IC autocovariance at lag 3 | -8.0e-7 | 6.6e-6 |
| IC autocovariance at lag 6 | 9.3e-7 | 5.8e-6 |
| cov(IC, LS) | 1.804e-4 | 1.820e-4 |

The model is essentially exact.

* **The gap is estimator bias.** The direct estimator centres each simulated series on its own mean, which
  biases every lag's autocovariance by about −LRV/T ≈ −6e-6. That bias accounts for the variance, lag 1,
  lag 2, lag 3 and lag 6 gaps (reviewer's check).
* **Lags 3 and 6 are real, not model error.** At lag ≥ 3 the two 3-month windows no longer overlap, so the
  model's positive values there are real selection-sample statistics. They are the rank correlations of
  lagged 3-month returns, i.e. the rank ICs of past-return windows disclosed in §1. The direct simulation
  estimates the same quantities with that bias and with noise.

The vectorized EWC matches `stats.mean_inference`: max |dt| 2.2e-15, max |dp| 7.2e-8.

### 5.2 The real sample

| fact | value |
|---|---|
| XNYS sessions with bars (<= 2023-12-31) | 2,961; 0 stray closure bars; 0 sessions without bars |
| duplicate vendor keys (date, securityID) | 400, dropped (never picked) |
| universe-flag gap | 2012-06-29: the vendor published no earnings counters; flag taken from 2012-06-21 |
| selection formations (3-month label ends before 2024-01-01) | **138** (2012-03-30 .. 2023-08-31, last exit 2023-12-01) = 11.50 years |
| holdout formations with a 3-month label by 2026-09-18 | 29 (2024-01-31 .. 2026-05-29; a calendar count, no data read) |
| names per formation: all / investable / small / large | mean 3,439 / 2,302 / 1,073 / 1,278 (min 3,060 / 2,123 / 927 / 1,181) |
| lines in the selection panel | 7,148; 0 formations without NYSE breakpoints |
| annual Sharpe for t > 3 on monthly returns | 0.885 over 11.50 years (0.911 over 10.83 years) |
| null 3-month rank IC (rho 0.9) | sd 0.01706 per formation; autocorrelation 0.549 / 0.249 / 0.025 / 0.022 at lags 1 / 2 / 3 / 6 (lags 3 and 6 carry the selection-sample lagged-return rank ICs of §1: ≈ +0.035 / +0.043 after dividing out the kernel and the name overlap) |
| EWC degrees of freedom (h = 3) | 10 at T = 130; 3 at T = 30 |

The price-only sample reaches 138 selection formations. The 130 used below is the policy's
reference T. A feature with a shorter history (for example a 252-day window, or a SEC-derived feature
that starts later) sees fewer formations; section 5.5 shows what that costs.

### 5.3 Null false-qualification (acceptance)

Setup: 400 runs x 200 null features, T = 130 selection / 30 holdout, `n_trials` 4,800.

* **Per-feature rates** carry Wilson 95% intervals.
* **FWER = FDR.** The last two columns give the share of runs in which at least one of the 200 nulls
  passes. Every feature is null, so every pass is false and the false-discovery proportion is 1{any pass}.
  **Under the complete null, FDR ≡ FWER**, and this column is the gating family's FDR. Its target is the
  replication gate's BH q = 0.10.

Two runs produced these rows:

* the run of record (rows rho 0, 0.9, 0.98 and the regime stress);
* fix round 1 (2026-09-26 18:12Z, `--null-only --null-rhos 0.9,0.95,0.97,0.98,0.99`, same seeds), which added
  rows 0.95, 0.97 and 0.99. Its 0.9, 0.98 and stress rows reproduced the run of record exactly.

| scenario | one-sided p <= .05 | HLZ \|z\| >= 3 | replication selection | replication final | discovery selection | discovery final | FDR = FWER rep. selection | FDR = FWER disc. |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| rho 0 | 0.0512 | 0.0025 | 0.00047 [.00035, .00065] | 0.00014 | 0 [0, .00005] | 0 | 0.088 | 0 |
| rho 0.9 | 0.0561 | 0.0032 | 0.00055 [.00041, .00074] | 0.00029 | 0 [0, .00005] | 0 | 0.095 [0.070, 0.128] | 0 |
| rho 0.95 | 0.0630 | 0.0046 | 0.00096 [.00077, .00120] | 0.00050 | 0 [0, .00005] | 0 | **0.155 [0.123, 0.194]** | 0 |
| rho 0.97 | 0.0678 | 0.0049 | 0.00095 [.00076, .00119] | 0.00045 | 0 [0, .00005] | 0 | **0.153 [0.121, 0.191]** | 0 |
| rho 0.98 | 0.0724 [.0706, .0742] | 0.0058 | 0.00106 [.00086, .00131] | 0.00055 | 0 [0, .00005] | 0 | **0.160 [0.127, 0.199]** | 0 |
| rho 0.99 | 0.0803 [.0784, .0822] | 0.0076 | 0.00187 [.00160, .00220] | 0.00081 | 0 [0, .00005] | 0 | **0.228 [0.189, 0.271]** | 0 |
| rho 0.9 + AR(0.5) regime stress | 0.0558 | 0.0035 | 0.00084 [.00066, .00106] | 0.00032 | 0 [0, .00005] | 0 | **0.148 [0.116, 0.186]** | 0 |

Nominal rates:

* per feature: 0.05 for replication (one-sided alpha) and 0.0027 for discovery (HLZ, two-sided);
* family: FDR q = 0.10 (replication) and 0.05 (discovery).

**Acceptance, read honestly (ruling C-50).**

* **Discovery is within nominal on every reading.** It is 0 in 320,000 feature draws per scenario, and its
  family FDR is 0.
* **Replication's per-feature rate is far below 0.05 everywhere**, at most 0.0019.
* **But its family FDR, the relevant nominal for a BH-gated family, is controlled only up to persistence
  0.9:** 0.088 at rho 0 and 0.095 at rho 0.9.
* **At persistence ≥ 0.95 it exceeds q = 0.10:** 0.153–0.160 at 0.95–0.98 and 0.228 at 0.99, with every
  lower Wilson bound above 0.10. The regime stress also exceeds it (0.148). Levels between 0.9 and 0.95 were
  not simulated.

The cause is the EWC fixed-b p-value, which is anti-conservative at high persistence. The bare one-sided test
rejects 6.3% / 6.8% / 7.2% / 8.0% at nominal 5% for rho 0.95 / 0.97 / 0.98 / 0.99, and HLZ rejects up to 0.76%
against 0.27%. The stacked gates only remove BH rejections, so BH alone does at least as badly.

This is a known property of frozen v4; its thresholds stay as they are (C-49). Instead, every graded row
reports `persistence_1m` and `ewc_oversize_risk`, which is true at a 1-month rank autocorrelation of 0.945 or
more (the level of latent rho 0.95). A reader of a wave's grades sees which passes come from features in the
over-rejecting regime.

For `w1_price`, the reviewer expects the flag on the long-window and level features listed below. That is an
expectation, not a measurement: the flag is measured per feature at grading.

* `me_line_log`, `prc_log`;
* `beta_bab_1260d`, `ret_36_13`, `ret_60_13`;
* `rvol_252d`, `turnover_252d`, `ami_252d`, `zero_trade_252d`, `dolvol_126d`, `prc_highprc_252d`.

### 5.4 Power (rho 0.9, T 130, holdout 30, 20 of 200 true, 400 runs)

ICIR_3m = mean / sd of the monthly-sampled 3-month rank IC. The planted signal's annual EW decile long-short
Sharpe comes out about equal to its ICIR_3m (0.20 at 0.2, 0.60 at 0.6, 0.99 at 1.0).

| ICIR_3m | \|z\| >= 2 | HLZ | v3 significance | v3 holdout gate (T 30) | **v4 replication selection** | **v4 replication final** | rep. holdout pass given selection | v4 discovery selection | partial-null FQR rep. |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.2 | 0.236 | 0.036 | 0.036 | 0.202 | 0.027 | 0.018 | 0.674 | 0.000 | 0.0011 |
| 0.3 | 0.453 | 0.103 | 0.103 | 0.289 | 0.119 | 0.093 | 0.777 | 0.000 | 0.0020 |
| 0.4 | 0.677 | 0.219 | 0.219 | 0.394 | 0.359 | 0.305 | 0.850 | 0.000 | 0.0040 |
| 0.5 | 0.857 | 0.382 | 0.388 | 0.499 | 0.661 | 0.588 | 0.890 | 0.000 | 0.0070 |
| 0.6 | 0.950 | 0.593 | 0.644 | 0.604 | 0.856 | 0.784 | 0.916 | 0.000 | 0.0074 |
| 0.7 | 0.988 | 0.779 | 0.844 | 0.694 | 0.952 | 0.880 | 0.924 | 0.000 | 0.0072 |
| 0.8 | 0.998 | 0.895 | 0.943 | 0.770 | 0.989 | 0.914 | 0.925 | 0.000 | 0.0084 |
| 1.0 | 1.000 | 0.989 | 0.996 | 0.905 | 1.000 | 0.936 | 0.936 | 0.000 | 0.0080 |
| 1.2 | 1.000 | 0.999 | 1.000 | 0.963 | 1.000 | 0.929 | 0.929 | 0.001 | 0.0081 |
| 1.5 | 1.000 | 1.000 | 1.000 | 0.994 | 1.000 | 0.934 | 0.934 | 0.001 | 0.0078 |
| 2.0 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 0.930 | 0.930 | 0.002 | 0.0081 |

ICIR_3m at 50% / 80% power (interpolated on the grid):

| path | 50% | 80% |
|---|---:|---:|
| \|z\| >= 2 alone | 0.32 | 0.47 |
| HLZ \|z\| >= 3 alone | 0.56 | 0.72 |
| v3 significance | 0.54 | 0.68 |
| v3 holdout gate (holdout z >= 1.5, T 30) | 0.50 | 0.84 |
| **v4 replication, selection** | **0.45** | **0.57** |
| **v4 replication, final (after the holdout)** | **0.47** | **0.62** |
| v4 discovery, selection / final | not reached by 2.0 | not reached |

Against the review's approximations:

* **HLZ path.** The review put it at about 0.60; measured, it is 0.56 at 50% power and 0.72 at 80%.
* **BH + |z| >= 2.** The review put it at about 0.35; |z| >= 2 alone reaches 50% at 0.32. v4 replication,
  which adds BH q <= 0.10, the investable cell and the size buckets, reaches 50% at 0.45.
* **Old holdout gate.** The review put it at about 0.65; measured, 50% at 0.50 and 80% at 0.84. The gate is
  coarse because T = 30 gives 3 EWC degrees of freedom.

At ICIR >= 0.6, the v4 holdout rule ("sign consistent and no significant shrink") keeps 92-94% of true
selection passes. The v3 holdout gate, taken alone, passes a true signal only 60-77% of the time at ICIR
0.6-0.8.

### 5.5 Sensitivities (200 runs each)

v4 replication selection, ICIR_3m at 50% / 80% power; HLZ alone in the last column:

| scenario | 50% | 80% | HLZ 50% |
|---|---:|---:|---:|
| main (T 130, rho 0.9, 20 of 200 true) | 0.45 | 0.57 | 0.56 |
| T 128 | 0.45 | 0.57 | 0.55 |
| T 104 | 0.51 | 0.65 | 0.66 |
| T 80 | 0.61 | 0.77 | 0.78 |
| rho 0 (no persistence) | 0.27 | 0.36 | 0.34 |
| rho 0.98 | 0.46 | 0.59 | 0.57 |
| 1 of 200 true | 0.61 | 0.83 | 0.54 |
| 100 of 200 true | 0.35 | 0.49 | 0.55 |

How to read these:

* **Sample length.** Power falls quickly with T. A feature that enters with 80 selection formations needs
  ICIR about 0.77 for 80% power.
* **Feature persistence** sets how many independent observations the overlapping formations hold. A
  persistent feature (rho 0.9) makes consecutive 3-month ICs strongly correlated (lag-1 autocorrelation 0.55),
  which shrinks the effective sample. A non-persistent feature reaches 80% power at ICIR 0.36.
* **The BH gating family** makes power depend on how many true signals a wave holds. With a lone true
  signal among 200, the replication gate is stricter than HLZ at 80% (0.83 vs 0.72), because BH q <= 0.10
  over 200 hypotheses then behaves like a Bonferroni cut. With 100 true signals among 200, BH admits more
  and the partial-null false-qualification rate rises to about 0.02 per null feature, still below the
  nominal 0.05.
* **Partial-null FQR under persistence.** At rho 0.98, the rate of null features qualifying alongside true
  ones is 0.010-0.011.

#### Discovery gate (DSR >= 0.95 with the registry's `n_trials`)

Setting: rho 0.9, T 130, 20 of 200 true. Values are the rate of v4 discovery selection passes.

| DSR `n_trials` | ICIR 1.0 | ICIR 1.5 | ICIR 2.0 |
|---:|---:|---:|---:|
| 200 | 0.011 | 0.041 | 0.079 |
| 900 | 0.002 | 0.005 | 0.010 |
| 4,800 (main table, 400 runs) | 0.000 | 0.001 | 0.002 |

The DSR benchmark is the expected maximum Sharpe of `n_trials` null trials, scaled by `sharpe_variance`.
`sharpe_variance` is the cross-feature variance of the wave's tested cells. Planted signals inflate it,
and their inflation grows with their own strength, so in this setting the bar rises as the signals get
stronger.

A wave with a lone true signal would keep the variance near its null level. That run was **not
simulated**: the memory gate stayed closed for 1.6 hours after the run of record. An analytic estimate for
that case:

* **Null spread.** The null per-quarter Sharpe has sd about 0.14-0.15 (long-run-variance factor 2.65-3 over
  130 monthly-sampled 3-month returns).
* **Benchmark.** At 4,800 trials it is about 3.68 x 0.145 ≈ 0.53 per quarter.
* **DSR >= 0.95.** This needs SR >= 0.53 + 1.645 / sqrt(42) ≈ 0.79 per quarter: an annual long-short
  Sharpe, and so an ICIR_3m, of about 1.6 for 50% power.
* **At 200 trials** the same arithmetic gives about 1.3.

This is an estimate, not a measurement.

### 5.6 What this means for waves

* **Replication (published anomalies with a pre-registered sign)** is the workable class. At the price-only
  sample's T, a published anomaly needs ICIR_3m about 0.57 (annual decile long-short Sharpe about 0.57) for an
  80% chance of `selection_pass`, and about 0.62 to survive the holdout as well. The project's research estimate
  of post-decay Sharpe for true published anomalies is about 0.2-0.3 (tier-1 v2 status handoff, section 5). At
  that strength the gate passes 3-12%, so **most true replication rows are expected to grade
  `not_significant`**. A `not_significant` is a statement about power, not a refutation. Composites and
  longer samples are the levers.
* **Discovery is effectively closed at realistic ICIR.** The gate requires DSR >= 0.95 against a
  registry-wide `n_trials` in the thousands. That bar sits far above HLZ: measured power is ≈ 0 up to ICIR
  2.0 when a wave carries many true signals, and an estimated ICIR of about 1.6 is needed even for a lone
  signal. These are the brief's frozen thresholds. A discovery row has no prior, and the registry makes
  every tested configuration count. Because `trials_so_far()` grows with every wave, each later wave's DSR
  bar rises further.
* **The per-feature false-qualification rate is tiny, but the family FDR is not controlled for persistent
  features.** Stacked gates keep the per-feature rate at or below 0.0019. For features with 1-month rank
  autocorrelation ≥ 0.945 (latent persistence ≥ 0.95), however, the replication family's complete-null FDR
  is 0.15–0.23 against q = 0.10. A persistent feature's `selection_pass` carries `ewc_oversize_risk = true`
  and deserves the skepticism that implies.

### 5.7 Limitations

* **The null features are independent of returns and of each other.** Real features share factor exposures,
  so their ICs move together. Each feature's EWC test is still valid, but false passes then cluster: one
  factor shock can carry several correlated rows through BH together. A persistent factor-return regime
  behaves like the regime-stress scenario (FDR = FWER 0.148 under the complete null).
* **The calibration saw selection-sample returns.** The null covariance is built from the real 2012–2023
  3-month forward returns. Its lagged blocks are the rank ICs of past-return windows (§1: ≈ +0.035 at lag 3
  and +0.043 at lag 6, both published here). No catalog feature's statistic was computed, and the `w1_price`
  rows were fixed before the read. Even so, a strictly blind calibration would have used a train-only window
  or a label-free proxy.
* **Proxies.** The universe (earnings-active lines) and ME (vendor current shares) stand in for the
  point-in-time universe and market cap. Only slice membership depends on them.
* **Stand-in holdout.** The holdout is drawn from the last 30 selection formations, so it assumes no regime
  change after 2023. The real holdout is sealed.
* **Idealized planted signal.** It is equally strong in every slice, which is the best case for the
  investable and size gates. Its long-short is a Gaussian projection with no fat tails.
* The vendor's 2021-01-04 factor artifact (VA1, a -1.5% to -3.4% return bias on dividend payers) is not
  repaired in this study's returns. It touches the formations whose 3-month labels span that session (up to
  three). Those returns enter only the weights of the null covariance and the planted long-short
  projection, so the effect on the rates above is small but not measured.
