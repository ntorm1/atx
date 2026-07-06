#include "atx/vol/curve_selector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"  // american_price
#include "atx/vol/calib.hpp"     // build_observations, build_observations_european, FitObs
#include "atx/vol/deamer.hpp"    // resolve_chain_forward
#include "atx/vol/universe.hpp"  // Chain, Underlying

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
  v.push_back(convex);
  v.push_back(essvi);
  return v;
}

namespace {

// Running out-of-sample accumulator for one candidate across expiries.
struct Accum {
  std::size_t n = 0, in_band = 0;
  double wsum = 0.0, win = 0.0;
  std::size_t dof_sum = 0, n_slices = 0;
};

}  // namespace

Result<SelectorResult> select_curve(const Underlying& under,
                                    const SurfaceParityInputs& in,
                                    const SelectorConfig& sel) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument,
               "select_curve: non-positive S or non-finite r");
  }
  const std::vector<CurveConfig> candidates =
      sel.candidates.empty() ? default_selector_candidates() : sel.candidates;
  if (candidates.empty()) {
    return Err(ErrorCode::InvalidArgument, "select_curve: no candidates");
  }

  // Score every candidate against a SHARED per-expiry split: the expensive cold
  // de-Americanization (build_observations_european) and the even/odd strike
  // split are computed ONCE per expiry and reused across all candidates — only
  // the per-slice fit + held-out re-pricing is per-candidate. Mirrors
  // spy_oos_check: fit the EVEN strikes, score the HELD-OUT odd strikes by
  // re-Americanizing the model IV against the raw NBBO.
  std::vector<Accum> acc(candidates.size());
  unsigned scored_expiries = 0;
  for (const Chain& chain : under.chains) {
    const double T = chain.T;
    if (!(T > 0.019)) {
      continue;
    }
    if (sel.oos_max_expiries > 0 && scored_expiries >= sel.oos_max_expiries) {
      break;
    }
    const auto d = resolve_chain_forward(chain, in.S, in.r, in.cash_divs,
                                         in.now_ts_ns, in.deam);
    if (!d) {
      continue;
    }
    const double F = d->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      continue;
    }
    const double q_eff = in.r - std::log(F / in.S) / T;
    const double df = std::exp(-in.r * T);

    const auto am = build_observations(chain, F, T, df, in.calib);
    const auto eu = build_observations_european(chain, in.S, in.r, F, T, df, in.calib);
    if (!am || !eu) {
      continue;
    }
    if (am->obs.size() != eu->obs.size() || eu->obs.size() < 8) {
      continue;  // misaligned or too thin to split
    }
    const std::size_t m = eu->obs.size();
    std::vector<std::size_t> ord(m);
    std::iota(ord.begin(), ord.end(), std::size_t{0});
    std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
      return eu->obs[a].K < eu->obs[b].K;
    });

    std::vector<FitObs> fit_obs;
    fit_obs.reserve(m / 2 + 1);
    for (std::size_t p = 0; p < m; p += 2) {
      fit_obs.push_back(eu->obs[ord[p]]);
    }
    if (fit_obs.size() < 3) {
      continue;
    }
    ++scored_expiries;

    for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
      auto slice = fit_slice_curve(candidates[ci], fit_obs, F, T, df);
      if (!slice) {
        continue;
      }
      const IVolCurve* const cv = slice->get();
      acc[ci].dof_sum += cv->dof();
      ++acc[ci].n_slices;
      for (std::size_t p = 1; p < m; p += 2) {  // held-out = odd strikes
        const FitObs& oe = eu->obs[ord[p]];
        const FitObs& oa = am->obs[ord[p]];
        const double half = 0.5 * oa.spread;
        const double bid = oa.mid - half, ask = oa.mid + half;
        if (!(bid > 0.0) || !(ask > bid)) {
          continue;
        }
        const double miv = cv->iv(oe.k);
        if (!std::isfinite(miv)) {
          continue;
        }
        const auto fv = american_price(in.S, oa.K, T, miv, in.r, q_eff, oa.side,
                                       in.deam.method, in.deam.al_opts);
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
      }
    }
  }

  SelectorResult out;
  out.scores.reserve(candidates.size());
  for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
    const Accum& a = acc[ci];
    CandidateScore cs;
    cs.kind = candidates[ci].kind;
    cs.dof_sum = a.dof_sum;
    cs.n_slices = a.n_slices;
    cs.n_holdout = a.n;
    cs.oos_in_band =
        (a.n > 0) ? static_cast<double>(a.in_band) / static_cast<double>(a.n) : 0.0;
    cs.oos_vw = (a.wsum > 0.0) ? a.win / a.wsum : 0.0;
    out.scores.push_back(cs);
  }

  // Best out-of-sample vega-weighted in-band; ties (within parsimony_margin)
  // break toward the smaller per-slice DoF (a dense curve wins only when its
  // extra flexibility pays off out of sample).
  double best_vw = -1.0;
  for (const CandidateScore& s : out.scores) {
    if (s.n_holdout > 0) {
      best_vw = std::max(best_vw, s.oos_vw);
    }
  }
  if (best_vw < 0.0) {
    return Err(ErrorCode::NotFound,
               "select_curve: no candidate produced a scorable fit");
  }

  auto avg_dof = [](const CandidateScore& s) {
    return s.n_slices > 0
               ? static_cast<double>(s.dof_sum) / static_cast<double>(s.n_slices)
               : 1.0e18;
  };
  std::size_t best_i = 0;
  bool have = false;
  for (std::size_t i = 0; i < out.scores.size(); ++i) {
    const CandidateScore& s = out.scores[i];
    if (s.n_holdout == 0 || s.oos_vw < best_vw - sel.parsimony_margin) {
      continue;  // not within the tie band of the leader
    }
    if (!have || avg_dof(s) < avg_dof(out.scores[best_i]) ||
        (avg_dof(s) == avg_dof(out.scores[best_i]) && s.oos_vw > out.scores[best_i].oos_vw)) {
      best_i = i;
      have = true;
    }
  }

  out.chosen = candidates[best_i];
  out.chosen_index = best_i;
  return Ok(std::move(out));
}

}  // namespace atx::vol
