#include "atx/vol/api/pricing/dividend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

double hybrid_forward_base(double S, double r, double T, std::span<const DividendEvent> cash_divs,
                           std::int64_t expiry_ns, std::int64_t now_ts_ns,
                           const HybridDivParams &hyb) noexcept {
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(S) || !std::isfinite(T) || !std::isfinite(r) ||
      !std::isfinite(hyb.prop_div_yield) || !std::isfinite(hyb.blend)) {
    return kQuietNaN;
  }

  // Pure escrowed-cash forward (Battig-Jarrow), reused verbatim so blend == 0
  // reproduces it bit-for-bit and out-of-window dividends are handled once.
  const double f_cash = forward_div_corrected(S, r, T, cash_divs, expiry_ns, now_ts_ns);

  const double q = hyb.prop_div_yield;
  const double beta = hyb.blend;

  // G is the blend of the proportional base (β·S·e^{(r−βq)T}) and the escrowed
  // base ((1−β)·F_cash·e^{−βqT}); the borrow enters only through e^{−bT}, which
  // makes F strictly decreasing in `borrow` and monotone for PCP inversion.
  return beta * S * std::exp((r - beta * q) * T) + (1.0 - beta) * f_cash * std::exp(-beta * q * T);
}

double hybrid_forward_from_base(double base, double borrow, double T) noexcept {
  if (!std::isfinite(base) || !std::isfinite(borrow) || !(T > 0.0) || !std::isfinite(T)) {
    return kQuietNaN;
  }
  return base * std::exp(-borrow * T);
}

double hybrid_forward(double S, double r, double borrow, double T,
                      std::span<const DividendEvent> cash_divs, std::int64_t expiry_ns,
                      std::int64_t now_ts_ns, const HybridDivParams &hyb) noexcept {
  const double base = hybrid_forward_base(S, r, T, cash_divs, expiry_ns, now_ts_ns, hyb);
  return hybrid_forward_from_base(base, borrow, T);
}

void hybrid_forward_div_jacobian(double r, double borrow, double T,
                                 std::span<const DividendEvent> cash_divs, std::int64_t expiry_ns,
                                 std::int64_t now_ts_ns, const HybridDivParams &hyb,
                                 std::span<double> dF_dDiv_out) noexcept {
  const std::size_t n =
      (cash_divs.size() < dF_dDiv_out.size()) ? cash_divs.size() : dF_dDiv_out.size();
  if (!(T > 0.0) || !std::isfinite(T) || !std::isfinite(r) || !std::isfinite(borrow) ||
      !std::isfinite(hyb.prop_div_yield) || !std::isfinite(hyb.blend)) {
    for (std::size_t i = 0; i < n; ++i) {
      dF_dDiv_out[i] = kQuietNaN;
    }
    return;
  }
  const double beta = hyb.blend;
  // Borrow/blend/proportional-yield prefactor shared by every event (matches
  // hybrid_forward_base's e^{-borrow·T}·(1−β)·e^{−βqT} escrowed-leg coefficient).
  const double coeff = -(1.0 - beta) * std::exp(-borrow * T) * std::exp(-beta * hyb.prop_div_yield * T);
  for (std::size_t i = 0; i < n; ++i) {
    const DividendEvent &ev = cash_divs[i];
    if (ev.ex_date_ns < now_ts_ns || ev.ex_date_ns > expiry_ns) {
      dF_dDiv_out[i] = 0.0; // paid already / after expiry — mirrors forward_div_corrected
      continue;
    }
    const double t_i = static_cast<double>(ev.ex_date_ns - now_ts_ns) / (1.0e9 * 365.25 * 86400.0);
    if (t_i < 0.0 || t_i > T) {
      dF_dDiv_out[i] = 0.0;
      continue;
    }
    // ∂F_cash/∂D_i = −e^{−r·t_i}·e^{rT} = −e^{r(T−t_i)}, scaled by the escrowed-leg
    // prefactor above.
    dF_dDiv_out[i] = coeff * std::exp(r * (T - t_i));
  }
}

