# Phase 4b — Implementation Progress

**Worktree:** NONE — in-place on the active shared branch (the established engine workflow; Phase 3b/3c/4a worked this way too; `.agents/atx-engine/agent.md` is the authority).
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `3d09371` (the 4a close)
**Started:** 2026-06-06
**Source plan:** [`phase-4-signal-combination-risk-implementation-plan.md`](phase-4-signal-combination-risk-implementation-plan.md)
**Prior progress:** Phase 4a ([`phase-4a-progress.md`](phase-4a-progress.md))

---

## Plan adjustments vs. the source plan

Sprint 4b covers P4-6…P4-10 (factor exposures → factored covariance V=XFXᵀ+D → WeightPolicy neutralization → turnover-penalized optimizer → integration/capacity/close). The source plan §1–§11 is frozen; the **amendment §0 (2026-06-06)** overrides on conflict.

**As-built PanelView exposes OHLCV ONLY** (`open/high/low/close/volume`, newest-first; no `cap`, no `adv` field, no `IndClass`/sector). So P4-6's §5.4 exposures reconcile as follows: **Momentum, Volatility, Beta, Liquidity** (adv20 = mean(close×volume)) are derived purely from OHLCV; **Size (ln cap) and sector dummies require OPTIONAL EXTERNAL inputs** (a per-instrument `cap` span + a `group_map` span — cap is fundamental data outside the price-volume panel; the sector group map is the §0-H `IndClass` map P4-8 also consumes). When those inputs are absent, the corresponding columns are omitted (config-gated). PanelView has no `t`/`window` index — it is a trailing newest-first view, so the §4 `build(panel, t, window)` interface reconciles to a PanelView (row 0 = current date, PIT structural) + a trailing `window` for the per-date factor computations.

**§0-G** one cost surface: P4-9's κ + P4-10's capacity √-impact read the SAME `exec::ExecutionSimulator` (no second cost path).

**§0-H** group-neutralize (P4-8) reuses the DSL `CsDemeanG` per-group-demean semantics + IndClass map.

**P4-7 reuses the Ledoit-Wolf closed-form** already implemented and corrected in P4-4 (`combine/combiner.hpp` `ledoit_wolf_intensity`, canonical 1/T² normalization) for the factor covariance `F`.

**The fit/apply firewall (§3.1)** proven in 4a extends to the factor model (fitted on a trailing window, applied OOS; P4-10 proves truncation-invariance).

Realistic scope for this sprint:

1. **P4-6** — Factor exposures `X`: OHLCV-derived style factors (Momentum, Volatility, Beta, Liquidity; Size + sector dummies config-gated on optional external inputs), cross-sectionally standardized; `StyleFactor`, `FactorModelConfig`, exposure builder.
2. **P4-7** — Factored covariance `V = XFXᵀ + D`: cross-sectional WLS factor regression, Ledoit-Wolf shrunk `F` (reusing P4-4 LW closed-form), specific variance diagonal `D`, Woodbury inverse; `FactorModel`, `FactorModelBuilder`.
3. **P4-8** — `WeightPolicy` neutralization: wire inert `industry_neutral` (group-demean, `CsDemeanG` semantics + IndClass map), add factor-neutralize (`FactorModel::neutralize`), decay, truncation; all stages default no-op so Phase-2 `WeightPolicy` tests stay bit-identical.
4. **P4-9** — Turnover-penalized risk-aware optimizer: `max αᵀw − λwᵀVw − κ‖w−w_prev‖₁`; recovers the `WeightPolicy` dollar-neutral book at λ=κ=0 bit-for-bit; `OptimizerConfig`, `PortfolioOptimizer`; one cost surface via `exec::ExecutionSimulator`.
5. **P4-10** — Integration/capacity/close: fit/apply firewall truncation-invariance across combiner + factor model, whole-layer determinism hash, capacity curve (`CapacityPoint`, `capacity_curve`), walk-forward combined backtest, bench, 4b close.

---

## Per-unit ledger

| Unit    | Status | Commit     | Notes |
|---------|--------|------------|-------|
| P4b-0   | done   | `ac3b26a`  | scaffold + ledger; RiskScaffold 1/1/0/0 |
| P4-6    | done   | `7c87d79`  | factor exposure matrix `X` builder; RiskExposures 11/11/0/0 (see note) |
| P4-7a   | done   | `caae1c9`  | FactorModel factored-V apply-math (risk/apply_inverse/neutralize); RiskFactorModel 12/12/0/0 (cq fix: +`<span>`, +create-time K-stack bound) |
| P4-7b   | done   | `PENDING`  | FactorModelBuilder (per-date WLS-bootstrapped-from-OLS estimating X,F,D; reuses combine LW for F); RiskFactorBuilder 8/8/0/0 |
| P4-8    | —      | —          | — |
| P4-9    | —      | —          | — |
| P4-10   | —      | —          | — |

