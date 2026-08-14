#include "analytics/earnings_repro.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "analytics/earnings_repro_config.hpp" // EarningsReproConfig, InterpSpace
#include "atx/vol/api/fitting/session.hpp"               // VolaSession (full def), PricedSurface
#include "atx/vol/api/fitting/sr_tenor_grid.hpp"         // SrTenorGrid, tenor_years

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
// the header's module doc, point 4). Space-independent (a single pillar has no
// cross-pillar interpolation), so it is shared across every config convention.
[[nodiscard]] double clamp_atm_cen(const ListedPillar &p, double emove, double wcen_floor) noexcept {
  return censored_atm_vol(CensorObsInput{p.T, p.w_dirty, p.n}, emove, wcen_floor);
}

// Censored ATM vol at an INTERIOR query tenor `Tq` bracketed by pillars
// `lo`/`hip` (lo.T < Tq <= hip.T), under the (censor_space, interp) convention:
//
//   censor_space == true  (SR FLEX): censor each pillar BEFORE interpolating.
//     - Variance: interpolate censored TOTAL VARIANCE linearly in T
//       (`event_aware_w`, n_query = 0 -> censor both pillars, blend, no re-add)
//       -- the historical default path.
//     - Vol:      interpolate censored VOL linearly in T (each pillar's
//       `censored_atm_vol`).
//   censor_space == false (plain): interpolate a single cross-pillar DIRTY
//     quantity, then censor ONCE with the query tenor's own event count `nq`.
//     - Variance: interpolate the dirty total variance linearly in T.
//     - Vol:      interpolate the dirty vol linearly in T, rebuild the dirty
//       variance at Tq, then censor.
//
// Every censored value comes from `event_vol.hpp` / `earnings_term_fit.hpp`
// primitives; this only assembles their inputs and blends in the requested
// space. `wcen_floor` floors the final censored variance before the sqrt.
[[nodiscard]] double interior_censored_vol(const ListedPillar &lo, const ListedPillar &hip,
                                           double Tq, std::size_t nq, double emove,
                                           double wcen_floor, bool censor_space,
                                           InterpSpace interp) noexcept {
  const double frac = (Tq - lo.T) / (hip.T - lo.T);
  if (censor_space) {
    if (interp == InterpSpace::Variance) {
      const double w_cen = event_aware_w(lo.w_dirty, lo.T, lo.n, hip.w_dirty, hip.T, hip.n, Tq,
                                         /*n_query=*/0, emove);
      return std::sqrt(std::max(w_cen, wcen_floor) / Tq);
    }
    // interp == Vol
    const double s_lo =
        censored_atm_vol(CensorObsInput{lo.T, lo.w_dirty, lo.n}, emove, wcen_floor);
    const double s_hi =
        censored_atm_vol(CensorObsInput{hip.T, hip.w_dirty, hip.n}, emove, wcen_floor);
    return s_lo + (s_hi - s_lo) * frac;
  }
  // censor_space == false (interpolate-then-censor)
  double w_dirty_q{};
  if (interp == InterpSpace::Variance) {
    w_dirty_q = lo.w_dirty + (hip.w_dirty - lo.w_dirty) * frac;
  } else {
    const double s_lo = std::sqrt(lo.w_dirty / lo.T);
    const double s_hi = std::sqrt(hip.w_dirty / hip.T);
    const double s_q = s_lo + (s_hi - s_lo) * frac;
    w_dirty_q = s_q * s_q * Tq;
  }
  const double w_cen = censored_total_variance(w_dirty_q, nq, emove);
  return std::sqrt(std::max(w_cen, wcen_floor) / Tq);
}

} // namespace

Result<EarningsReproResult> run_earnings_repro(const VolaSession &sess, const EventSchedule &sched,
                                                std::int64_t now_ns,
                                                const EarningsReproConfig &cfg) {
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

  // Tenor year-fractions (WIRED knobs `time` / `clock_days_per_year`):
  // `clock_days_per_year > 0` uses a fixed clock `N_trading_days / clock`;
  // otherwise the calendar-aware trading-day advance under `cfg.time`.
  for (std::size_t i = 0; i < SrTenorGrid::kTradingDays.size(); ++i) {
    if (cfg.clock_days_per_year > 0.0) {
      out.tenor_T[i] = static_cast<double>(SrTenorGrid::kTradingDays[i]) / cfg.clock_days_per_year;
    } else {
      // Propagates the VolTime coverage error (vol_time.hpp): the 504-trading-day
      // tenor reaches ~2y past `now_ns`, so a late-window snapshot resolves off
      // the end of the default calendar. A poisoned tenor_T would silently
      // corrupt the censored-term fit that consumes it.
      ATX_TRY(const double tenor_T, tenor_years(now_ns, SrTenorGrid::kTradingDays[i], cfg.time));
      out.tenor_T[i] = tenor_T;
    }
  }

  EarningsFitConfig fit_cfg;
  fit_cfg.tenor_T = out.tenor_T; // non-owning view into `out`, alive for the rest of this call

  auto fit_res = fit_earnings_term(obs, fit_cfg);
  if (!fit_res.has_value()) {
    return Err(fit_res.error());
  }
  out.fit = *fit_res;

  // PRIMARY atmCenI: censored-space interpolation at each of the 12 SR tenors
  // (WIRED knobs `censor_space` / `interp`; see `interior_censored_vol`). A
  // tenor outside the listed range clamps flat to the nearest pillar's own
  // censored ATM vol (space-independent).
  for (std::size_t i = 0; i < out.tenor_T.size(); ++i) {
    const double Tq = out.tenor_T[i];
    out.n_earn[i] = count_events_at(sched, now_ns, Tq);

    // `listed` is ascending T; find the first pillar with T >= Tq.
    std::size_t hi = 0;
    while (hi < listed.size() && listed[hi].T < Tq) {
      ++hi;
    }

    if (hi == 0) {
      out.atm_cen_i[i] = clamp_atm_cen(listed.front(), out.fit.emove, fit_cfg.wcen_floor);
    } else if (hi >= listed.size()) {
      out.atm_cen_i[i] = clamp_atm_cen(listed.back(), out.fit.emove, fit_cfg.wcen_floor);
    } else {
      const ListedPillar &lo = listed[hi - 1];
      const ListedPillar &hip = listed[hi];
      if (!(hip.T > lo.T)) {
        // Defensive: a degenerate/duplicate-T bracket (should not occur on a
        // validly-fitted ascending-T surface) -- clamp to the upper pillar.
        out.atm_cen_i[i] = clamp_atm_cen(hip, out.fit.emove, fit_cfg.wcen_floor);
      } else {
        out.atm_cen_i[i] = interior_censored_vol(lo, hip, Tq, out.n_earn[i], out.fit.emove,
                                                 fit_cfg.wcen_floor, cfg.censor_space, cfg.interp);
      }
    }
  }

  return Ok(std::move(out));
}

Result<EarningsReproResult> run_earnings_repro(const VolaSession &sess, const EventSchedule &sched,
                                                std::int64_t now_ns) {
  // Historical behavior, bit-preserved: a default config whose time convention
  // is the session's own (Calendar365 default, clock-aware tenors, censored
  // variance-space interpolation).
  EarningsReproConfig cfg;
  cfg.time = sess.inputs().time;
  return run_earnings_repro(sess, sched, now_ns, cfg);
}

} // namespace atx::vol
