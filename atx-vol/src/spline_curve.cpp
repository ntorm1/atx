#include "atx/vol/spline_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve_spd
#include "atx/vol/arb.hpp"             // CalendarPairProjection (calendar cone)
#include "atx/vol/vol_curve.hpp"       // IVolCurve, SplineVolCurve (full class)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::linalg::MatX;
using atx::core::linalg::solve_spd;
using atx::core::linalg::VecX;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── Natural cubic spline core (Thomas algorithm on the tridiagonal system) ──
//
// Second derivatives M[0..n-1] of the natural cubic spline interpolating
// (x[i], y[i]), M[0] = M[n-1] = 0. n < 3 returns all-zero (a straight line
// through <=2 points has no curvature, which is the correct natural-spline
// answer). Pure function of its inputs; no allocation beyond the 5 working
// vectors sized to the interior system (K = n - 2).
[[nodiscard]] std::vector<double> natural_spline_m(std::span<const double> x,
                                                    std::span<const double> y) noexcept {
  const std::size_t n = x.size();
  std::vector<double> M(n, 0.0);
  if (n < 3) {
    return M;
  }
  const std::size_t K = n - 2;
  std::vector<double> h(n - 1);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    h[i] = x[i + 1] - x[i];
  }
  std::vector<double> a(K), b(K), c(K), d(K);
  for (std::size_t t = 0; t < K; ++t) {
    const std::size_t i = t + 1;
    a[t] = h[t];
    b[t] = 2.0 * (h[t] + h[t + 1]);
    c[t] = h[t + 1];
    d[t] = 6.0 * ((y[i + 1] - y[i]) / h[t + 1] - (y[i] - y[i - 1]) / h[t]);
  }
  std::vector<double> cp(K), dp(K);
  cp[0] = (K > 1) ? c[0] / b[0] : 0.0;
  dp[0] = d[0] / b[0];
  for (std::size_t t = 1; t < K; ++t) {
    const double denom = b[t] - a[t] * cp[t - 1];
    cp[t] = (t + 1 < K) ? c[t] / denom : 0.0;
    dp[t] = (d[t] - a[t] * dp[t - 1]) / denom;
  }
  std::vector<double> Y(K);
  Y[K - 1] = dp[K - 1];
  for (std::size_t ti = K - 1; ti > 0; --ti) {
    Y[ti - 1] = dp[ti - 1] - cp[ti - 1] * Y[ti];
  }
  for (std::size_t t = 0; t < K; ++t) {
    M[t + 1] = Y[t];
  }
  return M;
}

// Evaluate the natural cubic spline (x, y, M) at `z`, CLAMPING z into
// [x.front(), x.back()] first -- this is the "flat wings" extension the whole
// family relies on. n == 0 -> NaN; n == 1 -> the single knot value.
[[nodiscard]] double spline_eval(std::span<const double> x, std::span<const double> y,
                                 std::span<const double> M, double z) noexcept {
  const std::size_t n = x.size();
  if (n == 0) {
    return kNaN;
  }
  if (n == 1) {
    return y[0];
  }
  if (z <= x.front()) {
    z = x.front();
  } else if (z >= x.back()) {
    z = x.back();
  }
  const auto it = std::upper_bound(x.begin(), x.end(), z);
  std::size_t idx = static_cast<std::size_t>(it - x.begin());
  if (idx == 0) {
    idx = 1;
  } else if (idx >= n) {
    idx = n - 1;
  }
  --idx;  // interval [idx, idx+1]
  const double h = x[idx + 1] - x[idx];
  if (!(h > 0.0)) {
    return y[idx];
  }
  const double A = (x[idx + 1] - z) / h;
  const double B = (z - x[idx]) / h;
  return A * y[idx] + B * y[idx + 1] +
         ((A * A * A - A) * M[idx] + (B * B * B - B) * M[idx + 1]) * (h * h) / 6.0;
}