---

## Phase 4b sprint commits

| Commit    | Unit        | Test counts |
|-----------|-------------|-------------|
| `ac3b26a` | marker (P4b-0) | RiskScaffold 1/1/0/0 |
| `7c87d79` | feat (P4-6)    | RiskExposures 11/11/0/0 |
| `caae1c9` | feat (P4-7a)   | RiskFactorModel 11/11/0/0 |
| `1b76e5b` | fix (P4-7a)    | RiskFactorModel 12/12/0/0 (cq: +`<span>`, +create-time K-stack bound) |
| `PENDING` | feat (P4-7b)   | RiskFactorBuilder 8/8/0/0 |

---

## P4-6 — Factor exposure matrix `X` (the exposure builder)

`risk/exposures.hpp` adds `StyleFactor` (Size, Momentum, Volatility, Beta, Liquidity;
matches `fwd.hpp` `: atx::u8`), `FactorModelConfig`, `ColumnTag`, `ExposureMatrix`,
and `build_exposures(panel, cfg, row, market_cap, group_id) -> Result<ExposureMatrix>`.
Per the §0 amendment, the as-built `PanelView` exposes OHLCV ONLY, so the §5.4
factors split: **Momentum** (`ts_sum(ret,252)−ts_sum(ret,21)`), **Volatility**
(`popstd(ret,60)`), **Beta** (`cov(ret_i,ret_mkt)/var(ret_mkt)` over 252 rows;
`ret_mkt` = equal-weight mean over present instruments), and **Liquidity**
(`ln(adv20)`, `adv20 = mean(close·volume)` over 20 rows) are PANEL-DERIVED from
`ret[r][i] = close(r,i)/close(r+1,i)−1`; **Size** (`ln cap`) and the **sector
dummies** require OPTIONAL EXTERNAL spans (`market_cap`, `group_id`, each length ==
`instruments()` when present, EMPTY = absent). When cap is empty the Size column is
OMITTED (never fabricated); when `group_id` is empty OR `sector_factors` is false,
no sector columns are emitted. PIT is STRUCTURAL: the builder reads only panel rows
`>= row` (`row` 0 = current date; `row` lets P4-7 rebuild `X` at each historical
date). **Drop rule (§3.3):** an instrument with a NaN in ANY EMITTED style column
(after the cap/availability gate) is dropped — it appears in no `instrument_rows`
entry and contributes to no column (sector membership alone does NOT keep a
style-NaN instrument); the z-score runs AFTER the drop over survivors. Each STYLE
column is z-scored cross-sectionally `(v−mean)/popstd`; a single-instrument or
zero-variance column standardizes to **0** (degenerate). Sector dummies (0/1) are
NOT standardized. **Column order (deterministic):** sector dummies first (ascending
group id), then style columns in `StyleFactor` enum order. `ExposureMatrix` carries
`atx::core::linalg::MatX x` (column-major Eigen `M_valid×K`), the ascending
surviving-instrument index list, and the `ColumnTag` column→factor map. `Err` on
`row >= rows()` or a non-empty span whose length != `instruments()`. NO RNG;
order-fixed (ascending row, ascending instrument) reductions; EXHAUSTIVE switch over
`StyleFactor` (no default). Header-only inline (matches the `combine/*.hpp` pattern).
**11 new tests** (`RiskExposures`), verified via
`--gtest_list_tests --gtest_filter=RiskExposures.*`: Size = ln(cap) standardized,
missing-cap drop, standardized-column mean≈0/std≈1, momentum + volatility window
truncation-invariance, sector dummies sum to group size, single-sector all-ones,
1-instrument degenerate-z→0, column order, row-out-of-range error, span-length
mismatch error. `/W4 /permissive- /WX` clean; `11/11` green.

---

## P4-7a — FactorModel (factored covariance V = XFXᵀ + D, apply-math)

