#pragma once

// atx::engine::cost — Cost Calibration & Capacity layer (Sprint S6).
//
// LAYER CONTRACT (C1–C8 cost-fidelity rules)
// ============================================
//
// C1  CALIBRATED-NOT-GUESSED
//     Every impact/commission coefficient is fit to realized fills, not hand-
//     set. Fitting emits a FitReport with R², stderr, and residual quantiles
//     so the caller can judge quality.  CostKnobs derived from the data are
//     always recorded with the window that produced them.
//
// C2  FIT/APPLY FIREWALL
//     Calibration uses a trailing window of past fills; the resulting
//     CostKnobs are applied FORWARD only.  The fit is truncation-invariant:
//     adding future bars cannot change the coefficients for a closed window.
//     No peeking; no look-ahead.
//
// C3  TEMP/PERM SEPARATION
//     Temporary impact (Y·σ·partᵟ) is folded into the fill price only — it
//     reverts as the order completes and leaves no residual in the mark.
//     Permanent impact (0.5·γ·σ·part) shifts the mark linearly in
//     participation rate with zero temp leakage: the mark shift does NOT
//     include the temporary component.  Both terms are calibrated and
//     verified independently.
//
// C4  DIFFERENTIAL CORRECTNESS
//     A test harness injects synthetic (Y, δ, γ) coefficients into the
//     execution simulator, runs fills through the calibration path, and
//     asserts that the recovered CalibratedCost reproduces the injected
//     parameters within a tight tolerance.  This catches mis-specified
//     regression designs before they corrupt live estimates.
//
// C5  DETERMINISM
//     Fits are byte-stable for a given fill window: same fills → same
//     CalibratedCost.  IRLS (if used for δ) is iterative but RNG-free; the
//     iteration count is fixed (matches OptimizerConfig::max_iters discipline
//     from risk/optimizer.hpp).  Capacity-curve replay is fully deterministic.
//
// C6  ONE COST MODEL
//     Calibration produces exactly one exec::ImpactCfg (Y, δ, γ).  Capacity
//     calls risk::capacity_curve with that cfg.  Turnover penalty reuses
//     risk::OptimizerConfig::turnover_penalty (κ); there is one Σ|Δw|
//     turnover definition shared across all consumers.  No second impact
//     formula is introduced anywhere in this layer.
//
// C7  PATTERN B — ROBUST/NONLINEAR LS
//     The δ-exponent fit (log-linearized + IRLS) consumes atx::core::linalg
//     {ols, wls, ridge} (see Seam Map below).  If the required robust-LS
//     primitive is not yet present in atx-core it is an engine-local
//     implementation first, then a Pattern-B L7 lift to atx-core.
//
// C8  COST IS A DECISION INPUT
//     CalibratedCost feeds alpha fitness scoring, signal gates, and the κ
//     derivation.  Capacity is a first-class output, not an afterthought: the
//     capacity_curve output is a CapacityPoint[] that the portfolio lifecycle
//     and risk modules consume directly.
//
// SEAM MAP — existing engine types this layer reuses (as-built, confirmed S6-0)
// ==============================================================================
//
//  exec::ImpactCfg      (exec/execution_sim.hpp:150)
//    { f64 Y=1.0; f64 delta=0.5; f64 gamma=0.314; }
//    CALIBRATION TARGET: S6-1 fits (Y, δ, γ) to fill observations and emits
//    a new ImpactCfg.  S6-2 verifies temp/perm separation via zero-leakage
//    tests using the simulator's accessors impact_cfg() and commission_cfg().
//
//  exec::SlippageCfg    (exec/execution_sim.hpp:140) — f64 bps=5.0
//    Calibrated into CalibratedCost.slippage; feeds round_trip_cost_bps (S6-4).
//  exec::CommissionCfg  (exec/execution_sim.hpp:161) — f64 per_dollar_bps=15.0
//    Read via ExecutionSimulator::commission_cfg() (read-only accessor; the cost
//    layer does not call it as-built — round-trip cost is derived from the
//    calibrated impact + slippage, not the commission).
//
//  exec::FillPayload    (exec/payloads.hpp:138)
//    { InstrumentId id; i64 qty; core::Decimal price; core::Decimal fee;
//      f64 impact; core::time::Timestamp t; }
//    The raw fill stream is the input to S6-1 calibration.
//
//  risk::OptimizerConfig (risk/optimizer.hpp:89)
//    { f64 risk_aversion=1.0; f64 turnover_penalty=0.0; ... }
//    turnover_penalty is κ.  S6-4 derives κ (= round_trip_cost_bps/1e4) from
//    calibrated cost and writes it into a new OptimizerConfig (one-κ, C6).
//
//  risk::CapacityPoint  (risk/capacity.hpp:107)
//    { f64 aum; f64 net_edge_bps; }
//    risk::capacity_curve returns a vector<CapacityPoint>.  S6-3 wraps that
//    call (per-alpha / per-mega) + the capacity-point root-find, and exposes
//    the curve to the portfolio / strategy-sizing layers.
//
//  Portfolio::apply_fill (portfolio/portfolio.hpp:171)
//    PRECONDITION (ATX_ASSERT): f.qty != 0  AND  f.price > 0  AND  f.fee >= 0.
//    CRITICAL: a synthetic qty=0 fee-only fill ABORTS in a Debug build.
//    S6-5 (short-borrow accrual) therefore CANNOT use a zero-qty fill as a
//    pure-fee debit.  The apply_cash path (portfolio.hpp:246) computes
//      notional = Decimal::from_int(f.qty) * f.price
//      cash    -= notional + fee
//    A qty=0 fill would make notional=0 and debit only the fee — which is the
//    correct semantic — but the ATX_ASSERT(f.qty != 0) fires first in Debug.
//    DECISION (recorded S6-0): S6-5 uses the additive
//    Portfolio::accrue_financing(core::Decimal) fallback (the ONE reviewed
//    engine touch for the entire S6 sprint) instead of a synthetic zero-qty
//    fill.  accrue_financing directly debits cash without requiring a qty.
//
//  atx::core::linalg    (atx-core/include/atx/core/linalg/regression.hpp)
//    Namespace: atx::core::linalg  (NOT atx::core::regression).
//    Primitives: ols, wls, ridge — each returns Result<OlsResult>.
//    Check success with .has_value() (tl::expected; no .ok()).
//    Access coefficients via ->beta.
//    S6-1 consumes wls for the log-linearized impact regression.
//
// IMPACT FORMULAE (confirmed against execution_sim.hpp comments)
// ==============================================================
//
//   part  = fillable / ADV                        (participation rate, ∈ [0,1])
//   sigma = realized return volatility (annualised, then scaled per convention)
//
//   Temporary impact (folded into fill price, reverts):
//     temp = Y · σ · part^δ
//
//   Permanent impact (shifts the mark, does NOT revert):
//     perm = 0.5 · γ · σ · part
//
//   Calibration (log-linearized OLS / IRLS for δ):
//     log(fill_slippage / sigma) = log(Y) + δ · log(part)
//
// =============================================================================

