#pragma once

// atx::engine::risk — FactorModel: the FACTORED covariance V = X F Xᵀ + D (P4-7a).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  FactorModel is a Barra-style risk model kept in FACTORED form
//      V = X F Xᵀ + D ,   X : M×K exposures,  F : K×K factor covariance (SPD),
//                          D : M specific (idiosyncratic) variances (diagonal, >0)
//  and applies it WITHOUT ever materializing the dense M×M V (M ≫ K, so a dense
//  V would be O(M²) memory and O(M²) per matvec — defeated here by working in the
//  K-dimensional factor space):
//    risk(w)               = (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²      O(MK + K²)
//    apply_inverse(in,out) = V⁻¹·in via Woodbury               O(MK + K³)
//    neutralize(signal)    = s − X (XᵀX)⁻¹ Xᵀ s  (in place)    O(MK + K³)
//  plus a carried fit window [fit_begin, fit_end).
//
//  P4-7a SCOPE: this unit is the orchestrator's split of plan-P4-7. It stores a
//  GIVEN (X, F, D) and applies it. The per-date cross-sectional WLS that ESTIMATES
//  X, F, D — `FactorModelBuilder::build` — is P4-7b and is ADDED to this same
//  header later (see the marker near the bottom). Nothing here estimates anything.
//
// ===========================================================================
//  Woodbury inverse + the cached capacitance (the WHOLE POINT)
// ===========================================================================
//  V⁻¹·x = D⁻¹x − D⁻¹X (F⁻¹ + Xᵀ D⁻¹ X)⁻¹ Xᵀ D⁻¹x        (Sherman-Morrison-Woodbury)
//  The K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X is SPD (F⁻¹ SPD + Xᵀ D⁻¹ X PSD). It is
//  built ONCE at construction and its Cholesky (Eigen::LLT) is CACHED, so each
//  apply_inverse is two matvecs (Xᵀ·, X·) + one cached K×K solve — never a refactor
//  and never an M×M materialization. dinv = 1/D is cached too.
//
// ===========================================================================
//  The D floor + the neutralize ridge (numerical guards)
// ===========================================================================
//  D floor: d_i ← max(d_i, kSpecificVarFloor). An all-zero specific-variance
//  instrument would make D⁻¹ infinite and V only PSD; flooring keeps D⁻¹ finite and
//  V positive-DEFINITE. kSpecificVarFloor = 1e-12 is far below any real variance.
//  Neutralize ridge: XᵀX (K×K) is singular when X has collinear columns, so
//  neutralize solves (XᵀX + kNeutralizeRidge·I) z = Xᵀs. kNeutralizeRidge = 1e-10
//  is a numerical guard so a collinear exposure block still residualizes; it is far
//  below any real factor-exposure scale and does not bias a full-rank X materially.
//
// ===========================================================================
//  NaN-in-neutralize policy
// ===========================================================================
//  A NaN in the input signal PROPAGATES through neutralize: a "no opinion" cell
//  stays NaN in the output (Xᵀs picks up the NaN, so the whole residual carries it).
//  This is intentional — the caller's WeightPolicy maps a NaN weight to 0; we do not
//  silently fabricate an opinion for a missing cell here.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG. risk() reductions run in canonical ascending order (instrument, then
//  factor). The apply/neutralize matvecs and the cached Cholesky are deterministic
//  given (X, F, D). Same inputs → same outputs.

#include <algorithm> // std::clamp (fixed factor-cov shrink override)
#include <cmath>     // std::isnan (FactorModelBuilder: per-date return drop)
#include <span>      // std::span (risk / apply_inverse / neutralize args)
#include <utility>   // std::move
#include <vector>    // std::vector (FactorModelBuilder cold-path scratch)

#include <Eigen/Dense> // Eigen::LLT, Eigen::Index, Eigen::Map

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/core/linalg/linalg.hpp"     // MatX, VecX (column-major Eigen)
#include "atx/core/linalg/regression.hpp" // ols, wls (per-date factor-return solve)

#include "atx/engine/combine/combiner.hpp" // combine::detail::ledoit_wolf_intensity (canonical LW)
// PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.
// This is the (new in S8.1) risk→cost edge: the robust factor-return path composes a
// √-cap / inverse-specific-variance PRIOR weight with the existing Huber IRLS kernel
// rather than introducing a second engine-local IRLS loop.
#include "atx/engine/cost/robust_ls.hpp" // cost::irls_huber, RobustCfg, RobustFit (S8.1 robust path)
#include "atx/engine/loop/panel_types.hpp" // PanelView (the trailing newest-first panel)
#include "atx/engine/risk/exposures.hpp"   // build_exposures, ExposureMatrix, detail::step_return
#include "atx/engine/risk/fwd.hpp"         // FactorModel / FactorModelBuilder fwd decls