`risk/factor_model.hpp` adds `FactorModel` — a Barra-style risk model kept in
FACTORED form `V = X F Xᵀ + D` (`X` M×K exposures, `F` K×K SPD factor covariance,
`D` M specific variances) that applies `V` WITHOUT ever materializing the dense M×M
matrix. **P4-7a is the orchestrator's split of plan-P4-7:** this unit is the value
type + its apply-path math, constructed from a GIVEN (X, F, D); **P4-7b adds
`FactorModelBuilder::build`** (the per-date cross-sectional WLS that ESTIMATES X, F,
D) — a clean marker is left in the header for it (no stub). `FactorModel::create(x,
f, d, fit_begin, fit_end) -> Result<FactorModel>` validates the shapes
(`F` K×K with K==`X.cols()`, `D` length==`X.rows()`==M, `fit_begin < fit_end`),
**floors D** (`d_i ← max(d_i, kSpecificVarFloor)`, `kSpecificVarFloor = 1e-12`) so
D⁻¹ is finite and V is positive-DEFINITE even for a zero-idiosyncratic-variance
instrument, requires `F` SPD via a Cholesky (`Eigen::LLT`; failure → `Err`), and
**precomputes + caches** the whole apply path: `dinv = 1/D`, `F⁻¹` (from F's LLT),
and the K×K **capacitance** `C = F⁻¹ + Xᵀ diag(dinv) X` whose `Eigen::LLT` is held as
a member — so each apply is matvecs + one cached K×K solve, never a refactor, never
an O(M²) materialization. `risk(w)` (`[[nodiscard]] const noexcept`, **genuinely
alloc-free** — manual order-fixed loops over a fixed K-stack `g`-buffer, NOT Eigen
temporaries) returns `(Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²` in O(MK+K²). `apply_inverse(in,
out)` is **Woodbury** `out = D⁻¹in − D⁻¹X C⁻¹ Xᵀ D⁻¹in` (O(MK+K³), small documented
K-sized temporaries, spans mapped to `Eigen::Map`). `neutralize(signal)` residualizes
in place: `s ← s − X (XᵀX + kNeutralizeRidge·I)⁻¹ Xᵀ s` — a TINY ridge
(`kNeutralizeRidge = 1e-10`) keeps a collinear/rank-deficient X solvable; **NaN cells
propagate** ("no opinion" stays NaN; the caller's `WeightPolicy` maps NaN→weight 0).
`fit_begin`/`fit_end` are carried + exposed (the apply-only-after-fit_end firewall is
enforced by the P4-9/P4-10 CALLERS, not here). atx-core API used: `Eigen::LLT<MatX>`
directly for the cached F and C factorizations (Eigen is available transitively via
`linalg.hpp`); `atx::core::linalg::MatX`/`VecX`, `Result`/`Ok`/`Err`. NO RNG;
order-fixed reductions. Header-only inline (matches `combine/*.hpp`). **11 new tests**
(`RiskFactorModel`), verified via `--gtest_list_tests
--gtest_filter=RiskFactorModel.*`: K=1 + K=2 `risk(w)` cross-checked against a
TEST-ONLY dense `V = X F Xᵀ + diag(D)`; `apply_inverse` round-trips both directions
(`V·V⁻¹x ≈ x`, `V⁻¹·Vx ≈ x`, ~1e-9); `neutralize` orthogonality (`Xᵀs ≈ 0`); the
zero-specific-variance boundary (floored → V PD, `apply_inverse` finite, round-trips
within the ill-conditioned tol); fit-window accessors; and four construction `Err`
cases (F dim mismatch, D length mismatch, empty/inverted window, non-SPD F).
`/W4 /permissive- /WX` clean; `11/11` green.

---

## P4-7b — FactorModelBuilder (per-date cross-sectional WLS estimating X, F, D)

`risk/factor_model.hpp` adds `FactorModelBuilder` (at the marker P4-7a reserved) —
the per-date cross-sectional WLS that ESTIMATES `(X, F, D)` from the panel and calls
`FactorModel::create`. **Estimation (§5.4), WLS-bootstrapped-from-OLS**, a FIXED
deterministic two-pass (no convergence loop): **Pass A (OLS)** runs `linalg::ols(X[s],
r[s])` over each window date, accumulates the per-instrument OLS residual series, and
emits an initial specific variance `d0_i = pop-var(u_ols_i)` floored to a tiny ε
(`kBootstrapVarFloor = 1e-12`, so `1/d0_i` is finite when a date fits exactly).
**Pass B (WLS)** runs `linalg::wls(X[s], r[s], 1/d0_i)` over each date, storing the
factor return `f[s]` (a `window×K` series) and the FINAL residual `u[s]`. Per-date
returns are `step_return(panel, s, inst) = close(s)/close(s+1)−1` (the P4-6 helper,
reused via `risk/exposures.hpp`'s `detail::step_return`); a NaN return drops that
(date, instrument) LISTWISE from that date's regression. **atx-core regression API
used: `ols` + `wls`** (the QR-based estimators returning `OlsResult{beta, r2,
residuals}` — `residuals` is the unweighted `y − Xβ`, exactly the specific return);
no hand-rolled solve. `F = LedoitWolf(cov(f over window))` — **REUSED** the CANONICAL
combine LW closed form `combine::detail::ledoit_wolf_intensity(S, centered)` (the 4b
plan mandates ONE canonical LW reused from P4-4; reaching into `combine::detail` is
clean since `combiner.hpp` is header-only) on the column-demeaned `f`-series + its MLE
covariance `S` (divisor T), assembling `F = (1−δ)S + δ·m·I`, `m = tr(S)/K`;
`cfg.factor_cov_shrink ≥ 0` overrides δ with the fixed clamped value. `D_i =
pop-var(u_i over window)` for the instruments in the CURRENT cross-section `X[0]`
(length M, aligned to `X[0].instrument_rows`; `create` floors to `kSpecificVarFloor`).
Final call: `FactorModel::create(X[0].x, F, D, fit_begin=0, fit_end=window)`.

**The §4 `build(panel, t, window)` → row-0 reconciliation:** the as-built `PanelView`
is newest-first with NO absolute `t`, so (like P4-6) `t` collapses to **row 0 = the
current cross-section**; the model is built to apply at the present date and **fit over
the trailing rows `[0, window)`** (`fit_begin=0`, `fit_end=window`; the
apply-after-fit_end firewall is P4-10's to assert). Reconciled signature:
`build(const PanelView&, usize window, span<const f64> market_cap, span<const u32>
group_id) const`. **PIT is STRUCTURAL:** `build_exposures(..., row=s, ...)` and
`step_return` read only rows `≥ s` (the past). `market_cap`/`group_id` are reused for
EVERY `X[s]` (sector is static; **cap-as-static is a documented approximation** — only
a current cap span exists in the as-built world). **Under-determined-date policy:** a
date with `M_s < K` (or a rank-deficient kernel result) is **SKIPPED** (both passes
share the rule), `Err(InvalidArgument)` only if fewer than 2 usable dates (or usable
dates < K) remain — skipping is documented as the recommended choice. **Err paths:**
`window < 2` and `window < K` (T<K) → `Err(InvalidArgument)`; `n_stat_factors > 0` OR
`n_dead_factors > 0` → `Err(NotImplemented, "stat/dead factor rungs are a deferred 4b
residual")` — the advanced PCA/dead-alpha rungs (default 0) are an EXPLICIT deferral,
NOT a silent skip or stub. (The plan wrote `Unimplemented`; the atx-core enum
enumerator is `ErrorCode::NotImplemented`.) COLD path (allocates window scratch);
NO RNG; order-fixed variance/cov reductions; deterministic (fixed two-pass).
Header-only inline. The `build`/`accumulate_ols`/`accumulate_wls`/`factor_covariance`
helpers are each ≤ ~52 lines.

**8 new tests** (`RiskFactorBuilder`), verified `8/8` via
`ctest -R "FactorModelBuilder|RiskFactorBuilder"`: WLS factor-return step recovers
`f_true` on a residual-free `r = X·f_true` (K=2; the `wls` kernel directly); D ==
per-instrument FINAL WLS residual variance (a K=1 single-sector panel with KNOWN
returns whose builder D, read out via `risk`-pairing `risk(e_i−e_j)=D_i+D_j`, matches
an in-test faithful replication of the exact two-pass WLS); the canonical LW oracle
matches the closed form (δ∈[0,1], `(1−δ)S+δmI` SPD); build() end-to-end (synthetic
single-sector panel → usable K=1 `FactorModel`: `risk`/`apply_inverse` finite, K
matches the emitted column, `fit_begin`/`fit_end` = 0/window, a repeat build is
byte-identical); fit-window truncation-invariance (mutating rows beyond `window+1` —
the window's return reach — leaves the model byte-identical); and three Err boundaries
(`window<2`, `window<K` via 3 sectors / window 2, `n_stat_factors>0` →
`NotImplemented`). `/W4 /permissive- /WX` clean; **P4-7a `RiskFactorModel` 12/12 and
P4-6 `RiskExposures` 11/11 NOT regressed** (full 1543-test engine suite green).

---

## What Phase 4b proves / Next

_(Fill at sprint close.)_
