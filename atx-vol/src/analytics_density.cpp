// Risk-neutral density, implied CDF, and model-free implied variance.
//
// Breeden–Litzenberger density from Black-76 prices on the served surface's IV,
// implied CDF/quantiles, BKM model-free moments, and the OTM log-strip variance
// (see analytics.hpp). Independent of the primitives and aggregation TUs.
//
// The strike grid is UNIFORM IN LOG-FORWARD-MONEYNESS k = ln(K/F); every integral
// ∫g(K)dK is evaluated as ∫g(K)·K dk (the K = F·e^k Jacobian) with composite
// Simpson weights (n odd ⇒ Simpson is exact-order and free of the rectangle-rule
// endpoint bias). Both `var_swap_vol` and `risk_neutral_density` share one
// `build_price_grid` kernel so the grid, the served IV batch, and the discounted
// OTM strip are computed exactly once per call.

#include "atx/vol/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/strip_grid.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Odd grid size at least 11. Composite Simpson wants an odd node count (an even
// interval count) and enough resolution for the 3-point second difference; we
// BUMP a too-small request up rather than erroring on it.
//
// E2: node-count policy delegated to the shared strip/grid convention
// (`strip_grid.hpp`). The `< 11` test stays on the signed request so a negative
// `n_grid` still floors to 11 instead of converting to a huge size_t.
[[nodiscard]] int odd_grid_size(int requested) {
  const std::size_t base =
      requested < 11 ? std::size_t{11} : static_cast<std::size_t>(requested);
  return static_cast<int>(strip::odd_nodes(base, std::size_t{11}));
}

// Log-moneyness grid k_i = -kh + i·Δk (K_i = F·e^{k_i}) plus the served IV, the
// discounted Black-76 call, and the discounted OTM premium at each node. Shared
// by the var-swap and risk-neutral-density strips so both build the grid ONCE.
struct PriceGrid {
  std::vector<double> k;     // log-forward-moneyness nodes, ascending, symmetric ±kh
  std::vector<double> K;     // absolute strikes F·e^{k_i}
  std::vector<double> sigma; // served IV, NaN wings flat-filled to nearest finite
  std::vector<double> C;     // discounted Black-76 call at each node
  std::vector<double> O;     // discounted OTM premium: put if K < F else call
  double F{0.0};
  double df{0.0};
  double dk{0.0};           // uniform log-moneyness spacing 2·kh/(n-1)
  bool extrapolated{false}; // IV was flat-filled (NaN wing) or the tenor is
                            // outside the fitted pillar T-range (flat-extrapolated)
};

// Composite Simpson of f(0..n-1) in the uniform variable with spacing dk. `n`
// MUST be odd (odd_grid_size) so the interval count n-1 is even and the rule is
// exact for cubics. `f(i)` returns the integrand sample at node i.
template <typename Fn> [[nodiscard]] double simpson_weighted(std::size_t n, double dk, Fn &&f) {
  double acc = f(0) + f(n - 1);
  for (std::size_t i = 1; i + 1 < n; ++i) {
    // E2: shared weight, identical to the retired inline (i % 2 ? 4 : 2).
    acc += strip::simpson_weight(i, n) * f(i);
  }
  return acc * dk / 3.0;
}