namespace atx::engine::risk {

// Specific-variance floor: d_i ← max(d_i, kSpecificVarFloor) so D⁻¹ is finite and
// V is positive-DEFINITE even for a zero-idiosyncratic-variance instrument.
inline constexpr atx::f64 kSpecificVarFloor = 1e-12;

// Tiny diagonal ridge on XᵀX in neutralize so a collinear (rank-deficient) exposure
// block still residualizes; far below any real factor-exposure scale.
inline constexpr atx::f64 kNeutralizeRidge = 1e-10;

// ===========================================================================
//  FactorModel — factored covariance V = X F Xᵀ + D (apply-math; P4-7a).
// ===========================================================================
class FactorModel {
public:
  // Build a FactorModel from estimated X (M×K), F (K×K SPD), D (M specific
  // variances), and the fit window [fit_begin, fit_end). Validates the shapes,
  // FLOORS D (kSpecificVarFloor), requires F to be SPD (its Cholesky succeeds), and
  // PRECOMPUTES + CACHES everything the apply path needs (dinv, the Cholesky of the
  // K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X). Err on a shape violation, an empty window,
  // or a non-SPD F.
  [[nodiscard]] static atx::core::Result<FactorModel>
  create(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
         atx::usize fit_begin, atx::usize fit_end) {
    if (f.rows() != f.cols() || f.rows() != x.cols()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: F must be K×K with K == X.cols()");
    }
    // Hard create-time bound: risk() accumulates g_k into a fixed K-stack buffer
    // (kMaxFactorsStack) whose only run-time guard is a debug ATX_ASSERT (compiled
    // out under NDEBUG). Rejecting K > kMaxFactorsStack HERE makes that buffer
    // provably un-overrunnable in release without burdening the noexcept apply path.
    if (x.cols() > kMaxFactorsStack) {
      return atx::core::Err(
          atx::core::ErrorCode::InvalidArgument,
          "FactorModel::create: factor count exceeds the risk() stack buffer bound");
    }
    if (d.size() != x.rows()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: D length must equal X.rows() (M)");
    }
    if (fit_begin >= fit_end) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: require fit_begin < fit_end");
    }

    // Floor D so D⁻¹ is finite and V is PD; cache dinv = 1/D.
    atx::core::linalg::VecX dinv(d.size());
    for (Eigen::Index i = 0; i < d.size(); ++i) {
      const atx::f64 di = d[i] < kSpecificVarFloor ? kSpecificVarFloor : d[i];
      d[i] = di;
      dinv[i] = 1.0 / di;
    }

    // F⁻¹ via a Cholesky of F (K×K). A failed LLT means F is not SPD → Err.
    Eigen::LLT<atx::core::linalg::MatX> f_llt(f);
    if (f_llt.info() != Eigen::Success) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: F is not symmetric positive-definite");
    }
    const atx::core::linalg::MatX f_inv =
        f_llt.solve(atx::core::linalg::MatX::Identity(f.rows(), f.cols()));

