#include "atx/engine/risk/factor_model.hpp"

// atx::engine::risk — FactorModel apply-math + FactorModelBuilder estimation +
// detail:: kernels (the BODIES). S8.8a header/source split: this TU holds the heavy
// estimation includes (Ledoit-Wolf combine, robust IRLS, EWMA/Newey-West, eigen-
// adjust, APCA, VRA, horizon-blend, specific-risk, regression) so the hub header
// (factor_model.hpp) no longer leaks them into its 42 dependents. PURE refactor —
// the math is byte-identical to the pre-split header-only implementation (R10).

#include <algorithm> // std::clamp, std::sort, std::minmax_element (W0-R0 D floor)
#include <cmath>     // std::isnan, std::isfinite, std::log, std::exp, std::sqrt
#include <span>      // std::span (PIT side inputs, residual counts)
#include <utility>   // std::move
#include <vector>    // std::vector (per-date designs, column maps)

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
  RiskEstimatorDiagnostics diagnostics;
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
                    atx::usize fit_begin, atx::usize fit_end, RiskEstimatorDiagnostics diagnostics) {
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
  impl->diagnostics = std::move(diagnostics);
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
const RiskEstimatorDiagnostics& FactorModel::estimator_diagnostics() const noexcept {
  return impl_->diagnostics;
}

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

std::vector<atx::usize> map_columns(const std::vector<ColumnTag> &date_cols,
                                    const std::vector<ColumnTag> &model_cols) {
  std::vector<atx::usize> out(date_cols.size(), kNoColumn);
  for (atx::usize c = 0U; c < date_cols.size(); ++c) {
    const ColumnTag &dc = date_cols[c];
    for (atx::usize j = 0U; j < model_cols.size(); ++j) { // K ≤ 256: linear scan is fine
      const ColumnTag &mc = model_cols[j];
      if (dc.kind != mc.kind) {
        continue;
      }
      const bool same = (dc.kind == ColumnTag::Kind::Style) ? (dc.style == mc.style)
                                                            : (dc.group_id == mc.group_id);
      if (same) {
        out[c] = j;
        break;
      }
    }
  }
  return out;
}

namespace {

// Median of a copy of `v` (order-fixed: full sort; even length ⇒ mean of the middle
// two). Empty ⇒ 0.0.
[[nodiscard]] atx::f64 median_of(std::vector<atx::f64> v) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const atx::usize n = v.size();
  return ((n % 2U) == 1U) ? v[n / 2U] : 0.5 * (v[n / 2U - 1U] + v[n / 2U]);
}

// cov.specific_floor_frac as applied: clamped to [0, 1]; a non-finite value (which
// std::clamp would pass through, and `!(d >= NaN)` would then turn every D into NaN)
// is treated as 0 = no global floor. The builder entry points reject it up front
// (InvalidArgument); this is the defence for direct detail:: callers.
[[nodiscard]] atx::f64 effective_floor_frac(atx::f64 frac) noexcept {
  return std::isfinite(frac) ? std::clamp(frac, 0.0, 1.0) : 0.0;
}

// The R-05 global floor, IN PLACE: median over the finite entries of `d`, then every
// entry below frac·median (or NaN) is raised to it. Fills st.median / st.floor /
// st.n_floored. Shared by the fundamental (floor_specific_variances) and statistical
// (build_stat_factor_model) builders so "no D < frac·median(D)" holds on both.
void floor_at_median(atx::core::linalg::VecX &d, atx::f64 frac, SpecificFloorStats &st) {
  const atx::usize m = static_cast<atx::usize>(d.size());
  std::vector<atx::f64> all;
  all.reserve(m);
  for (atx::usize r = 0U; r < m; ++r) {
    const atx::f64 dr = d[static_cast<Eigen::Index>(r)];
    if (std::isfinite(dr)) {
      all.push_back(dr);
    }
  }
  st.median = median_of(std::move(all));
  st.floor = (st.median > 0.0) ? effective_floor_frac(frac) * st.median : 0.0;
  for (atx::usize r = 0U; r < m; ++r) {
    const Eigen::Index ri = static_cast<Eigen::Index>(r);
    if (!(d[ri] >= st.floor)) { // also lifts a NaN entry to the floor
      d[ri] = st.floor;
      ++st.n_floored;
    }
  }
}

} // namespace

