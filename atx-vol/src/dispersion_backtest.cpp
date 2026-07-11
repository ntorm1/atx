#include "atx/vol/dispersion_backtest.hpp"

#include <cmath>
#include <cstdint>
#include <utility>

#include "atx/vol/contract_projection.hpp"

namespace atx::vol {

DispersionStrategy make_dispersion_backtest_strategy(DispersionUniverse universe,
                                                     const DispersionBacktestConfig &config) {
  DispersionConfig dispersion;
  dispersion.target_T = config.target_dte_days / 365.25;
  dispersion.target_vega = config.gross_index_vega;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, config.min_names};
  dispersion.record_diagnostics = config.record_diagnostics;
  if (config.project_to_calendar_expiry) {
    dispersion.projected_maturity = ProjectedMaturitySpec::days(
        static_cast<std::int32_t>(std::llround(config.target_dte_days)));
  }

  LifecycleSpec lifecycle;
  lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  lifecycle.entry_every_n = config.entry_every_n;
  lifecycle.roll_at_T = config.roll_dte_days / 365.25;

  HedgeSpec hedge;
  hedge.kind = HedgeSpec::Kind::DeltaToZero;
  hedge.cadence = HedgeSpec::Cadence::Daily;
  hedge.band = config.delta_band;
  return DispersionStrategy{std::move(universe), dispersion, lifecycle, hedge};
}

Result<BacktestResult> run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                                               const DispersionBacktestConfig &config) {
  DispersionStrategy strategy = make_dispersion_backtest_strategy(std::move(universe), config);
  return run_backtest(clock, strategy, config.run);
}

} // namespace atx::vol