    // Capacitance C = F⁻¹ + Xᵀ diag(dinv) X (K×K, SPD). Cache its Cholesky.
    const atx::core::linalg::MatX cap = f_inv + x.transpose() * dinv.asDiagonal() * x;
    Eigen::LLT<atx::core::linalg::MatX> cap_llt(cap);
    if (cap_llt.info() != Eigen::Success) {
      // C is SPD in exact arithmetic; a failure here is a degenerate/ill-scaled X.
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "FactorModel::create: capacitance factorization failed");
    }

    return atx::core::Ok(FactorModel(std::move(x), std::move(f), std::move(d), std::move(dinv),
                                     std::move(cap_llt), fit_begin, fit_end));
  }

  [[nodiscard]] atx::usize n_factors() const noexcept {
    return static_cast<atx::usize>(x_.cols()); // K
  }
  [[nodiscard]] atx::usize n_instruments() const noexcept {
    return static_cast<atx::usize>(x_.rows()); // M
  }

  // wᵀ V w computed in factor space: (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i². noexcept and
  // ALLOC-FREE — manual order-fixed loops (ascending i, then k), NOT Eigen
  // temporaries — so it is genuinely the per-rebalance apply path the optimizer can
  // call without heap traffic. g_k is accumulated into a fixed K-stack buffer.
  [[nodiscard]] atx::f64 risk(std::span<const atx::f64> w) const noexcept {
    const Eigen::Index m = x_.rows();
    const Eigen::Index k = x_.cols();
    ATX_ASSERT(w.size() == static_cast<atx::usize>(m));
    ATX_ASSERT(k <= kMaxFactorsStack);

    atx::f64 g[kMaxFactorsStack] = {}; // g_k = Σ_i X(i,k)·w_i
    atx::f64 spec = 0.0;               // Σ_i D_i w_i²
    for (Eigen::Index i = 0; i < m; ++i) {
      const atx::f64 wi = w[static_cast<atx::usize>(i)];
      spec += d_[i] * wi * wi;
      for (Eigen::Index col = 0; col < k; ++col) {
        g[col] += x_(i, col) * wi;
      }
    }
    atx::f64 quad = 0.0; // Σ_k Σ_l g_k F(k,l) g_l
    for (Eigen::Index a = 0; a < k; ++a) {
      for (Eigen::Index b = 0; b < k; ++b) {
        quad += g[a] * f_(a, b) * g[b];
      }
    }
    return quad + spec;
  }

  // V⁻¹·in via Woodbury, O(MK + K³): out = D⁻¹in − D⁻¹X C⁻¹ Xᵀ D⁻¹in, where C is the
  // cached capacitance. The K-sized temporaries (t2,t3) are a small documented
  // apply-path allocation; the M-sized work aliases the spans (no M×M anywhere).
  void apply_inverse(std::span<const atx::f64> in, std::span<atx::f64> out) const {
    const Eigen::Index m = x_.rows();
    ATX_ASSERT(in.size() == static_cast<atx::usize>(m));
    ATX_ASSERT(out.size() == static_cast<atx::usize>(m));

    Eigen::Map<const atx::core::linalg::VecX> in_v(in.data(), m);
    Eigen::Map<atx::core::linalg::VecX> out_v(out.data(), m);

    const atx::core::linalg::VecX t1 = dinv_.cwiseProduct(in_v); // D⁻¹ in        (M)
    const atx::core::linalg::VecX t2 = x_.transpose() * t1;      // Xᵀ D⁻¹ in     (K)
    const atx::core::linalg::VecX t3 = cap_llt_.solve(t2);       // C⁻¹ ·         (K)
    const atx::core::linalg::VecX t4 = x_ * t3;                  // X C⁻¹ ·       (M)
    out_v = t1 - dinv_.cwiseProduct(t4);                         // D⁻¹in − D⁻¹X·  (M)
  }

  // Factor-neutralize a signal IN PLACE: s ← s − X (XᵀX)⁻¹ Xᵀ s, the residual of s
  // on the factor exposures. A tiny ridge (kNeutralizeRidge) on XᵀX keeps a
  // collinear exposure block solvable. NaN cells propagate (see header note).
  void neutralize(std::span<atx::f64> signal) const {
    const Eigen::Index m = x_.rows();
    const Eigen::Index k = x_.cols();
    ATX_ASSERT(signal.size() == static_cast<atx::usize>(m));

    Eigen::Map<atx::core::linalg::VecX> s(signal.data(), m);
    const atx::core::linalg::VecX b = x_.transpose() * s;   // Xᵀ s            (K)
    atx::core::linalg::MatX gram = x_.transpose() * x_;     // XᵀX             (K×K)
    gram.diagonal().array() += kNeutralizeRidge;            // ridge guard
    const atx::core::linalg::VecX z = gram.ldlt().solve(b); // (XᵀX+εI)⁻¹ Xᵀ s (K)
    s.noalias() -= x_ * z;                                  // s − X z         (M)
    ATX_UNUSED(k);
  }

  [[nodiscard]] atx::usize fit_begin() const noexcept { return fit_begin_; }
  [[nodiscard]] atx::usize fit_end() const noexcept { return fit_end_; }