namespace atx::engine::cost {

// ---------------------------------------------------------------------------
// Forward declarations — full definitions in their respective unit headers.
// ---------------------------------------------------------------------------

/// Calibrated cost coefficients fit to a trailing fill window.
/// As-built: { exec::ImpactCfg impact; exec::SlippageCfg slippage; FitReport report; }.
/// Full definition: cost/calibration.hpp (S6-1).
struct CalibratedCost;

/// Auditable fit diagnostics emitted alongside a CalibratedCost (C1).
/// As-built fields: { f64 r2_temp; f64 r2_perm; f64 delta_stderr; f64 Y_stderr;
///                    usize n_fills; f64 resid_p95; }.
/// Full definition: cost/calibration.hpp (S6-1).
struct FitReport;

/// The three emitted decision knobs (C8). As-built: { f64 kappa;
/// combine::GateConfig gate; f64 fitness_cost_floor; } — emit-only values the
/// caller feeds into OptimizerConfig.κ / the gate / the factory fitness floor.
/// Full definition: cost/cost_aware.hpp (S6-4).
struct CostKnobs;

/// Short-borrow cost model: { f64 annual_rate; DayCount day_count; }.
/// Accrual is applied via Portfolio::accrue_financing (NOT a zero-qty fill).
/// Full definition: cost/borrow.hpp (S6-5).
struct BorrowModel;

} // namespace atx::engine::cost