// ── Active-knot window selection ────────────────────────────────────────────
//
// Grid indices whose z lies in [z_lo - 1, z_hi + 1], expanded outward
// (never removing a matched point) until at least 4 knots are selected.
// Precondition: grid.size() >= 4 (checked by the caller), which guarantees
// termination.
[[nodiscard]] std::vector<std::size_t> select_active_knots(std::span<const double> grid,
                                                            double z_lo, double z_hi) noexcept {
  const std::size_t n = grid.size();
  const double lo = z_lo - 1.0;
  const double hi = z_hi + 1.0;
  std::size_t first = n;
  std::size_t last = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (grid[i] >= lo && grid[i] <= hi) {
      if (first == n) {
        first = i;
      }
      last = i;
    }
  }
  if (first == n) {
    // No grid point falls in the padded window (data sits inside a grid gap
    // wider than 2 units): seed on the single nearest grid point instead.
    const double mid = 0.5 * (z_lo + z_hi);
    double best_d = std::numeric_limits<double>::infinity();
    std::size_t best_i = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const double dist = std::fabs(grid[i] - mid);
      if (dist < best_d) {
        best_d = dist;
        best_i = i;
      }
    }
    first = best_i;
    last = best_i;
  }
  while ((last - first + 1) < 4 && (first > 0 || last + 1 < n)) {
    if (first > 0) {
      --first;
      if (last - first + 1 >= 4) {
        break;
      }
    }
    if (last + 1 < n) {
      ++last;
    }
  }
  std::vector<std::size_t> idx;
  idx.reserve(last - first + 1);
  for (std::size_t i = first; i <= last; ++i) {
    idx.push_back(i);
  }
  return idx;
}

// ── Post-fit Lee/Roper butterfly-density scan ───────────────────────────────
//
// Mirrors arb_check_butterfly's finite-difference scheme and tolerance
// exactly (arb.cpp): central differences on a uniform k-grid, sharing FD
// neighbours across sample points, g(k) < -1e-9 counted as a violation. A
// diagnostic only -- never rejects or projects the fit.
[[nodiscard]] std::size_t count_butterfly_violations(std::span<const double> zk,
                                                      std::span<const double> mult,
                                                      std::span<const double> m2nd, double atm,
                                                      double T, double z_lo,
                                                      double z_hi) noexcept {
  if (!(z_hi > z_lo) || !(atm > 0.0) || !(T > 0.0)) {
    return 0;
  }
  const double sqrtT = std::sqrt(T);
  const double k_lo = z_lo * atm * sqrtT;
  const double k_hi = z_hi * atm * sqrtT;
  if (!(k_hi > k_lo)) {
    return 0;
  }
  constexpr std::uint32_t kNGrid = 128;
  const double dk = (k_hi - k_lo) / static_cast<double>(kNGrid);
  const double inv_2dk = 0.5 / dk;
  const double inv_dksq = 1.0 / (dk * dk);

  const auto w_at = [&](double k) noexcept {
    const double z = k / (atm * sqrtT);
    const double m = spline_eval(zk, mult, m2nd, z);
    const double sigma = atm * m;
    return sigma * sigma * T;
  };

  std::size_t violations = 0;
  for (std::uint32_t g = 1; g < kNGrid; ++g) {
    const double k = k_lo + static_cast<double>(g) * dk;
    const double w_lo = w_at(k - dk);
    const double w_mi = w_at(k);
    const double w_hi = w_at(k + dk);
    if (!(w_mi > 1.0e-12) || !std::isfinite(w_lo) || !std::isfinite(w_hi)) {
      continue;
    }
    const double w_p = (w_hi - w_lo) * inv_2dk;
    const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;
    const double term1_inner = 1.0 - 0.5 * k * w_p / w_mi;
    const double term1 = term1_inner * term1_inner;
    const double term2 = 0.25 * w_p * w_p * (0.25 + 1.0 / w_mi);
    const double term3 = 0.5 * w_pp;
    const double g_density = term1 - term2 + term3;
    if (g_density < -1.0e-9) {
      ++violations;
    }
  }
  return violations;
}

}  // namespace

// ── SplineVolCurve (declared in vol_curve.hpp) ──────────────────────────────

SplineVolCurve::SplineVolCurve(SplineVolParams p, double T, double F, double df)
    : IVolCurve(T, F, df), p_(std::move(p)), m2nd_(natural_spline_m(p_.z, p_.mult)) {}

