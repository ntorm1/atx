#include "fitting/essvi_anchored.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <utility>

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// Smallest psi the sweep will consider. A slice with psi below this is a flat
// curve with no skew or curvature, which no market produces; keeping the bound
// strictly positive also keeps phi = psi/theta finite.
constexpr double kPsiFloor = 1.0e-10;

// Relative / absolute slack for the invariant assertions. The optimiser is
// allowed to land ON an interval endpoint, so an assertion evaluated in a
// different association order than the bound that produced it must tolerate one
// ulp of drift; anything larger is a real violation.
constexpr double kArbRelTol = 1.0e-9;
constexpr double kArbAbsTol = 1.0e-12;

// Golden ratio constant of Brent's method, (3 - sqrt(5)) / 2.
constexpr double kBrentGold = 0.3819660112501051;

[[nodiscard]] double w_model(double theta, double psi, double rho,
                             double k) noexcept {
  const double a = psi * k + rho * theta;
  const double inner = a * a + theta * theta * (1.0 - rho * rho);
  return 0.5 * (theta + rho * psi * k + std::sqrt(inner));
}

// theta as a closed form of (rho, psi) through the anchor — Corbetta et al.'s
// relation (A); see the header.
[[nodiscard]] constexpr double anchored_theta(double rho, double psi,
                                              double k_star,
                                              double theta_star) noexcept {
  return theta_star - rho * psi * k_star;
}

struct BrentOutcome {
  double x{};
  double f{};
  std::uint32_t n_eval{};
};

// Brent (1973) parabolic-interpolation / golden-section minimisation on the
// closed interval [a, b]. Bounded by `max_iter` — the loop cannot run long, and
// the evaluation count is therefore a fixed budget rather than a data-dependent
// one. Deterministic: no random restarts, no adaptive termination on anything
// but the bracket width.
template <typename Fn>
[[nodiscard]] BrentOutcome brent_minimize(Fn&& fn, double a, double b,
                                          double tol, std::uint16_t max_iter) {
  BrentOutcome out{};
  if (!(b > a)) {
    out.x = a;
    out.f = fn(a);
    out.n_eval = 1u;
    return out;
  }
  double x = a + kBrentGold * (b - a);
  double w = x;
  double v = x;
  double fx = fn(x);
  double fw = fx;
  double fv = fx;
  std::uint32_t n_eval = 1u;
  double d = 0.0;
  double e = 0.0;

  for (std::uint16_t iter = 0; iter < max_iter; ++iter) {
    const double xm = 0.5 * (a + b);
    const double tol1 = tol * std::fabs(x) + 1.0e-300;
    const double tol2 = 2.0 * tol1;
    if (std::fabs(x - xm) <= tol2 - 0.5 * (b - a)) {
      break;
    }
    bool golden = true;
    if (std::fabs(e) > tol1) {
      const double r = (x - w) * (fx - fv);
      double q = (x - v) * (fx - fw);
      double p = (x - v) * q - (x - w) * r;
      q = 2.0 * (q - r);
      if (q > 0.0) {
        p = -p;
      }
      q = std::fabs(q);
      const double e_prev = e;
      e = d;
      if (std::fabs(p) < std::fabs(0.5 * q * e_prev) && p > q * (a - x) &&
          p < q * (b - x)) {
        d = p / q;
        const double u_try = x + d;
        if (u_try - a < tol2 || b - u_try < tol2) {
          d = (xm >= x) ? tol1 : -tol1;
        }
        golden = false;
      }
    }
    if (golden) {
      e = (x >= xm) ? (a - x) : (b - x);
      d = kBrentGold * e;
    }
    const double u =
        (std::fabs(d) >= tol1) ? (x + d) : (x + ((d >= 0.0) ? tol1 : -tol1));
    const double fu = fn(u);
    ++n_eval;
    if (fu <= fx) {
      if (u >= x) {
        a = x;
      } else {
        b = x;
      }
      v = w;
      w = x;
      x = u;
      fv = fw;
      fw = fx;
      fx = fu;
    } else {
      if (u < x) {
        a = u;
      } else {
        b = u;
      }
      // SAFETY: the `w`/`v` comparisons are exact copies of the same double
      // (the incumbent point), so equality here is identity, not a tolerance
      // question. This is Brent's published bookkeeping unchanged.
      if (fu <= fw || w == x) {
        v = w;
        w = u;
        fv = fw;
        fw = fu;
      } else if (fu <= fv || v == x || v == w) {
        v = u;
        fv = fu;
      }
    }
  }
  out.x = x;
  out.f = fx;
  out.n_eval = n_eval;
  return out;
}

