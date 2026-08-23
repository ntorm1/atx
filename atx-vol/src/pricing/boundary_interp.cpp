#include "pricing/boundary_interp.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <span>

#include "pricing/al_probe.hpp"          // env-gated AL hot-path zone timers (Perf 2b step 1)
#include "pricing/american_boundary.hpp" // amer:: seam
#include "atx/core/math.hpp"     // atx::core::clamp
#include "atx/vol/api/pricing/black76.hpp"   // black76_price (European legs)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using detail::classify_regime;
using detail::ExerciseRegime;
using detail::SigmaBoundaryInterp;

namespace {

inline constexpr double kPi = 3.14159265358979323846;

// σ-axis Chebyshev-Lobatto node i of n, in [-1, 1]. IDENTICAL to american.cpp's
// al_cheb_node — re-expressed so the hot τ-axis kernel stays internal-linkage.
[[nodiscard]] double cheb_node(unsigned i, unsigned n) noexcept {
  if (n <= 1) {
    return 0.0;
  }
  if (i == 0) {
    return -1.0;
  }
  if (i == n - 1) {
    return 1.0;
  }
  return -std::cos(kPi * static_cast<double>(i) / static_cast<double>(n - 1));
}

// 2nd-kind barycentric Lagrange interpolation of y[] on nodes z[] with weights
// w[], evaluated at zq. IDENTICAL scheme to american.cpp's al_cheb_eval_t<0>.
[[nodiscard]] double bary_eval(const double *z, const double *w, const double *y, unsigned n,
                               double zq) noexcept {
  if (n == 0) {
    return 0.0;
  }
  if (n == 1) {
    return y[0];
  }
  double num = 0.0;
  double den = 0.0;
  for (unsigned i = 0; i < n; ++i) {
    const double dz = zq - z[i];
    if (dz == 0.0) {
      return y[i];
    }
    const double qq = w[i] / dz;
    num += qq * y[i];
    den += qq;
  }
  return num / den;
}

// An odd Lobatto grid contains a lower-order Lobatto grid at its even indices
// (9 nodes contain 5). Recompute the embedded grid's alternating weights: the
// full grid's even-index weights are not the embedded grid's weights.
[[nodiscard]] double bary_eval_embedded_even(const double *z, const double *y, unsigned n,
                                             double zq) noexcept {
  if (n < 3 || (n & 1u) == 0u) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const unsigned embedded_n = (n + 1u) / 2u;
  double num = 0.0;
  double den = 0.0;
  for (unsigned j = 0; j < embedded_n; ++j) {
    const unsigned i = 2u * j;
    const double dz = zq - z[i];
    if (dz == 0.0) {
      return y[i];
    }
    double weight = (j & 1u) != 0u ? -1.0 : 1.0;
    if (j == 0u || j + 1u == embedded_n) {
      weight *= 0.5;
    }
    const double quotient = weight / dz;
    num += quotient * y[i];
    den += quotient;
  }
  return num / den;
}

// Shared message for the double-continuation corner the ALO scheme cannot price
// (matches american.cpp's kDoubleContinuationMsg).
constexpr const char *kDoubleContinuationMsg =
    "double-continuation regime (put q < r <= 0 / call r < q <= 0): the "
    "single-boundary Andersen-Lake scheme cannot represent two exercise "
    "boundaries; see Andersen-Lake 2021 (double-boundary case)";

// Cold reference price of ONE internal-put strike — bit-identical to the
// andersen_lake American arm (same amer:: primitives, same clamp order). Used as
// the reference AND the ColdFallback path. Returns nullopt on a boundary
// collapse / table-missing (caller surfaces the error).
[[nodiscard]] std::optional<double> cold_internal_put(double Sp, double Kp, double T, double sigma,
                                                      double rp, double qp,
                                                      const amer::AlScheme &sch) noexcept {
  amer::AlBoundary bnd{};
  amer::AlWorkspace ws{};
  const amer::AlSolveStatus st = amer::al_solve_put_boundary(Kp, T, sigma, rp, qp, sch, bnd, ws);
  if (st != amer::AlSolveStatus::Ok) {
    return std::nullopt;
  }
  return amer::al_put_price_from_boundary(bnd, ws, Sp, Kp, T, sigma, rp, qp);
}

// Core of both public slice routes. `is_call` selects the McDonald-Schroder
// internal-put mapping; the regime/degenerate short-circuits are done by the
// callers (this runs only for the American regime, T > 0).
[[nodiscard]] Status slice_sigma_impl(bool is_call, double S, std::span<const double> strikes,
                                      std::span<const double> sigmas, double T, double r, double q,
                                      std::span<double> price_out, const SigmaInterpOptions &sopts,
                                      const std::optional<AlOpts> &opts, SigmaSliceStats *stats) {
  const alprobe::Scope probe_zone(alprobe::Zone::SliceSigma);
  const std::size_t n = strikes.size();
  const amer::AlScheme sch = amer::scheme_from_opts(opts);

  // Internal-put mapping, both halves single-sourced (see boundary_interp.hpp):
  // the per-strike (Sp, Kp) half via detail::internal_put_coords, the build-input
  // (rate, yield) half via detail::internal_put_rates. Kp_ref = S is a
  // homogeneity choice, not part of the duality itself.
  const Side side = is_call ? Side::Call : Side::Put;
  const detail::InternalPutRates rates = detail::internal_put_rates(side, r, q);
  const double rp = rates.rp;
  const double qp = rates.qp;
  const double Kp_ref = S;

  SigmaSliceStats st{};
  st.n_strikes = n;

  // ── σ clamp box ────────────────────────────────────────────────────────
  // Explicit box when both bounds set; else auto = [min, max] over the strikes
  // whose σ clears the small-σ guard (Chebyshev-Lobatto includes the endpoints,
  // so every in-guard σ is interpolation, never extrapolation).
  double sig_lo = sopts.sigma_lo;
  double sig_hi = sopts.sigma_hi;
  const bool auto_box = !(sig_lo > 0.0 && sig_hi > sig_lo);
  if (auto_box) {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
      if (sigmas[i] >= sopts.min_sigma) {
        lo = std::fmin(lo, sigmas[i]);
        hi = std::fmax(hi, sigmas[i]);
      }
    }
    sig_lo = lo;
    sig_hi = hi;
  }