std::pair<double, double> SplineVolCurve::data_k_range() const noexcept {
  const double axis = p_.atm_vol * std::sqrt(T_);
  if (!(axis > 0.0) || !std::isfinite(axis)) {
    return {-std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
  }
  return {p_.z_lo_valid * axis, p_.z_hi_valid * axis};
}

bool SplineVolCurve::is_extrapolated(double k_log) const noexcept {
  const auto [lo, hi] = data_k_range();
  return k_log < lo || k_log > hi;
}

double SplineVolCurve::w(double k_log) const noexcept {
  if (!(T_ > 0.0) || !(F_ > 0.0) || !(df_ > 0.0) || !std::isfinite(k_log)) {
    return kNaN;
  }
  if (!(p_.atm_vol > 0.0) || p_.z.empty() || p_.z.size() != p_.mult.size()) {
    return kNaN;
  }
  const double z = k_log / (p_.atm_vol * std::sqrt(T_));
  double m = spline_eval(p_.z, p_.mult, m2nd_, z);
  if (!(m > 0.0) || !std::isfinite(m)) {
    return kNaN;
  }
  // Served-multiple ceiling: bound the spline's between-knot / data-gap
  // OVERSHOOT so no served point spikes to an economically impossible vol. This
  // keeps the calendar floor (which reads a prior slice's served w) from
  // reacting to a phantom spike and over-lifting the whole next slice off band.
  if (p_.mult_cap > 0.0 && m > p_.mult_cap) m = p_.mult_cap;
  const double sigma = p_.atm_vol * m;
  // w_offset is the calendar-cone projection's uniform total-variance lift
  // (0 unless a genuine crossing against the prior slice was cleared).
  return sigma * sigma * T_ + p_.w_offset;
}

// ── SplineVolCurve::project_calendar ────────────────────────────────────────
//
// Per-knot, ATM-preserving calendar lift onto the cone above `w_prev` — the
// SplineVol analogue of arb_project_calendar_{svi,c8}_pair (arb.cpp). See the
// method contract in vol_curve.hpp. Each pass only RAISES a knot multiple, so
// served total variance is non-decreasing across passes: the projection is a
// bounded monotone map onto the calendar cone and therefore terminates (same
// argument as the C8/SVI `v`/`a` shift).
Result<CalendarPairProjection>
SplineVolCurve::project_calendar(const std::function<double(double)> &w_prev,
                                 double k_min, double k_max,
                                 std::uint32_t n_grid, double kprev_lo,
                                 double kprev_hi) {
  // Mirror validate_pair_projection_inputs (arb.cpp), plus the SplineVol-specific
  // degeneracy guards so we never divide by a non-positive ATM vol or T.
  if (!w_prev || n_grid == 0 || !(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "spline calendar pair projection: require previous curve and valid grid");
  }
  if (!(p_.atm_vol > 0.0) || !std::isfinite(p_.atm_vol) || !(T_ > 0.0) ||
      p_.z.empty() || p_.z.size() != p_.mult.size()) {
    return Err(ErrorCode::Unavailable,
               "spline calendar pair projection: degenerate slice");
  }

  // Task F-4: this WAS a replicated literal, with a comment asking the reader
  // to keep it matching arb.cpp's file-local copy. It is now THE constant
  // (arb.hpp), which arb.cpp's own pair projections name too, so "agrees on
  // the same convergence bar" holds by construction rather than by upkeep.
  constexpr double kCalendarPairTol = kCalendarTotalVarianceTol;
  constexpr double kLiftEps = 1.0e-9;  // strict-clearance inflation (siblings' +1e-9)

  const double atm = p_.atm_vol;
  const double axis = atm * std::sqrt(T_);  // standardized moneyness z = k / axis
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);

  // Enforcement domain = the TRADEABLE OVERLAP of the two slices' data-supported
  // log-moneyness ranges. This slice's own data range is [z_lo_valid,
  // z_hi_valid] * axis; `kprev_lo/kprev_hi` is the previous slice's (passed by
  // the caller, +-inf if unknown). Calendar no-arbitrage is a statement about
  // TRADEABLE quotes: outside the overlap at least one slice is pure
  // extrapolation (a natural spline's flat wing), where a crossing is not a real
  // arb and where forcing the global spline up to clear it would drag the
  // in-sample smile off its bid-ask band. The served-surface calendar CHECK is
  // restricted to the same overlap via SplineVolCurve::is_extrapolated, so the
  // metric and the enforcement agree.
  const double dom_lo = std::max(p_.z_lo_valid * axis, kprev_lo);
  const double dom_hi = std::min(p_.z_hi_valid * axis, kprev_hi);

  // Served total variance at log-moneyness k under the current knot multiples
  // (offset applied separately). SAME arithmetic as w()'s finite branch.
  const double mcap = p_.mult_cap;
  const auto served_base = [&](double k) noexcept -> double {
    const double z = k / axis;
    double m = spline_eval(p_.z, p_.mult, m2nd_, z);
    if (!(m > 0.0) || !std::isfinite(m)) {
      return kNaN;  // matches w()'s NaN-on-undershoot; skipped as non-comparable
    }
    if (mcap > 0.0 && m > mcap) m = mcap;  // match w()'s served ceiling
    const double sigma = atm * m;
    return sigma * sigma * T_;
  };

  const std::size_t K = p_.z.size();
  double offset = 0.0;  // uniform fallback for any residual the knot lifts miss

  // Residual of w_prev over the base curve on the comparable OVERLAP grid:
  // {max_k [w_prev(k) - (base(k)+offset)]_+, any-comparable}.
  const auto overlap_residual = [&]() noexcept -> std::pair<double, bool> {
    double resid = 0.0;
    bool any = false;
    for (std::uint32_t gi = 0; gi <= n_grid; ++gi) {
      const double k = k_min + dk * static_cast<double>(gi);
      if (k < dom_lo || k > dom_hi) continue;
      const double wp = w_prev(k);
      const double base = served_base(k);
      if (!std::isfinite(wp) || !std::isfinite(base) || !(base > 0.0)) continue;
      any = true;
      resid = std::max(resid, wp - (base + offset));
    }
    return {resid, any};
  };

  CalendarPairProjection out;
  const std::pair<double, bool> before = overlap_residual();
  if (!before.second) {
    // No shared tradeable range (disjoint data): nothing to enforce; the served
    // calendar check (is_extrapolated) likewise finds no comparable point.
    return Ok(out);
  }
  out.max_deficit_before = before.first;

  // PER-KNOT, ATM-preserving lift over the overlap: the spline interpolates each
  // knot exactly at its own z, so raising mult[j] to sqrt((w_prev(k_j)-offset)/T)
  // /atm makes served w at k_j clear w_prev there. Only knots whose OWN k_j
  // crosses are lifted -- the rest of the smile (in particular the z=0 ATM knot,
  // unless it genuinely crosses) keeps its fitted value, so the near-money
  // bid-ask fit is preserved (a uniform level shift would move it off band). A
  // few passes absorb the cubic's between-knot coupling; each step only raises,
  // so the map onto the calendar cone is bounded and monotone.
  constexpr std::uint32_t kMaxPasses = 6;
  for (std::uint32_t pass = 0; pass < kMaxPasses; ++pass) {
    const std::pair<double, bool> gap = overlap_residual();
    if (!gap.second || gap.first <= kCalendarPairTol) break;
    bool changed = false;
    for (std::size_t j = 0; j < K; ++j) {
      const double k_j = p_.z[j] * axis;
      if (k_j < dom_lo || k_j > dom_hi) continue;  // wing / non-overlap knot
      const double wp_j = w_prev(k_j);
      const double base_j = served_base(k_j);
      if (!std::isfinite(wp_j) || !std::isfinite(base_j)) continue;
      if (wp_j - (base_j + offset) > kCalendarPairTol) {
        const double need_w = wp_j - offset;  // required base variance at k_j
        if (need_w > 0.0) {
          const double target = (std::sqrt(need_w / T_) / atm) * (1.0 + kLiftEps);
          if (std::isfinite(target) && target > p_.mult[j]) {
            p_.mult[j] = target;
            changed = true;
          }
        }
      }
    }
    if (!changed) break;  // knot lifts exhausted; any residual is between-knot
    m2nd_ = natural_spline_m(p_.z, p_.mult);
    ++out.passes;
  }

  // Fallback: a small uniform offset clears any residual the per-knot lifts left
  // between knots (natural-cubic undershoot). Guaranteed to finish in one shot
  // and, being only the leftover ringing gap, negligible for the band fit.
  const std::pair<double, bool> after_knots = overlap_residual();
  if (after_knots.second && after_knots.first > kCalendarPairTol) {
    offset += after_knots.first * (1.0 + kLiftEps) + kLiftEps;
    ++out.passes;
  }
  p_.w_offset = offset;
  return Ok(out);
}