struct Anchor {
  double k_star{};
  double theta_star{};
  bool ok{false};
};

// Corbetta et al. anchor on the single market point NEAREST the ATM forward, so
// the O(k*^2) truncation in relation (A) is as small as the chain allows.
//
// This returns the nearest `max_n` instead, in ascending |k|, because that one
// point sets the slice's ENTIRE level: theta = theta* - rho*psi*k*, so whatever
// bid/ask noise sits on the nearest quote's mid is transferred undamped into
// w(0), while the incumbent LM path averages the level over the whole chain and
// suppresses that noise by roughly sqrt(n). MEASURED (2026-08-16, 611-symbol
// session): with a single anchor the anchored path lost 8 cells to the risk
// surface's in-band quality floor -- not to arbitrage, which was clean -- and
// the synthetic 2%-noise comparison put its total-variance RMSE 0-13% above the
// LM path's.
//
// Trying several anchors and keeping the one with the lowest TOTAL objective is
// a DISCRETE model selection over observed data, exactly like the rho grid. It
// costs nothing structurally: the count of FREE parameters is still two, the
// interval algebra is unchanged (it consumes a scalar (k*, theta*) whatever its
// provenance), and there is still no starting point to choose or tune. It is
// also not the "interpolate an ATM vol from bracketing strikes" step the
// anchoring trick exists to avoid -- no value between quotes is ever
// manufactured; each candidate is an observed quote.
//
// Ties resolve to the lower index, so the ordering is deterministic.
[[nodiscard]] std::vector<Anchor> pick_anchors(std::span<const FitObs> obs,
                                               std::size_t max_n) {
  std::vector<Anchor> out;
  if (max_n == 0u) {
    return out;
  }
  out.reserve(max_n);
  // Selection sort over the candidate set: max_n is a small fixed budget, so
  // this is cheaper than sorting the whole observation set and needs no scratch.
  std::vector<bool> taken(obs.size(), false);
  for (std::size_t slot = 0; slot < max_n; ++slot) {
    double best_abs_k = std::numeric_limits<double>::infinity();
    std::size_t best = obs.size();
    for (std::size_t i = 0; i < obs.size(); ++i) {
      if (taken[i] || !(obs[i].w_mkt > 0.0) || !std::isfinite(obs[i].k)) {
        continue;
      }
      const double abs_k = std::fabs(obs[i].k);
      if (abs_k < best_abs_k) {
        best_abs_k = abs_k;
        best = i;
      }
    }
    if (best == obs.size()) {
      break;  // no usable observation left
    }
    taken[best] = true;
    out.push_back(Anchor{obs[best].k, obs[best].w_mkt, true});
  }
  return out;
}

}  // namespace

// ── Evaluation ───────────────────────────────────────────────────────────

double anchored_w(const AnchoredSlice& s, double k) noexcept {
  return w_model(s.theta, s.psi, s.rho, k);
}

EssviParams anchored_to_essvi(const AnchoredSlice& s) noexcept {
  EssviParams p{};
  p.theta = s.theta;
  p.phi = (s.theta > 0.0) ? (s.psi / s.theta) : 0.0;
  p.rho = s.rho;
  p.T = s.T;
  p.F = s.F;
  // The Mingone cube coordinates are diagnostics on this path (the serving
  // evaluator reads theta/phi/rho), but downstream warm-seed reuse inspects
  // them for finiteness, so fill them consistently.
  const EssviCube cube = essvi_natural_to_reparam(p.theta, p.phi, p.rho, p.T);
  p.psi = cube.psi;
  p.p = cube.p;
  p.lambda = cube.lambda;
  return p;
}

// ── Feasible interval ────────────────────────────────────────────────────

