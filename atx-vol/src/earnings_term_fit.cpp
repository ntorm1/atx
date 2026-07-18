#include "atx/vol/earnings_term_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/event_vol.hpp" // censored_total_variance, kWCenFloor

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

double censored_atm_vol(const CensorObsInput &o, double emove, double wcen_floor) noexcept {
  // censored_total_variance already floors at the fixed kWCenFloor; flooring
  // again against the caller-supplied wcen_floor lets a caller impose a
  // stricter bound without touching event_vol.hpp's own constant (see the
  // header's self-review notes).
  const double w_cen = censored_total_variance(o.w_dirty, o.n, emove);
  const double floored = std::max(w_cen, wcen_floor);
  return std::sqrt(floored / o.T);
}

namespace {

// Decay-search bounds/grid density and golden-section iteration count for
// `fit_term_curve_for_emove`'s inner 1-D `decay` search. Not exposed via
// `EarningsFitConfig`: the brief fixes these as implementation constants
// (a later joint-fit task can promote them to config knobs if a real
// underlying ever needs a wider/narrower decay prior).
constexpr double kDecayLo = 0.1;
constexpr double kDecayHi = 30.0;
constexpr int kDecayGridN = 40; // log-spaced coarse grid points
// Golden-section constant (sqrt(5) - 1) / 2, same constant/pattern as
// s3.cpp's seed search. 80 iterations shrinks the (already-narrow, one
// coarse-grid-step-wide) refine bracket by 0.618^80 -- many orders of
// magnitude below double precision -- a statically-bounded loop (JPL Rule 2).
constexpr double kGolden = 0.6180339887498949;
constexpr int kGoldenIters = 80;

// One candidate decay's linear-LSQ solve: `{lt, a}` (a = st - lt) minimizing
// sum((y_i - lt - a*exp(-decay*T_i))^2), plus the resulting unweighted RMS
// residual.
struct DecayFit {
  double lt{};
  double a{};
  double rms{};
};

// Solves the 2x2 normal equations for `y ~= lt*1 + a*b(T;decay)` by
// unweighted least squares (EarningsFitConfig's LSQ weighting is "uniform in
// v1" -- see the header), then reports the RMS residual at that solution.
//
// Guards the solve against a singular/ill-conditioned Gram matrix -- e.g.
// every `obs[i].T` identical (every b_i is then the SAME constant, so the
// `{1,b}` basis collapses onto one column), or `decay` large enough that
// every b_i underflows to ~0 (same collapse) -- by comparing `|det|` against
// the Gram matrix's own diagonal scale (`n*sum_bb`) rather than a fixed
// absolute epsilon: an absolute threshold would be wrong for both a 2-point
// and a 200-point fit, and wrong again if the censored vols are quoted in
// vol-points vs. decimal. On a guard hit this falls back to the flat mean
// fit (`a=0`, `lt` = mean of `y`) rather than dividing by a ~0 determinant --
// by Cauchy-Schwarz the 2x2 Gram determinant `n*sum_bb - sum_b^2` is always
// >= 0, so a near-zero determinant only ever signals "no information
// distinguishes st from lt at this decay", never a sign-indeterminate solve.
// This same guard transparently covers `obs.empty()` (n=0 makes every sum,
// and hence `det`, exactly 0), so the caller needs no separate empty-span
// branch.
[[nodiscard]] DecayFit fit_decay(std::span<const CensorObsInput> obs, std::span<const double> y,
                                 double decay) noexcept {
  const std::size_t n = obs.size();
  const double n_d = static_cast<double>(n);
  double sum_b = 0.0, sum_bb = 0.0, sum_y = 0.0, sum_yb = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double b = std::exp(-decay * obs[i].T);
    sum_b += b;
    sum_bb += b * b;
    sum_y += y[i];
    sum_yb += y[i] * b;
  }

