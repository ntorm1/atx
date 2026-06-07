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
| P4-7b   | done   | `4d8f4a9`  | FactorModelBuilder (per-date WLS-bootstrapped-from-OLS estimating X,F,D; reuses combine LW for F); RiskFactorBuilder 8/8/0/0 |
| P4-8    | done   | `a815d0d`  | WeightPolicy neutralization (group-demean + truncation); industry_neutral now LIVE; bit-identical-when-off guard; WeightPolicyNeutralize 9/9/0/0 (existing WeightPolicy 20/20 UNCHANGED). DECAY + FACTOR-neutralize deferred (residuals below) |
| P4-9    | done   | `ef77f8a`  | turnover-penalized risk-aware optimizer (`PortfolioOptimizer`); λ=κ=0 recovers WeightPolicy book ≤1e-9; factored V via `apply_inverse` only; RiskOptimizer 16/16/0/0 (cq fix `e2d4395`: +`<limits>`, λ-scaling comment corrected) |
| P4-10a  | done   | `5d7aa0e`  | capacity curve (`CapacityPoint`, `capacity_curve`); √-impact edge erosion via sim's OWN `ImpactCfg` (one cost surface §0-G); +additive `ExecutionSimulator::impact_cfg()` accessor; RiskCapacity 9/9/0/0 (ExecSim 24/24 unchanged; cq fix `2a42e69`: drop unused `<limits>`/`<cstdint>`) |
| P4-10b  | —      | —          | integration proofs: firewall truncation-invariance + determinism hash + walk-forward combined backtest (orchestrator split of plan-P4-10) |
| P4-10c  | —      | —          | bench (combiner fit / factor build / optimizer solve / walk-forward) |

---

## Phase 4b sprint commits

| Commit    | Unit        | Test counts |
|-----------|-------------|-------------|
| `ac3b26a` | marker (P4b-0) | RiskScaffold 1/1/0/0 |
| `7c87d79` | feat (P4-6)    | RiskExposures 11/11/0/0 |
| `caae1c9` | feat (P4-7a)   | RiskFactorModel 11/11/0/0 |
| `1b76e5b` | fix (P4-7a)    | RiskFactorModel 12/12/0/0 (cq: +`<span>`, +create-time K-stack bound) |
| `4d8f4a9` | feat (P4-7b)   | RiskFactorBuilder 8/8/0/0 |
| `a815d0d` | feat (P4-8)  | WeightPolicyNeutralize 9/9/0/0 (existing WeightPolicy 20/20 unchanged; full engine suite 1552/1552) |
| `ef77f8a` | feat (P4-9)  | RiskOptimizer 16/16/0/0 (Risk\|WeightPolicy 77/77; full engine suite 1568/1568) |
| `e2d4395` | fix (P4-9)   | RiskOptimizer 16/16/0/0 (cq: +`<limits>` include, corrected λ-scaling comment) |
| `5d7aa0e` | feat (P4-10a)| RiskCapacity 9/9/0/0 (ExecSim 24/24 unchanged; Risk 57/57) |
| `2a42e69` | fix (P4-10a) | RiskCapacity 9/9/0/0 (cq: drop unused `<limits>`/`<cstdint>`) |

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

## P4-8 — WeightPolicy neutralization (group-demean + truncation)

`loop/weight_policy.hpp` (the ONE Phase-2 unit P4-8 modifies) wires the two §5.5
neutralization stages the orchestrator scoped to the as-built stateless/loop-local
policy. The §5.5 realized order is
`winsorize → transform → [GROUP-neutralize] → dollar-neutralize → gross-normalize →
[TRUNCATE]`. **The IRON RULE held:** with `industry_neutral=false` + `truncation=0.0`
+ an empty `group_map` (all defaults) NONE of the new branches run, so the output is
BYTE-IDENTICAL to the Phase-2 pipeline — the 20 pre-existing `WeightPolicy` tests pass
UNCHANGED (their file was not touched).

