# Sprint 6 — Cost Calibration & Capacity — Implementation Progress

**Status:** ✅ CLOSED — 6 units, 32 tests (CostScaffold 4 · RobustLs 4 · Calibration 5 · TempPerm 4 · Capacity 4 · CostAware 5 · Borrow 3 · CostIntegration 3), full engine suite green (1018/1020; 2 pre-existing `_NOT_BUILT` stubs). One reviewed engine touch (`Portfolio::accrue_financing`, §0.7). Two-stage review (spec + quality) passed on every unit.
**Worktree:** `C:\Users\natha\atx-wt\atx-engine-cost` (dedicated worktree on its own branch — explicit pathspecs only, no push; merges back to `feat/atx-core-stdlib` at close)
**Branch:** `feat/atx-engine-cost`
**Base:** `feat/atx-core-stdlib` @ `e451c5e`
**Started:** 2026-06-09
**Source plan:** [`sprint-6-cost-calibration-capacity.md`](sprint-6-cost-calibration-capacity.md)

---

## Plan adjustments vs. the source plan (the §0 as-built amendment)

The implementation plan's §0 reconciles the spec against reconnaissance of the merged engine. The eight load-bearing corrections:

1. **Coefficients live in exec::ImpactCfg → calibration emits an ImpactCfg (§0.1).** The impact model parameters (Y, δ, γ) are fields of `exec::ImpactCfg` (execution_sim.hpp:150). S6-1 calibration reads fill observations and emits a NEW `exec::ImpactCfg`; it does not introduce a separate "CostModel" config struct. `ExecutionSimulator::impact_cfg()` is the read-only accessor S6 consumes.

2. **Defaults are a literature mix, not a fitted estimate → calibrate per-universe (§0.2).** The as-built defaults `Y=1.0, delta=0.5, gamma=0.314` are documentation placeholders from Appendix A, not fitted to any real fill stream. S6-1 treats them as starting values only; the calibrated estimate must replace them before any risk/capacity call can be trusted.

3. **δ is nonlinear in the raw fill → log-linearize + IRLS on atx-core ols/wls (§0.3).** The square-root impact exponent δ appears nonlinearly in `part^δ`. S6-1 log-linearizes (`log(slippage/σ) = log(Y) + δ·log(part)`) and fits with `atx::core::linalg::wls` (weighted by observation uncertainty). If iterative-reweighted LS is needed for robustness, it is an engine-local IRLS loop over `wls`, consistent with Pattern-B: consume `atx-core` primitives, lift if the primitive is missing.

4. **risk::capacity_curve already exists → extend it (§0.4).** `risk::capacity_curve` (risk/capacity.hpp) is the as-built capacity sweep; it already takes an `exec::ImpactCfg`. S6-4 calls it with the S6-1 calibrated cfg. No new capacity formula is introduced.

5. **κ already exists in OptimizerConfig → derive it, don't invent a field (§0.5).** `risk::OptimizerConfig::turnover_penalty` (optimizer.hpp:91) is the turnover-penalty κ, defaulting to 0.0. S6-3 derives κ from calibrated commission cost and writes it into a fresh `OptimizerConfig`; no new field is added to the risk layer.

6. **Temp/perm split already exists in the simulator → prove zero-leakage (§0.6).** The execution simulator already computes temp impact (folded into fill price) and perm impact (mark shift) as separate terms. S6-2 verifies empirically that the mark is NOT shifted by the temporary component — i.e., zero leakage from temp into perm — via a differential test over the simulator's recorded fill prices.

7. **Borrow accrual is absent → additive accrue_financing (§0.7).** There is no short-borrow cost anywhere in the current engine. S6-5 adds it via `Portfolio::accrue_financing(core::Decimal)` — the ONE reviewed engine touch for the entire S6 sprint. `BorrowModel` computes the daily interest debit; `accrue_financing` applies it.

8. **Fill streams under-charge observed cost → record the gap, not rewrite the sim (§0.8).** If calibrated impact differs from the sim's default coefficients, S6 records the gap in `FitReport` and updates `ImpactCfg` for future backtests. It does NOT retroactively alter past fill records or rewrite simulation history.

---

## Kickoff risks

### (a) Pattern-B robust-LS edge → atx-core L7

Log-linearized OLS via `atx::core::linalg::wls` is available at base `e451c5e`. If the calibration path needs a more robust M-estimator (Huber/bisquare IRLS) for heavy-tailed fill noise, that is an engine-local implementation first, then a Pattern-B L7 lift to `atx-core`. Do NOT block S6-1 on the atx-core promotion; ship engine-local IRLS if needed, file the residual.

