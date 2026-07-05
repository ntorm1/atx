#include "atx/vol/black76.hpp"

#include <cmath>

#include "atx/core/math.hpp"

namespace atx::vol {

using atx::core::inv_sqrt_2pi;
using atx::core::norm_cdf;

namespace {

// Discounted intrinsic — the T<=0 / sigma<=0 collapse shared by every entry.
[[nodiscard]] double intrinsic(double F, double K, double df, Side side) noexcept {
  const double intr = (side == Side::Call) ? (F - K) : (K - F);
  return df * (intr > 0.0 ? intr : 0.0);
}

} // namespace

double black76_price(double F, double K, double T, double sigma, double df,
                     Side side) noexcept {
  if (T <= 0.0 || sigma <= 0.0) {
    return intrinsic(F, K, df, side);
  }
  const double sqrt_t = std::sqrt(T);
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double ln_fk = std::log(F / K);
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  if (side == Side::Call) {
    return df * (F * norm_cdf(d1) - K * norm_cdf(d2));
  }
  return df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
}

double black76_price_from_lnfk(double F, double K, double T, double sigma,
                               double df, double ln_fk, double sqrt_t,
                               Side side) noexcept {
  if (T <= 0.0 || sigma <= 0.0) {
    return intrinsic(F, K, df, side);
  }
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  if (side == Side::Call) {
    return df * (F * norm_cdf(d1) - K * norm_cdf(d2));
  }
  return df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
}

Black76Aux black76_aux(double F, double K, double T, double sigma, double df,
                       Side side) noexcept {
  if (T <= 0.0 || sigma <= 0.0) {
    return Black76Aux{intrinsic(F, K, df, side), 0.0, 0.0, 0.5};
  }
  const double sqrt_t = std::sqrt(T);
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double ln_fk = std::log(F / K);
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  const double n_d1 = norm_cdf(d1);
  const double n_d2 = norm_cdf(d2);
  // Use 1-Φ identities on the put leg to avoid two extra erfc calls.
  const double price = (side == Side::Call)
                           ? df * (F * n_d1 - K * n_d2)
                           : df * (K * (1.0 - n_d2) - F * (1.0 - n_d1));
  return Black76Aux{price, d1, d2, n_d1};
}

Black76ValueVega black76_value_and_vega(double F, double K, double T,
                                        double sigma, double df, Side side,
                                        double sqrt_t_in) noexcept {
  if (T <= 0.0 || sigma <= 0.0) {
    return Black76ValueVega{intrinsic(F, K, df, side), 0.0};
  }
  const double sqrt_t = (sqrt_t_in >= 0.0) ? sqrt_t_in : std::sqrt(T);
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double ln_fk = std::log(F / K);
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  const double n_d1 = norm_cdf(d1);
  const double n_d2 = norm_cdf(d2);
  // Vega = F·df·φ(d1)·√T, with φ(d1) = (1/√(2π))·exp(-d1²/2).
  const double pdf_d1 = inv_sqrt_2pi<double> * std::exp(-0.5 * d1 * d1);
  const double vega = F * df * pdf_d1 * sqrt_t;
  const double price = (side == Side::Call)
                           ? df * (F * n_d1 - K * n_d2)
                           : df * (K * (1.0 - n_d2) - F * (1.0 - n_d1));
  return Black76ValueVega{price, vega};
}

} // namespace atx::vol
