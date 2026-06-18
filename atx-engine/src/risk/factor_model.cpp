#include "atx/engine/risk/factor_model.hpp"

// atx::engine::risk — FactorModel apply-math + FactorModelBuilder estimation +
// detail:: kernels (the BODIES). S8.8a header/source split: this TU holds the heavy
// estimation includes (Ledoit-Wolf combine, robust IRLS, EWMA/Newey-West, eigen-
// adjust, APCA, VRA, horizon-blend, specific-risk, regression) so the hub header
// (factor_model.hpp) no longer leaks them into its 42 dependents. PURE refactor —
// the math is byte-identical to the pre-split header-only implementation (R10).

#include <algorithm> // std::clamp (fixed factor-cov shrink override)
#include <cmath>     // std::isnan (FactorModelBuilder: per-date return drop)
#include <utility>   // std::move

#include <Eigen/Dense> // Eigen::LLT, Eigen::Index, Eigen::Map

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_UNUSED, ATX_TRY

#include "atx/core/linalg/regression.hpp" // ols, wls (per-date factor-return solve)

#include "atx/engine/combine/combiner.hpp" // combine::detail::ledoit_wolf_intensity (canonical LW)
// PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.
#include "atx/engine/cost/robust_ls.hpp" // cost::irls_huber, RobustCfg, RobustFit (S8.1 robust path)
#include "atx/engine/risk/cov_ewma.hpp"     // ewma_factor_covariance (S8.2 EWMA + Newey-West path)
#include "atx/engine/risk/eigen_adjust.hpp" // eigen_adjust (S8.3 Monte-Carlo eigenfactor de-biasing)
#include "atx/engine/risk/exposures.hpp"    // build_exposures, ExposureMatrix, detail::step_return
#include "atx/engine/risk/horizon_blend.hpp" // blend_factor_cov, blend_specific (S8.8 horizon blend)
#include "atx/engine/risk/specific_risk.hpp" // specific_risk_blend, SpecificRisk (S8.4 specific risk)
#include "atx/engine/risk/stat_factor_model.hpp" // detail::{gram_matrix,…} (S8.6 APCA kernels)
#include "atx/engine/risk/vol_regime.hpp"        // vol_regime_multiplier, RegimeAdjust (S8.5 VRA)