private:
  // Max K we materialize the risk() g-buffer for on the stack. K is the factor count
  // (sector dummies + ≤5 style factors); 256 is far above any realistic factor block
  // yet keeps the buffer tiny. ENFORCED at construction (create() rejects K > this),
  // so risk()'s fixed buffer is provably never overrun; risk()'s ATX_ASSERT is a
  // debug double-check, not the primary guard.
  static constexpr Eigen::Index kMaxFactorsStack = 256;

  FactorModel(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
              atx::core::linalg::VecX dinv, Eigen::LLT<atx::core::linalg::MatX> cap_llt,
              atx::usize fit_begin, atx::usize fit_end)
      : x_{std::move(x)}, f_{std::move(f)}, d_{std::move(d)}, dinv_{std::move(dinv)},
        cap_llt_{std::move(cap_llt)}, fit_begin_{fit_begin}, fit_end_{fit_end} {}

  atx::core::linalg::MatX x_;                   // M×K exposures
  atx::core::linalg::MatX f_;                   // K×K factor covariance (SPD)
  atx::core::linalg::VecX d_;                   // M specific variances (floored, > 0)
  atx::core::linalg::VecX dinv_;                // M elementwise 1/D (cached for Woodbury)
  Eigen::LLT<atx::core::linalg::MatX> cap_llt_; // cached Cholesky of C = F⁻¹ + Xᵀ D⁻¹ X
  atx::usize fit_begin_;
  atx::usize fit_end_;
};