SpecificFloorStats floor_specific_variances(const ExposureMatrix &x0,
                                            std::span<const atx::usize> obs,
                                            const CovarianceConfig &cov,
                                            atx::core::linalg::VecX &d) {
  SpecificFloorStats st{};
  const atx::usize m = static_cast<atx::usize>(d.size());
  ATX_ASSERT(obs.size() == m);
  ATX_ASSERT(x0.n_instruments() == m);
  if (m == 0U) {
    return st;
  }
  atx::usize full = 0U;
  for (const atx::usize n : obs) {
    full = std::max(full, n);
  }
  st.min_obs = std::min(cov.specific_min_obs, std::max<atx::usize>(2U, (full + 1U) / 2U));

  // Thick anchor set: enough residuals and a usable (finite, positive) estimate.
  std::vector<atx::usize> thick;
  thick.reserve(m);
  std::vector<atx::f64> thick_d;
  thick_d.reserve(m);
  for (atx::usize r = 0U; r < m; ++r) {
    const atx::f64 dr = d[static_cast<Eigen::Index>(r)];
    if (obs[r] >= st.min_obs && std::isfinite(dr) && dr > 0.0) {
      thick.push_back(r);
      thick_d.push_back(dr);
    }
  }
  for (atx::usize r = 0U; r < m; ++r) {
    st.n_thin += (obs[r] < st.min_obs) ? 1U : 0U;
  }

  if (st.n_thin > 0U && !thick.empty()) {
    const atx::f64 med_thick = median_of(thick_d);
    const auto [min_it, max_it] = std::minmax_element(thick_d.begin(), thick_d.end());
    const atx::f64 d_lo = *min_it;
    const atx::f64 d_hi = *max_it;
    atx::core::linalg::VecX d_str =
        atx::core::linalg::VecX::Constant(static_cast<Eigen::Index>(m), med_thick);

    // Structural design: x0 columns with any non-zero entry on the thick set, plus an
    // intercept when there is no sector column (style z-scores are centred, so without
    // an intercept the fit would force the mean ln d to 0).
    bool has_sector = false;
    std::vector<Eigen::Index> cols;
    for (atx::usize c = 0U; c < x0.columns.size(); ++c) {
      const Eigen::Index ci = static_cast<Eigen::Index>(c);
      bool nonzero = false;
      for (const atx::usize r : thick) {
        nonzero = nonzero || (x0.x(static_cast<Eigen::Index>(r), ci) != 0.0);
      }
      if (nonzero) {
        cols.push_back(ci);
        has_sector = has_sector || (x0.columns[c].kind == ColumnTag::Kind::Sector);
      }
    }
    const Eigen::Index p =
        static_cast<Eigen::Index>(cols.size()) + (has_sector ? Eigen::Index{0} : Eigen::Index{1});
    const auto design_row = [&](atx::usize r, atx::core::linalg::MatX &z, Eigen::Index zr) {
      Eigen::Index zc = 0;
      if (!has_sector) {
        z(zr, zc++) = 1.0;
      }
      for (const Eigen::Index ci : cols) {
        z(zr, zc++) = x0.x(static_cast<Eigen::Index>(r), ci);
      }
    };
    if (static_cast<Eigen::Index>(thick.size()) > p) {
      atx::core::linalg::MatX z(static_cast<Eigen::Index>(thick.size()), p);
      atx::core::linalg::VecX y(static_cast<Eigen::Index>(thick.size()));
      for (atx::usize a = 0U; a < thick.size(); ++a) {
        design_row(thick[a], z, static_cast<Eigen::Index>(a));
        y[static_cast<Eigen::Index>(a)] = std::log(thick_d[a]);
      }
      const auto fit = atx::core::linalg::ols(z, y);
      if (fit) {
        atx::core::linalg::MatX zall(static_cast<Eigen::Index>(m), p);
        for (atx::usize r = 0U; r < m; ++r) {
          design_row(r, zall, static_cast<Eigen::Index>(r));
        }
        const atx::core::linalg::VecX pred = zall * fit->beta;
        for (atx::usize r = 0U; r < m; ++r) {
          d_str[static_cast<Eigen::Index>(r)] =
              std::clamp(std::exp(pred[static_cast<Eigen::Index>(r)]), d_lo, d_hi);
        }
        st.structural = true;
      }
    }

    const atx::f64 inv_min = 1.0 / static_cast<atx::f64>(st.min_obs); // min_obs >= 1 here
    for (atx::usize r = 0U; r < m; ++r) {
      if (obs[r] >= st.min_obs) {
        continue;
      }
      const Eigen::Index ri = static_cast<Eigen::Index>(r);
      const atx::f64 own = (std::isfinite(d[ri]) && d[ri] > 0.0) ? d[ri] : 0.0;
      const atx::f64 gamma = std::clamp(static_cast<atx::f64>(obs[r]) * inv_min, 0.0, 1.0);
      const atx::f64 sigma = gamma * std::sqrt(own) + (1.0 - gamma) * std::sqrt(d_str[ri]);
      d[ri] = sigma * sigma;
    }
  }

  // Global floor at frac · median(D) (median over finite entries, after the fallback).
  floor_at_median(d, cov.specific_floor_frac, st);
  return st;
}

} // namespace detail

