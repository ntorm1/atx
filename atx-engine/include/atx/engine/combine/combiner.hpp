#pragma once

// atx::engine::combine — AlphaCombiner: progressive per-alpha blend-weight fit (P4-4).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  AlphaCombiner::fit(pool, fit_begin, fit_end) fits a per-alpha blend weight
//  vector over the gated pool on the TRAILING window [fit_begin, fit_end), via one
//  of five §5.3 methods (default ShrinkageMv). Each weight w_i scales constituent
//  alpha i's contribution to the combined signal; P4-5 wraps the pool + a frozen
//  Combination as an ISignalSource and APPLIES the weights point-in-time AFTER
//  fit_end. This is the COLD path (it may allocate scratch matrices).
//
// ===========================================================================
//  §3.1 fit/apply firewall — the central correctness rail
// ===========================================================================
//  fit() reads ONLY the row sub-span [fit_begin, fit_end) of each alpha's PnL
//  stream: every per-method helper takes window_span(pool, id) ==
//  pnl(id).subspan(fit_begin, T). A fit function PHYSICALLY CANNOT index a row
//  >= fit_end — there is no code path that reads outside the sub-span. This is
//  provable and pinned by the truncation-invariance test (mutating rows >= fit_end
//  leaves the fitted weights byte-identical). Combination carries [fit_begin,
//  fit_end) so the APPLY side (P4-5 / P4-10) can enforce apply-only-after-fit_end;
//  populating those fields is this unit's job, asserting the apply rail is not.
//
// ===========================================================================
//  §3.2 determinism — non-negotiable
// ===========================================================================
//  NO RNG anywhere. Every reduction (means, covariance sums, traces, Frobenius
//  norms) runs in canonical ascending alpha-id / period order. Every iterative
//  step (the bounded-regression clip/renorm) runs a FIXED iteration count in FIXED
//  order — no convergence-dependent early exit. Same input -> byte-identical
//  weights (P4-10 hash-checks the whole layer).
//
// ===========================================================================
//  As-built reconciliations (documented, load-bearing)
// ===========================================================================
//  * IcWeighted IC proxy: the store holds realized PnL streams, NOT raw signals or
//    forward returns, so the IC-from-signal form is not computable here. Per the
//    plan's explicit return-only proxy, IC_i ≈ window-sharpe_i =
//    mean(pnl_i[window]) / std(pnl_i[window]) (POPULATION std), w_i ∝
//    max(sharpe_i, 0), Σw = 1. CRITICAL (firewall): the sharpe is RECOMPUTED over
//    [fit_begin, fit_end) from the sub-span — the stored AlphaMetrics.sharpe (over
//    the FULL stream) would leak data outside the window and is NOT used. The §0-F
//    structural-zero convention (P4-2) is applied: IF index 0 falls in the window
//    it is excluded from the per-alpha moments (matches P4-2). IC-from-signal is
//    deferred until a raw-signal store exists.
//  * Ledoit-Wolf: atx-core has NO ledoit_wolf/shrinkage helper (grepped linalg/ +
//    stats/), so the canonical LW 2004 closed-form intensity is implemented below
//    (1/T² normalization; cited at the call site). Computed deterministically
//    (order-fixed sums).
//  * Covariance for ShrinkageMv: the SCM uses a COMPLETE-CASE (listwise) MLE
//    covariance S = (1/T_cc)·Σ_t r_t r_tᵀ over the demeaned window (periods where
//    ANY alpha is NaN are dropped) — for SPD-ness AND so the LW intensity and the
//    shrink/solve share the SAME S (coherence: a pairwise S for the solve with a
//    listwise S for the intensity would mismatch the off-diagonal scales once
//    δ < 1). The combine/correlation.hpp pairwise_complete_cov helper (the §3.3
//    NaN-policy covariance analog) is RETAINED for any genuinely-pairwise use but
//    is not used in this fit path. For the common no-NaN window the two agree.
//  * BoundedRegression realizes a RETURN-SPACE bounded MV fit (signal-space bounded
//    regression needs raw signals — deferred). It uses atx-core pca() for the
//    top-k eigenpairs and a closed-form ridge-regularized PC-space solve (no
//    hand-rolled eigen). The clip<->renorm loop is a FIXED 4 passes (documented).