  // ── build the interpolant (only when it can pay: flag on, T not near-expiry,
  // a non-degenerate box, and more strikes than σ-nodes) ──────────────────
  const bool box_ok = std::isfinite(sig_lo) && std::isfinite(sig_hi) && (sig_hi - sig_lo) > 1.0e-9;
  const bool want_interp =
      sopts.use_sigma_boundary_interp && (T >= sopts.min_tau) && box_ok && (n > sopts.n_sigma);
  SigmaBoundaryInterp interp;
  bool interp_ok = false;
  if (want_interp) {
    interp_ok = interp.build(Kp_ref, T, rp, qp, sig_lo, sig_hi, sopts.n_sigma, sch);
    if (interp_ok) {
      st.used_interp = true;
      st.n_sigma = sopts.n_sigma;
      st.n_boundary_solves += sopts.n_sigma;
      st.sigma_lo = sig_lo;
      st.sigma_hi = sig_hi;
    }
  }

  // ── per-strike routing ─────────────────────────────────────────────────
  for (std::size_t i = 0; i < n; ++i) {
    const double sig = sigmas[i];
    const auto [Sp, Kp] = detail::internal_put_coords(side, S, strikes[i]);

    // Degenerate σ collapses to intrinsic (no boundary solve).
    if (sig <= 1.0e-8) {
      const double intr = is_call ? (S - strikes[i]) : (strikes[i] - S);
      price_out[i] = (intr > 0.0) ? intr : 0.0;
      continue;
    }

    const bool route_interp = interp_ok && (sig >= sopts.min_sigma) && (sig >= sig_lo - 1.0e-12) &&
                              (sig <= sig_hi + 1.0e-12);
    if (route_interp) {
      price_out[i] = interp.price_side(side, S, strikes[i], sig);
      ++st.n_interp;
      continue;
    }

    // ColdFallback: bit-identical to andersen_lake at this strike.
    const std::optional<double> px = cold_internal_put(Sp, Kp, T, sig, rp, qp, sch);
    if (!px.has_value()) {
      return Err(ErrorCode::NotImplemented,
                 "andersen_lake_slice_sigma: asymptotic boundary collapsed (xmax <= 0)");
    }
    price_out[i] = *px;
    ++st.n_cold_fallback;
    ++st.n_boundary_solves; // a genuine cold solve
  }

  if (stats != nullptr) {
    *stats = st;
  }
  return Ok();
}

} // namespace

