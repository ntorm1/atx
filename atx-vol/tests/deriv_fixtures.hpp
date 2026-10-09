#pragma once

// Shared vol-derivatives test fixtures — Task 0 of the vol-derivs production
// sprint. Centralizes the flat/skewed eSSVI surface + curve-set builders that
// `derivatives_test.cpp` hand-rolled per-TU, and adds a REALISTIC skewed
// surface with known analytic ATM-skew structure plus an independent
// seeded-Monte-Carlo realized-variance oracle. Header-only: every entry point
// is a small `inline` pure builder, safe to include from any number of test
// translation units with no ODR risk (matches `tests/support/
// analytics_fixture.hpp`'s convention).
//
// ── Fixture surface family (make_flat_surface / make_skew_surface) ────────
//
// Both builders stack slices on the SAME canonical 4-pillar tenor grid
// (`kFixturePillarsT`: 1M/3M/6M/1Y) into the legacy `EssviSurface` container
// (`src/fitting/legacy_surface.hpp`) — the surface type `derivatives.hpp`'s
// templated fair-strike/pricing entries were written against, and the type
// `derivatives_test.cpp`'s existing `make_flat_surface`/`make_steep_wing_
// surface` helpers already build by hand. `make_curves` builds a `CurveSet`
// on the same pillar grid so a caller pairs the two directly.
//
// eSSVI backbone (surface.hpp): w(k) = (theta/2)(1 + rho*phi*k +
// sqrt((phi*k+rho)^2 + (1-rho^2))). At k == 0 this collapses to w(0) == theta
// for ANY (phi, rho) — both builders exploit that identity to pin ATM total
// variance to theta_i = sigma^2 * T_i (or atm_vol^2 * T_i) independent of the
// skew/curvature parameters, exactly mirroring `derivatives_test.cpp`'s flat
// fixture convention.
//
// ── make_skew_surface's parameter mapping ──────────────────────────────────
//
// The ATM total-variance skew slope is a closed form: dw/dk(0) = theta * phi
// * rho (differentiate the backbone above at k=0). Substituting theta = atm_
// vol^2 * T and converting to IV-space (w = iv^2 * T => dw/dk = 2*iv*T*
// d(iv)/dk) gives an ATM IV-skew-slope of
//
//     d(iv)/dk(0) = atm_vol * phi * rho / 2
//
// which is INDEPENDENT OF T when phi is held fixed across pillars (theta's T
// dependence cancels exactly). `make_skew_surface` fixes rho == kSkewRho
// (~-0.7, a realistic steep equity put-skew shape per the sprint brief) and
// solves this identity for phi AT THE REFERENCE TENOR `kSkewRefT` (3M) to hit
// the caller's `skew_slope`:
//
//     phi_ref = 2 * skew_slope / (atm_vol * kSkewRho)
//
// `convexity` then drives a power-law term-structure DECAY of phi across the
// other pillars (the classical SSVI phi(theta) ~ theta^-gamma shape, applied
// here directly in T since theta ~ T at fixed atm_vol):
//
//     phi(T) = phi_ref * (kSkewRefT / T) ^ convexity
//
// convexity > 0 steepens the short end and flattens the long end (phi(1M) >
// phi_ref > phi(1Y)) — the realistic shape real equity boards show. Every
// resulting phi is checked against the Gatheral-Jacquier butterfly ceiling
// `essvi_phi_max(theta, rho) = min(4/(theta(1+|rho|)), 2/sqrt(theta(1+|rho|)))`
// (vol_surface.hpp) — the SAME ceiling the production eSSVI cube fit enforces
// (essvi_calib.cpp's `lee_project` / Mingone cube clamp) — so a slice this
// builder produces is butterfly-arb-free by the same construction the fit
// pipeline guarantees, not merely by accident of the chosen constants.
//
// ── MC realized-variance oracle ────────────────────────────────────────────
//
// `mc_realized_variance` is an INDEPENDENT oracle for the strip/aged-dispatch
// math in derivatives.cpp: it never calls into RealizedTracker or the strip
// quadrature, only `std::mt19937_64` + `std::normal_distribution` exact
// Black-Scholes log-return increments (no discretization error — the
// increments ARE the exact GBM transition density at each step). Determinism
// is by construction: a fixed seed drives a fixed, purely sequential draw
// sequence, so two calls with the same seed produce bit-identical output.

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "fitting/legacy_surface.hpp"           // EssviSurface
#include "atx/vol/api/pricing/rates_curve.hpp"  // CurveSet, ForwardPoint
#include "atx/vol/api/fitting/surface.hpp"      // EssviSlice
#include "atx/vol/api/core/types.hpp"           // Status
#include "atx/vol/api/fitting/vol_surface.hpp"  // essvi_phi_max