PsiInterval anchored_psi_bounds(double rho, double k_star, double theta_star,
                                const AnchoredSlice* prev) noexcept {
  PsiInterval iv{};
  iv.lo = kPsiFloor;
  iv.hi = -1.0;  // empty until the butterfly bound is established
  if (!(theta_star > 0.0) || !(std::fabs(rho) < 1.0) || !std::isfinite(k_star)) {
    return iv;
  }
  const double one_plus = 1.0 + std::fabs(rho);

  // Butterfly #1: psi * (1 + |rho|) <= 4.
  double hi = 4.0 / one_plus;

  // Butterfly #2: psi^2 * (1 + |rho|) <= 4 * theta, with theta from relation
  // (A). The positive root of the resulting quadratic is Corbetta et al.'s
  // psi_+; it also subsumes theta > 0, which needs no separate constraint.
  const double lin = rho * k_star / one_plus;
  const double psi_plus =
      -2.0 * lin + std::sqrt(4.0 * lin * lin + 4.0 * theta_star / one_plus);
  hi = std::min(hi, psi_plus);

  if (prev != nullptr) {
    // Calendar #2 and #3, at fixed rho: |rho*psi - chi_1| <= psi - psi_1 with
    // psi >= psi_1 splits into two lower bounds, both of which dominate psi_1
    // itself.
    const double p1 = prev->psi;
    const double r1 = prev->rho;
    if (p1 > 0.0 && std::fabs(r1) < 1.0) {
      const double lo_a = p1 * (1.0 - r1) / (1.0 - rho);
      const double lo_b = p1 * (1.0 + r1) / (1.0 + rho);
      iv.lo = std::max(iv.lo, std::max(p1, std::max(lo_a, lo_b)));
    }
    // Calendar #1: theta >= theta_1, i.e. rho*k* * psi <= theta* - theta_1.
    const double c = rho * k_star;
    const double slack = theta_star - prev->theta;
    if (c > 0.0) {
      hi = std::min(hi, slack / c);
    } else if (c < 0.0) {
      iv.lo = std::max(iv.lo, slack / c);
    } else if (slack < 0.0) {
      return iv;  // theta is pinned at theta* and already inverts: infeasible
    }
  }

  iv.hi = hi;
  return iv;
}

// ── Assertions ───────────────────────────────────────────────────────────

bool anchored_butterfly_ok(const AnchoredSlice& s) noexcept {
  if (!(s.theta > 0.0) || !(s.psi > 0.0) || !(std::fabs(s.rho) < 1.0)) {
    return false;
  }
  const double one_plus = 1.0 + std::fabs(s.rho);
  const double c1 = s.psi * one_plus;
  if (!(c1 <= 4.0 * (1.0 + kArbRelTol) + kArbAbsTol)) {
    return false;
  }
  const double c2 = s.psi * s.psi * one_plus;
  const double bound2 = 4.0 * s.theta;
  return c2 <= bound2 * (1.0 + kArbRelTol) + kArbAbsTol;
}

bool anchored_calendar_ok(const AnchoredSlice& prev,
                          const AnchoredSlice& next) noexcept {
  const double theta_tol =
      kArbRelTol * std::max(std::fabs(prev.theta), std::fabs(next.theta)) +
      kArbAbsTol;
  if (!(next.theta >= prev.theta - theta_tol)) {
    return false;
  }
  const double psi_tol =
      kArbRelTol * std::max(std::fabs(prev.psi), std::fabs(next.psi)) +
      kArbAbsTol;
  const double d_psi = next.psi - prev.psi;
  if (!(d_psi >= -psi_tol)) {
    return false;
  }
  const double d_chi = next.chi() - prev.chi();
  return std::fabs(d_chi) <= d_psi + psi_tol;
}

// ── Calibration ──────────────────────────────────────────────────────────

