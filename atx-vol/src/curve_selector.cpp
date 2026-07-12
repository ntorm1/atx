#include "atx/vol/curve_selector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"     // american_price
#include "atx/vol/arb.hpp"          // arb_check_butterfly_svi_mm, arb_check_butterfly_slice
#include "atx/vol/calib.hpp"        // build_observations, build_observations_european, FitObs
#include "atx/vol/deamer.hpp"       // resolve_chain_forward
#include "atx/vol/fit_metrics.hpp"  // slice_fit_metrics, SliceFitMetrics
#include "atx/vol/universe.hpp"     // Chain, Underlying

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::vector<CurveConfig> default_selector_candidates() {
  std::vector<CurveConfig> v;
  CurveConfig convex;
  convex.kind = VolCurveKind::ConvexDense;
  convex.convex.node_cap = 40;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CurveConfig linear;
  linear.kind = VolCurveKind::LinearVariance;
  CurveConfig svi;
  svi.kind = VolCurveKind::Svi;
  CurveConfig c8;
  c8.kind = VolCurveKind::C8;
  v.push_back(convex);
  v.push_back(linear);
  v.push_back(essvi);
  v.push_back(svi);
  v.push_back(c8);
  return v;
}

namespace detail {

// Per-kind butterfly violation count for a fitted slice (the selection-time
// mapping): closed-form Martini-Mingone for raw-SVI, grid Durrleman g-check for
// C8, the fitted post-fit Lee/Roper diagnostic count carried on the params for
// SplineVol (see spline_curve.hpp's file-top comment, step 6 -- NOT projected,
// just reported), and 0 for the by-construction / out-of-scope kinds
// (ConvexDense, eSSVI, LinearVariance). `k_lo`/`k_hi` bound the C8 grid (padded
// by the caller).
[[nodiscard]] std::uint32_t slice_butterfly_violations(const IVolCurve &cv,
                                                       double T, double k_lo,
                                                       double k_hi) noexcept {
  switch (cv.kind()) {
  case VolCurveKind::Svi: {
    const auto &sp = static_cast<const SviCurve &>(cv).slice();
    return arb_check_butterfly_svi_mm(sp, T).n_violations;
  }
  case VolCurveKind::C8: {
    const auto bf = arb_check_butterfly_slice(
        [&cv](double kk) { return cv.w(kk); }, T, k_lo, k_hi, 64u);
    return bf.has_value() ? static_cast<std::uint32_t>(bf->size()) : 0u;
  }
  case VolCurveKind::ConvexDense:
  case VolCurveKind::Essvi:
  case VolCurveKind::LinearVariance:
    return 0u;  // by-construction arb-free (LinearVariance g-check out of scope)
  case VolCurveKind::SplineVol: {
    const auto &sv = static_cast<const SplineVolCurve &>(cv);
    const std::size_t n = sv.params().n_butterfly_viol;
    return n > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(n);
  }
  }
  return 0u;
}

}  // namespace detail