namespace atx::vol::deriv_testkit {

// Canonical fixture tenor grid (year-fractions), ascending: 1M, 3M, 6M, 1Y.
// Every builder in this header stacks its slices/pillars on exactly this
// grid so `make_flat_surface`/`make_skew_surface`/`make_curves` outputs are
// always tenor-compatible with each other.
inline constexpr std::array<double, 4> kFixturePillarsT = {
    1.0 / 12.0, // 1M
    0.25,       // 3M
    0.5,        // 6M
    1.0,        // 1Y
};

// Reference tenor `make_skew_surface`'s `skew_slope` is calibrated at (must
// be one of `kFixturePillarsT`).
inline constexpr double kSkewRefT = 0.25; // 3M

// Fixed ATM correlation for the skew fixture — "rho ~= -0.7-equivalent skew"
// per the sprint brief: a realistic steep equity put-skew shape.
inline constexpr double kSkewRho = -0.7;

// Flat lognormal surface: iv(k, T) == sigma at every k, for every pillar in
// `kFixturePillarsT`. theta_i = sigma^2 * T_i, phi ~ 0 (not exactly 0 — an
// eps floor matches `derivatives_test.cpp`'s existing pattern and keeps the
// slice off the essvi_phi_max(theta, 0) == 0 degenerate boundary), rho = 0.
[[nodiscard]] inline EssviSurface make_flat_surface(double sigma) {
  assert(sigma > 0.0 && "make_flat_surface: sigma must be positive");
  EssviSurface surf(kFixturePillarsT.size());
  for (std::size_t i = 0; i < kFixturePillarsT.size(); ++i) {
    const double T = kFixturePillarsT[i];
    const EssviSlice slice{sigma * sigma * T, 1.0e-6, 0.0, T};
    [[maybe_unused]] const Status st = surf.set_slice(i, slice);
    assert(st.has_value() && "make_flat_surface: set_slice failed");
  }
  return surf;
}

// Realistic skewed surface — see the file-header derivation. `atm_vol` sets
// theta_i = atm_vol^2 * T_i (ATM iv == atm_vol at every pillar exactly, as
// for `make_flat_surface`); `skew_slope` sets the ATM IV-skew-slope
// d(iv)/dk(0) at the `kSkewRefT` (3M) pillar; `convexity` is the power-law
// exponent shaping how that curvature decays/grows across the other
// pillars. rho is fixed at `kSkewRho` on every slice (calendar-ordered,
// same shape at every tenor). Every (theta_i, phi_i, kSkewRho) triple is
// asserted inside the Gatheral-Jacquier butterfly bound (documented
// precondition on the CALLER's (atm_vol, skew_slope, convexity) choice, not
// silently clamped — mirrors `Surface<Slice>`'s own "precondition, not
// verified [in release]" convention).
[[nodiscard]] inline EssviSurface make_skew_surface(double atm_vol, double skew_slope,
                                                     double convexity) {
  assert(atm_vol > 0.0 && "make_skew_surface: atm_vol must be positive");
  const double phi_ref = 2.0 * skew_slope / (atm_vol * kSkewRho);
  EssviSurface surf(kFixturePillarsT.size());
  for (std::size_t i = 0; i < kFixturePillarsT.size(); ++i) {
    const double T = kFixturePillarsT[i];
    const double theta = atm_vol * atm_vol * T;
    const double phi = phi_ref * std::pow(kSkewRefT / T, convexity);
    assert(phi > 0.0 && "make_skew_surface: derived phi must be positive");
    assert(phi < essvi_phi_max(theta, kSkewRho) &&
           "make_skew_surface: phi breaches the Gatheral-Jacquier butterfly bound "
           "-- choose a smaller |skew_slope| or |convexity|");
    const EssviSlice slice{theta, phi, kSkewRho, T};
    [[maybe_unused]] const Status st = surf.set_slice(i, slice);
    assert(st.has_value() && "make_skew_surface: set_slice failed");
  }
  return surf;
}

// CurveSet on the same canonical pillar grid: flat continuously-compounded
// yield `r`, and a forward point at every pillar F_i = spot * exp((r - q) *
// T_i) — a nontrivial (constant, nonzero) carry r - q, unlike
// `derivatives_test.cpp`'s existing zero-rate `make_flat_curves`. The yield
// curve's own pillars bracket the fixture grid with room either side
// (matches `derivatives_test.cpp`'s existing `make_flat_curves` spacing
// convention) so `resolve_forward`/`YieldCurve::disc` never hit the flat-
// extrapolation edge for any T in or near `kFixturePillarsT`.
[[nodiscard]] inline CurveSet make_curves(double spot, double r, double q) {
  assert(spot > 0.0 && "make_curves: spot must be positive");
  CurveSet cs;
  cs.spot = spot;

  const double t_lo = kFixturePillarsT.front();
  const double t_hi = kFixturePillarsT.back();
  const double t[] = {t_lo * 0.5, 0.5 * (t_lo + t_hi), t_hi * 2.0};
  const double zero[] = {r, r, r};
  [[maybe_unused]] const Status st = cs.set_yield(t, zero);
  assert(st.has_value() && "make_curves: set_yield failed");

  std::vector<ForwardPoint> pts(kFixturePillarsT.size());
  for (std::size_t i = 0; i < kFixturePillarsT.size(); ++i) {
    pts[i].T = kFixturePillarsT[i];
    pts[i].F = spot * std::exp((r - q) * kFixturePillarsT[i]);
  }
  cs.forward.set(pts);
  return cs;
}

// ── Seeded Monte-Carlo realized-variance oracle ────────────────────────────

// Black-Scholes model parameters the MC path simulation needs. `spot` is
// carried for interface completeness (a log-return sum is scale-free in
// spot) but not read by `mc_realized_variance` itself.
struct McModelParams {
  double spot{};
  double r{};
  double q{};
  double sigma{};
  double T{};
};

struct McRvResult {
  double mean_rv{};   // sample mean of annualized realized variance
  double stderr_rv{}; // standard error of that mean
  std::uint64_t n_paths{};
};

// Simulate `n_paths` independent GBM paths of `n_steps` discrete-monitoring
// increments each over [0, params.T], and return the sample mean/stderr of
// the annualized realized variance RV = (1/T) * sum(log-return^2). Each
// increment is an EXACT draw from the GBM transition density (no Euler
// discretization error): dlogS_i = (r - q - sigma^2/2)*dt + sigma*sqrt(dt)*Z,
// dt = T/n_steps, Z ~ N(0,1) from `std::mt19937_64`(seed) +
// `std::normal_distribution`.
//
// Closed form this reproduces (flat-BS, discrete monitoring):
//   E[RV] = sigma^2 + (T/n_steps) * (r - q - sigma^2/2)^2
// (derivation: each increment has mean mu*dt and variance sigma^2*dt with mu
// = r-q-sigma^2/2, so E[sum r_i^2] = n_steps*sigma^2*dt + n_steps*mu^2*dt^2 =
// sigma^2*T + mu^2*dt*T; dividing by T gives the formula above.)
//
// Determinism: a fixed `seed` drives a fixed, purely sequential (no
// threading) draw sequence, so two calls with identical arguments produce
// bit-identical `mean_rv`/`stderr_rv`.
//
// Uses Welford's online algorithm for the running mean/variance (single
// pass, no per-path storage, numerically stable at n_paths in the 1e5-1e6
// range) rather than accumulating sum/sum-of-squares directly.
[[nodiscard]] inline McRvResult mc_realized_variance(const McModelParams &params,
                                                      std::uint64_t n_paths,
                                                      std::uint32_t n_steps,
                                                      std::uint64_t seed) {
  assert(params.T > 0.0 && "mc_realized_variance: T must be positive");
  assert(params.sigma > 0.0 && "mc_realized_variance: sigma must be positive");
  assert(n_paths > 0 && "mc_realized_variance: n_paths must be positive");
  assert(n_steps > 0 && "mc_realized_variance: n_steps must be positive");

  const double dt = params.T / static_cast<double>(n_steps);
  const double drift = (params.r - params.q - 0.5 * params.sigma * params.sigma) * dt;
  const double vol_dt = params.sigma * std::sqrt(dt);

  std::mt19937_64 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);