Result<AnchoredSlice> anchored_fit_slice(std::span<const FitObs> obs, double T,
                                         double F, const AnchoredOpts& opts,
                                         const AnchoredSlice* prev,
                                         AnchoredDiag* out_diag) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_fit_slice: empty observations or non-positive T");
  }
  const std::size_t n_anchor =
      (opts.n_anchor_candidates >= 1u)
          ? static_cast<std::size_t>(opts.n_anchor_candidates)
          : std::size_t{1u};
  const std::vector<Anchor> anchors = pick_anchors(obs, n_anchor);
  if (anchors.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_fit_slice: no observation carries a usable anchor "
               "(positive total variance at a finite log-moneyness)");
  }
  const double rho_max =
      (opts.rho_max > 0.0 && opts.rho_max < 1.0) ? opts.rho_max : 0.999;
  const std::uint16_t n_rho = (opts.n_rho >= 2u) ? opts.n_rho : 2u;
  const std::uint16_t brent_iter =
      (opts.brent_max_iter >= 1u) ? opts.brent_max_iter : 1u;
  const double brent_tol = (opts.brent_tol > 0.0) ? opts.brent_tol : 1.0e-9;

  std::uint32_t n_eval = 0u;
  std::uint32_t n_feasible = 0u;

  // Weighted total-variance SSE — the same objective domain the LM path
  // minimises, so residuals from the two calibrators are directly comparable.
  const auto objective = [&](const Anchor& a, double rho, double psi) noexcept {
    const double theta = anchored_theta(rho, psi, a.k_star, a.theta_star);
    if (!(theta > 0.0)) {
      return std::numeric_limits<double>::max();
    }
    double sse = 0.0;
    for (const FitObs& o : obs) {
      const double wt = (o.weight_w > 0.0) ? o.weight_w : 1.0;
      const double r = w_model(theta, psi, rho, o.k) - o.w_mkt;
      sse += wt * r * r;
    }
    return sse;
  };

  // Incumbent for the anchor currently being swept.
  double cur_sse = std::numeric_limits<double>::infinity();
  double cur_rho = 0.0;
  double cur_psi = 0.0;
  bool cur_have = false;
  // Best across all anchors, each already fully refined.
  double best_sse = std::numeric_limits<double>::infinity();
  double best_rho = 0.0;
  double best_psi = 0.0;
  std::size_t best_anchor = 0u;
  bool have_best = false;

  // One deterministic sweep: sample rho, solve the constrained 1-D problem in
  // psi exactly, keep the incumbent. No starting point enters anywhere.
  const auto sweep = [&](const Anchor& a, double rho_lo, double rho_hi) {
    for (std::uint16_t j = 0; j < n_rho; ++j) {
      const double u = (n_rho == 1u)
                           ? 0.5
                           : (static_cast<double>(j) /
                              static_cast<double>(n_rho - 1u));
      double rho = rho_lo + (rho_hi - rho_lo) * u;
      rho = std::clamp(rho, -rho_max, rho_max);
      const PsiInterval iv =
          anchored_psi_bounds(rho, a.k_star, a.theta_star, prev);
      if (iv.empty()) {
        continue;
      }
      ++n_feasible;
      const auto fn = [&](double psi) noexcept { return objective(a, rho, psi); };
      const BrentOutcome br = brent_minimize(fn, iv.lo, iv.hi, brent_tol, brent_iter);
      n_eval += br.n_eval;
      // The constrained minimum frequently sits ON an endpoint (that is what a
      // binding calendar bound means), where an interior-point method converges
      // slowly; evaluating both endpoints makes the bound exact for two evals.
      double cand_psi = br.x;
      double cand_sse = br.f;
      const double f_lo = objective(a, rho, iv.lo);
      const double f_hi = objective(a, rho, iv.hi);
      n_eval += 2u;
      if (f_lo < cand_sse) {
        cand_sse = f_lo;
        cand_psi = iv.lo;
      }
      if (f_hi < cand_sse) {
        cand_sse = f_hi;
        cand_psi = iv.hi;
      }
      if (!cur_have || cand_sse < cur_sse) {
        cur_sse = cand_sse;
        cur_psi = cand_psi;
        cur_rho = rho;
        cur_have = true;
      }
    }
  };

  // Each anchor candidate gets its OWN coarse sweep plus full refinement, and
  // only the refined results are compared. Comparing coarse results instead
  // would be unsound: the coarse rho resolution is ~1e-1, refinement moves the
  // objective by more than the gap between anchors, and the anchor that happens
  // to win at coarse resolution is not the one that wins after refining
  // (measured: it cost up to 8% of in-sample RMSE on the noisy fixture).
  for (std::size_t ai = 0; ai < anchors.size(); ++ai) {
    cur_have = false;
    cur_sse = std::numeric_limits<double>::infinity();
    sweep(anchors[ai], -rho_max, rho_max);
    if (!cur_have) {
      continue; // this anchor admits no feasible psi for any sampled rho
    }
    // Refine on the bracket around this anchor's incumbent. Each pass narrows
    // the rho resolution by ~n_rho.
    double half = (2.0 * rho_max) / static_cast<double>(n_rho - 1u);
    for (std::uint16_t pass = 0; pass < opts.n_refine_passes; ++pass) {
      const double lo = std::max(-rho_max, cur_rho - half);
      const double hi = std::min(rho_max, cur_rho + half);
      sweep(anchors[ai], lo, hi);
      half = (hi - lo) / static_cast<double>(n_rho - 1u);
    }
    if (!have_best || cur_sse < best_sse) {
      best_sse = cur_sse;
      best_psi = cur_psi;
      best_rho = cur_rho;
      best_anchor = ai;
      have_best = true;
    }
  }
  if (!have_best) {
    return Err(ErrorCode::Unavailable,
               "anchored_fit_slice: every sampled rho has an empty admissible "
               "psi interval (the no-arbitrage constraints and the anchor are "
               "jointly infeasible)");
  }

  const Anchor& anchor = anchors[best_anchor];
  AnchoredSlice out{};
  out.rho = best_rho;
  out.psi = best_psi;
  out.theta = anchored_theta(best_rho, best_psi, anchor.k_star, anchor.theta_star);
  out.k_star = anchor.k_star;
  out.theta_star = anchor.theta_star;
  out.T = T;
  out.F = F;
  if (!(out.theta > 0.0) || !std::isfinite(out.theta) ||
      !std::isfinite(out.psi) || !std::isfinite(out.rho)) {
    return Err(ErrorCode::Unavailable,
               "anchored_fit_slice: sweep produced a degenerate slice");
  }

  if (out_diag != nullptr) {
    out_diag->n_obs = static_cast<std::uint32_t>(obs.size());
    out_diag->n_objective_evals = n_eval;
    out_diag->n_rho_feasible = n_feasible;
    out_diag->sse = best_sse;
    double acc = 0.0;
    for (const FitObs& o : obs) {
      const double r = anchored_w(out, o.k) - o.w_mkt;
      acc += r * r;
    }
    out_diag->rmse_w = std::sqrt(acc / static_cast<double>(obs.size()));
    out_diag->k_star = anchor.k_star;
    out_diag->theta_star = anchor.theta_star;
  }
  return Ok(std::move(out));
}