// ===========================================================================
//  FactorModelBuilder — estimation bodies.
// ===========================================================================

atx::core::Result<FactorModel> FactorModelBuilder::build(const PanelView &panel, atx::usize window,
                                                         std::span<const atx::f64> market_cap,
                                                         std::span<const atx::u32> group_id) const {
  return build(panel, window, PitSideInputs::broadcast(market_cap, group_id));
}

atx::core::Result<FactorComponents>
FactorModelBuilder::build_components(const PanelView &panel, atx::usize window,
                                     std::span<const atx::f64> market_cap,
                                     std::span<const atx::u32> group_id) const {
  return build_components(panel, window, PitSideInputs::broadcast(market_cap, group_id));
}

atx::core::Result<ExposureMatrix>
FactorModelBuilder::regression_exposures(const PanelView &panel, atx::usize s,
                                         const PitSideInputs &side) const {
  return build_exposures(panel, cfg, detail::exposure_row(cfg.exposure_timing, s), side);
}

atx::core::Result<FactorReturnPanel>
FactorModelBuilder::factor_returns(const PanelView &panel, atx::usize window,
                                   const PitSideInputs &side) const {
  ExposureMatrix x0;
  atx::core::linalg::MatX fseries;
  std::vector<std::vector<atx::f64>> u_by_inst;
  std::vector<atx::usize> dates;
  std::vector<atx::usize> missing;
  ATX_TRY(atx::usize used, run_passes(panel, window, side, x0, fseries, u_by_inst, dates, missing));
  return atx::core::Ok(FactorReturnPanel{fseries.topRows(static_cast<Eigen::Index>(used)),
                                          std::move(x0.columns), std::move(dates),
                                          std::move(missing)});
}

atx::core::Result<FactorModel> FactorModelBuilder::build(const PanelView &panel, atx::usize window,
                                                         const PitSideInputs &side) const {
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
    if (cfg.cov.estimator.rule != RiskEstimatorRule::LegacyV1)
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
          "risk V2: statistical factors require HybridFactorModelBuilder");
    return build_stat_factor_model(panel, window, side, cfg, cfg.n_stat_factors,
                                   cfg.cov.apca_gls_reweight, cfg.factor_cov_shrink);
  }
  // Fundamental path: estimate (X, F, D) in build_components (the S7-3 augmentation seam),
  // then assemble. build_components returns EXACTLY the (X, F, D, fit_end) create consumes.
  ATX_TRY(FactorComponents comp, build_components(panel, window, side));
  return FactorModel::create(std::move(comp.X), std::move(comp.F), std::move(comp.D),
                             /*fit_begin=*/0U, /*fit_end=*/comp.fit_end, std::move(comp.diagnostics));
}