// ===========================================================================
//  FactorModelBuilder — per-date cross-sectional WLS estimating (X, F, D) (P4-7b).
//
//  build(panel, window, market_cap, group_id) ESTIMATES the factored covariance
//  V = X[0] F X[0]ᵀ + diag(D) from a trailing newest-first PanelView, then calls
//  FactorModel::create. The §4 build(panel, t, window) interface reconciles to the
//  as-built PanelView (newest-first, NO absolute t): row 0 IS the current cross-
//  section, so `t` collapses to row 0 and the model is built to apply at the
//  present date, fit over the trailing rows [0, window).
//
//  Estimation (§5.4), a FIXED deterministic two-pass (no convergence loop):
//    Pass A (OLS, equal weights) over each date s in [0, window): per-date factor
//      returns f_ols[s], residuals u_ols[s]. Accumulate u_ols per universe
//      instrument over the window -> an initial specific variance d0_i = var(u_ols_i)
//      (floored to a tiny ε). This BOOTSTRAPS the WLS weights from an OLS pass.
//    Pass B (WLS, weights 1/d0_i) over each date: factor returns f[s] (a window×K
//      series), residuals u[s].
//    F = LedoitWolf(cov(f over window)) — the CANONICAL combine LW closed form
//      (REUSED from combine::detail::ledoit_wolf_intensity; the 4b plan mandates one
//      canonical LW). cfg.factor_cov_shrink >= 0 overrides δ with the fixed value.
//    D_i = var(u_i over window) per CURRENT cross-section instrument (X[0]).
//
//  PIT is STRUCTURAL: build_exposures(...,row=s,...) and step_return read only rows
//  >= s (the past); no future bar can enter. market_cap / group_id are reused for
//  EVERY X[s] (sector is static; cap-as-static is a documented approximation — the
//  as-built world exposes only a CURRENT cap span). Under-determined dates (M_s < K)
//  are SKIPPED (not fatal) as long as >= 2 usable dates remain. The PCA / dead-alpha
//  rungs (cfg.n_stat_factors / n_dead_factors, DEFAULT 0) are a deferred 4b residual
//  -> Err(NotImplemented), NOT a silent skip. Err if window < 2, window < K (T < K),
//  too few usable dates, or FactorModel::create rejects (non-SPD F / dim / K bound).
//
//  COLD path (allocates window scratch). atx-core kernels for ALL regression; the
//  only hand math is the order-fixed variance/cov reductions. NO RNG; deterministic.
// ===========================================================================
namespace detail {

// One date's cross-section returns over the surviving instruments of `xm`:
// r_i = step_return(panel, s, inst) for inst in xm.instrument_rows. Any NaN return
// drops that (date, instrument) listwise — the kept rows are reported via `keep`
// (indices into xm.instrument_rows) so the caller can sub-select X[s]'s rows too.
[[nodiscard]] inline atx::core::linalg::VecX date_returns(const PanelView &panel, atx::usize s,
                                                          const ExposureMatrix &xm,
                                                          std::vector<atx::usize> &keep) {
  keep.clear();
  keep.reserve(xm.instrument_rows.size());
  std::vector<atx::f64> vals;
  vals.reserve(xm.instrument_rows.size());
  for (atx::usize j = 0U; j < xm.instrument_rows.size(); ++j) {
    const atx::f64 r = step_return(panel, s, xm.instrument_rows[j]); // close(s)/close(s+1)−1 (P4-6)
    if (!std::isnan(r)) {
      keep.push_back(j);
      vals.push_back(r);
    }
  }
  atx::core::linalg::VecX out(static_cast<Eigen::Index>(vals.size()));
  for (atx::usize j = 0U; j < vals.size(); ++j) {
    out[static_cast<Eigen::Index>(j)] = vals[j];
  }
  return out;
}

// Sub-select the rows of `x` named by `keep` (the listwise-kept instruments of a
// date). Keeps all K columns. Deterministic (keep is ascending by construction).
[[nodiscard]] inline atx::core::linalg::MatX select_rows(const atx::core::linalg::MatX &x,
                                                         const std::vector<atx::usize> &keep) {
  atx::core::linalg::MatX out(static_cast<Eigen::Index>(keep.size()), x.cols());
  for (atx::usize j = 0U; j < keep.size(); ++j) {
    out.row(static_cast<Eigen::Index>(j)) = x.row(static_cast<Eigen::Index>(keep[j]));
  }
  return out;
}

// Population variance of a value series over the window (order-fixed two-pass mean
// then sum-of-squares). 0 or 1 samples -> 0.0 (FactorModel::create floors D anyway).
[[nodiscard]] inline atx::f64 pop_variance(const std::vector<atx::f64> &xs) noexcept {
  const atx::usize n = xs.size();
  if (n < 2U) {
    return 0.0;
  }
  atx::f64 sum = 0.0;
  for (const atx::f64 v : xs) {
    sum += v;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (const atx::f64 v : xs) {
    const atx::f64 d = v - mean;
    ss += d * d;
  }
  return ss / static_cast<atx::f64>(n);
}

// The K×K Ledoit-Wolf shrunk covariance of a T×K factor-return series. REUSES the
// CANONICAL combine LW intensity (combine::detail::ledoit_wolf_intensity) on the
// column-demeaned series + its MLE covariance S (divisor T) — the SAME inputs that
// helper is derived for. F = (1−δ)·S + δ·m·I, m = tr(S)/K. cfg_shrink >= 0 overrides
// δ with the fixed value (clamped). Order-fixed demean; SPD by the shrinkage toward
// m·I (FactorModel::create re-checks via its Cholesky).
[[nodiscard]] inline atx::core::linalg::MatX factor_covariance(atx::core::linalg::MatX fseries,
                                                               atx::f64 cfg_shrink) {
  const Eigen::Index t = fseries.rows();
  const Eigen::Index k = fseries.cols();
  for (Eigen::Index c = 0; c < k; ++c) { // column-demean (each factor-return series)
    const atx::f64 mean = fseries.col(c).mean();
    fseries.col(c).array() -= mean;
  }
  const atx::core::linalg::MatX s =
      (fseries.transpose() * fseries) / static_cast<atx::f64>(t); // MLE cov (divisor T)
  const atx::f64 m = s.trace() / static_cast<atx::f64>(k);
  const atx::f64 delta = (cfg_shrink >= 0.0)
                             ? std::clamp(cfg_shrink, 0.0, 1.0)
                             : combine::detail::ledoit_wolf_intensity(s, fseries); // canonical LW
  atx::core::linalg::MatX f = (1.0 - delta) * s;
  f.diagonal().array() += delta * m;
  return f;
}

// S8.1 robust-prior weight over a date's KEPT instruments. The robust IRLS uses
// w0_i as the FIXED prior weight (multiplied by the per-iteration Huber factor):
//   * cap_weight && caps present ⇒ √-cap weighting from the exposures helper,
//     w0_i = √(cap_i) / mean_j √(cap_j) (down-weights mega-caps without letting a few
//     names dominate; flat caps reproduce Ones). Where that helper FLAGS a row (a
//     non-positive cap, returned as 0.0) we substitute the 1/d0_i fallback so a single
//     bad cap cannot zero a row out.
//   * otherwise ⇒ w0_i = 1/d0_i, the SAME inverse-specific-variance weight the P4 WLS
//     pass uses (so the robust fit keeps the WLS character, adding ONLY the Huber
//     down-weighting).
// `keep` indexes into `xm.instrument_rows`; d0 is indexed by the universe instrument.
// The √-cap math itself lives in exposures::detail::sqrt_cap_weight (the spec's reuse
// home); only the builder-specific 1/d0 composition stays here. Order-fixed.
[[nodiscard]] inline atx::core::linalg::VecX
robust_prior_weight(const ExposureMatrix &xm, const std::vector<atx::usize> &keep,
                    const atx::core::linalg::VecX &d0, std::span<const atx::f64> market_cap,
                    bool cap_weight) {
  const Eigen::Index nk = static_cast<Eigen::Index>(keep.size());
  // Universe-instrument index of each kept row (the order sqrt_cap_weight expects).
  std::vector<atx::usize> kept_instrument_rows;
  kept_instrument_rows.reserve(keep.size());
  for (const atx::usize j : keep) {
    kept_instrument_rows.push_back(xm.instrument_rows[j]);
  }
  atx::core::linalg::VecX w0(nk);
  if (cap_weight && !market_cap.empty()) {
    const atx::core::linalg::VecX cap_w = sqrt_cap_weight(market_cap, kept_instrument_rows);
    for (atx::usize j = 0U; j < kept_instrument_rows.size(); ++j) {
      const atx::f64 cw = cap_w[static_cast<Eigen::Index>(j)];
      w0[static_cast<Eigen::Index>(j)] =
          (cw > 0.0)
              ? cw // valid √-cap weight (helper guarantees strictly positive)
              : 1.0 / d0[static_cast<Eigen::Index>(kept_instrument_rows[j])]; // flagged ⇒ 1/d0
    }
    return w0;
  }
  for (atx::usize j = 0U; j < kept_instrument_rows.size(); ++j) {
    w0[static_cast<Eigen::Index>(j)] = 1.0 / d0[static_cast<Eigen::Index>(kept_instrument_rows[j])];
  }
  return w0;
}

} // namespace detail

class FactorModelBuilder {
public:
  FactorModelConfig cfg;