namespace atx::engine::risk {

// ===========================================================================
//  FactorModel::Impl — the private (X, F, D, dinv, cached Cholesky) state behind
//  the pimpl. Holds the SAME members the pre-split private section held; the
//  apply-math reads them out-of-line below. Byte-identical state + arithmetic.
// ===========================================================================
struct FactorModel::Impl {
  atx::core::linalg::MatX x_;                   // M×K exposures
  atx::core::linalg::MatX f_;                   // K×K factor covariance (SPD)
  atx::core::linalg::VecX d_;                   // M specific variances (floored, > 0)
  atx::core::linalg::VecX dinv_;                // M elementwise 1/D (cached for Woodbury)
  Eigen::LLT<atx::core::linalg::MatX> cap_llt_; // cached Cholesky of C = F⁻¹ + Xᵀ D⁻¹ X
};

FactorModel::FactorModel(std::unique_ptr<Impl> impl, atx::usize fit_begin, atx::usize fit_end)
    : impl_{std::move(impl)}, fit_begin_{fit_begin}, fit_end_{fit_end} {}

// Deep copy — replicates the pre-split value semantics (the old class held Eigen
// members by value). Impl is fully Eigen-copyable (MatX/VecX/LLT all copy).
FactorModel::FactorModel(const FactorModel &other)
    : impl_{other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr},
      fit_begin_{other.fit_begin_}, fit_end_{other.fit_end_} {}
FactorModel &FactorModel::operator=(const FactorModel &other) {
  if (this != &other) {
    impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    fit_begin_ = other.fit_begin_;
    fit_end_ = other.fit_end_;
  }
  return *this;
}
FactorModel::FactorModel(FactorModel &&) noexcept = default;
FactorModel &FactorModel::operator=(FactorModel &&) noexcept = default;
FactorModel::~FactorModel() = default;

atx::core::Result<FactorModel>
FactorModel::create(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
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

  auto impl = std::make_unique<Impl>();
  impl->x_ = std::move(x);
  impl->f_ = std::move(f);
  impl->d_ = std::move(d);
  impl->dinv_ = std::move(dinv);
  impl->cap_llt_ = std::move(cap_llt);
  return atx::core::Ok(FactorModel(std::move(impl), fit_begin, fit_end));
}

atx::usize FactorModel::n_factors() const noexcept {
  return static_cast<atx::usize>(impl_->x_.cols()); // K
}
atx::usize FactorModel::n_instruments() const noexcept {
  return static_cast<atx::usize>(impl_->x_.rows()); // M
}
const atx::core::linalg::MatX &FactorModel::exposures() const noexcept { return impl_->x_; }

atx::f64 FactorModel::risk(std::span<const atx::f64> w) const noexcept {
  const atx::core::linalg::MatX &x_ = impl_->x_;
  const atx::core::linalg::MatX &f_ = impl_->f_;
  const atx::core::linalg::VecX &d_ = impl_->d_;
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

void FactorModel::apply_inverse(std::span<const atx::f64> in, std::span<atx::f64> out) const {
  const atx::core::linalg::MatX &x_ = impl_->x_;
  const atx::core::linalg::VecX &dinv_ = impl_->dinv_;
  const Eigen::LLT<atx::core::linalg::MatX> &cap_llt_ = impl_->cap_llt_;
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

void FactorModel::apply(std::span<const atx::f64> in, std::span<atx::f64> out) const {
  const atx::core::linalg::MatX &x_ = impl_->x_;
  const atx::core::linalg::MatX &f_ = impl_->f_;
  const atx::core::linalg::VecX &d_ = impl_->d_;
  const Eigen::Index m = x_.rows();
  ATX_ASSERT(in.size() == static_cast<atx::usize>(m));
  ATX_ASSERT(out.size() == static_cast<atx::usize>(m));

  Eigen::Map<const atx::core::linalg::VecX> in_v(in.data(), m);
  Eigen::Map<atx::core::linalg::VecX> out_v(out.data(), m);

  const atx::core::linalg::VecX t = x_.transpose() * in_v; // Xᵀ in          (K)
  const atx::core::linalg::VecX ft = f_ * t;               // F Xᵀ in        (K)
  out_v = x_ * ft + d_.cwiseProduct(in_v);                 // X F Xᵀ in + D∘in (M)
}

const atx::core::linalg::VecX &FactorModel::specific_var() const noexcept { return impl_->d_; }
const atx::core::linalg::MatX &FactorModel::factor_cov() const noexcept { return impl_->f_; }

void FactorModel::neutralize(std::span<atx::f64> signal) const {
  const atx::core::linalg::MatX &x_ = impl_->x_;
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

atx::usize FactorModel::fit_begin() const noexcept { return fit_begin_; }
atx::usize FactorModel::fit_end() const noexcept { return fit_end_; }

// ===========================================================================
//  FactorModelBuilder detail kernels (P4-7b). date_returns / select_rows /
//  pop_variance / robust_prior_weight are file-local (anonymous namespace) —
//  no external caller. factor_covariance is exported (declared in the header) so
//  the risk_cov_ewma test's single-window MLE reference resolves to this symbol.
// ===========================================================================
namespace detail {
namespace {

// One date's cross-section returns over the surviving instruments of `xm`:
// r_i = step_return(panel, s, inst) for inst in xm.instrument_rows. Any NaN return
// drops that (date, instrument) listwise — the kept rows are reported via `keep`
// (indices into xm.instrument_rows) so the caller can sub-select X[s]'s rows too.
[[nodiscard]] atx::core::linalg::VecX date_returns(const PanelView &panel, atx::usize s,
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
[[nodiscard]] atx::core::linalg::MatX select_rows(const atx::core::linalg::MatX &x,
                                                  const std::vector<atx::usize> &keep) {
  atx::core::linalg::MatX out(static_cast<Eigen::Index>(keep.size()), x.cols());
  for (atx::usize j = 0U; j < keep.size(); ++j) {
    out.row(static_cast<Eigen::Index>(j)) = x.row(static_cast<Eigen::Index>(keep[j]));
  }
  return out;
}

// Population variance of a value series over the window (order-fixed two-pass mean
// then sum-of-squares). 0 or 1 samples -> 0.0 (FactorModel::create floors D anyway).
[[nodiscard]] atx::f64 pop_variance(const std::vector<atx::f64> &xs) noexcept {
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

// S8.1 robust-prior weight over a date's KEPT instruments. The robust IRLS uses
// w0_i as the FIXED prior weight (multiplied by the per-iteration Huber factor):
//   * cap_weight && caps present ⇒ √-cap weighting from the exposures helper,
//     w0_i = √(cap_i) / mean_j √(cap_j); where that helper FLAGS a row (a non-positive
//     cap, returned as 0.0) we substitute the 1/d0_i fallback.
//   * otherwise ⇒ w0_i = 1/d0_i, the SAME inverse-specific-variance weight the P4 WLS
//     pass uses. `keep` indexes into `xm.instrument_rows`; d0 is indexed by the
//     universe instrument. The √-cap math lives in exposures::detail::sqrt_cap_weight.
//     Order-fixed.
[[nodiscard]] atx::core::linalg::VecX
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

} // namespace

// The K×K Ledoit-Wolf shrunk covariance of a T×K factor-return series (exported;
// see header). F = (1−δ)·S + δ·m·I; cfg_shrink >= 0 overrides δ (clamped).
atx::core::linalg::MatX factor_covariance(atx::core::linalg::MatX fseries, atx::f64 cfg_shrink) {
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

} // namespace detail

// ===========================================================================
//  FactorModelBuilder — estimation bodies.
// ===========================================================================

atx::core::Result<FactorModel> FactorModelBuilder::build(const PanelView &panel, atx::usize window,
                                                         std::span<const atx::f64> market_cap,
                                                         std::span<const atx::u32> group_id) const {
  // Rung dispatch (in the thin wrapper). The dead-alpha rung is STILL a deferred
  // residual (S7.3) → NotImplemented; the statistical (APCA) rung is WIRED (S8.6),
  // a DISTINCT model variant. Dead takes precedence so a (stat>0 AND dead>0) config
  // is rejected, not silently fit as statistical-only.
  if (cfg.n_dead_factors > 0U) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          "FactorModelBuilder::build: dead-alpha factor rung is a deferred "
                          "residual"); // NOT a silent skip — an explicit deferral
  }
  if (cfg.n_stat_factors > 0U) {
    return build_stat_factor_model(panel, window, market_cap, group_id, cfg, cfg.n_stat_factors,
                                   cfg.cov.apca_gls_reweight, cfg.factor_cov_shrink);
  }
  // Fundamental path: estimate (X, F, D) in build_components (the S7-3 augmentation seam),
  // then assemble. build_components returns EXACTLY the (X, F, D, fit_end) create consumes.
  ATX_TRY(FactorComponents comp, build_components(panel, window, market_cap, group_id));
  return FactorModel::create(std::move(comp.X), std::move(comp.F), std::move(comp.D),
                             /*fit_begin=*/0U, /*fit_end=*/comp.fit_end);
}

atx::core::Result<FactorComponents>
FactorModelBuilder::build_components(const PanelView &panel, atx::usize window,
                                     std::span<const atx::f64> market_cap,
                                     std::span<const atx::u32> group_id) const {
  if (cfg.n_stat_factors > 0U || cfg.n_dead_factors > 0U) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          "FactorModelBuilder::build_components: stat/dead rungs are dispatched "
                          "by build()"); // build() handles APCA / the dead-alpha deferral
  }
  if (window < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: require window >= 2");
  }
  // X[0] (the CURRENT cross-section) defines M and the emitted factor count K.
  ATX_TRY(ExposureMatrix x0, build_exposures(panel, cfg, /*row=*/0U, market_cap, group_id));
  const atx::usize k = x0.n_factors();
  if (k == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: no factor columns emitted");
  }
  if (window < k) { // T < K -> the factor-return cov is rank-deficient
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: window < factor count (T < K)");
  }

  // Pass A (OLS) -> bootstrap specific variances d0; Pass B (WLS) -> f[s], u[s].
  const atx::usize n_inst = panel.instruments();
  atx::core::linalg::VecX d0(static_cast<Eigen::Index>(n_inst));
  ATX_TRY(atx::usize used_a, accumulate_ols(panel, window, market_cap, group_id, k, d0));
  if (used_a < 2U) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        "FactorModelBuilder::build_components: too few usable dates (M_s < K everywhere)");
  }

  std::vector<std::vector<atx::f64>> u_by_inst(n_inst); // final residuals per universe inst
  atx::core::linalg::MatX fseries(static_cast<Eigen::Index>(window), static_cast<Eigen::Index>(k));
  // Pass B dispatch: robust root-cap + Huber IRLS when opted in (cfg.cov), else the
  // P4 plain inverse-specific-variance WLS. Both emit the SAME (window×K) factor-
  // return series + per-instrument residuals downstream; the default keeps P4 exactly.
  ATX_TRY(atx::usize used_b,
          cfg.cov.robust_regression
              ? accumulate_robust(panel, window, market_cap, group_id, d0, fseries, u_by_inst)
              : accumulate_wls(panel, window, market_cap, group_id, d0, fseries, u_by_inst));
  if (used_b < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: too few usable WLS dates");
  }
  // Compact fseries to the rows actually filled (under-determined dates skipped).
  const atx::core::linalg::MatX fkept = fseries.topRows(static_cast<Eigen::Index>(used_b));
  if (fkept.rows() < static_cast<Eigen::Index>(k)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: usable dates < K (factor cov rank)");
  }

  // Factor-covariance dispatch. DEFAULT (LedoitWolfSingle) is the as-built P4 path,
  // byte-identical. EwmaNeweyWest (S8.2, opt-in) is the split vol/corr half-life EWMA
  // covariance recombined F_ij=ρ_ij·σ_i·σ_j with a Newey-West Bartlett serial-corr add
  // and an SPD eigenvalue floor. EXHAUSTIVE switch (no default — a new enumerator is a
  // compile error). `fkept` rows are the compacted kept dates, newest (row 0) first.
  atx::core::linalg::MatX f;
  switch (cfg.cov.factor_cov_method) {
  case FactorCovMethod::LedoitWolfSingle:
    f = detail::factor_covariance(fkept, cfg.factor_cov_shrink);
    break;
  case FactorCovMethod::EwmaNeweyWest:
    // S8.8 short/long-horizon blend (opt-in via cfg.cov.horizon_blend; DEFAULT false is
    // the single-horizon S8.2 path, byte-identical). Build the SAME `fkept` series at
    // TWO half-life sets and convex-blend F = w·F_short + (1−w)·F_long. Both inputs are
    // SPD (eigenvalue-floored) so the convex combo is SPD (no extra PSD repair).
    if (cfg.cov.horizon_blend) {
      const atx::core::linalg::MatX f_short = ewma_factor_covariance(
          fkept, cfg.cov.vol_halflife, cfg.cov.corr_halflife, cfg.cov.nw_lags);
      const atx::core::linalg::MatX f_long = ewma_factor_covariance(
          fkept, cfg.cov.vol_halflife_long, cfg.cov.corr_halflife_long, cfg.cov.nw_lags);
      f = blend_factor_cov(f_short, f_long, cfg.cov.horizon_blend_weight);
    } else {
      f = ewma_factor_covariance(fkept, cfg.cov.vol_halflife, cfg.cov.corr_halflife,
                                 cfg.cov.nw_lags);
    }
    break;
  }
  // S8.3 eigenfactor risk adjustment (opt-in via cfg.cov.eigen_adjust_sims > 0; DEFAULT 0
  // is a no-op so the P4 / S8.2 covariance is untouched, byte-identical). Monte-Carlo
  // de-biases the factor-covariance eigenvariances (Menchero-Wang-Orr) BEFORE create's
  // SPD gate; the adjustment is PSD-preserving (γ²>0). This is the ONLY RNG site in the
  // build — its Xoshiro256pp seed is cfg.cov.eigen_adjust_seed, recorded for replay.
  if (cfg.cov.eigen_adjust_sims > 0U) {
    ATX_TRY(atx::core::linalg::MatX f_adj,
            eigen_adjust(f, cfg.cov.eigen_adjust_sims, cfg.cov.eigen_adjust_amplify,
                         cfg.cov.eigen_adjust_seed));
    f = std::move(f_adj);
  }
  // S8.5 Volatility Regime Adjustment (opt-in via cfg.cov.vra_halflife > 0; DEFAULT 0 is
  // a no-op, byte-identical). The market-wide regime multiplier λ² is computed ONCE from
  // `fkept` and the FINALIZED (post-eigen-adjust) F, then rescales BOTH F ← λ²·F and
  // D ← λ²·D (PSD-preserving — λ² > 0). RNG-free.
  atx::core::linalg::VecX d = specific_variances(x0, u_by_inst, window);
  if (cfg.cov.vra_halflife > 0U) {
    const RegimeAdjust ra = vol_regime_multiplier(fkept, f, cfg.cov.vra_halflife);
    f *= ra.lambda2;
    d *= ra.lambda2;
  }
  return atx::core::Ok(
      FactorComponents{std::move(x0.x), std::move(f), std::move(d), /*fit_end=*/window});
}

// ===========================================================================
//  build_stat_factor_model — the S8.6 STATISTICAL (APCA) model variant.
//
//  Steps (Connor-Korajczyk 2-pass; algorithm detail in stat_factor_model.hpp):
//    1. Cross-section = the row-0 build_exposures survivors.
//    2. Build the complete-case return panel R (N×T): keep an instrument only if ALL
//       T trailing returns are non-NaN. Column-demean each row.
//    3. Validate N > T, T > K, N > K.
//    4. Pass 1 (equal-weight): Fhat = top-K of the T×T Gram; B, s_n from R.
//    5. Pass 2 (GLS, when gls_reweight): re-extract Fhat from the 1/√s_n-reweighted
//       Gram; recover the FINAL B, s_n from the UN-weighted R.
//    6. X = B, F = factor_covariance(Fhat, factor_cov_shrink), D = s_n; create.
//  PIT-structural; RNG-free, order-fixed ⇒ byte-identical on replay (Fhat sign-pinned).
// ===========================================================================
atx::core::Result<FactorModel> FactorModelBuilder::build_stat_factor_model(
    const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
    std::span<const atx::u32> group_id, const FactorModelConfig &cfg, atx::usize n_stat,
    bool gls_reweight, atx::f64 factor_cov_shrink) {
  if (window < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_stat_factor_model: require window >= 2");
  }
  // The current cross-section (row 0) defines the candidate instrument set — the SAME
  // survivors the fundamental X[0] would use, so M aligns across variants.
  ATX_TRY(ExposureMatrix x0, build_exposures(panel, cfg, /*row=*/0U, market_cap, group_id));

  // Complete-case return panel R (N×T): an instrument survives only if all T trailing
  // returns are clean. Order-fixed (ascending cross-section row, then ascending date).
  // newest date t == column 0.
  const atx::usize t_dates = window;
  std::vector<atx::usize> kept_inst; // universe index of each surviving asset
  std::vector<atx::f64> flat;        // row-major N×T scratch (asset-major)
  kept_inst.reserve(x0.instrument_rows.size());
  flat.reserve(x0.instrument_rows.size() * t_dates);
  std::vector<atx::f64> series(t_dates); // hoisted: reused (overwritten) per instrument
  for (const atx::usize inst : x0.instrument_rows) {
    bool complete = true;
    for (atx::usize t = 0U; t < t_dates && complete; ++t) {
      const atx::f64 r = detail::step_return(panel, t, inst);
      complete = !std::isnan(r);
      series[t] = r;
    }
    if (complete) {
      kept_inst.push_back(inst);
      for (const atx::f64 v : series) {
        flat.push_back(v);
      }
    }
  }

  const atx::usize n = kept_inst.size();
  const atx::usize k = n_stat;
  // The asymptotic-PCA validity conditions: N > T (the trick needs many names per date),
  // T > K and N > K (K factors are estimable from T dates / N assets).
  if (n <= t_dates || t_dates <= k || n <= k) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        "build_stat_factor_model: require N > T > K and N > K (complete-case panel)");
  }

  // Pack R (N×T) into a column-major MatX (asset rows, date columns; column 0 = newest),
  // then column-demean each asset row.
  atx::core::linalg::MatX r_panel(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(t_dates));
  for (atx::usize a = 0U; a < n; ++a) {
    for (atx::usize t = 0U; t < t_dates; ++t) {
      r_panel(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(t)) = flat[a * t_dates + t];
    }
  }
  detail::demean_rows(r_panel);

  // Pass 1 (equal-weighted): Fhat from the T×T Gram of R; B, s_n from R.
  ATX_TRY(atx::core::linalg::MatX fhat, detail::apca_factor_returns(r_panel, k));
  ATX_TRY(atx::core::linalg::MatX b, detail::exposures(r_panel, fhat));
  atx::core::linalg::VecX s = detail::specific_variances(r_panel, b, fhat);

  // Pass 2 (GLS, opt-in): re-extract Fhat from the 1/√s_n-reweighted Gram, then recover
  // the FINAL B and s_n from the UN-weighted R (original return scale).
  if (gls_reweight) {
    const atx::core::linalg::MatX r_w = detail::gls_reweight(r_panel, s);
    ATX_TRY(atx::core::linalg::MatX fhat_gls, detail::apca_factor_returns(r_w, k));
    fhat = std::move(fhat_gls);
    ATX_TRY(atx::core::linalg::MatX b_gls, detail::exposures(r_panel, fhat));
    b = std::move(b_gls);
    s = detail::specific_variances(r_panel, b, fhat);
  }

  // Assemble: X = B (N×K), F = LW-shrunk covariance of the factor-return series, D = s_n.
  atx::core::linalg::MatX f = detail::factor_covariance(fhat, factor_cov_shrink);
  return FactorModel::create(std::move(b), std::move(f), std::move(s), /*fit_begin=*/0U,
                             /*fit_end=*/window);
}

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_ols(const PanelView &panel, atx::usize window,
                                   std::span<const atx::f64> market_cap,
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

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_wls(const PanelView &panel, atx::usize window,
                                   std::span<const atx::f64> market_cap,
                                   std::span<const atx::u32> group_id,
                                   const atx::core::linalg::VecX &d0,
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
      u_by_inst[xs.instrument_rows[keep[j]]].push_back(fit->residuals[static_cast<Eigen::Index>(j)]);
    }
  }
  return atx::core::Ok(used);
}

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_robust(const PanelView &panel, atx::usize window,
                                      std::span<const atx::f64> market_cap,
                                      std::span<const atx::u32> group_id,
                                      const atx::core::linalg::VecX &d0,
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
    // Rank probe: cost::irls_huber fails LOUD (ATX_CHECK) on a rank-deficient design,
    // but a degenerate date must be SKIPPED here (matching the WLS pass's `if (!fit)`
    // skip). An OLS solve on the post-constraint design is the same rank test wls/irls
    // would apply, run once up front so the IRLS only ever sees a full-rank system.
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

atx::core::linalg::VecX
FactorModelBuilder::specific_variances(const ExposureMatrix &x0,
                                       const std::vector<std::vector<atx::f64>> &u_by_inst,
                                       atx::usize window) const {
  switch (cfg.cov.specific_method) {
  case SpecificRiskMethod::PopVariance: {
    const atx::usize m = x0.n_instruments();
    atx::core::linalg::VecX d(static_cast<Eigen::Index>(m));
    for (atx::usize r = 0U; r < m; ++r) {
      const atx::usize inst = x0.instrument_rows[r];
      d[static_cast<Eigen::Index>(r)] = detail::pop_variance(u_by_inst[inst]);
    }
    return d;
  }
  case SpecificRiskMethod::EwmaNeweyWestStructural: {
    // S8.8 short/long-horizon blend for D (opt-in via cfg.cov.horizon_blend). Run the
    // SAME structural specific-risk estimator at TWO specific half-lives and convex-blend
    // d = w·d_short + (1−w)·d_long (PSD-trivial: each entry stays positive). DEFAULT
    // horizon_blend == false ⇒ the single-horizon S8.4 path, byte-identical.
    const atx::core::linalg::VecX d_short =
        specific_risk_blend(x0, u_by_inst, window, cfg.cov.spec_halflife, cfg.cov.spec_nw_lags,
                            cfg.cov.structural_blend)
            .variances;
    if (!cfg.cov.horizon_blend) {
      return d_short;
    }
    const atx::core::linalg::VecX d_long =
        specific_risk_blend(x0, u_by_inst, window, cfg.cov.spec_halflife_long,
                            cfg.cov.spec_nw_lags, cfg.cov.structural_blend)
            .variances;
    return blend_specific(d_short, d_long, cfg.cov.horizon_blend_weight);
  }
  }
  return {}; // unreachable (switch exhaustive over SpecificRiskMethod)
}

} // namespace atx::engine::risk
