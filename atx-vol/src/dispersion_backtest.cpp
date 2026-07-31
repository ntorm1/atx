#include "atx/vol/research/dispersion_backtest.hpp"

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
  specs.dispersion = dispersion_config_from(config);
  specs.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  specs.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  specs.lifecycle.entry_every_n = config.entry_every_n;
  specs.lifecycle.roll_at_T = config.roll_dte_days / 365.25;
  specs.hedge.kind = config.hedge_kind;
  specs.hedge.cadence = config.hedge_cadence;
  specs.hedge.band = config.delta_band;
  return specs;
}

} // namespace

DispersionConfig dispersion_config_from(const DispersionBacktestConfig &config) {
  DispersionConfig dispersion;
  dispersion.target_T = config.target_dte_days / 365.25;
  dispersion.target_vega = config.gross_index_vega;
  dispersion.side = config.side;
  dispersion.multiplier = config.multiplier;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, config.min_names};
  dispersion.record_diagnostics = config.record_diagnostics;
  // X4 policies. Both default to the shipped construction, so a spec that names
  // neither builds exactly the book it always did.
  dispersion.weighting = config.weighting;
  dispersion.strike = config.strike;
  if (config.project_to_calendar_expiry) {
    dispersion.projected_maturity =
        ProjectedMaturitySpec::days(static_cast<std::int32_t>(std::llround(config.target_dte_days)));
  }
  return dispersion;
}

double fill_price(double signed_qty, double mid, double half_spread, double adv_frac,
                  const DispersionCostModel &m) noexcept {
  // Direction comes from the sign of the traded quantity: a BUY (qty > 0) lifts
  // the offer and pays impact, a SELL hits the bid and pays it too. `Side` in this
  // codebase is the option's Call/Put, NOT a trade direction, so it deliberately
  // plays no part here.
  const double direction = signed_qty >= 0.0 ? 1.0 : -1.0;
  const double impact = mid * m.k * std::pow(std::max(adv_frac, 0.0), m.beta);
  return mid + direction * (half_spread + impact);
}

FrictionModel dispersion_effective_frictions(const FrictionModel &base,
                                             const DispersionCostModel &costs) {
  FrictionModel effective = base;
  if (!costs.active()) {
    return effective; // exact mid-fill reproduction
  }
  // REVIEW C-4. The impact rides its OWN additive lane (`FrictionModel::
  // impact_fraction`) and the configured spread kind is left exactly as the spec
  // named it. This used to rewrite `spread_kind` to `PriceBps` unconditionally,
  // which silently DELETED a configured `vol_tick`: `base.vol_tick` survived in
  // the object, but the engine dispatches on the kind and never read it, so a
  // legal `VolTicks + impact` spec charged impact ONLY and overstated NAV.
  //
  // Additive composition is the semantics both headers already documented —
  // `fill_price` (dispersion_backtest.hpp) is `mid + direction * (half_spread +
  // impact)`, and `DispersionFrictionRegime::FrictionedWithImpact`
  // (dispersion_run.hpp) is "the above PLUS an active square-root impact model".
  //
  // Accumulating onto `base.impact_fraction` rather than assigning keeps the
  // fold composable and never silently drops an impact a caller already set.
  effective.impact_fraction +=
      costs.k * std::pow(std::max(costs.adv_fraction, 0.0), costs.beta);
  return effective;
}

DispersionStrategy make_dispersion_backtest_strategy(DispersionUniverse universe,
                                                     const DispersionBacktestConfig &config) {
  const DispersionSpecs specs = make_specs(config);
  DispersionStrategy strategy{std::move(universe), specs.dispersion, specs.lifecycle, specs.hedge};
  strategy.set_risk_limits(config.limits);
  return strategy;
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
  DispersionStrategy strategy{std::move(seed), specs.dispersion, specs.lifecycle, specs.hedge,
                              std::move(resolver)};
  strategy.set_risk_limits(config.limits);
  return strategy;
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