// Build the shared price grid. `F`, `T`, `df` are the caller's validated carry.
// The half-width in k is max(|k_min|, k_max, width_sigmas·σ_atm·√T): the vol-
// scaled term keeps enough tail coverage for long-dated / high-vol tenors, with
// [k_min, k_max] as a floor. NaN wings are flat-extrapolated to the nearest
// finite IV so every Black-76 price stays finite.
[[nodiscard]] PriceGrid build_price_grid(const PricedSurface &ps, double F, double T, double df,
                                         const RndConfig &cfg) {
  const int n = odd_grid_size(cfg.n_grid);
  const std::size_t nn = static_cast<std::size_t>(n);

  // E2: span policy delegated to the shared strip/grid convention
  // (`strip_grid.hpp`). Bit-identical to the retired inline
  // `max(max(-k_min,k_max), width_sigmas*atm*sqrt(T))` — this TU is where the
  // policy was already RIGHT; E2 propagates it to the derivatives var strip
  // rather than changing it here.
  const double atm = ps.iv(F, T);
  const double kh =
      strip::adaptive_half_width(std::max(-cfg.k_min, cfg.k_max), atm, T, cfg.width_sigmas);

  PriceGrid g;
  g.F = F;
  g.df = df;
  g.dk = 2.0 * kh / static_cast<double>(n - 1);
  g.k.resize(nn);
  g.K.resize(nn);
  g.sigma.resize(nn);
  g.C.resize(nn);
  g.O.resize(nn);
  for (std::size_t i = 0; i < nn; ++i) {
    g.k[i] = -kh + g.dk * static_cast<double>(i);
    g.K[i] = F * std::exp(g.k[i]);
  }

  // One fused batch resolves the shared-T bracket / carry ONCE and fills the
  // served IV — bit-identical to a per-node ps.iv(K_i, T) scalar loop.
  std::vector<double> Tspan(nn, T);
  std::vector<Side> sides(nn, Side::Call); // IV is side-independent
  std::vector<double> price_scratch(nn);
  std::vector<Status> status_scratch(nn);
  const Status batch = ps.evaluate_batch(
      g.K, Tspan, sides, PricedSurface::EvalField::Iv, /*analytic=*/false,
      PricedSurface::EvaluationSoA{g.sigma, price_scratch, {}, status_scratch, {}, {}});
  if (!batch.has_value()) {
    // Well-formed spans make this unreachable; fall back to the scalar carry path.
    for (std::size_t i = 0; i < nn; ++i) {
      g.sigma[i] = ps.iv(g.K[i], T);
    }
  }

  // Two-pass flat-fill of the NaN wings. Forward: each NaN takes the nearest
  // finite IV to its left; backward: still-NaN leading nodes take the right.
  double last = kNaN;
  for (std::size_t i = 0; i < nn; ++i) {
    if (std::isfinite(g.sigma[i])) {
      last = g.sigma[i];
    } else if (std::isfinite(last)) {
      g.sigma[i] = last;
      g.extrapolated = true;
    }
  }
  double next = kNaN;
  for (std::size_t i = nn; i-- > 0;) {
    if (std::isfinite(g.sigma[i])) {
      next = g.sigma[i];
    } else if (std::isfinite(next)) {
      g.sigma[i] = next;
      g.extrapolated = true;
    }
  }

  // A tenor outside the fitted pillar T-range is served by a FLAT-EXTRAPOLATED
  // smile (no NaN wing, but every node's IV is an extrapolation, not a fitted
  // number) — flag it too so the density carries the same caveat.
  const auto ctx = ps.context();
  if (!ctx.empty() && (T < ctx.front().T || T > ctx.back().T)) {
    g.extrapolated = true;
  }

  // Discounted Black-76 call and OTM premium (put below the forward, call
  // at/above it) at each node.
  for (std::size_t i = 0; i < nn; ++i) {
    const double call = black76_price(F, g.K[i], T, g.sigma[i], df, Side::Call);
    g.C[i] = call;
    g.O[i] = (g.K[i] < F) ? black76_price(F, g.K[i], T, g.sigma[i], df, Side::Put) : call;
  }
  return g;
}