### (b) apply_fill(qty=0) ABORTS — S6-5 uses accrue_financing, NOT a zero-qty fill

`Portfolio::apply_fill` (portfolio.hpp:171–174) has three `ATX_ASSERT` preconditions:
- `f.price > Decimal{}` (zero-price guard)
- `f.qty != 0` (zero-qty guard)
- `f.fee >= Decimal{}` (fee sign guard)

A synthetic `qty=0` fee-only fill would compute `notional = Decimal::from_int(0) * price = 0` and debit only the fee from cash — which is the correct semantic for a financing charge. However, `ATX_ASSERT(f.qty != 0)` fires first in a Debug build, aborting the entire test binary before the cash debit occurs.

**DECISION (S6-0):** S6-5 uses the additive `Portfolio::accrue_financing(core::Decimal)` fallback — the ONE reviewed engine touch for S6 — to debit borrow cost directly to cash without requiring a fill quantity. `accrue_financing` will be added to `Portfolio` in S6-5 as a single-line `cash_ -= amount` method (plus the usual `ATX_ASSERT(amount >= Decimal{})` guard). This is architecturally cleaner than abusing the fill path for a non-fill event.

### (c) Shared-branch / explicit-pathspec discipline

This branch (`feat/atx-engine-cost`) is a DEDICATED worktree. Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit to verify only the intended files appear; NEVER touch `atx-core/*` or `atx-tsdb/*`; do not push.

### (d) Dominant risk: a cost model that looks calibrated but peeks / double-counts / leaks temp into the mark

The four silent failure modes that are hardest to catch:
- **Look-ahead leak**: fit window overlaps with the test/apply window → measured R² is inflated, forward performance is not.
- **Double-counting**: temp impact folded into the fill price AND also recorded as a mark shift → overstated permanent cost, understated expected return.
- **Temp/perm leakage**: the mark after a fill includes the temporary component → every subsequent bar's return is biased by the fill; rolling-vol estimates are corrupted.
- **κ mis-scaling**: turnover penalty derived from a per-notional commission applied to a weight-space turnover → the wrong units, the wrong penalty magnitude.

Each S6 unit carries a non-vacuous differential proof test against its relevant failure mode (C4 rule).

---

## Per-unit status

