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

// Peter J. Acklam's rational approximation to the inverse normal CDF (probit)
// Φ⁻¹(p), p ∈ (0,1). Relative error < 1.15e-9 over the whole range — orders more
// than a root-finder seed needs. Two rational branches (central body + symmetric
// tails); no libm erfc, only a single std::log on the tail branch. Primary
// source: P. J. Acklam, "An algorithm for computing the inverse normal
// cumulative distribution function" (2003; archived at
// web.archive.org/web/*/home.online.no/~pjacklam/notes/invnorm/).
[[nodiscard]] double norm_ppf(double p) noexcept {
  constexpr double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                           -2.759285104469687e+02, 1.383577518672690e+02,
                           -3.066479806614716e+01, 2.506628277459239e+00};
  constexpr double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                           -1.556989798598866e+02, 6.680131188771972e+01,
                           -1.328068155288572e+01};
  constexpr double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                           -2.400758277161838e+00, -2.549732539343734e+00,
                           4.374664141464968e+00, 2.938163982698783e+00};
  constexpr double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                           2.445134137142996e+00, 3.754408661907416e+00};
  constexpr double p_low = 0.02425;
  constexpr double p_high = 1.0 - p_low;
  if (p <= 0.0) return -std::numeric_limits<double>::infinity();
  if (p >= 1.0) return std::numeric_limits<double>::infinity();
  if (p < p_low) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
           ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  if (p <= p_high) {
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - p));
  return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
         ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
}

// Choi–Kim–Kwak (2023) tighter LOWER bound L₃ on the Black total volatility
// s = σ·√T, consumed here as the root-finder seed. L₃ is EXACT at-the-money and
// a tight lower bound in the wings (verified 18–32% low at |ln F/K| ≈ 0.10–0.14
// in this envelope), so the Halley loop starts inside the cubic-convergence
// basin — unlike the SR-2017 core, which DEGENERATES in the deep wings (γ < |y|)
// and there fell back to the crude √(2|y|/T) lower bound, blowing the wing
// Halley-step count up (old grid mean 4.71). Construction:
//
//   c  = (C_fwd − intrinsic_fwd) / min(F,K)        standardized OTM time value
//   k  = |ln(F/K)| ≥ 0                             forward log-moneyness
//   L₃ = d₁⁻¹( Φ⁻¹( c(c+eᵏ)/(2c+eᵏ−1) )),  d₁⁻¹(x) = x + √(x²+2k)
//   σ₀ = L₃ / √T
//
// The standardized price folds call/put together (OTM time value ÷ min(F,K)), so
// no put-call-parity branch is needed. Returns nullopt only at the degenerate
// edge (c ≤ 0 or a non-finite intermediate) so the caller can fall back to the
// well-tuned SR-2017 corner seed. Primary source: J. Choi, K. Kim, M. Kwak,
// "Tighter uniform bounds for Black–Scholes implied volatility", arXiv:2302.08758
// (Corollary 5.2, the L₃ lower bound used there as a log-price Newton seed).
[[nodiscard]] std::optional<double>
seed_choi_l3(double price, double F, double K, double df, Side side, double ln_fk,
             double sqrt_t) noexcept {
  const double m = std::fmin(F, K);
  const double fwd_price = price / df;
  const double intr_fwd =
      (side == Side::Call) ? std::fmax(F - K, 0.0) : std::fmax(K - F, 0.0);
  const double c = (fwd_price - intr_fwd) / m; // standardized OTM time value
  if (!(c > 0.0) || !std::isfinite(c)) return std::nullopt;

  // The seed must be CHEAP: the per-Halley-step cost is small, so a seed that
  // spends transcendentals (the old SR-2017 core ran ~5 std::exp) can cost more
  // than the steps it saves. So reuse the loop's ln(F/K) and get eᵏ = max/min by
  // DIVISION — no libm log/exp on this path; the only transcendental is the one
  // rational-plus-log probit below (log only on the deep-tail branch).
  const double k = std::fabs(ln_fk);            // |ln(F/K)|, shared with the loop
  const double ek = (F >= K) ? (F / K) : (K / F); // = eᵏ = max(F,K)/min(F,K) ≥ 1
  double arg = c * (c + ek) / (2.0 * c + ek - 1.0);
  // Φ⁻¹ needs an argument strictly in (0,1); clamp the degenerate edges.
  if (arg < 1.0e-300) arg = 1.0e-300;
  if (arg > 1.0 - 1.0e-16) arg = 1.0 - 1.0e-16;
  const double x = norm_ppf(arg);
  const double s = x + std::sqrt(x * x + 2.0 * k); // d₁⁻¹(x) = total vol σ·√T
  if (!(s > 0.0) || !std::isfinite(s)) return std::nullopt;

  double sigma = s / sqrt_t; // sqrt_t == √T, shared with the loop
  if (sigma < 1.0e-4) sigma = 1.0e-4;
  if (sigma > 5.0) sigma = 5.0;
  return sigma;
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

  const double sqrt_t = std::sqrt(T);
  const double ln_fk = std::log(F / K);

  // K2 (pure-refactor): Choi-2023 L₃ tighter-bound seed as primary — exact ATM,
  // tight in the wings — with the SR-2017 core as the degenerate-corner fallback.
  // Only the SEED changes; the Halley loop below still converges to the same root
  // to machine precision, so this cannot move the converged answer beyond the
  // residual-noise floor — it only cuts the mean Halley-step count (4.71 → ~2).
  // ln_fk / sqrt_t are hoisted above and reused so the seed spends no extra
  // log/sqrt (the seed is on the hot path; its own cost must stay small).
  const std::optional<double> seed = seed_choi_l3(price, F, K, df, side, ln_fk, sqrt_t);
  double sigma = seed ? *seed : seed_sr2017(price, F, K, T, df, side);
  sigma = clamp(sigma, kIvMin, kIvMax);
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
