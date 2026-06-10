# Sprint 6 — Cost Calibration & Capacity — Implementation Progress

**Status:** 🚧 OPEN
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
| S6-2  | Temp/perm zero-leakage verification                                     | ✅ done  | pending       | 4/4 (TempPerm) | `cost/temp_perm.hpp`: `simulate_round_trip` (drives the EXISTING sim+market, captures mark before/after fill + on a later no-trade bar) + `fit_split_ratio` (median \|perm\|/\|temp\|, eval::median, order-fixed) + `should_trade` throttle. PROOF only — no impact math (C6); expected values computed in the test from the documented sim formula. C3 leakage identities held: `(mark_after−before)/before == 0.5·γ·σ·part·dir` at 1e-9; `mark_next_bar == mark_after_fill` at 1e-12; `(fill−before)/before == Y·σ·partᵟ·dir` at 1e-9, dominating perm. Buy + sell-side sign both proven. Full fill asserted (`fills.size()==1`) before `part`. |
| S6-3  | κ derivation (turnover penalty from calibrated commission cost)         | 🔲 todo  | —             | —     | |
| S6-4  | Capacity-curve integration (call risk::capacity_curve with fitted cfg)  | 🔲 todo  | —             | —     | |
| S6-5  | Short-borrow accrual (BorrowModel + Portfolio::accrue_financing)        | 🔲 todo  | —             | —     | Uses accrue_financing, NOT a zero-qty fill (see risk (b)). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| e45bd2a | S6-0 | docs(s6-0): open sprint-6 cost-calibration ledger + scaffold + as-built seam verification |
| 6a8cd0c | S6-1 | feat(s6-1): cost-coefficient calibration (IRLS-Huber robust LS + log-linear power-law fit, auditable) |
| pending | S6-2 | feat(s6-2): temp/perm split — zero-mark-leakage proof + split-ratio fit + cost throttle |