  const double det = n_d * sum_bb - sum_b * sum_b;
  const double scale = std::max(n_d * sum_bb, 1.0);
  const bool singular = !(std::abs(det) > 1.0e-12 * scale);

  double lt = 0.0;
  double a = 0.0;
  if (n > 0) {
    if (!singular) {
      lt = (sum_y * sum_bb - sum_b * sum_yb) / det;
      a = (n_d * sum_yb - sum_b * sum_y) / det;
    } else {
      lt = sum_y / static_cast<double>(n); // flat mean fallback (a stays 0)
    }
  }
  // n == 0: lt = a = 0.0, the "sane flat curve" default already set above.

  double sse = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double b = std::exp(-decay * obs[i].T);
    const double resid = y[i] - (lt + a * b);
    sse += resid * resid;
  }
  const double rms = (n > 0) ? std::sqrt(sse / static_cast<double>(n)) : 0.0;
  return DecayFit{lt, a, rms};
}

// A candidate decay value paired with its 2x2-solved fit -- the golden-
// section refine's return shape (it must report both, not just the fit,
// since the caller compares this against the coarse grid's own best decay).
struct DecayCandidate {
  double decay{};
  DecayFit fit{};
};

// Golden-section-minimizes `fit_decay(...).rms` over `[lo, hi]` -- statically
// bounded at `kGoldenIters` iterations (same pattern as s3.cpp's seed
// search): 0.618^80 shrinks the bracket many orders of magnitude below
// double precision, so `kGoldenIters` is never the binding constraint on
// accuracy. `lo == hi` (degenerate bracket; only possible if `kDecayGridN`
// were ever shrunk to 1) short-circuits to that single point rather than
// running a zero-width search.
[[nodiscard]] DecayCandidate golden_section_refine(std::span<const CensorObsInput> obs,
                                                    std::span<const double> y, double lo,
                                                    double hi) noexcept {
  if (!(hi > lo)) {
    return DecayCandidate{lo, fit_decay(obs, y, lo)};
  }
  double c = hi - kGolden * (hi - lo);
  double d = lo + kGolden * (hi - lo);
  DecayFit fc = fit_decay(obs, y, c);
  DecayFit fd = fit_decay(obs, y, d);
  for (int it = 0; it < kGoldenIters; ++it) {
    if (fc.rms < fd.rms) {
      hi = d;
      d = c;
      fd = fc;
      c = hi - kGolden * (hi - lo);
      fc = fit_decay(obs, y, c);
    } else {
      lo = c;
      c = d;
      fc = fd;
      d = lo + kGolden * (hi - lo);
      fd = fit_decay(obs, y, d);
    }
  }
  const double decay = 0.5 * (lo + hi);
  return DecayCandidate{decay, fit_decay(obs, y, decay)};
}

} // namespace