// Inverse CDF by linear interpolation: the strike where the (monotone) CDF first
// reaches probability p. Flat outside [cdf.front(), cdf.back()].
[[nodiscard]] double inverse_cdf(const std::vector<double> &K, const std::vector<double> &cdf,
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
[[nodiscard]] double interp_cdf_at(const std::vector<double> &K, const std::vector<double> &cdf,
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

} // namespace

Result<double> var_swap_vol(const PricedSurface &ps, double T, const RndConfig &cfg) {
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: T must be finite and > 0");
  }
  const double F = ps.forward_at(T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: non-positive forward");
  }
  const double df = std::exp(-ps.rate_at(T) * T);
  const double er = 1.0 / df; // e^{rT}

  const PriceGrid g = build_price_grid(ps, F, T, df, cfg);
  if (!(g.dk > 0.0) || !std::isfinite(g.dk)) {
    return Err(ErrorCode::InvalidArgument, "var_swap_vol: degenerate strike grid");
  }
  const std::size_t nn = g.K.size();

  // MFIV log-strip integrated in k with the Jacobian:
  //   K_var = (2·e^{rT}/T)·∫ O(K)/K² dK = (2·e^{rT}/T)·Simpson_k[O_i/K_i].
  const double strip = simpson_weighted(nn, g.dk, [&](std::size_t i) { return g.O[i] / g.K[i]; });
  const double k_var = (2.0 * er / T) * strip;
  return Ok(std::sqrt(k_var > 0.0 ? k_var : 0.0));
}