#include <algorithm> // std::min, std::clamp
#include <cmath>     // std::isnan, std::sqrt, std::abs
#include <span>      // std::span
#include <vector>    // std::vector (cold-path scratch)

#include <Eigen/Dense> // VecX/MatX assembly for the linalg kernels

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, i64, u8, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/linalg/pca.hpp"    // pca, PcaResult
#include "atx/core/linalg/solve.hpp"  // solve_spd

#include "atx/engine/combine/store.hpp" // AlphaStore, AlphaId

namespace atx::engine::combine {

// ===========================================================================
//  CombineMethod — five §5.3 blend methods (matches fwd.hpp `: atx::u8`).
//
//  The fit() switch is EXHAUSTIVE (no default) so a new enumerator is a compile
//  error, not a silent fall-through.
// ===========================================================================
enum class CombineMethod : atx::u8 {
  EqualWeight,       // w_i = 1/N
  RankAverage,       // uniform 1/N here; rank-space combine is P4-5's job
  IcWeighted,        // w_i ∝ max(window-sharpe_i, 0)
  ShrinkageMv,       // Ledoit-Wolf shrunk mean-variance
  BoundedRegression, // ridge MV in top-k PC space, clipped to |w| <= bound
};

// ===========================================================================
//  CombinerConfig — §4 knobs.
// ===========================================================================
struct CombinerConfig {
  CombineMethod method = CombineMethod::ShrinkageMv;
  atx::f64 shrinkage = -1.0;    // Ledoit-Wolf AUTO if < 0, else fixed in [0,1]
  atx::f64 weight_bound = 0.10; // per-alpha |weight| cap (BoundedRegression)
  atx::f64 ridge_lambda = 1e-3; // regression regularization
  atx::usize n_pcs = 0;         // 0 => all PCs; else top-k SCM PCs (N >> T)
};

// ===========================================================================
//  Combination — the fitted blend (§4).
//
//  weights.size() == pool.size() (per-alpha blend weight in id order). The
//  [fit_begin, fit_end) fields are the apply-side firewall the downstream source
//  enforces (apply only AFTER fit_end).
// ===========================================================================
struct Combination {
  std::vector<atx::f64> weights;
  atx::usize fit_begin;
  atx::usize fit_end;
};

namespace detail {

// Sample-mean of a window sub-span over its non-NaN entries (canonical ascending
// order). NaN entries are skipped (pairwise-complete spirit); 0 valid entries ->
// 0.0 (a degenerate window contributes no expected return).
[[nodiscard]] inline atx::f64 window_mean(std::span<const atx::f64> w) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (const atx::f64 x : w) {
    if (!std::isnan(x)) {
      sum += x;
      ++n;
    }
  }
  return (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
}

// IcWeighted IC proxy: POPULATION sharpe = mean/std over the window, EXCLUDING the
// structural index 0 (P4-2 §0-F) when it falls in the window. `window` is the
// [fit_begin, fit_end) sub-span; `begin_is_zero` is true iff fit_begin == 0 (so the
// first window element is the structural zero to skip). Returns 0.0 for a flat /
// empty window (no Sharpe information).
[[nodiscard]] inline atx::f64 window_sharpe(std::span<const atx::f64> window,
                                            bool begin_is_zero) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize t = 0U; t < window.size(); ++t) {
    if (t == 0U && begin_is_zero) {
      continue; // structural zero excluded from the moment
    }
    const atx::f64 x = window[t];
    if (std::isnan(x)) {
      continue;
    }
    sum += x;
    ++n;
  }
  if (n == 0U) {
    return 0.0;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (atx::usize t = 0U; t < window.size(); ++t) {
    if (t == 0U && begin_is_zero) {
      continue;
    }
    const atx::f64 x = window[t];
    if (std::isnan(x)) {
      continue;
    }
    ss += (x - mean) * (x - mean);
  }
  const atx::f64 var = ss / static_cast<atx::f64>(n); // population (matches P4-2)
  return (var <= 0.0) ? 0.0 : mean / std::sqrt(var);
}

// Per-alpha window sub-span: pnl(id).subspan(fit_begin, T). The ONLY data fit()
// ever reads — the firewall is enforced by construction (no path reads >= fit_end).
[[nodiscard]] inline std::span<const atx::f64> window_span(const AlphaStore &pool, atx::usize id,
                                                           atx::usize fit_begin, atx::usize t) {
  return pool.pnl(AlphaId{static_cast<atx::u32>(id)}).subspan(fit_begin, t);
}

// Renormalize so Σ|w_i| == 1 (the gross-exposure target). A zero-gross vector
// (all weights 0) falls back to uniform 1/N so the combination is never empty.
inline void renorm_abs_sum(std::vector<atx::f64> &w) noexcept {
  atx::f64 gross = 0.0;
  for (const atx::f64 x : w) {
    gross += std::abs(x);
  }
  if (gross <= 0.0) {
    const atx::f64 u = 1.0 / static_cast<atx::f64>(w.size());
    for (atx::f64 &x : w) {
      x = u;
    }
    return;
  }
  const atx::f64 inv = 1.0 / gross;
  for (atx::f64 &x : w) {
    x *= inv;
  }
}

// Per-alpha window means, in id order (the MV target μ).
[[nodiscard]] inline atx::core::linalg::VecX window_means(const AlphaStore &pool, atx::usize n,
                                                          atx::usize fit_begin, atx::usize t) {
  atx::core::linalg::VecX mu(static_cast<Eigen::Index>(n));
  for (atx::usize i = 0U; i < n; ++i) {
    mu[static_cast<Eigen::Index>(i)] = window_mean(window_span(pool, i, fit_begin, t));
  }
  return mu;
}

// Complete-case MLE sample covariance S = (1/T_cc)·Σ_t r_t r_tᵀ from the demeaned
// T_cc×N window matrix `centered` (each column already demeaned by its window mean).
// DIVISOR T_cc (the MLE / 1/T form), NOT T_cc−1: the Ledoit-Wolf 2004 closed form
// is derived for this MLE covariance, so the intensity AND the shrink/solve must
// share the SAME S for coherence (§ COHERENCE). Symmetric by construction. Empty
// window (T_cc == 0) yields a zero matrix (the caller's T>=2 guard plus complete-
// case dropping make this only reachable for an all-NaN window).
[[nodiscard]] inline atx::core::linalg::MatX mle_covariance(const atx::core::linalg::MatX &centered,
                                                            atx::usize n) {
  const Eigen::Index t = centered.rows();
  atx::core::linalg::MatX s =
      atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
  if (t < 1) {
    return s;
  }
  // S = (1/T_cc) Rᵀ R  (Eigen's matrix product sums in a fixed order -> deterministic).
  s = (centered.transpose() * centered) / static_cast<atx::f64>(t);
  return s;
}

// Ledoit & Wolf (2004) "A well-conditioned estimator for large-dimensional
// covariance matrices" — optimal shrinkage intensity δ toward the scaled-identity
// target F = μ·I, μ = tr(S)/N. ALL terms are computed from the ONE complete-case
// demeaned sample matrix `centered` (T_cc×N) and its MLE covariance `s` (divisor
// T_cc — see mle_covariance). Closed form (order-fixed sums, no RNG):
//   μ   = tr(S) / N
//   d²  = ‖S − μ·I‖²_F                         (NO 1/N — Frobenius sq-dist to target)
//   π̂   = (1/T_cc) Σ_t ‖ r_t r_tᵀ − S ‖²_F      (mean over t of per-sample sq-dist)
//   b̄²  = π̂ / T_cc                              (the 1/T factor; == (1/T_cc²)·Σ ‖..‖²_F)
//   b²  = min(b̄², d²)                           (LW guarantee b̄² ≤ d²)
//   δ   = (d² == 0) ? 0 : clamp(b² / d², 0, 1)
// HISTORICAL DEFECT (fixed): an earlier revision divided d² by N and b̄² by N·T
// instead of T², inflating b̄²/d² by ~T/N so δ saturated to 1 (full shrinkage to
// μ·I) on every realistic window, discarding the off-diagonal SCM structure. The
// 1/T² normalization is the canonical LW result. A degenerate d² == 0 (S already
// == μ·I) gives δ = 0 (the identity target already equals S — no shrinkage needed).
[[nodiscard]] inline atx::f64 ledoit_wolf_intensity(const atx::core::linalg::MatX &s,
                                                    const atx::core::linalg::MatX &centered) {
  const Eigen::Index n = s.rows();
  const Eigen::Index t = centered.rows();
  if (t < 1) {
    return 0.0; // no observations -> no shrinkage signal
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 tf = static_cast<atx::f64>(t);
  const atx::f64 mu = s.trace() / nf;
  // d² = ‖S − μ·I‖²_F (no 1/N).
  atx::core::linalg::MatX diff = s;
  diff.diagonal().array() -= mu;
  const atx::f64 d2 = diff.squaredNorm();
  if (d2 <= 0.0) {
    return 0.0; // S already proportional to I -> target == S -> no shrinkage
  }
  // π̂ = (1/T_cc) Σ_t ‖ r_t r_tᵀ − S ‖²_F. Order-fixed sum over periods.
  atx::f64 pi_sum = 0.0;
  for (Eigen::Index k = 0; k < t; ++k) {
    const atx::core::linalg::VecX r = centered.row(k).transpose();
    atx::core::linalg::MatX outer = r * r.transpose();
    outer -= s;
    pi_sum += outer.squaredNorm();
  }
  const atx::f64 pi_hat = pi_sum / tf;
  const atx::f64 b2 = pi_hat / tf;                 // b̄² = π̂ / T_cc  (the missing 1/T)
  const atx::f64 b2_clamped = (b2 > d2) ? d2 : b2; // b² = min(b̄², d²)
  return std::clamp(b2_clamped / d2, 0.0, 1.0);
}

// Complete-case column-demeaned window data, laid out T×N (rows = periods,
// columns = alphas — the pca()/regression design-matrix convention). A period is
// kept only if EVERY alpha is non-NaN at it (complete-case rows), so the LW outer
// products and the pca() route see a clean rectangular block. Returns the matrix
// AND the kept-row count via `kept`. For the common no-NaN window every row is kept.
[[nodiscard]] inline atx::core::linalg::MatX
complete_case_centered(const AlphaStore &pool, atx::usize n, atx::usize fit_begin, atx::usize t,
                       const atx::core::linalg::VecX &mu) {
  // First pass: count complete-case rows (no NaN in any alpha at that period).
  std::vector<atx::usize> kept_rows;
  kept_rows.reserve(t);
  for (atx::usize p = 0U; p < t; ++p) {
    bool complete = true;
    for (atx::usize i = 0U; i < n && complete; ++i) {
      complete = !std::isnan(window_span(pool, i, fit_begin, t)[p]);
    }
    if (complete) {
      kept_rows.push_back(p);
    }
  }
  const auto rows = static_cast<Eigen::Index>(kept_rows.size());
  atx::core::linalg::MatX centered(rows, static_cast<Eigen::Index>(n));
  for (Eigen::Index r = 0; r < rows; ++r) {
    const atx::usize p = kept_rows[static_cast<atx::usize>(r)];
    for (atx::usize i = 0U; i < n; ++i) {
      const atx::f64 v = window_span(pool, i, fit_begin, t)[p];
      centered(r, static_cast<Eigen::Index>(i)) = v - mu[static_cast<Eigen::Index>(i)];
    }
  }
  return centered;
}

// ---------------------------------------------------------------------------
//  Per-method weight fits (each ≤ ~60 lines; the fit() dispatch is a thin switch).
// ---------------------------------------------------------------------------

// EqualWeight / RankAverage: uniform 1/N. (RankAverage's rank-space combination is
// done by P4-5; fit() returns uniform weights here — the METHOD flag differs.)
[[nodiscard]] inline std::vector<atx::f64> fit_uniform(atx::usize n) {
  return std::vector<atx::f64>(n, 1.0 / static_cast<atx::f64>(n));
}

// IcWeighted: w_i ∝ max(window-sharpe_i, 0), Σw = 1. All-non-positive sharpes ->
// uniform fallback (no positive-IC alpha to weight). begin_is_zero excludes the
// structural index 0 from each per-alpha moment (P4-2 §0-F) when fit_begin == 0.
[[nodiscard]] inline std::vector<atx::f64> fit_ic_weighted(const AlphaStore &pool, atx::usize n,
                                                           atx::usize fit_begin, atx::usize t) {
  const bool begin_is_zero = (fit_begin == 0U);
  std::vector<atx::f64> w(n, 0.0);
  atx::f64 total = 0.0;
  for (atx::usize i = 0U; i < n; ++i) {
    const atx::f64 s = window_sharpe(window_span(pool, i, fit_begin, t), begin_is_zero);
    const atx::f64 clipped = (s > 0.0) ? s : 0.0; // max(IC, 0)
    w[i] = clipped;
    total += clipped;
  }
  if (total <= 0.0) {
    return fit_uniform(n); // no positive-IC alpha -> uniform
  }
  const atx::f64 inv = 1.0 / total;
  for (atx::f64 &x : w) {
    x *= inv;
  }
  return w;
}

// The Ledoit-Wolf shrunk covariance Σ̂ and the intensity δ used to build it, over
// the §3.1 window. COHERENCE: the SAME complete-case MLE covariance S (divisor
// T_cc, listwise NaN-dropped) feeds BOTH the LW intensity AND the shrink/solve —
// computing the intensity on listwise rows while solving against a pairwise_complete
// S would be incoherent once δ < 1 (the off-diagonal scales would not match). For
// the common no-NaN window this is identical to the pairwise covariance. The
// pairwise_complete_cov helper remains for any genuinely-pairwise use elsewhere.
// `cfg_shrinkage >= 0` overrides δ with the fixed value clamped to [0,1].
struct ShrunkCov {
  atx::core::linalg::MatX sigma; // Σ̂ = δ·μ·I + (1−δ)·S
  atx::core::linalg::VecX mu;    // per-alpha mean-return target (MV right-hand side)
  atx::f64 delta;                // the LW (or fixed) shrinkage intensity actually used
};

// Numerical SPD floor for the AUTO Ledoit-Wolf intensity. The statistically
// optimal LW δ can come out ~0 on a SINGULAR sample covariance (T_cc < N): when the
// populated subspace looks near-spherical the data carries little shrinkage signal,
// so δ→0 and Σ̂ ≈ S keeps the null-space eigenvalue at 0 (Cholesky then fails). The
// contract requires shrinkage to RESCUE the T<N singular case (return finite
// weights), so the AUTO intensity is floored to this tiny constant: it lifts every
// null-space eigenvalue to ≥ δ_floor·μ_id > 0 (Σ̂ SPD), while staying FAR below any
// statistically-meaningful δ — in the well-conditioned regime LW δ ≫ kLwSpdFloor,
// so the inverse-variance tilt is unaffected (the floor never binds there). It is a
// NUMERICAL SPD guard, not a statistical choice, and applies ONLY to the AUTO path
// (a caller-supplied fixed cfg.shrinkage is honored verbatim).
inline constexpr atx::f64 kLwSpdFloor = 1e-6;

[[nodiscard]] inline ShrunkCov shrunk_covariance(const AlphaStore &pool, atx::usize n,
                                                 atx::usize fit_begin, atx::usize t,
                                                 atx::f64 cfg_shrinkage) {
  using namespace atx::core::linalg;
  const VecX mu_ret = window_means(pool, n, fit_begin, t);
  const MatX centered = complete_case_centered(pool, n, fit_begin, t, mu_ret);
  const MatX s = mle_covariance(centered, n);        // complete-case MLE S (divisor T_cc)
  const f64 mu_id = s.trace() / static_cast<f64>(n); // scaled-identity target factor
  f64 delta = 0.0;
  if (cfg_shrinkage < 0.0) {
    // AUTO LW, floored for SPD-ness on a singular S (see kLwSpdFloor).
    delta = std::max(ledoit_wolf_intensity(s, centered), kLwSpdFloor);
  } else {
    delta = std::clamp(cfg_shrinkage, 0.0, 1.0); // fixed intensity, honored verbatim
  }
  // Σ̂ = δ·μ·I + (1−δ)·S.
  MatX sigma = (1.0 - delta) * s;
  sigma.diagonal().array() += delta * mu_id;
  return ShrunkCov{std::move(sigma), mu_ret, delta};
}

// ShrinkageMv: solve Σ̂ w = μ via SPD Cholesky on the LW-shrunk covariance, then
// renormalize Σ|w| = 1. Returns Err if Σ̂ is not SPD even after shrinkage (the
// solve_spd factorization fails — e.g. δ == 0 on a singular S).
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
fit_shrinkage_mv(const AlphaStore &pool, atx::usize n, atx::usize fit_begin, atx::usize t,
                 atx::f64 cfg_shrinkage) {
  using namespace atx::core::linalg;
  const ShrunkCov sc = shrunk_covariance(pool, n, fit_begin, t, cfg_shrinkage);
  ATX_TRY(VecX raw, solve_spd(sc.sigma, sc.mu)); // SPD after shrinkage; Err if not
  std::vector<atx::f64> w(n);
  for (atx::usize i = 0U; i < n; ++i) {
    w[i] = raw[static_cast<Eigen::Index>(i)];
  }
  renorm_abs_sum(w); // Σ|w| = 1
  return atx::core::Ok(std::move(w));
}

// BoundedRegression: ridge-regularized MV weights in the top-k PC space of S, then
// clip |w_i| <= bound and re-project (Σ|w| = 1) over a FIXED 4 passes.
//   pca(centered-window) -> V_k (components, N×k), Λ_k (explained_variance, k).
//   w_pc[j] = (V_kᵀ μ)[j] / (Λ_k[j] + ridge_lambda);  w = V_k · w_pc.
// FIXED 4-pass clip<->renorm (no convergence-dependent exit, §3.2 determinism):
// after the final renorm a clipped weight may overshoot the bound by a bounded
// margin (the renorm rescales clipped weights up); the test tolerance accounts for
// it. Returns Err only if pca() rejects the window (already guarded by T>=2 / N>=1).
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
fit_bounded_regression(const AlphaStore &pool, atx::usize n, atx::usize fit_begin, atx::usize t,
                       atx::f64 weight_bound, atx::f64 ridge_lambda, atx::usize n_pcs) {
  using namespace atx::core::linalg;
  const VecX mu = window_means(pool, n, fit_begin, t);
  const MatX centered = complete_case_centered(pool, n, fit_begin, t, mu);
  const atx::i64 k = (n_pcs == 0U) ? -1 : static_cast<atx::i64>(std::min(n_pcs, n));
  ATX_TRY(PcaResult model, pca(centered, k)); // top-k eigenpairs (no hand-rolled eigen)
  const MatX &v = model.components;           // N × k (unit eigenvectors)
  const VecX &lam = model.explained_variance; // k (descending variance)
  const VecX vtmu = v.transpose() * mu;       // V_kᵀ μ  (length k)
  VecX w_pc(vtmu.size());
  for (Eigen::Index j = 0; j < vtmu.size(); ++j) {
    w_pc[j] = vtmu[j] / (lam[j] + ridge_lambda); // ridge-regularized PC-space MV
  }
  const VecX w_vec = v * w_pc; // map back to alpha space (length N)
  std::vector<atx::f64> w(n);
  for (atx::usize i = 0U; i < n; ++i) {
    w[i] = w_vec[static_cast<Eigen::Index>(i)];
  }
  // FIXED 4-pass clip<->renorm (deterministic; documented count).
  constexpr int kClipPasses = 4;
  for (int pass = 0; pass < kClipPasses; ++pass) {
    for (atx::f64 &x : w) {
      x = std::clamp(x, -weight_bound, weight_bound);
    }
    renorm_abs_sum(w);
  }
  return atx::core::Ok(std::move(w));
}

} // namespace detail

// ===========================================================================
//  AlphaCombiner — fit per-alpha blend weights over the gated pool (§8 P4-4).
// ===========================================================================
class AlphaCombiner {
public:
  CombinerConfig cfg;

