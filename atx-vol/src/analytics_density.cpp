// Risk-neutral density, implied CDF, and model-free implied variance.
//
// Breeden–Litzenberger density from Black-76 prices on the served surface's IV,
// implied CDF/quantiles, BKM model-free moments, and the OTM log-strip variance
// (see analytics.hpp). Independent of the primitives and aggregation TUs.

#include "atx/vol/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/priced_surface.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Uniform-in-K strike grid plus the (flat-extrapolated) served IV at each node.
struct StrikeGrid {
  std::vector<double> K;      // ascending absolute strikes
  std::vector<double> sigma;  // served IV, NaN wings flat-filled to nearest finite
  double dK{0.0};             // uniform spacing (Khi − Klo)/(n − 1)
  bool extrapolated{false};   // any NaN wing was flat-filled
};

// Odd grid size at least 11 (central 2nd-difference / strip integration want an
// odd node count and enough resolution).
[[nodiscard]] int odd_grid_size(int requested) {
  int n = requested < 11 ? 11 : requested;
  if (n % 2 == 0) {
    ++n;
  }
  return n;
}

// Build the K_i = Klo + i·ΔK grid (Klo = F·e^{k_min}, Khi = F·e^{k_max}) and the
// served IV at each node. Far-wing NaNs are flat-extrapolated to the nearest
// finite IV so Black-76 prices stay finite across the whole grid.
[[nodiscard]] StrikeGrid build_strike_grid(const PricedSurface& ps, double F, double T, int n,
                                           const RndConfig& cfg) {
  const std::size_t nn = static_cast<std::size_t>(n);
  StrikeGrid g;
  const double Klo = F * std::exp(cfg.k_min);
  const double Khi = F * std::exp(cfg.k_max);
  g.dK = (Khi - Klo) / static_cast<double>(n - 1);
  g.K.resize(nn);
  g.sigma.resize(nn);
  for (std::size_t i = 0; i < nn; ++i) {
    g.K[i] = Klo + g.dK * static_cast<double>(i);
    g.sigma[i] = ps.iv(g.K[i], T);
  }
  // Forward fill: each NaN takes the nearest finite IV to its left.
  double last = kNaN;
  for (std::size_t i = 0; i < nn; ++i) {
    if (std::isfinite(g.sigma[i])) {
      last = g.sigma[i];
    } else if (std::isfinite(last)) {
      g.sigma[i] = last;
      g.extrapolated = true;
    }
  }
  // Backward fill: still-NaN leading nodes take the nearest finite IV to their right.
  double next = kNaN;
  for (std::size_t i = nn; i-- > 0;) {
    if (std::isfinite(g.sigma[i])) {
      next = g.sigma[i];
    } else if (std::isfinite(next)) {
      g.sigma[i] = next;
      g.extrapolated = true;
    }
  }
  return g;
}

// OTM discounted Black-76 premium: put below the forward, call at/above it. The
// call price at/above F is passed in (already computed for Breeden–Litzenberger)
// to avoid a redundant Black-76 evaluation.
[[nodiscard]] double otm_price(double F, double K, double T, double sigma, double df,
                               double call_price) {
  if (K < F) {
    return black76_price(F, K, T, sigma, df, Side::Put);
  }
  return call_price;
}

// Inverse CDF by linear interpolation: the strike where the (monotone) CDF first
// reaches probability p. Flat outside [cdf.front(), cdf.back()].
[[nodiscard]] double inverse_cdf(const std::vector<double>& K, const std::vector<double>& cdf,
                                 double p) {
  const std::size_t n = K.size();
  if (n == 0) {
    return kNaN;
  }
  if (p <= cdf.front()) {
    return K.front();
  }
  if (p >= cdf.back()) {
    return K.back();
  }
  for (std::size_t i = 1; i < n; ++i) {
    if (cdf[i] >= p) {
      const double c0 = cdf[i - 1];
      const double c1 = cdf[i];
      const double t = (c1 > c0) ? (p - c0) / (c1 - c0) : 0.0;
      return K[i - 1] + t * (K[i] - K[i - 1]);
    }
  }
  return K.back();
}

// Linear interpolation of the (monotone) CDF at an arbitrary strike x.
[[nodiscard]] double interp_cdf_at(const std::vector<double>& K, const std::vector<double>& cdf,
                                   double x) {
  const std::size_t n = K.size();
  if (n == 0) {
    return kNaN;
  }
  if (x <= K.front()) {
    return cdf.front();
  }
  if (x >= K.back()) {
    return cdf.back();
  }
  for (std::size_t i = 1; i < n; ++i) {
    if (K[i] >= x) {
      const double k0 = K[i - 1];
      const double k1 = K[i];
      const double t = (k1 > k0) ? (x - k0) / (k1 - k0) : 0.0;
      return cdf[i - 1] + t * (cdf[i] - cdf[i - 1]);
    }
  }
  return cdf.back();
}

}  // namespace

