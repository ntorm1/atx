#include "atx/vol/dividend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

double hybrid_forward(double S, double r, double borrow, double T,
                      std::span<const DividendEvent> cash_divs,
                      std::int64_t expiry_ns, std::int64_t now_ts_ns,
                      const HybridDivParams &hyb) noexcept {
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r) || !std::isfinite(borrow) ||
      !std::isfinite(hyb.prop_div_yield) || !std::isfinite(hyb.blend)) {
    return kQuietNaN;
  }

  // Pure escrowed-cash forward (Battig-Jarrow), reused verbatim so blend == 0
  // reproduces it bit-for-bit and out-of-window dividends are handled once.
  const double f_cash =
      forward_div_corrected(S, r, T, cash_divs, expiry_ns, now_ts_ns);

  const double q = hyb.prop_div_yield;
  const double beta = hyb.blend;

  // G is the blend of the proportional base (β·S·e^{(r−βq)T}) and the escrowed
  // base ((1−β)·F_cash·e^{−βqT}); the borrow enters only through e^{−bT}, which
  // makes F strictly decreasing in `borrow` and monotone for PCP inversion.
  const double g = beta * S * std::exp((r - beta * q) * T) +
                   (1.0 - beta) * f_cash * std::exp(-beta * q * T);
  return g * std::exp(-borrow * T);
}

Result<double> imply_borrow_european_pcp(double call_price, double put_price,
                                         double S, double K, double T, double r,
                                         std::span<const DividendEvent> cash_divs,
                                         std::int64_t expiry_ns,
                                         std::int64_t now_ts_ns,
                                         const HybridDivParams &hyb, double b_lo,
                                         double b_hi, double tol) noexcept {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !std::isfinite(r) ||
      !std::isfinite(call_price) || !std::isfinite(put_price)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: non-finite or non-positive input");
  }
  if (!(b_lo < b_hi)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: require b_lo < b_hi");
  }
  if (!(tol > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: tol must be positive");
  }

  const double df = std::exp(-r * T);
  const double lhs = call_price - put_price; // observed C − P

  // Objective g(b) = e^{−rT}(F(b) − K) − (C − P). Strictly decreasing in b.
  const auto obj = [&](double b) noexcept {
    const double f =
        hybrid_forward(S, r, b, T, cash_divs, expiry_ns, now_ts_ns, hyb);
    return df * (f - K) - lhs;
  };

  double lo = b_lo;
  double hi = b_hi;
  double g_lo = obj(lo);
  double g_hi = obj(hi);
  if (!std::isfinite(g_lo) || !std::isfinite(g_hi)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: non-finite forward at bracket");
  }
  if (g_lo == 0.0) {
    return Ok(lo);
  }
  if (g_hi == 0.0) {
    return Ok(hi);
  }
  if ((g_lo > 0.0) == (g_hi > 0.0)) {
    return Err(ErrorCode::OutOfRange,
               "imply_borrow_european_pcp: implied borrow outside [b_lo, b_hi]");
  }

  // Bisection: bounded iteration cap (JPL Rule 2); 200 halvings drives any
  // sane bracket below `tol` long before the cap.
  constexpr int kMaxIter = 200;
  double mid = 0.5 * (lo + hi);
  for (int it = 0; it < kMaxIter; ++it) {
    mid = 0.5 * (lo + hi);
    const double g_mid = obj(mid);
    if (g_mid == 0.0 || (hi - lo) < tol) {
      return Ok(mid);
    }
    // Keep the sub-interval whose endpoints straddle the sign change.
    if ((g_mid > 0.0) == (g_lo > 0.0)) {
      lo = mid;
      g_lo = g_mid;
    } else {
      hi = mid;
    }
  }
  return Ok(mid);
}

Result<double> imply_forward_atm_pcp(std::span<const CoTermQuote> quotes, double S,
                                     double T, double r, std::size_t n_atm) {
  if (quotes.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "imply_forward_atm_pcp: empty quote strip");
  }
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_forward_atm_pcp: non-finite or non-positive input");
  }
  if (n_atm == 0) {
    return Err(ErrorCode::InvalidArgument,
               "imply_forward_atm_pcp: n_atm must be >= 1");
  }

  // Select the k strikes nearest to S by |strike − S| (partial sort of indices;
  // fitting-time convenience, not a hot path).
  std::vector<std::size_t> idx(quotes.size());
  for (std::size_t i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  const std::size_t k = std::min(n_atm, quotes.size());
  std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k),
                    idx.end(), [&](std::size_t a, std::size_t b) noexcept {
                      return std::fabs(quotes[a].strike - S) <
                             std::fabs(quotes[b].strike - S);
                    });

  const double carry = std::exp(r * T); // e^{+rT}
  double sum = 0.0;
  for (std::size_t j = 0; j < k; ++j) {
    const CoTermQuote &qz = quotes[idx[j]];
    sum += (qz.call_mid - qz.put_mid) * carry + qz.strike;
  }
  return Ok(sum / static_cast<double>(k));
}

} // namespace atx::vol
