#pragma once

// atx::engine::quant — European Black-Scholes-Merton pricing, vega, greeks, and
// an implied-vol solver (Newton-Raphson seeded at 0.5 with a bisection fallback
// on [1e-6, 5.0]). All functions pure and header-only. `theta` is per calendar
// day; `vega` is per 1.00 absolute change in volatility (per year). Unsolvable
// or out-of-domain inputs yield NaN rather than throwing.

#include <cmath>

namespace atx::engine::quant {

[[nodiscard]] inline double norm_pdf(double x) {
  return 0.3989422804014327 * std::exp(-0.5 * x * x);
}

[[nodiscard]] inline double norm_cdf(double x) {
  return 0.5 * std::erfc(-x * 0.7071067811865476);
}

[[nodiscard]] inline double bs_d1(double S, double K, double T, double r, double q,
                                  double sigma) {
  return (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
}

[[nodiscard]] inline double bs_price(double S, double K, double T, double r, double q,
                                     double sigma, bool is_call) {
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
    const double intrinsic = is_call ? (S - K) : (K - S);
    return intrinsic > 0.0 ? intrinsic : 0.0;
  }
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  const double d2 = d1 - sigma * std::sqrt(T);
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  if (is_call) {
    return S * dfq * norm_cdf(d1) - K * dfr * norm_cdf(d2);
  }
  return K * dfr * norm_cdf(-d2) - S * dfq * norm_cdf(-d1);
}

[[nodiscard]] inline double bs_vega(double S, double K, double T, double r, double q,
                                    double sigma) {
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
    return 0.0;
  }
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  return S * std::exp(-q * T) * norm_pdf(d1) * std::sqrt(T);
}

struct Greeks {
  double delta;
  double gamma;
  double vega;   // per 1.00 vol, per year
  double theta;  // per calendar day
};

[[nodiscard]] inline Greeks bs_greeks(double S, double K, double T, double r, double q,
                                      double sigma, bool is_call) {
  const double nan = std::nan("");
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0 || !std::isfinite(sigma)) {
    return Greeks{nan, nan, nan, nan};
  }
  const double sqrtT = std::sqrt(T);
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  const double d2 = d1 - sigma * sqrtT;
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  const double pdf = norm_pdf(d1);
  Greeks g{};
  g.gamma = dfq * pdf / (S * sigma * sqrtT);
  g.vega = S * dfq * pdf * sqrtT;
  if (is_call) {
    g.delta = dfq * norm_cdf(d1);
    const double theta = -(S * dfq * pdf * sigma) / (2.0 * sqrtT) -
                         r * K * dfr * norm_cdf(d2) + q * S * dfq * norm_cdf(d1);
    g.theta = theta / 365.0;
  } else {
    g.delta = dfq * (norm_cdf(d1) - 1.0);
    const double theta = -(S * dfq * pdf * sigma) / (2.0 * sqrtT) +
                         r * K * dfr * norm_cdf(-d2) - q * S * dfq * norm_cdf(-d1);
    g.theta = theta / 365.0;
  }
  return g;
}

[[nodiscard]] inline double implied_vol(double price, double S, double K, double T, double r,
                                        double q, bool is_call) {
  const double nan = std::nan("");
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T) ||
      !std::isfinite(r) || !std::isfinite(q) ||
      T <= 0.0 || S <= 0.0 || K <= 0.0 || price <= 0.0) {
    return nan;
  }
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  const double fwd = is_call ? (S * dfq - K * dfr) : (K * dfr - S * dfq);
  const double intrinsic = fwd > 0.0 ? fwd : 0.0;
  const double upper = is_call ? S * dfq : K * dfr;
  if (price < intrinsic - 1e-9 || price > upper + 1e-9) {
    return nan;
  }
  // Newton-Raphson.
  double sigma = 0.5;
  for (int i = 0; i < 100; ++i) {
    const double diff = bs_price(S, K, T, r, q, sigma, is_call) - price;
    if (std::abs(diff) < 1e-7) {
      return sigma;
    }
    const double v = bs_vega(S, K, T, r, q, sigma);
    if (v < 1e-12) {
      break;
    }
    sigma -= diff / v;
    if (!(sigma > 1e-6 && sigma < 5.0)) {
      break;
    }
  }
  // Bisection fallback on [1e-6, 5.0].
  double lo = 1e-6;
  double hi = 5.0;
  double flo = bs_price(S, K, T, r, q, lo, is_call) - price;
  double fhi = bs_price(S, K, T, r, q, hi, is_call) - price;
  if (flo * fhi > 0.0) {
    return nan;
  }
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    const double fm = bs_price(S, K, T, r, q, mid, is_call) - price;
    if (std::abs(fm) < 1e-7) {
      return mid;
    }
    if (flo * fm <= 0.0) {
      hi = mid;
      fhi = fm;
    } else {
      lo = mid;
      flo = fm;
    }
  }
  return 0.5 * (lo + hi);
}

} // namespace atx::engine::quant