| Unit  | Title                                                                   | Status   | Commit SHA(s) | Tests | Notes |
|-------|-------------------------------------------------------------------------|----------|---------------|-------|-------|
| S6-0  | Marker + ledger + scaffold + as-built seam verification                 | ✅ done  | e45bd2a       | 4/4 (CostScaffold) | `cost/fwd.hpp` + scaffold test + ledger. apply_fill(qty=0) abort finding confirmed; accrue_financing decision recorded. |
| S6-1  | Fill-stream ingestion + impact calibration (Y, δ, γ)                   | ✅ done  | 6a8cd0c       | 9/9 (RobustLs 4 + Calibration 5) | `cost/robust_ls.hpp` (IRLS-Huber) + `cost/calibration.hpp`. As-built API reconciliation: added `CostObs` + `prior` + `calibrate_from_obs` because the single-snapshot `Market` + `FillPayload` carry no per-fill `mark_before` or realized perm-shift, so γ must come from explicit observations (or stay at the prior on the snapshot path). δ-stderr from σ̂²·(XᵀX)⁻¹; Y-stderr via the delta method (Y=exp(β₀)); γ via through-origin σ·p regression. |
| S6-2  | Temp/perm zero-leakage verification                                     | ✅ done  | 3c8e109       | 4/4 (TempPerm) | `cost/temp_perm.hpp`: `simulate_round_trip` (drives the EXISTING sim+market, captures mark before/after fill + on a later no-trade bar) + `fit_split_ratio` (median \|perm\|/\|temp\|, eval::median, order-fixed) + `should_trade` throttle. PROOF only — no impact math (C6); expected values computed in the test from the documented sim formula. C3 leakage identities held: `(mark_after−before)/before == 0.5·γ·σ·part·dir` at 1e-9; `mark_next_bar == mark_after_fill` at 1e-12; `(fill−before)/before == Y·σ·partᵟ·dir` at 1e-9, dominating perm. Buy + sell-side sign both proven. Full fill asserted (`fills.size()==1`) before `part`. |
| S6-3  | Capacity curve wrappers — per-alpha + per-mega-book + capacity_point    | ✅ done  | 9f3a8c5       | 4/4 (Capacity) | `cost/capacity.hpp`: `capacity_for_book`, `capacity_for_alpha` (last-period weights), `capacity_point` (bracket + linear-interp zero-crossing). C6: delegates entirely to `risk::capacity_curve`, zero impact math. Monotonicity guard (ATX_CHECK). Confirmed cap_hi(Y=2.0) < cap_lo(Y=0.5). |
| S6-4  | Cost-aware knobs — calibrated cost → fitness floor + gate max_turnover + κ | ✅ done  | 84223d5       | 5/5 (CostAware) | `cost/cost_aware.hpp`: `round_trip_cost_bps` (rt_frac = 2·(temp+slip_frac)+perm, ·1e4; reuses the SIM's temp=Y·σ·p^δ / perm=0.5·γ·σ·p forms — C6, no second model), `cost_aware_knobs` (κ=rt_bps/1e4; gate.max_turnover = 0.70·h/(h+0.10·rt_bps) clamped to [0.05,0.70], monotone-DECREASING in cost + INCREASING in horizon; fitness_cost_floor=rt_bps) and `cost_adjusted_fitness` (net = fitness − turnover·rt_bps·kFitnessScale, kFitnessScale=0.1). EMIT-ONLY: returns the values; never edits optimizer/gate/fitness. **As-built reconciliation:** Market has no public universe iterator (only `stats(id)` for a known id), so the universe-representative `(ref_participation, ref_sigma)` are EXPLICIT scalars the caller supplies — not a Market handle. Non-vacuous down-rank: raw fitness favors the loser (2.4 ≥ 2.0); cost flips net (winner 1.92 > loser 1.68 at rt=8bps). |
| S6-5  | Short-borrow accrual (BorrowModel + Portfolio::accrue_financing)        | ✅ done  | c4c3ff3       | 6/6 (Borrow 3 + CostIntegration 3) | `cost/borrow.hpp`: `DayCount` (D360/D365/D252, exhaustive switch no `default`), `BorrowModel`, `day_count_denom`, `daily_borrow` (short-notional only: Σ over the explicit universe of \|qty\|·mark for qty<0; f64 sum crosses to exact Decimal ONCE via `from_double(...).value_or(Decimal{})` at the ledger boundary), `accrue_borrow` (ADDITIVE — rides `Portfolio::accrue_financing`, NOT a synthetic fill per the S6-0 finding). **THE ONE REVIEWED ENGINE TOUCH (§0.7):** added `Portfolio::accrue_financing(core::Decimal charge)` — a single additive cash-only mutator (`ATX_CHECK(charge >= Decimal{})`; `cash_ = cash_ - charge`) in the PUBLIC section after `apply_fill`; no other portfolio.hpp method touched. Hand arithmetic: 2,000,000 short notional · 0.05 / 360 ≈ 277.78/day. C4 (hand arithmetic) + §0.7 (additive debit, no position change) + dollar-neutral edge (long-only ⇒ 0). Integration: EndToEnd (calibrate→sim→capacity→gate composes), C5 (byte-identical replay), C2 (truncation-invariant at chain level). Portfolio suite 21/21 unchanged; full suite 1018/1020 (2 pre-existing `_NOT_BUILT` stubs). Bench `cost_bench.cpp` (calibration n∈{500,2000,8000} + IRLS + capacity grid∈{8,32} + borrow) compiles green. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| e45bd2a | S6-0 | docs(s6-0): open sprint-6 cost-calibration ledger + scaffold + as-built seam verification |
| 6a8cd0c | S6-1 | feat(s6-1): cost-coefficient calibration (IRLS-Huber robust LS + log-linear power-law fit, auditable) |
| 3c8e109 | S6-2 | feat(s6-2): temp/perm split — zero-mark-leakage proof + split-ratio fit + cost throttle |
| 9f3a8c5 | S6-3 | feat(s6-3): capacity curve — per-alpha + per-mega wrappers + capacity-point root-find (reuses risk::capacity_curve) |
| 84223d5 | S6-4 | feat(s6-4): cost-aware knobs — calibrated cost → factory fitness floor + gate max_turnover + optimizer kappa |
| c4c3ff3 | S6-5 | feat(s6-5): borrow accrual (additive Portfolio::accrue_financing) + integration proofs + bench |
| _(this commit)_ | close | docs(s6-5): close sprint-6 — 6 units, 32 tests; cost calibration + capacity + borrow shipped |

---

## Exit criteria — met

- **Calibration recovers injected synthetic coefficients** (`Calibration.RecoversInjectedSyntheticCoefficients`: Y/δ/γ within tol) with **auditable fit quality** (`ReportsHonestFitQuality`: R²>0.8, δ-/Y-stderr>0, n_fills exact). C1 ✅
- **Fit/apply firewall, truncation-invariant** (`Calibration.FitOnTrailing_TruncationInvariant` + `CostIntegration.CostModelNeverPeeks_TruncationInvariant`). C2 ✅
- **Zero temporary-cost leakage into the forward mark** (`TempPerm.TemporaryCost_DoesNotLeakIntoForwardMark` + sell-side mirror; mark moves by perm only @1e-9, no growth on the no-trade bar @1e-12). C3 ✅
- **Capacity curve monotone-decreasing + finite capacity point; per-mega runs the calibrated sim** (`Capacity.CurveMonotoneDecreasingInAum`, `CapacityPoint_FiniteOnRealEdgeFixture`, `PerAlpha_FromStreams`, `ReusesSimImpactCfg_NoSecondModel`). ✅
- **Cost-aware fitness down-ranks a high-turnover net-loser** (`CostAware.DownRanksHighTurnoverNetLoser`, non-vacuous: raw favors the loser, cost flips it) + κ/gate scale with cost. C8 ✅
- **Borrow accrual correct + additive** (`Borrow.*`: hand-arithmetic 277.78/day, cash debited with no position change, long-only ⇒ 0). ✅
- **Determinism end-to-end** (`RobustLs.SameData_ByteIdentical`, `CostIntegration.FullChain_ReplaysByteIdentical`). C5 ✅
- **One cost model (C6)** — calibration emits `exec::ImpactCfg`; capacity calls `risk::capacity_curve`; κ reuses `OptimizerConfig.turnover_penalty`; `round_trip_cost_bps` reuses the sim's `temp`/`perm` forms; no second impact formula anywhere. ✅
- **Pattern B (C7)** — IRLS-Huber robust LS shipped engine-local over atx-core `linalg::{ols,wls}`; lift to atx-core L7 recorded below.
- `/W4 /permissive- /WX` + strict-FP clean; one test file per unit; full engine suite green (no regression).

---

## Residuals → ROADMAP backlog

1. **Robust/nonlinear LS (IRLS-Huber) → atx-core L7 promotion.** `cost/robust_ls.hpp` is the engine-local Pattern-B kernel (consumes `atx::core::linalg::wls`). Promote the IRLS-Huber loop to `atx-core` `linalg` once a second consumer appears (the precedent chain: S1 stats_ext→L6, S2 DetPool→L4, S3 sep-CMA→L7, S4 SimHash→atx-core, S5 elastic-net/GBT/HMM→L7).
2. **`alpha::extract_streams` full-impact re-pricing (§0.8).** Stored alpha PnL nets only the PerDollar commission (`turnover_cost_rate = per_dollar_bps/1e4`), not impact/slippage — so library/pool PnL under-charges. S6 did **not** rewrite `streams.hpp` (that would change S3/S5 behavior mid-flight); the cost-honesty improvement is opt-in via the S6-4 knobs. A future unit may re-price streams through the calibrated sim.
3. **The one engine touch — `Portfolio::accrue_financing` (§0.7) — shipped.** Recorded here as the sole mutation of pre-existing engine source in S6 (additive cash-only debit; Portfolio suite 21/21 unchanged). No further action; noted for auditability.

---

## Baton → S7 (Portfolio Construction & Lifecycle)

S6 makes cost **true** and capacity **known**, so S7 can size and allocate the book against a real ceiling:
- the multi-period optimizer consumes the **calibrated κ** (`cost_aware_knobs` → `OptimizerConfig.turnover_penalty`) so turnover is priced honestly;
- the alpha-decay monitor reads the **calibrated cost** to distinguish a decaying alpha from a cost-flooded one;
- capital allocation respects the **per-mega capacity point** (`capacity_point`) S6-3 computes;
- discovery (S3) and the learned combiner (S5) now have a calibrated cost floor (`fitness_cost_floor`) to mine **net-profitable**, not gross-mirage, alphas.

**Open at close:** the Pattern-B robust-LS L7 promotion; the `extract_streams` full-impact re-pricing (§0.8). **Done in the worktree, pending in the main checkout:** the spec file `sprint-6-cost-calibration-capacity.md` Status→✅ and the `sprint-6.md` user-reference are NOT in this worktree (untracked in main) — flip/create them in the main checkout at merge time.