atx::core::Result<atx::usize>
FactorModelBuilder::run_passes(const PanelView &panel, atx::usize window,
                               const PitSideInputs &side, ExposureMatrix &x0,
                               atx::core::linalg::MatX &fseries,
                               std::vector<std::vector<atx::f64>> &u_by_inst,
                               std::vector<atx::usize> &dates,
                               std::vector<atx::usize> &missing,
                               atx::core::linalg::MatX* dated_residuals) const {
  if (cfg.n_stat_factors > 0U || cfg.n_dead_factors > 0U) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          "FactorModelBuilder::build_components: stat/dead rungs are dispatched "
                          "by build()"); // build() handles APCA / the dead-alpha deferral
  }
  if (window < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: require window >= 2");
  }
  if (!std::isfinite(cfg.cov.specific_floor_frac)) { // NaN/Inf would poison every D and d0
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: specific_floor_frac must be "
                          "finite");
  }
  const atx::usize n_inst = panel.instruments();
  if (cfg.cov.estimator.rule != RiskEstimatorRule::LegacyV1 &&
      cfg.cov.estimator.rule != RiskEstimatorRule::EffectiveHistoryV2)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "risk: unknown estimator rule");
  ATX_TRY_VOID(side.validate(n_inst));
  // Point-in-time side inputs must cover every exposure row the passes read: rows
  // [0, exposure_row(window − 1)] clipped to the panel (R-06; never a silent reuse).
  if (!side.is_static() && panel.rows() > 0U) {
    const atx::usize last_row =
        std::min(detail::exposure_row(cfg.exposure_timing, window - 1U), panel.rows() - 1U);
    if (!side.covers(last_row)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModelBuilder: point-in-time side inputs must cover every "
                            "exposure row of the window");
    }
  }
  if (cfg.cov.estimator.rule == RiskEstimatorRule::EffectiveHistoryV2) {
    const auto limit = static_cast<atx::usize>(FactorModel::kMaxFactorsStack);
    const auto share = cfg.cov.estimator.max_working_bytes / 3;
    if (n_inst == 0 || window > panel.rows() || window > share / 64 / n_inst)
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "risk V2: dated residual workspace budget");
    atx::usize k_bound = 0;
    // Count bounded distinct groups BEFORE build_exposures allocates M x K.
    for (atx::usize s = 0; s <= window; ++s) {
      const auto row = s == 0 ? 0 : detail::exposure_row(cfg.exposure_timing, s - 1);
      if (row >= panel.rows() || !side.covers(row))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "risk V2: incomplete exposure clock");
      const auto groups = side.group_at(row,n_inst);
      const auto styles = detail::emitted_styles(cfg,!side.cap_at(row,n_inst).empty()).size();
      std::vector<atx::u32> unique;
      unique.reserve(limit);
      if (cfg.sector_factors)
        for (auto g : groups) {
          if (g == kNoGroup || std::find(unique.begin(),unique.end(),g) != unique.end()) continue;
          if (unique.size() >= limit - styles)
            return atx::core::Err(atx::core::ErrorCode::OutOfRange, "risk V2: factor dimension bound before allocation");
          unique.push_back(g);
        }
      k_bound = std::max(k_bound, styles + unique.size());
    }
    if (k_bound == 0 || k_bound > share / 128 / k_bound || n_inst > share / 128 / k_bound)
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "risk V2: factor workspace budget");
  }
  if (dated_residuals != nullptr) {
    if (n_inst == 0 || window > cfg.cov.estimator.max_working_bytes / 64 / n_inst)
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "risk V2: residual workspace budget");
    dated_residuals->setConstant(static_cast<Eigen::Index>(window),
        static_cast<Eigen::Index>(n_inst), std::numeric_limits<atx::f64>::quiet_NaN());
  }
  // X[0] (the CURRENT cross-section) defines M and the emitted factor count K.
  ATX_TRY(ExposureMatrix x0_built, build_exposures(panel, cfg, /*row=*/0U, side));
  x0 = std::move(x0_built);
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
  atx::core::linalg::VecX d0(static_cast<Eigen::Index>(n_inst));
  ATX_TRY(atx::usize used_a, accumulate_ols(panel, window, side, d0));
  if (used_a < 2U) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        "FactorModelBuilder::build_components: too few usable dates (M_s < K everywhere)");
  }

  u_by_inst.assign(n_inst, std::vector<atx::f64>{}); // final residuals per universe inst
  fseries.setZero(static_cast<Eigen::Index>(window), static_cast<Eigen::Index>(k));
  dates.clear();
  missing.assign(k, 0U);
  // Pass B dispatch: robust root-cap + Huber IRLS when opted in (cfg.cov), else the
  // P4 plain inverse-specific-variance WLS. Both emit the SAME (window×K) factor-
  // return series + per-instrument residuals downstream; the default keeps P4 exactly.
  ATX_TRY(atx::usize used_b,
          cfg.cov.robust_regression
              ? accumulate_robust(panel, window, side, x0, d0, fseries, u_by_inst, dates, missing, dated_residuals)
              : accumulate_wls(panel, window, side, x0, d0, fseries, u_by_inst, dates, missing, dated_residuals));
  if (used_b < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: too few usable WLS dates");
  }
  return atx::core::Ok(used_b);
}

