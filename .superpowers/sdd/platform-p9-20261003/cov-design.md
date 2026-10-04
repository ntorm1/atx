# COV design: covariance of record without look-ahead, and a fast binary container

**Date** 2026-10-03. **Author** cov-design agent (research + design only; no code edited, nothing built or run, no
commit). **Tree read** `C:/atx-wt/pool-2` at `5292b46e` (branch `feat/platform-v8-20260929`) plus wave-1 lane heads
read through `git show` (C1 `10c35df3`). **Ruled inputs** progress.md COV-1 and COV-2 (lines 175-177).

**Blindness statement.** No file under `build-equity/`, no integration-log result row, no root-R0-* report and nothing
dated 2024-01-01 or later was opened. While locating the COV-1 / COV-2 rulings, `progress.md:165-185` was displayed;
it holds public ledger lines (R0-11 / R0-12 verdicts and printed statistics, which lane rule 3 calls public). The one
fact this design uses from them is structural: Y-1 (vol-target-v1 on the atx-risk-v1 store) was accepted and became
Y-F, so the vol-target rule is the levered book's rule (progress.md:167). No number from those lines enters any
constant, rule or prediction below.

**Labels.** Every claim carries one tag: **[code]** read in code (path:line), **[src n]** sourced (bibliography,
section 8), **[design]** my judgment, **[arith]** planner arithmetic, **[est]** an estimate, not a measurement. Sources
marked "not fetched" were not opened in this pass; their claims are flagged where used.

---

## 0. Summary

1. **atx already has a USE4S-class fundamental risk model** (atx-risk-v1.1: market + 50 FF49 slots + 11 styles, EWMA
   84 / 504 with Newey-West 5 / 2, VRA 42, structural factor and specific fallbacks, Bayesian size-decile shrinkage,
   bias harness) and a vol-target / risk-target / spo stack that reads it in factored form [code]. What is missing is
   narrower than "a covariance pipeline": (a) no eigenfactor risk adjustment in the model of record although the
   engine has the kernel; (b) no producer-side truncation test (the timing claim is a comment); (c) a store format
   that is a directory of raw stacked arrays read by seek-per-row, with no per-row stamp, no ids, no magic, and a
   full-file SHA-256 per open; (d) no eigenfactor / minimum-variance / optimised-book bias families in the harness of
   record; (e) no engine-level zero-copy reader for new consumers (a max-Sharpe target, attribution, diagnostics).