namespace {

// Running out-of-sample accumulator for one candidate across expiries.
struct Accum {
  std::size_t n = 0, in_band = 0;
  double wsum = 0.0, win = 0.0;
  std::size_t dof_sum = 0, n_slices = 0;
  // Task C2.5 — held-out fit-metric collection + butterfly disqualification.
  std::vector<double> iv_model, iv_mkt, bid, ask, vega;
  std::uint32_t n_butterfly_viol = 0;
  bool disqualified = false;
};

[[nodiscard]] bool valid_expiry_rates(const SurfaceParityInputs &in,
                                      const Underlying &under) noexcept {
  if (in.expiry_rates.empty() && in.expiry_rate_T.empty()) {
    return true;
  }
  if (in.expiry_rates.size() != under.chains.size() ||
      in.expiry_rate_T.size() != under.chains.size()) {
    return false;
  }
  for (std::size_t i = 0; i < under.chains.size(); ++i) {
    if (!std::isfinite(in.expiry_rates[i]) || !std::isfinite(in.expiry_rate_T[i]) ||
        !(in.expiry_rate_T[i] > 0.0) || in.expiry_rate_T[i] != under.chains[i].T) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] double expiry_rate(const SurfaceParityInputs &in, std::size_t index) noexcept {
  return in.expiry_rates.empty() ? in.r : in.expiry_rates[index];
}

} // namespace

Result<SelectorResult> select_curve(const Underlying &under, const SurfaceParityInputs &in,
                                    const SelectorConfig &sel) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument, "select_curve: non-positive S or non-finite r");
  }
  if (!valid_expiry_rates(in, under)) {
    return Err(ErrorCode::InvalidArgument, "select_curve: invalid expiry rate vectors");
  }
  std::vector<CurveConfig> candidates =
      sel.candidates.empty() ? default_selector_candidates() : sel.candidates;
  if (candidates.empty()) {
    return Err(ErrorCode::InvalidArgument, "select_curve: no candidates");
  }

  // Gated SplineVol candidacy (Task I5): append a SplineVol candidate to the
  // working ladder when ANY supplied candidate opts in via
  // `CurveConfig::spline_candidate`. `default_selector_candidates()` itself
  // never sets the flag, so a caller passing an empty (default) candidate
  // list is completely unaffected -- bit-identical selection to pre-task.
  // The appended candidate's `SplineFitOpts` are copied from the FIRST
  // candidate that set the flag (not left default-constructed), so a
  // caller's tuned spline knobs (grid/lambda/mult_floor/min_obs) actually
  // reach the fit it asked for.
  {
    bool want_spline = false;
    bool has_spline = false;
    const CurveConfig *spline_source = nullptr;
    for (const CurveConfig &c : candidates) {
      if (c.spline_candidate && spline_source == nullptr) {
        spline_source = &c;
      }
      want_spline = want_spline || c.spline_candidate;
      has_spline = has_spline || (c.kind == VolCurveKind::SplineVol);
    }
    if (want_spline && !has_spline) {
      CurveConfig spline_cfg;
      spline_cfg.kind = VolCurveKind::SplineVol;
      if (spline_source != nullptr) {
        spline_cfg.spline = spline_source->spline;
      }
      candidates.push_back(spline_cfg);
    }
  }

  // Score every candidate against a SHARED per-expiry split: the expensive cold
  // de-Americanization (build_observations_european) and the even/odd strike
  // split are computed ONCE per expiry and reused across all candidates — only
  // the per-slice fit + held-out re-pricing is per-candidate. Mirrors
  // spy_oos_check: fit the EVEN strikes, score the HELD-OUT odd strikes by
  // re-Americanizing the model IV against the raw NBBO.
  // build_observations_european applies `max_obs_per_slice` internally, while the
  // raw American population below does not; a configured cap would therefore make
  // the two sizes disagree and the alignment guard would silently skip EVERY dense
  // expiry, failing the whole selection with NotFound. The cap is a cold-fit
  // latency knob, not a modelling choice, so score the family on the full board
  // and leave the cap to the production fit that follows.
  CalibOpts sel_calib = in.calib;
  sel_calib.max_obs_per_slice = 0;

  std::vector<Accum> acc(candidates.size());
  unsigned scored_expiries = 0;
  for (std::size_t chain_index = 0u; chain_index < under.chains.size(); ++chain_index) {
    const Chain &chain = under.chains[chain_index];
    const double rate = expiry_rate(in, chain_index);
    const double T = chain.T;
    if (!(T > 0.019)) {
      continue;
    }
    if (sel.oos_max_expiries > 0 && scored_expiries >= sel.oos_max_expiries) {
      break;
    }
    const auto d = resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, in.deam);
    if (!d) {
      continue;
    }
    const double F = d->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      continue;
    }
    const double q_eff = rate - std::log(F / in.S) / T;
    const double df = std::exp(-rate * T);

    const auto am = build_observations(chain, F, T, df, sel_calib);
    const auto eu = build_observations_european(chain, in.S, rate, F, T, df, sel_calib);
    if (!am || !eu) {
      continue;
    }
    if (am->obs.size() != eu->obs.size() || eu->obs.size() < 8) {
      continue; // misaligned or too thin to split
    }
    const std::size_t m = eu->obs.size();
    std::vector<std::size_t> ord(m);
    std::iota(ord.begin(), ord.end(), std::size_t{0});
    std::sort(ord.begin(), ord.end(),
              [&](std::size_t a, std::size_t b) { return eu->obs[a].K < eu->obs[b].K; });

    std::vector<FitObs> fit_obs;
    fit_obs.reserve(m / 2 + 1);
    for (std::size_t p = 0; p < m; p += 2) {
      fit_obs.push_back(eu->obs[ord[p]]);
    }
    if (fit_obs.size() < 3) {
      continue;
    }
    ++scored_expiries;

    // Grid bounds for the C8 butterfly disqualification check: the fitted
    // strikes padded by 0.5 in log-moneyness (matches the fit_slice_curve gate).
    double fit_k_lo = fit_obs.front().k;
    double fit_k_hi = fit_obs.front().k;
    for (const FitObs &o : fit_obs) {
      fit_k_lo = std::min(fit_k_lo, o.k);
      fit_k_hi = std::max(fit_k_hi, o.k);
    }

    for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
      auto slice = fit_slice_curve(candidates[ci], fit_obs, F, T, df);
      if (!slice) {
        continue;
      }
      const IVolCurve *const cv = slice->get();
      acc[ci].dof_sum += cv->dof();
      ++acc[ci].n_slices;
      // Butterfly disqualification (fit-metrics selection signal): a family with
      // any butterfly-violating fitted slice scores as a fit-failure.
      const std::uint32_t nv =
          detail::slice_butterfly_violations(*cv, T, fit_k_lo - 0.5, fit_k_hi + 0.5);
      acc[ci].n_butterfly_viol += nv;
      if (nv > 0) {
        acc[ci].disqualified = true;
      }
      for (std::size_t p = 1; p < m; p += 2) { // held-out = odd strikes
        const FitObs &oe = eu->obs[ord[p]];
        const FitObs &oa = am->obs[ord[p]];
        const double half = 0.5 * oa.spread;
        const double bid = oa.mid - half, ask = oa.mid + half;
        if (!(bid > 0.0) || !(ask > bid)) {
          continue;
        }
        const double miv = cv->iv(oe.k);
        if (!std::isfinite(miv)) {
          continue;
        }
        const auto fv = american_price(in.S, oa.K, T, miv, rate, q_eff, oa.side, in.deam.method,
                                       in.deam.al_opts);
        if (!fv) {
          continue;
        }
        const bool inb = (*fv >= bid && *fv <= ask);
        ++acc[ci].n;
        if (inb) {
          ++acc[ci].in_band;
        }
        const double w = (oe.vega > 0.0) ? oe.vega * oe.vega : 0.0;
        acc[ci].wsum += w;
        if (inb) {
          acc[ci].win += w;
        }
        // Held-out fit metrics (vol space, self-consistent European bid/ask/vega
        // from the de-Americanized obs). Feeds slice_fit_metrics -> chi2_reduced.
        const double eu_half = 0.5 * oe.spread;
        const double eu_bid = oe.mid - eu_half;
        const double eu_ask = oe.mid + eu_half;
        if (std::isfinite(oe.sigma_mkt) && oe.sigma_mkt > 0.0 && oe.vega > 0.0 &&
            eu_bid > 0.0 && eu_ask > eu_bid) {
          acc[ci].iv_model.push_back(miv);
          acc[ci].iv_mkt.push_back(oe.sigma_mkt);
          acc[ci].bid.push_back(eu_bid);
          acc[ci].ask.push_back(eu_ask);
          acc[ci].vega.push_back(oe.vega);
        }
      }
    }
  }

  SelectorResult out;
  out.scores.reserve(candidates.size());
  for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
    const Accum &a = acc[ci];
    CandidateScore cs;
    cs.kind = candidates[ci].kind;
    cs.dof_sum = a.dof_sum;
    cs.n_slices = a.n_slices;
    cs.n_holdout = a.n;
    cs.n_in_band = a.in_band;
    cs.vega_weight_in_band = a.win;
    cs.vega_weight_total = a.wsum;
    cs.oos_in_band = (a.n > 0) ? static_cast<double>(a.in_band) / static_cast<double>(a.n) : 0.0;
    cs.oos_vw = (a.wsum > 0.0) ? a.win / a.wsum : 0.0;
    cs.n_butterfly_viol = a.n_butterfly_viol;
    cs.disqualified = a.disqualified;
    // Reduced chi-square (and companion metrics) over the held-out sample. Needs
    // N > dof for a positive denominator; otherwise the metrics stay invalid and
    // chi2_reduced does not participate in the tie-break.
    if (a.iv_model.size() > a.dof_sum) {
      const auto m_fit =
          slice_fit_metrics(a.iv_model, a.iv_mkt, a.bid, a.ask, a.vega, a.dof_sum);
      if (m_fit.has_value()) {
        cs.chi2_reduced = m_fit->chi2_reduced;
        cs.rmse_vol = m_fit->rmse_vol;
        cs.avE5_vol = m_fit->avE5_vol;
        cs.n_within_band = m_fit->n_within_band;
        cs.metrics_valid = true;
      }
    }
    out.scores.push_back(cs);
  }

  // A scorable, non-disqualified candidate must exist.
  double best_vw = -1.0;
  for (const CandidateScore &s : out.scores) {
    if (s.n_holdout > 0 && !s.disqualified) {
      best_vw = std::max(best_vw, s.oos_vw);
    }
  }
  if (best_vw < 0.0) {
    return Err(ErrorCode::NotFound, "select_curve: no candidate produced a scorable fit");
  }

  // Winner: oos_vw, then chi2_reduced closest to 1, then parsimony DoF (see
  // select_best_candidate). Butterfly-disqualified families are excluded.
  const std::size_t best_i = select_best_candidate(out.scores, sel.parsimony_margin);
  out.chosen = candidates[best_i];
  out.chosen_index = best_i;
  return Ok(std::move(out));
}