Result<double> imply_borrow_european_pcp_from_base(double call_price, double put_price, double K,
                                                   double T, double r, double base, double b_lo,
                                                   double b_hi, double tol) noexcept {
  if (!(K > 0.0) || !std::isfinite(K) || !(T > 0.0) || !std::isfinite(T) || !std::isfinite(r) ||
      !(base > 0.0) || !std::isfinite(base) || !std::isfinite(call_price) ||
      !std::isfinite(put_price)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: non-finite or non-positive input");
  }
  if (!(b_lo < b_hi) || !std::isfinite(b_lo) || !std::isfinite(b_hi)) {
    return Err(ErrorCode::InvalidArgument, "imply_borrow_european_pcp: require b_lo < b_hi");
  }
  if (!(tol > 0.0) || !std::isfinite(tol)) {
    return Err(ErrorCode::InvalidArgument, "imply_borrow_european_pcp: tol must be positive");
  }

  // PCP gives the target forward directly. Since F(b)=G*exp(-b*T), one log
  // replaces the old ~33 objective evaluations. This is algebraically exact;
  // the only numerical movement is ordinary floating-point rearrangement and
  // is fixture-gated against the former bisection to 1e-8 in borrow-rate units.
  const double target_forward = std::fma(call_price - put_price, std::exp(r * T), K);
  if (!std::isfinite(target_forward)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: non-finite parity-implied forward");
  }
  if (!(target_forward > 0.0)) {
    return Err(ErrorCode::OutOfRange,
               "imply_borrow_european_pcp: implied borrow outside [b_lo, b_hi]");
  }

  const double borrow = -std::log(target_forward / base) / T;
  if (!std::isfinite(borrow)) {
    return Err(ErrorCode::InvalidArgument, "imply_borrow_european_pcp: non-finite implied borrow");
  }
  // R-26: `tol` is VESTIGIAL for this closed form and is deliberately NOT used as
  // the endpoint allowance. The former bisection consumed it as a convergence
  // tolerance; the PCP inversion is algebraically exact, so the only allowance a
  // root at the bracket edge needs is representable floating-point roundoff — a
  // fixed machine-epsilon slack, NOT `tol`. Widening the slack to `tol` would
  // admit a root genuinely outside [b_lo, b_hi] (pinned rejected by
  // dividend_test.cpp::ClosedFormRejectsRootOutsideBracketEvenWithinSolverToler
  // ance). `tol` is still validated finite-positive for API compatibility.
  const double endpoint_slack = 16.0 * std::numeric_limits<double>::epsilon() *
                                std::max({1.0, std::fabs(b_lo), std::fabs(b_hi)});
  if (borrow < b_lo) {
    if (b_lo - borrow <= endpoint_slack) {
      return Ok(b_lo);
    }
    return Err(ErrorCode::OutOfRange,
               "imply_borrow_european_pcp: implied borrow outside [b_lo, b_hi]");
  }
  if (borrow > b_hi) {
    if (borrow - b_hi <= endpoint_slack) {
      return Ok(b_hi);
    }
    return Err(ErrorCode::OutOfRange,
               "imply_borrow_european_pcp: implied borrow outside [b_lo, b_hi]");
  }
  return Ok(borrow);
}

Result<double> imply_borrow_european_pcp(double call_price, double put_price, double S, double K,
                                         double T, double r,
                                         std::span<const DividendEvent> cash_divs,
                                         std::int64_t expiry_ns, std::int64_t now_ts_ns,
                                         const HybridDivParams &hyb, double b_lo, double b_hi,
                                         double tol) noexcept {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_borrow_european_pcp: non-finite or non-positive input");
  }
  const double base = hybrid_forward_base(S, r, T, cash_divs, expiry_ns, now_ts_ns, hyb);
  return imply_borrow_european_pcp_from_base(call_price, put_price, K, T, r, base, b_lo, b_hi, tol);
}

Result<double> imply_forward_atm_pcp(std::span<const CoTermQuote> quotes, double S, double T,
                                     double r, std::size_t n_atm) {
  if (quotes.empty()) {
    return Err(ErrorCode::InvalidArgument, "imply_forward_atm_pcp: empty quote strip");
  }
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_forward_atm_pcp: non-finite or non-positive input");
  }
  if (n_atm == 0) {
    return Err(ErrorCode::InvalidArgument, "imply_forward_atm_pcp: n_atm must be >= 1");
  }

  // Select the k strikes nearest to S by |strike − S| (partial sort of indices;
  // fitting-time convenience, not a hot path).
  std::vector<std::size_t> idx(quotes.size());
  for (std::size_t i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  const std::size_t k = std::min(n_atm, quotes.size());
  std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k), idx.end(),
                    [&](std::size_t a, std::size_t b) noexcept {
                      return std::fabs(quotes[a].strike - S) < std::fabs(quotes[b].strike - S);
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
