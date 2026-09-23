#pragma once

// atx::engine::risk — L7 realized P&L attribution + realized-vs-predicted TE check.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  attribute(w, X, f, r, cost, borrow) splits one period's book P&L into
//    factor_k  = (Xᵀw)_k · f_k                     (per-factor P&L)
//    specific  = Σ_i w_i · (r_i − x_iᵀ f)          (idiosyncratic P&L)
//    cost, borrow                                  (charged, positive = paid)
//    total     = Σ_i w_i r_i − cost − borrow
//  so that Σ_k factor_k + specific − cost − borrow == total up to rounding (the
//  specific term is computed directly from the residual returns, NOT as a plug, so the
//  identity is a real check of the inputs' consistency).
//
//  AttributionLedger folds daily attributions; TrackingErrorCheck compares the
//  realized active returns with the model's predicted tracking error (bias statistic
//  sd(r/σ̂) against the 1 ± √(2/T) band); risk_split gives the ex-ante factor /
//  specific variance decomposition of a book under a FactorModel.
//
//  A NaN asset return contributes 0 (a halted name's position is marked flat for the
//  period). All reductions are order-fixed (ascending asset, then factor). Header-only.

#include <cmath>  // std::isnan, std::sqrt, std::log
#include <span>   // std::span
#include <vector> // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/risk/exposures.hpp"    // ExposureMatrix
#include "atx/engine/risk/factor_model.hpp" // FactorModel

namespace atx::engine::risk {

struct Attribution {
  std::vector<atx::f64> factor; // per-factor P&L (length K)
  atx::f64 factor_total = 0.0;  // Σ factor
  atx::f64 specific = 0.0;      // Σ w_i u_i
  atx::f64 cost = 0.0;          // trading cost charged this period
  atx::f64 borrow = 0.0;        // short-borrow fee charged this period
  atx::f64 gross = 0.0;         // Σ w_i r_i
  atx::f64 total = 0.0;         // gross − cost − borrow