Result<double> var_swap_vol(const PricedSurface& ps, double T, const RndConfig& cfg) {
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: T must be finite and > 0");
  }
  const double F = ps.forward_at(T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: non-positive forward");
  }
  const double df = std::exp(-ps.rate_at(T) * T);
  const double er = 1.0 / df;  // e^{rT}

  const int n = odd_grid_size(cfg.n_grid);
  StrikeGrid g = build_strike_grid(ps, F, T, n, cfg);
  if (!(g.dK > 0.0) || !std::isfinite(g.dK)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: degenerate strike grid");
  }

  // MFIV log-strip: K_var = (2·e^{rT}/T)·Σ OTM(K_i)/K_i² · ΔK.
  double strip = 0.0;
  for (std::size_t i = 0; i < g.K.size(); ++i) {
    const double K = g.K[i];
    const double call = black76_price(F, K, T, g.sigma[i], df, Side::Call);
    const double O = otm_price(F, K, T, g.sigma[i], df, call);
    strip += (O / (K * K)) * g.dK;
  }
  const double k_var = (2.0 * er / T) * strip;
  return Ok(std::sqrt(k_var > 0.0 ? k_var : 0.0));
}

Result<RiskNeutralDensity> risk_neutral_density(const PricedSurface& ps, double T,
                                                const RndConfig& cfg) {
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: T must be finite and > 0");
  }
  if (cfg.n_grid < 11) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: n_grid must be >= 11");
  }
  const double F = ps.forward_at(T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: non-positive forward");
  }
  const double df = std::exp(-ps.rate_at(T) * T);
  const double er = 1.0 / df;  // e^{rT}

  const int n = odd_grid_size(cfg.n_grid);
  const std::size_t nn = static_cast<std::size_t>(n);
  StrikeGrid g = build_strike_grid(ps, F, T, n, cfg);
  const double dK = g.dK;
  if (!(dK > 0.0) || !std::isfinite(dK)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: degenerate strike grid");
  }

  // Discounted Black-76 calls across the grid.
  std::vector<double> C(nn);
  for (std::size_t i = 0; i < nn; ++i) {
    C[i] = black76_price(F, g.K[i], T, g.sigma[i], df, Side::Call);
  }

  // Breeden–Litzenberger: the risk-neutral density is q(K) = e^{rT} ∂²C/∂K².
  // C here is already discounted, so multiplying the discounted central second
  // difference by e^{rT} = 1/df recovers the undiscounted density.
  std::vector<double> q(nn, 0.0);
  for (std::size_t i = 1; i + 1 < nn; ++i) {
    const double d2 = (C[i + 1] - 2.0 * C[i] + C[i - 1]) / (dK * dK);
    double qi = er * d2;
    if (!(qi > 0.0)) {
      qi = 0.0;  // clamp negatives (butterfly-arb noise) and NaN to 0
    }
    q[i] = qi;
  }
  q[0] = q[1];
  q[nn - 1] = q[nn - 2];

  double mass = 0.0;
  for (std::size_t i = 0; i < nn; ++i) {
    mass += q[i] * dK;
  }
  if (!(mass > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: degenerate density");
  }

  // Normalize to unit mass and accumulate the CDF.
  std::vector<double> pdf(nn);
  std::vector<double> cdf(nn);
  double run = 0.0;
  for (std::size_t i = 0; i < nn; ++i) {
    pdf[i] = q[i] / mass;
    run += pdf[i] * dK;
    cdf[i] = run;
  }
  if (cdf[nn - 1] > 1.0) {
    cdf[nn - 1] = 1.0;
  }

  // Direct RND-integrated moments (price space).
  double mean = 0.0;
  for (std::size_t i = 0; i < nn; ++i) {
    mean += g.K[i] * pdf[i] * dK;
  }
  double var = 0.0;
  double m3 = 0.0;
  double m4 = 0.0;
  for (std::size_t i = 0; i < nn; ++i) {
    const double d = g.K[i] - mean;
    const double w = pdf[i] * dK;
    var += d * d * w;
    m3 += d * d * d * w;
    m4 += d * d * d * d * w;
  }
  const double sd = std::sqrt(var > 0.0 ? var : 0.0);
  const double skewness = (var > 0.0) ? m3 / (var * sd) : 0.0;    // m3 / var^{1.5}
  const double kurtosis = (var > 0.0) ? m4 / (var * var) : 0.0;

  // BKM model-free moments on the log return R = ln(K/F). Unified Carr–Madan
  // strip over OTM options (put for K<F, call otherwise) with the discounted
  // prices O_i. er·V, er·W, er·X are E[R²], E[R³], E[R⁴].
  double V = 0.0;
  double W = 0.0;
  double X = 0.0;
  for (std::size_t i = 0; i < nn; ++i) {
    const double K = g.K[i];
    const double R = std::log(K / F);
    const double O = otm_price(F, K, T, g.sigma[i], df, C[i]);
    const double inv_k2 = 1.0 / (K * K);
    const double vW = (2.0 * (1.0 - R)) * inv_k2;
    const double wW = (6.0 * R - 3.0 * R * R) * inv_k2;
    const double xW = (12.0 * R * R - 4.0 * R * R * R) * inv_k2;
    V += vW * O * dK;
    W += wW * O * dK;
    X += xW * O * dK;
  }
  // Forward-referenced drift: with R = ln(S_T/F), E[e^R] = E[S_T/F] = 1, so the
  // series 1 = 1 + E[R] + E[R²]/2 + … gives E[R] = −E[R²]/2 − E[R³]/6 − E[R⁴]/24.
  // (The classic spot-referenced "e^{rT} − 1" lead term is absent here precisely
  // because we reference the forward, not spot; keeping it would inject a spurious
  // ~−0.02 into μ and a spurious skew of ~−0.5 into a symmetric lognormal.)
  const double mu = -(er / 2.0) * V - (er / 6.0) * W - (er / 24.0) * X;
  const double bkm_variance = er * V - mu * mu;
  const double bkm_var_pos = bkm_variance > 0.0 ? bkm_variance : 0.0;
  const double bkm_skew =
      (bkm_var_pos > 0.0)
          ? (er * W - 3.0 * mu * er * V + 2.0 * mu * mu * mu) / std::pow(bkm_var_pos, 1.5)
          : 0.0;
  const double bkm_kurt =
      (bkm_var_pos > 0.0)
          ? (er * X - 4.0 * mu * er * W + 6.0 * er * mu * mu * V - 3.0 * mu * mu * mu * mu) /
                (bkm_var_pos * bkm_var_pos)
          : 0.0;
  const double skew_index = 100.0 - 10.0 * bkm_skew;

  // Inverse-CDF quantiles and P(S_T ≤ F).
  std::vector<double> quantile_p = cfg.quantiles;
  std::vector<double> quantile_k(quantile_p.size());
  for (std::size_t j = 0; j < quantile_p.size(); ++j) {
    quantile_k[j] = inverse_cdf(g.K, cdf, quantile_p[j]);
  }
  const double prob_below_forward = interp_cdf_at(g.K, cdf, F);

  RiskNeutralDensity out;
  out.T = T;
  out.forward = F;
  out.df = df;
  out.strikes = std::move(g.K);
  out.pdf = std::move(pdf);
  out.cdf = std::move(cdf);
  out.mean = mean;
  out.variance = var;
  out.skewness = skewness;
  out.kurtosis = kurtosis;
  out.bkm_variance = bkm_variance;
  out.bkm_skew = bkm_skew;
  out.bkm_kurt = bkm_kurt;
  out.skew_index = skew_index;
  out.mass_before_norm = mass;
  out.quantile_p = std::move(quantile_p);
  out.quantile_k = std::move(quantile_k);
  out.prob_below_forward = prob_below_forward;
  out.valid = true;
  return Ok(std::move(out));
}