  // Fit weights on [fit_begin, fit_end). COLD path (allocates scratch). Reads ONLY
  // the window sub-span of each PnL row (the §3.1 firewall). Err on an empty pool,
  // T < 2, a window exceeding the streams, or a non-SPD covariance that shrinkage
  // cannot rescue (documented). Combination carries [fit_begin, fit_end).
  [[nodiscard]] atx::core::Result<Combination> fit(const AlphaStore &pool, atx::usize fit_begin,
                                                   atx::usize fit_end) const {
    const atx::usize n = pool.size();
    if (n == 0U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "AlphaCombiner::fit: empty pool");
    }
    if (fit_end <= fit_begin || (fit_end - fit_begin) < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "AlphaCombiner::fit: fit window must have T >= 2 periods");
    }
    if (fit_end > pool.n_periods()) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "AlphaCombiner::fit: fit_end exceeds the pool period count");
    }
    const atx::usize t = fit_end - fit_begin;

    // Single alpha: the only gross-1 allocation is w = [1]. Short-circuits before
    // any covariance/solve so it cannot fail (matches the §8 boundary contract).
    if (n == 1U) {
      return make_combination(std::vector<atx::f64>{1.0}, fit_begin, fit_end);
    }

    // EXHAUSTIVE switch over CombineMethod — NO default (a new enumerator is a
    // compile error). EqualWeight/RankAverage are closed-form; the rest read R.
    switch (cfg.method) {
    case CombineMethod::EqualWeight:
    case CombineMethod::RankAverage:
      return make_combination(detail::fit_uniform(n), fit_begin, fit_end);
    case CombineMethod::IcWeighted:
      return make_combination(detail::fit_ic_weighted(pool, n, fit_begin, t), fit_begin, fit_end);
    case CombineMethod::ShrinkageMv: {
      ATX_TRY(std::vector<atx::f64> w,
              detail::fit_shrinkage_mv(pool, n, fit_begin, t, cfg.shrinkage));
      return make_combination(std::move(w), fit_begin, fit_end);
    }
    case CombineMethod::BoundedRegression: {
      ATX_TRY(std::vector<atx::f64> w,
              detail::fit_bounded_regression(pool, n, fit_begin, t, cfg.weight_bound,
                                             cfg.ridge_lambda, cfg.n_pcs));
      return make_combination(std::move(w), fit_begin, fit_end);
    }
    }
    // Unreachable: the switch is exhaustive over CombineMethod.
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "AlphaCombiner::fit: unrecognized CombineMethod");
  }

private:
  // Assemble the Combination value (weights + the firewall window fields).
  [[nodiscard]] static atx::core::Result<Combination>
  make_combination(std::vector<atx::f64> weights, atx::usize fit_begin, atx::usize fit_end) {
    return atx::core::Ok(Combination{std::move(weights), fit_begin, fit_end});
  }
};

} // namespace atx::engine::combine