namespace detail {

bool SigmaBoundaryInterp::build(double Kp_ref, double T, double rp, double qp, double sigma_lo,
                                double sigma_hi, std::uint16_t n_sigma,
                                const amer::AlScheme &sch) noexcept {
  const alprobe::Scope probe_zone(alprobe::Zone::SigmaInterpBuild);
  ok_ = false;
  if (!(T > 1.0e-12) || !(sigma_hi > sigma_lo) || n_sigma < 2 || n_sigma > kSigmaMax) {
    return false;
  }
  if (!(amer::al_xmax_put(Kp_ref, rp, qp) > 0.0)) {
    return false; // non-American regime — no asymptotic boundary
  }
  sch_ = sch;
  T_ = T;
  rp_ = rp;
  qp_ = qp;
  sigma_lo_ = sigma_lo;
  sigma_hi_ = sigma_hi;
  n_sigma_ = n_sigma;
  n_boundary_ = sch.n_boundary;

  // σ Chebyshev-Lobatto nodes + 2nd-kind barycentric weights (endpoints halved).
  for (unsigned s = 0; s < n_sigma; ++s) {
    sz_[s] = cheb_node(s, n_sigma);
    double w = (s & 1u) ? -1.0 : 1.0;
    if (s == 0 || s + 1 == n_sigma) {
      w *= 0.5;
    }
    sw_[s] = w;
  }

  // n_σ cold solves at Kp_ref; store each collocation node's y[] across σ.
  //
  // R-11c investigated chaining al_solve_put_boundary_WARM across these nodes
  // (node 0 cold, each later node seeded from its converged neighbour) on the
  // theory that adjacent σ-nodes are small bumps. They are NOT, and the idea is
  // deliberately not taken up. A Chebyshev-Lobatto grid clusters at the box ENDS,
  // so its mid-box gaps are the widest in the grid: ~0.12 on the [0.15, 0.8]
  // smile box and ~0.03 even on a flat 0.24 board — 25x to 1200x the "~0.1% bump"
  // al_solve_put_boundary_warm is documented for. From that far away a converged
  // neighbour is a WORSE seed than the cold Barone-Adesi-Whaley approximation it
  // would replace, and because the sweep budget is fixed (n_iter_jn + n_iter_fp)
  // with only an early-exit on tol, the solve does not spend longer to recover —
  // it returns AlSolveStatus::Ok carrying an under-converged boundary. That is no
  // longer SILENT — al_solve_put_boundary's optional AlSolveResid out-parameter
  // reports the achieved residual (T1) — but Ok still does not imply convergence,
  // so a future warm-chaining attempt must gate on AlSolveResid::converged() rather
  // than on the status, and the measurement below is still what decides the case.
  //
  // Measured worst |node price - cold per-strike andersen_lake| (the σ-nodes are
  // where barycentric evaluation is exact, so this is the seed's error alone):
  //     cold build (this code)              7.1e-15
  //     warm-chained, box [0.084, 0.24]     3.1e-05
  //     warm-chained, box [0.15,  0.80]     1.3e-04
  // The warm error lands at the same scale as the route's own economic budget
  // (min(0.005, 0.1 x vega x 1e-4, half-spread)), and the 9-vs-5 embedded gate
  // could not catch it because both estimators would share the same wrong y[].
  // It bought ~3.5% of retained-arm throughput — far under the noise floor of the
  // measurement, let alone the accuracy. SigmaInterp.NodeBuildMatchesColdSolve
  // pins the cold build's 7.1e-15 and is the guard on any future attempt.
  const double half_span = 0.5 * (sigma_hi - sigma_lo);
  bool captured = false;
  for (unsigned s = 0; s < n_sigma; ++s) {
    const double sig_s = sigma_lo + half_span * (sz_[s] + 1.0);
    amer::AlBoundary bnd{};
    const amer::AlSolveStatus stv =
        amer::al_solve_put_boundary(Kp_ref, T, sig_s, rp, qp, sch, bnd, ws_);
    if (stv != amer::AlSolveStatus::Ok) {
      return false;
    }
    if (!captured) {
      scratch_ = bnd; // K-independent node structure (z/wbary/x/tau/n/T)
      captured = true;
    }
    for (unsigned k = 0; k < n_boundary_; ++k) {
      series_[k * kSigmaMax + s] = bnd.y[k];
    }
  }
  ok_ = true;
  return true;
}

double SigmaBoundaryInterp::price_internal_put(double Sp, double Kp, double sigma) noexcept {
  // Map σ into the box's z ∈ [-1, 1] (clamped: an out-of-box σ is the caller's
  // responsibility, but clamping keeps the barycentric sum well-posed).
  double z = 0.0;
  if (sigma_hi_ > sigma_lo_) {
    z = 2.0 * (sigma - sigma_lo_) / (sigma_hi_ - sigma_lo_) - 1.0;
    z = atx::core::clamp(z, -1.0, 1.0);
  }
  // Interpolate each collocation node's dimensionless y[] in σ.
  for (unsigned k = 0; k < n_boundary_; ++k) {
    scratch_.y[k] = bary_eval(sz_, sw_, &series_[k * kSigmaMax], n_sigma_, z);
  }
  // T8 homogeneity rescale to this strike, then the existing premium quadrature.
  scratch_.K = Kp;
  scratch_.xmax = amer::al_xmax_put(Kp, rp_, qp_);
  return amer::al_put_price_from_boundary(scratch_, ws_, Sp, Kp, T_, sigma, rp_, qp_);
}

double SigmaBoundaryInterp::price_internal_put_embedded(double Sp, double Kp,
                                                        double sigma) noexcept {
  if (!ok_ || n_sigma_ < 3u || (n_sigma_ & 1u) == 0u || sigma < sigma_lo_ || sigma > sigma_hi_) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double z = 2.0 * (sigma - sigma_lo_) / (sigma_hi_ - sigma_lo_) - 1.0;
  for (unsigned k = 0; k < n_boundary_; ++k) {
    scratch_.y[k] = bary_eval_embedded_even(sz_, &series_[k * kSigmaMax], n_sigma_, z);
  }
  scratch_.K = Kp;
  scratch_.xmax = amer::al_xmax_put(Kp, rp_, qp_);
  return amer::al_put_price_from_boundary(scratch_, ws_, Sp, Kp, T_, sigma, rp_, qp_);
}

} // namespace detail