TermCurve fit_term_curve_for_emove(std::span<const CensorObsInput> obs, double emove,
                                   const EarningsFitConfig &cfg) noexcept {
  // Precompute every observation's censored ATM vol ONCE, up front -- this is
  // the fit's only allocation. The decay-search loops below (coarse grid +
  // golden-section refine) touch only obs/y (already-allocated spans) and
  // local scalar sums; they allocate nothing.
  std::vector<double> y;
  y.reserve(obs.size());
  for (const auto &o : obs) {
    y.push_back(censored_atm_vol(o, emove, cfg.wcen_floor));
  }
  const std::span<const double> y_span{y};

  // Coarse log-spaced grid over [kDecayLo, kDecayHi]: statically bounded
  // (kDecayGridN iterations), finds which decade-scale neighborhood of decay
  // values minimizes RMS residual before the golden-section refine below
  // narrows in on it. Log spacing matches decay's natural (multiplicative)
  // scale -- a linear grid would under-resolve the short end and over-
  // resolve the long end.
  const double log_lo = std::log(kDecayLo);
  const double log_hi = std::log(kDecayHi);
  const double log_step = (log_hi - log_lo) / static_cast<double>(kDecayGridN - 1);

  double best_decay = kDecayLo;
  DecayFit best_fit = fit_decay(obs, y_span, best_decay);
  int best_idx = 0;
  for (int i = 1; i < kDecayGridN; ++i) {
    const double decay = std::exp(log_lo + static_cast<double>(i) * log_step);
    const DecayFit fit = fit_decay(obs, y_span, decay);
    if (fit.rms < best_fit.rms) {
      best_fit = fit;
      best_decay = decay;
      best_idx = i;
    }
  }

  // Refine within the coarse grid's neighboring bracket around the best
  // index (clamped at the grid ends), then keep whichever of {coarse best,
  // refined} has the lower RMS -- the refine's narrower bracket cannot make
  // the fit worse than the coarse grid already found, but comparing rather
  // than assuming keeps this correct even if it someday did.
  const int lo_idx = std::max(best_idx - 1, 0);
  const int hi_idx = std::min(best_idx + 1, kDecayGridN - 1);
  const double lo = std::exp(log_lo + static_cast<double>(lo_idx) * log_step);
  const double hi = std::exp(log_lo + static_cast<double>(hi_idx) * log_step);
  const DecayCandidate refined = golden_section_refine(obs, y_span, lo, hi);
  if (refined.fit.rms < best_fit.rms) {
    best_fit = refined.fit;
    best_decay = refined.decay;
  }

  return TermCurve{best_fit.lt + best_fit.a, best_fit.lt, best_decay, best_fit.rms};
}

double term_curve_value(const TermCurve &c, double T) noexcept {
  return c.lt + (c.st - c.lt) * std::exp(-c.decay * T);
}