**Config additions** to `struct WeightPolicy`: `industry_neutral` is now WIRED (no
longer the inert asserted-false flag — header doc updated to describe the live
group-demean + the `group_map` requirement); new `atx::f64 truncation = 0.0` (per-name
|w_i| cap in FINAL gross-normalized units; 0 disables; typical 0.01–0.10, Phase-5
calibrates). `to_target_weights` gains ONE defaulted parameter
`std::span<const atx::u32> group_map = {}` so every existing call site + behavior is
unchanged. The Phase-2 `ATX_ASSERT(!industry_neutral)` is REPLACED by
`ATX_ASSERT(!industry_neutral || group_map.size() == n)` (fail-closed on misconfig,
matching the original intent — group_map is REQUIRED + universe-aligned when on).

**GROUP-demean realization (CsDemeanG / §0-H semantics):** after `apply_transform`,
BEFORE the global demean, `group_demean` demeans the dense live buffer WITHIN each
group. DETERMINISTIC, order-fixed: distinct live group ids are enumerated in ASCENDING
order (`std::sort` + `std::unique` over the live ids — NOT an `unordered_map` iteration
into a reduction), and each group's mean is subtracted from its members in ascending-k
order. A NaN name is already excluded (it never enters the dense buffer), so it neither
gets weight nor pollutes a group mean. After group-demean each group sums to ~0, so the
subsequent global demean is a ~no-op (correct). A single group over all names ⇒ the
per-group demean == the global demean (tested == the Phase-2 path).

**TRUNCATE clip-renorm (fixed-iteration, determinism §3.2):** `dense` is
gross-normalized FIRST (so the cap is compared in FINAL weight units — the gross step
runs UNCONDITIONALLY, preserving the bit-identical-when-off path), then
`truncate_renorm` runs a FIXED `kTruncateIters = 8` passes of (clip to ±cap, renorm
Σ|w|→gross_leverage) — NO convergence-dependent early exit. `kTruncateIters` is a named
constant. After the fixed passes (which settle WHICH names bind) a `finalize_truncation`
pass clips HARD to the cap then pours the remaining gross budget onto the UNBINDING
(sub-cap) names ALONE — so for a FEASIBLE cap both invariants hold to rounding
(`|w_i| ≤ cap` AND `Σ|w| == gross_leverage`) WITHOUT a convergence-tolerance dependence,
and a name pins at EXACTLY the cap. No `gross_normalize` follows truncate (it would
rescale the pinned weights over the cap). **Documented degenerate:** an INFEASIBLY-small
cap (`truncation·n_active < gross_leverage`) leaves no unbinding mass to absorb the
deficit, so every name pins at the cap and `Σ|w| < gross_leverage` — the cap wins (never
a div-by-zero; the all-zero buffer is a no-op).

**Bit-identical guard (the critical test):** `StagesOff_ByteIdenticalToPhase2Pipeline`
sweeps 3 signals (incl. NaN holes) × {Rank, ZScore} × {dollar_neutral on/off} × {gross
1.0/2.0/0.5} = 36 configs and asserts `to_target_weights(signal, universe)` (no
group_map, stages off) is `EXPECT_EQ` byte-identical to an INDEPENDENT recomputation of
the Phase-2 winsorize→transform→demean→gross pipeline.

**9 new tests** (`WeightPolicyNeutralize`), verified via `ctest -R WeightPolicy`
(29 total = 20 existing UNCHANGED + 9 new; full engine suite **1552/1552** green):
group-demean each group sums to 0 (even + uneven groups), single-group == global demean,
NaN excluded from the group mean; truncate no |w|>cap & Σ|w|≈gross, outlier pinned at the
cap, infeasible-cap pins-every-name-below-gross degenerate, truncate-disabled==Phase-2;
and the 36-config bit-identical guard. `/W4 /permissive- /WX` clean; `clang-format`
applied; clang-tidy NOT run (per unit constraints).

### Deferred residuals (lifted to ROADMAP at 4b close)

