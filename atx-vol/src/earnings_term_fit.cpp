#include "atx/vol/earnings_term_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "atx/vol/event_vol.hpp" // censored_total_variance

namespace atx::vol {

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

} // namespace atx::vol
