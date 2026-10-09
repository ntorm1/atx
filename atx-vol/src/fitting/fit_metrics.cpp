#include "atx/vol/api/fitting/fit_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

double vol_error_bar(double bid_price, double ask_price, double vega,
                     double min_bar, double max_bar) noexcept {
  const double half_spread = 0.5 * (ask_price - bid_price);
  // A vanishing / non-finite vega carries no vol information about the price
  // spread — return the widest bar rather than divide by (near) zero.
  if (!(vega > 0.0) || !std::isfinite(vega) || !std::isfinite(half_spread)) {
    return max_bar;
  }
  double bar = half_spread / vega;
  if (bar < min_bar) bar = min_bar;  // zero / crossed spread → floor
  if (bar > max_bar) bar = max_bar;  // tiny vega → cap
  return bar;
}

Result<ChiSquareResult> reduced_chi_square(std::span<const double> resid_vol,
                                           std::span<const double> err_bar_vol,
                                           std::size_t dof) noexcept {
  if (resid_vol.size() != err_bar_vol.size()) {
    return Err(ErrorCode::InvalidArgument,
               "reduced_chi_square: resid/err_bar length mismatch");
  }
  const std::size_t n = resid_vol.size();
  if (n == 0) {
    return Err(ErrorCode::InvalidArgument, "reduced_chi_square: empty input");
  }
  if (n <= dof) {
    return Err(ErrorCode::InvalidArgument,
               "reduced_chi_square: N must exceed dof");
  }

  double chi2 = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double err = err_bar_vol[i];
    if (!(err > 0.0) || !std::isfinite(err)) {
      return Err(ErrorCode::InvalidArgument,
                 "reduced_chi_square: err_bar must be finite and > 0");
    }
    const double z = resid_vol[i] / err;
    chi2 += z * z;
  }

  const double denom = static_cast<double>(n - dof);
  return Ok(ChiSquareResult{chi2, chi2 / denom, n, dof});
}

EdgeResult minimum_edge(double iv_model, double iv_mkt, double err_bar_vol,
                        double k) noexcept {
  const double edge = iv_model - iv_mkt;
  const double band = k * err_bar_vol;
  const bool within = std::fabs(edge) < band;
  const double n_sigma = (err_bar_vol > 0.0) ? (edge / err_bar_vol) : 0.0;
  return EdgeResult{within, edge, n_sigma};
}

double avg_abs_error_e5(std::span<const double> resid) noexcept {
  const std::size_t n = resid.size();
  if (n == 0) return 0.0;
  double acc = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    acc += std::fabs(resid[i]);
  }
  return (acc / static_cast<double>(n)) * 1.0e5;
}

Result<SliceFitMetrics> slice_fit_metrics(std::span<const double> iv_model,
                                          std::span<const double> iv_mkt,
                                          std::span<const double> bid_price,
                                          std::span<const double> ask_price,
                                          std::span<const double> vega,
                                          std::size_t dof) noexcept {
  const std::size_t n = iv_model.size();
  if (iv_mkt.size() != n || bid_price.size() != n || ask_price.size() != n ||
      vega.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "slice_fit_metrics: input length mismatch");
  }
  if (n == 0) {
    return Err(ErrorCode::InvalidArgument, "slice_fit_metrics: empty slice");
  }
  if (n <= dof) {
    return Err(ErrorCode::InvalidArgument,
               "slice_fit_metrics: N must exceed dof");
  }

  double sum_sq = 0.0;     // Σ r²                      (unweighted)
  double sum_w_sq = 0.0;   // Σ w·r²  ==  Σ (r/σ_err)²  (== χ²)
  double sum_w = 0.0;      // Σ w      (w = 1/σ_err²)
  double sum_abs = 0.0;    // Σ |r|
  std::size_t n_within = 0;

  for (std::size_t i = 0; i < n; ++i) {
    const double resid = iv_model[i] - iv_mkt[i];
    // err ≥ min_bar > 0 by construction, so every divisor below is positive.
    const double err = vol_error_bar(bid_price[i], ask_price[i], vega[i]);
    const double inv = 1.0 / err;
    const double w = inv * inv;

    sum_sq += resid * resid;
    sum_w_sq += w * resid * resid;
    sum_w += w;
    sum_abs += std::fabs(resid);
    if (std::fabs(resid) < err) {
      ++n_within;
    }
  }

  const double dn = static_cast<double>(n);
  SliceFitMetrics out{};
  out.rmse_vol = std::sqrt(sum_sq / dn);
  out.rmse_vol_weighted = (sum_w > 0.0) ? std::sqrt(sum_w_sq / sum_w) : 0.0;
  out.chi2_reduced = sum_w_sq / static_cast<double>(n - dof);
  out.avE5_vol = (sum_abs / dn) * 1.0e5;
  out.n = n;
  out.n_within_band = n_within;
  return Ok(out);
}

Result<BandViolationStats> band_violation_stats(
    std::span<const double> model_price, std::span<const double> bid_price,
    std::span<const double> ask_price) noexcept {
  const std::size_t n_in = model_price.size();
  if (bid_price.size() != n_in || ask_price.size() != n_in) {
    return Err(ErrorCode::InvalidArgument,
               "band_violation_stats: input length mismatch");
  }

  BandViolationStats out{};
  out.max_err_idx = static_cast<std::size_t>(-1);
  // Any real (scored) violation is >= 0, so this sentinel always loses to the
  // first scored quote, letting `viol > best_viol` alone drive first-index
  // tie-breaking without a separate "have we scored anything yet" flag.
  double best_viol = -1.0;
  double sum_signed = 0.0;

  for (std::size_t i = 0; i < n_in; ++i) {
    const double bid = bid_price[i];
    const double ask = ask_price[i];
    if (!(std::isfinite(bid) && std::isfinite(ask) && ask >= bid)) {
      // Crossed quote (ask < bid) or a non-finite bid/ask: neither defines a
      // valid band to violate, and a NaN/Inf bound would otherwise poison
      // max_prc_err / avg_signed_err below — skip either way (not scored).
      continue;
    }
    const double p = model_price[i];
    if (!std::isfinite(p)) {
      continue;  // undefined model price: skip rather than contaminate stats
    }

    const double bid_viol = bid - p;  // > 0 iff model_price < bid
    const double ask_viol = p - ask;  // > 0 iff model_price > ask
    if (bid_viol > 0.0) ++out.n_bid_miss;
    if (ask_viol > 0.0) ++out.n_ask_miss;

    const double viol = std::max({bid_viol, ask_viol, 0.0});
    if (viol > best_viol) {
      best_viol = viol;
      out.max_err_idx = i;
    }

    sum_signed += p - 0.5 * (bid + ask);
    ++out.n;
  }

  out.max_prc_err = (out.n > 0) ? best_viol : 0.0;
  out.avg_signed_err =
      (out.n > 0) ? sum_signed / static_cast<double>(out.n) : 0.0;
  return Ok(out);
}

}  // namespace atx::vol
