#include "atx/vol/implied_vol.hpp"

#include <cmath>
#include <limits>
#include <optional>

#include "atx/core/error.hpp"
#include "atx/core/math.hpp"

namespace atx::vol {

using atx::core::clamp;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::norm_cdf;
using atx::core::norm_pdf;
using atx::core::Ok;

namespace {

// High-precision π (beats the <cmath> literal in the last ULPs; the seed's
// Polya factor 2/π is sensitive to it).
constexpr double kPi = 3.141592653589793238462643383279502884;

struct NoArbBand {
  bool inside;
  double intrinsic; // discounted intrinsic (df·max(intr,0))
};

// No-arbitrage band: for a call max(0,F-K) <= C/df <= F; for a put analogously.
[[nodiscard]] NoArbBand no_arb_band(double price, double F, double K, double df,
                                    Side side) noexcept {
  double intr, upper;
  if (side == Side::Call) {
    intr = (F > K) ? (F - K) : 0.0;
    upper = F;
  } else {
    intr = (K > F) ? (K - F) : 0.0;
    upper = K;
  }
  const double pn = price / df;
  const bool inside = (pn > intr - 1e-15) && (pn < upper + 1e-15);
  return NoArbBand{inside, df * intr};
}

// Defense-in-depth Brenner-Subrahmanyam + |k|/T fallback seed for the corners
// where the SR-2017 quadratic degenerates (deep wings near the no-arb edge).
[[nodiscard]] double seed_bs_k(double price, double F, double T, double df,
                               double y) noexcept {
  const double scale = df * F * std::sqrt(T / (2.0 * kPi));
  const double s_bs = (scale > 0.0) ? (price / scale) : 0.2;
  const double s_k = std::sqrt(2.0 * std::fabs(y) / T);
  double s_fb = (s_k > s_bs) ? s_k : s_bs;
  if (s_fb < 1.0e-3) s_fb = 1.0e-3;
  if (s_fb > 5.0) s_fb = 5.0;
  return s_fb;
}

// The SR-2017 closed-form core. Returns nullopt where the quadratic degenerates
// so the caller can fall back. Mirrors the C `goto fallback` control flow.
[[nodiscard]] std::optional<double>
seed_sr2017_core(double price_eff, double K, double T, double df,
                 Side side_eff, double y) noexcept {
  const double Q = price_eff / (df * K);
  const double f = std::exp(y);
  const double eps = (side_eff == Side::Call) ? 1.0 : -1.0;
  const double R = 2.0 * Q - eps * (f - 1.0);

  const double kp = 2.0 / kPi; // Polya factor
  const double e_pky = std::exp(kp * y);
  const double e_mky = std::exp(-kp * y);
  const double e_y_h = std::exp((1.0 - kp) * y);
  const double e_y_l = std::exp(-(1.0 - kp) * y);

  const double diff = e_y_h - e_y_l;
  const double A = diff * diff;
  const double sum_h = e_y_h + e_y_l;
  const double f2_R2 = f * f - R * R;
  const double B = 4.0 * (e_mky + e_pky) - 2.0 * sum_h * (1.0 + f2_R2) / f;

  const double Q_minus = Q - eps * (f - 1.0);
  const double f_plus = f + 1.0;
  const double C = (4.0 * Q / (f * f)) * Q_minus * (f_plus - R) * (f_plus + R);

  // Stable quadratic root: 2C / (B + sqrt(B²+4AC)); avoids B - sqrt cancel.
  double disc = B * B + 4.0 * A * C;
  if (disc < 0.0) disc = 0.0;
  const double denom = B + std::sqrt(disc);
  if (denom < 1.0e-30 || !std::isfinite(denom)) return std::nullopt;

  const double beta = 2.0 * C / denom;
  if (!(beta > 0.0) || !std::isfinite(beta)) return std::nullopt;

  const double gamma = -std::log(beta) / kp;
  if (gamma < std::fabs(y)) return std::nullopt; // sum form needs γ ≥ |y|

  const double sqrt_v = std::sqrt(gamma + y) + std::sqrt(gamma - y);
  if (!(sqrt_v > 0.0) || !std::isfinite(sqrt_v)) return std::nullopt;

  double s = sqrt_v / std::sqrt(T);
  if (s < 1.0e-4) s = 1.0e-4;
  if (s > 5.0) s = 5.0;
  return s;
}

[[nodiscard]] double seed_sr2017(double price, double F, double K, double T,
                                 double df, Side side) noexcept {
  // ATM bypass: y→0 makes A=0 and B has a removable singularity. Brenner-
  // Subrahmanyam is exact at ATM (1st-order Taylor of B76 in σ).
  if (std::fabs(F - K) < 1.0e-3 * std::fmax(F, K)) {
    const double scale = df * F * std::sqrt(T / (2.0 * kPi));
    if (scale > 0.0) {
      double s = price / scale;
      if (s < 1.0e-4) s = 1.0e-4;
      if (s > 5.0) s = 5.0;
      return s;
    }
  }

  // Put-call parity to the OTM equivalent — SR-2017's Polya-CDF form is well
  // conditioned only on the OTM tail. IV is invariant under parity.
  double price_eff = price;
  Side side_eff = side;
  if (side == Side::Call && F > K) {
    price_eff = price - df * (F - K);
    side_eff = Side::Put;
  } else if (side == Side::Put && K > F) {
    price_eff = price + df * (F - K);
    side_eff = Side::Call;
  }
  if (price_eff < 1.0e-15) price_eff = 1.0e-15;

  const double y = std::log(F / K);
  if (auto s = seed_sr2017_core(price_eff, K, T, df, side_eff, y)) {
    return *s;
  }
  return seed_bs_k(price, F, T, df, y);
}

// Shared inversion core. `Trace == true` records the Halley-iteration count and
// which termination test fired (test/measurement seam only). `Trace == false`
// compiles every trace statement out via `if constexpr`, so the production entry
// point below pays exactly zero overhead — no null-checks, no writes.
//
// K1 (accuracy-improving): the price-residual convergence test now compares the
// residual against a *notional-scaled* tolerance rather than the absolute
// `kIvTol`. `kIvTol` is documented as a tolerance in VOL units; the loop's
// residual `fval = price_model - price` is in PRICE units. The floating-point
// noise floor of that residual is ~ε·df·max(F,K) (the magnitude of the terms
// `df·F·Φ(d1)` and `df·K·Φ(d2)` whose difference forms price_model). For a
// high-notional / high-vega option that floor exceeds the absolute 1e-12, so the
// old `|fval| < kIvTol` test could never fire — the loop always fell through to
// the vol-step test, burning one extra (wasted) Halley evaluation past the point
// where σ had already reached machine precision. Scaling the price tolerance by
// the residual's own noise floor makes the test fire exactly when the residual
// can no longer improve (σ machine-precise), saving that final evaluation while
// holding σ to machine precision — far inside the 1e-4 vol economic bound.
template <bool Trace>
[[nodiscard]] Result<double> implied_vol_impl(double price, double F, double K, double T, double df,
                                              Side side, int *iters, int *exit_reason) {
  if constexpr (Trace) {
    *iters = 0;
    *exit_reason = -1; // -1 none, 0 price-residual test, 1 vol-step test
  }
  if (!std::isfinite(price) || !std::isfinite(F) || !std::isfinite(K) ||
      !std::isfinite(T) || !std::isfinite(df)) {
    return Err(ErrorCode::OutOfRange, "implied_vol: non-finite input");
  }
  if (F <= 0.0 || K <= 0.0 || T <= 0.0 || df <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "implied_vol: F/K/T/df must be > 0");
  }

