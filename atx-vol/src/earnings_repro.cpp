#include "atx/vol/earnings_repro.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/session.hpp"       // VolaSession (full def), PricedSurface
#include "atx/vol/sr_tenor_grid.hpp" // SrTenorGrid, tenor_years

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// One listed expiry's raw (dirty) inputs plus its own T, kept as a separate
// local type (rather than re-deriving T from `CensorObsInput` repeatedly) so
// the bracket search below reads plainly.
struct ListedPillar {
  double T{};
  double w_dirty{};
  std::size_t n{};
};

// Censored ATM vol at one CLAMP pillar: reuses `censored_atm_vol` (Task 1,
// earnings_term_fit.hpp) directly on the pillar's OWN T -- the flat-
// extrapolation convention for a query tenor outside the listed range (see
// the header's module doc, point 4).
[[nodiscard]] double clamp_atm_cen(const ListedPillar &p, double emove, double wcen_floor) noexcept {
  return censored_atm_vol(CensorObsInput{p.T, p.w_dirty, p.n}, emove, wcen_floor);
}

} // namespace

Result<EarningsReproResult> run_earnings_repro(const VolaSession &sess, const EventSchedule &sched,
                                                std::int64_t now_ns) {
  auto ps_res = sess.to_priced_surface();
  if (!ps_res.has_value()) {
    return Err(ps_res.error());
  }
  const PricedSurface &ps = *ps_res;

  const auto slices = sess.surface().essvi_slices(); // ascending T (VolSurface precondition)

  std::vector<ListedPillar> listed;
  listed.reserve(slices.size());
  for (const auto &sl : slices) {
    const double T = sl.T;
    if (!(T > 0.0)) {
      continue;
    }
    const double F = ps.forward_at(T);
    if (!(F > 0.0)) {
      continue;
    }
    const double w_dirty = ps.total_variance(F, T);
    std::size_t n{};
    if (sl.expiry_ns != 0) {
      n = sched.count_between(now_ns, sl.expiry_ns);
    } else {
      // Unstamped slice fallback: synthesize the maturity instant from T
      // (Calendar365-inverse) -- the same fallback SessionInputs::events'
      // own implied-eMove solve uses (session.hpp doc, solve_implied_emove).
      n = count_events_at(sched, now_ns, T);
    }
    listed.push_back(ListedPillar{T, w_dirty, n});
  }

  if (listed.size() < 2) {
    return Err(ErrorCode::InvalidArgument,
               "run_earnings_repro: fewer than 2 listed expiries with a positive T/forward");
  }

  EarningsReproResult out;
  std::vector<CensorObsInput> obs;
  obs.reserve(listed.size());
  out.listed_obs.reserve(listed.size());
  for (const auto &p : listed) {
    const CensorObsInput o{p.T, p.w_dirty, p.n};
    obs.push_back(o);
    out.listed_obs.push_back(o);
  }

  for (std::size_t i = 0; i < SrTenorGrid::kTradingDays.size(); ++i) {
    out.tenor_T[i] = tenor_years(now_ns, SrTenorGrid::kTradingDays[i], sess.inputs().time);
  }

  EarningsFitConfig cfg;
  cfg.tenor_T = out.tenor_T; // non-owning view into `out`, alive for the rest of this call

  auto fit_res = fit_earnings_term(obs, cfg);
  if (!fit_res.has_value()) {
    return Err(fit_res.error());
  }
  out.fit = *fit_res;

  // PRIMARY atmCenI: raw censored-space interpolation at each of the 12 SR
  // tenors, bracketing the listed pillars and reusing `event_aware_w`
  // (n_query = 0, so it censors + interpolates with no re-add) -- see the
  // header's module doc, point 4.
  for (std::size_t i = 0; i < out.tenor_T.size(); ++i) {
    const double Tq = out.tenor_T[i];
    out.n_earn[i] = count_events_at(sched, now_ns, Tq);

    // `listed` is ascending T; find the first pillar with T >= Tq.
    std::size_t hi = 0;
    while (hi < listed.size() && listed[hi].T < Tq) {
      ++hi;
    }

    if (hi == 0) {
      // Tq at or before the first listed pillar: clamp flat.
      out.atm_cen_i[i] = clamp_atm_cen(listed.front(), out.fit.emove, cfg.wcen_floor);
    } else if (hi >= listed.size()) {
      // Tq past the last listed pillar: clamp flat.
      out.atm_cen_i[i] = clamp_atm_cen(listed.back(), out.fit.emove, cfg.wcen_floor);
    } else {
      const ListedPillar &lo = listed[hi - 1];
      const ListedPillar &hip = listed[hi];
      if (!(hip.T > lo.T)) {
        // Defensive: a degenerate/duplicate-T bracket (should not occur on a
        // validly-fitted ascending-T surface) -- clamp to the upper pillar
        // rather than divide by zero inside event_aware_w's interpolation
        // weight.
        out.atm_cen_i[i] = clamp_atm_cen(hip, out.fit.emove, cfg.wcen_floor);
      } else {
        const double w_cen = event_aware_w(lo.w_dirty, lo.T, lo.n, hip.w_dirty, hip.T, hip.n, Tq,
                                           /*n_query=*/0, out.fit.emove);
        const double floored = std::max(w_cen, cfg.wcen_floor);
        out.atm_cen_i[i] = std::sqrt(floored / Tq);
      }
    }
  }

  return Ok(std::move(out));
}

} // namespace atx::vol
