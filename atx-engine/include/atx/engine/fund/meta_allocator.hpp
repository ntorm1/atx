#pragma once

// atx::engine::fund — MetaAllocator: the N-book LIFT of the scalar book::size_book
// (P2-S2-2). The fund-level capital allocator that turns a trailing sleeve-return
// covariance Ω into per-sleeve capital weights c_s.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  book::size_book maps ONE book's (Sharpe, vol, capacity) to a scalar gross via a
//  fractional-Kelly rule. MetaAllocator lifts that to S sleeves: given a trailing
//  sleeve-return covariance Ω (S×S), per-sleeve vols σ = sqrt(diag Ω) and a per-
//  sleeve capacity box, it
//    (1) computes a RISK-BUDGET weight vector w_rb (Σ=1, w>0) by one of three
//        methods — inverse-vol, the strictly-convex log-barrier Equal-Risk-
//        Contribution (ERC, the default), or Hierarchical Risk Parity (HRP);
//    (2) applies a fund-level portfolio-of-books FRACTIONAL-KELLY leverage with a
//        gross-clipped VOL-TARGET, producing capital weights
//          c_s = clip( k·w_rb_s , 0 , caps_s ),   Σ_s |c_s| ≤ max_gross.
//
//  Ω is an INPUT here (built by a later S2 unit). This unit is PURE LINEAR ALGEBRA
//  on a given Ω — it estimates nothing and never peeks at future data.
//
// ===========================================================================
//  The algorithms (load-bearing — the equations ARE the contract)
// ===========================================================================
//  InverseVol (A1):           w_s ∝ 1/σ_s, normalized to Σ=1. The equicorrelation
//    closed form / cheap fallback.
//  EqualRiskContribution (A1, DEFAULT): the Spinu log-barrier ERC
//      min ½ wᵀΩw − Σ_s b_s ln(w_s)   on w > 0,
//    whose stationarity is the budget condition w_s·(Ωw)_s = b_s (Σ b = 1, b > 0;
//    b = cfg.risk_budget if non-empty, else equal 1/S). Solved by cyclical
//    coordinate descent (CCD): per-coordinate closed-form positive root
//      w_s = ( −β + sqrt(β² + 4·Ω_ss·b_s) ) / (2·Ω_ss),   β = Σ_{t≠s} w_t·Ω_st
//    swept ascending s for EXACTLY solve_iters full sweeps (NO residual early-exit,
//    determinism R1), then renormalized to Σ=1.
//  HierarchicalRiskParity (A2, López de Prado 2016): correlation-distance →
//    single-linkage agglomerative tree → quasi-diagonal seriation → recursive
//    bisection with inverse-variance sub-weights and split factor
//    α = 1 − V₁/(V₁+V₂), V = w̃ᵀΩ_sub w̃. NEVER inverts Ω. Normalized to Σ=1.
//
//  Kelly/cap composition (A3/A5): σ_fund = sqrt(w_rbᵀΩw_rb); the vol-target scale
//    k_vol = (target_vol>0 && σ_fund>0) ? target_vol/σ_fund : 1; k = fractional_kelly·k_vol.
//    The gross is then capped FIRST (k_eff = min(k, max_gross/‖w_rb‖₁) so Σ|c| ≤
//    max_gross regardless of the per-sleeve box), THEN the per-sleeve capacity box
//    clips each entry: c_s = clip(k_eff·w_rb_s, 0, caps_s). (Order documented: the
//    gross scale is applied before the box; the box can only shrink an entry, so it
//    preserves Σ|c| ≤ max_gross while enforcing each c_s ≤ caps_s.)
//
// ===========================================================================
//  Degenerate / empty fallback (§0.8)
// ===========================================================================
//  S=0 (empty Ω) ⇒ empty weights. A degenerate Ω (any non-finite entry, or a zero/
//  negative diagonal, or an ERC solve that cannot proceed) ⇒ fall back to inverse-
//  vol (or equal weights if the vols are unusable). NEVER a future-data peek, NEVER
//  a throw — a degenerate covariance returns a sane diversified weight, not an error.
//
// ===========================================================================
//  Determinism (R1)
// ===========================================================================
//  solve_iters is FIXED — the CCD sweep is ascending s and runs the full count with
//  NO convergence early-exit. quad_form, the L1 norm, the budget normalization and
//  every reduction are order-fixed (ascending index). No std::unordered_* in the
//  numeric paths, no RNG, no clock. Two builds on the same inputs yield BYTE-
//  IDENTICAL c (determinism rests on order-fixed reductions only; there is no
//  /fp:precise flag).

#include <span>   // std::span (allocate args)
#include <vector> // std::vector (config budget, output weights)

#include <Eigen/Core> // Eigen::Index (column-major MatX/VecX indexing)

#include "atx/core/error.hpp"         // Result, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen)
#include "atx/core/types.hpp"         // f64, usize, u8