  double mean = 0.0;
  double m2 = 0.0;
  for (std::uint64_t p = 1; p <= n_paths; ++p) {
    double sum_sq_ret = 0.0;
    for (std::uint32_t i = 0; i < n_steps; ++i) {
      const double dlog_s = drift + vol_dt * z(rng);
      sum_sq_ret += dlog_s * dlog_s;
    }
    const double rv = sum_sq_ret / params.T;
    const double delta = rv - mean;
    mean += delta / static_cast<double>(p);
    const double delta2 = rv - mean;
    m2 += delta * delta2;
  }

  const double n = static_cast<double>(n_paths);
  const double sample_var = (n_paths > 1) ? m2 / (n - 1.0) : 0.0;
  return McRvResult{mean, std::sqrt(sample_var / n), n_paths};
}

// Task F-2 (PV-F1 / LIT-7): the gamma-swap sibling of `mc_realized_variance`
// above -- IDENTICAL GBM path simulation (same exact-transition-density
// increments, same seed/determinism contract), extended with Lee's w(y) =
// y/Y0 weight: RV_gamma = (1/T) * sum_i (S_i/S0) * r_i^2, S0 the path's own
// starting spot (`params.spot`, which THIS function actually reads, unlike
// the plain estimator above where it is scale-free and unused) and S_i the
// spot immediately AFTER the i-th return -- the same "weight applies to the
// return just realized" convention `RealizedTracker::observe` (derivatives.
// cpp) uses.
//
// Closed form this reproduces in the CONTINUOUS-MONITORING (n_steps -> inf)
// limit, for FLAT Black-Scholes: E[RV_gamma] = sigma^2 * (e^{(r-q)T} - 1) /
// ((r-q)*T), collapsing to sigma^2 exactly at r == q (l'Hopital) -- see
// `GammaSwap.CarryApproximationClosedForm` (derivatives_test.cpp) for the
// from-scratch re-derivation and its comparison against the strip's own
// single-expiry K_gamma.
[[nodiscard]] inline McRvResult mc_gamma_realized_variance(const McModelParams &params,
                                                            std::uint64_t n_paths,
                                                            std::uint32_t n_steps,
                                                            std::uint64_t seed) {
  assert(params.T > 0.0 && "mc_gamma_realized_variance: T must be positive");
  assert(params.sigma > 0.0 && "mc_gamma_realized_variance: sigma must be positive");
  assert(params.spot > 0.0 && "mc_gamma_realized_variance: spot must be positive");
  assert(n_paths > 0 && "mc_gamma_realized_variance: n_paths must be positive");
  assert(n_steps > 0 && "mc_gamma_realized_variance: n_steps must be positive");

  const double dt = params.T / static_cast<double>(n_steps);
  const double drift = (params.r - params.q - 0.5 * params.sigma * params.sigma) * dt;
  const double vol_dt = params.sigma * std::sqrt(dt);

  std::mt19937_64 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);

  double mean = 0.0;
  double m2 = 0.0;
  for (std::uint64_t p = 1; p <= n_paths; ++p) {
    double log_ratio = 0.0;  // running ln(S_i / S0)
    double sum_weighted_sq_ret = 0.0;
    for (std::uint32_t i = 0; i < n_steps; ++i) {
      const double dlog_s = drift + vol_dt * z(rng);
      log_ratio += dlog_s;
      const double weight = std::exp(log_ratio);  // S_i / S0
      sum_weighted_sq_ret += weight * dlog_s * dlog_s;
    }
    const double rv = sum_weighted_sq_ret / params.T;
    const double delta = rv - mean;
    mean += delta / static_cast<double>(p);
    const double delta2 = rv - mean;
    m2 += delta * delta2;
  }

  const double n = static_cast<double>(n_paths);
  const double sample_var = (n_paths > 1) ? m2 / (n - 1.0) : 0.0;
  return McRvResult{mean, std::sqrt(sample_var / n), n_paths};
}

} // namespace atx::vol::deriv_testkit