namespace {

// ── Outer eMove search (Task 4) ────────────────────────────────────────────
//
// Wraps `fit_term_curve_for_emove` as a 1-D search over `emove`: a coarse
// LINEAR grid across `[emove_lo, emove_hi]` finds a competitive neighborhood,
// then a golden-section refine (bounded by `EarningsFitConfig::max_iters`,
// per the brief) narrows that neighborhood to the precise optimum -- the
// same "grid finds the region, golden-section refines it" structure
// `fit_term_curve_for_emove`'s own decay search already uses above, extended
// to this outer layer for the reason explained on `search_emove` itself: a
// bare two-point golden section over the WHOLE bracket has no defense
// against a non-unimodal objective when its two starting interior points
// both land on the wrong side of the true optimum. These tunables are NOT
// exposed via `EarningsFitConfig` (mirrors kDecayLo/kDecayHi/... above): the
// brief fixes the search's bracket (cfg.emove_lo/emove_hi) and refine cap
// (cfg.max_iters); these are the grid density and secondary-diagnosis
// epsilons for the bracket-convergence / bound-pinned / flat-objective
// checks, scaled to the eMove/vol domain (O(1e-2)-O(1)), never to
// DBL_EPSILON.
constexpr int kOuterGridN = 41;               // coarse linear grid density (mirrors kDecayGridN)
constexpr double kOuterBracketTol = 1e-9;     // golden-section bracket-width convergence
constexpr double kOuterBoundTol = 1e-6;       // "optimum sits on a bound" epsilon
constexpr double kOuterFlatRelTol = 1e-2;     // relative-improvement flatness epsilon
constexpr double kOuterFlatScaleFloor = 1e-6; // floor so a near-zero scan scale doesn't
                                               // blow up the relative flatness ratio

// True iff `emove` forces at least one event-bearing (`n > 0`) observation's
// RAW censored variance below the EFFECTIVE floor `censored_atm_vol` actually
// applies: `max(kWCenFloor, wcen_floor)` -- `censored_total_variance`
// (event_vol.hpp) floors at the fixed `kWCenFloor` first, then
// `censored_atm_vol` floors AGAIN at the caller-supplied `wcen_floor` (see
// this header's own self-review notes on that double floor); using only
// `kWCenFloor` here would under-detect clamping whenever a caller passes a
// STRICTER `cfg.wcen_floor > kWCenFloor`. Either floor binding means the
// model assumption `w_dirty >= n*emove^2` is violated for that observation
// at this candidate. The floor exists as a numerical guard for downstream
// serving (so a later `sqrt` never sees a non-positive value), NOT as
// evidence that a candidate `emove` is a good fit: once censoring clamps,
// that observation's `censored_atm_vol` collapses toward `sqrt(floor/T)`
// regardless of what its true censored vol should have been, and a FLEXIBLE
// 3-parameter term curve can fit a whole batch of such near-identical,
// near-zero, clamped points deceptively well (a spuriously LOW rms_resid
// that reflects the clamp, not the model). See `outer_objective`/
// `search_emove` for how this keeps the outer search from mistaking that
// clamp-driven artifact for the true optimum.
[[nodiscard]] bool any_observation_floors(std::span<const CensorObsInput> obs, double emove,
                                          double wcen_floor) noexcept {
  const double effective_floor = std::max(kWCenFloor, wcen_floor);
  for (const auto &o : obs) {
    if (o.n == 0) {
      continue; // w_cen = w_dirty - 0 = w_dirty, already validated > 0
    }
    const double raw = o.w_dirty - static_cast<double>(o.n) * emove * emove;
    if (raw < effective_floor) {
      return true;
    }
  }
  return false;
}

// The outer search's objective: `curve.rms_resid`, but `+infinity` (strictly
// worse than every legitimate candidate's finite rms) when `emove` clamps
// any observation (`any_observation_floors`). Applied to EVERY candidate the
// grid scan and golden-section refine touch, so neither stage can converge
// into -- or even directionally drift toward -- the clamp-collapse region on
// raw rms_resid alone. `curve.rms_resid` itself (Task 3's own, unmodified
// metric) is untouched; this is purely a comparison-ranking layer the outer
// search applies on top of it.
[[nodiscard]] double outer_objective(std::span<const CensorObsInput> obs, double emove,
                                     const TermCurve &curve, double wcen_floor) noexcept {
  if (any_observation_floors(obs, emove, wcen_floor)) {
    return std::numeric_limits<double>::infinity();
  }
  return curve.rms_resid;
}

// One evaluated candidate emove: the value, its wrapped `TermCurve` fit
// (`curve.rms_resid` is Task 3's own unmodified metric -- what
// `EarningsTermFit::fit_error` ultimately reports), and `objective` (what
// `search_emove`'s comparisons actually rank candidates by; see
// `outer_objective`).
struct EmoveEval {
  double emove{};
  TermCurve curve{};
  double objective{};
};

[[nodiscard]] EmoveEval evaluate_emove(std::span<const CensorObsInput> obs, double emove,
                                       const EarningsFitConfig &cfg) noexcept {
  const TermCurve curve = fit_term_curve_for_emove(obs, emove, cfg);
  return EmoveEval{emove, curve, outer_objective(obs, emove, curve, cfg.wcen_floor)};
}

// Golden-section search result plus the two diagnostic flags
// `classify_fit_code` maps onto `EmoveFitCode`.
struct OuterSearchResult {
  EmoveEval best{};
  bool max_steps_hit{};
  bool is_flat{};
};

// 1-D search minimizing `evaluate_emove(...).objective` over
// `[emove_lo, emove_hi]` (reordered defensively if the caller passed them
// backwards -- see below): a `kOuterGridN`-point coarse LINEAR grid (emove
// is naturally linear scale, unlike decay's multiplicative one -- no
// log-spacing needed) finds the best-scoring neighborhood, then a
// golden-section refine -- bounded by `cfg.max_iters` refine steps, per the
// brief -- narrows JUST that neighborhood (the grid's best index +/- one
// grid step) to the precise optimum. Mirrors `fit_term_curve_for_emove`'s
// own decay-search structure exactly (coarse grid -> local golden-section
// refine -> keep whichever of {grid best, refined} is better).
//
// WHY a grid stage at all, when the brief names "golden-section" alone: a
// bare two-point golden section starting from the WHOLE `[emove_lo,
// emove_hi]` bracket has no fallback when its two initial interior points
// (at the golden-ratio fractions of the bracket) both already sit in a
// region `outer_objective` scores worse than the true optimum -- the
// comparison between two already-bad points carries no information about
// which direction (if either) leads back toward the real minimum, so the
// bracket can shrink the WRONG way and never recover (this is exactly what
// a batch of event-bearing observations with a wide default
// `[0, emove_hi=0.30]` bracket can trigger: past some `emove`, EVERY
// observation clamps per `any_observation_floors`, and the two initial
// golden-ratio points can both already be past that clamp threshold). The
// grid's `kOuterGridN` points span the WHOLE bracket up front, so at least
// one of them almost always lands inside whatever neighborhood is genuinely
// competitive, giving the refine a correctly-anchored starting bracket
// instead of blindly bisecting from the two golden-ratio points alone. This
// is the brief's own "objective may not be perfectly unimodal on noisy real
// data" caveat, made concrete.
//
// `is_flat` (-> `EmoveFitCode::CenterFlat`) evidence: the WORST finite
// (non-clamped) objective seen anywhere in the grid, `scan_max_finite`,
// against the search's own best finding -- a real, identified minimum
// improves substantially on that generic/arbitrary-point scale; an
// under-identified objective (too few observations relative to the term
// curve's own 3 free parameters, or an event-count pattern that does not
// distinguish emove from the curve shape) does not, regardless of which
// point the search happens to converge near. Clamped (+infinity) grid
// points are excluded from this scale on purpose -- they say nothing about
// whether the LEGITIMATE region of the bracket identifies emove.
[[nodiscard]] OuterSearchResult search_emove(std::span<const CensorObsInput> obs, double emove_lo,
                                             double emove_hi,
                                             const EarningsFitConfig &cfg) noexcept {
  // Defensive ordering only, not validation: emove_lo/emove_hi are the
  // caller's contract (like fit_term_curve_for_emove's own cfg fields are;
  // see fit_earnings_term's doc comment). A misconfigured lo > hi still
  // produces a sane, ordered bracket instead of walking the golden-section
  // math backwards.
  const double lo0 = std::min(emove_lo, emove_hi);
  const double hi0 = std::max(emove_lo, emove_hi);

  // Coarse grid: kOuterGridN evenly spaced points INCLUDING both endpoints
  // (i=0 => lo0, i=kOuterGridN-1 => hi0), statically bounded (JPL Rule 2).
  EmoveEval best{};
  bool best_set = false;
  double scan_max_finite = 0.0;
  bool any_finite = false;
  int best_idx = 0;
  for (int i = 0; i < kOuterGridN; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(kOuterGridN - 1);
    const double e = lo0 + frac * (hi0 - lo0);
    const EmoveEval ev = evaluate_emove(obs, e, cfg);
    if (std::isfinite(ev.objective)) {
      scan_max_finite = any_finite ? std::max(scan_max_finite, ev.objective) : ev.objective;
      any_finite = true;
    }
    if (!best_set || ev.objective < best.objective) {
      best = ev;
      best_idx = i;
      best_set = true;
    }
  }

  // Golden-section refine within the coarse grid's neighboring bracket
  // around the best index (clamped at the grid ends) -- mirrors
  // `fit_term_curve_for_emove`'s own decay-search refine exactly. Capped at
  // cfg.max_iters (clamped non-negative -- a caller-supplied negative cap
  // degenerates to "no refine steps", never a negative/unbounded trip
  // count -- JPL Rule 2).
  const double grid_step = (hi0 - lo0) / static_cast<double>(kOuterGridN - 1);
  const int lo_idx = std::max(best_idx - 1, 0);
  const int hi_idx = std::min(best_idx + 1, kOuterGridN - 1);
  double lo = lo0 + static_cast<double>(lo_idx) * grid_step;
  double hi = lo0 + static_cast<double>(hi_idx) * grid_step;
  const int iters = std::max(cfg.max_iters, 0);
  double c = hi - kGolden * (hi - lo);
  double d = lo + kGolden * (hi - lo);
  EmoveEval fc = evaluate_emove(obs, c, cfg);
  EmoveEval fd = evaluate_emove(obs, d, cfg);
  for (int it = 0; it < iters; ++it) {
    if (!(hi - lo > kOuterBracketTol)) {
      break; // bracket already converged; further refine steps buy nothing
    }
    if (fc.objective < fd.objective) {
      hi = d;
      d = c;
      fd = fc;
      c = hi - kGolden * (hi - lo);
      fc = evaluate_emove(obs, c, cfg);
    } else {
      lo = c;
      c = d;
      fc = fd;
      d = lo + kGolden * (hi - lo);
      fd = evaluate_emove(obs, d, cfg);
    }
  }
  // Cap exhausted before the LOCAL refine bracket collapsed below
  // tolerance => the search did not converge (EmoveFitCode::MaxSteps); a
  // zero-iteration cap also lands here, correctly (no refine happened).
  const bool max_steps_hit = (hi - lo > kOuterBracketTol);

  EmoveEval refined = evaluate_emove(obs, 0.5 * (lo + hi), cfg);
  for (const EmoveEval &cand : {fc, fd}) {
    if (cand.objective < refined.objective) {
      refined = cand;
    }
  }
  // Keep whichever of {grid best, refined} scores better -- the refine's
  // narrower bracket cannot make the fit worse than the grid already found,
  // but comparing rather than assuming keeps this correct even if it someday
  // did (same pattern as fit_term_curve_for_emove's own decay refine above).
  if (refined.objective < best.objective) {
    best = refined;
  }
  if (std::isfinite(refined.objective)) {
    scan_max_finite = any_finite ? std::max(scan_max_finite, refined.objective) : refined.objective;
    any_finite = true;
  }

  const double scale = std::max(scan_max_finite, kOuterFlatScaleFloor);
  // best.curve.rms_resid (not best.objective, which is +infinity in the
  // fully-degenerate all-clamped fallback -- see any_finite below) is the
  // TRUE residual to compare against the grid's generic scale.
  const double improvement = scan_max_finite - best.curve.rms_resid;
  // !any_finite: every grid point AND the refine clamped (a pathological
  // cfg, e.g. emove_lo itself already clamps) -- no legitimate evidence
  // anywhere in the bracket, so this is flat/uninformative by definition,
  // not merely "improvement too small".
  const bool is_flat = !any_finite || !(improvement > kOuterFlatRelTol * scale);

  return OuterSearchResult{best, max_steps_hit, is_flat};
}

// Maps a completed `search_emove` result to `EmoveFitCode`, per the brief.
// Priority order (checked top to bottom, first match wins):
//   1. MaxSteps -- the search did not converge, so its bound/flat diagnosis
//      below is not yet trustworthy (the bracket may still be wide).
//   2. CenterFlat -- converged, but the data don't discriminate emove.
//      Checked before the bound tests: a flat objective's "best" point can
//      land anywhere (including near an edge) on numerical noise alone, and
//      reporting CenterFlat is the more informative/honest signal than a
//      spurious LeftBound/RightBound.
//   3. LeftBound / RightBound -- converged, non-flat, but the optimum sits
//      at (or past) a search-bracket edge -- a real signal the bracket
//      itself may be too narrow.
//   4. Minimum -- a genuine interior optimum.
[[nodiscard]] EmoveFitCode classify_fit_code(const OuterSearchResult &search, double emove_lo,
                                             double emove_hi) noexcept {
  if (search.max_steps_hit) {
    return EmoveFitCode::MaxSteps;
  }
  if (search.is_flat) {
    return EmoveFitCode::CenterFlat;
  }
  const double lo = std::min(emove_lo, emove_hi);
  const double hi = std::max(emove_lo, emove_hi);
  if (search.best.emove <= lo + kOuterBoundTol) {
    return EmoveFitCode::LeftBound;
  }
  if (search.best.emove >= hi - kOuterBoundTol) {
    return EmoveFitCode::RightBound;
  }
  return EmoveFitCode::Minimum;
}

// Samples a fitted parametric curve at every `tenor_T` grid point -- the
// SECONDARY `atm_cen` read (see `fit_earnings_term`'s own doc comment for why
// this is not the PRIMARY atmCenI_Nd target). Empty `tenor_T` => empty
// result, matching the brief. Bounded by `tenor_T.size()` (the SR grid is 12
// points; a caller-supplied span is otherwise unbounded but this is the
// function's only allocation, sized once up front, same pattern as
// `fit_term_curve_for_emove`'s own `y` precompute above).
[[nodiscard]] std::vector<double> sample_atm_cen(const TermCurve &curve,
                                                 std::span<const double> tenor_T) {
  std::vector<double> out;
  out.reserve(tenor_T.size());
  for (const double T : tenor_T) {
    out.push_back(term_curve_value(curve, T));
  }
  return out;
}

} // namespace