- **DECAY** — a stateless `const WeightPolicy` holds NO signal history, so a faithful
  d-window `TsDecayLinear` TEMPORAL decay cannot live in it: it needs a signal-history
  input or a stateful policy (an architectural change beyond wiring a cross-sectional
  stage). NOT implemented (a ledger residual, not a code stub).
- **FACTOR-neutralize** — the `FactorModel::neutralize` PRIMITIVE already ships + is
  tested in P4-7a (`risk/factor_model.hpp`); only the POLICY WIRING is deferred. The
  `FactorModel` carries no self-describing instrument→universe mapping (`create` takes
  only X,F,D), so wiring it needs the model→universe `instrument_rows` threaded through
  AND pulls the whole `risk/` + combiner include chain into this Phase-2 header. NOT
  implemented here (a ledger residual, not a code stub).

---

## P4-9 — Turnover-penalized risk-aware optimizer (`PortfolioOptimizer`)

`risk/optimizer.hpp` (new, header-only inline, ns `atx::engine::risk`) adds `OptimizerConfig`
(λ `risk_aversion`, κ `turnover_penalty`, L `gross_leverage`, `name_cap`, `dollar_neutral`,
FIXED `max_iters=64`) + `PortfolioOptimizer::solve(span<const f64> alpha, const FactorModel& V,
span<const f64> w_prev) -> Result<vector<f64>>` — the §5.6 book
`max αᵀw − λwᵀVw − κ‖w−w_prev‖₁` s.t. `Σw=0`, `Σ|w|≤L`, `|w_i|≤cap`. The forward decls already
existed in `risk/fwd.hpp` (untouched).

**Solve.** Deterministic FIXED-iteration (`max_iters`, NO convergence early-exit) gradient→prox→project
loop. The smooth target is built WITHOUT ever materializing the M×M V: at **λ>0** it is the analytic
dollar-neutral MV optimum `t = (1/2λ)·P·V⁻¹·P·α` via `FactorModel::apply_inverse` (Woodbury) ONLY
(no forward `Vw`, no dense V; P = mean-subtraction dollar-neutral projection); at **λ=0** there is no
risk term to invert, so the smooth target is the pure-alpha direction `demean(α)` (NaN→0, excluded from
the mean) — a `// SAFETY:`-documented branch that makes pin #1 exact. The target is then PROJECTED onto
`{Σw=0, Σ|w|=L, |w_i|≤cap}` reusing WeightPolicy's `demean`→`gross_normalize` arithmetic; the κ turnover
term is a proximal soft-threshold toward `w_prev` (empty `w_prev` ⇒ materialized once to flat/0). Because
the smooth target is the PROJECTED (gross-normalized) book, the κ=0 surrogate is stationary at the optimum
— so the `w_prev=w*` no-trade boundary is an EXACT fixed point. **Single scratch alloc per solve**
(allocate-once-per-rebalance, WeightPolicy precedent); NO heap inside the iteration loop; O(iters·(MK+K³)).
NO RNG; order-fixed reductions.