std::size_t select_best_candidate(const std::vector<CandidateScore> &scores,
                                  double parsimony_margin) noexcept {
  double best_vw = -1.0;
  for (const CandidateScore &s : scores) {
    if (s.n_holdout > 0 && !s.disqualified) {
      best_vw = std::max(best_vw, s.oos_vw);
    }
  }
  if (best_vw < 0.0) {
    return 0;  // caller gates on scorability; nothing selectable
  }

  const auto avg_dof = [](const CandidateScore &s) {
    return s.n_slices > 0 ? static_cast<double>(s.dof_sum) / static_cast<double>(s.n_slices)
                          : 1.0e18;
  };
  // Distance of the reduced chi-square from the ideal 1.0; candidates without a
  // valid reduced chi-square do not compete on this axis (treated as +inf).
  const auto chi2_dist = [](const CandidateScore &s) {
    return s.metrics_valid ? std::fabs(s.chi2_reduced - 1.0)
                           : std::numeric_limits<double>::infinity();
  };
  constexpr double kChi2Tol = 1.0e-9;  // fall through to DoF when chi2 ~equal

  std::size_t best_i = 0;
  bool have = false;
  for (std::size_t i = 0; i < scores.size(); ++i) {
    const CandidateScore &s = scores[i];
    if (s.disqualified || s.n_holdout == 0 || s.oos_vw < best_vw - parsimony_margin) {
      continue;  // failed / disqualified / outside the tie band of the leader
    }
    if (!have) {
      best_i = i;
      have = true;
      continue;
    }
    const CandidateScore &b = scores[best_i];
    const double ds = chi2_dist(s);
    const double db = chi2_dist(b);
    if (ds < db - kChi2Tol) {
      best_i = i;
      continue;
    }
    if (ds > db + kChi2Tol) {
      continue;
    }
    const double as = avg_dof(s);
    const double ab = avg_dof(b);
    if (as < ab) {
      best_i = i;
      continue;
    }
    if (as > ab) {
      continue;
    }
    if (s.oos_vw > b.oos_vw) {
      best_i = i;
    }
  }
  return best_i;
}

Result<CandidateScore> score_curve_oos(const Underlying &under, const SurfaceParityInputs &in,
                                       const CurveConfig &curve, const SelectorConfig &scoring) {
  SelectorConfig one = scoring;
  one.candidates = {curve};
  ATX_TRY(SelectorResult selected, select_curve(under, in, one));
  if (selected.scores.size() != 1u) {
    return Err(ErrorCode::Internal, "score_curve_oos: one-family score unavailable");
  }
  return Ok(selected.scores.front());
}

} // namespace atx::vol