// ── Public opt-in slice routes ───────────────────────────────────────────

Status andersen_lake_put_slice_sigma(double S, std::span<const double> strikes,
                                     std::span<const double> sigmas, double T, double r, double q,
                                     std::span<double> price_out, const SigmaInterpOptions &sopts,
                                     const std::optional<AlOpts> &opts, SigmaSliceStats *stats) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice_sigma: S must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice_sigma: T must be >= 0");
  }
  if (strikes.size() != price_out.size() || strikes.size() != sigmas.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_put_slice_sigma: strikes / sigmas / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_put_slice_sigma: every strike must be > 0");
    }
  }
  for (const double s : sigmas) {
    if (!(s >= 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_put_slice_sigma: every sigma must be >= 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice_sigma: r and q must be finite");
  }
  const std::size_t n = strikes.size();

  if (stats != nullptr) {
    *stats = SigmaSliceStats{};
    stats->n_strikes = n;
  }

  // Degenerate T ~ 0 -> put intrinsic max(K_i - S, 0) per strike.
  if (T <= 1.0e-12) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = strikes[i] - S;
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }

  switch (classify_regime(/*rate=*/r, /*yield=*/q)) {
  case ExerciseRegime::European: {
    const double F = S * std::exp((r - q) * T);
    const double df = std::exp(-r * T);
    for (std::size_t i = 0; i < n; ++i) {
      const double sig = sigmas[i];
      price_out[i] = (sig <= 1.0e-8) ? ((strikes[i] - S) > 0.0 ? strikes[i] - S : 0.0)
                                     : black76_price(F, strikes[i], T, sig, df, Side::Put);
    }
    return Ok();
  }
  case ExerciseRegime::Unsupported:
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  case ExerciseRegime::American:
    break;
  }

  return slice_sigma_impl(/*is_call=*/false, S, strikes, sigmas, T, r, q, price_out, sopts, opts,
                          stats);
}

Status andersen_lake_call_slice_sigma(double S, std::span<const double> strikes,
                                      std::span<const double> sigmas, double T, double r, double q,
                                      std::span<double> price_out, const SigmaInterpOptions &sopts,
                                      const std::optional<AlOpts> &opts, SigmaSliceStats *stats) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice_sigma: S must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice_sigma: T must be >= 0");
  }
  if (strikes.size() != price_out.size() || strikes.size() != sigmas.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice_sigma: strikes / sigmas / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_call_slice_sigma: every strike must be > 0");
    }
  }
  for (const double s : sigmas) {
    if (!(s >= 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_call_slice_sigma: every sigma must be >= 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice_sigma: r and q must be finite");
  }
  const std::size_t n = strikes.size();

  if (stats != nullptr) {
    *stats = SigmaSliceStats{};
    stats->n_strikes = n;
  }

  // Degenerate T ~ 0 -> call intrinsic max(S - K_i, 0) per strike.
  if (T <= 1.0e-12) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = S - strikes[i];
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }

  // Call regime is classified in the internal-put (rate=q, yield=r) terms.
  switch (classify_regime(/*rate=*/q, /*yield=*/r)) {
  case ExerciseRegime::European: {
    const double F = S * std::exp((r - q) * T);
    const double df = std::exp(-r * T);
    for (std::size_t i = 0; i < n; ++i) {
      const double sig = sigmas[i];
      price_out[i] = (sig <= 1.0e-8) ? ((S - strikes[i]) > 0.0 ? S - strikes[i] : 0.0)
                                     : black76_price(F, strikes[i], T, sig, df, Side::Call);
    }
    return Ok();
  }
  case ExerciseRegime::Unsupported:
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  case ExerciseRegime::American:
    break;
  }

  return slice_sigma_impl(/*is_call=*/true, S, strikes, sigmas, T, r, q, price_out, sopts, opts,
                          stats);
}

} // namespace atx::vol