**The λ-collapse (documented, spec-acceptable).** Since the `(1/2λ)` scalar washes out under the gross
normalization, the λ effect on the FINAL book is **binary** (off at λ=0, on at λ>0) — only the
λ-independent V⁻¹ directional tilt survives; any two λ>0 give the byte-identical book. This is the
mathematically forced consequence of fixing `Σ|w|=L` on a dollar-neutral book (the MV optimum's direction
is scale-free), NOT a defect: the qualitative spec intent ("risk-aware tilts weight away from high-variance
names") holds and is tested (high-var pair gross 0.5→0.024). The cq comment fix (`e2d4395`) corrected the
header to state this binary effect rather than implying a graduated `(1/2λ)` shrink.

**16 new tests** (`RiskOptimizer`), `16/16` via `ctest -R RiskOptimizer`, `Risk|WeightPolicy` `77/77`,
full engine suite **1568/1568**: λ=κ=0 recovers `gross_normalize(demean(α))` ≤1e-9 (+ L=2 variant +
NaN-hole variant asserting held-out names ==0); κ=0 pure-MV (w_prev-independent, bitwise); raising-λ shrinks
high-variance gross; κ>0 reduces turnover vs κ=0; name-cap never exceeded; Σw=0 & Σ|w|≤L invariants;
determinism (bitwise `EXPECT_EQ` on re-run); boundaries `w_prev=w*`-no-trade, single-name-→0,
cap-below-equal-weight-all-pinned, empty-w_prev==flat; and two dim-mismatch `Err` paths. `/W4 /permissive-
/WX` clean. Reviews: spec ✅ COMPLIANT (independent rebuild; λ-collapse judged spec-acceptable),
code-quality ✅ APPROVED (two Minors fixed in `e2d4395`).

---

## P4-10a — Capacity curve (`capacity_curve` + `CapacityPoint`)

`risk/capacity.hpp` (new, header-only inline, ns `atx::engine::risk`) adds
`struct CapacityPoint { f64 aum; f64 net_edge_bps; }` (matching the pre-existing `fwd.hpp` decl —
untouched) + `capacity_curve(span<const f64> weights, const PanelView& panel,
const exec::ExecutionSimulator& sim, span<const f64> aum_grid) -> vector<CapacityPoint>` — the RenTech
§9.6 capacity report: the AUM at which √-impact erodes net edge to zero. **First slice of the orchestrator's
P4-10 split** (capacity model only; firewall/determinism/walk-forward = P4-10b, bench = P4-10c).

**Model.** `gross_edge_bps = 1e4·mean_{r}(Σ_i w_i·ret_i(r))` (AUM-independent, computed once; `ret` reuses
the P4-6 `detail::step_return`, PIT/structural). Per name at a swept `aum`: `notional_i = aum·|w_i|`,
`shares_i = notional_i/close(0,i)`, `ADV_i = mean close·volume` (adv20), `part_i = shares_i/ADV_i`,
`sigma_i = popstd(ret_i)` (vol60), `temp_i = Y·sigma_i·part_i^δ`. **§0-G ONE COST SURFACE:** Y, δ are read
from `sim.impact_cfg()` — a NEW purely-additive read-only accessor on `ExecutionSimulator` mirroring the
existing `commission_cfg()` (the ONE permitted Phase-2 touch; ExecSim suite 24/24 UNCHANGED, no behavior
change). `cost_bps(aum) = 1e4·Σ_i |w_i|·temp_i`; `net_edge_bps = gross_edge_bps − cost_bps(aum)`. Scoped to
the size-dependent √-impact term ONLY (commission ≈scale-free in bps, slippage excluded by design — RT §9.6
capacity is impact-bounded). Named windows `kCapacityAdvWindow=20`/`kCapacityVolWindow=60` (Phase-5
calibrates); every return-window CLAMPED to `rows()-1` (no OOB under NDEBUG); guards on `price≤0`, `ADV≤0`,
`part/sigma≤0`, NaN/zero weights (no NaN leak). Cold/research-cadence (a few scratch allocs OK). NO RNG;
order-fixed reductions; deterministic.

**9 new tests** (`RiskCapacity`), `9/9` via `ctest -R RiskCapacity` (ExecSim 24/24 unchanged; Risk 57/57;
`/W4 /permissive- /WX` clean): net-edge monotone-non-increasing in AUM + zero-crossing (the capacity point);
AUM→0 ⇒ net≈gross; ONE-cost-surface (a sim with larger `ImpactCfg.Y` ⇒ strictly smaller net edge — proves
it reads the sim's coeff, not a constant); determinism (bitwise `memcmp` re-run); length/order/aum 1:1;
boundaries empty-grid, zero/NaN-weight-excluded, short/1-row-panel degenerate, hand-computed
constant-return-zero-σ ⇒ net==gross. Reviews: spec ✅ COMPLIANT (independent rebuild; accessor additive,
math + §0-G correct), code-quality ✅ (two unused-include Minors fixed in `2a42e69`).

---

## What Phase 4b proves / Next

_(Fill at sprint close.)_