  // factor_total + specific − cost − borrow (== total up to rounding).
  [[nodiscard]] atx::f64 component_sum() const noexcept {
    return factor_total + specific - cost - borrow;
  }
};

// One period's attribution. `x` is M×K (row i = asset i of w / asset_ret),
// `factor_ret` length K. PRECONDITION: w.size() == asset_ret.size() == x.rows(),
// factor_ret.size() == x.cols() (debug-asserted).
[[nodiscard]] inline Attribution attribute(std::span<const atx::f64> w,
                                           const atx::core::linalg::MatX &x,
                                           std::span<const atx::f64> factor_ret,
                                           std::span<const atx::f64> asset_ret, atx::f64 cost,
                                           atx::f64 borrow) {
  const Eigen::Index m = x.rows();
  const Eigen::Index k = x.cols();
  ATX_ASSERT(w.size() == static_cast<atx::usize>(m));
  ATX_ASSERT(asset_ret.size() == static_cast<atx::usize>(m));
  ATX_ASSERT(factor_ret.size() == static_cast<atx::usize>(k));
  Attribution a;
  a.factor.assign(static_cast<atx::usize>(k), 0.0);
  a.cost = cost;
  a.borrow = borrow;
  std::vector<atx::f64> expo(static_cast<atx::usize>(k), 0.0); // (Xᵀw)_k
  for (Eigen::Index i = 0; i < m; ++i) {
    const atx::f64 wi = w[static_cast<atx::usize>(i)];
    const atx::f64 ri = asset_ret[static_cast<atx::usize>(i)];
    if (std::isnan(ri)) {
      continue; // halted name: flat for the period
    }
    atx::f64 fit = 0.0;
    for (Eigen::Index c = 0; c < k; ++c) {
      const atx::f64 xic = x(i, c);
      expo[static_cast<atx::usize>(c)] += xic * wi;
      fit += xic * factor_ret[static_cast<atx::usize>(c)];
    }
    a.gross += wi * ri;
    a.specific += wi * (ri - fit);
  }
  for (Eigen::Index c = 0; c < k; ++c) {
    const atx::f64 pnl = expo[static_cast<atx::usize>(c)] * factor_ret[static_cast<atx::usize>(c)];
    a.factor[static_cast<atx::usize>(c)] = pnl;
    a.factor_total += pnl;
  }
  a.total = a.gross - cost - borrow;
  return a;
}

// Overload over a per-date ExposureMatrix (its rows are the book's assets).
[[nodiscard]] inline Attribution attribute(std::span<const atx::f64> w, const ExposureMatrix &x,
                                           std::span<const atx::f64> factor_ret,
                                           std::span<const atx::f64> asset_ret, atx::f64 cost,
                                           atx::f64 borrow) {
  return attribute(w, x.x, factor_ret, asset_ret, cost, borrow);
}

// Running sum of daily attributions (cumulative, simple-sum P&L).
class AttributionLedger {
public:
  void add(const Attribution &a) {
    if (cum_.factor.size() < a.factor.size()) {
      cum_.factor.resize(a.factor.size(), 0.0);
    }
    for (atx::usize c = 0; c < a.factor.size(); ++c) {
      cum_.factor[c] += a.factor[c];
    }
    cum_.factor_total += a.factor_total;
    cum_.specific += a.specific;
    cum_.cost += a.cost;
    cum_.borrow += a.borrow;
    cum_.gross += a.gross;
    cum_.total += a.total;
    ++n_;
  }
  [[nodiscard]] const Attribution &cumulative() const noexcept { return cum_; }
  [[nodiscard]] atx::usize days() const noexcept { return n_; }

private:
  Attribution cum_;
  atx::usize n_ = 0U;
};

// Ex-ante variance split of a book under a factor model.
struct RiskSplit {
  atx::f64 factor_var = 0.0;   // (Xᵀw)ᵀ F (Xᵀw)
  atx::f64 specific_var = 0.0; // Σ D_i w_i²
  [[nodiscard]] atx::f64 total_var() const noexcept { return factor_var + specific_var; }
};

// PRECONDITION: w.size() == model.n_instruments().
[[nodiscard]] inline RiskSplit risk_split(const FactorModel &model, std::span<const atx::f64> w) {
  const atx::core::linalg::MatX &x = model.exposures();
  const atx::core::linalg::MatX &f = model.factor_cov();
  const atx::core::linalg::VecX &d = model.specific_var();
  ATX_ASSERT(w.size() == static_cast<atx::usize>(x.rows()));
  atx::core::linalg::VecX g = atx::core::linalg::VecX::Zero(x.cols());
  RiskSplit s;
  for (Eigen::Index i = 0; i < x.rows(); ++i) {
    const atx::f64 wi = w[static_cast<atx::usize>(i)];
    g += x.row(i).transpose() * wi;
    s.specific_var += d[i] * wi * wi;
  }
  s.factor_var = g.dot(f * g);
  return s;
}

// Realized-vs-predicted tracking-error check. Feed each period's realized ACTIVE return
// and the model's predicted TE (σ̂, same period horizon) for that period's book.
class TrackingErrorCheck {
public:
  // Non-positive / non-finite σ̂ or a NaN return is ignored.
  void add(atx::f64 realized_active, atx::f64 predicted_te) noexcept {
    if (std::isnan(realized_active) || !(predicted_te > 0.0) || std::isnan(predicted_te)) {
      return;
    }
    const atx::f64 z = realized_active / predicted_te;
    ++n_;
    sz_ += z;
    szz_ += z * z;
    sr_ += realized_active;
    srr_ += realized_active * realized_active;
    spred_ += predicted_te;
  }
  [[nodiscard]] atx::usize n() const noexcept { return n_; }
  // Bias statistic: sample std of z = r/σ̂ (≈ 1 for a well-calibrated model).
  [[nodiscard]] atx::f64 bias() const noexcept { return sample_sd(sz_, szz_); }
  // Half-width of the 95% band around 1 (√(2/T)).
  [[nodiscard]] atx::f64 band_half_width() const noexcept {
    return n_ == 0U ? 0.0 : std::sqrt(2.0 / static_cast<atx::f64>(n_));
  }
  [[nodiscard]] bool in_band() const noexcept {
    return n_ >= 2U && std::abs(bias() - 1.0) <= band_half_width();
  }
  [[nodiscard]] atx::f64 realized_te() const noexcept { return sample_sd(sr_, srr_); }
  [[nodiscard]] atx::f64 mean_predicted_te() const noexcept {
    return n_ == 0U ? 0.0 : spred_ / static_cast<atx::f64>(n_);
  }

private:
  [[nodiscard]] atx::f64 sample_sd(atx::f64 s, atx::f64 ss) const noexcept {
    if (n_ < 2U) {
      return 0.0;
    }
    const atx::f64 nf = static_cast<atx::f64>(n_);
    const atx::f64 var = (ss - s * s / nf) / (nf - 1.0);
    return var > 0.0 ? std::sqrt(var) : 0.0;
  }
  atx::usize n_ = 0U;
  atx::f64 sz_ = 0.0;
  atx::f64 szz_ = 0.0;
  atx::f64 sr_ = 0.0;
  atx::f64 srr_ = 0.0;
  atx::f64 spred_ = 0.0;
};

} // namespace atx::engine::risk
