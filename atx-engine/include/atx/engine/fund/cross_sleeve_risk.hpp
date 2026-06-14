#pragma once

// atx::engine::fund — Cross-Sleeve Risk (P2-S2-3): aggregate factor exposure, the
// fund factor/specific risk split, and the per-sleeve Euler component risk.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  S2-2's MetaAllocator turns a sleeve-return covariance Ω into per-sleeve capital
//  weights c_s. This unit is the RISK REPORT for the resulting netted fund book
//      W = Σ_s c_s · w_s        (w_s the per-sleeve book, all over the SAME universe M)
//  given the SHARED risk::FactorModel V (one factored X F Xᵀ + D every sleeve sees)
//  and that same S×S sleeve-return covariance Ω. It computes, with NO re-estimation
//  and NO dense M×M materialization (§0.4):
//    (a) sigma_fund   = sqrt(WᵀVW)            — the V-based BOOK vol (FactorModel::risk)
//    (b) factor_var   = risk(W) − WᵀDW        — the factor variance via the identity
//        specific_var = WᵀDW                    (= b_fundᵀ F b_fund; NO F accessor)
//    (c) b_fund       = Xᵀ W                   — the aggregate factor exposure (length K)
//    (d) risk_contrib = Euler RC_s OVER Ω      — Σ_s RC_s = sqrt(cᵀΩc) (the Ω-sleeve vol)
//  plus sleeve_return_cov, which BUILDS Ω from trailing per-sleeve P&L.
//
// ===========================================================================
//  The factor/specific split WITHOUT an F accessor (§0.4 — load-bearing)
// ===========================================================================
//  FactorModel keeps V factored and deliberately exposes NO F / factor_cov() accessor
//  and NEVER a dense M×M V. So we recover the factor variance from the IDENTITY
//      WᵀVW = b_fundᵀ F b_fund + WᵀDW       (b_fund = XᵀW, D the specific variances)
//  ⇒  factor_var = risk(W) − WᵀDW           — both terms are O(MK)/O(M) and accessor-
//  available (risk(W), specific_var()). This is algebraically EXACT (it IS the factor
//  model's definition of V), so factor_var equals the true b_fundᵀ F b_fund without
//  the test (or this unit) ever touching F.
//
// ===========================================================================
//  The Euler component risk is over Ω, NOT over V (R4 — the load-bearing subtlety)
// ===========================================================================
//  The per-sleeve Euler decomposition answers "how much of the SLEEVE-portfolio risk
//  does sleeve s carry", so it is taken over the SLEEVE-return covariance Ω and the
//  capital weights c — NOT over V/W. For the Euler identity Σ_s RC_s = σ to hold
//  EXACTLY the divisor MUST be the Ω-based sleeve-portfolio vol
//      sigma_sleeve = sqrt(cᵀΩc) ,   RC_s = c_s·(Ωc)_s / sigma_sleeve
//  ⇒ Σ_s RC_s = (cᵀΩc)/sqrt(cᵀΩc) = sqrt(cᵀΩc) = sigma_sleeve.
//  Dividing by the V-based sigma_fund instead would BREAK the sum. sigma_fund (V-based
//  BOOK vol) and sigma_sleeve (Ω-based, the RC divisor) are two DISTINCT risk views
//  (plan §4.3): sigma_fund prices the netted instrument book against the shared factor
//  model; sigma_sleeve prices the sleeve-of-sleeves portfolio against Ω. risk_contrib
//  sums to sigma_sleeve. An all-zero c ⇒ sigma_sleeve = 0 ⇒ every RC_s = 0 (the
//  div-by-zero is guarded), matching the W=0 ⇒ sigma_fund = 0 degenerate book.
//
// ===========================================================================
//  Determinism (R1) / no-look-ahead (R2)
// ===========================================================================
//  Every reduction (W accumulation, WᵀDW, XᵀW, cᵀΩc, the pop-variance two-pass) runs
//  ascending-index, order-fixed; no RNG, no clock, no std::unordered_*. sleeve_return_cov
//  consumes ONLY the P&L spans passed — the TRAILING window is the CALLER's slice, so it
//  reads no future data and is STRUCTURALLY PIT-safe (a pure function of its input
//  window). Same inputs ⇒ byte-identical outputs.