2. **Recommended pipeline (COV-v1 recipe `atx-cov-v1`)** = atx-risk-v1.1 stages unchanged + Menchero-Wang-Orr
   eigenfactor adjustment (USE4's milder "simulated" form, a = 1) on the fully observed factor block, seeded per
   session, then VRA; specific risk unchanged; stamped per session; written to a new container beside the legacy
   files. Statistical (hybrid) factors and dense shrinkage / DCC-NL are benchmarks or P10, not P9 production.
3. **No-look-ahead invariant** (section 3.2): block t is a function of returns, exposures, membership and caps at
   sessions <= t only, stamped `as_of = cutoff = session t`; the house decision at d (after d's mark, fill at d+1)
   reads block d. Enforced by a producer truncation suite (delete-after and perturb-after, byte-identical blocks,
   plus a planted-leak probe that must fail) and by reader refusals.
4. **Format** (section 4): one self-describing little-endian `.atxcov` file per (role, recipe) run: 4 KiB header,
   4 KiB-aligned per-session blocks with 64-byte-aligned arrays, a date index, per-block SHA-256 under a root digest,
   trailer; mmap zero-copy read with full or lazy verification. Written publish-last beside the legacy
   `atx.risk-model/v1` files; a `container` key in the manifest switches `spo::RiskStore` to it with bit-identical
   `RiskSlice`s, so no NAV file is edited and every existing pin is untouched.
5. **Integration** (section 5): vol-target, risk-target and spo-v1/2/3 read COV through the unchanged `--risk-model`
   flag (the choice of store pin is the switch). A max-Sharpe step is a NAV `target`-kind rule (K-P9-7) between the
   composition output and the construction rule, needing C3's registry: a P10 lane by default, optional wave 3b.
6. **One lane (COV, wave 2, L)**; optional second lane COV-MV (wave 3b) only if the PM registers P9-MV in P9.

---

## 1. What exists today

### 1.1 The estimator of record: atx-risk-v1.1 (atx-impl)

| item | what the code does | where |
|---|---|---|
| model id, geometry | `atx-risk-v1.1`; 62 factors = market + 50 industry slots (FF49 1..49 -> 0..48, residual 49) + 11 styles | [code] `atx-impl/src/strategy_risk_model.hpp:64-80` |
| exposures | styles z-scored per session: MAD fence (3.5 x 1.4826), cap-weighted mean, equal-weighted SD, clip 3, missing 0 | [code] `strategy_risk_model.hpp:11-17, 146-170` |
| factor returns | WLS of session-t simple returns on X_{t-1}, weights sqrt(cap_{t-1}), sum_k cap_k f_k = 0 over active industries; thin styles dropped | [code] `strategy_risk_model.hpp:18-21, 118-144`; recipe text `strategy_risk_verb.cpp:840-843` |
| factor covariance | EWMA vol HL 84 x NW ratio (5 lags, NW HL 252), EWMA correlation HL 504 + NW 2, recursive O(K^2) per session, PSD by eigenvalue floor | [code] `strategy_risk_model.hpp:22-28, 83-87, 213-248`; floor `strategy_risk_model.cpp:878-885` |
| VRA | lambda^2 = EWMA_42 of B_t^2 on prior pre-VRA forecasts, factor and specific | [code] `strategy_risk_model.hpp:26-28, 47` |
| thin factors | structural blend w own + (1-w) class prior, w = n / 63; off-diagonals scaled until PD | [code] `strategy_risk_model.hpp:29-40, 88-91` |
| specific risk | EWMA 84 + NW 5, structural ln-vol regression blend gamma = min(1, h/252), bounded [p1, p99], Bayesian shrink to size deciles q = .1, specific VRA; hard invariant (0, 1) refuses the run | [code] `strategy_risk_model.hpp:41-49, 92-100, 172-211` |
| eigenfactor adjustment | **absent** from the model of record (no call to `eigen_adjust*` in `strategy_risk_model.cpp`) | [code] grep of `strategy_risk_model.cpp` |
| timing claim | "every quantity at t reads rows <= t only"; one sequential pass t = 0..dates-1, sinks see each session once | [code] `strategy_risk_model.hpp:50`; `strategy_risk_model.cpp:1027-1068` |
| producer truncation test | **absent**: `strategy_risk_model_test.cpp` has 29 tests (WLS, EWMA parity with the engine, bias bands, structural, robustness, verb, seal) and none deletes or perturbs the future | [code] `atx-impl/tests/strategy_risk_model_test.cpp:100-1494` (test list) |
| bias harness | families random (64 seeded dollar-neutral books), factor, book; complete forecasts only; missing return counts 0 and is reported; USE4 bands | [code] `strategy_risk_model.hpp:250-263, 339-411` |
| industry PIT | `grp_ff49` = SIC as of each filing, read with clock < mark of d-L | [code] `atx-engine/tools/prepare_research_fields.py:29, 430` |
| seal | a role reaching the seal is refused without `--unseal OWNER` | [code] `strategy_risk_verb.cpp:862-872` |
| Debug speed | `strategy_risk_model.cpp`, `strategy_risk_verb.cpp`, `strategy_spo.cpp` are compiled /O2 even in the Debug tree | [code] `atx-impl/CMakeLists.txt:114-129` |

### 1.2 The store and its readers

| item | today | where |
|---|---|---|
| writer | `atx-equity-strategy-risk risk --role --fields --output NEWDIR [--emit-exposures all]`; streams per-session files, `manifest.json` last; a refusal leaves no manifest | [code] `strategy_risk_verb.cpp:1-4, 356-454, 959-998` |
| files | `factor_covariance.f64` dates x 62 x 62 row-major (NaN unforecast), `specific_variance.f64` dates x N (NaN none), `style_exposures.f32` dates x N x 11 (0 missing), `industry_slot.u8` dates x N (255 no row), `factor_structural.u8`, `diagnostics.csv`, `factor_returns.csv` | [code] `strategy_risk_verb.cpp:944-955` (layouts) |
| manifest | schema `atx.risk-model/v1`, role pin, fields pin, seal block, producer (exe SHA-256, git SHA), geometry, recipe, bias summary, per-file SHA-256 and bytes | [code] `strategy_risk_verb.cpp:909-957` |
| reader | `spo::RiskStore::open(dir, manifest_sha256, role_sha256)`: manifest SHA, schema, status, role pin, geometry 62/11, then **SHA-256 of every payload file**; `read(d)` opens four files with `ifstream`, seeks, reads row d, NaN covariance entries -> 0 (counted), non-finite f32 styles -> 0 | [code] `atx-impl/src/strategy_spo.cpp:648-769`; API `strategy_spo.hpp:221-266` |
| pin and role check | NAV opens the store once per run, before any replay work, and shares it across books | [code] `atx-impl/src/strategy_nav_v7.cpp:1088-1097` |
| vol-target-v1 | L_t = clip(L sigma_ref / sigma_hat, 1, L); sigma_hat = sqrt(252 x book variance) of the book's current weights at gross 1 on store row d; sigma_ref = running mean; cadence 21 | [code] `strategy_vol_target.hpp:3-17`; `strategy_vol_target.cpp:12-27` |
| risk-target-v1 | L_t = clip(S / (b sigma_hat), .8 L, 1.25 L), b 1.15, cadence 21, same store and forecast | [code] `strategy_risk_target.hpp:3-18` |
| per-book scaler (after C1) | `BookScaler::estimate` calls `risk_->read(d, slice_)`, compacts the priced names (slot < 50 and finite positive specific) and calls the engine kernel on a `FactorRiskView` | [code] `git show feat/p9-c1-20261003:atx-impl/src/strategy_risk_target.hpp` (BookScaler); base `strategy_risk_target.cpp:125-155` |
| engine kernel view | `FactorRiskView{groups, styles, group (u32, 1..groups, 0 none), exposures n x styles f64, covariance K x K f64, specific f64}` | [code] `atx-engine/include/atx/engine/book/risk_target.hpp:55-62` |
| decision timing | decide at session d after its mark, fill at d+1's close, first return row d+2 | [code] `atx-impl/src/strategy_nav_replay.hpp:17-21` |
| consumer truncation test | C1 added `VolTarget.TruncationInvariant`: replaces the store's and the panel's future after row T, checks every leverage record and daily row <= T bit for bit, and that the first estimate after T moves | [code] `git show feat/p9-c1-20261003:atx-impl/tests/strategy_vol_target_test.cpp` lines 635-720 |
| the pin in the cell | Y-1 spec = Y-F0 + `nav --vol-target vol-target-v1 --risk-model <store> --risk-model-sha256 <pin>`; P14 role pin; a `--risk-model` NAV gets no history read | [code] `.superpowers/sdd/platform-v8-20260929/v8y-prereg.md:214-232, 389, 488` |
| other store readers | spo-v1/v2/v3 (cost-aware MV and implied-aim tracking; gradient `X (F (X'w)) + D w`, "never an N x N matrix") | [code] `atx-impl/src/strategy_spo.hpp:3-46`; `strategy_spo_v3.hpp:20` |

### 1.3 Engine pieces already present (atx-engine/risk)

| piece | status | where |
|---|---|---|
| `FactorModel` (factored V, Woodbury `apply_inverse` via capacitance Cholesky, never dense) | used by the engine optimizer | [code] `atx-engine/include/atx/engine/risk/README.md:70-88` |
| `eigen_adjust_v2(F, effective_obs, sims=64, amplification=1.4, seed=7, max_bytes)`: MWO with the observed Kish effective count as simulated history; seed-pinned canonical draw order | exists, unused by atx-risk-v1.1; **default amplification 1.4** | [code] `eigen_adjust.hpp:44-56, 80-92` |
| `hybrid_factor_model` (fundamental + APCA statistical block, Bai-Ng / MP factor count) | exists, research path | [code] `hybrid_factor_model.hpp:1-42` |
| `shrinkage.hpp` (Ledoit-Wolf constant correlation, MP clip), `psd_repair.hpp` | exist | [code] `risk/README.md:38-43` |
| `model_validation` v1 (bias, Q, MRAD, min-variance and optimised books) | v1 **renormalises books on realised availability** (a forward-looking filter); v2 (21-day) never renormalises | [code] `model_validation.hpp:29-33, 127-131` |
| `FactorModelArtifact` serializer | flat LE dump, no magic, no alignment, no ids or dates, wyhash digest "not cross-process/platform-portable" | [code] `atx-engine/include/atx/engine/data/factor_model_artifact.hpp:62-78, 205-213` |
| tsdb segment format (precedent) | `tag8` magic, version, POD header with `static_assert` sizes, offsets from base, 64-byte block alignment, footer seal marker + CRC32; header carries a wall-clock field | [code] `atx-tsdb/include/atx/tsdb/segment.hpp:22-41, 53-95` |
| read-only mmap RAII | `atx::tsdb::Mapping::map_file_ro(path, expected_bytes, max_bytes)`, `prefetch()` | [code] `atx-tsdb/include/atx/tsdb/mapping.hpp:17-42`; already used by `atx-engine/src/data/panel_store.cpp:18` |
| SHA-256 | incremental, SHA-NI backend when CPUID allows | [code] `atx-core/include/atx/core/sha256.hpp:14-58` |

### 1.4 The gap, precisely

| gap | consequence | closed by |
|---|---|---|
| no eigenfactor adjustment in the model of record | optimised and concentrated books' risk is under-forecast in the small eigen-directions (USE4: lowest-vol eigenfactors realise ~40% above forecast) [src 1 §4.2] | COV task 2 |
| PIT claim untested at the producer | a future edit that reads a full-sample statistic would pass every existing test | COV task 3 (truncation suite + planted leak) |
| store open hashes every payload (~314 MiB at D 1,000, N 5,627) and `read` reopens four files per row [arith from the layouts] | acceptable for today's NAV (one open; vol-target reads one row per 21 sessions) [code]; wasteful for per-decision consumers and research loops | COV task 1, 4 |
| no per-row stamp, ids or recipe hash in the payload | a row cannot prove what it saw; axes are checked only against the role | COV task 1 (block header + index) |
| no eigen / MVP / optimised bias families of record | the standard out-of-sample tests [src 1 App. A, src 2, src 14] are not printed | COV task 5 |
| no engine zero-copy reader | a max-Sharpe target or attribution would copy and re-validate | COV task 1 (`CovModelFile::view`) |

---

## 2. Literature survey: what institutional equity risk models do, and what each step costs here

Scale used for costs: N = 1,000-5,000 names, T = 1,000 sessions, K = 62 factors (today's geometry) [arith unless
tagged]. "Fixes" = the estimation error the step addresses.

### 2.1 Factor models: fundamental, statistical, hybrid

- **Fundamental (Barra USE4, Axioma AXUS4).** Asset returns are regressed daily on known exposures (country/market,
  industries, styles) with sqrt-cap weights and an industry sum-to-zero constraint; factor returns feed the factor
  covariance and residuals feed specific risk [src 1 §2-3, src 4]. AXUS4: 68 GICS industries, 13-14 styles,
  root-cap Huber-robust regression, estimation universe ~2,861 names on average, estimated daily [src 4].
  Northfield's US fundamental model: 67 factors (beta, 11 fundamentals, 55 industries) [src 6]. *Fixes:* reduces
  N(N+1)/2 parameters to K(K+1)/2 + N and makes V positive definite whenever D > 0 [src 33 §2]. *Data:* PIT
  descriptors, industry codes, caps, returns. *Cost:* one weighted least-squares per session, O(N K_s^2 + N) with
  one-hot industries (`hybrid_factor_model.hpp:12-19`) [code], ~2e7 flops per session at N 5,000 -> seconds per
  1,000 sessions [arith]. **atx has it** (atx-risk-v1.1).
- **Statistical (APCA).** AXUS4-stat: 15 factors by 2-pass asymptotic principal components with residual-variance
  weighting, one year of returns for exposures, four years of factor returns for their covariance [src 4]. *Fixes:*
  latent co-movement the named factors miss. *Cost:* Omega = R'R / N is T_w x T_w: O(N T_w^2) = 3.2e8 per refit at
  T_w 252 [arith]. **atx has the kernel** (engine `stat_factor_model`, `hybrid_factor_model`) [code], not in the
  model of record.
- **Hybrid ("blind factors in residual").** Northfield adds temporary factors = principal components of the residual
  covariance with significant eigenvalues, to absorb omitted-variable bias when a new pervasive force appears
  ("the Internet effect") [src 5]. Ledoit-Wolf report that an approximate factor model whose residual covariance is
  shrunk non-linearly is robust to using the market factor only, i.e. it "recover[s] left-over factor structure"
  automatically [src 13 §5]. *Implication:* a statistical completion of the residual is the scalable route to the
  same benefit; it changes the factor geometry (section 6, Q7).

### 2.2 Exponentially weighted estimation with separate half-lives

USE4: factor volatility HL 84 (USE4S) / 252 (USE4L), correlation HL 504, Newey-West 5 lags for volatility and 2 for
correlations, VRA HL 42 / 168 (Table 4.1) [src 1 §4.1]. The correlation half-life "must be sufficiently long so that
the effective number of observations T is significantly greater than the number of factors K" or optimised
portfolios show "spuriously low factor risk"; the volatility half-life can be shorter because diagonal sampling error
barely affects conditioning [src 1 §4.1]. AXUS4: variance HL 125 / 60 (MH / SH), correlation HL 250 / 125 [src 4].
*Cost:* recursive EWMA O(K^2 (1 + L)) per session, trivial [code `LaggedEwma`, `strategy_risk_model.hpp:219-244`].
**atx has USE4S's numbers exactly** [code `strategy_risk_model.hpp:83-87`].

### 2.3 Serial correlation and asynchronous trading

- USE4 forecasts a one-month horizon from daily factor returns and applies Newey-West (1987) to account for serial
  correlation; missing factor returns are handled by the EM algorithm of Dempster (1977) [src 1 §4.1; src 7, src 23
  not fetched]. Axioma: NW with 2 days of autocorrelation, a fixed 1-day lag for statistical factors, 1 day for
  specific returns [src 4].
- Nonsynchronous trading (stale prices of thinly traded names) induces spurious autocorrelation and lead-lag in
  measured returns [src 8]. Burns, Engle and Mezrich (1998) show correlations of asynchronously sampled series are
  biased and propose synchronisation [src 9, record only, not fetched]. *Implication for atx:* a single-market US
  close-to-close panel has no cross-time-zone asynchrony; the residual effect (stale prices of illiquid names) is
  what the NW lags cover. No new stage [design].

### 2.4 Eigenfactor risk adjustment and volatility regime adjustment

- **Eigenfactor adjustment (Menchero, Wang, Orr).** Bias statistics of eigenfactors rise as eigen-volatility falls;
  the lowest-volatility eigenfactors realise about 40% more volatility than predicted, and all 100 optimised factor
  portfolios in USE4's test fall outside the 95% band [src 1 §4.2]. Simulating histories from the sample covariance,
  re-estimating and comparing the simulated eigenfactors' true and predicted variance measures the bias, stable over
  time (smallest eigenfactor 1.4-1.6 about 98% of the time over ~16 years) [src 2]. The empirical scale a = 1.4 removes
  the eigenfactor biases [src 2 App. A]; but it "may induce small biases for the pure factors", so **USE4 ships the
  milder simulated adjustment (Eq. B7)** [src 1 §4.2]. Published as Menchero, Wang, Orr (2012) FAJ 68(3) [src 3].
  *Fixes:* optimiser-driven under-forecasting. *Cost:* per session O(M (K^2 T_sim + K^3)); with M 64, K 62,
  T_sim ~1,454 (Kish count of an HL-504 EWMA, ~2.885 x HL [arith]) about 1.6 GFLOP per session, 1.6 TFLOP per 1,000
  sessions [arith]: tens of seconds to a few minutes on an optimised multi-threaded build, impractical at /Od [est].
- **VRA.** Factor and specific forecasts are rescaled by an EWMA of the cross-sectional bias statistic (USE4 §4.3,
  §5.3) [src 1]; Axioma's DVA rescales returns by dispersion trends [src 4; patent via src 33]. **atx has VRA** [code].

### 2.5 Specific risk: time series, structural model, Bayesian shrinkage

USE4: specific variance from the time series of daily specific returns with an EWMA and a Newey-West adjustment; for
IPOs, thin trading and fat-tailed names the forecast is blended with a structural model (ln specific vol regressed
daily on the model's factors, exponentiation correction E0 slightly above 1); blending parameter gamma 1 for clean
histories, 0 for strong violations; then Bayesian shrinkage to size deciles and a specific VRA [src 1 §5.1-5.3].
Axioma: 250-day history, HL 125 / 60, NW 1 day, plus issuer-specific covariance for multiple lines of one issuer [src
4]. Stambaugh (1997) shows histories of unequal length should be combined, not truncated to the common span [src
22]. **atx has the USE4 recipe** with gamma = min(1, h/252) [code]. Issuer-specific covariance is not modelled
(dual-class lines would be priced as independent); low priority for a ~1,850-name decision universe [design].

### 2.6 Shrinkage and random-matrix cleaning of the full matrix

- **Linear shrinkage** to a structured target (constant correlation, single factor, scaled identity) with an
  analytically optimal intensity [src 10 not fetched, src 11]. **Non-linear shrinkage** shrinks each sample eigenvalue
  separately; the analytical formula is "typically 1,000 times faster" than QuEST and handles dimension up to 10,000
  [src 12]; Ledoit-Wolf's review: the analytical strategy is "basically as fast as linear shrinkage" and handles
  N = 10,000 and more, numerical strategies not much above 1,000 [src 13 §3].
- **RMT cleaning.** Rotationally invariant estimators (Bun, Bouchaud, Potters) are consistent cleaners of large
  correlation matrices without structural priors and found superior to earlier methods on financial data [src 15].
- *Fixes:* noise in the sample eigen-spectrum of an N x N matrix. *Data:* T >> 1 returns per name, ideally a common
  window. *Cost at N 5,000:* the sample matrix alone is 2.5e10 flops to form and 1.25e11 to eigendecompose per
  estimate, and 200 MB per session to store (190 GiB for 1,000 sessions) [arith]. **Not a daily production store
  at this N** [design]; usable as a monthly benchmark on a <= 1,000-name liquid sub-universe (P10, section 6 Q14).

### 2.7 Dynamic models at scale

Engle-Ledoit-Wolf DCC-NL combines composite-likelihood DCC with non-linear shrinkage of the correlation-targeting
(intercept) matrix and handles at least 1,000 assets; it is evaluated by the out-of-sample standard deviation of the
global minimum variance portfolio at N 100 / 500 / 1,000 with T = 1,250 daily returns, monthly rebalancing [src 14].
The Ledoit-Wolf review reports that dynamic estimators beat static ones and that an approximate factor model with
DCC-NL residuals (AFM-DCC-NL) was the best overall in their backtests [src 13 §5]. *Cost:* N univariate GARCH fits plus
correlation dynamics per window; the conditional matrix is dense N x N per session (storage as 2.6) [arith]. **Not
adopted for P9** [design]; its lesson that conditional (fast) volatility matters is already carried by VRA [design].

### 2.8 Out-of-sample evaluation: bias statistics and minimum-variance tests

- **Bias statistic** b = SD of z_t = r_t / sigma_hat_{t-1}; 95% band 1 +- sqrt(2/T) for normal returns; rolling-window
  versions and the specific bias on cap-weighted cross-sections [src 1 App. A]. atx implements these [code
  `strategy_risk_model.hpp:250-263`].
- **Eigenfactor and optimised-portfolio bias** are the tests that expose estimation error random books hide [src 1
  §4.2, src 2]; "second-order risk": optimised portfolios acquire exposure to model uncertainty itself [src 19].
- **GMV out-of-sample SD** is the primary criterion for a covariance estimator [src 14 §6.2].
- **A trap to avoid:** ELW pick each investment date's universe from stocks with a complete return history *and a
  complete return future over the next 21 days*, which they call "not a feasible one in real life but commonly
  applied" [src 14 §6.1]; atx's engine `validate_risk_model` v1 likewise renormalises on realised availability
  [code `model_validation.hpp:29-33`]. COV diagnostics follow the atx harness rule instead (missing return counts 0
  and is reported; never renormalise on the future) [code `strategy_risk_model.hpp:339-352`; design].

### 2.9 How optimisers consume the model

The MOSEK cookbook keeps x' V x = ||F^{1/2}' X' x||^2 + ||D^{1/2} x||^2 as two cones: N(K+1) non-zeros instead of
N(N+1)/2, with solve-time gains growing with N [src 21]. Boyd et al.: exploiting the factor model drops complexity from
O(n^3) to O(n k^2); a single core solves a 1,500-asset, 50-factor single-period problem in under half a second [src 20
§4.2, §4.7]. **atx already never densifies** (Woodbury in `FactorModel`, `y = X'w` auxiliaries in the QP, spo's
`X (F (X'w)) + D w`) [code]. Factor alignment problems (risk factors, alpha factors and constraints mutually
misaligned) cause risk under-estimation of optimised portfolios [src 18]; the eigenfactor adjustment is the model-side
remedy [src 1, src 2].

### 2.10 Missing data, IPOs, delistings and a changing universe

| issue | institutional practice | atx today | COV |
|---|---|---|---|
| new factor / thin industry | EM correlation (USE4, Dempster 1977) [src 1 §4.1] | structural class-prior blend, PD scaling [code] | keep [design] |
| IPO / short history | structural specific blend [src 1 §5.1]; combine unequal histories [src 22] | gamma = min(1, h/252) [code] | keep; stamp per-row flag |
| delisting | name leaves the estimation universe at exit | no row after exit; returns guarded (abs log > 1.5) [code `strategy_risk_model.hpp:283-285`] | keep; harness counts a missing return as 0 |
| universe churn | per-date estimation universe, grandfathering (Axioma) [src 4] | per-date: members present at t-1 and t with cap [code `strategy_risk_verb.cpp:930-935`] | keep; never filter by future availability [src 14 caveat] |
| reclassification | PIT industry | `grp_ff49` SIC as of filing, lagged [code] | keep |

### 2.11 Fast array formats

| format | how it is fast | what it lacks for this use | source |
|---|---|---|---|
| NumPy `.npy` | magic `\x93NUMPY`, version bytes, LE header length, header padded so the data start is 64-byte aligned, memory-mappable | one array per file; no index, no hash | [src 27] |
| Arrow IPC / Feather V2 | buffers 8- or 64-byte aligned (64 for SIMD), `ARROW1` magic at both ends, footer with record-batch offsets for random access, zero-copy by pointer arithmetic, LE default | schema machinery and a library dependency for a fixed, small schema; no per-batch content hash | [src 28; Feather = IPC file, src 29 not fetched] |
| HDF5 | chunked layout, compression filters, chunk cache | compressed chunks are decoded, not mapped; performance depends on chunk/cache tuning (a 1 MB -> 3 MB cache gave 1,000x in one case) | [src 30] |
| PNG chunks (design precedent) | 8-byte signature that detects text-mode and transmission corruption; unknown *critical* chunk -> refuse, unknown *ancillary* chunk -> ignore | n/a | [src 31 §5.2, §5.4] |
| mmap caveats | correctness and performance problems for DBMS write paths and larger-than-memory random reads | n/a: COV is read-only, written once, and fits in memory | [src 32; design] |
| tsdb segment (house) | magic, version, offsets, 64-byte blocks, CRC footer | wall-clock field in the header (breaks byte determinism) | [code `segment.hpp:53-71`] |

**What a factored-covariance container needs** [design]: per-session random access (an index), zero-copy f64 arrays
the engine views take as-is (64-byte alignment), ids and a stamp per block, one pin for the whole series, cheap open,
verification that can be deferred per block, refusal of truncated or partially written files, forward compatibility
with critical/ancillary sections, and no wall clock.

---

## 3. Recommended pipeline for atx

### 3.1 Stages (each separately testable)

| stage | content | new or kept | test that isolates it |
|---|---|---|---|
| S0 PIT slice | session t sees returns r_s (over (s-1, s]), exposures, membership, presence and caps at s <= t; estimation universe U_t = members present at t-1 and t with cap_{t-1} > 0 | kept [code] | `CovTruncation.*` (3.3) |
| S1 factor returns | constrained WLS of r_t on X_{t-1}, sqrt(cap_{t-1}) weights, thin styles dropped | kept | `RiskWls.*` (existing) |
| S2a factor covariance | EWMA vol 84 x NW(5, HL 252), correlation 504 + NW 2, structural fill, eigenvalue floor | kept | `RiskEwma.*`, `RiskModel.*` (existing) |
| **S2b eigenfactor adjustment** | MWO on the fully observed block: `eigen_adjust_v2(F_keep, T_eff, M = 64, a = 1.0, seed = mix(recipe seed, session_ns))`, T_eff = Kish count of the correlation EWMA's weights at t; structural rows bordered afterwards with today's PD scaling; flag per block | **new** (engine kernel, no duplicated math) | `CovEigen.*` (brief) |
| S2c VRA | lambda^2 from the post-S2b, pre-VRA prior forecasts (USE4 order: eigen adjustment, then VRA [src 1 §4.2-4.3]) | kept (input changes only under the new recipe) | `CovEigen.VraReadsTheAdjustedPrior` |
| S3 specific risk | TS EWMA + NW, structural blend, decile shrinkage, specific VRA, invariant | kept | existing |
| S4 statistical completion | APCA residual factors (Northfield / Axioma hybrid [src 4, 5]) | **deferred to COV-v2 (P10)**: changes the geometry | - |
| S5 stamp + checks | as_of, cutoff, history_first, recipe hash; F finite on forecast factors and Cholesky-able; D invariant | **new** stamp | `CovContainer.*` |
| S6 write | one block per session into the container, publish-last | **new** | `CovContainer.*`, `CovRecipe.*` |
| S7 diagnostics | bias families random, factor, book (existing) + eigen (eigenportfolios of F_{t-1}), minvar (fully-invested V^{-1}1 over eligible names via Woodbury), optimized (random-alpha V^{-1}alpha books, dollar-neutral, gross 1), QLIKE, MRAD; behind `--bias-families extended` | **new** families | `CovBias.*` |

Recipe `atx-cov-v1` = S0-S3 + S2b on, S4 off; `atx-risk-v1.1` = today, byte-identical [design]. Constants: every
atx-risk-v1.1 constant unchanged; S2b a = 1.0 because USE4 ships the un-amplified simulated adjustment [src 1 §4.2]
(the engine default 1.4 must be overridden explicitly [code `eigen_adjust.hpp:89-92`]); M = 64 (engine default) and
the Kish-count simulated length (engine v2 semantics) [design: neither is given in USE4's text read here]. No
constant is fitted on returns (house rule, `strategy_risk_model.hpp:9-10`).

Why per-session seeding [design]: block t then depends on (F_t, T_eff_t, session_t) only, not on how many draws
earlier sessions consumed, so (i) the truncation property holds for S2b, (ii) the M simulations of one session can run
on a deterministic pool with an ordered reduction, invariant to thread count, (iii) a run starting at a later first
session reproduces S2b exactly whenever F_t agrees.

### 3.2 The no-look-ahead invariant (normative text for the brief and K-P9-12)

**I-1 Inputs.** Block t (the model as of the close of session t, forecasting returns over (t, t+h]) is a deterministic
function of: (a) returns r_s, s <= t; (b) exposures and descriptor fields at s <= t, each field on its own PIT clock
(the fields manifest's `point_in_time: true` and seal); (c) member, present and cap at s <= t; (d) the recipe. Nothing
else: no statistic over the role's whole session axis, no count of sessions, no filter of instruments by data after t
(no "complete future" universe, [src 14 §6.1]), no value that depends on whether an instrument id exists after t.

**I-2 Stamp.** Each block records `as_of_session = sessions[t]`, `data_cutoff_session` (the last session whose data
entered; equal to as_of for atx recipes), `history_first_session` (the first session whose return entered any
estimator), and the container records the recipe SHA-256, role pin and inputs SHA-256.

**I-3 Consumer rule.** Under the house timing (decide after d's mark, fill at d+1's close, first return row d+2;
`strategy_nav_replay.hpp:17-21`) a decision at session d may read block b only if `data_cutoff(b) <= session(d)`; the
replay reads block d exactly. The d-2 lag of fitted rules (DEC-9) does not apply: that lag protects a mapping from
features to *future labels* r(d+2); a covariance uses realised returns, and r_d is known at d's mark [design]. A
recipe for live operation may lag one session (cutoff = d-1) if the close of d is not processed before the decision
deadline; not needed for the replay [design].

**I-4 Reader refusals.** A reader refuses: a block whose cutoff exceeds the requested session; a container whose role
pin differs from the caller's role; a session axis that differs from the replay's (today's `check_axes` semantics); a
recipe or geometry other than the pinned one; an incomplete or truncated file; any hash mismatch; a container whose
last session reaches the seal without an unseal record in its metadata; an unknown required section or array; a major
version above 1.

### 3.3 How tests enforce it

| test | what it does | teeth |
|---|---|---|
| `CovTruncation.BlocksBeforeTheCutAreByteIdentical` | synthetic panel (planted factors, two IPOs, two delistings, membership churn, missing descriptors, thin industry); for cuts c in {63, 252, 300}: container A on the full panel, B on the panel truncated after c; every block t <= c byte-equal and its SHA-256 equal | B is a different file (index, root differ) |
| `CovTruncation.PerturbedFutureLeavesThePastUnchanged` | every value after c replaced (prices, volume, member flips, caps, descriptors), same instrument axis | at least one block after c must differ |
| `CovTruncation.FutureInstrumentsDoNotMoveThePast` | instruments that first appear after c added to the axis; blocks <= c equal after projecting by id (role_index may shift) | - |
| `CovTruncation.DetectsAPlantedLeak` | the same harness run on a deliberately leaky estimator (one style standardised by its full-sample mean) must report the first violating session | proves the harness can fail |
| `CovEigen.SeededPerSession` | S2b at t equal whether the run starts at session 0 or at t's history start with the same F_t; equal at 1 and 4 workers | - |
| `CovContainer.ReaderRefusesAFutureCutoff` | `at_or_before(s)` never returns a block with cutoff > s (property over every s); `exact(s)` refuses a missing s | - |
| consumer side | C1's `VolTarget.TruncationInvariant` (kept) | - |
| real data (root, optional, 0 trials) | `cov-diff --through S` between a full-TRAIN container and one from a role truncated at S | needs a truncated role build |

---

## 4. Binary format `atx.cov-container` 1.0 (file `model.atxcov`)

### 4.1 Conventions

All integers little-endian; floats IEEE-754 binary64 / binary32 little-endian; the writer and reader
`static_assert(std::endian::native == std::endian::little)` as the tsdb segment does [code `segment.hpp:22-23`]. All
padding bytes are zero. No wall-clock field anywhere (byte determinism). Offsets are bytes from the file start.
Alignment: header and every block start on 4,096-byte boundaries (page); every array inside a block starts on a
64-byte boundary (cache line / AVX-512, the Arrow and npy recommendation [src 27, 28]).

### 4.2 File layout

```
[0, 4096)            FileHeader (256 bytes used, rest zero)
[4096, B_end)        BLOCKS: block 0, block 1, ... each 4096-aligned, ascending as_of
[B_end, ...)         SECTION TABLE (64 B per entry, 4096-aligned start)
                     META_JSON, FACTOR_NAMES, DATE_INDEX, BLOCK_HASHES (each 64-aligned; listed in the table)
[file_bytes-64, ..)  Trailer
```

### 4.3 FileHeader (offset 0)

| off | type | field | value / rule |
|---|---|---|---|
| 0 | u8[8] | magic | `89 43 4F 56 0D 0A 1A 0A` ("\x89COV\r\n\x1a\n", the PNG signature pattern [src 31 §5.2]) |
| 8 | u16 | major | 1 (reader refuses > 1) |
| 10 | u16 | minor | 0 (reader accepts any minor of major 1) |
| 12 | u32 | header_bytes | 4096 |
| 16 | u32 | endian_tag | 0x01020304 (disk bytes 04 03 02 01) |
| 20 | u32 | flags | bit0 complete (set by finalize), bit1 exposures are f32, bit2 seal-unsealed (META names the owner); others 0 |
| 24 | u64 | file_bytes | exact file size |
| 32 | u32 | section_count | |
| 36 | u32 | date_count | D |
| 40 | u64 | section_table_offset | 4096-aligned |
| 48 | u32 | factor_count | K = 1 + G + S |
| 52 | u32 | group_count | G (one-hot classification columns) |
| 56 | u32 | dense_count | S (dense exposure columns) |
| 60 | u32 | max_rows | max rows over blocks |
| 64 | i64 | first_session_ns | as_of of block 0 |
| 72 | i64 | last_session_ns | as_of of block D-1 |
| 80 | u8[32] | recipe_sha256 | SHA-256 of META.recipe (canonical JSON) |
| 112 | u8[32] | role_sha256 | the role manifest pin, raw bytes |
| 144 | u8[32] | inputs_sha256 | SHA-256 of META.inputs (fields pin, descriptors used, book-weights pin) |
| 176 | u8[48] | reserved | zero |
| 224 | u8[32] | root_sha256 | section 4.7 |

Factor order is fixed: column 0 market, columns 1..G groups, columns G+1..G+S dense (the `FactorRiskView` order
[code `risk_target.hpp:55-62`] and atx-risk-v1's 0 / 1+slot / 51+style order [code `strategy_risk_model.hpp:77-80`]).

### 4.4 Section table entry (64 bytes)

`u32 kind | u32 flags (bit0 required, bit1 in root) | u64 offset | u64 bytes | u64 count | u32 dtype | u32 align |
u8[24] zero`. dtype codes: 1 u8, 2 u16, 3 u32, 4 u64, 5 i64, 6 f32, 7 f64, 8 utf8. Kinds (ascending, each at most
once): 1 META_JSON (required; canonical JSON: recipe, inputs, producer {exe SHA-256, git SHA, build type}, seal block,
the risk verb's manifest-equivalent provenance, array names), 2 FACTOR_NAMES (required; utf8, NUL-separated, K
names), 3 DATE_INDEX (required), 4 BLOCK_HASHES (required), 5 BLOCKS (required; spans every block). Unknown kind with
bit0 set: refuse; with bit0 clear: ignore (PNG critical / ancillary rule [src 31 §5.4]). Sections may not overlap or
leave [header_bytes, file_bytes - 64).

### 4.5 DATE_INDEX entry (64 bytes, D entries)

`i64 as_of_session_ns | i64 data_cutoff_ns | i64 history_first_ns | u64 block_offset | u64 block_bytes | u32 rows |
u32 priced_rows | u32 role_date_index | u32 flags | u32 unforecast_factors | u32 zero`. Rules: as_of strictly
ascending; cutoff <= as_of; block_offset 4096-aligned, inside BLOCKS, ascending, non-overlapping; priced_rows <=
rows <= max_rows. Flags: bit0 forecast (the legacy `diagnostics.csv` forecast column), bit1 factor_complete (F finite
everywhere), bit2 structural factors present, bit3 eigen-adjusted, bit4 VRA applied.

### 4.6 Block (one per session)

BlockHeader (48 bytes): `u8[8] "COVBLK01" | i64 as_of | i64 cutoff | u32 rows N | u32 priced P | u32 K | u32 G |
u32 S | u32 array_count A`, then the array table `ArrayEntry[A]` (24 bytes each: `u32 id | u32 dtype | u64
offset_in_block | u64 count`; id bit 31 set = optional), zero-padded to the next 64-byte boundary (48 + 11 x 24 =
312 -> 320 bytes in v1.0 [arith]). Arrays (v1.0), each 64-byte aligned:

| id | array | dtype | count | rule |
|---|---|---|---|---|
| 1 | IDS | u64 | N | security id per row |
| 2 | ROLE_INDEX | u32 | N | role instrument index |
| 3 | GROUP | u32 | N | 1..G, 0 = none (the `FactorRiskView` convention; legacy slot = GROUP - 1, 255 when 0) |
| 4 | EXPOSURES | f32 or f64 | N x S | row-major; header flag bit1 says which |
| 5 | SPECIFIC | f64 | N | daily specific variance; finite > 0 for rows < P; NaN allowed for rows >= P |
| 6 | FACTOR_COV | f64 | K x K | row-major daily; NaN where unforecast (finite everywhere iff index flag bit1) |
| 7 (opt) | FACTOR_RETURN | f64 | K | realised over (t-1, t], NaN inactive (bias statistics from the file alone) |
| 8 (opt) | FACTOR_FLAGS | u8 | K | bit0 forecast, bit1 structural |
| 9 (opt) | ROW_FLAGS | u8 | N | bit0 eligible (estimation universe), bit1 structural specific (gamma < 1), bit2 bounded |
| 10 (opt) | DIAG | f64 | 32 | lambda2 factor / specific, bias factor / specific, structural scale, r2, T_eff, ... (names in META) |
| 11 (opt) | EIGEN_GAMMA | f64 | K | S2b gammas, ascending eigenvalue order |

**Row rule (lossless legacy scatter)** [design]: rows = every role instrument with an exposure row, a finite specific
variance or a nonzero style at t; rows [0, P) are the priced names (exposure row and finite positive specific
variance, `has_row` / `priced` [code `strategy_spo.cpp:881-883`, `strategy_risk_target.cpp:31-34`]) in ascending role
index, then the rest in ascending role index. Consequence: `FactorRiskView` over rows [0, P) is zero-copy for GROUP,
SPECIFIC and FACTOR_COV (when factor_complete) and for EXPOSURES when f64; and the dense legacy `RiskSlice` is
rebuilt bit for bit (defaults: slot 255, specific the estimator's quiet NaN, styles 0).

**Exposure dtype** [design]: the `risk` verb writes f32 in v1.0 for both recipes, so the container and the legacy
`style_exposures.f32` are the same bits and the two read paths are provably identical; f64 is a legal writer choice
(zero-copy exposures) for engine-only writers. Precision of f32 on z in [-3, 3]: spacing <= 2.4e-7 [arith].

### 4.7 Integrity

- block_hash[t] = SHA-256 of the block's bytes [offset, offset + block_bytes) (BLOCK_HASHES, 32 bytes each).
- root_sha256 = SHA-256( header[0:224] || section table || META_JSON || FACTOR_NAMES || DATE_INDEX || BLOCK_HASHES ),
  i.e. it commits to every byte of the model through the per-block hashes (a two-level Merkle tree) [design].
- Trailer (64 bytes): `u8[8] "COVEND01" | u64 file_bytes | u8[32] root_sha256 | u8[16] zero`. Absent or mismatched
  trailer = an unfinished write.
- **Identity across builds** = equal recipe, role, inputs and equal BLOCK_HASHES; META (producer) differences are a
  ruled substitution, as manifests' `producer` keys are today [design; DEC-20 practice].
- The directory manifest (section 4.10) records `root_sha256` and the whole-file `file_sha256`; the house pin stays
  the manifest SHA-256.

### 4.8 Read path

1. `atx::tsdb::Mapping::map_file_ro(path, expected_bytes = stat size, max_bytes)` [code `mapping.hpp:26-27`].
2. Header: magic, major, endian tag, flags.complete, file_bytes == mapping size; trailer magic, size and root equal
   the header's.
3. Section table: bounds, alignment, no overlap, required kinds known, dtype x count == bytes.
4. Recompute root (only metadata bytes: ~100 KB at D 1,000 [arith]) and compare with the header and the caller's pin.
5. Index: rules of 4.5.
6. Verification mode: **Full** (SHA-256 every block at open, parallel over blocks on a deterministic pool; default for
   any run that writes a ledgered output) or **Lazy** (a block is hashed on first touch, guarded by a per-block
   atomic flag; for research loops and benches). There is no unverified mode.
7. `view(t)`: block header checks (magic, as_of and counts equal the index, array table in bounds and aligned), then
   spans into the mapping (`std::span<const f64>` etc.; no copy). `at_or_before(s)` / `exact(s)` apply I-4.
8. Lifetime: views borrow the mapping; the reader object owns it (move-only), immutable after open except the lazy
   flags (idempotent atomics). Windows: a mapped file cannot be replaced, so writers only ever create new output
   directories (today's rule, `strategy_risk_verb.cpp:970-972`).

### 4.9 Write path

Streaming writer: create `<dir>/model.atxcov.partial`; reserve the 4 KiB header; append each session's block at the
next 4096 boundary while hashing it; keep only the index and the hashes in memory (O(D)); `finalize()` writes the
section table, META, names, index and hashes, the header (flag complete, root), the trailer; flushes
(`FlushFileBuffers` / `fsync`); renames to `model.atxcov`. Enforced at append: as_of strictly ascending, cutoff <=
as_of, geometry constant, rows ordered by the row rule, priced rows really priced. A refusal mid-run leaves only the
`.partial` (and, as today, no manifest).

### 4.10 Series storage: one container per run, not one file per session

[design] One container per (role, recipe, window) with an index, because: (1) one artifact = one pin, as every store
today (`--risk-model-sha256`); a file per session means D pins or a second manifest layer; (2) one mapping and one
open instead of D opens; per-file open cost on Windows is not negligible [est, unmeasured]; (3) one publish-last
rename makes the series atomic; (4) **prefix property**: the blocks of a run truncated at c are byte-equal to the first
c+1 blocks of the full run, so `cov-diff --through` proves truncation identity on real data by comparing hash tables;
(5) per-block hashes recover what per-file hashes would give (localised corruption, lazy checks). Cost: appending a
session rewrites the file; research runs are batch, and a production daily feed would write monthly segment containers
chained by a small segment list (P10) [design]. Vendors deliver daily models as flat files or a proprietary database
[src 4 "Data format"]; that is a delivery format, not a constraint here.

### 4.11 Sizes [arith]

Per block ~ 8N (ids) + 4N (role index) + 4N (group) + 8N (specific) + 4SN or 8SN (exposures) + N (row flags) + 8K^2
+ ~1 KB, rounded to 4 KiB. K 62, S 11, D 1,000: N 3,000 -> 231 MiB (f32) / 356 MiB (f64); N 5,627 -> 402 MiB (f32) /
641 MiB (f64). Today's store at N 5,627: ~314 MiB. The legacy files are still written beside the container (identity
bridge), so a container-bearing output is ~2x today's disk; acceptable, and mapped pages are resident only when
touched [design].

### 4.12 Load-time targets and how the lane's benchmark measures them

Bench `atx-engine/bench/risk_cov_container_bench.cpp` (google benchmark, auto-globbed into `atx-engine-bench`
[code `atx-engine/bench/CMakeLists.txt:4-10`]); synthetic container K 62, S 11, D 1,000, N 3,000 and 5,627, f32 and
f64; root builds it on the `rel` preset with `ATX_BUILD_BENCH=ON`, runs `--benchmark_filter=CovContainer
--benchmark_repetitions=5`, reports medians, the SHA-256 backend in use, and warm vs first-map (cold cache needs a
standby-list flush with admin rights; optional).

| benchmark | target (Release, this host) [est] |
|---|---|
| `BM_CovOpenLazy` (header, table, root over metadata, index) | <= 2 ms at D 1,000 |
| `BM_CovOpenFull` (all blocks hashed, deterministic pool) | <= 0.5 s for 402 MiB with SHA-NI (single-thread rate also printed) |
| `BM_CovFirstTouch` (lazy verify + view of one block) | <= 1 ms |
| `BM_CovView` (verified block) | <= 5 us |
| `BM_CovWrite` (excluding estimation) | >= 300 MB/s |
| `BM_LegacyStoreOpen` / `BM_LegacyStoreRead` (an emulation of `RiskStore::open` / `read` on the same synthetic data: whole-file SHA-256 of the four payloads, four `ifstream` open + seek + read per row) | reported for the ratio; it is an emulation, labelled as such |

A target missed is reported with the measured number; it does not block the merge (speed is not an identity gate)
unless open-full is slower than the legacy open [design].

---

## 5. Integration

### 5.1 Vol-target and risk-target behind a flag, flag-absent byte-identical

- **Writer flags** (risk verb): `--recipe atx-risk-v1.1|atx-cov-v1` (default `atx-risk-v1.1`), `--emit-container`
  (absent = no container), `--bias-families v1|extended` (default `v1`). All three absent: every output byte as today
  (the manifest included; its `producer` keys differ across builds exactly as they do now) [design].
- **Reader switch**: `spo::RiskStore::open` keeps today's path for any manifest without a `container` key. With the
  key, it maps the container (Full verification, root equal to the manifest's), checks role pin, geometry 62 / 50 /
  11, date count and sessions equal to `diagnostics.csv`, and its forecast flags equal to the index's; `read(d)`
  scatters block d into the dense `RiskSlice` with today's NaN -> 0 covariance counting and non-finite style -> 0
  rules. The legacy payload files are then not hashed (they are not read) [design].
- **Hence no NAV file changes**: vol-target-v1, risk-target-v1 and spo-v1/2/3 read whichever store the cell's
  `--risk-model DIR --risk-model-sha256 SHA` names [code `strategy_nav_v7.cpp:1088-1097`]. The switch is the store
  pin, which is already part of every such cell's registered spec (v8y-prereg:217-218). Existing pins point at
  manifests without the key, so every accepted NAV is untouched by construction.
- **Proof obligations** (brief tests): `CovStore.RiskSliceFromContainerEqualsDirectory` (every d, every field, bit
  for bit, including `nan_covariance_entries`), `CovStore.VolAndRiskTargetRecordsUnchanged` (C1's fixtures, both
  laws, records and daily rows bit for bit), and root's real-data step (brief, "Root verifies" 3).
- **Engine consumers**: `atx::engine::risk::CovModelFile::view(t)` returns a `CovBlockView`; `factor_risk_view(view,
  scratch)` yields a `FactorRiskView` over rows [0, P) (zero-copy except f32 exposures widened into `scratch`).

### 5.2 A mean-variance / max-Sharpe step: what it needs and where it sits

Today's chain [code + plan]: signals -> IC runner composition (D1 rule table) -> shared desired target -> NAV
construction rule (aim-partial-v5 / spo-v3) -> leverage rule (fixed L / risk-target / vol-target) -> EXECUTE.

- **Placement** [design]: a NAV rule of kind `target` (K-P9-7 already lists the kind) between the shared desired
  target and the construction rule: `mv-aim-v1`: desired_d <- gross_normalise(P V_d^{-1} P alpha_d), alpha_d = IC
  sigma_i z_i (Grinold-Kahn, as spo-v2 defines it [code `strategy_spo.hpp:17-28`]), P the dollar-neutral projection,
  V_d from block d. Construction (partial trading toward L_t x target) and leverage (vol-target on the new book) follow
  unchanged. With gross normalisation the risk-aversion scale washes out and only the V^{-1} tilt remains (the engine
  fast path's property [code `risk/README.md:146-159`]).
- **What it needs from COV**: block d's X, F, D for the priced rows (zero-copy `FactorRiskView`); `FactorModel::create`
  + `apply_inverse` (Woodbury, O(N K^2 + K^3)) [code]; optional neutrality rows from `materialize`; the stamp to prove
  block d's cutoff <= d. Cost per decision ~ N K^2 = 2e7 flops at N 5,000 [arith], negligible.
- **Why the eigenfactor adjustment matters here**: a V^{-1} tilt loads exactly on the small eigen-directions the raw
  model under-forecasts [src 1 §4.2, src 2, src 18, src 19].
- **Not in COV**: it needs C3's typed rule registry (wave 3) and a construction cell; default P10, optional lane COV-MV
  in wave 3b (brief, second section). spo-v1/v2 already implement MV with costs on the same store [code]; their v7
  cell outcomes are ledgered and were not opened here; the PM should read them before registering P9-MV.

### 5.3 Evaluation that is legitimate under this sprint's rules

| who | what | trials |
|---|---|---|
| lane | synthetic data only: planted-model bias in band; S2b moves simulated small-eigenfactor bias toward 1 and lowers the true variance of the estimated min-variance portfolio under the planted truth (deterministic seeds, averaged); container round-trip, corruption, truncation suites; bench on synthetic files | 0 |
| root, D-COV (zero-trial diagnostic, between wave 2's merge and any cell that reads COV) | risk verb on the TRAIN role twice (`--recipe atx-risk-v1.1` and `atx-cov-v1`, both `--bias-families extended --emit-container`); prints the families random, factor, eigen, minvar, optimized, QLIKE, MRAD side by side on identical observations; no book family, no NAV, no Sharpe | 0 (reads no book statistic; like AL-COMB's §8 Q5 diagnostic) |
| root, P9-V / P9-MV (proposed cells, COV-2) | book-level comparisons, registered in `p9-prereg.md` before any read | +1 each (cov-plan-amendment.md) |

---

## 6. Open questions and risks (each with a recommended answer)

| # | question / risk | recommendation [design] |
|---|---|---|
| Q1 | Which store do P9 cells read when they read a risk store? | The pinned v8 store stays the default; a cell reads COV only if registered to (COV-2). D-COV is printed before P9-V is registered and its numbers become P9-V's registered predictions. |
| Q2 | Is D-COV a trial? | 0 trials: it reads universe returns for risk-forecast accuracy, no book statistic, and decides nothing by itself. Cost if wrong: N_c +1 (E[max] +0.2%). |
| Q3 | Eigen adjustment scale a | 1.0 (USE4's shipped milder adjustment [src 1 §4.2]); never tuned. Cost if wrong: residual eigen bias (a < optimum) is visible in D-COV, fixed in P10. |
| Q4 | S2b cost on a Debug build | The S2b call sits in `strategy_risk_model.cpp` (/O2 in Debug, `atx-impl/CMakeLists.txt:114-129`) but `eigen_adjust_v2` is compiled in the engine lib at the tree's flags; root measures the `atx-cov-v1` run wall in the identity step and, if > 600 s, builds the risk exe on Release after its v1.1 payload identity (a DEC-11-style ruling). |
| Q5 | f32 exposures in v1.0 | Yes for the risk verb (bit identity with the legacy files); f64 allowed by the format. |
| Q6 | One lane or two | One lane COV (L, wave 2); split by task boundary (tasks 1 + 6 engine, 2-5 impl) only if a second pool is idle. COV-MV is a separate optional wave-3b lane tied to P9-MV. |
| Q7 | Statistical completion (hybrid) | COV-v2 in P10: changes K, so `RiskSlice` (fixed 62) cannot carry it until C3 generalises the slice. |
| Q8 | mv-aim-v1 in P9? | Default P10. If the owner wants the max-Sharpe path tested in P9, fund COV-MV (wave 3b after C3) and register P9-MV; N_c 71. |
| Q9 | OD-3 readability of vol-targeted books | No change in P9 (DEC-3 runbook is fixed). Note for P10: a COV build over the history block (`--unseal OWNER`) registered before the read would make a `--risk-model` NAV readable. |
| Q10 | Ownership | `strategy_risk_model.{hpp,cpp}`, `strategy_risk_verb.cpp`, `tools/equity_strategy_risk.cpp` and the RiskStore section of `strategy_spo.{hpp,cpp}` are unowned in plan §2.2; declare them COV's in wave 2; C3 inherits `strategy_spo.*` in wave 3 (its `atx-impl-core` split moves them). |
| Q11 | Bias JSON bytes | `--bias-families v1` default keeps `bias.json` and the manifest's `bias_harness` block byte-identical. |
| Q12 | Lazy verification hides a corrupt block until first touch | Full is the default for any run that writes a ledgered output; Lazy only for diagnostics and benches. |
| Q13 | Windows file mapping and AV scanning latency | Unmeasured; the bench reports first-map vs warm. A mapped file cannot be replaced: writers create new directories only (already the rule). |
| Q14 | Dense shrinkage / DCC-NL benchmarks | Not stored, not in P9. P10 zero-trial benchmark on a <= 1,000-name liquid sub-universe, monthly, GMV OOS SD [src 14] without forward-looking universe filters. |
| R1 | Two read paths for one store could drift | Bit-identity tests on synthetic data plus root's real-data `cov-diff --legacy` and Y-1 NAV re-run (brief). |
| R2 | S2b changes VRA inputs | Order fixed (S2b, then VRA on the adjusted prior), tested; recipe v1.1 path untouched. |
| R3 | A refusal at a late session voids the run (house rule) | Kept; the prefix property is stated for successful runs only. |
| R4 | f32 / f64 choice breaks zero-copy for exposures | Exposures are copied into scratch when f32 (N x S, ~62K values at N 5,627, microseconds) [arith]. |

---

## 7. What this design does not decide

The PM rules: COV's wave and slot, the P9-V and P9-MV registrations and the new N_c ceiling (COV-2), whether COV-MV
is funded, the Release ruling of Q4, the D-COV trial count, and the G-P10 gate row. The owner decides nothing new
here (OD-P9-3 still governs leverage).

---

## 8. Bibliography

| # | source | quality | used for | read? |
|---|---|---|---|---|
| 1 | Menchero, Orr, Wang (2011), "The Barra US Equity Model (USE4): Methodology Notes", MSCI. https://www.top1000funds.com/wp-content/uploads/2011/09/USE4_Methodology_Notes_August_2011.pdf | primary (vendor) | Table 4.1, §4.1-4.3, §5.1-5.3, App. A-B | yes (text extracted) |
| 2 | Menchero, Wang, Orr (2011), "Eigen-Adjusted Covariance Matrices", MSCI Research Insight. https://www.top1000funds.com/wp-content/uploads/2011/06/Eigenfactor_Adjusted_Covariance_Matrices_May2011.pdf | primary (vendor research) | simulated bias, stability 1.4-1.6, a = 1.4 | yes (text extracted) |
| 3 | Menchero, Wang, Orr (2012), "Improving Risk Forecasts for Optimized Portfolios", Financial Analysts Journal 68(3). https://www.tandfonline.com/doi/abs/10.2469/faj.v68.n3.5 ; SSRN https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2066849 | primary | publication record | search record |
| 4 | Axioma (2016), "Axioma United States Equity Factor Risk Models" (AXUS4 factsheet). https://cdn2.hubspot.net/hubfs/2174119/Return%20Downloads/Factsheet-AXUS4-1.pdf | primary (vendor) | structure, half-lives, NW, APCA, ISC, DVA, delivery | yes (text extracted) |
| 5 | Baig (2005), "Equity Risk Modeling: Innovations in Methods and Best Practices", Northfield. https://www.northinfo.com/Documents/170.pdf | primary (vendor talk) | hybrid "blind factors in residual" | yes (text extracted) |
| 6 | Northfield, "US Fundamental Equity" model page. https://www.northinfo.com/resource-details.php?id=2 | secondary (vendor page) | 67 factors | yes |
| 7 | Newey, West (1987), Econometrica 55(3):703-708. https://doi.org/10.2307/1913610 | primary | HAC | not fetched (cited by 1) |
| 8 | Lo, MacKinlay (1990), "An Econometric Analysis of Nonsynchronous Trading", J. Econometrics 45:181-211. https://www.nber.org/papers/w2960 | primary | nontrading effects | search record (abstract) |
| 9 | Burns, Engle, Mezrich (1998), "Correlations and Volatilities of Asynchronous Data", J. Derivatives 5:7-18 (record: https://fmwww.bc.edu/repec/jof/jforec/jof273.rdf) | primary | asynchronous correlations | citation record only |
| 10 | Ledoit, Wolf (2004), "Honey, I Shrunk the Sample Covariance Matrix", JPM. https://www.econ.uzh.ch/dam/jcr:ffffffff-961c-1dd9-ffff-ffffb4762fbf/honey.pdf | primary | constant-correlation target | not fetched (via 33) |
| 11 | Ledoit, Wolf (2004), "A Well-Conditioned Estimator for Large-Dimensional Covariance Matrices", JMA 88(2):365-411. https://ideas.repec.org/a/eee/jmvana/v88y2004i2p365-411.html | primary | linear shrinkage | search record |
| 12 | Ledoit, Wolf (2020), "Analytical Nonlinear Shrinkage of Large-Dimensional Covariance Matrices", Annals of Statistics 48(5); WP https://ideas.repec.org/p/zur/econwp/264.html | primary | 1,000x faster than QuEST; N to 10,000 | yes (abstract) |
| 13 | Ledoit, Wolf (2022), "The Power of (Non-)Linear Shrinking: A Review and Guide to Covariance Matrix Estimation", J. Financial Econometrics 20(1):187-218. https://www.econ.uzh.ch/dam/jcr:e946b1e3-35e8-4c4f-894f-5f4306bf28a5/jfec_2022.pdf | primary (review) | §3 cost, §5 factor models, AFM-DCC-NL, POET | yes (text extracted) |
| 14 | Engle, Ledoit, Wolf (2019), "Large Dynamic Covariance Matrices", JBES 37(2):363-375. https://www.econ.uzh.ch/dam/jcr:28fa9939-753e-4f5c-932d-945872f30cfd/jbes_2019.pdf | primary | DCC-NL, N 1,000, GMV SD, forward-looking filter caveat | yes (text extracted) |
| 15 | Bun, Bouchaud, Potters (2017), "Cleaning Large Correlation Matrices: Tools from Random Matrix Theory", Physics Reports. https://arxiv.org/abs/1610.08104 | primary (review) | RIE | yes (abstract) |
| 18 | Ceria, Saxena, Stubbs (2012), "Factor Alignment Problems and Quantitative Portfolio Management", JPM 38(2):29-43. https://www.pm-research.com/content/iijpormgmt/38/2/29 | primary | alignment problems | search record (abstract) |
| 19 | Shepard (2009), "Second Order Risk". https://arxiv.org/abs/0908.2455 | primary (preprint) | optimised-portfolio model risk | yes (abstract) |
| 20 | Boyd, Busseti, Diamond, Kahn, Koh, Nystrup, Speth (2017), "Multi-Period Trading via Convex Optimization", Foundations and Trends in Optimization 3(1). https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf | primary | O(n^3) -> O(nk^2); 1,500 assets / 50 factors < 0.5 s | yes (text extracted) |
| 21 | MOSEK ApS, "Portfolio Optimization Cookbook: Factor models". https://docs.mosek.com/portfolio-cookbook/factormodels.html | secondary (vendor docs) | conic factor form, N(K+1) non-zeros | yes |
| 22 | Stambaugh (1997), "Analyzing Investments Whose Histories Differ in Length", JFE 45(3):285-331. https://ideas.repec.org/a/eee/jfinec/v45y1997i3p285-331.html | primary | unequal histories | search record (abstract) |
| 23 | Dempster, Laird, Rubin (1977), "Maximum Likelihood from Incomplete Data via the EM Algorithm", JRSS-B 39(1) | primary | EM for missing factor returns | not fetched (cited by 1) |
| 27 | NumPy, "numpy.lib.format" (NPY format). https://numpy.org/doc/stable/reference/generated/numpy.lib.format.html | primary (spec) | magic, 64-byte header padding, mmap | yes |
| 28 | Apache Arrow, "Arrow Columnar Format". https://arrow.apache.org/docs/format/Columnar.html | primary (spec) | alignment, IPC file, zero-copy | yes |
| 29 | Apache Arrow, "Feather File Format". https://arrow.apache.org/docs/python/feather.html | primary (docs) | Feather V2 = IPC file | not fetched |
| 30 | The HDF Group, "Chunking in HDF5" and "Improving I/O Performance When Working with HDF5 Compressed Datasets". https://support.hdfgroup.org/documentation/hdf5/latest/hdf5_chunking.html ; https://support.hdfgroup.org/documentation/hdf5/latest/improve_compressed_perf.html | primary (docs) | chunk/compression trade-offs | search summary |
| 31 | W3C (2025), "Portable Network Graphics (PNG) Specification (Third Edition)" §5.2, §5.4. https://www.w3.org/TR/png-3/ | primary (spec) | signature, critical / ancillary | yes |
| 32 | Crotty, Leis, Pavlo (2022), "Are You Sure You Want to Use MMAP in Your Database Management System?", CIDR. https://www.cidrdb.org/cidr2022/papers/p13-crotty.pdf | primary | mmap caveats | search record (abstract) |
| 33 | atx-engine/research/covariance-matrix-construction-massive-universe-deep-dive.md (in-repo prior pass) | secondary (internal, 3-vote verified per its legend) | parameter cheat-sheet, Axioma DVA patent | yes |

Numbers 16, 17, 24-26 of the drafting list were dropped (not needed after verification).
