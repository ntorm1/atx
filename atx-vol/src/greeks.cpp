#include "atx/vol/greeks.hpp"

#include <cmath>

#include "atx/core/math.hpp"

namespace atx::vol {

using atx::core::norm_cdf;
using atx::core::norm_pdf;

Black76Greeks black76_greeks(double F, double K, double T, double sigma,
                             double r, double df, Side side) noexcept {
  // Degenerate: Greeks are zero except delta, which becomes the intrinsic
  // step. Not bit-perfect but acceptable for a T->0 / sigma->0 fallback.
  if (T <= 0.0 || sigma <= 0.0) {
    const double intr = (side == Side::Call) ? (F - K) : (K - F);
    const double delta =
        (intr > 0.0) ? ((side == Side::Call) ? df : -df) : 0.0;
    const double price = df * (intr > 0.0 ? intr : 0.0);
    return Black76Greeks{Greeks{delta, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
                         price};
  }

  const double sqrt_t = std::sqrt(T);
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double ln_fk = std::log(F / K);
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  const double n_d1 = norm_cdf(d1);
  const double n_d2 = norm_cdf(d2);
  const double phi_d1 = norm_pdf(d1); // φ(d1), shared by gamma/vega/vanna/...

  Greeks g{};
  double price;
  if (side == Side::Call) {
    price = df * (F * n_d1 - K * n_d2);
    g.delta = df * n_d1;
  } else {
    price = df * (K * (1.0 - n_d2) - F * (1.0 - n_d1));
    g.delta = -df * (1.0 - n_d1);
  }
  g.rho = -T * price; // ∂P/∂r — same form for call and put

  // Call/put-symmetric second-order sensitivities.
  g.gamma = (df * phi_d1) / (F * v);   // ∂²P/∂F²
  g.vega = df * F * phi_d1 * sqrt_t;   // ∂P/∂σ
  // vanna = ∂²P/∂F∂σ = -df·φ(d1)·d2/σ (raw ∂d1/∂σ = -d2/σ; no √T factor).
  g.vanna = -df * phi_d1 * d2 / sigma;
  g.volga = g.vega * d1 * d2 / sigma;  // ∂²P/∂σ² = vega·d1·d2/σ

  // theta = r·price - df·F·φ(d1)·σ/(2√T)  (calendar-time; equal call/put).
  g.theta = r * price - df * F * phi_d1 * sigma / (2.0 * sqrt_t);
  // charm = r·delta + df·φ(d1)·d2/(2T)    (identical for call/put).
  g.charm = r * g.delta + df * phi_d1 * d2 / (2.0 * T);

  return Black76Greeks{g, price};
}

} // namespace atx::vol
