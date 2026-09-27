# W1-R2 implementation and qualification boundary

Status: source and postimplementation fixtures frozen; no local compilation, test execution, market-data access, empirical simulation or performance claim. Parent owns registration and combined qualification. Pool4 lease/branch retained. Original scope: production swarm W1-R2 and findings R-07/R-08/R-09/R-18.

## Implemented engine contract

`risk/estimator_policy.hpp` defines explicit `LegacyV1` / `EffectiveHistoryV2`. Existing defaults, legacy inline kernels and valid-domain one-day validation arithmetic remain legacy. New heavy code is out of line in `src/risk/{cov_ewma,eigen_adjust,specific_risk,vol_regime}.cpp` (root registers). No local CMake changes.

* Covariance V2 retains actual newest-first session ages, missing-value masks, and observed-pair lag distances. Half-life-specific zero-lag and Bartlett lag moments use the same mean and weighting recipe; lag weights attach to the older pair endpoint. Fast variance uses the separately estimated serial-correlation ratio. Factor correlation has its own half-life; recombination receives a checked PSD floor. Infinity, malformed clocks and resource violations return errors. Missing factor coordinates are NaN in V2 instead of observed zeros.
* Eigen adjustment uses rounded Kish effective observations from the covariance history, amplification 1.4, explicit simulation count/seed, checked symmetry/SPD even for inactive adjustment, and a combined workspace budget. Effective T must identify K for an active simulation. No K-derived synthetic history length. Failed samples return an error, not neutral calibration evidence.
* Specific risk uses EWMA84/NW5 (NW half-life252), log-vol exposure regression with an intercept and exponentiation correction, and cap-weighted size-decile shrinkage with q=.1. Thin assets blend toward structural volatility according to observed effective-history reliability. Missing/degenerate structural fits use a reported observed-population volatility estimate; an entirely unsupported population errors. This history-only reliability rule is a declared model assumption, not a full reproduction of vendor missingness/tail diagnostics. Unknown/nonpositive current market caps are rejected. No executable liquidity/borrow inference is made.
* VRA accepts actual realized returns plus independently recorded prior variances, their strictly earlier availability clocks and optional prior weights. Factor and specific adjustments are separate; specific evidence requires prior cap weights at the shared cleaning boundary. Final-fit APCA/WLS histories never masquerade as earlier forecasts. Absent/insufficient prior evidence either errors (`RequireObservedV1`) or leaves the relevant multiplier at one with `UnavailableUnverified` status (`LeaveUnadjustedUnverifiedV2`). The latter is the explicit V2 policy default, not a calibrated adjustment.
* Fundamental builder V2 records residual dates, forwards cleaning, and checks factor/workspace bounds before exposure allocation. The old statistical-only builder rejects V2 with direction to the hybrid API. Hybrid V2 requires an explicit market factor, finite current exposures and caps; it retains thin assets, trains the APCA panel only on complete histories and reports zero-APCA-loading fallback. Missing current structural identities fail explicitly instead of silently dropping an asset. Existing legacy missing-history filtering is unchanged.

`RiskEstimatorDiagnostics` travels with both `FactorComponents` and the assembled/copyable `FactorModel`. It includes locale-independent bit-preserving policy recipe, effective/simulated observations, separate VRA status/lambdas, prior evidence ID, and fallback counts. The prior evidence ID is caller-bound provenance, not a cryptographic proof of historical availability. The availability inequalities are checked. Model artifact writers selecting V2 must persist this returned metadata with their input/axis identity. No existing application default or unrelated artifact writer was switched in this lane.

## Explicit validation protocol

`validate_risk_model_21d` is separate from the unchanged legacy API. Fixed forecast-date holdings earn the sum of 21 daily arithmetic returns; variance is 21 times daily long-run variance. This documented forecast is not compounded buy-and-hold wealth. Nonzero holdings with missing realized returns make that book observation unavailable; no future-known survivor renormalization occurs.

The scorecard covers equal weight, minimum variance, 100 default deterministic random-budget minimum-variance books, optimized books, minimum-specific-risk factor-mimicking eigenportfolios (K-by-K solve), and forecast-specific-risk deciles. Frozen-exposure GLS residual deciles account for projection leverage in predicted variance. They are ex-post diagnostics and never VRA input records. Stable factor dimension is required for the eigenportfolio series; rank-deficient mimicking systems report unavailable observations.

Full-sample bias and `absolute_bias_deviation` are distinct from MRAD. MRAD uses 12 contiguous 21-session forecast slots; a missing slot invalidates its windows instead of compressing time. The scorecard averages absolute rolling bias deviations over eligible book/window pairs within each cohort. It explicitly disables MRAD for step !=21 and excludes pooled specific-decile observations. QLIKE is z^2-log(z^2)-1; exact zero realizations retain an infinite loss (JSON null plus explicit zero count). Undefined metrics emit null. No calibrated confidence bands are claimed.

Method reference: [MSCI USE4 Methodology Notes, sections4/5 and AppendixA2](https://dmmn26wgpgtie.cloudfront.net/wp-content/uploads/2011/09/23120404/USE4_Methodology_Notes_August_2011.pdf). Recipe differences and diagnostic limitations above are intentional, visible assumptions; empirical acceptance is not inferred from source resemblance.

## Source receipts

* `01d288128e4c821da982d26fedebfb543f774427`: covariance / effective-history eigen kernels.
* `5e228f45fc91d438a21273c9a71406d89fadf233`: prior-forecast VRA / specific-risk kernel.
* `6a0ebbbdfa3fe28363970a4a6db6f01a86d1bfa8`: inactive eigen validation and locale-stable identity.
* `4592f4672521152e28c0889f1dc4b51e48908d22`: builder/provenance integration.
* `c7c3c9cd3318134031263641092b03a9255643a8`: early dimension/workspace guards.
* `ab461e6b1d3a6ad822566f2c3b6d986b6acf5cf0`: 21-session validation.
* `a2d92b2d00245c87c77a9b57e2382d6d72a3f108`: precompile missing-brace repair and stat-fallback distinction.
* `80026ae67b4712f4bc795af41698342d2066f32b`: review correction from full-window deviation to real rolling MRAD.
* `290835794090456b40ae95556facf398b62462ea`: formatting of the four new production CPPs.
* `93552d943cb2d5dd23fec5820abe5babebc15413`, `a6bea5a81f5e459c5a40ade68a3b3f6cbfe4418c`: postimplementation bounded fixtures.

`git diff --check` passed. New CPPs/fixture were parsed by clang-format for formatting only; this is not C++ compilation or runtime evidence. A missing loop brace was found manually and repaired before any build. No failed build is hidden.

## Parent qualification required

Filter `RiskEstimatorV2.*` contains 11 new checks: ten in new `tests/risk/risk_estimator_v2_test.cpp`, one in existing `risk_covariance_integration_test.cpp`. They cover independent scalar weighted/NW oracles, effective simulation length/repeatability, clock rejection/unavailability, explicit separate multipliers, structural thin fallback and decile formula, hybrid retention/preflight, fundamental forwarding/provenance, fixed-book missingness, locale/config identity, and regime-changing rolling MRAD/calendar holes. Existing owning legacy covariance/eigen/VRA/specific/hybrid/validation suites should also qualify the retained paths. Source review does not establish byte parity or runtime pass counts.

Still open: build/header checks and focused runtime; K60/T252 minimum-variance calibration acceptance; regime-shift empirical adjustment acceptance; broad thin-asset empirical coverage; large-universe time/RSS. No large simulation or benchmark was launched. Assumption-bearing outputs and missing prior forecasts must remain visible during any eventual native-data qualification.
