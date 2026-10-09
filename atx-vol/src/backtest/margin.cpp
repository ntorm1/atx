// margin.cpp — see margin.hpp for the model. Two pure functions, no engine
// dependency (this file must not include backtest.hpp).

#include "backtest/margin.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace atx::vol {

using atx::core::Ok;

double regt_short_option_margin(double spot, double strike, double premium, double mult,
                                Side side) noexcept {
  const double otm = side == Side::Call ? std::max(strike - spot, 0.0) : std::max(spot - strike, 0.0);
  // The one call/put asymmetry in the Reg-T floor: a call's floor is 10% of the
  // underlying; a put's is 10% of the STRIKE (FINRA's rule, verbatim per the
  // sprint brief). See margin.hpp's `regt_short_option_margin` doc comment.
  const double floor_base = side == Side::Call ? spot : strike;
  const double req = std::max(0.20 * spot - otm, 0.10 * floor_base);
  return req * mult + premium;
}

Result<double> scenario_margin(const PortfolioPricer &pricer, const SurfaceSet &base,
                               const MarginScenarioSpec &spec) {
  const std::span<const Position> positions = pricer.portfolio().positions();
  const std::vector<Position> book(positions.begin(), positions.end());

  ScenarioGridSpec grid;
  grid.spot_pct = {-spec.spot_shock, 0.0, spec.spot_shock};
  grid.vol_bump = {-spec.vol_shock, 0.0, spec.vol_shock};
  grid.dr = spec.dr;
  grid.dt = spec.dt;
  grid.n_threads = spec.n_threads;
  grid.analytic_greeks = spec.analytic_greeks;
  grid.taylor_radius_spot = spec.taylor_radius_spot;
  grid.taylor_radius_vol = spec.taylor_radius_vol;

  ATX_TRY(const ScenarioGridResult result, scenario_grid(book, base, grid));

  // Worst (most negative) cell, floored at 0.0: a book that never loses across
  // the grid owes no collateral, not a negative "credit". Scanned in the
  // result's own row-major cell order, which `scenario_grid` documents as
  // bit-identical at any thread count, so this reduction is deterministic too.
  double worst = 0.0;
  for (const double pnl : result.pnl) {
    if (pnl < worst) {
      worst = pnl;
    }
  }
  return Ok(-worst);
}

} // namespace atx::vol