atx::core::Result<FactorComponents>
FactorModelBuilder::build_components(const PanelView &panel, atx::usize window,
                                     const PitSideInputs &side) const {
  const bool v2 = cfg.cov.estimator.rule == RiskEstimatorRule::EffectiveHistoryV2;
  atx::core::linalg::MatX dated;
  ExposureMatrix x0;
  atx::core::linalg::MatX fseries;
  std::vector<std::vector<atx::f64>> u_by_inst;
  std::vector<atx::usize> dates;
  std::vector<atx::usize> missing;
  ATX_TRY(atx::usize used_b,
          run_passes(panel, window, side, x0, fseries, u_by_inst, dates, missing, v2 ? &dated : nullptr));
  const atx::usize k = x0.n_factors();
  // Compact fseries to the rows actually filled (under-determined dates skipped).
  // V1 retains zero returns for absent model factors. Explicit V2 marks them
  // missing and estimates covariance on observed pairs with original date ages.
  const atx::core::linalg::MatX fkept = fseries.topRows(static_cast<Eigen::Index>(used_b));
  if (fkept.rows() < static_cast<Eigen::Index>(k)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FactorModelBuilder::build_components: usable dates < K (factor cov rank)");
  }

  if (v2) {
    const auto m = x0.n_instruments();
    const auto caps = side.cap_at(0, panel.instruments());
    if (caps.size() != panel.instruments())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "risk V2: current market caps required");
    atx::core::linalg::MatX residuals(static_cast<Eigen::Index>(dates.size()),
                                     static_cast<Eigen::Index>(m));
    std::vector<atx::f64> current_caps(m);
    for (atx::usize i = 0; i < m; ++i) {
      current_caps[i] = caps[x0.instrument_rows[i]];
      for (atx::usize t = 0; t < dates.size(); ++t)
        residuals(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(i)) =
            dated(static_cast<Eigen::Index>(dates[t]), static_cast<Eigen::Index>(x0.instrument_rows[i]));
    }
    ATX_TRY(auto clean, clean_risk_estimates_v2(fkept, residuals, x0.x, current_caps,
        dates, cfg.cov.estimator, prior_forecasts));
    return atx::core::Ok(FactorComponents{std::move(x0.x), std::move(clean.factor_covariance),
        std::move(clean.specific_variances), window, std::move(clean.diagnostics)});
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
  // R-05 thin-name floor (DEFAULT StructuralMedianV2; NoneV1 = the pre-W0 D, which
  // FactorModel::create only floors at kSpecificVarFloor). EXHAUSTIVE switch.
  switch (cfg.cov.specific_floor) {
  case SpecificFloorRule::NoneV1:
    break;
  case SpecificFloorRule::StructuralMedianV2: {
    std::vector<atx::usize> obs(x0.n_instruments());
    for (atx::usize r = 0U; r < obs.size(); ++r) {
      obs[r] = u_by_inst[x0.instrument_rows[r]].size();
    }
    const detail::SpecificFloorStats st =
        detail::floor_specific_variances(x0, std::span<const atx::usize>{obs}, cfg.cov, d);
    ATX_UNUSED(st);
    break;
  }
  }
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
//    6. X = B, F = factor_covariance(Fhat, factor_cov_shrink), D = s_n floored at
//       specific_floor_frac·median(s_n) under SpecificFloorRule::StructuralMedianV2
//       (W0-R0, R-05; NoneV1 = the raw s_n); create.
//  PIT-structural; RNG-free, order-fixed ⇒ byte-identical on replay (Fhat sign-pinned).
// ===========================================================================
atx::core::Result<FactorModel> FactorModelBuilder::build_stat_factor_model(
    const PanelView &panel, atx::usize window, const PitSideInputs &side,
    const FactorModelConfig &cfg, atx::usize n_stat, bool gls_reweight,
    atx::f64 factor_cov_shrink) {
  if (window < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_stat_factor_model: require window >= 2");
  }
  if (!std::isfinite(cfg.cov.specific_floor_frac)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_stat_factor_model: specific_floor_frac must be finite");
  }
  // The current cross-section (row 0) defines the candidate instrument set — the SAME
  // survivors the fundamental X[0] would use, so M aligns across variants.
  ATX_TRY(ExposureMatrix x0, build_exposures(panel, cfg, /*row=*/0U, side));

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

  // R-05 on the statistical variant: every asset here has all T residuals (complete
  // case), so there are no thin names, but a near-stale series can still produce a
  // tiny s_n. Under StructuralMedianV2 apply the same global floor as the fundamental
  // builder: s_n >= specific_floor_frac·median(s). NoneV1 keeps the pre-W0 s_n.
  switch (cfg.cov.specific_floor) {
  case SpecificFloorRule::NoneV1:
    break;
  case SpecificFloorRule::StructuralMedianV2: {
    detail::SpecificFloorStats st{};
    detail::floor_at_median(s, cfg.cov.specific_floor_frac, st);
    break;
  }
  }

  // Assemble: X = B (N×K), F = LW-shrunk covariance of the factor-return series, D = s_n.
  atx::core::linalg::MatX f = detail::factor_covariance(fhat, factor_cov_shrink);
  return FactorModel::create(std::move(b), std::move(f), std::move(s), /*fit_begin=*/0U,
                             /*fit_end=*/window);
}

namespace {

// One estimation date's regression inputs (W0-R0). `xs` is the exposure block that
// explains r_s — built at detail::exposure_row(cfg.exposure_timing, s) with THAT row's
// side inputs (R-03, R-06) — `r` the clean returns r_s over its rows, `keep` the kept
// row indices into xs.instrument_rows, `xsr` the matching design rows, `cap` the
// exposure date's cap slice (robust-path weights).
struct DateDesign {
  ExposureMatrix xs;
  atx::core::linalg::VecX r;
  std::vector<atx::usize> keep;
  atx::core::linalg::MatX xsr;
  std::span<const atx::f64> cap;
};

// Fill `out` for estimation date s. Ok(false) = the date is unusable (no close at s+1
// for r_s, no exposure row inside the panel, no factor columns, or fewer clean rows
// than the date's OWN column count K_s — R-04: never the model's K). Err propagates
// build_exposures (malformed / non-covering side inputs).
[[nodiscard]] atx::core::Result<bool> date_design(const PanelView &panel,
                                                  const FactorModelConfig &cfg, atx::usize s,
                                                  const PitSideInputs &side, DateDesign &out) {
  if (s + 1U >= panel.rows()) {
    return atx::core::Ok(false); // r_s = close(s)/close(s+1) − 1 needs row s+1
  }
  const atx::usize xrow = detail::exposure_row(cfg.exposure_timing, s);
  if (xrow >= panel.rows()) {
    return atx::core::Ok(false);
  }
  ATX_TRY(ExposureMatrix xs, build_exposures(panel, cfg, xrow, side));
  out.xs = std::move(xs);
  out.r = detail::date_returns(panel, s, out.xs, out.keep);
  // A sector whose every member lost its return at s has an all-zero dummy on the kept
  // rows: drop that column (it has no factor return this date) instead of letting the
  // rank-deficient design skip the whole date (R-04). Style columns are kept as is.
  std::vector<atx::usize> kept_cols;
  kept_cols.reserve(out.xs.columns.size());
  for (atx::usize c = 0U; c < out.xs.columns.size(); ++c) {
    bool live = (out.xs.columns[c].kind != ColumnTag::Kind::Sector);
    for (atx::usize j = 0U; j < out.keep.size() && !live; ++j) {
      live = out.xs.x(static_cast<Eigen::Index>(out.keep[j]), static_cast<Eigen::Index>(c)) != 0.0;
    }
    if (live) {
      kept_cols.push_back(c);
    }
  }
  if (kept_cols.size() != out.xs.columns.size()) {
    atx::core::linalg::MatX xc(out.xs.x.rows(), static_cast<Eigen::Index>(kept_cols.size()));
    std::vector<ColumnTag> tags;
    tags.reserve(kept_cols.size());
    for (atx::usize c = 0U; c < kept_cols.size(); ++c) {
      xc.col(static_cast<Eigen::Index>(c)) = out.xs.x.col(static_cast<Eigen::Index>(kept_cols[c]));
      tags.push_back(out.xs.columns[kept_cols[c]]);
    }
    out.xs.x = std::move(xc);
    out.xs.columns = std::move(tags);
  }
  const atx::usize ks = out.xs.n_factors();
  if (ks == 0U || out.keep.size() < ks) {
    return atx::core::Ok(false); // under-determined cross-section (M_s < K_s)
  }
  out.xsr = detail::select_rows(out.xs.x, out.keep);
  out.cap = side.cap_at(xrow, panel.instruments());
  return atx::core::Ok(true);
}

// Scatter one date's coefficient vector (length K_s, the DATE's column order) into
// row `u` of the window×K factor-return series by column IDENTITY (R-04). Model
// columns without a date column stay 0 and are counted in `missing`; date columns the
// model lacks (a group absent today) are regressed on but not carried.
template <class Coef>
void scatter_factor_returns(const Coef &beta, const std::vector<atx::usize> &col_map,
                            atx::core::linalg::MatX &fseries, atx::usize u,
                            std::vector<atx::usize> &missing, bool missing_is_nan = false) {
  const atx::usize k = static_cast<atx::usize>(fseries.cols());
  std::vector<bool> hit(k, false);
  for (atx::usize c = 0U; c < col_map.size(); ++c) {
    const atx::usize j = col_map[c];
    if (j == detail::kNoColumn) {
      continue;
    }
    ATX_ASSERT(j < k);
    fseries(static_cast<Eigen::Index>(u), static_cast<Eigen::Index>(j)) =
        beta[static_cast<Eigen::Index>(c)];
    hit[j] = true;
  }
  for (atx::usize j = 0U; j < k; ++j) {
    if (!hit[j]) {
      fseries(static_cast<Eigen::Index>(u), static_cast<Eigen::Index>(j)) =
          missing_is_nan ? std::numeric_limits<atx::f64>::quiet_NaN() : 0.0;
      ++missing[j];
    }
  }
}

} // namespace

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_ols(const PanelView &panel, atx::usize window,
                                   const PitSideInputs &side,
                                   atx::core::linalg::VecX &d0_out) const {
  const atx::usize n_inst = panel.instruments();
  std::vector<std::vector<atx::f64>> resid(n_inst); // OLS residual series per universe inst
  DateDesign dd;
  atx::usize used = 0U;
  for (atx::usize s = 0U; s < window; ++s) {
    ATX_TRY(bool usable, date_design(panel, cfg, s, side, dd));
    if (!usable) {
      continue; // no prior close / under-determined cross-section -> skip this date
    }
    const auto fit = atx::core::linalg::ols(dd.xsr, dd.r);
    if (!fit) {
      continue; // rank-deficient date -> skip (e.g. a degenerate sector block)
    }
    ++used;
    for (atx::usize j = 0U; j < dd.keep.size(); ++j) {
      resid[dd.xs.instrument_rows[dd.keep[j]]].push_back(
          fit->residuals[static_cast<Eigen::Index>(j)]);
    }
  }
  for (atx::usize i = 0U; i < n_inst; ++i) {
    const atx::f64 v = detail::pop_variance(resid[i]);
    d0_out[static_cast<Eigen::Index>(i)] = (v < kBootstrapVarFloor) ? kBootstrapVarFloor : v;
  }
  // R-05 applies to the pass-B WEIGHTS too: a 0/1-residual name has d0 = 1e-12, i.e. a
  // 1e12 WLS weight that lets it dictate its sector's factor return. Under
  // StructuralMedianV2 thin names shrink toward the median d0 of the well-observed
  // names and every d0 is floored at specific_floor_frac·median (same thresholds as D).
  if (cfg.cov.specific_floor == SpecificFloorRule::StructuralMedianV2) {
    atx::usize full = 0U;
    for (const auto &series : resid) {
      full = std::max(full, series.size());
    }
    const atx::usize min_obs =
        std::min(cfg.cov.specific_min_obs, std::max<atx::usize>(2U, (full + 1U) / 2U));
    std::vector<atx::f64> thick;
    for (atx::usize i = 0U; i < n_inst; ++i) {
      if (!resid[i].empty() && resid[i].size() >= min_obs) {
        thick.push_back(d0_out[static_cast<Eigen::Index>(i)]);
      }
    }
    if (!thick.empty() && min_obs > 0U) {
      const atx::f64 med_thick = detail::median_of(std::move(thick));
      for (atx::usize i = 0U; i < n_inst; ++i) {
        const atx::usize n = resid[i].size();
        if (n == 0U || n >= min_obs) {
          continue;
        }
        const Eigen::Index ii = static_cast<Eigen::Index>(i);
        const atx::f64 gamma = static_cast<atx::f64>(n) / static_cast<atx::f64>(min_obs);
        const atx::f64 sigma = gamma * std::sqrt(d0_out[ii]) + (1.0 - gamma) * std::sqrt(med_thick);
        d0_out[ii] = std::max(sigma * sigma, kBootstrapVarFloor);
      }
    }
    std::vector<atx::f64> seen; // every weighted name's d0, after the thin fallback
    for (atx::usize i = 0U; i < n_inst; ++i) {
      if (!resid[i].empty()) { // never regressed -> never weighted
        seen.push_back(d0_out[static_cast<Eigen::Index>(i)]);
      }
    }
    const atx::f64 floor = detail::effective_floor_frac(cfg.cov.specific_floor_frac) *
                           detail::median_of(std::move(seen));
    for (atx::usize i = 0U; i < n_inst; ++i) {
      const Eigen::Index ii = static_cast<Eigen::Index>(i);
      if (!resid[i].empty() && d0_out[ii] < floor) {
        d0_out[ii] = floor;
      }
    }
  }
  return atx::core::Ok(used);
}

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_wls(const PanelView &panel, atx::usize window,
                                   const PitSideInputs &side, const ExposureMatrix &x0,
                                   const atx::core::linalg::VecX &d0,
                                   atx::core::linalg::MatX &fseries,
                                   std::vector<std::vector<atx::f64>> &u_by_inst,
                                   std::vector<atx::usize> &dates,
                                   std::vector<atx::usize> &missing,
                               atx::core::linalg::MatX* dated_residuals) const {
  DateDesign dd;
  atx::usize used = 0U;
  for (atx::usize s = 0U; s < window; ++s) {
    ATX_TRY(bool usable, date_design(panel, cfg, s, side, dd));
    if (!usable) {
      continue; // under-determined -> skip (matches Pass A's skip rule)
    }
    atx::core::linalg::VecX w(static_cast<Eigen::Index>(dd.keep.size())); // 1/d0_i (P4-2 IVW)
    for (atx::usize j = 0U; j < dd.keep.size(); ++j) {
      w[static_cast<Eigen::Index>(j)] =
          1.0 / d0[static_cast<Eigen::Index>(dd.xs.instrument_rows[dd.keep[j]])];
    }
    const auto fit = atx::core::linalg::wls(dd.xsr, dd.r, w);
    if (!fit) {
      continue; // rank-deficient weighted date -> skip
    }
    ATX_ASSERT(static_cast<atx::usize>(fit->beta.size()) == dd.xs.n_factors());
    scatter_factor_returns(fit->beta, detail::map_columns(dd.xs.columns, x0.columns), fseries,
                           used, missing, cfg.cov.estimator.rule == RiskEstimatorRule::EffectiveHistoryV2);
    dates.push_back(s);
    ++used;
    for (atx::usize j = 0U; j < dd.keep.size(); ++j) {
      const auto inst = dd.xs.instrument_rows[dd.keep[j]];
      const auto value = fit->residuals[static_cast<Eigen::Index>(j)];
      u_by_inst[inst].push_back(value);
      if (dated_residuals != nullptr)
        (*dated_residuals)(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(inst)) = value;
    }
  }
  return atx::core::Ok(used);
}

atx::core::Result<atx::usize>
FactorModelBuilder::accumulate_robust(const PanelView &panel, atx::usize window,
                                      const PitSideInputs &side, const ExposureMatrix &x0,
                                      const atx::core::linalg::VecX &d0,
                                      atx::core::linalg::MatX &fseries,
                                      std::vector<std::vector<atx::f64>> &u_by_inst,
                                      std::vector<atx::usize> &dates,
                                      std::vector<atx::usize> &missing,
                               atx::core::linalg::MatX* dated_residuals) const {
  const cost::RobustCfg rcfg{/*huber_k=*/cfg.cov.huber_c, /*max_iter=*/cfg.cov.robust_iters,
                             /*tol=*/0.0};
  DateDesign dd;
  atx::usize used = 0U;
  for (atx::usize s = 0U; s < window; ++s) {
    ATX_TRY(bool usable, date_design(panel, cfg, s, side, dd));
    if (!usable) {
      continue; // under-determined -> skip (matches the WLS pass's skip rule)
    }
    if (cfg.cov.industry_sum_to_zero) {
      detail::apply_industry_sum_to_zero(dd.xsr, dd.xs, dd.keep, dd.cap);
    }
    // Rank probe: cost::irls_huber fails LOUD (ATX_CHECK) on a rank-deficient design,
    // but a degenerate date must be SKIPPED here (matching the WLS pass's `if (!fit)`
    // skip). An OLS solve on the post-constraint design is the same rank test wls/irls
    // would apply, run once up front so the IRLS only ever sees a full-rank system.
    if (!atx::core::linalg::ols(dd.xsr, dd.r)) {
      continue; // rank-deficient (e.g. a collinear sector block) -> skip this date
    }
    const atx::core::linalg::VecX w0 =
        detail::robust_prior_weight(dd.xs, dd.keep, d0, dd.cap, cfg.cov.cap_weight);
    const cost::RobustFit fit = cost::irls_huber(dd.xsr, dd.r, rcfg, &w0);
    ATX_ASSERT(static_cast<atx::usize>(fit.beta.size()) == dd.xs.n_factors());
    scatter_factor_returns(fit.beta, detail::map_columns(dd.xs.columns, x0.columns), fseries,
                           used, missing, cfg.cov.estimator.rule == RiskEstimatorRule::EffectiveHistoryV2);
    dates.push_back(s);
    ++used;
    for (atx::usize j = 0U; j < dd.keep.size(); ++j) {
      const auto inst = dd.xs.instrument_rows[dd.keep[j]];
      u_by_inst[inst].push_back(fit.residuals[j]);
      if (dated_residuals != nullptr)
        (*dated_residuals)(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(inst)) = fit.residuals[j];
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