// ── Interpolation ────────────────────────────────────────────────────────

Result<AnchoredSlice> anchored_interpolate(const AnchoredSlice& lo,
                                           const AnchoredSlice& hi, double T) {
  if (!(hi.T > lo.T) || !(lo.T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_interpolate: degenerate maturity bracket");
  }
  if (!(T >= lo.T) || !(T <= hi.T)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_interpolate: T outside the bracket — this API "
               "interpolates only and never extrapolates");
  }
  // The proof in the header assumes both endpoints are admissible and mutually
  // calendar-consistent. Verify rather than assume: applied outside its
  // hypotheses the interpolation is not arbitrage-free.
  if (!anchored_butterfly_ok(lo) || !anchored_butterfly_ok(hi)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_interpolate: an endpoint slice is not butterfly-free");
  }
  if (!anchored_calendar_ok(lo, hi)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_interpolate: the endpoint slices calendar-cross");
  }

  const double lambda = (T - lo.T) / (hi.T - lo.T);
  const double theta = lo.theta + (hi.theta - lo.theta) * lambda;
  const double psi = lo.psi + (hi.psi - lo.psi) * lambda;
  const double chi = lo.chi() + (hi.chi() - lo.chi()) * lambda;
  if (!(psi > 0.0) || !(theta > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "anchored_interpolate: degenerate interpolated slice");
  }

  AnchoredSlice out{};
  out.theta = theta;
  out.psi = psi;
  out.rho = std::clamp(chi / psi, -1.0, 1.0);
  out.k_star = 0.0;  // no anchor observation: the slice was never fit
  out.theta_star = theta;
  out.T = T;
  out.F = lo.F + (hi.F - lo.F) * lambda;
  return Ok(std::move(out));
}

// ── Sequential driver ────────────────────────────────────────────────────