  // Estimate (X, F, D) over the trailing `window` cross-sections and assemble the
  // FactorModel. COLD path. PIT-structural (reads only rows >= each date). See the
  // header block above for the full estimation contract. `[[nodiscard]] const`.
  [[nodiscard]] atx::core::Result<FactorModel> build(const PanelView &panel, atx::usize window,
                                                     std::span<const atx::f64> market_cap,
                                                     std::span<const atx::u32> group_id) const {
    if (cfg.n_stat_factors > 0U || cfg.n_dead_factors > 0U) {
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                            "FactorModelBuilder::build: stat/dead factor rungs are a deferred "
                            "4b residual"); // NOT a silent skip — an explicit deferral
    }
    if (window < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: require window >= 2");
    }
    // X[0] (the CURRENT cross-section) defines M and the emitted factor count K.
    ATX_TRY(ExposureMatrix x0, build_exposures(panel, cfg, /*row=*/0U, market_cap, group_id));
    const atx::usize k = x0.n_factors();
    if (k == 0U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: no factor columns emitted");
    }
    if (window < k) { // T < K -> the factor-return cov is rank-deficient
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: window < factor count (T < K)");
    }

    // Pass A (OLS) -> bootstrap specific variances d0; Pass B (WLS) -> f[s], u[s].
    const atx::usize n_inst = panel.instruments();
    atx::core::linalg::VecX d0(static_cast<Eigen::Index>(n_inst));
    ATX_TRY(atx::usize used_a, accumulate_ols(panel, window, market_cap, group_id, k, d0));
    if (used_a < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: too few usable dates (M_s < K everywhere)");
    }

    std::vector<std::vector<atx::f64>> u_by_inst(n_inst); // final residuals per universe inst
    atx::core::linalg::MatX fseries(static_cast<Eigen::Index>(window),
                                    static_cast<Eigen::Index>(k));
    // Pass B dispatch: robust root-cap + Huber IRLS when opted in (cfg.cov), else the
    // P4 plain inverse-specific-variance WLS. Both emit the SAME (window×K) factor-
    // return series + per-instrument residuals downstream; the default keeps P4 exactly.
    ATX_TRY(atx::usize used_b,
            cfg.cov.robust_regression
                ? accumulate_robust(panel, window, market_cap, group_id, d0, fseries, u_by_inst)
                : accumulate_wls(panel, window, market_cap, group_id, d0, fseries, u_by_inst));
    if (used_b < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: too few usable WLS dates");
    }
    // Compact fseries to the rows actually filled (under-determined dates skipped).
    const atx::core::linalg::MatX fkept = fseries.topRows(static_cast<Eigen::Index>(used_b));
    if (fkept.rows() < static_cast<Eigen::Index>(k)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder::build: usable dates < K (factor cov rank)");
    }

    const atx::core::linalg::MatX f = detail::factor_covariance(fkept, cfg.factor_cov_shrink);
    const atx::core::linalg::VecX d = specific_variances(x0, u_by_inst);
    return FactorModel::create(std::move(x0.x), f, d, /*fit_begin=*/0U, /*fit_end=*/window);
  }

private:
  // ε floor for the bootstrap weights so 1/d0_i is finite for a zero-residual
  // instrument (a date with M_s==K fits exactly -> 0 OLS residual). Far below any
  // real return variance, so it never tilts a well-populated instrument's weight.
  static constexpr atx::f64 kBootstrapVarFloor = 1e-12;

