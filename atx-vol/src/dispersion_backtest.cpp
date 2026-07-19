#include "atx/vol/dispersion_backtest.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "atx/vol/contract_projection.hpp"

namespace atx::vol {

namespace {

// The config -> (sizing, lifecycle, hedge) mapping shared by both
// make_dispersion_backtest_strategy overloads (the frozen and the point-in-time
// paths), so the two construct bit-identical strategies apart from the resolver.
struct DispersionSpecs {
  DispersionConfig dispersion;
  LifecycleSpec lifecycle;
  HedgeSpec hedge;
};

[[nodiscard]] DispersionSpecs make_specs(const DispersionBacktestConfig &config) {
  DispersionSpecs specs;
  specs.dispersion.target_T = config.target_dte_days / 365.25;
  specs.dispersion.target_vega = config.gross_index_vega;
  specs.dispersion.side = DispersionSide::ShortIndexLongNames;
  specs.dispersion.multiplier = 100.0;
  specs.dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, config.min_names};
  specs.dispersion.record_diagnostics = config.record_diagnostics;
  if (config.project_to_calendar_expiry) {
    specs.dispersion.projected_maturity = ProjectedMaturitySpec::days(
        static_cast<std::int32_t>(std::llround(config.target_dte_days)));
  }
  specs.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  specs.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  specs.lifecycle.entry_every_n = config.entry_every_n;
  specs.lifecycle.roll_at_T = config.roll_dte_days / 365.25;
  specs.hedge.kind = HedgeSpec::Kind::DeltaToZero;
  specs.hedge.cadence = HedgeSpec::Cadence::Daily;
  specs.hedge.band = config.delta_band;
  return specs;
}

} // namespace

DispersionStrategy make_dispersion_backtest_strategy(DispersionUniverse universe,
                                                     const DispersionBacktestConfig &config) {
  const DispersionSpecs specs = make_specs(config);
  return DispersionStrategy{std::move(universe), specs.dispersion, specs.lifecycle, specs.hedge};
}

DispersionStrategy make_dispersion_backtest_strategy(std::vector<UniverseRow> schedule,
                                                     const DispersionBacktestConfig &config,
                                                     std::string_view index_symbol) {
  const DispersionSpecs specs = make_specs(config);
  // Seed with the EARLIEST block so the universe is always a valid, non-empty
  // object; on_step re-resolves the correct point-in-time membership on step 0.
  // (`read_universe` sorts rows by (effective_date, symbol), so front() carries
  // the minimum effective_date.)
  DispersionUniverse seed;
  if (!schedule.empty()) {
    if (Result<DispersionUniverse> first =
            universe_at(schedule, schedule.front().effective_date, index_symbol)) {
      seed = std::move(*first);
    }
  }
  auto resolver = make_pit_universe_resolver(std::move(schedule), std::string(index_symbol));
  return DispersionStrategy{std::move(seed), specs.dispersion, specs.lifecycle, specs.hedge,
                            std::move(resolver)};
}

Result<BacktestResult> run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                                               const DispersionBacktestConfig &config) {
  DispersionStrategy strategy = make_dispersion_backtest_strategy(std::move(universe), config);
  return run_backtest(clock, strategy, config.run);
}

Result<BacktestResult> run_dispersion_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                                               const DispersionBacktestConfig &config,
                                               std::string_view index_symbol) {
  DispersionStrategy strategy =
      make_dispersion_backtest_strategy(std::move(schedule), config, index_symbol);
  return run_backtest(clock, strategy, config.run);
}

} // namespace atx::vol