Result<std::vector<AnchoredSliceResult>> anchored_fit_sequence(
    std::span<const AnchoredSliceRequest> reqs, const AnchoredOpts& opts) {
  std::vector<AnchoredSliceResult> out(reqs.size());
  double prev_T = 0.0;
  for (const AnchoredSliceRequest& r : reqs) {
    if (!(r.T > 0.0) || !(r.T > prev_T)) {
      return Err(ErrorCode::InvalidArgument,
                 "anchored_fit_sequence: maturities must be positive and "
                 "strictly ascending");
    }
    prev_T = r.T;
  }

  // Pass 1 — calibrate, each slice constrained by the previous CALIBRATED one.
  AnchoredSlice prev{};
  bool has_prev = false;
  for (std::size_t i = 0; i < reqs.size(); ++i) {
    const AnchoredSliceRequest& r = reqs[i];
    if (!r.fit_independently || r.obs.empty()) {
      continue;
    }
    AnchoredDiag diag{};
    Result<AnchoredSlice> fit = anchored_fit_slice(
        r.obs, r.T, r.F, opts, has_prev ? &prev : nullptr, &diag);
    if (!fit.has_value()) {
      // A slice that will not calibrate is not lost here: pass 2 still offers
      // it interpolation if its neighbours bracket it.
      continue;
    }
    out[i].slice = *fit;
    out[i].origin = AnchoredSliceOrigin::Calibrated;
    out[i].diag = diag;
    prev = *fit;
    has_prev = true;
  }

  // Pass 2 — interpolate everything strictly bracketed by two calibrated
  // maturities. Anything outside the calibrated domain stays dropped: an
  // extrapolated tenor is exactly what this must not manufacture.
  for (std::size_t i = 0; i < reqs.size(); ++i) {
    if (out[i].origin != AnchoredSliceOrigin::Dropped) {
      continue;
    }
    std::size_t before = reqs.size();
    for (std::size_t j = i; j-- > 0;) {
      if (out[j].origin == AnchoredSliceOrigin::Calibrated) {
        before = j;
        break;
      }
    }
    std::size_t after = reqs.size();
    for (std::size_t j = i + 1; j < reqs.size(); ++j) {
      if (out[j].origin == AnchoredSliceOrigin::Calibrated) {
        after = j;
        break;
      }
    }
    if (before == reqs.size() || after == reqs.size()) {
      continue;  // not bracketed — no slice, by design
    }
    Result<AnchoredSlice> mid =
        anchored_interpolate(out[before].slice, out[after].slice, reqs[i].T);
    if (!mid.has_value()) {
      continue;
    }
    mid->F = reqs[i].F;
    out[i].slice = *mid;
    out[i].origin = AnchoredSliceOrigin::Interpolated;
    out[i].diag.n_obs = static_cast<std::uint32_t>(reqs[i].obs.size());
    out[i].diag.k_star = 0.0;
    out[i].diag.theta_star = mid->theta;
    double acc = 0.0;
    for (const FitObs& o : reqs[i].obs) {
      const double res = anchored_w(*mid, o.k) - o.w_mkt;
      acc += res * res;
    }
    if (!reqs[i].obs.empty()) {
      out[i].diag.rmse_w =
          std::sqrt(acc / static_cast<double>(reqs[i].obs.size()));
    }
  }
  return Ok(std::move(out));
}

// ── Qualification activation seam ────────────────────────────────────────

namespace {

[[nodiscard]] bool env_truthy(const char* name) noexcept {
#if defined(_MSC_VER)
  std::size_t len = 0;
  char buf[16] = {};
  if (::getenv_s(&len, buf, sizeof buf, name) != 0 || len == 0u) {
    return false;
  }
  const char c = buf[0];
#else
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == ' ') {
    return false;
  }
  const char c = v[0];
#endif
  return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' || c == 'o' ||
         c == 'O';
}

struct AnchoredEnvPolicy {
  bool anchored{false};
  bool interpolate_thin{false};
};

[[nodiscard]] const AnchoredEnvPolicy& anchored_env_policy() noexcept {
  // Read once per process: function-local static initialisation is
  // thread-safe and happens exactly once (C++11 [stmt.dcl]/4).
  static const AnchoredEnvPolicy policy = [] {
    AnchoredEnvPolicy p{};
    p.anchored = env_truthy("ATX_VOL_ESSVI_ANCHORED");
    p.interpolate_thin = env_truthy("ATX_VOL_ESSVI_ANCHORED_INTERP");
    return p;
  }();
  return policy;
}

}  // namespace

void apply_anchored_env_policy(CalibOpts& calib) noexcept {
  const AnchoredEnvPolicy& p = anchored_env_policy();
  calib.essvi_anchored = calib.essvi_anchored || p.anchored;
  calib.essvi_anchored_interpolate_thin =
      calib.essvi_anchored_interpolate_thin || p.interpolate_thin;
}

}  // namespace atx::vol
