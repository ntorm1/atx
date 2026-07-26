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
  lifecycle.entry = config.entry;
  lifecycle.holding = config.holding;
  lifecycle.entry_every_n = config.entry_every_n;
  lifecycle.roll_at_T = config.roll_dte_days / 365.25;

  HedgeSpec hedge;
  hedge.kind = HedgeSpec::Kind::DeltaToZero;
  hedge.cadence = HedgeSpec::Cadence::Daily;
  hedge.band = config.delta_band;
  return DispersionStrategy{std::move(universe), dispersion, lifecycle, hedge};
}

DispersionBacktestConfig make_dispersion_ladder_config(double target_dte_days,
                                                       double gross_index_vega,
                                                       std::size_t min_names) {
  DispersionBacktestConfig config;
  config.target_dte_days = target_dte_days;
  config.gross_index_vega = gross_index_vega;
  config.min_names = min_names;
  config.entry = LifecycleSpec::Entry::EveryStep;
  config.holding = LifecycleSpec::Holding::HoldToExpiry;
  // `entry_every_n` is read only under Entry::EveryNDays; pinned to 1 anyway so a
  // reader of a dumped config cannot mistake the inherited default of 21 for a
  // cadence this shape honours.
  config.entry_every_n = 1u;
  return config;
}

Result<BacktestResult> run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                                               const DispersionBacktestConfig &config) {
  DispersionStrategy strategy = make_dispersion_backtest_strategy(std::move(universe), config);
  return run_backtest(clock, strategy, config.run);
}

} // namespace atx::vol