Result<EarningsTermFit> fit_earnings_term(std::span<const CensorObsInput> obs,
                                          const EarningsFitConfig &cfg) {
  // Validate at the boundary (agent profile SS4): obs.size() < 2 cannot pin
  // down the term curve's own 3 free parameters {st,lt,decay} independently
  // of emove (see the header's doc comment), and a non-finite/non-positive
  // T or w_dirty would propagate NaN/garbage through every downstream sqrt
  // and division rather than failing loudly here.
  if (obs.size() < 2) {
    return Err(ErrorCode::InvalidArgument,
               "fit_earnings_term: need >= 2 expiries to identify {emove, st, lt, decay}");
  }

  bool all_zero_events = true;
  for (const auto &o : obs) {
    if (!std::isfinite(o.T) || !(o.T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "fit_earnings_term: every T must be finite and > 0");
    }
    if (!std::isfinite(o.w_dirty) || !(o.w_dirty > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "fit_earnings_term: every w_dirty must be finite and > 0");
    }
    if (o.n != 0) {
      all_zero_events = false;
    }
  }

  EarningsTermFit out{};
  out.expiry_count = obs.size();

  if (all_zero_events) {
    // No scheduled event before any listed expiry: emove is not
    // identifiable from obs at all (there is nothing to censor out). This is
    // the caller's ex-event curve, still a valid Ok result (see the header's
    // doc comment), not an error -- skip the outer search entirely rather
    // than run it over an objective that is exactly emove-invariant by
    // construction (every obs[i].n == 0 makes censored_atm_vol's
    // n_i*emove^2 term identically 0 for every candidate emove).
    const TermCurve curve = fit_term_curve_for_emove(obs, 0.0, cfg);
    out.emove = 0.0;
    out.st = curve.st;
    out.lt = curve.lt;
    out.decay = curve.decay;
    out.fit_error = curve.rms_resid;
    out.fit_code = EmoveFitCode::CenterFlat;
    out.atm_cen = sample_atm_cen(curve, cfg.tenor_T);
    return Ok(std::move(out));
  }

  const OuterSearchResult search = search_emove(obs, cfg.emove_lo, cfg.emove_hi, cfg);
  out.emove = search.best.emove;
  out.st = search.best.curve.st;
  out.lt = search.best.curve.lt;
  out.decay = search.best.curve.decay;
  out.fit_error = search.best.curve.rms_resid;
  out.fit_code = classify_fit_code(search, cfg.emove_lo, cfg.emove_hi);
  out.atm_cen = sample_atm_cen(search.best.curve, cfg.tenor_T);
  return Ok(std::move(out));
}

} // namespace atx::vol