#include <span>   // std::span (sleeve books / P&L args)
#include <vector> // std::vector (b_fund, risk_contrib)

#include "atx/core/error.hpp"         // Result, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX (Ω), VecX
#include "atx/core/types.hpp"         // f64

#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel (the SHARED V)

namespace atx::engine::fund {

// ===========================================================================
//  FundRisk — the cross-sleeve risk report for the netted fund book W = Σ c_s w_s.
// ===========================================================================
struct FundRisk {
  // sqrt(WᵀVW) via FactorModel::risk(W) — the V-based BOOK vol (R6). NOT the RC divisor.
  atx::f64 sigma_fund = 0.0;
  // = risk(W) − WᵀDW (= b_fundᵀ F b_fund; recovered WITHOUT an F accessor, §0.4).
  atx::f64 factor_var = 0.0;
  // = WᵀDW, the specific (idiosyncratic) variance (D from FactorModel::specific_var()).
  atx::f64 specific_var = 0.0;
  // Aggregate factor exposure Xᵀ W (length K) (B4).
  std::vector<atx::f64> b_fund;
  // Per-sleeve Euler component risk RC_s OVER Ω (length S); Σ_s RC_s = sqrt(cᵀΩc) (R4).
  std::vector<atx::f64> risk_contrib;
};

// ===========================================================================
//  fund_risk — the cross-sleeve risk of the netted book W = Σ_s c_s w_s.
//
//  sleeve_books : w_s, one span per sleeve, EACH length M == V.n_instruments().
//  c            : per-sleeve capital weights (length S == sleeve_books.size()).
//  V            : the SHARED factored covariance (one X,F,D every sleeve sees).
//  Omega        : the S×S sleeve-return covariance (the SAME Ω the allocator used).
//
//  Validates at the boundary: sleeve_books.size() == c.size() == S; each
//  sleeve_books[s].size() == M; Omega.rows() == Omega.cols() == S. Else
//  Err(InvalidArgument). Order-fixed, no re-estimation, NO dense M×M (§0.4). (R1)
//
//  NaN policy: a NaN in c or sleeve_books PROPAGATES into W and onward to every
//  derived quantity (caller responsibility — this unit does not scrub inputs),
//  mirroring sleeve_return_cov's explicit per-series NaN note below.
// ===========================================================================
[[nodiscard]] atx::core::Result<FundRisk>
fund_risk(std::span<const std::span<const atx::f64>> sleeve_books,
          std::span<const atx::f64> c, const risk::FactorModel& V,
          const atx::core::linalg::MatX& Omega);

// ===========================================================================
//  sleeve_return_cov — the S×S sleeve-return covariance Ω from trailing per-sleeve P&L.
//
//  sleeve_pnl : one P&L series per sleeve. The TRAILING window is the CALLER's job —
//  it passes only ≤t P&L, so this reads no future data (structurally PIT-safe, R2).
//
//  S == sleeve_pnl.size(); S==0 ⇒ Ok(0×0). All series MUST be the same length (else
//  Err(InvalidArgument)). Per sleeve σ_s = sqrt(pop-variance over its non-NaN entries)
//  (an all-NaN or <2-sample series ⇒ σ_s = 0 ⇒ a zero, finite row/col). Off-diagonal
//  Ω(s,t) = pairwise_complete_corr(pnl_s, pnl_t)·σ_s·σ_t (REUSES the shared corr helper —
//  there is no pairwise_complete_cov); diagonal Ω(s,s) = σ_s² (= var, since corr(s,s)=1).
//  Symmetric (compute s≤t, mirror); ascending s,t. (R1/R2)
// ===========================================================================
[[nodiscard]] atx::core::Result<atx::core::linalg::MatX>
sleeve_return_cov(std::span<const std::span<const atx::f64>> sleeve_pnl);

} // namespace atx::engine::fund