namespace atx::engine::fund {

// ===========================================================================
//  RiskBudgetMethod — the risk-budget weighting scheme (A1/A2).
// ===========================================================================
enum class RiskBudgetMethod : atx::u8 {
  InverseVol,             // w_s ∝ 1/σ_s (equicorrelation closed form / cheap fallback)
  EqualRiskContribution,  // Spinu log-barrier ERC (the default; CCD, fixed iters)
  HierarchicalRiskParity, // HRP (López de Prado 2016; never inverts Ω)
};

// ===========================================================================
//  MetaAllocatorConfig — the fund-level allocator knobs.
// ===========================================================================
struct MetaAllocatorConfig {
  RiskBudgetMethod method = RiskBudgetMethod::EqualRiskContribution; // default allocator (A1)
  // Per-sleeve risk budget b_s for ERC (Σ b = 1, b > 0). EMPTY ⇒ equal b_s = 1/S.
  // Validated at the boundary when non-empty: size S, all > 0, Σ ≈ 1. (A1)
  std::vector<atx::f64> risk_budget;
  atx::f64 fractional_kelly = 0.3; // c: fund-level portfolio-of-books Kelly leverage (A3)
  atx::f64 target_vol = 0.0;       // σ* fund vol target; 0 ⇒ Kelly scale only (A5)
  atx::f64 max_gross = 4.0;        // fund gross cap G: Σ|c| ≤ G (A5)
  atx::usize solve_iters = 64;     // FIXED CCD sweep count for the ERC solve — NO early-exit (R1)
};

// ===========================================================================
//  CapitalWeights — the per-sleeve capital weight c_s (post Kelly + clip).
// ===========================================================================
struct CapitalWeights {
  std::vector<atx::f64> c; // length S; c_s ∈ [0, caps_s], Σ|c| ≤ max_gross
};

// ===========================================================================
//  MetaAllocator — risk-budget + portfolio-Kelly → per-sleeve capital weights.
// ===========================================================================
class MetaAllocator {
public:
  MetaAllocatorConfig cfg;

  // Compute the per-sleeve capital weights from a trailing sleeve-return covariance.
  //   Omega     : S×S trailing sleeve-return covariance (symmetric, ideally SPD).
  //   sleeve_vol: per-sleeve vols (= sqrt(diag Ω); the inverse-vol / fallback input).
  //   caps      : per-sleeve capacity box (c_s clipped to [0, caps_s], §0.3).
  // Order-fixed, fixed-iteration (R1). Validates at the boundary
  //   Omega.rows()==Omega.cols()==S==sleeve_vol.size()==caps.size() and, if
  // cfg.risk_budget is non-empty, that it is size S, all > 0, Σ ≈ 1 — else
  // Err(InvalidArgument). A degenerate (non-finite / non-positive-diagonal) Ω falls
  // back to inverse-vol and still returns Ok (§0.8); S=0 ⇒ Ok with empty weights.
  [[nodiscard]] atx::core::Result<CapitalWeights>
  allocate(const atx::core::linalg::MatX &Omega, std::span<const atx::f64> sleeve_vol,
           std::span<const atx::f64> caps) const;

private:
  // --- risk-budget kernels (each returns w_rb with Σ=1, w>0) ---

  // Inverse-vol: w_s ∝ 1/σ_s, normalized to Σ=1. Non-positive / non-finite σ_s fall
  // back to equal weights (a degenerate vol cannot define a 1/σ tilt). (A1)
  [[nodiscard]] static atx::core::linalg::VecX inverse_vol(std::span<const atx::f64> sleeve_vol);

  // ERC log-barrier (A1): min ½wᵀΩw − Σ b_s ln(w_s). Cyclical coordinate descent,
  // per-coordinate positive root, swept ascending s for EXACTLY `iters` full sweeps
  // (NO early-exit, R1), then renormalized to Σ=1. Warm-started at 1/σ (inverse-vol)
  // when available, else 1/S. `b` is the per-sleeve budget (Σ=1, b>0).
  [[nodiscard]] static atx::core::linalg::VecX
  erc_log_barrier(const atx::core::linalg::MatX &Omega, std::span<const atx::f64> b,
                  atx::usize iters);

  // HRP (A2, López de Prado 2016): correlation-distance → single-linkage tree →
  // quasi-diagonal seriation → recursive bisection (inverse-variance sub-weights,
  // split factor α = 1 − V₁/(V₁+V₂)). NEVER inverts Ω. Normalized to Σ=1.
  [[nodiscard]] static atx::core::linalg::VecX hrp_weights(const atx::core::linalg::MatX &Omega);

  // Dispatch on cfg.method → the chosen risk-budget kernel. `b` is the resolved
  // budget (equal when cfg.risk_budget is empty); sleeve_vol is the warm-start /
  // inverse-vol input.
  [[nodiscard]] static atx::core::linalg::VecX
  risk_budget_weights(RiskBudgetMethod method, const atx::core::linalg::MatX &Omega,
                      std::span<const atx::f64> sleeve_vol, std::span<const atx::f64> b,
                      atx::usize iters);

  // Order-fixed quadratic form wᵀΩw (ascending i then j; the σ_fund² reduction).
  [[nodiscard]] static atx::f64 quad_form(const atx::core::linalg::MatX &Omega,
                                          const atx::core::linalg::VecX &w);

  // Kelly/cap composition (A3/A5): from w_rb and σ_fund, apply the fractional-Kelly
  // × vol-target scale, gross-cap it (Σ|c| ≤ max_gross), then clip each entry to the
  // per-sleeve box [0, caps_s]. Returns the capital-weight vector c.
  [[nodiscard]] static std::vector<atx::f64>
  apply_kelly_caps(const atx::core::linalg::VecX &w_rb, atx::f64 sigma_fund,
                   std::span<const atx::f64> caps, const MetaAllocatorConfig &cfg);

  // True iff Ω is degenerate for the ERC/HRP path: empty-incompatible non-finite
  // entry, or a non-positive diagonal (a zero/negative variance). Triggers the
  // inverse-vol fallback (§0.8).
  [[nodiscard]] static bool is_degenerate(const atx::core::linalg::MatX &Omega) noexcept;
};

} // namespace atx::engine::fund