// ── fit_spline_vol_slice ─────────────────────────────────────────────────────

Result<std::unique_ptr<IVolCurve>> fit_spline_vol_slice(std::span<const FitObs> obs_eu, double F,
                                                        double T, double df,
                                                        const SplineFitOpts &opts) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "fit_spline_vol_slice: F/T/df must be positive");
  }
  if (opts.grid.size() < 4) {
    return Err(ErrorCode::InvalidArgument, "fit_spline_vol_slice: grid must have >= 4 knots");
  }
  if (obs_eu.size() < opts.min_obs) {
    return Err(ErrorCode::InvalidArgument, "fit_spline_vol_slice: fewer than min_obs observations");
  }

  std::vector<double> k_v, iv_v, wt_v;
  k_v.reserve(obs_eu.size());
  iv_v.reserve(obs_eu.size());
  wt_v.reserve(obs_eu.size());
  for (const FitObs &o : obs_eu) {
    if (!std::isfinite(o.k) || !(o.sigma_mkt > 0.0) || !std::isfinite(o.sigma_mkt) ||
        !(o.weight_w > 0.0) || !std::isfinite(o.weight_w)) {
      continue;
    }
    k_v.push_back(o.k);
    iv_v.push_back(o.sigma_mkt);
    wt_v.push_back(o.weight_w);
  }
  if (k_v.size() < opts.min_obs) {
    return Err(ErrorCode::InvalidArgument,
               "fit_spline_vol_slice: fewer than min_obs valid observations");
  }

  const double sqrtT = std::sqrt(T);
  const std::size_t n = k_v.size();

  // Step 1 (part a): global vega-weighted mean IV -- the ATM-band width guess.
  double wsum = 0.0, vsum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    wsum += wt_v[i];
    vsum += wt_v[i] * iv_v[i];
  }
  if (!(wsum > 0.0)) {
    return Err(ErrorCode::Unavailable, "fit_spline_vol_slice: zero total observation weight");
  }
  const double sigma_guess = vsum / wsum;
  if (!(sigma_guess > 0.0) || !std::isfinite(sigma_guess)) {
    return Err(ErrorCode::Unavailable, "fit_spline_vol_slice: degenerate global IV seed");
  }

  // Step 1 (part b): vega-weighted mean IV within the half-ATM-vol band,
  // falling back to the global mean when the band is empty.
  const double band = 0.5 * sigma_guess * sqrtT;
  double bw_sum = 0.0, bv_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(k_v[i]) <= band) {
      bw_sum += wt_v[i];
      bv_sum += wt_v[i] * iv_v[i];
    }
  }
  const double atm_seed = (bw_sum > 0.0) ? (bv_sum / bw_sum) : sigma_guess;
  if (!(atm_seed > 0.0) || !std::isfinite(atm_seed)) {
    return Err(ErrorCode::Unavailable, "fit_spline_vol_slice: degenerate ATM seed");
  }

  // One full standardize -> active-knots -> cardinal-basis -> penalized-WLS
  // pass at a given ATM seed. Shared by the seed pass and the one refinement
  // pass (step 1's "two-pass total, deterministic" re-standardization).
  const auto run_pass = [&](double atm) -> Result<SplineVolParams> {
    if (!(atm > 0.0) || !std::isfinite(atm)) {
      return Err(ErrorCode::Unavailable, "fit_spline_vol_slice: non-positive ATM vol in pass");
    }
    std::vector<double> z(n), y(n);
    double z_lo = std::numeric_limits<double>::infinity();
    double z_hi = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
      z[i] = k_v[i] / (atm * sqrtT);
      y[i] = iv_v[i] / atm;
      z_lo = std::min(z_lo, z[i]);
      z_hi = std::max(z_hi, z[i]);
    }

    const std::vector<std::size_t> active = select_active_knots(opts.grid, z_lo, z_hi);
    const std::size_t K = active.size();
    std::vector<double> zk(K);
    for (std::size_t j = 0; j < K; ++j) {
      zk[j] = opts.grid[active[j]];
    }

    // Cardinal basis: B[i][j] = basis_j(z_i), one tridiagonal solve per knot.
    MatX B(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(K));
    std::vector<double> unit(K, 0.0);
    for (std::size_t j = 0; j < K; ++j) {
      std::fill(unit.begin(), unit.end(), 0.0);
      unit[j] = 1.0;
      const std::vector<double> M = natural_spline_m(zk, unit);
      for (std::size_t i = 0; i < n; ++i) {
        B(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
            spline_eval(zk, unit, M, z[i]);
      }
    }

    VecX wv(static_cast<Eigen::Index>(n));
    VecX yv(static_cast<Eigen::Index>(n));
    for (std::size_t i = 0; i < n; ++i) {
      wv(static_cast<Eigen::Index>(i)) = wt_v[i];
      yv(static_cast<Eigen::Index>(i)) = y[i];
    }
    MatX AtA = B.transpose() * wv.asDiagonal() * B;
    VecX Aty = B.transpose() * (wv.asDiagonal() * yv);

    // Second-difference roughness penalty lambda * D^T D.
    if (K >= 3 && opts.lambda > 0.0) {
      MatX D = MatX::Zero(static_cast<Eigen::Index>(K - 2), static_cast<Eigen::Index>(K));
      for (std::size_t r = 0; r + 2 < K; ++r) {
        D(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(r)) = 1.0;
        D(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(r + 1)) = -2.0;
        D(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(r + 2)) = 1.0;
      }
      AtA += opts.lambda * (D.transpose() * D);
    }

    ATX_TRY(VecX m, solve_spd(AtA, Aty));

    std::vector<double> mult(K);
    for (std::size_t j = 0; j < K; ++j) {
      mult[j] = std::max(m(static_cast<Eigen::Index>(j)), opts.mult_floor);
    }

    SplineVolParams p;
    p.atm_vol = atm;
    p.z_lo_valid = z_lo;
    p.z_hi_valid = z_hi;
    p.z = std::move(zk);
    p.mult = std::move(mult);
    p.mult_cap = opts.mult_ceil;  // served-multiple overshoot ceiling (0 = off)

    const std::vector<double> m2nd_final = natural_spline_m(p.z, p.mult);
    p.n_butterfly_viol = count_butterfly_violations(p.z, p.mult, m2nd_final, p.atm_vol, T,
                                                     p.z_lo_valid, p.z_hi_valid);
    return Ok(std::move(p));
  };

  ATX_TRY(SplineVolParams p1, run_pass(atm_seed));

  // Step 1 refinement: re-seed sigma_ATM at the fitted spline's value at
  // z = 0 (z = 0 always corresponds to k = 0, independent of the ATM seed
  // used to standardize), then re-standardize + refit once more.
  const std::vector<double> m2nd_p1 = natural_spline_m(p1.z, p1.mult);
  const double m_at_zero = spline_eval(p1.z, p1.mult, m2nd_p1, 0.0);
  double atm_refined = p1.atm_vol * m_at_zero;
  if (!(atm_refined > 0.0) || !std::isfinite(atm_refined)) {
    atm_refined = p1.atm_vol;  // fallback: keep the pass-1 seed
  }

  ATX_TRY(SplineVolParams p2, run_pass(atm_refined));

  std::unique_ptr<IVolCurve> curve = std::make_unique<SplineVolCurve>(std::move(p2), T, F, df);
  return Ok(std::move(curve));
}

}  // namespace atx::vol