  const NoArbBand band = no_arb_band(price, F, K, df, side);
  if (!band.inside) {
    return Err(ErrorCode::OutOfRange, "implied_vol: price outside no-arb band");
  }
  // A price at intrinsic implies σ→0 (no finite IV); clamp to the floor.
  if (price <= band.intrinsic + 1e-15) {
    return Ok(kIvMin);
  }

  double sigma = seed_sr2017(price, F, K, T, df, side);
  sigma = clamp(sigma, kIvMin, kIvMax);

  const double sqrt_t = std::sqrt(T);
  const double ln_fk = std::log(F / K);
  // Notional-scaled floor for the price-residual test (see the K1 note above).
  // kIvResidNoiseFloor·ε·df·max(F,K) is the rounding-noise level of price_model;
  // once |fval| drops below it, further Halley steps cannot reduce the residual.
  const double price_noise =
      kIvResidNoiseFloor * std::numeric_limits<double>::epsilon() * df * std::fmax(F, K);
  for (int iter = 0; iter < kIvMaxIter; ++iter) {
    const double v = sigma * sqrt_t;
    const double inv_v = 1.0 / v;
    const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
    const double d2 = d1 - v;
    const double n_d1 = norm_cdf(d1);
    const double n_d2 = norm_cdf(d2);
    const double phi_d1 = norm_pdf(d1);

    const double price_model =
        (side == Side::Call) ? df * (F * n_d1 - K * n_d2)
                             : df * (K * (1.0 - n_d2) - F * (1.0 - n_d1));

    const double fval = price_model - price;
    if (std::fabs(fval) < price_noise) {
      if constexpr (Trace) {
        *exit_reason = 0;
      }
      return Ok(sigma);
    }

    const double vega = df * F * phi_d1 * sqrt_t;
    if (vega < 1e-20) {
      // Vega collapse — typical at deep wings near expiry.
      return Err(ErrorCode::Unavailable, "implied_vol: vega collapse");
    }
    const double volga = vega * d1 * d2 / sigma;

    // Halley (order-3) step on f(σ) = price_model - price.
    const double fp = vega;
    const double fpp = volga;
    const double num = 2.0 * fval * fp;
    const double den = 2.0 * fp * fp - fval * fpp;
    double step = (std::fabs(den) < 1e-30) ? (-fval / fp) : (-num / den);

    // Multiplicative bound: σ_{n+1} ∈ [σ_n/2, 2σ_n].
    if (step > sigma) step = sigma;
    if (step < -0.5 * sigma) step = -0.5 * sigma;
    sigma += step;
    sigma = clamp(sigma, kIvMin, kIvMax);
    if constexpr (Trace) {
      ++(*iters);
    }

    if (std::fabs(step) < kIvTol) {
      if constexpr (Trace) {
        *exit_reason = 1;
      }
      return Ok(sigma);
    }
  }

  return Err(ErrorCode::Unavailable, "implied_vol: exhausted iterations");
}

} // namespace

Result<double> implied_vol(double price, double F, double K, double T,
                           double df, Side side) {
  return implied_vol_impl<false>(price, F, K, T, df, side, nullptr, nullptr);
}

// Test/measurement seam (not in the public header). Same inversion as
// implied_vol, additionally reporting the number of Halley steps computed
// (`iters`) and which termination test fired (`exit_reason`: 0 = price-residual,
// 1 = vol-step, -1 = none/error). Used by the K1 convergence tests.
Result<double> implied_vol_traced(double price, double F, double K, double T, double df, Side side,
                                  int &iters, int &exit_reason) {
  return implied_vol_impl<true>(price, F, K, T, df, side, &iters, &exit_reason);
}

} // namespace atx::vol