  // Pass A: per date s, build X[s], read the date returns (listwise NaN-drop), run
  // OLS (skip an under-determined date M_s < K), accumulate the per-instrument OLS
  // residual series; emit d0_i = floored pop-var of that series. Returns the count
  // of usable dates. A linalg kernel failure on a usable date is non-fatal (the date
  // is skipped) — only a build_exposures failure (a bad span) propagates.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_ols(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                 std::span<const atx::u32> group_id, atx::usize k,
                 atx::core::linalg::VecX &d0_out) const {
    const atx::usize n_inst = panel.instruments();
    std::vector<std::vector<atx::f64>> resid(n_inst); // OLS residual series per universe inst
    std::vector<atx::usize> keep;
    atx::usize used = 0U;
    for (atx::usize s = 0U; s < window; ++s) {
      ATX_TRY(ExposureMatrix xs, build_exposures(panel, cfg, s, market_cap, group_id));
      const atx::core::linalg::VecX r = detail::date_returns(panel, s, xs, keep);
      if (keep.size() < k) {
        continue; // under-determined cross-section (M_s < K) -> skip this date
      }
      const atx::core::linalg::MatX xsr = detail::select_rows(xs.x, keep);
      const auto fit = atx::core::linalg::ols(xsr, r);
      if (!fit) {
        continue; // rank-deficient date -> skip (e.g. a degenerate sector block)
      }
      ++used;
      for (atx::usize j = 0U; j < keep.size(); ++j) {
        resid[xs.instrument_rows[keep[j]]].push_back(fit->residuals[static_cast<Eigen::Index>(j)]);
      }
    }
    for (atx::usize i = 0U; i < n_inst; ++i) {
      const atx::f64 v = detail::pop_variance(resid[i]);
      d0_out[static_cast<Eigen::Index>(i)] = (v < kBootstrapVarFloor) ? kBootstrapVarFloor : v;
    }
    return atx::core::Ok(used);
  }

  // Pass B: per date s, build X[s], read the date returns, run WLS with weights
  // 1/d0_i over the kept instruments (skip M_s < K), store the factor return f[s] in
  // the next free row of `fseries` and append the per-instrument WLS residual to
  // `u_by_inst`. Returns the count of usable WLS dates (== filled fseries rows). A
  // per-date kernel failure is non-fatal (skip); a bad span propagates.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_wls(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                 std::span<const atx::u32> group_id, const atx::core::linalg::VecX &d0,
                 atx::core::linalg::MatX &fseries,
                 std::vector<std::vector<atx::f64>> &u_by_inst) const {
    const atx::usize k = static_cast<atx::usize>(fseries.cols());
    std::vector<atx::usize> keep;
    atx::usize used = 0U;
    for (atx::usize s = 0U; s < window; ++s) {
      ATX_TRY(ExposureMatrix xs, build_exposures(panel, cfg, s, market_cap, group_id));
      const atx::core::linalg::VecX r = detail::date_returns(panel, s, xs, keep);
      if (keep.size() < k) {
        continue; // under-determined -> skip (matches Pass A's skip rule)
      }
      const atx::core::linalg::MatX xsr = detail::select_rows(xs.x, keep);
      atx::core::linalg::VecX w(static_cast<Eigen::Index>(keep.size())); // weight 1/d0_i (P4-2 IVW)
      for (atx::usize j = 0U; j < keep.size(); ++j) {
        w[static_cast<Eigen::Index>(j)] =
            1.0 / d0[static_cast<Eigen::Index>(xs.instrument_rows[keep[j]])];
      }
      const auto fit = atx::core::linalg::wls(xsr, r, w);
      if (!fit) {
        continue; // rank-deficient weighted date -> skip
      }
      for (atx::usize c = 0U; c < k; ++c) {
        fseries(static_cast<Eigen::Index>(used), static_cast<Eigen::Index>(c)) =
            fit->beta[static_cast<Eigen::Index>(c)];
      }
      ++used;
      for (atx::usize j = 0U; j < keep.size(); ++j) {
        u_by_inst[xs.instrument_rows[keep[j]]].push_back(
            fit->residuals[static_cast<Eigen::Index>(j)]);
      }
    }
    return atx::core::Ok(used);
  }

  // Pass B (ROBUST, S8.1; opt-in via cfg.cov.robust_regression). Mirrors
  // accumulate_wls but, per date, composes a FIXED √-cap / inverse-specific-variance
  // PRIOR weight (detail::robust_prior_weight) with the S6-1 Huber IRLS kernel
  // (cost::irls_huber) instead of a single WLS solve — the prior keeps the WLS
  // character while Huber down-weights fat-tailed outlier instrument-dates. When
  // cfg.cov.industry_sum_to_zero is set, the kept design's industry dummies are
  // cap-weight mean-centered first (detail::apply_industry_sum_to_zero) to resolve the
  // market/industry-dummy collinearity. Stores the robust factor-return beta into the
  // next free fseries row and appends the per-instrument residuals — identical
  // downstream to the WLS path. Runs EXACTLY cfg.cov.robust_iters IRLS steps: we pass
  // tol = 0.0 so the kernel's `max_dh < tol` convergence test can never fire, making
  // the loop a true FIXED-COUNT iteration (the project's determinism rule favors a
  // fixed count over a convergence-dependent exit, and it moots any prior-weight
  // interaction with the kernel's Huber-factor convergence test). RNG-free. A per-date
  // kernel failure is non-fatal (skip); a bad span propagates. Returns the usable-date
  // count. (The S6 calibration path keeps the kernel's default tol; only this risk path
  // forces tol = 0.)
  // PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_robust(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                    std::span<const atx::u32> group_id, const atx::core::linalg::VecX &d0,
                    atx::core::linalg::MatX &fseries,
                    std::vector<std::vector<atx::f64>> &u_by_inst) const {
    const atx::usize k = static_cast<atx::usize>(fseries.cols());
    const cost::RobustCfg rcfg{/*huber_k=*/cfg.cov.huber_c, /*max_iter=*/cfg.cov.robust_iters,
                               /*tol=*/0.0};
    std::vector<atx::usize> keep;
    atx::usize used = 0U;
    for (atx::usize s = 0U; s < window; ++s) {
      ATX_TRY(ExposureMatrix xs, build_exposures(panel, cfg, s, market_cap, group_id));
      const atx::core::linalg::VecX r = detail::date_returns(panel, s, xs, keep);
      if (keep.size() < k) {
        continue; // under-determined -> skip (matches the WLS pass's skip rule)
      }
      atx::core::linalg::MatX xsr = detail::select_rows(xs.x, keep);
      if (cfg.cov.industry_sum_to_zero) {
        detail::apply_industry_sum_to_zero(xsr, xs, keep, market_cap);
      }
      // Rank probe: cost::irls_huber fails LOUD (ATX_CHECK) on a rank-deficient
      // design, but a degenerate date must be SKIPPED here (matching the WLS pass's
      // `if (!fit)` skip). An OLS solve on the post-constraint design is the same
      // rank test wls/irls would apply, run once up front so the IRLS only ever sees a
      // full-rank system.
      if (!atx::core::linalg::ols(xsr, r)) {
        continue; // rank-deficient (e.g. a collinear sector block) -> skip this date
      }
      const atx::core::linalg::VecX w0 =
          detail::robust_prior_weight(xs, keep, d0, market_cap, cfg.cov.cap_weight);
      const cost::RobustFit fit = cost::irls_huber(xsr, r, rcfg, &w0);
      for (atx::usize c = 0U; c < k; ++c) {
        fseries(static_cast<Eigen::Index>(used), static_cast<Eigen::Index>(c)) =
            fit.beta[static_cast<Eigen::Index>(c)];
      }
      ++used;
      for (atx::usize j = 0U; j < keep.size(); ++j) {
        u_by_inst[xs.instrument_rows[keep[j]]].push_back(fit.residuals[j]);
      }
    }
    return atx::core::Ok(used);
  }

  // Per current-cross-section instrument (X[0].instrument_rows order), D_i =
  // pop-var of its final WLS residual series; absent/short series -> 0 (create()
  // floors to kSpecificVarFloor). Length == M == X[0].n_instruments().
  [[nodiscard]] static atx::core::linalg::VecX
  specific_variances(const ExposureMatrix &x0,
                     const std::vector<std::vector<atx::f64>> &u_by_inst) {
    const atx::usize m = x0.n_instruments();
    atx::core::linalg::VecX d(static_cast<Eigen::Index>(m));
    for (atx::usize r = 0U; r < m; ++r) {
      const atx::usize inst = x0.instrument_rows[r];
      d[static_cast<Eigen::Index>(r)] = detail::pop_variance(u_by_inst[inst]);
    }
    return d;
  }
};

} // namespace atx::engine::risk