Result<RiskNeutralDensity> risk_neutral_density(const PricedSurface &ps, double T,
                                                const RndConfig &cfg) {
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: T must be finite and > 0");
  }
  const double F = ps.forward_at(T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: non-positive forward");
  }
  const double df = std::exp(-ps.rate_at(T) * T);
  const double er = 1.0 / df; // e^{rT}

  const PriceGrid g = build_price_grid(ps, F, T, df, cfg);
  const double dk = g.dk;
  if (!(dk > 0.0) || !std::isfinite(dk)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: degenerate strike grid");
  }
  const std::size_t nn = g.K.size();

  // Breeden–Litzenberger: q(K) = (1/df)·C''(K). The strikes are LOG-spaced, so
  // use the NON-UNIFORM 3-point second derivative (h1 = K_i−K_{i-1},
  // h2 = K_{i+1}−K_i). Endpoints copy their interior neighbour; negatives
  // (butterfly-arb noise) and NaN are clamped to 0.
  std::vector<double> q(nn, 0.0);
  for (std::size_t i = 1; i + 1 < nn; ++i) {
    const double h1 = g.K[i] - g.K[i - 1];
    const double h2 = g.K[i + 1] - g.K[i];
    const double d2 =
        2.0 * (h2 * g.C[i - 1] - (h1 + h2) * g.C[i] + h1 * g.C[i + 1]) / (h1 * h2 * (h1 + h2));
    double qi = er * d2;
    if (!(qi > 0.0)) {
      qi = 0.0;
    }
    q[i] = qi;
  }
  q[0] = q[1];
  q[nn - 1] = q[nn - 2];

  // Risk-neutral mass ∫q(K)dK = Simpson_k[q_i·K_i].
  const double mass = simpson_weighted(nn, dk, [&](std::size_t i) { return q[i] * g.K[i]; });
  if (!(mass > 0.0) || !std::isfinite(mass)) {
    return Err(ErrorCode::InvalidArgument, "risk_neutral_density: degenerate density");
  }

  // Normalized density in K (pdf_i = q_i/mass) and a CENTERED CDF: cumulative
  // trapezoid of pdf·K in k (fixes the left-Riemann +½·pdf·dK bias), renormalized
  // to end at 1 and clamped monotone to [0, 1].
  std::vector<double> pdf(nn);
  for (std::size_t i = 0; i < nn; ++i) {
    pdf[i] = q[i] / mass;
  }
  std::vector<double> cdf(nn);
  cdf[0] = 0.0;
  for (std::size_t i = 1; i < nn; ++i) {
    const double g0 = pdf[i - 1] * g.K[i - 1];
    const double g1 = pdf[i] * g.K[i];
    cdf[i] = cdf[i - 1] + 0.5 * (g0 + g1) * dk;
  }
  const double cdf_total = cdf[nn - 1];
  for (std::size_t i = 0; i < nn; ++i) {
    double c = (cdf_total > 0.0) ? cdf[i] / cdf_total : cdf[i];
    c = std::clamp(c, 0.0, 1.0);
    cdf[i] = c;
  }

  // Price-space moments about the RND mean, integrated in k with the Jacobian.
  const double mean =
      simpson_weighted(nn, dk, [&](std::size_t i) { return g.K[i] * pdf[i] * g.K[i]; });
  const double variance = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double d = g.K[i] - mean;
    return d * d * pdf[i] * g.K[i];
  });
  const double m3 = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double d = g.K[i] - mean;
    return d * d * d * pdf[i] * g.K[i];
  });
  const double m4 = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double d = g.K[i] - mean;
    return d * d * d * d * pdf[i] * g.K[i];
  });
  const double sd = std::sqrt(variance > 0.0 ? variance : 0.0);
  const double skewness = (variance > 0.0) ? m3 / (variance * sd) : 0.0; // m3 / var^{1.5}
  const double kurtosis = (variance > 0.0) ? m4 / (variance * variance) : 0.0;

  // BKM model-free moments on the log return R = k_i = ln(K_i/F). Carr–Madan OTM
  // strip integrated in k with the Jacobian: er·V, er·W, er·X are E[R²], E[R³],
  // E[R⁴].
  const double V = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double R = g.k[i];
    return (2.0 * (1.0 - R)) / (g.K[i] * g.K[i]) * g.O[i] * g.K[i];
  });
  const double W = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double R = g.k[i];
    return (6.0 * R - 3.0 * R * R) / (g.K[i] * g.K[i]) * g.O[i] * g.K[i];
  });
  const double X = simpson_weighted(nn, dk, [&](std::size_t i) {
    const double R = g.k[i];
    return (12.0 * R * R - 4.0 * R * R * R) / (g.K[i] * g.K[i]) * g.O[i] * g.K[i];
  });
  // Forward-referenced drift: with R = ln(S_T/F), E[e^R] = 1, so
  // E[R] = −E[R²]/2 − E[R³]/6 − E[R⁴]/24. The classic spot-referenced "e^{rT}−1"
  // lead term is absent precisely because we reference the forward, not spot;
  // keeping it would inject a spurious skew into a symmetric lognormal.
  const double mu = -(er / 2.0) * V - (er / 6.0) * W - (er / 24.0) * X;
  const double bkm_variance = er * V - mu * mu;
  const double bkm_var_pos = bkm_variance > 0.0 ? bkm_variance : 0.0;
  const double bkm_skew = (bkm_var_pos > 0.0) ? (er * W - 3.0 * mu * er * V + 2.0 * mu * mu * mu) /
                                                    std::pow(bkm_var_pos, 1.5)
                                              : 0.0;
  const double bkm_kurt =
      (bkm_var_pos > 0.0)
          ? (er * X - 4.0 * mu * er * W + 6.0 * er * mu * mu * V - 3.0 * mu * mu * mu * mu) /
                (bkm_var_pos * bkm_var_pos)
          : 0.0;
  const double skew_index = 100.0 - 10.0 * bkm_skew;

  // Model-free implied variance off the SAME strip (matches var_swap_vol exactly).
  const double vs_strip = simpson_weighted(nn, dk, [&](std::size_t i) { return g.O[i] / g.K[i]; });
  const double k_var = (2.0 * er / T) * vs_strip;
  const double var_swap = std::sqrt(k_var > 0.0 ? k_var : 0.0);

  // Inverse-CDF quantiles and P(S_T ≤ F) off the centered CDF.
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
  out.variance = variance;
  out.skewness = skewness;
  out.kurtosis = kurtosis;
  out.bkm_variance = bkm_variance;
  out.bkm_skew = bkm_skew;
  out.bkm_kurt = bkm_kurt;
  out.skew_index = skew_index;
  out.var_swap_vol = var_swap;
  out.mass_before_norm = mass;
  out.extrapolated = g.extrapolated;
  out.quantile_p = std::move(quantile_p);
  out.quantile_k = std::move(quantile_k);
  out.prob_below_forward = prob_below_forward;
  out.valid = true;
  return Ok(std::move(out));
}

double implied_cdf(const PricedSurface &ps, double T, double K,
                   const RndConfig & /*cfg*/) noexcept {
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

} // namespace atx::vol