double implied_cdf(const PricedSurface& ps, double T, double K, const RndConfig& /*cfg*/) noexcept {
  if (!(T > 0.0) || !(K > 0.0)) {
    return kNaN;
  }
  const double F = ps.forward_at(T);
  if (!(F > 0.0)) {
    return kNaN;
  }
  const double df = std::exp(-ps.rate_at(T) * T);
  const double h = std::max(K * 1.0e-4, 1.0e-6);
  const double sig_up = ps.iv(K + h, T);
  const double sig_dn = ps.iv(K - h, T);
  if (!std::isfinite(sig_up) || !std::isfinite(sig_dn)) {
    return kNaN;
  }
  const double c_up = black76_price(F, K + h, T, sig_up, df, Side::Call);
  const double c_dn = black76_price(F, K - h, T, sig_dn, df, Side::Call);
  const double d_cdk = (c_up - c_dn) / (2.0 * h);
  // For a discounted call, ∂C/∂K = −df·Q(S_T > K), so the risk-neutral CDF is
  // 1 + (1/df)·∂C/∂K = Q(S_T ≤ K).
  double cdf = 1.0 + (1.0 / df) * d_cdk;
  if (cdf < 0.0) {
    cdf = 0.0;
  }
  if (cdf > 1.0) {
    cdf = 1.0;
  }
  return cdf;
}

}  // namespace atx::vol
